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
 * VulkanNew3D.h
 *
 * New3D::CNew3D equivalent: a thin graphics-API layer over New3D::CNew3DBase,
 * which does all the scene decoding. The line-of-sight readback completes a frame
 * later rather than stalling the queue.
 *
 * One device-local vertex buffer laid out like the GL VBO: ROM vertices from
 * offset 0, RAM vertices from MAX_ROM_VERTS. Vulkan cannot rename a range still
 * being read, so the append-only ROM region is shared while the RAM region is
 * replicated per frame in flight, a mesh's vboOffset selecting that frame's copy.
 */

#ifndef INCLUDED_VULKAN_NEW3D_H
#define INCLUDED_VULKAN_NEW3D_H

#ifdef SUPERMODEL_VULKAN

#include <array>
#include <vector>
#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

#include "Graphics/New3D/Model.h"
#include "Graphics/New3D/New3DBase.h"
#include "Graphics/Vulkan/VulkanComposite.h"
#include "Graphics/Vulkan/VulkanContext.h"
#include "Graphics/Vulkan/VulkanPipelines.h"
#include "Graphics/Vulkan/VulkanResources.h"
#include "Graphics/Vulkan/VulkanShaderInterface.h"
#include "Graphics/Vulkan/VulkanTextureBank.h"
#include "Types.h"
#include "Util/NewConfig.h"

namespace Vulkan
{

class VulkanNew3D final : public New3D::CNew3DBase
{
public:
  VulkanNew3D(Context &context, const Util::Config::Node &config, const std::string &gameName);
  ~VulkanNew3D() override;

  VulkanNew3D(const VulkanNew3D &) = delete;
  VulkanNew3D &operator=(const VulkanNew3D &) = delete;

  // SetStepping, SetSunClamp, SetBlockCulling and GetLosValue come from CNew3DBase.
  Result Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes,
              unsigned totalXRes, unsigned totalYRes, unsigned aaTarget) override;
  void AttachMemory(const uint32_t *cullingRAMLoPtr, const uint32_t *cullingRAMHiPtr,
                    const uint32_t *polyRAMPtr, const uint32_t *vromPtr,
                    const uint16_t *textureRAMPtr) override;
  void UploadTextures(unsigned level, unsigned x, unsigned y, unsigned width,
                      unsigned height) override;
  // CPU-only scene build plus the LOS scan that depends on it. No Vulkan calls, so
  // this is valid before Context::BeginFrame() has waited on the frame's fence.
  void PrepareFrame() override;
  void BeginFrame() override;
  void EndFrame() override;
  void RenderFrame() override;

private:
  // One pending readback per priority layer, per frame in flight; slot = priority.
  struct LosSlot
  {
    bool pending = false;
  };

  void Destroy();

  bool CreateVertexBuffer(std::string *error);
  bool CreateDescriptors(std::string *error);
  // Re-creates the descriptor sets if the pipeline cache owning their layouts was
  // rebuilt, so validity does not depend on renderer initialization order.
  bool EnsureDescriptors(std::string *error);
  bool CheckDeviceLimits(std::string *error) const;

  void UploadVertexData();
  uint32_t FirstVertexFor(const New3D::Mesh &mesh) const;

  bool RenderScene(int priority, bool renderOverlay, New3D::Layer layer);
  void DrawScrollFog();
  void DrawAmbientFog();
  void DrawFogQuad(const New3D::Viewport &viewport, const float rgba[4], float attenuation,
                   float ambient, const float spotFogColor[3], const float spotEllipse[4]);
  bool ProcessLos(int priority);
  void RecordWhiteFlash();

  // Vulkan::Composite call site, placed where CNew3D::RenderFrame calls
  // R3DFrameBuffers::Draw()/DrawScaled() so 3D blends over the 2D bottom layer.
  void RecordFrameBufferComposite();

