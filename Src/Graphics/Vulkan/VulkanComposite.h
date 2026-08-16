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

/*
 * VulkanComposite.h
 *
 * R3DFrameBuffers::Draw()/DrawScaled() equivalent: resolves the three New3D
 * scene colour attachments into SceneTargets::SceneColourImage().
 *
 * Source and destination differ in size, SceneTargets::Width()/Height() against
 * SceneWidth()/SceneHeight(). The push-constant source size is the attachment
 * size, which is what the Scaled shaders clamp texelFetch() against.
 */

#ifndef INCLUDED_VULKAN_COMPOSITE_H
#define INCLUDED_VULKAN_COMPOSITE_H

#ifdef SUPERMODEL_VULKAN

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

#include "Graphics/Vulkan/VulkanContext.h"
#include "Graphics/Vulkan/VulkanPipelines.h"
#include "Graphics/Vulkan/VulkanResources.h"

namespace Vulkan
{

class Composite
{
public:
  Composite() = default;
  ~Composite();

  Composite(const Composite &) = delete;
  Composite &operator=(const Composite &) = delete;

  // Safe to call more than once, and on an object that was never used.
  void Destroy(const DeviceContext &device);

  // R3DFrameBuffers::Draw() equivalent. Needs an active frame and no render pass.
  // scissor is Main's display box in sceneColour coordinates, origin top left.
  bool Record(Context &context, const VkRect2D &scissor, std::string *error);

  // R3DFrameBuffers::DrawScaled() equivalent. The rectangle is in OpenGL
  // framebuffer coordinates (origin lower left) within the sceneColour image.
  bool RecordScaled(Context &context, int32_t x, int32_t y, uint32_t width, uint32_t height,
                    const VkRect2D &scissor, std::string *error);

private:
  bool Prepare(Context &context, std::string *error);
  bool RecordInternal(Context &context, bool scaled, const VkViewport &viewport,
                      const VkRect2D &scissor, std::string *error);

  static bool SetError(std::string *error, const std::string &message);

  bool m_created = false;
  VkSampler m_sampler = VK_NULL_HANDLE;
  DescriptorAllocator m_descriptors;

  // The Scaled variants share the unscaled layouts, so two sets cover four programs.
  VkDescriptorSet m_baseSet = VK_NULL_HANDLE;
  VkDescriptorSet m_transSet = VK_NULL_HANDLE;

  // Last written state, used to detect a render target resize.
  VkDescriptorSetLayout m_baseLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_transLayout = VK_NULL_HANDLE;
  VkImageView m_colourView = VK_NULL_HANDLE;
  VkImageView m_trans0View = VK_NULL_HANDLE;
  VkImageView m_trans1View = VK_NULL_HANDLE;
};

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
#endif // INCLUDED_VULKAN_COMPOSITE_H
