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
 * VulkanRenderTargets.h
 *
 * New3D::R3DFrameBuffers equivalent, plus the offscreen images the 2D, post
 * processing, crosshair and screenshot stages need.
 *
 * Three sizes are tracked separately, coinciding only when RenderScale is off
 * and Supersampling is 1:
 *   attachment  Width()/Height()             New3D's internal render resolution
 *   scene       SceneWidth()/SceneHeight()   supersampled composition image
 *   output      OutputWidth()/OutputHeight() presented, unsupersampled image
 *
 * Unlike GL, where attachment 3 was GLES-only, the LOS attachment always exists:
 * both R3D fragment shaders declare outLos unconditionally.
 */

#ifndef INCLUDED_VULKAN_RENDER_TARGETS_H
#define INCLUDED_VULKAN_RENDER_TARGETS_H

#ifdef SUPERMODEL_VULKAN

#include <array>
#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

#include "VulkanResources.h"
#include "VulkanShaderInterface.h"

namespace Vulkan
{

class SceneTargets
{
public:
  static constexpr VkFormat kColourFormat = VK_FORMAT_R8G8B8A8_UNORM;

  static constexpr VkFormat kLosFormat = kR3DLosFormat;

  static constexpr uint32_t kSceneColourAttachmentCount = 4;

  static constexpr uint32_t kSceneAttachmentCount = kSceneColourAttachmentCount + 1;

  // Colour indices are kR3DAttachmentColour/Trans0/Trans1/Los.
  static constexpr uint32_t kSceneAttachmentDepthStencil = kSceneColourAttachmentCount;

  // Load behaviour of the New3D scene render pass. Only variants with identical
  // attachment references are compatible, so each needs its own pipelines.
  enum class ScenePass : uint32_t
  {
    ClearAll = 0,
    BeginLayer,
    // As above with the LOS attachment unused, for frames where no viewport asks
    // for a line-of-sight sample: eight bytes per pixel kept out of tile memory.
    ClearAllNoLos,
    BeginLayerNoLos,
    // Each translucency pass writes exactly one colour attachment; the other
    // three are VK_ATTACHMENT_UNUSED and stay out of tile memory.
    TransFirst,
    TransSecond,
    Count
  };

  enum class SimplePass : uint32_t
  {
    Clear = 0,
    Load,
    // Contents undefined on entry. For a pass that writes every pixel of the
    // render area; anything left unwritten is undefined rather than preserved.
    Discard,
    Count
  };

  SceneTargets() = default;
  ~SceneTargets();

  SceneTargets(const SceneTargets &) = delete;
  SceneTargets &operator=(const SceneTargets &) = delete;

  // All three sizes must be non-zero. Context::EnsureSceneTargets() is the only
  // caller and completes partial requests.
  bool Create(const DeviceContext &device, uint32_t width, uint32_t height, uint32_t sceneWidth,
              uint32_t sceneHeight, uint32_t outputWidth, uint32_t outputHeight,
              std::string *error);

  // Safe to call at any time, including after a failed Create().
  void Destroy(const DeviceContext &device);

  // Destroy() followed by Create(). The caller must have idled the device.
  bool Resize(const DeviceContext &device, uint32_t width, uint32_t height, uint32_t sceneWidth,
              uint32_t sceneHeight, uint32_t outputWidth, uint32_t outputHeight,
              std::string *error);

  // Set before Create(). When the resolve would copy scene colour to the output
  // unchanged, the two become one image and the resolve disappears. Matching sizes
  // are not enough to tell: only the post processor knows about colour transforms.
  void SetResolveIsIdentity(bool identity) { m_resolveIsIdentity = identity; }
  bool OutputAliasesSceneColour() const { return m_outputAliasesSceneColour; }

  bool IsValid() const { return m_created; }

  uint32_t Width() const { return m_width; }
  uint32_t Height() const { return m_height; }
  uint32_t SceneWidth() const { return m_sceneWidth; }
  uint32_t SceneHeight() const { return m_sceneHeight; }
  uint32_t OutputWidth() const { return m_outputWidth; }
  uint32_t OutputHeight() const { return m_outputHeight; }
  VkExtent2D Extent() const { return VkExtent2D{ m_width, m_height }; }
  VkExtent2D SceneExtent() const { return VkExtent2D{ m_sceneWidth, m_sceneHeight }; }
  VkExtent2D OutputExtent() const { return VkExtent2D{ m_outputWidth, m_outputHeight }; }

