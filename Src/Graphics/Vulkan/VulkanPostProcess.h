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
 * VulkanPostProcess.h
 *
 * SuperAA equivalent: supersampling resolve and CRT colour transform from
 * SceneTargets::SceneColourImage() into SceneTargets::OutputImage(). Of the three
 * sizes in VulkanRenderTargets.h it owns only the output size, passing zeroes for
 * the rest.
 *
 * Deviation from GL: this pass always runs, reducing to a per-pixel copy at
 * supersampling 1 with CRT colours off. Init() must run after VulkanNew3D::Init()
 * and before the 2D and crosshair renderers create their descriptor sets, since a
 * resize rebuilds every image, render pass and pipeline.
 */

#ifndef INCLUDED_VULKAN_POST_PROCESS_H
#define INCLUDED_VULKAN_POST_PROCESS_H

#ifdef SUPERMODEL_VULKAN

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

#include "Graphics/IPostProcessor.h"
#include "Graphics/Vulkan/VulkanContext.h"
#include "Graphics/Vulkan/VulkanPipelines.h"
#include "Graphics/Vulkan/VulkanResources.h"
#include "Supermodel.h"

namespace Vulkan
{

class PostProcess final : public IPostProcessor
{
public:
  PostProcess(Context &context, int supersampling, CRTcolor crtColors);
  ~PostProcess() override;

  PostProcess(const PostProcess &) = delete;
  PostProcess &operator=(const PostProcess &) = delete;

  // Resolution of the presented output. May be called again after a resize.
  Result Init(unsigned outputWidth, unsigned outputHeight);

  void Draw() override;

private:
  bool Prepare(std::string *error);
  // Supersampling clamped to the actual scene-to-output ratio, so the shader
  // cannot fetch outside the scene colour image.
  int EffectiveSampleCount() const;

  static bool SetError(std::string *error, const std::string &message);

  Context &m_context;
  // True when the resolve is an unchanged copy, so a transfer can replace it.
  bool IsIdentityResolve() const;

  const int m_aa;
  const CRTcolor m_crtColors;

  bool m_initialized = false;
  bool m_errorReported = false;

  VkSampler m_sampler = VK_NULL_HANDLE;
  DescriptorAllocator m_descriptors;
  VkDescriptorSet m_set = VK_NULL_HANDLE;

  // Last written state, used to detect a render target resize.
  VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
  VkImageView m_sceneColourView = VK_NULL_HANDLE;
};

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
#endif // INCLUDED_VULKAN_POST_PROCESS_H
