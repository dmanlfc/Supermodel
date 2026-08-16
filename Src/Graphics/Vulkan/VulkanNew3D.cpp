/**
 ** Supermodel
 ** A Sega Model 3 Arcade Emulator.
 ** Copyright 2003-2026 The Supermodel Team
 **
 ** This file is part of Supermodel.
 **
 ** Supermodel is free software: you can redistribute it and/or modify it under
 ** the terms of the GNU General Public License as published by the Free
 ** Software Foundation, either version 3 of the License, or (at your option)
 ** any later version.
 **
 ** Supermodel is distributed in the hope that it will be useful, but WITHOUT
 ** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 ** FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 ** more details.
 **
 ** You should have received a copy of the GNU General Public License along
 ** with Supermodel.  If not, see <http://www.gnu.org/licenses/>.
 **/

#ifdef SUPERMODEL_VULKAN

#include "VulkanNew3D.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

#include "OSD/Logger.h"

namespace Vulkan
{
namespace
{

constexpr VkDeviceSize kVertexStride = sizeof(New3D::FVertex);

// The dynamic vertex region is staged in one copy, so its own ring has to hold a
// full MAX_RAM_VERTS worth or a busy frame falls back to a synchronous submission.
static_assert(static_cast<VkDeviceSize>(MAX_RAM_VERTS) * kVertexStride <=
                  Context::kVertexStagingBytesPerFrame,
              "kVertexStagingBytesPerFrame cannot hold MAX_RAM_VERTS dynamic vertices");

// Number of vertices the full screen quad shaders draw, matching
// glDrawArrays(GL_TRIANGLE_STRIP, 0, 4).
constexpr uint32_t kFullScreenQuadVertexCount = 4;

int32_t ClampToRange(int32_t value, int32_t low, int32_t high)
{
  return value < low ? low : (value > high ? high : value);
}

bool SetErrorMessage(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

} // anonymous namespace

VulkanNew3D::VulkanNew3D(Context &context, const Util::Config::Node &config,
                         const std::string &gameName)
  : New3D::CNew3DBase(config, gameName), m_context(context)
{
  // CNew3DBase has already set m_numPolyVerts from QuadRendering; the pipeline
  // topology has to agree with it.
  const bool quadsRequested = config["QuadRendering"].ValueAs<bool>();
  m_quadRendering = quadsRequested && m_context.GeometryShaderAvailable();
  m_program = m_quadRendering ? Program::R3DQuad : Program::R3DTriangle;
  m_ready = true;
}

VulkanNew3D::~VulkanNew3D()
{
  Destroy();
}

void VulkanNew3D::Destroy()
{
  const DeviceContext &device = m_context.Device();
  if (device.device == VK_NULL_HANDLE)
    return;

  // Everything below may still be referenced by submitted work.
  vkDeviceWaitIdle(device.device);

  // The Context outlives this renderer, so any line-of-sight readback still waiting on a
  // fence must be withdrawn before `this` goes away.
  m_context.CancelFrameCompletions(this);
  for (auto &frame : m_losSlots)
  {
    for (LosSlot &slot : frame)
      slot.pending = false;
  }

  m_descriptors.Destroy(device);
  m_r3dSet = VK_NULL_HANDLE;
  m_scrollFogSet = VK_NULL_HANDLE;
  m_composite.Destroy(device);
  m_textureBank[0].Destroy(device);
  m_textureBank[1].Destroy(device);
  m_vertexBuffer.Destroy(device);
  m_romVertexBytes = 0;
  m_initialized = false;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool VulkanNew3D::CheckDeviceLimits(std::string *error) const
{
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(m_context.Device().physicalDevice, &properties);
  const VkPhysicalDeviceLimits &limits = properties.limits;

  const bool renderTargetExceedsLimits =
      m_totalXRes > limits.maxImageDimension2D || m_totalYRes > limits.maxImageDimension2D ||
      m_totalXRes > limits.maxFramebufferWidth || m_totalYRes > limits.maxFramebufferHeight ||
      m_totalXRes > limits.maxViewportDimensions[0] || m_totalYRes > limits.maxViewportDimensions[1];
  const bool outputViewportExceedsLimits =
      m_outputXRes > limits.maxViewportDimensions[0] || m_outputYRes > limits.maxViewportDimensions[1];

  if (renderTargetExceedsLimits || outputViewportExceedsLimits)
  {
    std::ostringstream message;
    message << "New3D dimensions exceed the Vulkan device limits: render target " << m_totalXRes
            << "x" << m_totalYRes << ", output viewport " << m_outputXRes << "x" << m_outputYRes
            << "; maximum 2D image " << limits.maxImageDimension2D << ", framebuffer "
            << limits.maxFramebufferWidth << "x" << limits.maxFramebufferHeight << ", viewport "
            << limits.maxViewportDimensions[0] << "x" << limits.maxViewportDimensions[1] << ".";
    if (error != nullptr)
      *error = message.str();
    return false;
  }
  return true;
}

bool VulkanNew3D::CreateVertexBuffer(std::string *error)
{
  m_romCapacityBytes = static_cast<VkDeviceSize>(MAX_ROM_VERTS) * kVertexStride;
  const VkDeviceSize ramCapacityBytes =
      static_cast<VkDeviceSize>(MAX_RAM_VERTS) * kVertexStride * Context::kFramesInFlight;

  m_romBaseBytes = 0;
  m_ramBaseBytes = m_romCapacityBytes;

  const VkDeviceSize totalBytes = m_romCapacityBytes + ramCapacityBytes;
  const VkBufferUsageFlags usage =
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

  // Prefer memory that is both host visible and device local: on unified memory the dynamic
  // vertices are written straight into place, with no staging copy, no separate submission
  // and nothing to order against the vertex fetch. Discrete layouts fall back to staging.
  std::string hostVisibleError;
  if (!m_vertexBuffer.Create(m_context.Device(), totalBytes, usage,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             0, &hostVisibleError))
  {
    if (!m_vertexBuffer.Create(m_context.Device(), totalBytes, usage,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, error))
      return false;
  }

  InfoLog("Vulkan Real3D dynamic vertices are %s.",
          m_vertexBuffer.MappedPointer() != nullptr
              ? "written directly into device memory"
              : "staged through a separate transfer submission");
  m_romVertexBytes = 0;
  return true;
}

// Both backends compute mesh.vboOffset against a single notional buffer whose dynamic half
// starts at MAX_ROM_VERTS. Reaching the region through the binding offset keeps the index
// small and lets the two regions sit anywhere.
VkDeviceSize VulkanNew3D::VertexBindOffsetFor(const New3D::Mesh &mesh) const
{
  if (mesh.vboOffset < MAX_ROM_VERTS)
    return m_romBaseBytes;
  return m_ramBaseBytes +
         static_cast<VkDeviceSize>(m_context.FrameIndex()) * MAX_RAM_VERTS * kVertexStride;
}

bool VulkanNew3D::EnsureDescriptors(std::string *error)
{
  // Context::EnsureSceneTargets() destroys the pipeline cache owning these layouts, and
  // another renderer sizing the output image can trigger that after this one initialized.
  // It idles the device first, so re-creating the sets on a layout change is safe.
  PipelineCache &pipelines = m_context.Pipelines();
  const VkDescriptorSetLayout r3dLayout = pipelines.SetLayout(SetLayoutId::R3D);
  const VkDescriptorSetLayout scrollFogLayout = pipelines.SetLayout(SetLayoutId::ScrollFog);
  if (r3dLayout == VK_NULL_HANDLE || scrollFogLayout == VK_NULL_HANDLE)
    return SetErrorMessage(error, "The Vulkan Real3D descriptor set layouts are unavailable");

  if (m_r3dSet != VK_NULL_HANDLE && m_scrollFogSet != VK_NULL_HANDLE &&
      m_r3dSetLayout == r3dLayout && m_scrollFogSetLayout == scrollFogLayout)
    return true;

  m_descriptors.Destroy(m_context.Device());
  m_r3dSet = VK_NULL_HANDLE;
  m_scrollFogSet = VK_NULL_HANDLE;
  if (!CreateDescriptors(error))
    return false;

  m_r3dSetLayout = r3dLayout;
  m_scrollFogSetLayout = scrollFogLayout;
  return true;
}

bool VulkanNew3D::CreateDescriptors(std::string *error)
{
  // Two persistent sets, allocated once: the R3D set the draw loop rebinds with
  // a new dynamic offset, and the scroll fog set.
  const std::vector<VkDescriptorPoolSize> sizes = {
    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 },
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kR3DTextureBankCount },
  };
  if (!m_descriptors.Create(m_context.Device(), sizes, 2, error))
    return false;

