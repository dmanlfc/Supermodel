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

#include "VulkanTextureBank.h"

#include <cstring>
#include <sstream>

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

bool SetError(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

// The same tables the GL uploader uses, except that GL's Y table stops at level 10; the
// trailing entry keeps level 11 in bounds without changing any level GL can address.
constexpr int mipXBase[TextureBank::kMipLevels] =
  {0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047};
constexpr int mipYBase[TextureBank::kMipLevels] =
  {0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023, 1023};

uint32_t MipExtent(uint32_t base, int level)
{
  const uint32_t shifted = base >> static_cast<uint32_t>(level);
  return (shifted != 0) ? shifted : 1u;
}

} // anonymous namespace

bool TextureBank::Create(const DeviceContext &deviceContext, std::string *error)
{
  if (deviceContext.device == VK_NULL_HANDLE)
    return SetError(error, "Vulkan texture bank creation requires an initialized device");
  if (m_image.Valid() || m_sampler != VK_NULL_HANDLE)
    return SetError(error, "Vulkan texture bank created twice without an intervening Destroy()");

  if (!m_image.Create(deviceContext, kFormat, kWidth, kHeight, kMipLevels,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, error))
    return false;

  // Mirrors the GL sampler state: point sampling within and between levels, wrapping in both
  // directions.
  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = VK_FILTER_NEAREST;
  samplerInfo.minFilter = VK_FILTER_NEAREST;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.mipLodBias = 0.0f;
  samplerInfo.anisotropyEnable = VK_FALSE;
  samplerInfo.maxAnisotropy = 1.0f;
  samplerInfo.compareEnable = VK_FALSE;
  samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = static_cast<float>(kMipLevels);
  samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  samplerInfo.unnormalizedCoordinates = VK_FALSE;

  const VkResult result = vkCreateSampler(deviceContext.device, &samplerInfo, nullptr, &m_sampler);
  if (result != VK_SUCCESS)
  {
    m_sampler = VK_NULL_HANDLE;
    m_image.Destroy(deviceContext);
    return SetError(error, VkError("vkCreateSampler(texture bank)", result));
  }

  // The GL constructor stops incrementing at the 1x1 level, so its reported count is one
  // less than the number of allocated levels. Reproduced so callers see identical values.
  m_numLevels = static_cast<int>(kMipLevels) - 1;
  return true;
}

void TextureBank::Destroy(const DeviceContext &deviceContext)
{
  if (m_sampler != VK_NULL_HANDLE && deviceContext.device != VK_NULL_HANDLE)
    vkDestroySampler(deviceContext.device, m_sampler, nullptr);
  m_sampler = VK_NULL_HANDLE;
  m_image.Destroy(deviceContext);
  m_textureRam = nullptr;
  m_numLevels = 0;
}

void TextureBank::AttachMemory(const uint16_t *textureRam)
{
  m_textureRam = textureRam;
}

bool TextureBank::HasPendingTransition() const
{
  return m_image.Valid() && m_image.CurrentLayout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void TextureBank::RecordPendingTransition(VkCommandBuffer commandBuffer)
{
  if (!HasPendingTransition())
    return;
  m_image.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  // This transition orders every write in the epoch against the shader reads that follow,
  // so nothing tracked can alias a later copy.
  m_writtenRects.clear();
}

bool TextureBank::RecordUpload(VkCommandBuffer commandBuffer, RingBuffer &staging, int level, int x,
                               int y, int width, int height, std::string *error)
{
  if (commandBuffer == VK_NULL_HANDLE)
    return SetError(error, "Texture bank upload received no command buffer");
  if (!m_image.Valid())
    return SetError(error, "Texture bank upload requested before the image was created");
  if (m_textureRam == nullptr)
    return SetError(error, "Texture bank upload requested before AttachMemory()");
  if (level < 0 || static_cast<uint32_t>(level) >= kMipLevels)
    return SetError(error, "Texture bank upload requested for an out of range mip level");
  if (width <= 0 || height <= 0)
    return SetError(error, "Texture bank upload requested for an empty rectangle");
  if (x < 0 || y < 0 || static_cast<uint32_t>(x + width) > kWidth ||
      static_cast<uint32_t>(y + height) > kHeight)
    return SetError(error, "Texture bank upload reads outside of texture RAM");

  // Absolute sheet coordinates become coordinates inside the mip level.
  const int subX = x - mipXBase[level];
  const int subY = y - mipYBase[level];
  if (subX < 0 || subY < 0 ||
      static_cast<uint32_t>(subX + width) > MipExtent(kWidth, level) ||
      static_cast<uint32_t>(subY + height) > MipExtent(kHeight, level))
    return SetError(error, "Texture bank upload falls outside of the destination mip level");

  const VkDeviceSize rowBytes = static_cast<VkDeviceSize>(width) * sizeof(uint16_t);
  const VkDeviceSize totalBytes = rowBytes * static_cast<VkDeviceSize>(height);

  // vkCmdCopyBufferToImage requires an offset that is a multiple of 4 and of the 2 byte texel.
  void *staged = nullptr;
  VkDeviceSize stagingOffset = 0;
  if (!staging.Suballocate(totalBytes, 4, &staged, &stagingOffset))
    return SetError(error, "Texture bank staging ring buffer is exhausted this frame");

  // Rows are gathered into a tightly packed block; the source stride is always the full 2048
  // texel sheet width, never the copied width.
  uint8_t *destination = static_cast<uint8_t *>(staged);
  for (int i = 0; i < height; i++)
  {
    const uint16_t *source = m_textureRam + (static_cast<size_t>(y + i) * kWidth) + x;
    std::memcpy(destination + static_cast<size_t>(i) * rowBytes, source, rowBytes);
  }

  if (m_image.CurrentLayout() == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
  {
    // An earlier copy this frame already left the image in this layout, so no layout change
    // is coming to carry a dependency. The game streams pages within one frame, and two
    // unordered overlapping copies can resolve in either order. Only genuinely overlapping
    // rectangles need serializing; disjoint pages and different mip levels do not, so most
    // copies proceed with no barrier at all (a whole full-image pipeline barrier per copy is
    // expensive on tiled/mobile GPUs, forcing tile-memory and cache flushes).
    bool overlaps = false;
    for (const WrittenRect &r : m_writtenRects)
    {
      if (r.level == level && subX < r.x + r.width && r.x < subX + width &&
          subY < r.y + r.height && r.y < subY + height)
      {
        overlaps = true;
        break;
      }
    }

    if (overlaps)
    {
      m_image.RecordBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT);
      // The barrier orders every prior write, so tracking can restart from this copy.
      m_writtenRects.clear();
    }
  }
  else
  {
    m_image.RecordTransition(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    // A fresh write epoch: earlier writes were ordered by the layout transition.
    m_writtenRects.clear();
  }

  m_writtenRects.push_back({level, subX, subY, width, height});

  VkBufferImageCopy copy{};
  copy.bufferOffset = stagingOffset;
  copy.bufferRowLength = static_cast<uint32_t>(width);
  copy.bufferImageHeight = static_cast<uint32_t>(height);
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.mipLevel = static_cast<uint32_t>(level);
  copy.imageSubresource.baseArrayLayer = 0;
  copy.imageSubresource.layerCount = 1;
  copy.imageOffset = {subX, subY, 0};
  copy.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
  vkCmdCopyBufferToImage(commandBuffer, staging.Handle(), m_image.Handle(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  return true;
}

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