  // Negative-height viewports, per VulkanShaderInterface.h's Y convention.
  VkViewport MakeViewport() const;
  VkViewport MakeViewport(float x, float y, float width, float height) const;
  VkViewport MakeSceneViewport() const;
  VkViewport MakeOutputViewport() const;
  VkRect2D MakeScissor() const;
  VkRect2D MakeScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) const;
  VkRect2D MakeSceneScissor() const;
  VkRect2D MakeOutputScissor() const;

  VkFormat ColourFormat() const { return kColourFormat; }
  VkFormat LosFormat() const { return kLosFormat; }
  VkFormat DepthStencilFormat() const { return m_depthStencilFormat; }
  VkFormat SceneColourFormat() const { return kColourFormat; }
  VkFormat OutputFormat() const { return kColourFormat; }

  // Non-const: recording a layout transition mutates the Image's tracked layout.
  Image &ColourImage() { return m_colour; }
  Image &Trans0Image() { return m_trans0; }
  Image &Trans1Image() { return m_trans1; }
  Image &LosImage() { return m_los; }
  Image &DepthStencilImage() { return m_depthStencil; }
  Image &DepthStencilCopyImage() { return m_depthStencilCopy; }
  Image &SceneColourImage() { return m_sceneColour; }
  Image &OutputImage() { return m_outputAliasesSceneColour ? m_sceneColour : m_output; }

  const Image &ColourImage() const { return m_colour; }
  const Image &Trans0Image() const { return m_trans0; }
  const Image &Trans1Image() const { return m_trans1; }
  const Image &LosImage() const { return m_los; }
  const Image &DepthStencilImage() const { return m_depthStencil; }
  const Image &DepthStencilCopyImage() const { return m_depthStencilCopy; }
  const Image &SceneColourImage() const { return m_sceneColour; }
  const Image &OutputImage() const
  {
    return m_outputAliasesSceneColour ? m_sceneColour : m_output;
  }

  // Indexed by kR3DAttachmentColour/Trans0/Trans1. Returns nullptr if out of range.
  Image *ColourAttachment(uint32_t attachment);

  VkImageView ColourView() const;
  VkImageView Trans0View() const;
  VkImageView Trans1View() const;
  VkImageView LosView() const;
  VkImageView DepthStencilView() const;
  VkImageView DepthStencilCopyView() const;
  VkImageView SceneColourView() const;
  VkImageView OutputView() const;

  VkRenderPass ScenePassHandle(ScenePass pass) const;
  VkRenderPass SceneColourPassHandle(SimplePass pass) const;
  VkRenderPass OutputPassHandle(SimplePass pass) const;

  // One per pass variant: a framebuffer only works with a render pass compatible
  // with the one it was built against, and the variants differ in which colour
  // attachments are VK_ATTACHMENT_UNUSED. All share the same attachment views.
  VkFramebuffer SceneFramebuffer(ScenePass pass) const
  {
    const size_t index = static_cast<size_t>(pass);
    return index < m_sceneFramebuffers.size() ? m_sceneFramebuffers[index] : VK_NULL_HANDLE;
  }
  VkFramebuffer SceneColourFramebuffer() const { return m_sceneColourFramebuffer; }
  VkFramebuffer OutputFramebuffer() const { return m_outputFramebuffer; }

  // Each covers the whole of the image it draws into, so the render area follows
  // the attachment, scene and output size respectively.
  void RecordBeginScenePass(VkCommandBuffer commandBuffer, ScenePass pass) const;
  // area restricts the render area, and so what SimplePass::Clear clears, to less
  // than the whole composition image. Null means all of it.
  void RecordBeginSceneColourPass(VkCommandBuffer commandBuffer, SimplePass pass,
                                  const VkClearColorValue &clearColour,
                                  const VkRect2D *area = nullptr) const;
  void RecordBeginOutputPass(VkCommandBuffer commandBuffer, SimplePass pass,
                             const VkClearColorValue &clearColour) const;

