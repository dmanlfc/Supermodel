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

#include "VulkanResources.h"

#include <algorithm>
#include <sstream>
#include <utility>

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

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
{
  if (alignment <= 1)
    return value;
  const VkDeviceSize remainder = value % alignment;
  return (remainder == 0) ? value : value + (alignment - remainder);
}

VkDeviceSize AlignDown(VkDeviceSize value, VkDeviceSize alignment)
{
  if (alignment <= 1)
    return value;
  return value - (value % alignment);
}

VkDeviceSize NonCoherentAtomSize(const DeviceContext &deviceContext)
{
  if (deviceContext.physicalDevice == VK_NULL_HANDLE)
    return 1;
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(deviceContext.physicalDevice, &properties);
  return std::max<VkDeviceSize>(1, properties.limits.nonCoherentAtomSize);
}

bool IsHostVisibleMemoryType(const DeviceContext &deviceContext, uint32_t memoryType)
{
  if (deviceContext.physicalDevice == VK_NULL_HANDLE)
    return false;
  VkPhysicalDeviceMemoryProperties properties{};
  vkGetPhysicalDeviceMemoryProperties(deviceContext.physicalDevice, &properties);
  if (memoryType >= properties.memoryTypeCount)
    return false;
  return (properties.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}

void DeriveStageAccess(VkImageLayout layout, bool isSource, VkPipelineStageFlags *stage,
                       VkAccessFlags *access)
{
  switch (layout)
  {
  case VK_IMAGE_LAYOUT_UNDEFINED:
  case VK_IMAGE_LAYOUT_PREINITIALIZED:
    *stage = isSource ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    *access = 0;
    break;
  case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    *access = VK_ACCESS_TRANSFER_READ_BIT;
    break;
  case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    *access = VK_ACCESS_TRANSFER_WRITE_BIT;
    break;
  case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    *stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    *access = VK_ACCESS_SHADER_READ_BIT;
    break;
  case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    *access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    break;
  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    *stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    *access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    break;
  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    *stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    *access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    break;
  case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
    *stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    *access = 0;
    break;
  default:
    *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    *access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    break;
  }
}

} // anonymous namespace