  void SetSceneViewport(const New3D::Viewport &viewport);
  void FillViewportUniforms(const New3D::Viewport &viewport);
  void ApplyModelState(const New3D::Model &model);
  void FillMeshUniforms(const New3D::Mesh &mesh);
  bool BindMeshUniforms(VkCommandBuffer commandBuffer, VkPipelineLayout layout);
  VkPipeline PipelineForMesh(New3D::Layer layer, const New3D::Mesh &mesh);
  uint32_t DepthEncodeMode() const;
  VkDeviceSize VertexBindOffsetFor(const New3D::Mesh &mesh) const;
  bool UploadRomVertexData(std::string *error);
  void RecordVertexUploadBarrier(VkCommandBuffer commandBuffer);

  void BeginScenePass(SceneTargets::ScenePass pass);
  void EndScenePass();

  static void LosCompletion(void *user, uint64_t token);
  void ResolveLos(uint32_t frameIndex, int priority);

  Context &m_context;
  bool m_ready = false;
  bool m_initialized = false;
  bool m_quadRendering = false;
  Program m_program = Program::R3DTriangle;

  TextureBank m_textureBank[2];

  Composite m_composite;
  bool m_compositeErrorReported = false;

  // Main's glScissor box, in sceneColour coordinates with the origin top left.
  VkRect2D m_displayScissor{};

  // Texture uploads that arrived with no frame recording. CReal3D discards its
  // queue once handed over, so uploads not held here leave those pages stale.
  struct PendingTextureUpload
  {
    unsigned level, x, y, width, height;
  };
  static constexpr size_t kMaxPendingTextureUploads = 512;
  std::vector<PendingTextureUpload> m_pendingTextureUploads;

  void QueueTextureUpload(unsigned level, unsigned x, unsigned y, unsigned width, unsigned height);
  void DrainPendingTextureUploads();

  Buffer m_vertexBuffer;
  VkDeviceSize m_romVertexBytes = 0;
  VkDeviceSize m_romCapacityBytes = 0;
  // Byte offsets of the two regions inside m_vertexBuffer.
  VkDeviceSize m_romBaseBytes = 0;
  VkDeviceSize m_ramBaseBytes = 0;
  bool m_ramOverflowReported = false;
  bool m_uniformOverflowReported = false;
  bool m_pipelineErrorReported = false;
  bool m_descriptorErrorReported = false;

  DescriptorAllocator m_descriptors;
  VkDescriptorSet m_r3dSet = VK_NULL_HANDLE;
  VkDescriptorSet m_scrollFogSet = VK_NULL_HANDLE;
  // Layouts the sets above came from, used to detect a rebuilt pipeline cache.
  VkDescriptorSetLayout m_r3dSetLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_scrollFogSetLayout = VK_NULL_HANDLE;

  // Scratch state, refilled in place so the draw loop never allocates.
  R3DUniforms m_uniforms{};
  R3DUniforms m_lastUniforms{};
  R3DModelPushConstants m_push{};
  VkDeviceSize m_lastUniformOffset = 0;
  bool m_lastUniformValid = false;

  VkPipeline m_boundPipeline = VK_NULL_HANDLE;

  // Whether any viewport asks for a line-of-sight sample this frame. Constant for
  // the frame, and selects the render pass variants that drop the LOS attachment.
  bool m_losActive = false;

  // Per-mesh lookups collapsed to a comparison. All cleared by BeginScenePass.
  VkPipeline m_lastPipeline = VK_NULL_HANDLE;
  New3D::Layer m_lastPipelineLayer = New3D::Layer::colour;
  bool m_lastPipelineLayered = false;
  bool m_lastPipelineNoLos = false;
  VkPipelineLayout m_boundDescriptorLayout = VK_NULL_HANDLE;
  uint32_t m_boundDescriptorOffset = 0;
  bool m_descriptorBound = false;

  // R3DShader's mesh state machine, whose stencil configuration is order
  // dependent. Reset per (priority, overlay) iteration, as Start() does.
  bool m_meshStateDirty = true;
  bool m_shadowLayered = false;
  bool m_shadowNoLosReturn = false;
  bool m_effectiveLayered = false;
  bool m_scenePassActive = false;

  // Per-model texture offsets, folded into baseTexInfo and texturePage.
  int m_transX = 0;
  int m_transY = 0;
  int m_transPage = 0;

  std::array<std::array<LosSlot, Context::kLosSlotsPerFrame>, Context::kFramesInFlight> m_losSlots{};
};

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
#endif // INCLUDED_VULKAN_NEW3D_H
