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

#ifndef INCLUDED_VULKAN_TEXTURE_BANK_H
#define INCLUDED_VULKAN_TEXTURE_BANK_H

#ifdef SUPERMODEL_VULKAN

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "VulkanResources.h"

namespace Vulkan
{

// New3D::TextureBank equivalent: a 2048x1024 sheet of 16-bit texels backed by
// Real3D texture RAM, mip levels packed in via the mipXBase/mipYBase tables.
class TextureBank
{
public:
  static constexpr uint32_t kWidth = 2048;
  static constexpr uint32_t kHeight = 1024;
  // 2048x1024 down to 1x1.
  static constexpr uint32_t kMipLevels = 12;
  static constexpr VkFormat kFormat = VK_FORMAT_R16_UINT;

  TextureBank() = default;
  ~TextureBank() = default;

  TextureBank(const TextureBank &) = delete;
  TextureBank &operator=(const TextureBank &) = delete;

  bool Create(const DeviceContext &deviceContext, std::string *error);
  void Destroy(const DeviceContext &deviceContext);

  void AttachMemory(const uint16_t *textureRam);
  int GetNumberOfLevels() const { return m_numLevels; }

  // x/y are absolute sheet coordinates; source rows use the sheet's 2048 texel
  // stride regardless of the copied width. Leaves the image in
  // TRANSFER_DST_OPTIMAL, so finish the frame with RecordPendingTransition().
  bool RecordUpload(VkCommandBuffer commandBuffer, RingBuffer &staging, int level, int x, int y,
                    int width, int height, std::string *error);

  // True while the image is not in SHADER_READ_ONLY_OPTIMAL: fresh, or just uploaded.
  bool HasPendingTransition() const;
  // Leaves the image sampleable. A no-op when nothing is pending.
  void RecordPendingTransition(VkCommandBuffer commandBuffer);

  VkImageView View() const { return m_image.View(); }
  VkSampler Sampler() const { return m_sampler; }
  VkImage Handle() const { return m_image.Handle(); }
  VkImageLayout CurrentLayout() const { return m_image.CurrentLayout(); }
  bool Valid() const { return m_image.Valid() && m_sampler != VK_NULL_HANDLE; }

private:
  // A copy destination written since the image entered TRANSFER_DST_OPTIMAL, in
  // mip-local coordinates. Only copies whose rectangles overlap need a WAW barrier
  // between them; disjoint pages (and different mip levels) do not.
  struct WrittenRect
  {
    int level;
    int x;
    int y;
    int width;
    int height;
  };

  Image m_image;
  VkSampler m_sampler = VK_NULL_HANDLE;
  const uint16_t *m_textureRam = nullptr;
  int m_numLevels = 0;
  // Reset whenever the image (re)enters TRANSFER_DST_OPTIMAL or a barrier orders
  // everything before it; capacity is retained so the draw path never allocates.
  std::vector<WrittenRect> m_writtenRects;
};

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
#endif // INCLUDED_VULKAN_TEXTURE_BANK_H