uint32_t FindMemoryType(const DeviceContext &deviceContext, uint32_t typeBits,
                        VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                        bool *isCoherent)
{
  if (isCoherent != nullptr)
    *isCoherent = false;
  if (deviceContext.physicalDevice == VK_NULL_HANDLE)
    return UINT32_MAX;

  VkPhysicalDeviceMemoryProperties properties{};
  vkGetPhysicalDeviceMemoryProperties(deviceContext.physicalDevice, &properties);

  uint32_t fallback = UINT32_MAX;
  for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
  {
    if ((typeBits & (1u << i)) == 0 ||
        (properties.memoryTypes[i].propertyFlags & required) != required)
      continue;
    if ((properties.memoryTypes[i].propertyFlags & preferred) == preferred)
    {
      if (isCoherent != nullptr)
        *isCoherent = (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      return i;
    }
    if (fallback == UINT32_MAX)
      fallback = i;
  }

  if (fallback != UINT32_MAX && isCoherent != nullptr)
    *isCoherent = (properties.memoryTypes[fallback].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  return fallback;
}

// ---------------------------------------------------------------------------
// Buffer
// ---------------------------------------------------------------------------

Buffer::Buffer(Buffer &&other) noexcept
  : m_buffer(other.m_buffer),
    m_memory(other.m_memory),
    m_mapped(other.m_mapped),
    m_size(other.m_size),
    m_allocationSize(other.m_allocationSize),
    m_atomSize(other.m_atomSize),
    m_coherent(other.m_coherent)
{
  other.m_buffer = VK_NULL_HANDLE;
  other.m_memory = VK_NULL_HANDLE;
  other.m_mapped = nullptr;
  other.m_size = 0;
  other.m_allocationSize = 0;
  other.m_atomSize = 1;
  other.m_coherent = false;
}

Buffer &Buffer::operator=(Buffer &&other) noexcept
{
  if (this != &other)
  {
    std::swap(m_buffer, other.m_buffer);
    std::swap(m_memory, other.m_memory);
    std::swap(m_mapped, other.m_mapped);
    std::swap(m_size, other.m_size);
    std::swap(m_allocationSize, other.m_allocationSize);
    std::swap(m_atomSize, other.m_atomSize);
    std::swap(m_coherent, other.m_coherent);
  }
  return *this;
}

bool Buffer::Create(const DeviceContext &deviceContext, VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                    std::string *error)
{
  if (deviceContext.device == VK_NULL_HANDLE || deviceContext.physicalDevice == VK_NULL_HANDLE)
    return SetError(error, "Vulkan buffer creation requires an initialized device");
  if (m_buffer != VK_NULL_HANDLE)
    return SetError(error, "Vulkan buffer created twice without an intervening Destroy()");
  if (size == 0)
    return SetError(error, "Vulkan buffer creation requires a non-zero size");

  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult result = vkCreateBuffer(deviceContext.device, &bufferInfo, nullptr, &m_buffer);
  if (result != VK_SUCCESS)
  {
    m_buffer = VK_NULL_HANDLE;
    return SetError(error, VkError("vkCreateBuffer", result));
  }

  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(deviceContext.device, m_buffer, &requirements);

  bool coherent = false;
  const uint32_t memoryType = FindMemoryType(deviceContext, requirements.memoryTypeBits, required,
                                             preferred, &coherent);
  if (memoryType == UINT32_MAX)
  {
    Destroy(deviceContext);
    return SetError(error, "No Vulkan memory type satisfies the requested buffer properties");
  }

  VkMemoryAllocateInfo allocation{};
  allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memoryType;
  result = vkAllocateMemory(deviceContext.device, &allocation, nullptr, &m_memory);
  if (result != VK_SUCCESS)
  {
    m_memory = VK_NULL_HANDLE;
    Destroy(deviceContext);
    return SetError(error, VkError("vkAllocateMemory", result));
  }

  result = vkBindBufferMemory(deviceContext.device, m_buffer, m_memory, 0);
  if (result != VK_SUCCESS)
  {
    Destroy(deviceContext);
    return SetError(error, VkError("vkBindBufferMemory", result));
  }

  m_size = size;
  m_allocationSize = requirements.size;
  m_coherent = coherent;
  m_atomSize = coherent ? 1 : NonCoherentAtomSize(deviceContext);

  if (IsHostVisibleMemoryType(deviceContext, memoryType))
  {
    result = vkMapMemory(deviceContext.device, m_memory, 0, m_allocationSize, 0, &m_mapped);
    if (result != VK_SUCCESS)
    {
      m_mapped = nullptr;
      Destroy(deviceContext);
      return SetError(error, VkError("vkMapMemory", result));
    }
  }

  return true;
}

void Buffer::Destroy(const DeviceContext &deviceContext)
{
  if (deviceContext.device != VK_NULL_HANDLE)
  {
    if (m_mapped != nullptr && m_memory != VK_NULL_HANDLE)
      vkUnmapMemory(deviceContext.device, m_memory);
    if (m_buffer != VK_NULL_HANDLE)
      vkDestroyBuffer(deviceContext.device, m_buffer, nullptr);
    if (m_memory != VK_NULL_HANDLE)
      vkFreeMemory(deviceContext.device, m_memory, nullptr);
  }

  m_buffer = VK_NULL_HANDLE;
  m_memory = VK_NULL_HANDLE;
  m_mapped = nullptr;
  m_size = 0;
  m_allocationSize = 0;
  m_atomSize = 1;
  m_coherent = false;
}

bool Buffer::MappedRange(VkDeviceSize offset, VkDeviceSize size, VkMappedMemoryRange *range) const
{
  if (offset >= m_size)
    return false;
  if (size == VK_WHOLE_SIZE || size > m_size - offset)
    size = m_size - offset;
  if (size == 0)
    return false;

  const VkDeviceSize alignedOffset = AlignDown(offset, m_atomSize);
  VkDeviceSize alignedEnd = AlignUp(offset + size, m_atomSize);
  if (alignedEnd > m_allocationSize)
    alignedEnd = m_allocationSize;

  range->sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range->pNext = nullptr;
  range->memory = m_memory;
  range->offset = alignedOffset;
  // Vulkan requires either an atom-aligned size or a range that reaches the end
  // of the allocation; VK_WHOLE_SIZE covers the latter without extra rounding.
  range->size = (alignedEnd >= m_allocationSize) ? VK_WHOLE_SIZE : alignedEnd - alignedOffset;
  return true;
}

bool Buffer::Flush(const DeviceContext &deviceContext, VkDeviceSize offset, VkDeviceSize size,
                   std::string *error)
{
  if (m_memory == VK_NULL_HANDLE)
    return SetError(error, "Flush() called on an uncreated Vulkan buffer");
  if (m_coherent || m_mapped == nullptr)
    return true;

  VkMappedMemoryRange range{};
  if (!MappedRange(offset, size, &range))
    return true;

  const VkResult result = vkFlushMappedMemoryRanges(deviceContext.device, 1, &range);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkFlushMappedMemoryRanges", result));
  return true;
}

bool Buffer::Invalidate(const DeviceContext &deviceContext, VkDeviceSize offset, VkDeviceSize size,
                        std::string *error)
{
  if (m_memory == VK_NULL_HANDLE)
    return SetError(error, "Invalidate() called on an uncreated Vulkan buffer");
  if (m_coherent || m_mapped == nullptr)
    return true;

  VkMappedMemoryRange range{};
  if (!MappedRange(offset, size, &range))
    return true;

  const VkResult result = vkInvalidateMappedMemoryRanges(deviceContext.device, 1, &range);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkInvalidateMappedMemoryRanges", result));
  return true;
}

// ---------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------

Image::Image(Image &&other) noexcept
  : m_image(other.m_image),
    m_memory(other.m_memory),
    m_view(other.m_view),
    m_format(other.m_format),
    m_extent(other.m_extent),
    m_mipLevels(other.m_mipLevels),
    m_aspect(other.m_aspect),
    m_layout(other.m_layout)
{
  other.m_image = VK_NULL_HANDLE;
  other.m_memory = VK_NULL_HANDLE;
  other.m_view = VK_NULL_HANDLE;
  other.m_format = VK_FORMAT_UNDEFINED;
  other.m_extent = VkExtent2D{0, 0};
  other.m_mipLevels = 0;
  other.m_aspect = 0;
  other.m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

Image &Image::operator=(Image &&other) noexcept
{
  if (this != &other)
  {
    std::swap(m_image, other.m_image);
    std::swap(m_memory, other.m_memory);
    std::swap(m_view, other.m_view);
    std::swap(m_format, other.m_format);
    std::swap(m_extent, other.m_extent);
    std::swap(m_mipLevels, other.m_mipLevels);
    std::swap(m_aspect, other.m_aspect);
    std::swap(m_layout, other.m_layout);
  }
  return *this;
}

bool Image::Create(const DeviceContext &deviceContext, VkFormat format, uint32_t width,
                   uint32_t height, uint32_t mipLevels, VkImageUsageFlags usage,
                   VkImageAspectFlags aspect, std::string *error)
{
  if (deviceContext.device == VK_NULL_HANDLE || deviceContext.physicalDevice == VK_NULL_HANDLE)
    return SetError(error, "Vulkan image creation requires an initialized device");
  if (m_image != VK_NULL_HANDLE)
    return SetError(error, "Vulkan image created twice without an intervening Destroy()");
  if (width == 0 || height == 0)
    return SetError(error, "Vulkan image creation requires a non-zero extent");
  if (mipLevels == 0)
    mipLevels = 1;

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = format;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = mipLevels;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = usage;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkResult result = vkCreateImage(deviceContext.device, &imageInfo, nullptr, &m_image);
  if (result != VK_SUCCESS)
  {
    m_image = VK_NULL_HANDLE;
    return SetError(error, VkError("vkCreateImage", result));
  }

  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(deviceContext.device, m_image, &requirements);

  const uint32_t memoryType = FindMemoryType(deviceContext, requirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, nullptr);
  if (memoryType == UINT32_MAX)
  {
    Destroy(deviceContext);
    return SetError(error, "No device-local Vulkan memory type is available for this image");
  }

  VkMemoryAllocateInfo allocation{};
  allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memoryType;
  result = vkAllocateMemory(deviceContext.device, &allocation, nullptr, &m_memory);
  if (result != VK_SUCCESS)
  {
    m_memory = VK_NULL_HANDLE;
    Destroy(deviceContext);
    return SetError(error, VkError("vkAllocateMemory(image)", result));
  }

  result = vkBindImageMemory(deviceContext.device, m_image, m_memory, 0);
  if (result != VK_SUCCESS)
  {
    Destroy(deviceContext);
    return SetError(error, VkError("vkBindImageMemory", result));
  }

  // VUID-VkImageViewCreateInfo-image-04441: a view needs a usage it can serve, so a
  // transfer-only image such as the depth copy the layered passes stash must not get one.
  const VkImageUsageFlags viewableUsage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  if ((usage & viewableUsage) != 0)
  {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    result = vkCreateImageView(deviceContext.device, &viewInfo, nullptr, &m_view);
    if (result != VK_SUCCESS)
    {
      m_view = VK_NULL_HANDLE;
      Destroy(deviceContext);
      return SetError(error, VkError("vkCreateImageView", result));
    }
  }

  m_format = format;
  m_extent = VkExtent2D{width, height};
  m_mipLevels = mipLevels;
  m_aspect = aspect;
  m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  return true;
}

void Image::Destroy(const DeviceContext &deviceContext)
{
  if (deviceContext.device != VK_NULL_HANDLE)
  {
    if (m_view != VK_NULL_HANDLE)
      vkDestroyImageView(deviceContext.device, m_view, nullptr);
    if (m_image != VK_NULL_HANDLE)
      vkDestroyImage(deviceContext.device, m_image, nullptr);
    if (m_memory != VK_NULL_HANDLE)
      vkFreeMemory(deviceContext.device, m_memory, nullptr);
  }

  m_image = VK_NULL_HANDLE;
  m_memory = VK_NULL_HANDLE;
  m_view = VK_NULL_HANDLE;
  m_format = VK_FORMAT_UNDEFINED;
  m_extent = VkExtent2D{0, 0};
  m_mipLevels = 0;
  m_aspect = 0;
  m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void Image::RecordTransition(VkCommandBuffer commandBuffer, VkImageLayout newLayout,
                             VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                             VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
  if (commandBuffer == VK_NULL_HANDLE || m_image == VK_NULL_HANDLE || newLayout == m_layout)
    return;

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = srcAccess;
  barrier.dstAccessMask = dstAccess;
  barrier.oldLayout = m_layout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = m_image;
  barrier.subresourceRange.aspectMask = m_aspect;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = m_mipLevels;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  m_layout = newLayout;
}

void Image::RecordBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStage,
                          VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                          VkAccessFlags dstAccess) const
{
  if (commandBuffer == VK_NULL_HANDLE || m_image == VK_NULL_HANDLE)
    return;

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = srcAccess;
  barrier.dstAccessMask = dstAccess;
  barrier.oldLayout = m_layout;
  barrier.newLayout = m_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = m_image;
  barrier.subresourceRange.aspectMask = m_aspect;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = m_mipLevels;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void Image::RecordTransition(VkCommandBuffer commandBuffer, VkImageLayout newLayout)
{
  VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkAccessFlags srcAccess = 0;
  VkAccessFlags dstAccess = 0;
  DeriveStageAccess(m_layout, true, &srcStage, &srcAccess);
  DeriveStageAccess(newLayout, false, &dstStage, &dstAccess);
  RecordTransition(commandBuffer, newLayout, srcStage, dstStage, srcAccess, dstAccess);
}

// ---------------------------------------------------------------------------
// RingBuffer
// ---------------------------------------------------------------------------

RingBuffer::RingBuffer(RingBuffer &&other) noexcept
  : m_buffer(std::move(other.m_buffer)),
    m_bytesPerFrame(other.m_bytesPerFrame),
    m_cursor(other.m_cursor),
    m_highWaterMark(other.m_highWaterMark),
    m_minAlignment(other.m_minAlignment),
    m_frames(other.m_frames),
    m_currentFrame(other.m_currentFrame)
{
  other.m_bytesPerFrame = 0;
  other.m_cursor = 0;
  other.m_highWaterMark = 0;
  other.m_minAlignment = 1;
  other.m_frames = 0;
  other.m_currentFrame = 0;
}

RingBuffer &RingBuffer::operator=(RingBuffer &&other) noexcept
{
  if (this != &other)
  {
    m_buffer = std::move(other.m_buffer);
    std::swap(m_bytesPerFrame, other.m_bytesPerFrame);
    std::swap(m_cursor, other.m_cursor);
    std::swap(m_highWaterMark, other.m_highWaterMark);
    std::swap(m_minAlignment, other.m_minAlignment);
    std::swap(m_frames, other.m_frames);
    std::swap(m_currentFrame, other.m_currentFrame);
  }
  return *this;
}

bool RingBuffer::Create(const DeviceContext &deviceContext, VkDeviceSize bytesPerFrame,
                        uint32_t frames, VkBufferUsageFlags usage, std::string *error)
{
  if (deviceContext.device == VK_NULL_HANDLE || deviceContext.physicalDevice == VK_NULL_HANDLE)
    return SetError(error, "Vulkan ring buffer creation requires an initialized device");
  if (m_buffer.Valid())
    return SetError(error, "Vulkan ring buffer created twice without an intervening Destroy()");
  if (bytesPerFrame == 0 || frames == 0)
    return SetError(error, "Vulkan ring buffer requires a non-zero size and frame count");

  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(deviceContext.physicalDevice, &properties);

  // Slice bases must satisfy every alignment a descriptor bound into them could
  // require, and must be atom aligned so per-slice flushes never overlap.
  VkDeviceSize alignment = std::max<VkDeviceSize>(16, properties.limits.nonCoherentAtomSize);
  if ((usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0)
    alignment = std::max<VkDeviceSize>(alignment, properties.limits.minUniformBufferOffsetAlignment);
  if ((usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0)
    alignment = std::max<VkDeviceSize>(alignment, properties.limits.minStorageBufferOffsetAlignment);
  if ((usage & (VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) != 0)
    alignment = std::max<VkDeviceSize>(alignment, properties.limits.minTexelBufferOffsetAlignment);

  const VkDeviceSize sliceSize = AlignUp(bytesPerFrame, alignment);
  if (sliceSize == 0 || sliceSize > (~VkDeviceSize(0)) / frames)
    return SetError(error, "Vulkan ring buffer size overflows the addressable range");

  if (!m_buffer.Create(deviceContext, sliceSize * frames, usage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       error))
    return false;

  if (m_buffer.MappedPointer() == nullptr)
  {
    m_buffer.Destroy(deviceContext);
    return SetError(error, "Vulkan ring buffer memory could not be mapped");
  }

  m_bytesPerFrame = sliceSize;
  m_frames = frames;
  m_minAlignment = alignment;
  m_cursor = 0;
  m_currentFrame = 0;
  m_highWaterMark = 0;
  return true;
}

void RingBuffer::Destroy(const DeviceContext &deviceContext)
{
  m_buffer.Destroy(deviceContext);
  m_bytesPerFrame = 0;
  m_cursor = 0;
  m_highWaterMark = 0;
  m_minAlignment = 1;
  m_frames = 0;
  m_currentFrame = 0;
}

void RingBuffer::BeginFrame(uint32_t frameIndex)
{
  if (m_frames == 0)
    return;
  m_currentFrame = frameIndex % m_frames;
  m_cursor = 0;
}

bool RingBuffer::Suballocate(VkDeviceSize size, VkDeviceSize alignment, void **cpuPointer,
                             VkDeviceSize *bufferOffset)
{
  uint8_t *const mapped = static_cast<uint8_t *>(m_buffer.MappedPointer());
  if (mapped == nullptr || m_frames == 0 || cpuPointer == nullptr || bufferOffset == nullptr)
    return false;
  if (size > m_bytesPerFrame)
    return false;

  // Align the absolute buffer offset: slice bases only guarantee m_minAlignment,
  // so a caller asking for more must be honoured relative to the buffer start.
  const VkDeviceSize sliceBase = m_bytesPerFrame * m_currentFrame;
  const VkDeviceSize absolute = AlignUp(sliceBase + m_cursor, std::max<VkDeviceSize>(1, alignment));
  const VkDeviceSize offsetInSlice = absolute - sliceBase;
  if (offsetInSlice > m_bytesPerFrame || size > m_bytesPerFrame - offsetInSlice)
    return false;

  *bufferOffset = absolute;
  *cpuPointer = mapped + *bufferOffset;
  m_cursor = offsetInSlice + size;
  if (m_cursor > m_highWaterMark)
    m_highWaterMark = m_cursor;
  return true;
}

bool RingBuffer::FlushFrame(const DeviceContext &deviceContext, std::string *error)
{
  if (!m_buffer.Valid())
    return SetError(error, "FlushFrame() called on an uncreated Vulkan ring buffer");
  if (m_cursor == 0)
    return true;
  return m_buffer.Flush(deviceContext, m_bytesPerFrame * m_currentFrame, m_cursor, error);
}

// ---------------------------------------------------------------------------
// DescriptorAllocator
// ---------------------------------------------------------------------------

DescriptorAllocator::DescriptorAllocator(DescriptorAllocator &&other) noexcept
  : m_device(other.m_device), m_pool(other.m_pool), m_maxSets(other.m_maxSets)
{
  other.m_device = VK_NULL_HANDLE;
  other.m_pool = VK_NULL_HANDLE;
  other.m_maxSets = 0;
}

DescriptorAllocator &DescriptorAllocator::operator=(DescriptorAllocator &&other) noexcept
{
  if (this != &other)
  {
    std::swap(m_device, other.m_device);
    std::swap(m_pool, other.m_pool);
    std::swap(m_maxSets, other.m_maxSets);
  }
  return *this;
}

bool DescriptorAllocator::Create(const DeviceContext &deviceContext,
                                 const std::vector<VkDescriptorPoolSize> &sizes, uint32_t maxSets,
                                 std::string *error)
{
  if (deviceContext.device == VK_NULL_HANDLE)
    return SetError(error, "Vulkan descriptor pool creation requires an initialized device");
  if (m_pool != VK_NULL_HANDLE)
    return SetError(error, "Vulkan descriptor pool created twice without an intervening Destroy()");
  if (sizes.empty() || maxSets == 0)
    return SetError(error, "Vulkan descriptor pool requires at least one pool size and one set");

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = maxSets;
  poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
  poolInfo.pPoolSizes = sizes.data();

  const VkResult result = vkCreateDescriptorPool(deviceContext.device, &poolInfo, nullptr, &m_pool);
  if (result != VK_SUCCESS)
  {
    m_pool = VK_NULL_HANDLE;
    return SetError(error, VkError("vkCreateDescriptorPool", result));
  }

  m_device = deviceContext.device;
  m_maxSets = maxSets;
  return true;
}

void DescriptorAllocator::Destroy(const DeviceContext &deviceContext)
{
  VkDevice device = (m_device != VK_NULL_HANDLE) ? m_device : deviceContext.device;
  if (m_pool != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(device, m_pool, nullptr);
  m_device = VK_NULL_HANDLE;
  m_pool = VK_NULL_HANDLE;
  m_maxSets = 0;
}

void DescriptorAllocator::Destroy()
{
  if (m_pool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(m_device, m_pool, nullptr);
  m_device = VK_NULL_HANDLE;
  m_pool = VK_NULL_HANDLE;
  m_maxSets = 0;
}

bool DescriptorAllocator::Allocate(VkDescriptorSetLayout layout, VkDescriptorSet *set,
                                   std::string *error)
{
  if (set == nullptr)
    return SetError(error, "Vulkan descriptor set allocation received no output pointer");
  *set = VK_NULL_HANDLE;
  if (m_pool == VK_NULL_HANDLE || m_device == VK_NULL_HANDLE)
    return SetError(error, "Allocate() called on an uncreated Vulkan descriptor pool");
  if (layout == VK_NULL_HANDLE)
    return SetError(error, "Vulkan descriptor set allocation received no layout");

  VkDescriptorSetAllocateInfo allocateInfo{};
  allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocateInfo.descriptorPool = m_pool;
  allocateInfo.descriptorSetCount = 1;
  allocateInfo.pSetLayouts = &layout;

  const VkResult result = vkAllocateDescriptorSets(m_device, &allocateInfo, set);
  if (result != VK_SUCCESS)
  {
    *set = VK_NULL_HANDLE;
    return SetError(error, VkError("vkAllocateDescriptorSets", result));
  }
  return true;
}

bool DescriptorAllocator::Reset(std::string *error)
{
  if (m_pool == VK_NULL_HANDLE || m_device == VK_NULL_HANDLE)
    return SetError(error, "Reset() called on an uncreated Vulkan descriptor pool");
  const VkResult result = vkResetDescriptorPool(m_device, m_pool, 0);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkResetDescriptorPool", result));
  return true;
}

// ---------------------------------------------------------------------------
// ScopedCommandBuffer
// ---------------------------------------------------------------------------

bool ScopedCommandBuffer::Begin(const DeviceContext &deviceContext, std::string *error)
{
  if (deviceContext.device == VK_NULL_HANDLE)
    return SetError(error, "One-shot command buffer creation requires an initialized device");
  if (m_pool != VK_NULL_HANDLE)
    return SetError(error, "One-shot command buffer is already recording");

  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolInfo.queueFamilyIndex = deviceContext.graphicsFamily;
  VkResult result = vkCreateCommandPool(deviceContext.device, &poolInfo, nullptr, &m_pool);
  if (result != VK_SUCCESS)
  {
    m_pool = VK_NULL_HANDLE;
    return SetError(error, VkError("vkCreateCommandPool(one-shot)", result));
  }

  VkCommandBufferAllocateInfo allocateInfo{};
  allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocateInfo.commandPool = m_pool;
  allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocateInfo.commandBufferCount = 1;
  result = vkAllocateCommandBuffers(deviceContext.device, &allocateInfo, &m_commandBuffer);
  if (result != VK_SUCCESS)
  {
    m_commandBuffer = VK_NULL_HANDLE;
    Destroy(deviceContext);
    return SetError(error, VkError("vkAllocateCommandBuffers(one-shot)", result));
  }

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = vkBeginCommandBuffer(m_commandBuffer, &beginInfo);
  if (result != VK_SUCCESS)
  {
    Destroy(deviceContext);
    return SetError(error, VkError("vkBeginCommandBuffer(one-shot)", result));
  }

  m_recording = true;
  return true;
}

bool ScopedCommandBuffer::SubmitAndWait(const DeviceContext &deviceContext, std::string *error)
{
  if (!m_recording || m_commandBuffer == VK_NULL_HANDLE)
  {
    Destroy(deviceContext);
    return SetError(error, "SubmitAndWait() called without an active one-shot command buffer");
  }
  if (deviceContext.graphicsQueue == VK_NULL_HANDLE)
  {
    Destroy(deviceContext);
    return SetError(error, "One-shot command buffer submission requires a graphics queue");
  }

  VkResult result = vkEndCommandBuffer(m_commandBuffer);
  if (result != VK_SUCCESS)
  {
    Destroy(deviceContext);
    return SetError(error, VkError("vkEndCommandBuffer(one-shot)", result));
  }
  m_recording = false;

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &m_commandBuffer;
  result = vkQueueSubmit(deviceContext.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
  if (result != VK_SUCCESS)
  {
    Destroy(deviceContext);
    return SetError(error, VkError("vkQueueSubmit(one-shot)", result));
  }

  result = vkQueueWaitIdle(deviceContext.graphicsQueue);
  Destroy(deviceContext);
  if (result != VK_SUCCESS)
    return SetError(error, VkError("vkQueueWaitIdle(one-shot)", result));
  return true;
}

void ScopedCommandBuffer::Destroy(const DeviceContext &deviceContext)
{
  if (deviceContext.device != VK_NULL_HANDLE && m_pool != VK_NULL_HANDLE)
  {
    // Freeing the pool releases the command buffer with it.
    vkDestroyCommandPool(deviceContext.device, m_pool, nullptr);
  }
  m_pool = VK_NULL_HANDLE;
  m_commandBuffer = VK_NULL_HANDLE;
  m_recording = false;
}

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