  // Must be recorded once after Create()/Resize(), outside a render pass, to
  // move the new images out of VK_IMAGE_LAYOUT_UNDEFINED.
  void RecordInitialTransitions(VkCommandBuffer commandBuffer);

  void RecordSceneAttachmentsForSampling(VkCommandBuffer commandBuffer);
  void RecordSceneAttachmentsForRendering(VkCommandBuffer commandBuffer);

  void RecordSceneColourForSampling(VkCommandBuffer commandBuffer);
  void RecordSceneColourForRendering(VkCommandBuffer commandBuffer);

  // R3DFrameBuffers::StoreDepth/RestoreDepth equivalent. Record outside a render
  // pass. A copy, not a blit: vkCmdBlitImage is illegal for depth/stencil formats.
  void RecordStoreDepth(VkCommandBuffer commandBuffer);
  void RecordRestoreDepth(VkCommandBuffer commandBuffer);

  // R3DFrameBuffers::ReadLos equivalent. Two uint32_t are written at
  // bufferOffset: the depth bit pattern and the no-LOS-return flag.
  static constexpr VkDeviceSize kLosTexelBytes = 2 * sizeof(uint32_t);
  void RecordLosCopyToBuffer(VkCommandBuffer commandBuffer, int32_t x, int32_t y,
                             VkBuffer destination, VkDeviceSize bufferOffset);

  // The buffer must hold at least ScreenshotBufferSize() bytes from bufferOffset
  // and have been created with VK_BUFFER_USAGE_TRANSFER_DST_BIT.
  VkDeviceSize ScreenshotBufferSize() const;
  void RecordOutputCopyToBuffer(VkCommandBuffer commandBuffer, VkBuffer destination,
                                VkDeviceSize bufferOffset);

  // Exposed so a renderer that begins the scene pass itself stays consistent.
  static std::array<VkClearValue, kSceneAttachmentCount> SceneClearValues();

private:
  bool CreateImages(const DeviceContext &device, std::string *error);
  bool CreateRenderPasses(const DeviceContext &device, std::string *error);
  bool CreateFramebuffers(const DeviceContext &device, std::string *error);
  bool ChooseDepthStencilFormat(const DeviceContext &device, std::string *error);
  static void LogDepthPrecisionOptions(const DeviceContext &device);
  bool CheckColourFormats(const DeviceContext &device, std::string *error);

  bool CreateSceneRenderPass(const DeviceContext &device, ScenePass pass, std::string *error);
  bool CreateSimpleRenderPass(const DeviceContext &device, VkFormat format, SimplePass pass,
                              VkRenderPass *out, std::string *error);

  void RecordDepthCopy(VkCommandBuffer commandBuffer, Image &source, Image &destination,
                       VkImageLayout sourceSteady, VkImageLayout destinationSteady);

  static bool SetError(std::string *error, const std::string &message);

  // Kept so the destructor can release everything if Destroy() was not called.
  DeviceContext m_deviceContext{};

  bool m_created = false;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  uint32_t m_sceneWidth = 0;
  uint32_t m_sceneHeight = 0;
  uint32_t m_outputWidth = 0;
  uint32_t m_outputHeight = 0;
  VkFormat m_depthStencilFormat = VK_FORMAT_UNDEFINED;

  Image m_colour;
  Image m_trans0;
  Image m_trans1;
  Image m_los;
  Image m_depthStencil;
  Image m_depthStencilCopy;
  Image m_sceneColour;
  Image m_output;

  std::array<VkRenderPass, static_cast<size_t>(ScenePass::Count)> m_scenePasses{};
  std::array<VkRenderPass, static_cast<size_t>(SimplePass::Count)> m_sceneColourPasses{};
  std::array<VkRenderPass, static_cast<size_t>(SimplePass::Count)> m_outputPasses{};

  bool m_resolveIsIdentity = false;
  bool m_outputAliasesSceneColour = false;

  std::array<VkFramebuffer, static_cast<size_t>(ScenePass::Count)> m_sceneFramebuffers{};
  VkFramebuffer m_sceneColourFramebuffer = VK_NULL_HANDLE;
  VkFramebuffer m_outputFramebuffer = VK_NULL_HANDLE;
};

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
#endif // INCLUDED_VULKAN_RENDER_TARGETS_H