  PipelineCache &pipelines = m_context.Pipelines();
  if (!m_descriptors.Allocate(pipelines.SetLayout(SetLayoutId::R3D), &m_r3dSet, error))
    return false;
  if (!m_descriptors.Allocate(pipelines.SetLayout(SetLayoutId::ScrollFog), &m_scrollFogSet, error))
    return false;

  // The descriptors reference offset 0 of the uniform ring; the per-draw
  // suballocation is supplied as a dynamic offset.
  VkDescriptorBufferInfo r3dBuffer{};
  r3dBuffer.buffer = m_context.UniformRing().Handle();
  r3dBuffer.offset = 0;
  r3dBuffer.range = sizeof(R3DUniforms);

  VkDescriptorBufferInfo scrollFogBuffer{};
  scrollFogBuffer.buffer = m_context.UniformRing().Handle();
  scrollFogBuffer.offset = 0;
  scrollFogBuffer.range = sizeof(ScrollFogUniforms);

  std::array<VkDescriptorImageInfo, kR3DTextureBankCount> banks{};
  for (uint32_t i = 0; i < kR3DTextureBankCount; ++i)
  {
    banks[i].sampler = m_textureBank[i].Sampler();
    banks[i].imageView = m_textureBank[i].View();
    // The banks reach this layout before the first draw of every frame, see RenderFrame().
    banks[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }

  std::array<VkWriteDescriptorSet, 3> writes{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = m_r3dSet;
  writes[0].dstBinding = kR3DBindingUniforms;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  writes[0].pBufferInfo = &r3dBuffer;

  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = m_r3dSet;
  writes[1].dstBinding = kR3DBindingTextureBank;
  writes[1].descriptorCount = kR3DTextureBankCount;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[1].pImageInfo = banks.data();

  writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet = m_scrollFogSet;
  writes[2].dstBinding = kScrollFogBindingUniforms;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  writes[2].pBufferInfo = &scrollFogBuffer;

  vkUpdateDescriptorSets(m_context.Device().device, static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);
  return true;
}

Result VulkanNew3D::Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes,
                         unsigned totalXResParam, unsigned totalYResParam, unsigned /*aaTarget*/)
{
  if (!m_ready)
    return Result::FAIL;

  if (m_quadRendering != (m_numPolyVerts == 4))
  {
    // The base class derived four vertices per primitive from QuadRendering, but the device
    // cannot run the geometry stage the quad pipeline needs.
    ErrorLog("QuadRendering was requested but this Vulkan device does not support "
             "VkPhysicalDeviceFeatures::geometryShader, which the Real3D quad pipeline requires. "
             "Set QuadRendering=false, or use a device that reports geometryShader.");
    return Result::FAIL;
  }

  // Resolution and offset within the physical display area. Identical to CNew3D::Init.
  if (m_wideScreen)
  {
    m_outputXOffs = 0;
    m_outputYOffs = 0;
    m_outputXRes = totalXResParam;
    m_outputYRes = totalYResParam;
  }
  else
  {
    m_outputXOffs = xOffset;
    m_outputYOffs = yOffset;
    m_outputXRes = xRes;
    m_outputYRes = yRes;
  }

  if (m_renderScale > 0)
  {
    const unsigned scale = static_cast<unsigned>(m_renderScale);
    m_xRes = 496u * scale;
    m_yRes = 384u * scale;
    m_totalYRes = m_yRes;

    if (m_wideScreen && m_outputYRes > 0)
    {
      const unsigned long long scaledWidth =
          static_cast<unsigned long long>(m_yRes) * m_outputXRes;
      m_totalXRes = static_cast<unsigned>((scaledWidth + m_outputYRes / 2u) / m_outputYRes);
    }
    else
    {
      m_totalXRes = m_xRes;
    }

    m_xOffs = m_totalXRes > m_xRes ? (m_totalXRes - m_xRes) / 2u : 0u;
    m_yOffs = 0;
  }
  else
  {
    m_xOffs = xOffset;
    m_yOffs = yOffset;
    m_xRes = xRes;
    m_yRes = yRes;
    m_totalXRes = totalXResParam;
    m_totalYRes = totalYResParam;
  }
  m_xRatio = m_xRes * static_cast<float>(1.0 / 496.0);
  m_yRatio = m_yRes * static_cast<float>(1.0 / 384.0);

  // Main installs this with glScissor() and never disables GL_SCISSOR_TEST, so GL clips the
  // 3D composite to it. Insets the display area to hide the two pixel 2D/3D mismatch;
  // Render2D::ContentScissor() derives the same box for the 2D layers.
  {
    const int32_t correction = static_cast<int32_t>((yRes / 384.0) * 2.0 + 0.5);
    const int32_t sceneWidth = static_cast<int32_t>(totalXResParam);
    const int32_t sceneHeight = static_cast<int32_t>(totalYResParam);
    int32_t left = 0, bottom = 0, width = 0, height = 0;
    if (m_wideScreen)
    {
      left = 0;
      bottom = correction;
      width = sceneWidth;
      height = sceneHeight - correction * 2;
    }
    else
    {
      left = static_cast<int32_t>(xOffset) + correction;
      bottom = static_cast<int32_t>(yOffset) + correction;
      width = static_cast<int32_t>(xRes) - correction * 2;
      height = static_cast<int32_t>(yRes) - correction * 2;
    }

    // glScissor() measures from the lower left, VkRect2D from the upper left.
    const int32_t top = sceneHeight - (bottom + height);
    const int32_t x0 = ClampToRange(left, 0, sceneWidth);
    const int32_t y0 = ClampToRange(top, 0, sceneHeight);
    const int32_t x1 = ClampToRange(left + width, 0, sceneWidth);
    const int32_t y1 = ClampToRange(top + height, 0, sceneHeight);
    m_displayScissor.offset = VkOffset2D{x0, y0};
    m_displayScissor.extent = VkExtent2D{static_cast<uint32_t>(std::max(x1 - x0, 0)),
                                         static_cast<uint32_t>(std::max(y1 - y0, 0))};
  }

  std::string error;
  if (!CheckDeviceLimits(&error))
  {
    ErrorLog("%s", error.c_str());
    return Result::FAIL;
  }

  // R3DFrameBuffers::CreateFBO equivalent. Attachment size is m_totalXRes/m_totalYRes, this
  // renderer's internal render resolution; scene size is totalXResParam/totalYResParam, the
  // supersampled display resolution the 2D layers use. Output size is PostProcess's, hence 0.
  if (!m_context.EnsureSceneTargets(m_totalXRes, m_totalYRes, totalXResParam, totalYResParam, 0, 0,
                                    &error))
  {
    ErrorLog("Unable to create the Vulkan New3D render targets: %s", error.c_str());
    return Result::FAIL;
  }

  // Init() runs again after a window resize. The texture banks and vertex buffer
  // survive that; the descriptor sets do not.
  if (!m_initialized)
  {
    if (!m_textureBank[0].Create(m_context.Device(), &error) ||
        !m_textureBank[1].Create(m_context.Device(), &error))
    {
      ErrorLog("Unable to create the Vulkan Real3D texture banks: %s", error.c_str());
      Destroy();
      return Result::FAIL;
    }
    if (!CreateVertexBuffer(&error))
    {
      ErrorLog("Unable to create the Vulkan Real3D vertex buffer: %s", error.c_str());
      Destroy();
      return Result::FAIL;
    }
    // Texture RAM may have been attached before Init().
    if (m_textureRAM != nullptr)
    {
      m_textureBank[0].AttachMemory(m_textureRAM);
      m_textureBank[1].AttachMemory(m_textureRAM + (2048 * 1024));
    }
  }

  // EnsureSceneTargets() has already idled the device, so the old pool can go. The sets are
  // re-created here and again later if another renderer rebuilds the pipeline cache.
  m_r3dSetLayout = VK_NULL_HANDLE;
  m_scrollFogSetLayout = VK_NULL_HANDLE;
  if (!EnsureDescriptors(&error))
  {
    ErrorLog("Unable to create the Vulkan Real3D descriptor sets: %s", error.c_str());
    Destroy();
    return Result::FAIL;
  }
  m_initialized = true;

  // Warm the cache so that no pipeline is ever compiled inside the draw loop.
  if (!m_context.Pipelines().Prebuild(m_context.Device(), m_context.PipelineTargets(), &error))
  {
    ErrorLog("Unable to build the Vulkan Real3D pipelines: %s", error.c_str());
    return Result::FAIL;
  }

  return Result::OKAY;
}

void VulkanNew3D::AttachMemory(const uint32_t *cullingRAMLoPtr, const uint32_t *cullingRAMHiPtr,
                               const uint32_t *polyRAMPtr, const uint32_t *vromPtr,
                               const uint16_t *textureRAMPtr)
{
  AttachMemoryPointers(cullingRAMLoPtr, cullingRAMHiPtr, polyRAMPtr, vromPtr, textureRAMPtr);

  m_textureBank[0].AttachMemory(textureRAMPtr);
  m_textureBank[1].AttachMemory(textureRAMPtr + (2048 * 1024));
}

// ---------------------------------------------------------------------------
// Texture uploads
// ---------------------------------------------------------------------------

// A whole sheet invalidation supersedes everything queued before it, and past the cap a full
// reupload is cheaper than replaying rectangles, so the queue stays bounded however long
// uploads are refused.
void VulkanNew3D::QueueTextureUpload(unsigned level, unsigned x, unsigned y, unsigned width,
                                     unsigned height)
{
  const bool wholeSheet = width == 2048 && height == 2048;
  if (wholeSheet || m_pendingTextureUploads.size() >= kMaxPendingTextureUploads)
  {
    m_pendingTextureUploads.clear();
    m_pendingTextureUploads.push_back(wholeSheet ? PendingTextureUpload{level, x, y, width, height}
                                                 : PendingTextureUpload{0, 0, 0, 2048, 2048});
    return;
  }
  m_pendingTextureUploads.push_back(PendingTextureUpload{level, x, y, width, height});
}

void VulkanNew3D::DrainPendingTextureUploads()
{
  if (m_pendingTextureUploads.empty())
    return;
  // Swap first: UploadTextures re-queues if it still cannot record, and that must
  // not disturb the sequence being replayed.
  std::vector<PendingTextureUpload> pending;
  pending.swap(m_pendingTextureUploads);
  for (const PendingTextureUpload &upload : pending)
    UploadTextures(upload.level, upload.x, upload.y, upload.width, upload.height);
}

void VulkanNew3D::UploadTextures(unsigned level, unsigned x, unsigned y, unsigned width,
                                 unsigned height)
{
  // CReal3D::SyncSnapshots discards its queue wholesale and the OpenGL path can never
  // refuse, so a dropped request is lost for good and the pages it covered keep stale
  // contents. Hold it for the next frame that can record.
  if (!m_initialized || !m_context.FrameActive())
  {
    QueueTextureUpload(level, x, y, width, height);
    return;
  }

  std::string error;

  // Entire sheet invalidation, exactly as CNew3D::UploadTextures handles it.
  if (width == 2048 && height == 2048)
  {
    height = 1024;

    static constexpr int mipXBase[] = { 0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047 };
    static constexpr int mipYBase[] = { 0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023 };

    for (int i = 0; i < m_textureBank[0].GetNumberOfLevels(); i++)
    {
      if (!m_context.RecordTextureBankUpload(m_textureBank[0], i, mipXBase[i], mipYBase[i],
                                             static_cast<int>(width), static_cast<int>(height),
                                             &error) ||
          !m_context.RecordTextureBankUpload(m_textureBank[1], i, mipXBase[i], mipYBase[i],
                                             static_cast<int>(width), static_cast<int>(height),
                                             &error))
      {
        ErrorLog("Vulkan texture sheet upload failed: %s", error.c_str());
        return;
      }
      width = (width > 1) ? width / 2 : 1;
      height = (height > 1) ? height / 2 : 1;
    }

    return;
  }

  int page = 0;
  TranslateTexture(x, y, static_cast<int>(width), static_cast<int>(height), page);

  if (page < 0 || page > 1)
    return;
  if (!m_context.RecordTextureBankUpload(m_textureBank[page], static_cast<int>(level),
                                         static_cast<int>(x), static_cast<int>(y),
                                         static_cast<int>(width), static_cast<int>(height), &error))
    ErrorLog("Vulkan texture upload failed: %s", error.c_str());
}

// ---------------------------------------------------------------------------
// Vertex streaming
// ---------------------------------------------------------------------------

uint32_t VulkanNew3D::FirstVertexFor(const New3D::Mesh &mesh) const
{
  if (mesh.vboOffset < MAX_ROM_VERTS)
    return static_cast<uint32_t>(mesh.vboOffset);
  return static_cast<uint32_t>(mesh.vboOffset - MAX_ROM_VERTS);
}

// The scene passes declare a VK_SUBPASS_EXTERNAL dependency covering TRANSFER to
// VERTEX_INPUT, but that is not enough on a tiled GPU, whose binning phase fetches vertices
// outside the render phase the dependency guards. Order the copies explicitly.
void VulkanNew3D::RecordVertexUploadBarrier(VkCommandBuffer commandBuffer)
{
  // Widest barrier available: the correct narrow form, a TRANSFER to VERTEX_INPUT buffer
  // barrier, does not order the copy against this device's vertex fetch.
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;

  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void VulkanNew3D::UploadVertexData()
{
  std::string error;
  bool uploaded = false;

  // m_vbo.BufferSubData(MAX_ROM_VERTS * sizeof(FVertex), ...): the dynamic
  // vertices go into this frame in flight's copy of the RAM region.
  size_t ramVerts = m_polyBufferRam.size();

  if (ramVerts > MAX_RAM_VERTS)
  {
    if (!m_ramOverflowReported)
    {
      ErrorLog("The Real3D dynamic vertex buffer overflowed (%zu of %d vertices); the excess is "
               "dropped for this frame.", ramVerts, MAX_RAM_VERTS);
      m_ramOverflowReported = true;
    }
    ramVerts = MAX_RAM_VERTS;
  }
  if (ramVerts > 0)
  {
    const VkDeviceSize ramOffset =
        m_ramBaseBytes +
        static_cast<VkDeviceSize>(m_context.FrameIndex()) * MAX_RAM_VERTS * kVertexStride;
    const VkDeviceSize ramBytes = static_cast<VkDeviceSize>(ramVerts) * kVertexStride;

    if (void *mapped = m_vertexBuffer.MappedPointer())
    {
      // Context::BeginFrame() already waited on this frame's fence, so the GPU has finished
      // reading the slice being overwritten. No barrier: there is no GPU-side copy to order.
      std::memcpy(static_cast<uint8_t *>(mapped) + ramOffset, m_polyBufferRam.data(),
                  static_cast<size_t>(ramBytes));
      if (!m_vertexBuffer.Flush(m_context.Device(), ramOffset, ramBytes, &error))
        ErrorLog("Vulkan dynamic vertex flush failed: %s", error.c_str());
    }
    // Recording the copy into the frame's own command buffer does not order it against this
    // device's vertex fetch, with neither a TRANSFER to VERTEX_INPUT barrier nor a full
    // ALL_COMMANDS one. The data is host coherent, so this is ordering and not visibility,
    // which is why no barrier fixes it. RecordVertexUpload uses a separate submission,
    // waited on by semaphore.
    else if (!m_context.RecordVertexUpload(m_vertexBuffer.Handle(), ramOffset,
                                          m_polyBufferRam.data(), ramBytes, &error))
      ErrorLog("Vulkan dynamic vertex upload failed: %s", error.c_str());
  }

  if (UploadRomVertexData(&error))
    uploaded = true;

  // Every copy above has to be visible to the vertex fetch of the draws recorded
  // after this point, and only a barrier in this command buffer guarantees that.
  if (uploaded)
    RecordVertexUploadBarrier(m_context.CommandBuffer());
}

// True when it recorded a copy into the frame's command buffer.
bool VulkanNew3D::UploadRomVertexData(std::string *error)
{
  if (m_polyBufferRom.empty())
    return false;

  // sync rom memory with the vertex buffer
  const VkDeviceSize romBytes = static_cast<VkDeviceSize>(m_polyBufferRom.size()) * kVertexStride;
  const VkDeviceSize vboBytes = m_romVertexBytes;
  if (romBytes == vboBytes)
    return false;

  // check we haven't blown up the memory buffers
  // we will lose rom models for 1 frame if this happens, not the end of the
  // world, as probably won't ever happen anyway
  if (m_polyBufferRom.size() >= MAX_ROM_VERTS)
  {
    m_polyBufferRom.clear();
    m_romMap.clear();
    m_romVertexBytes = 0;
    return false;
  }

  if (romBytes < vboBytes)
    return false;   // the cache was reset behind us; nothing to append

  const VkDeviceSize size = romBytes - vboBytes;
  if (vboBytes + size >= m_romCapacityBytes)
    return false;   // VBO::AppendData refused the same case

  if (!m_context.RecordBufferUpload(m_vertexBuffer.Handle(), m_romBaseBytes + vboBytes,
                                    &m_polyBufferRom[static_cast<size_t>(vboBytes / kVertexStride)],
                                    size, error))
  {
    ErrorLog("Vulkan ROM vertex upload failed: %s", error->c_str());
    return false;
  }
  m_romVertexBytes = vboBytes + size;
  return true;
}

// ---------------------------------------------------------------------------
// Uniform state
// ---------------------------------------------------------------------------

void VulkanNew3D::SetSceneViewport(const New3D::Viewport &viewport)
{
  VkCommandBuffer commandBuffer = m_context.CommandBuffer();
  const SceneTargets &targets = m_context.Targets();
  const int32_t targetWidth = static_cast<int32_t>(targets.Width());
  const int32_t targetHeight = static_cast<int32_t>(targets.Height());

  // CalcViewport() produced GL framebuffer coordinates, origin at the lower left.
  // MakeViewport() wants upper left and restores GL clip space with a negative height.
  const float yTop = static_cast<float>(targetHeight - (viewport.y + viewport.height));
  const VkViewport vkViewport = targets.MakeViewport(static_cast<float>(viewport.x), yTop,
                                                     static_cast<float>(viewport.width),
                                                     static_cast<float>(viewport.height));
  vkCmdSetViewport(commandBuffer, 0, 1, &vkViewport);

  int32_t left = ClampToRange(viewport.x, 0, targetWidth);
  int32_t top = ClampToRange(static_cast<int32_t>(yTop), 0, targetHeight);
  int32_t right = ClampToRange(viewport.x + viewport.width, 0, targetWidth);
  int32_t bottom = ClampToRange(static_cast<int32_t>(yTop) + viewport.height, 0, targetHeight);

  // CNew3D disables GL_SCISSOR_TEST for the offscreen scene passes only when the render
  // scale is on. With it off, attachments and scene share a coordinate space, so Main's
  // display box still clips these passes in GL.
  if (m_renderScale == 0)
  {
    const int32_t displayLeft = m_displayScissor.offset.x;
    const int32_t displayTop = m_displayScissor.offset.y;
    left = std::max(left, displayLeft);
    top = std::max(top, displayTop);
    right = std::min(right, displayLeft + static_cast<int32_t>(m_displayScissor.extent.width));
    bottom = std::min(bottom, displayTop + static_cast<int32_t>(m_displayScissor.extent.height));
  }

  const VkRect2D scissor = targets.MakeScissor(left, top,
                                               static_cast<uint32_t>(std::max(right - left, 0)),
                                               static_cast<uint32_t>(std::max(bottom - top, 0)));
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void VulkanNew3D::FillViewportUniforms(const New3D::Viewport &viewport)
{
  // R3DShader::SetViewportUniforms, value for value.
  std::memcpy(m_uniforms.projMat, static_cast<const float *>(viewport.projectionMatrix),
              sizeof(m_uniforms.projMat));

  m_uniforms.fogColour[0] = viewport.fogColour[0];
  m_uniforms.fogColour[1] = viewport.fogColour[1];
  m_uniforms.fogColour[2] = viewport.fogColour[2];
  m_uniforms.fogColour[3] = 0.0f;                 // unused by the R3D shaders

  m_uniforms.fogDensity = viewport.fogDensity;
  m_uniforms.fogStart = viewport.fogStart;
  m_uniforms.fogAttenuation = viewport.fogAttenuation;
  m_uniforms.fogAmbient = viewport.fogAmbient;

  // glUniform3fv(lighting, 2, vp->lightingParams): vec3[2] widened to vec4[2].
  for (int i = 0; i < 2; i++)
  {
    m_uniforms.lighting[i][0] = viewport.lightingParams[i * 3 + 0];
    m_uniforms.lighting[i][1] = viewport.lightingParams[i * 3 + 1];
    m_uniforms.lighting[i][2] = viewport.lightingParams[i * 3 + 2];
    m_uniforms.lighting[i][3] = 0.0f;
  }
  m_uniforms.b_sunClamp = ShaderBool(viewport.sunClamp);
  m_uniforms.b_intensityClamp = ShaderBool(viewport.intensityClamp);

  for (int i = 0; i < 4; i++)
    m_uniforms.spotEllipse[i] = viewport.spotEllipse[i];
  m_uniforms.spotRange[0] = viewport.spotRange[0];
  m_uniforms.spotRange[1] = viewport.spotRange[1];
  for (int i = 0; i < 3; i++)
  {
    m_uniforms.spotColor[i] = viewport.spotColor[i];
    m_uniforms.spotFogColor[i] = viewport.spotFogColor[i];
  }
  m_uniforms.spotColor[3] = 0.0f;
  m_uniforms.spotFogColor[3] = 0.0f;

  m_uniforms.hardwareStep = viewport.hardwareStep;
  m_uniforms.cota = viewport.cota;

  // The OpenGL renderers always get GL_DEPTH32F_STENCIL8. A device offering only the
  // normalised integer format needs the shader to spread the reversed-Z range instead, or
  // coplanar surfaces such as facial decals lose to whatever drew last.
  m_uniforms.b_encodeDepth =
      DepthEncodeMode();

  // spotEllipse is in GL screen coordinates; the shader flips gl_FragCoord.y back with this.
  // The height is the render target's, the space CalcViewport/RenderViewport worked in.
  const std::array<float, 2> transform =
      MakeFragCoordYTransform(static_cast<float>(m_context.Targets().Height()));
  m_uniforms.fragCoordYTransform[0] = transform[0];
  m_uniforms.fragCoordYTransform[1] = transform[1];
}

// 0 leaves the reversed-Z value alone, which is right for a float attachment.
// 1 remaps it across a normalised integer one.
uint32_t VulkanNew3D::DepthEncodeMode() const
{
  if (m_context.Targets().DepthStencilFormat() == VK_FORMAT_D32_SFLOAT_S8_UINT)
    return 0;
  return 1u;
}

void VulkanNew3D::ApplyModelState(const New3D::Model &model)
{
  // R3DShader::SetModelStates.
  std::memcpy(m_push.modelMat, model.modelMat, sizeof(m_push.modelMat));
  m_push.modelScale = model.scale;
  m_push.nodeAlpha = model.alpha;

  m_transX = model.textureOffsetX;
  m_transY = model.textureOffsetY;
  m_transPage = model.page;
}

void VulkanNew3D::FillMeshUniforms(const New3D::Mesh &mesh)
{
  // R3DShader::SetMeshUniforms. The GL version only issued a glUniform on change; the whole
  // block is rewritten here and deduplicated in BindMeshUniforms() instead.
  m_uniforms.b_textureEnabled = ShaderBool(mesh.textured);
  m_uniforms.b_microTexture = ShaderBool(mesh.microTexture);
  m_uniforms.texturePage = mesh.page ^ m_transPage;
  m_uniforms.microTextureMinLOD = mesh.microTextureMinLOD;
  m_uniforms.microTextureID = mesh.microTextureID;

  m_uniforms.baseTexInfo[0] = mesh.x + m_transX;
  m_uniforms.baseTexInfo[1] = mesh.y + m_transY;
  m_uniforms.baseTexInfo[2] = mesh.width;
  m_uniforms.baseTexInfo[3] = mesh.height;
  m_uniforms.baseTexType = mesh.format;

  m_uniforms.b_textureInverted = ShaderBool(mesh.inverted);
  m_uniforms.b_alphaTest = ShaderBool(mesh.alphaTest);
  m_uniforms.b_textureAlpha = ShaderBool(mesh.textureAlpha);
  m_uniforms.fogIntensity = mesh.fogIntensity;

  m_uniforms.b_lightEnabled = ShaderBool(mesh.lighting);
  m_uniforms.shininess = mesh.shininess;
  m_uniforms.b_specularEnabled = ShaderBool(mesh.specular);
  m_uniforms.specularValue = mesh.specularValue;
  m_uniforms.b_fixedShading = ShaderBool(mesh.fixedShading);
  m_uniforms.b_smoothShading = ShaderBool(mesh.smoothShading);
  m_uniforms.b_polyAlpha = ShaderBool(mesh.polyAlpha);

  m_uniforms.textureWrapMode[0] = static_cast<int32_t>(mesh.wrapModeU);
  m_uniforms.textureWrapMode[1] = static_cast<int32_t>(mesh.wrapModeV);

  m_uniforms.b_noLosReturn = ShaderBool(mesh.noLosReturn);

  // R3DShader::SetMeshUniforms dirty-checks noLosReturn and layered separately, and noLosReturn
  // runs first and installs the non-layered configuration, so a layered mesh following one with
  // a different noLosReturn draws with de-duplication off and keeps all its polygons. Mirrored.
  {
    const bool layered = mesh.layered != 0;
    const bool noLosReturn = mesh.noLosReturn != 0;
    if (m_meshStateDirty || noLosReturn != m_shadowNoLosReturn)
    {
      m_shadowNoLosReturn = noLosReturn;
      m_effectiveLayered = false;
    }
    if (m_meshStateDirty || layered != m_shadowLayered)
    {
      m_shadowLayered = layered;
      m_effectiveLayered = layered;
    }
    m_meshStateDirty = false;
  }

  // translatorMap was a loose GL uniform; VulkanShaderInterface.h puts it in
  // the push constant block alongside the model state.
  m_push.b_translatorMap = ShaderBool(mesh.translatorMap);
}

bool VulkanNew3D::BindMeshUniforms(VkCommandBuffer commandBuffer, VkPipelineLayout layout)
{
  if (!m_lastUniformValid ||
      std::memcmp(&m_lastUniforms, &m_uniforms, sizeof(R3DUniforms)) != 0)
  {
    void *mapped = nullptr;
    VkDeviceSize offset = 0;
    if (m_context.UniformRing().Suballocate(sizeof(R3DUniforms),
                                            m_context.UniformOffsetAlignment(), &mapped, &offset))
    {
      std::memcpy(mapped, &m_uniforms, sizeof(R3DUniforms));
      m_lastUniforms = m_uniforms;
      m_lastUniformOffset = offset;
      m_lastUniformValid = true;
    }
    else
    {
      if (!m_uniformOverflowReported)
      {
        ErrorLog("The Vulkan uniform ring buffer ran out of space for this frame's Real3D state; "
                 "the remaining draws reuse the last uniform block.");
        m_uniformOverflowReported = true;
      }
      if (!m_lastUniformValid)
        return false;
    }
  }

  // Bindings persist within a command buffer, so tracking what is bound avoids a rebind per
  // mesh. BeginScenePass clears it in case another program's layout intervened.
  const uint32_t dynamicOffset = static_cast<uint32_t>(m_lastUniformOffset);
  if (!m_descriptorBound || dynamicOffset != m_boundDescriptorOffset ||
      layout != m_boundDescriptorLayout)
  {
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            kR3DDescriptorSet, 1, &m_r3dSet, 1, &dynamicOffset);
    m_boundDescriptorOffset = dynamicOffset;
    m_boundDescriptorLayout = layout;
    m_descriptorBound = true;
  }
  return true;
}

VkPipeline VulkanNew3D::PipelineForMesh(New3D::Layer layer, const New3D::Mesh &mesh)
{
  // Only three distinct pipelines exist per layer and the rest of the key is fixed for the
  // frame, so caching the last answer avoids building and hashing a key per mesh.
  // Invalidated whenever a pass begins.
  const bool noLosReturn = mesh.noLosReturn != 0;
  if (m_lastPipeline != VK_NULL_HANDLE && layer == m_lastPipelineLayer &&
      m_effectiveLayered == m_lastPipelineLayered && noLosReturn == m_lastPipelineNoLos)
    return m_lastPipeline;

  const PipelineKey key =
      PipelineCache::MakeR3DKey(m_context.PipelineTargets(), m_program, layer, m_effectiveLayered,
                                mesh.noLosReturn, m_losActive);
  std::string error;
  VkPipeline pipeline = m_context.Pipelines().Get(m_context.Device(), key, &error);
  if (pipeline == VK_NULL_HANDLE && !m_pipelineErrorReported)
  {
    ErrorLog("Unable to obtain a Vulkan Real3D pipeline: %s", error.c_str());
    m_pipelineErrorReported = true;
  }

  if (pipeline != VK_NULL_HANDLE)
  {
    m_lastPipeline = pipeline;
    m_lastPipelineLayer = layer;
    m_lastPipelineLayered = m_effectiveLayered;
    m_lastPipelineNoLos = noLosReturn;
  }
  return pipeline;
}

// ---------------------------------------------------------------------------
// Render passes
// ---------------------------------------------------------------------------

void VulkanNew3D::BeginScenePass(SceneTargets::ScenePass pass)
{
  if (m_scenePassActive)
    return;

  if (!m_losActive)
  {
    if (pass == SceneTargets::ScenePass::ClearAll)
      pass = SceneTargets::ScenePass::ClearAllNoLos;
    else if (pass == SceneTargets::ScenePass::BeginLayer)
      pass = SceneTargets::ScenePass::BeginLayerNoLos;
  }

  m_context.Targets().RecordBeginScenePass(m_context.CommandBuffer(), pass);
  m_scenePassActive = true;
  // Binding state is re-established inside every pass.
  m_boundPipeline = VK_NULL_HANDLE;
  m_lastPipeline = VK_NULL_HANDLE;
  m_descriptorBound = false;
}

void VulkanNew3D::EndScenePass()
{
  if (!m_scenePassActive)
    return;
  vkCmdEndRenderPass(m_context.CommandBuffer());
  m_scenePassActive = false;
  m_boundPipeline = VK_NULL_HANDLE;
  m_lastPipeline = VK_NULL_HANDLE;
  m_descriptorBound = false;
}

// ---------------------------------------------------------------------------
// Fog
// ---------------------------------------------------------------------------

void VulkanNew3D::DrawFogQuad(const New3D::Viewport &viewport, const float rgba[4],
                              float attenuation, float ambient, const float spotFogColor[3],
                              const float spotEllipse[4])
{
  VkCommandBuffer commandBuffer = m_context.CommandBuffer();

  const PipelineKey key = PipelineCache::MakeScrollFogKey(m_context.PipelineTargets(), m_losActive);
  std::string error;
  VkPipeline pipeline = m_context.Pipelines().Get(m_context.Device(), key, &error);
  if (pipeline == VK_NULL_HANDLE)
  {
    if (!m_pipelineErrorReported)
    {
      ErrorLog("Unable to obtain the Vulkan scroll fog pipeline: %s", error.c_str());
      m_pipelineErrorReported = true;
    }
    return;
  }

  ScrollFogUniforms uniforms{};
  uniforms.fogColour[0] = rgba[0];
  uniforms.fogColour[1] = rgba[1];
  uniforms.fogColour[2] = rgba[2];
  uniforms.fogColour[3] = rgba[3];
  for (int i = 0; i < 4; i++)
    uniforms.spotEllipse[i] = spotEllipse[i];
  uniforms.spotFogColor[0] = spotFogColor[0];
  uniforms.spotFogColor[1] = spotFogColor[1];
  uniforms.spotFogColor[2] = spotFogColor[2];
  uniforms.spotFogColor[3] = 0.0f;
  const std::array<float, 2> transform =
      MakeFragCoordYTransform(static_cast<float>(m_context.Targets().Height()));
  uniforms.fragCoordYTransform[0] = transform[0];
  uniforms.fragCoordYTransform[1] = transform[1];
  uniforms.fogAttenuation = attenuation;
  uniforms.fogAmbient = ambient;

  void *mapped = nullptr;
  VkDeviceSize offset = 0;
  if (!m_context.UniformRing().Suballocate(sizeof(ScrollFogUniforms),
                                           m_context.UniformOffsetAlignment(), &mapped, &offset))
  {
    if (!m_uniformOverflowReported)
    {
      ErrorLog("The Vulkan uniform ring buffer ran out of space for the scroll fog state.");
      m_uniformOverflowReported = true;
    }
    return;
  }
  std::memcpy(mapped, &uniforms, sizeof(uniforms));

  // glViewport(vp.x, vp.y, vp.width, vp.height) before the full screen quad.
  SetSceneViewport(viewport);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  m_boundPipeline = pipeline;

  const uint32_t dynamicOffset = static_cast<uint32_t>(offset);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_context.Pipelines().LayoutFor(Program::ScrollFog),
                          kScrollFogDescriptorSet, 1, &m_scrollFogSet, 1, &dynamicOffset);
  // Binding through another program's layout can disturb the R3D set, so make the
  // next mesh rebind rather than trust the cache.
  m_descriptorBound = false;
  vkCmdDraw(commandBuffer, kFullScreenQuadVertexCount, 1, 0, 0);
}

void VulkanNew3D::DrawScrollFog()
{
  // Identical selection logic to CNew3D::DrawScrollFog: highest scroll fog value starting
  // from the lowest priority layer, then a viewport of the same fog colour that fogs.
  New3D::Node *nodePtr = nullptr;

  for (int i = 0; i < 4 && !nodePtr; i++)
  {
    for (auto &n : m_nodes)
    {
      if (n.viewport.priority == i)
      {
        if (n.viewport.scrollFog > 0.f)
        {
          // check to see if we have a higher scroll fog value
          if (nodePtr)
          {
            if (nodePtr->viewport.scrollFog < n.viewport.scrollFog)
              nodePtr = &n;

            continue;
          }

          nodePtr = &n;
        }
      }
    }
  }

  if (!nodePtr)
    return;

  for (auto &n : m_nodes)
  {
    if (nodePtr->viewport.fogColour[0] == n.viewport.fogColour[0] &&
        nodePtr->viewport.fogColour[1] == n.viewport.fogColour[1] &&
        nodePtr->viewport.fogColour[2] == n.viewport.fogColour[2])
    {
      // spotlight on fog is disabled if colour = black
      bool spotOnFogEnabled = false;
      for (auto c : n.viewport.spotFogColor)
      {
        if (c > 0.0f)
        {
          spotOnFogEnabled = true;
          break;
        }
      }

      const bool fog = n.viewport.fogDensity > 0.0f || n.viewport.fogStart > 0.0f ||
                       n.viewport.scrollAtt > 0.0f;
      const bool spotlightFog = n.viewport.fogAttenuation > 0.0f && spotOnFogEnabled;

      if (fog || spotlightFog)
      {
        auto &vp = nodePtr->viewport;
        float rgba[4];

        rgba[0] = vp.fogColour[0];
        rgba[1] = vp.fogColour[1];
        rgba[2] = vp.fogColour[2];
        rgba[3] = vp.scrollFog;

        // if regular fog is enabled h/w seems to only apply this and skip the
        // spotlight on fog (ECA)
        const float fogAttentuation = (fog && spotlightFog) ? 0.0f : n.viewport.fogAttenuation;

        DrawFogQuad(vp, rgba, fogAttentuation, n.viewport.fogAmbient, n.viewport.spotFogColor,
                    n.viewport.spotEllipse);
        break;
      }
    }
  }
}

void VulkanNew3D::DrawAmbientFog()
{
  // Identical to CNew3D::DrawAmbientFog: the lowest fog ambient value from the
  // first priority layer that has any viewports at all.
  float fogAmbient = 1.0f;
  New3D::Node *nodePtr = nullptr;

  for (int i = 0; i < 4; i++)
  {
    bool hasPriority = false;

    for (auto &n : m_nodes)
    {
      auto &vp = n.viewport;

      if (vp.priority == i)
      {
        hasPriority = true;

        if (vp.scrollFog > 0.0f)
          continue;

        // if we don't have a fog density or fog start skip this viewport
        if (vp.fogDensity <= 0.0f && vp.fogStart <= 0.0f)
          continue;

        if (vp.fogAmbient < fogAmbient)
        {
          nodePtr = &n;
          fogAmbient = vp.fogAmbient;
        }
      }
    }

    if (nodePtr || hasPriority)
      break;
  }

  if (nodePtr)
  {
    auto &vp = nodePtr->viewport;
    const float rgba[] = { 0.0f, 0.0f, 0.0f, 1.0f - fogAmbient };
    // we assume spot light is not used
    DrawFogQuad(vp, rgba, 0.0f, 1.0f, vp.spotFogColor, vp.spotEllipse);
  }
}

// ---------------------------------------------------------------------------
// Line of sight
// ---------------------------------------------------------------------------

void VulkanNew3D::LosCompletion(void *user, uint64_t token)
{
  VulkanNew3D *self = static_cast<VulkanNew3D *>(user);
  if (self == nullptr)
    return;
  self->ResolveLos(static_cast<uint32_t>(token >> 8), static_cast<int>(token & 0xff));
}

void VulkanNew3D::ResolveLos(uint32_t frameIndex, int priority)
{
  if (priority < 0 || priority >= static_cast<int>(Context::kLosSlotsPerFrame))
    return;
  LosSlot &slot = m_losSlots[frameIndex % Context::kFramesInFlight][static_cast<uint32_t>(priority)];
  if (!slot.pending)
    return;
  slot.pending = false;

  uint32_t depthBits = 0;
  uint32_t noLosReturn = 0;
  std::string error;
  if (!m_context.ReadLosSlot(frameIndex, static_cast<uint32_t>(priority), &depthBits, &noLosReturn,
                             &error))
  {
    ErrorLog("Vulkan line-of-sight readback failed: %s", error.c_str());
    return;
  }

  // Decoded exactly as the SUPERMODEL_GLES branch of CNew3D::ProcessLos does.
  float depth = 0.0f;
  static_assert(sizeof(depth) == sizeof(depthBits), "LOS depth storage must match float size");
  std::memcpy(&depth, &depthBits, sizeof(depth));

  unsigned char stencilVal = noLosReturn != 0 ? 0x80 : 0x00;
  float zVal = depth / NEAR_PLANE;

  // apply our mask to stencil, because layered poly attributes use the lower bits
  stencilVal &= 0x80;

  // if the stencil val is zero that means we've hit sky or whatever, if it hits
  // a 1 we've hit geometry. The real3d returns 1 in the top bit of the float if
  // the line of sight test passes (ie doesn't hit geometry)
  auto zValP = reinterpret_cast<unsigned char *>(&zVal);   // this is legal in c++, casting to int technically isn't

  if (stencilVal == 0)
    zValP[0] |= 1;      // set first bit to 1
  else
    zValP[0] &= 0xFE;   // set first bit to zero

  std::lock_guard<std::mutex> guard(m_losMutex);
  m_losBack->value[priority] = zVal;
}

bool VulkanNew3D::ProcessLos(int priority)
{
  if (priority < 0 || priority >= static_cast<int>(Context::kLosSlotsPerFrame))
    return false;

  for (const auto &n : m_nodes)
  {
    if (n.viewport.priority == priority)
    {
      if (n.viewport.losPosX || n.viewport.losPosY)
      {
        int losX = 0;
        int losY = 0;
        TranslateLosPosition(n.viewport.losPosX, n.viewport.losPosY, losX, losY);

        // TranslateLosPosition() works in GL framebuffer coordinates, origin at the lower
        // left; a Vulkan image copy offset counts from the top.
        const SceneTargets &targets = m_context.Targets();
        const int32_t width = static_cast<int32_t>(targets.Width());
        const int32_t height = static_cast<int32_t>(targets.Height());
        const int32_t x = ClampToRange(losX, 0, width - 1);
        const int32_t y = ClampToRange(height - 1 - losY, 0, height - 1);

        const uint32_t frameIndex = m_context.FrameIndex();
        const uint32_t slotIndex = static_cast<uint32_t>(priority);

        // Never read the texel back here: copy it into this frame's readback
        // slot and decode it when the frame's fence signals.
        m_context.Targets().RecordLosCopyToBuffer(m_context.CommandBuffer(), x, y,
                                                  m_context.LosReadbackBuffer(),
                                                  m_context.LosReadbackOffset(frameIndex, slotIndex));

        LosSlot &slot = m_losSlots[frameIndex][slotIndex];
        slot.pending = true;

        std::string error;
        const uint64_t token = (static_cast<uint64_t>(frameIndex) << 8) |
                               static_cast<uint64_t>(slotIndex);
        if (!m_context.AddFrameCompletion(&LosCompletion, this, token, &error))
        {
          slot.pending = false;
          ErrorLog("Unable to schedule the Vulkan line-of-sight readback: %s", error.c_str());
        }

        return true;
      }
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

bool VulkanNew3D::RenderScene(int priority, bool renderOverlay, New3D::Layer layer)
{
  VkCommandBuffer commandBuffer = m_context.CommandBuffer();
  const VkPipelineLayout layout = m_context.Pipelines().LayoutFor(m_program);

  // The GL renderer bound the VAO and both texture banks here; the banks live in the
  // persistent descriptor set and the vertex buffer is bound once per pass.
  const VkBuffer vertexBuffer = m_vertexBuffer.Handle();

  // Each mesh is reached through its region's binding offset, so firstVertex stays
  // within that region rather than indexing across the whole buffer.
  VkDeviceSize boundVertexOffset = ~VkDeviceSize(0);

  bool hasOverlay = false;   // (high priority polys)

  for (auto &n : m_nodes)
  {
    if (n.viewport.priority != priority || n.models.empty())
      continue;

    CalcViewport(&n.viewport);
    SetSceneViewport(n.viewport);
    FillViewportUniforms(n.viewport);

    for (auto &m : n.models)
    {
      bool matrixLoaded = false;

      if (m.meshes->empty())
        continue;

      for (auto &mesh : *m.meshes)
      {
        if (mesh.highPriority)
          hasOverlay = true;

        if (!mesh.Render(layer, m.alpha))
          continue;
        if (mesh.highPriority != renderOverlay)
          continue;

        if (!matrixLoaded)
        {
          // do this here to stop loading matrices we don't need. Ie when
          // rendering non transparent etc
          ApplyModelState(m);
          matrixLoaded = true;
        }

        FillMeshUniforms(mesh);

        VkPipeline pipeline = PipelineForMesh(layer, mesh);
        if (pipeline == VK_NULL_HANDLE)
          continue;
        if (pipeline != m_boundPipeline)
        {
          vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
          m_boundPipeline = pipeline;
        }

        if (!BindMeshUniforms(commandBuffer, layout))
          continue;

        vkCmdPushConstants(commandBuffer, layout, kR3DPushConstantRange.stageFlags, 0,
                           static_cast<uint32_t>(sizeof(m_push)), &m_push);

        const VkDeviceSize bindOffset = VertexBindOffsetFor(mesh);
        if (bindOffset != boundVertexOffset)
        {
          vkCmdBindVertexBuffers(commandBuffer, kR3DVertexBinding, 1, &vertexBuffer, &bindOffset);
          boundVertexOffset = bindOffset;
        }

        // Same primitive counts as GL: three vertices per triangle, four per
        // GL_LINES_ADJACENCY quad.
        vkCmdDraw(commandBuffer, static_cast<uint32_t>(mesh.vertexCount), 1, FirstVertexFor(mesh), 0);
      }
    }
  }

  return hasOverlay;
}

void VulkanNew3D::RecordWhiteFlash()
{
  // Goes into sceneColour, not the output image: as in GL it must wipe the already-drawn
  // bottom 2D layer so TileGen's top layer draws over white. Main's display scissor still
  // confines it, and a loadOp clear ignores vkCmdSetScissor, so restrict the render area.
  VkCommandBuffer commandBuffer = m_context.CommandBuffer();
  VkClearColorValue white{};
  white.float32[0] = 1.0f;
  white.float32[1] = 1.0f;
  white.float32[2] = 1.0f;
  white.float32[3] = 1.0f;
  m_context.Targets().RecordSceneColourForRendering(commandBuffer);
  m_context.Targets().RecordBeginSceneColourPass(commandBuffer, SceneTargets::SimplePass::Clear,
                                                white, &m_displayScissor);
  vkCmdEndRenderPass(commandBuffer);
}

void VulkanNew3D::PrepareFrame()
{
  PrepareScene();

  // Same condition ProcessLos() applies per viewport. Deciding it up front lets the
  // base and colour-layer passes leave the LOS attachment unused for the whole frame.
  m_losActive = false;
  for (const auto &n : m_nodes)
  {
    if (n.viewport.losPosX || n.viewport.losPosY)
    {
      m_losActive = true;
      break;
    }
  }
}

void VulkanNew3D::RenderFrame()
{
  if (!ScenePrepared())
    PrepareFrame();                        // not hoisted this frame, so do it now
  ClearScenePrepared();

  if (!m_initialized || !m_context.FrameActive() || !m_context.SceneTargetsValid())
    return;

  // Before the banks are made sampleable below, and before the white flash can
  // return early, so a run of skipped frames cannot strand them.
  DrainPendingTextureUploads();

  if (m_blockCullingLatched)               // block culling disables 3D rendering
  {
    RecordWhiteFlash();
    return;
  }

  // Cheap identity comparison on the steady-state path; only re-allocates when
  // the pipeline cache has been rebuilt underneath us.
  std::string descriptorError;
  if (!EnsureDescriptors(&descriptorError))
  {
    if (!m_descriptorErrorReported)
    {
      ErrorLog("Unable to refresh the Vulkan Real3D descriptor sets: %s", descriptorError.c_str());
      m_descriptorErrorReported = true;
    }
    return;
  }

  VkCommandBuffer commandBuffer = m_context.CommandBuffer();

  // Both banks must be sampleable before the first draw; this is a no-op unless
  // UploadTextures() recorded a transfer into them.
  m_textureBank[0].RecordPendingTransition(commandBuffer);
  m_textureBank[1].RecordPendingTransition(commandBuffer);

  UploadVertexData();

  // Context::BeginFrame reset the uniform ring, so last frame's offset means nothing.
  m_lastUniformValid = false;
  m_boundPipeline = VK_NULL_HANDLE;
  m_lastPipeline = VK_NULL_HANDLE;
  m_descriptorBound = false;

  // colour draws to all buffers; for opaque pixels the transparent layers are masked.
  // ScenePass::ClearAll is the glClear(GL_COLOR_BUFFER_BIT) equivalent.
  BeginScenePass(SceneTargets::ScenePass::ClearAll);
  DrawAmbientFog();
  DrawScrollFog();                         // fog layer if applicable must be drawn here
  EndScenePass();

  for (int pri = 0; pri <= 3; pri++)
  {
    if (SkipLayer(pri))
      continue;

    for (int i = 0; i < 2; i++)
    {
      const bool renderOverlay = (i == 1);

      // CNew3D::SetRenderStates() ran R3DShader::Start() here, which marks the mesh
      // state dirty for the first mesh of all three layer passes below.
      m_meshStateDirty = true;
      m_shadowLayered = false;
      m_shadowNoLosReturn = false;

      // glClearDepth(0.0); glClear(DEPTH | STENCIL); PrepareLos().
      BeginScenePass(SceneTargets::ScenePass::BeginLayer);

      m_uniforms.b_discardAlpha = ShaderBool(true);
      m_uniforms.colourLayer = kR3DLayerColour;
      const bool hasOverlay = RenderScene(pri, renderOverlay, New3D::Layer::colour);

      EndScenePass();

      if (!renderOverlay)
        ProcessLos(pri);

      // glDepthFunc(GL_GREATER) for both transparency layers is part of the
      // pipeline key; DiscardAlpha(false) is not.
      m_uniforms.b_discardAlpha = ShaderBool(false);

      m_context.Targets().RecordStoreDepth(commandBuffer);
      m_uniforms.colourLayer = kR3DLayerTrans0;
      BeginScenePass(SceneTargets::ScenePass::TransFirst);
      RenderScene(pri, renderOverlay, New3D::Layer::trans1);
      EndScenePass();

      m_context.Targets().RecordRestoreDepth(commandBuffer);
      m_uniforms.colourLayer = kR3DLayerTrans1;
      BeginScenePass(SceneTargets::ScenePass::TransSecond);
      RenderScene(pri, renderOverlay, New3D::Layer::trans2);
      EndScenePass();

      if (!hasOverlay)
        break;                             // no high priority polys
    }
  }

  // The GL renderer finished with R3DFrameBuffers::Draw()/DrawScaled().
  RecordFrameBufferComposite();
}

void VulkanNew3D::RecordFrameBufferComposite()
{
  std::string error;
  bool recorded = false;

  if (m_renderScale > 0)
  {
    // R3DFrameBuffers::DrawScaled equivalent. The attachments are smaller than sceneColour,
    // so the result goes at the output rectangle, already in sceneColour's coordinate space
    // because Main scales every Init() argument by Supersampling.
    recorded = m_composite.RecordScaled(m_context, static_cast<int32_t>(m_outputXOffs),
                                        static_cast<int32_t>(m_outputYOffs), m_outputXRes,
                                        m_outputYRes, m_displayScissor, &error);
  }
  else
  {
    // m_r3dFrameBuffers.Draw(). Attachments and sceneColour are the same size here, so the
    // whole scene image is covered.
    recorded = m_composite.Record(m_context, m_displayScissor, &error);
  }

  if (!recorded && !m_compositeErrorReported)
  {
    ErrorLog("The Vulkan New3D framebuffer composite failed: %s", error.c_str());
    m_compositeErrorReported = true;
  }
}

void VulkanNew3D::BeginFrame()
{
}

void VulkanNew3D::EndFrame()
{
  EndScenePass();
}

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
