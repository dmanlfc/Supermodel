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

#include "VulkanCrosshair.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

#include "Inputs/Inputs.h"
#include "OSD/FileSystemPath.h"
#include "OSD/Logger.h"
#include "OSD/SDL/SDLIncludes.h"
#include "Util/Format.h"

namespace Vulkan
{
namespace
{

std::string VkError(const char *operation, VkResult result)
{
  std::ostringstream message;
  message << operation << " failed with VkResult " << static_cast<int>(result);
  return message.str();
}

// SDL_PIXELFORMAT_RGBA32 is byte order R, G, B, A on both endiannesses, matching this.
constexpr VkFormat kTextureFormat = VK_FORMAT_R8G8B8A8_UNORM;

} // anonymous namespace

Crosshair::Crosshair(Context &context, const Util::Config::Node &config)
  : m_config(config), m_context(context)
{
  m_sets.fill(VK_NULL_HANDLE);
}

Crosshair::~Crosshair()
{
  Destroy();
}

void Crosshair::Destroy()
{
  const DeviceContext &device = m_context.Device();
  if (device.device == VK_NULL_HANDLE)
    return;

  // Textures, buffers and descriptor sets may still be referenced by frames in flight.
  const VkResult idle = vkDeviceWaitIdle(device.device);
  if (idle != VK_SUCCESS)
    ErrorLog("%s", VkError("vkDeviceWaitIdle(crosshair shutdown)", idle).c_str());

  m_descriptors.Destroy(device);
  m_sets.fill(VK_NULL_HANDLE);
  m_setLayout = VK_NULL_HANDLE;

  m_vertexBuffer.Destroy(device);
  m_uvBuffer.Destroy(device);
  for (Image &texture : m_textures)
    texture.Destroy(device);

  if (m_sampler != VK_NULL_HANDLE)
  {
    vkDestroySampler(device.device, m_sampler, nullptr);
    m_sampler = VK_NULL_HANDLE;
  }
  m_initialized = false;
}

bool Crosshair::SetError(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

// ---------------------------------------------------------------------------
// Geometry, copied from CCrosshair
// ---------------------------------------------------------------------------

void Crosshair::BuildCrosshairVertices()
{
  m_verts.clear();
  if (!m_isBitmapCrosshair)
  {
    m_verts.emplace_back(0.0f, m_dist);  // bottom triangle
    m_verts.emplace_back(m_base / 2.0f, (m_dist + m_height));
    m_verts.emplace_back(-m_base / 2.0f, (m_dist + m_height));
    m_verts.emplace_back(0.0f, -m_dist);  // top triangle
    m_verts.emplace_back(-m_base / 2.0f, -(m_dist + m_height));
    m_verts.emplace_back(m_base / 2.0f, -(m_dist + m_height));
    m_verts.emplace_back(-m_dist, 0.0f);  // left triangle
    m_verts.emplace_back(-m_dist - m_height, (m_base / 2.0f));
    m_verts.emplace_back(-m_dist - m_height, -(m_base / 2.0f));
    m_verts.emplace_back(m_dist, 0.0f);  // right triangle
    m_verts.emplace_back(m_dist + m_height, -(m_base / 2.0f));
    m_verts.emplace_back(m_dist + m_height, (m_base / 2.0f));
  }
  else
  {
    m_verts.emplace_back(-m_squareSize / 2.0f, -m_squareSize / 2.0f);
    m_verts.emplace_back(m_squareSize / 2.0f, -m_squareSize / 2.0f);
    m_verts.emplace_back(m_squareSize / 2.0f, m_squareSize / 2.0f);
    m_verts.emplace_back(-m_squareSize / 2.0f, -m_squareSize / 2.0f);
    m_verts.emplace_back(m_squareSize / 2.0f, m_squareSize / 2.0f);
    m_verts.emplace_back(-m_squareSize / 2.0f, m_squareSize / 2.0f);
  }
}

void Crosshair::GunToViewCoords(float *x, float *y)
{
  *x = (*x - 150.0f) / (651.0f - 150.0f);  // Scale [150,651] -> [0.0,1.0]
  *y = (*y - 80.0f) / (465.0f - 80.0f);    // Scale [80,465] -> [0.0,1.0]
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

bool Crosshair::LoadTextures(std::string *error)
{
  const std::string files[kPlayers] = {
    Util::Format() << FileSystemPath::GetPath(FileSystemPath::Assets) << "p1crosshair.bmp",
    Util::Format() << FileSystemPath::GetPath(FileSystemPath::Assets) << "p2crosshair.bmp",
  };

  const DeviceContext &device = m_context.Device();

  for (uint32_t player = 0; player < kPlayers; ++player)
  {
    SDL_Surface *loaded = SDL_LoadBMP(files[player].c_str());
    if (loaded == nullptr)
      return SetError(error, "unable to load " + files[player] + ": " + SDL_GetError());

    // The GL path uploaded the surface's own pixels as GL_BGRA, or converted to RGBA for
    // GLES. One explicit conversion covers both and matches the image format's byte order.
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(loaded);
    if (rgba == nullptr)
      return SetError(error, "unable to convert " + files[player] + ": " + SDL_GetError());

    const uint32_t width = static_cast<uint32_t>(rgba->w);
    const uint32_t height = static_cast<uint32_t>(rgba->h);
    if (width == 0 || height == 0)
    {
      SDL_FreeSurface(rgba);
      return SetError(error, files[player] + " has no pixels");
    }

    if (!m_textures[player].Create(device, kTextureFormat, width, height, 1,
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, error))
    {
      SDL_FreeSurface(rgba);
      return false;
    }

    // One-shot host-visible staging buffer, released before returning either way.
    Buffer staging;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
    if (!staging.Create(device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        error) ||
        staging.MappedPointer() == nullptr)
    {
      staging.Destroy(device);
      SDL_FreeSurface(rgba);
      return SetError(error, "unable to stage the crosshair texture for " + files[player]);
    }

    // Row by row, because an SDL surface's pitch is not necessarily w * 4.
    uint8_t *destination = static_cast<uint8_t *>(staging.MappedPointer());
    const uint8_t *source = static_cast<const uint8_t *>(rgba->pixels);
    for (uint32_t row = 0; row < height; ++row)
      std::memcpy(destination + static_cast<size_t>(row) * width * 4u,
                  source + static_cast<size_t>(row) * static_cast<size_t>(rgba->pitch),
                  static_cast<size_t>(width) * 4u);
    SDL_FreeSurface(rgba);

    if (!staging.Flush(device, 0, bytes, error))
    {
      staging.Destroy(device);
      return false;
    }

    ScopedCommandBuffer scoped;
    if (!scoped.Begin(device, error))
    {
      staging.Destroy(device);
      return false;
    }

    m_textures[player].RecordTransition(scoped.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = width;
    copy.bufferImageHeight = height;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = 0;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset = VkOffset3D{ 0, 0, 0 };
    copy.imageExtent = VkExtent3D{ width, height, 1 };
    vkCmdCopyBufferToImage(scoped.Handle(), staging.Handle(), m_textures[player].Handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    m_textures[player].RecordTransition(scoped.Handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const bool submitted = scoped.SubmitAndWait(device, error);
    staging.Destroy(device);
    if (!submitted)
      return false;
  }

  return true;
}

bool Crosshair::CreateBuffers(std::string *error)
{
  const DeviceContext &device = m_context.Device();

  // CCrosshair sized its VBOs for MaxVerts positions. The data never changes after
  // BuildCrosshairVertices(), so a device-local write once replaces the per-draw
  // glBufferSubData.
  const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(kMaxVerts) * sizeof(BasicVertex);
  const VkDeviceSize uvBytes = static_cast<VkDeviceSize>(kMaxVerts) * sizeof(UVCoords);

  if (!m_vertexBuffer.Create(device, vertexBytes,
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, error))
    return false;
  if (!m_uvBuffer.Create(device, uvBytes,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, error))
    return false;

  // The whole range is written so the vertex stage never reads uninitialized memory: the
  // vector style draws twelve vertices but only six texture coordinates are meaningful.
  std::vector<BasicVertex> positions(kMaxVerts, BasicVertex(0.0f, 0.0f, 0.0f));
  std::copy(m_verts.begin(), m_verts.end(), positions.begin());
  std::vector<UVCoords> uvs(kMaxVerts, UVCoords(0.0f, 0.0f));
  std::copy(m_uvCoord.begin(), m_uvCoord.end(), uvs.begin());

  if (!m_context.RecordBufferUpload(m_vertexBuffer.Handle(), 0, positions.data(), vertexBytes,
                                    error))
    return false;
  if (!m_context.RecordBufferUpload(m_uvBuffer.Handle(), 0, uvs.data(), uvBytes, error))
    return false;
  return true;
}

bool Crosshair::CreateDescriptors(std::string *error)
{
  const DeviceContext &device = m_context.Device();

  if (m_sampler == VK_NULL_HANDLE)
  {
    // glTexParameteri(GL_TEXTURE_MAG_FILTER/MIN_FILTER, GL_LINEAR).
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    const VkResult result = vkCreateSampler(device.device, &samplerInfo, nullptr, &m_sampler);
    if (result != VK_SUCCESS)
    {
      m_sampler = VK_NULL_HANDLE;
      return SetError(error, VkError("vkCreateSampler(crosshair)", result));
    }
  }

  return RefreshDescriptors(error);
}

bool Crosshair::RefreshDescriptors(std::string *error)
{
  const DeviceContext &device = m_context.Device();
  const VkDescriptorSetLayout layout = m_context.Pipelines().SetLayout(SetLayoutId::Crosshair);
  if (layout == VK_NULL_HANDLE)
    return SetError(error, "The Vulkan crosshair descriptor set layout is unavailable");
  if (m_setLayout == layout && m_sets[0] != VK_NULL_HANDLE)
    return true;

  // First use, or Context::EnsureSceneTargets() rebuilt the pipeline cache; it idles first.
  m_descriptors.Destroy(device);
  m_sets.fill(VK_NULL_HANDLE);

  const std::vector<VkDescriptorPoolSize> sizes = {
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPlayers },
  };
  if (!m_descriptors.Create(device, sizes, kPlayers, error))
    return false;

  std::array<VkDescriptorImageInfo, kPlayers> images{};
  std::array<VkWriteDescriptorSet, kPlayers> writes{};
  for (uint32_t player = 0; player < kPlayers; ++player)
  {
    if (!m_descriptors.Allocate(layout, &m_sets[player], error))
      return false;

    images[player].sampler = m_sampler;
    images[player].imageView = m_textures[player].View();
    images[player].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[player].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[player].dstSet = m_sets[player];
    writes[player].dstBinding = kCrosshairBindingTexture;
    writes[player].descriptorCount = 1;
    writes[player].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[player].pImageInfo = &images[player];
  }

  vkUpdateDescriptorSets(device.device, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
  m_setLayout = layout;
  return true;
}

Result Crosshair::Init()
{
  const std::string style = Util::ToLower(m_config["CrosshairStyle"].ValueAs<std::string>());
  if (style == "bmp")
    m_isBitmapCrosshair = true;
  else if (style == "vector")
    m_isBitmapCrosshair = false;
  else
  {
    ErrorLog("Invalid crosshair style '%s', must be 'vector' or 'bmp'. Reverting to 'vector'.\n",
             style.c_str());
    m_isBitmapCrosshair = false;
  }

  std::string error;
  if (!LoadTextures(&error))
  {
    ErrorLog("Unable to load the crosshair textures: %s", error.c_str());
    Destroy();
    return Result::FAIL;
  }

  // Get DPI
  SDL_GetDisplayDPI(0, &m_diagDpi, &m_hDpi, &m_vDpi);
  m_dpiMultiplicator = m_hDpi / m_standardDpi;  // note : on linux VM diagdpi returns 0

  // 3d obj
  m_uvCoord.clear();
  m_uvCoord.emplace_back(0.0f, 0.0f);
  m_uvCoord.emplace_back(1.0f, 0.0f);
  m_uvCoord.emplace_back(1.0f, 1.0f);
  m_uvCoord.emplace_back(0.0f, 0.0f);
  m_uvCoord.emplace_back(1.0f, 1.0f);
  m_uvCoord.emplace_back(0.0f, 1.0f);

  BuildCrosshairVertices();

  if (!CreateBuffers(&error))
  {
    ErrorLog("Unable to create the Vulkan crosshair vertex buffers: %s", error.c_str());
    Destroy();
    return Result::FAIL;
  }
  if (!CreateDescriptors(&error))
  {
    ErrorLog("Unable to create the Vulkan crosshair descriptors: %s", error.c_str());
    Destroy();
    return Result::FAIL;
  }

  m_initialized = true;
  return Result::OKAY;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Crosshair::DrawCrosshair(New3D::Mat4 matrix, float x, float y, uint32_t player, unsigned xRes,
                              unsigned yRes)
{
  const float aspect = static_cast<float>(xRes) / static_cast<float>(yRes);
  uint32_t count = static_cast<uint32_t>(m_verts.size());
  if (count > kMaxVerts)
    count = kMaxVerts;   // maybe we could error out somehow

  VkCommandBuffer commandBuffer = m_context.CommandBuffer();
  PipelineCache &pipelines = m_context.Pipelines();

  std::string error;
  VkPipeline pipeline = pipelines.Get(
      m_context.Device(),
      PipelineCache::MakeCrosshairKey(m_context.PipelineTargets(), m_isBitmapCrosshair), &error);
  if (pipeline == VK_NULL_HANDLE)
  {
    if (!m_errorReported)
    {
      ErrorLog("Unable to obtain the Vulkan crosshair pipeline: %s", error.c_str());
      m_errorReported = true;
    }
    return;
  }

  matrix.Translate(x, y, 0);

  CrosshairPushConstants push{};
  if (!m_isBitmapCrosshair)
  {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    switch (player)
    {
    case 0:  // P1 red color
      r = 1.0f;
      g = 0.0f;
      b = 0.0f;
      break;
    case 1:  // P2 green color
      r = 0.0f;
      g = 1.0f;
      b = 0.0f;
      break;
    default:
      break;
    }

    matrix.Scale(m_dpiMultiplicator, m_dpiMultiplicator * aspect, 0);

    push.colour[0] = r;
    push.colour[1] = g;
    push.colour[2] = b;
    push.colour[3] = 1.0f;
    push.b_isBitmap = ShaderBool(false);
  }
  else
  {
    matrix.Scale(m_dpiMultiplicator * m_scaleBitmap, m_dpiMultiplicator * m_scaleBitmap * aspect, 0);

    push.colour[0] = 1.0f;
    push.colour[1] = 1.0f;
    push.colour[2] = 1.0f;
    push.colour[3] = 1.0f;
    push.b_isBitmap = ShaderBool(true);
  }
  std::memcpy(push.mvp, static_cast<const float *>(matrix), sizeof(push.mvp));

  const VkPipelineLayout layout = pipelines.LayoutFor(Program::Crosshair);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdPushConstants(commandBuffer, layout, kCrosshairPushConstantRange.stageFlags, 0,
                     static_cast<uint32_t>(sizeof(push)), &push);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                          kCrosshairDescriptorSet, 1, &m_sets[player], 0, nullptr);
  vkCmdDraw(commandBuffer, count, 1, 0, 0);
}

void Crosshair::Update(uint32_t currentInputs, CInputs *inputs, unsigned xOffset, unsigned yOffset,
                       unsigned xRes, unsigned yRes)
{
  bool offscreenTrigger[2]{ false };
  float x[2]{ 0.0f };
  float y[2]{ 0.0f };

  // Crosshairs can be enabled/disabled at run-time
  unsigned crosshairs = m_config["Crosshairs"].ValueAs<unsigned>();
  crosshairs &= 3;
  if (!crosshairs)
    return;
  if (!m_initialized || inputs == nullptr)
    return;
  if (!m_context.FrameActive() || !m_context.SceneTargetsValid())
    return;

  std::string error;
  if (!RefreshDescriptors(&error))
  {
    if (!m_errorReported)
    {
      ErrorLog("Unable to refresh the Vulkan crosshair descriptors: %s", error.c_str());
      m_errorReported = true;
    }
    return;
  }

  New3D::Mat4 m;
  m.Ortho(0.0, 1.0, 1.0, 0.0, -1.0f, 1.0f);

  // Convert gun coordinates to viewspace coordinates
  if (currentInputs & Game::INPUT_ANALOG_GUN1)
  {
    x[0] = (static_cast<float>(inputs->analogGunX[0]->value) / 255.0f);
    y[0] = ((255.0f - static_cast<float>(inputs->analogGunY[0]->value)) / 255.0f);
    offscreenTrigger[0] =
        inputs->analogTriggerLeft[0]->value || inputs->analogTriggerRight[0]->value;
  }
  else if (currentInputs & Game::INPUT_GUN1)
  {
    x[0] = static_cast<float>(inputs->gunX[0]->value);
    y[0] = static_cast<float>(inputs->gunY[0]->value);
    GunToViewCoords(&x[0], &y[0]);
    offscreenTrigger[0] = (inputs->trigger[0]->offscreenValue) > 0;
  }
  if (currentInputs & Game::INPUT_ANALOG_GUN2)
  {
    x[1] = (static_cast<float>(inputs->analogGunX[1]->value) / 255.0f);
    y[1] = ((255.0f - static_cast<float>(inputs->analogGunY[1]->value)) / 255.0f);
    offscreenTrigger[1] =
        inputs->analogTriggerLeft[1]->value || inputs->analogTriggerRight[1]->value;
  }
  else if (currentInputs & Game::INPUT_GUN2)
  {
    x[1] = static_cast<float>(inputs->gunX[1]->value);
    y[1] = static_cast<float>(inputs->gunY[1]->value);
    GunToViewCoords(&x[1], &y[1]);
    offscreenTrigger[1] = (inputs->trigger[1]->offscreenValue) > 0;
  }

  const bool drawPlayer1 = (crosshairs & 1) != 0 && !offscreenTrigger[0];
  const bool drawPlayer2 = (crosshairs & 2) != 0 && !offscreenTrigger[1];
  if (!drawPlayer1 && !drawPlayer2)
    return;

  SceneTargets &targets = m_context.Targets();
  VkCommandBuffer commandBuffer = m_context.CommandBuffer();

  // The crosshairs are drawn over the post-processed image, so the pass loads it.
  const VkClearColorValue unused{};
  targets.RecordBeginOutputPass(commandBuffer, SceneTargets::SimplePass::Load, unused);

  // glViewport(xOffset, yOffset, xRes, yRes) in GL's lower-left origin.
  const float top =
      static_cast<float>(targets.OutputHeight()) - static_cast<float>(yOffset + yRes);
  const VkViewport viewport = targets.MakeViewport(static_cast<float>(xOffset), top,
                                                  static_cast<float>(xRes),
                                                  static_cast<float>(yRes));
  vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

  const int32_t scissorX = std::min<int32_t>(static_cast<int32_t>(xOffset),
                                             static_cast<int32_t>(targets.OutputWidth()));
  const int32_t scissorY =
      std::min(std::max(static_cast<int32_t>(top), 0), static_cast<int32_t>(targets.OutputHeight()));
  const uint32_t scissorWidth =
      static_cast<uint32_t>(std::min<int32_t>(static_cast<int32_t>(xRes),
                                             static_cast<int32_t>(targets.OutputWidth()) - scissorX));
  const uint32_t scissorHeight = static_cast<uint32_t>(
      std::min<int32_t>(static_cast<int32_t>(yRes),
                        static_cast<int32_t>(targets.OutputHeight()) - scissorY));
  const VkRect2D scissor = targets.MakeScissor(scissorX, scissorY, scissorWidth, scissorHeight);
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

  const std::array<VkBuffer, 2> vertexBuffers = { m_vertexBuffer.Handle(), m_uvBuffer.Handle() };
  const std::array<VkDeviceSize, 2> vertexOffsets = { 0, 0 };
  vkCmdBindVertexBuffers(commandBuffer, kCrosshairVertexBindingPosition,
                         static_cast<uint32_t>(vertexBuffers.size()), vertexBuffers.data(),
                         vertexOffsets.data());

  // Draw visible crosshairs
  if (drawPlayer1)  // Player 1
    DrawCrosshair(m, x[0], y[0], 0, xRes, yRes);
  if (drawPlayer2)  // Player 2
    DrawCrosshair(m, x[1], y[1], 1, xRes, yRes);

  vkCmdEndRenderPass(commandBuffer);

  // Context::EndFrame() presents the output image only once it is marked written.
  m_context.MarkOutputImageWritten();
}

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
