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

#ifndef INCLUDED_VULKAN_RESOURCES_H
#define INCLUDED_VULKAN_RESOURCES_H

#ifdef SUPERMODEL_VULKAN

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace Vulkan
{

// Thin wrappers over Vulkan objects. None owns a VkDevice: it is passed back in on
// every call that needs it, so these can live in containers.

struct DeviceContext
{
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkQueue graphicsQueue = VK_NULL_HANDLE;
  uint32_t graphicsFamily = 0;
};

// UINT32_MAX when nothing satisfies the required flags. *isCoherent is optional.
uint32_t FindMemoryType(const DeviceContext &deviceContext, uint32_t typeBits,
                        VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                        bool *isCoherent);

class Buffer
{
public:
  Buffer() = default;
  ~Buffer() = default;

  Buffer(const Buffer &) = delete;
  Buffer &operator=(const Buffer &) = delete;
  Buffer(Buffer &&other) noexcept;
  // Swaps contents, so the caller keeps ownership of whatever this object held; it
  // cannot be released without a VkDevice.
  Buffer &operator=(Buffer &&other) noexcept;

  bool Create(const DeviceContext &deviceContext, VkDeviceSize size, VkBufferUsageFlags usage,
              VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, std::string *error);
  void Destroy(const DeviceContext &deviceContext);

  VkBuffer Handle() const { return m_buffer; }
  VkDeviceSize Size() const { return m_size; }
  // Null unless the buffer is host-visible, in which case it stays mapped for life.
  void *MappedPointer() const { return m_mapped; }
  bool IsCoherent() const { return m_coherent; }
  bool Valid() const { return m_buffer != VK_NULL_HANDLE; }

  // No-ops on coherent memory. Otherwise the range widens to nonCoherentAtomSize.
  bool Flush(const DeviceContext &deviceContext, VkDeviceSize offset, VkDeviceSize size,
             std::string *error);
  bool Invalidate(const DeviceContext &deviceContext, VkDeviceSize offset, VkDeviceSize size,
                  std::string *error);

private:
  bool MappedRange(VkDeviceSize offset, VkDeviceSize size, VkMappedMemoryRange *range) const;

  VkBuffer m_buffer = VK_NULL_HANDLE;
  VkDeviceMemory m_memory = VK_NULL_HANDLE;
  void *m_mapped = nullptr;
  VkDeviceSize m_size = 0;
  VkDeviceSize m_allocationSize = 0;
  VkDeviceSize m_atomSize = 1;
  bool m_coherent = false;
};

class Image
{
public:
  Image() = default;
  ~Image() = default;

  Image(const Image &) = delete;
  Image &operator=(const Image &) = delete;
  Image(Image &&other) noexcept;
  Image &operator=(Image &&other) noexcept;

  // A fresh image is tracked as VK_IMAGE_LAYOUT_UNDEFINED.
  bool Create(const DeviceContext &deviceContext, VkFormat format, uint32_t width, uint32_t height,
              uint32_t mipLevels, VkImageUsageFlags usage, VkImageAspectFlags aspect,
              std::string *error);
  void Destroy(const DeviceContext &deviceContext);

  VkImage Handle() const { return m_image; }
  VkImageView View() const { return m_view; }
  VkFormat Format() const { return m_format; }
  VkExtent2D Extent() const { return m_extent; }
  uint32_t MipLevels() const { return m_mipLevels; }
  VkImageAspectFlags Aspect() const { return m_aspect; }
  VkImageLayout CurrentLayout() const { return m_layout; }

  // Orders two accesses that do not change the layout, where RecordTransition
  // emits nothing.
  void RecordBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStage,
                     VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                     VkAccessFlags dstAccess) const;
  bool Valid() const { return m_image != VK_NULL_HANDLE; }

  void RecordTransition(VkCommandBuffer commandBuffer, VkImageLayout newLayout,
                        VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                        VkAccessFlags srcAccess, VkAccessFlags dstAccess);
  // Derives conservative stage/access masks from the old and new layouts.
  void RecordTransition(VkCommandBuffer commandBuffer, VkImageLayout newLayout);

private:
  VkImage m_image = VK_NULL_HANDLE;
  VkDeviceMemory m_memory = VK_NULL_HANDLE;
  VkImageView m_view = VK_NULL_HANDLE;
  VkFormat m_format = VK_FORMAT_UNDEFINED;
  VkExtent2D m_extent{0, 0};
  uint32_t m_mipLevels = 0;
  VkImageAspectFlags m_aspect = 0;
  VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// A persistently mapped host-visible buffer split into `frames` aligned slices, one
// written per frame in flight, keeping CPU writes off ranges the GPU may be reading.
class RingBuffer
{
public:
  RingBuffer() = default;
  ~RingBuffer() = default;

  RingBuffer(const RingBuffer &) = delete;
  RingBuffer &operator=(const RingBuffer &) = delete;
  RingBuffer(RingBuffer &&other) noexcept;
  RingBuffer &operator=(RingBuffer &&other) noexcept;

  bool Create(const DeviceContext &deviceContext, VkDeviceSize bytesPerFrame, uint32_t frames,
              VkBufferUsageFlags usage, std::string *error);
  void Destroy(const DeviceContext &deviceContext);

  // Selects the slice for the given frame in flight and resets its cursor.
  void BeginFrame(uint32_t frameIndex);

  // Returns false and leaves the cursor untouched when the slice cannot satisfy
  // the request. Never allocates.
  bool Suballocate(VkDeviceSize size, VkDeviceSize alignment, void **cpuPointer,
                   VkDeviceSize *bufferOffset);

  VkBuffer Handle() const { return m_buffer.Handle(); }
  VkDeviceSize BytesPerFrame() const { return m_bytesPerFrame; }
  uint32_t Frames() const { return m_frames; }
  uint32_t CurrentFrame() const { return m_currentFrame; }
  // Largest number of bytes handed out from a single slice so far.
  VkDeviceSize HighWaterMark() const { return m_highWaterMark; }
  bool Valid() const { return m_buffer.Valid(); }

  // Flushes only the range written since the last BeginFrame().
  bool FlushFrame(const DeviceContext &deviceContext, std::string *error);
  bool IsCoherent() const { return m_buffer.IsCoherent(); }
  VkDeviceSize BytesPerFrameSlice() const { return m_bytesPerFrame; }

private:
  Buffer m_buffer;
  VkDeviceSize m_bytesPerFrame = 0;
  VkDeviceSize m_cursor = 0;
  VkDeviceSize m_highWaterMark = 0;
  VkDeviceSize m_minAlignment = 1;
  uint32_t m_frames = 0;
  uint32_t m_currentFrame = 0;
};

// For descriptor sets allocated once and then updated in place. Deliberately
// offers no per-set free; use Reset() at a safe point instead.
class DescriptorAllocator
{
public:
  DescriptorAllocator() = default;
  ~DescriptorAllocator() = default;

  DescriptorAllocator(const DescriptorAllocator &) = delete;
  DescriptorAllocator &operator=(const DescriptorAllocator &) = delete;
  DescriptorAllocator(DescriptorAllocator &&other) noexcept;
  DescriptorAllocator &operator=(DescriptorAllocator &&other) noexcept;

  bool Create(const DeviceContext &deviceContext, const std::vector<VkDescriptorPoolSize> &sizes,
              uint32_t maxSets, std::string *error);
  void Destroy(const DeviceContext &deviceContext);
  void Destroy();

  bool Allocate(VkDescriptorSetLayout layout, VkDescriptorSet *set, std::string *error);
  // Invalidates every set handed out so far. Only safe once the GPU is idle.
  bool Reset(std::string *error = nullptr);

  VkDescriptorPool Handle() const { return m_pool; }
  uint32_t MaxSets() const { return m_maxSets; }
  bool Valid() const { return m_pool != VK_NULL_HANDLE; }

private:
  VkDevice m_device = VK_NULL_HANDLE;
  VkDescriptorPool m_pool = VK_NULL_HANDLE;
  uint32_t m_maxSets = 0;
};

// One-shot primary command buffer on a transient pool, for initial uploads and
// layout transitions outside the render loop.
class ScopedCommandBuffer
{
public:
  ScopedCommandBuffer() = default;
  ~ScopedCommandBuffer() = default;

  ScopedCommandBuffer(const ScopedCommandBuffer &) = delete;
  ScopedCommandBuffer &operator=(const ScopedCommandBuffer &) = delete;

  bool Begin(const DeviceContext &deviceContext, std::string *error);
  // Ends recording, submits to the graphics queue and waits for completion.
  // Releases the pool either way.
  bool SubmitAndWait(const DeviceContext &deviceContext, std::string *error);
  void Destroy(const DeviceContext &deviceContext);

  VkCommandBuffer Handle() const { return m_commandBuffer; }
  bool Recording() const { return m_recording; }

private:
  VkCommandPool m_pool = VK_NULL_HANDLE;
  VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
  bool m_recording = false;
};

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
#endif // INCLUDED_VULKAN_RESOURCES_H
