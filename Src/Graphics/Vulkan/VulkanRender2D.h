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
 * VulkanRender2D.h
 *
 * CRender2D equivalent, drawing the two TileGen surfaces with the Tilegen
 * pipelines rather than compositing on the CPU. GL's `#define UPSCALEMODE n`
 * recompile becomes a choice between Program::TilegenUpscale0..3.
 *
 * Every extent derives from SceneTargets::SceneWidth()/SceneHeight(), never
 * Width()/Height(), New3D's smaller internal resolution under RenderScale. A
 * surface is re-uploaded only when TileGen hands over a different buffer, since it
 * double buffers and repeats the pointer when a frame produced no new 2D output.
 */

#ifndef INCLUDED_VULKAN_RENDER2D_H
#define INCLUDED_VULKAN_RENDER2D_H

#ifdef SUPERMODEL_VULKAN

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "Graphics/IRender2D.h"
#include "Graphics/Vulkan/VulkanContext.h"
#include "Graphics/Vulkan/VulkanPipelines.h"
#include "Graphics/Vulkan/VulkanResources.h"
#include "Supermodel.h"
#include "Util/NewConfig.h"

struct TileGenBuffer;

namespace Vulkan
{

class Render2D final : public IRender2D
{
public:
  // A TileGen surface is 512 pixels wide, of which the leftmost 496 are visible.
  static constexpr uint32_t kSurfaceWidth = 496;
  static constexpr uint32_t kSurfaceHeight = 384;
  static constexpr uint32_t kSurfaceStride = 512;

  Render2D(Context &context, const Util::Config::Node &config);
  ~Render2D() override;

  Render2D(const Render2D &) = delete;
  Render2D &operator=(const Render2D &) = delete;

  // May be called again after a resolution change.
  Result Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes,
              unsigned totalXRes, unsigned totalYRes, UpscaleMode upscaleMode);

  void BeginFrame() override;
  void PreRenderFrame() override;
  void RenderFrameBottom() override;
  void RenderFrameTop() override;
  void EndFrame() override;
  void WriteVRAM(unsigned addr, uint32_t data) override;
  void AttachRegisters(const uint32_t *regPtr) override;
  void AttachPalette(const uint32_t *palPtr[2]) override;
  void AttachVRAM(const uint8_t *vramPtr) override;
  void AttachDrawBuffers(std::shared_ptr<TileGenBuffer> bottom,
                         std::shared_ptr<TileGenBuffer> top) override;

private:
  static constexpr uint32_t kSurfaceCount = 2;

  void Destroy();
  bool CreateSurfaces(std::string *error);
  bool CreateDescriptors(std::string *error);
  bool Upload(uint32_t index, std::string *error);
  void DrawSurface(uint32_t index, bool isBottom);
  // CRender2D::Setup2D's viewport, in Vulkan's upper-left origin convention.
  VkViewport SurfaceViewport(bool isBottom) const;
  VkRect2D ContentScissor() const;

  static bool SetError2D(std::string *error, const std::string &message);

  const Util::Config::Node &m_config;
  Context &m_context;

  // In sceneColour pixels: Main scales these by Supersampling before Init().
  unsigned m_xPixels = kSurfaceWidth;
  unsigned m_yPixels = kSurfaceHeight;
  unsigned m_xOffset = 0;
  unsigned m_yOffset = 0;
  unsigned m_totalXPixels = 0;
  unsigned m_totalYPixels = 0;
  unsigned m_correction = 0;
  UpscaleMode m_upscaleMode = UpscaleMode::Biquintic;

  bool m_initialized = false;
  bool m_uploadErrorReported = false;
  bool m_pipelineErrorReported = false;

  VkSampler m_sampler = VK_NULL_HANDLE;
  DescriptorAllocator m_descriptors;
  std::array<Image, kSurfaceCount> m_surfaces;
  std::array<VkDescriptorSet, kSurfaceCount> m_sets{};
  VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;

  std::array<std::shared_ptr<TileGenBuffer>, kSurfaceCount> m_drawBuffers;
  // Last buffer whose contents reached the device, per surface.
  std::array<const TileGenBuffer *, kSurfaceCount> m_uploaded{};
};

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
#endif // INCLUDED_VULKAN_RENDER2D_H
