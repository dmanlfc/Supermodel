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

#include "VulkanRender2D.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

#include "Model3/TileGenBuffer.h"
#include "OSD/Logger.h"

namespace Vulkan
{
namespace
{

// glDrawArrays(GL_TRIANGLE_STRIP, 0, 4) in CRender2D::DrawSurface.
constexpr uint32_t kFullScreenQuadVertexCount = 4;

// The TileGen surfaces are 32-bit little endian RGBA, what
// glTexSubImage2D(..., GL_RGBA, GL_UNSIGNED_BYTE, ...) consumed.
constexpr VkFormat kSurfaceFormat = VK_FORMAT_R8G8B8A8_UNORM;

std::string VkError(const char *operation, VkResult result)
{
  std::ostringstream message;
  message << operation << " failed with VkResult " << static_cast<int>(result);
  return message.str();
}

} // anonymous namespace

Render2D::Render2D(Context &context, const Util::Config::Node &config)
  : m_config(config), m_context(context)
{
  m_uploaded.fill(nullptr);
  m_sets.fill(VK_NULL_HANDLE);
}

Render2D::~Render2D()
{
  Destroy();
}

void Render2D::Destroy()
{
  const DeviceContext &device = m_context.Device();
  if (device.device == VK_NULL_HANDLE)
    return;

  // The surfaces and descriptor sets may still be referenced by frames in flight. The
  // objects are released either way, so a failed wait is only reported.
  const VkResult idle = vkDeviceWaitIdle(device.device);
  if (idle != VK_SUCCESS)
    ErrorLog("%s", VkError("vkDeviceWaitIdle(TileGen shutdown)", idle).c_str());

  m_descriptors.Destroy(device);
  m_sets.fill(VK_NULL_HANDLE);
  m_setLayout = VK_NULL_HANDLE;

  for (Image &surface : m_surfaces)
    surface.Destroy(device);
  m_uploaded.fill(nullptr);

  if (m_sampler != VK_NULL_HANDLE)
  {
    vkDestroySampler(device.device, m_sampler, nullptr);
    m_sampler = VK_NULL_HANDLE;
  }
  m_initialized = false;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool Render2D::CreateSurfaces(std::string *error)
{
  for (Image &surface : m_surfaces)
  {
    if (surface.Valid())
      continue;
    if (!surface.Create(m_context.Device(), kSurfaceFormat, kSurfaceWidth, kSurfaceHeight, 1,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, error))
      return false;
  }
  return true;
}

bool Render2D::CreateDescriptors(std::string *error)
{
  const DeviceContext &device = m_context.Device();
  const VkDescriptorSetLayout layout = m_context.Pipelines().SetLayout(SetLayoutId::Tilegen);
  if (layout == VK_NULL_HANDLE)
    return SetError2D(error, "The Vulkan tilegen descriptor set layout is unavailable");

  if (m_setLayout == layout && m_sets[0] != VK_NULL_HANDLE)
    return true;

  // First frame, or Context::EnsureSceneTargets() rebuilt the pipeline cache; it idles first.
  m_descriptors.Destroy(device);
  m_sets.fill(VK_NULL_HANDLE);

  const std::vector<VkDescriptorPoolSize> sizes = {
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSurfaceCount },
  };
  if (!m_descriptors.Create(device, sizes, kSurfaceCount, error))
    return false;

  std::array<VkDescriptorImageInfo, kSurfaceCount> images{};
  std::array<VkWriteDescriptorSet, kSurfaceCount> writes{};
  for (uint32_t i = 0; i < kSurfaceCount; ++i)
  {
    if (!m_descriptors.Allocate(layout, &m_sets[i], error))
      return false;

    images[i].sampler = m_sampler;
    images[i].imageView = m_surfaces[i].View();
    // PreRenderFrame() leaves both surfaces in this layout.
    images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = m_sets[i];
    writes[i].dstBinding = kTilegenBindingTex1;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &images[i];
  }

  vkUpdateDescriptorSets(device.device, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
  m_setLayout = layout;
  return true;
}

bool Render2D::SetError2D(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

Result Render2D::Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes,
                      unsigned totalXRes, unsigned totalYRes, UpscaleMode upscaleMode)
{
  // Identical to CRender2D::Init.
  m_xPixels = xRes;
  m_yPixels = yRes;
  m_xOffset = xOffset;
  m_yOffset = yOffset;
  m_totalXPixels = totalXRes;
  m_totalYPixels = totalYRes;
  m_correction = static_cast<unsigned>(((yRes / 384.) * 2.) + 0.5);  // the 2D layer is 2 pixels off the 3D
  const UpscaleMode requestedMode =
      (m_xPixels == kSurfaceWidth && m_yPixels == kSurfaceHeight) ? UpscaleMode::Nearest
                                                                 : upscaleMode;

  std::string error;
  const VkFilter filter = TilegenSamplerFilter(static_cast<int>(requestedMode));
  const bool filterChanged =
      m_sampler != VK_NULL_HANDLE &&
      TilegenSamplerFilter(static_cast<int>(m_upscaleMode)) != filter;
  m_upscaleMode = requestedMode;

  if (filterChanged)
  {
    // Only reachable across a resolution change, which idles the device.
    const VkResult idle = vkDeviceWaitIdle(m_context.Handle());
    if (idle != VK_SUCCESS)
    {
      ErrorLog("%s", VkError("vkDeviceWaitIdle(TileGen sampler change)", idle).c_str());
      return Result::FAIL;
    }
    vkDestroySampler(m_context.Handle(), m_sampler, nullptr);
    m_sampler = VK_NULL_HANDLE;
    m_descriptors.Destroy(m_context.Device());
    m_sets.fill(VK_NULL_HANDLE);
    m_setLayout = VK_NULL_HANDLE;
  }

  if (m_sampler == VK_NULL_HANDLE)
  {
    // glTexParameteri(GL_TEXTURE_WRAP_S/T, GL_CLAMP_TO_EDGE) plus the
    // GL_NEAREST/GL_LINEAR choice CRender2D made from the upscale mode.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = filter;
    samplerInfo.minFilter = filter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    const VkResult result = vkCreateSampler(m_context.Handle(), &samplerInfo, nullptr, &m_sampler);
    if (result != VK_SUCCESS)
    {
      m_sampler = VK_NULL_HANDLE;
      ErrorLog("%s", VkError("vkCreateSampler(tilegen)", result).c_str());
      return Result::FAIL;
    }
  }

  if (!CreateSurfaces(&error))
  {
    ErrorLog("Unable to create the Vulkan TileGen surfaces: %s", error.c_str());
    return Result::FAIL;
  }

  // The surfaces must leave VK_IMAGE_LAYOUT_UNDEFINED before they are sampled, in case a
  // frame draws before the tile generator has produced anything.
  if (m_surfaces[0].CurrentLayout() == VK_IMAGE_LAYOUT_UNDEFINED)
  {
    ScopedCommandBuffer scoped;
    if (!scoped.Begin(m_context.Device(), &error))
    {
      ErrorLog("Unable to prepare the Vulkan TileGen surfaces: %s", error.c_str());
      return Result::FAIL;
    }

    // Cleared, not just transitioned: a frame drawing before the first upload shows black.
    const VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    for (Image &surface : m_surfaces)
    {
      surface.RecordTransition(scoped.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      vkCmdClearColorImage(scoped.Handle(), surface.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &black, 1, &range);
      surface.RecordTransition(scoped.Handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (!scoped.SubmitAndWait(m_context.Device(), &error))
    {
      ErrorLog("Unable to prepare the Vulkan TileGen surfaces: %s", error.c_str());
      return Result::FAIL;
    }
  }

  if (!CreateDescriptors(&error))
  {
    ErrorLog("Unable to create the Vulkan TileGen descriptor sets: %s", error.c_str());
    return Result::FAIL;
  }

  m_initialized = true;
  return Result::OKAY;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

VkViewport Render2D::SurfaceViewport(bool isBottom) const
{
  const SceneTargets &targets = m_context.Targets();
  // The Y flip is measured against sceneColour's height, not the smaller attachment height.
  const float targetHeight = static_cast<float>(targets.SceneHeight());

  // CRender2D::Setup2D: the bottom layer covers the whole target when WideBackground is set,
  // otherwise both layers use the corrected, centred content rectangle.
  const bool stretchBottom = m_config["WideBackground"].ValueAs<bool>() && isBottom;
  if (stretchBottom)
    return targets.MakeViewport(0.0f, 0.0f, static_cast<float>(m_totalXPixels),
                                static_cast<float>(m_totalYPixels));

  // glViewport(m_xOffset - m_correction, m_yOffset + m_correction, m_xPixels, m_yPixels),
  // whose origin is the lower left corner.
  const float x = static_cast<float>(static_cast<int>(m_xOffset) - static_cast<int>(m_correction));
  const float bottom = static_cast<float>(m_yOffset + m_correction);
  const float height = static_cast<float>(m_yPixels);
  return targets.MakeViewport(x, targetHeight - (bottom + height),
                              static_cast<float>(m_xPixels), height);
}

VkRect2D Render2D::ContentScissor() const
{
  const SceneTargets &targets = m_context.Targets();
  // Clipped against the sceneColour image the 2D layers are drawn into; the attachment size
  // would clip the picture down to a 496x384 corner when RenderScale is on.
  const int32_t width = static_cast<int32_t>(targets.SceneWidth());
  const int32_t height = static_cast<int32_t>(targets.SceneHeight());

  // The rectangle Main's SetGLGeometry installed with glScissor, which stayed enabled for
  // every 2D draw. Expressed with the origin at the top left, where a Vulkan scissor counts.
  int32_t left = 0;
  int32_t bottom = 0;
  int32_t scissorWidth = width;
  int32_t scissorHeight = height;
  const int32_t correction = static_cast<int32_t>(m_correction);
  if (m_config["WideScreen"].ValueAs<bool>())
  {
    bottom = correction;
    scissorWidth = static_cast<int32_t>(m_totalXPixels);
    scissorHeight = static_cast<int32_t>(m_totalYPixels) - correction * 2;
  }
  else
  {
    left = static_cast<int32_t>(m_xOffset) + correction;
    bottom = static_cast<int32_t>(m_yOffset) + correction;
    scissorWidth = static_cast<int32_t>(m_xPixels) - correction * 2;
    scissorHeight = static_cast<int32_t>(m_yPixels) - correction * 2;
  }

  scissorWidth = std::max(scissorWidth, 0);
  scissorHeight = std::max(scissorHeight, 0);
  const int32_t top = height - (bottom + scissorHeight);

  const int32_t clampedLeft = std::min(std::max(left, 0), width);
  const int32_t clampedTop = std::min(std::max(top, 0), height);
  const uint32_t clampedWidth =
      static_cast<uint32_t>(std::min(scissorWidth, width - clampedLeft));
  const uint32_t clampedHeight =
      static_cast<uint32_t>(std::min(scissorHeight, height - clampedTop));
  return m_context.Targets().MakeScissor(clampedLeft, clampedTop, clampedWidth, clampedHeight);
}

// ---------------------------------------------------------------------------
// Uploads
// ---------------------------------------------------------------------------

bool Render2D::Upload(uint32_t index, std::string *error)
{
  const TileGenBuffer *buffer = m_drawBuffers[index].get();
  if (buffer == nullptr)
    return true;                       // no draw buffer yet, as in CRender2D
  if (m_uploaded[index] == buffer)
    return true;                       // the tile generator produced nothing new

  // The whole 512x384 surface is staged and the copy takes the visible 496 columns out of it,
  // as glPixelStorei(GL_UNPACK_ROW_LENGTH, 512) plus a 496x384 glTexSubImage2D did.
  const VkDeviceSize bytes =
      static_cast<VkDeviceSize>(kSurfaceStride) * kSurfaceHeight * sizeof(uint32_t);
  void *staged = nullptr;
  VkDeviceSize stagingOffset = 0;
  if (!m_context.StagingRing().Suballocate(bytes, 4, &staged, &stagingOffset))
    return SetError2D(error, "This frame's Vulkan staging slice could not hold a TileGen surface");
  std::memcpy(staged, buffer->data, static_cast<size_t>(bytes));

  VkCommandBuffer commandBuffer = m_context.CommandBuffer();
  Image &surface = m_surfaces[index];

  // Leaving SHADER_READ_ONLY_OPTIMAL also orders this write after the previous frame's
  // sampling of the same image, which may still be running.
  surface.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  VkBufferImageCopy copy{};
  copy.bufferOffset = stagingOffset;
  copy.bufferRowLength = kSurfaceStride;
  copy.bufferImageHeight = kSurfaceHeight;
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.mipLevel = 0;
  copy.imageSubresource.baseArrayLayer = 0;
  copy.imageSubresource.layerCount = 1;
  copy.imageOffset = VkOffset3D{ 0, 0, 0 };
  copy.imageExtent = VkExtent3D{ kSurfaceWidth, kSurfaceHeight, 1 };
  vkCmdCopyBufferToImage(commandBuffer, m_context.StagingRing().Handle(), surface.Handle(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  m_uploaded[index] = buffer;
  return true;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void Render2D::BeginFrame()
{
}

void Render2D::PreRenderFrame()
{
  if (!m_initialized || !m_context.FrameActive() || !m_context.SceneTargetsValid())
    return;

  std::string error;
  if (!CreateDescriptors(&error))
  {
    if (!m_uploadErrorReported)
    {
      ErrorLog("Unable to refresh the Vulkan TileGen descriptor sets: %s", error.c_str());
      m_uploadErrorReported = true;
    }
    return;
  }

  for (uint32_t i = 0; i < kSurfaceCount; ++i)
  {
    if (!Upload(i, &error) && !m_uploadErrorReported)
    {
      ErrorLog("Vulkan TileGen surface upload failed: %s", error.c_str());
      m_uploadErrorReported = true;
    }
  }

  // Both surfaces must be sampleable before the first draw; a no-op if not re-uploaded.
  VkCommandBuffer commandBuffer = m_context.CommandBuffer();
  for (Image &surface : m_surfaces)
    surface.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Render2D::DrawSurface(uint32_t index, bool isBottom)
{
  VkCommandBuffer commandBuffer = m_context.CommandBuffer();
  SceneTargets &targets = m_context.Targets();
  PipelineCache &pipelines = m_context.Pipelines();

  // CRender2D::DrawSurface blends both surfaces, not just the top one. Blending the bottom
  // layer against the freshly cleared target premultiplies it by its own alpha; without that,
  // transparent tiles show their colour instead of collapsing to black.
  const PipelineKey key = PipelineCache::MakeTilegenKey(m_context.PipelineTargets(),
                                                        static_cast<int>(m_upscaleMode), true);
  std::string error;
  VkPipeline pipeline = pipelines.Get(m_context.Device(), key, &error);
  if (pipeline == VK_NULL_HANDLE)
  {
    if (!m_pipelineErrorReported)
    {
      ErrorLog("Unable to obtain a Vulkan TileGen pipeline: %s", error.c_str());
      m_pipelineErrorReported = true;
    }
    return;
  }

  // The bottom layer clears the target, as CRender2D did with a full-target glClear and the
  // scissor test disabled; the top layer loads it.
  const VkClearColorValue clearColour = { { 0.0f, 0.0f, 0.0f, 0.0f } };
  targets.RecordBeginSceneColourPass(
      commandBuffer, isBottom ? SceneTargets::SimplePass::Clear : SceneTargets::SimplePass::Load,
      clearColour);

  const VkViewport viewport = SurfaceViewport(isBottom);
  vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
  const VkRect2D scissor = ContentScissor();
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipelines.LayoutFor(TilegenProgram(static_cast<int>(m_upscaleMode))),
                          kTilegenDescriptorSet, 1, &m_sets[index], 0, nullptr);
  vkCmdDraw(commandBuffer, kFullScreenQuadVertexCount, 1, 0, 0);

  vkCmdEndRenderPass(commandBuffer);
}

void Render2D::RenderFrameBottom()
{
  if (!m_initialized || !m_context.FrameActive() || !m_context.SceneTargetsValid())
    return;
  if (m_sets[0] == VK_NULL_HANDLE)
    return;
  DrawSurface(0, true);
}

void Render2D::RenderFrameTop()
{
  if (!m_initialized || !m_context.FrameActive() || !m_context.SceneTargetsValid())
    return;
  if (m_sets[1] == VK_NULL_HANDLE)
    return;
  DrawSurface(1, false);
}

void Render2D::EndFrame()
{
}

// Deprecated in CRender2D as well: the tile generator hands over finished surfaces.
void Render2D::WriteVRAM(unsigned, uint32_t)
{
}

void Render2D::AttachRegisters(const uint32_t *)
{
}

void Render2D::AttachPalette(const uint32_t *[2])
{
}

void Render2D::AttachVRAM(const uint8_t *)
{
}

void Render2D::AttachDrawBuffers(std::shared_ptr<TileGenBuffer> bottom,
                                 std::shared_ptr<TileGenBuffer> top)
{
  m_drawBuffers[0] = std::move(bottom);
  m_drawBuffers[1] = std::move(top);
}

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
