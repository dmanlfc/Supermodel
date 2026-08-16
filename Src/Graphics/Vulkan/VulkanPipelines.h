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
 * VulkanPipelines.h
 *
 * Descriptor set layouts, pipeline layouts and a keyed VkPipeline cache for
 * every shader in VulkanShaderInterface.h.
 *
 * PipelineKey holds the attachment formats as well as the render pass handle, so
 * a pipeline built against a pass Resize() has recreated cannot match; its
 * uint8_t fields cover only the basic VkBlendOp values. Viewport and scissor are
 * dynamic everywhere, so a resolution change never invalidates the cache.
 *
 * Per-attachment colour write masks require independentBlend, without which the
 * New3D layer scheme is inexpressible and Get() fails.
 */

#ifndef INCLUDED_VULKAN_PIPELINES_H
#define INCLUDED_VULKAN_PIPELINES_H

#ifdef SUPERMODEL_VULKAN

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <vulkan/vulkan.h>

#include "VulkanResources.h"
#include "VulkanShaderInterface.h"

#include "Graphics/New3D/Model.h"

namespace Vulkan
{

// Derived here so this header need not depend on VulkanRenderTargets.h.
inline constexpr uint32_t kScenePassColourAttachmentCount = kR3DAttachmentLos + 1;

// One entry per SPIR-V module embedded in obj/VulkanShaderSPIRV.h.
enum class ShaderModuleId : uint32_t
{
  R3DTriangleVert = 0,
  R3DTriangleFrag,
  R3DQuadVert,
  R3DQuadGeom,
  R3DQuadFrag,
  FullScreenQuadVert,
  FullScreenQuadUVVert,
  R3DFrameBufferBaseFrag,
  R3DFrameBufferTransFrag,
  R3DFrameBufferBaseScaledFrag,
  R3DFrameBufferTransScaledFrag,
  ScrollFogFrag,
  PostProcessFrag,
  TilegenVert,
  TilegenUpscale0Frag,
  TilegenUpscale1Frag,
  TilegenUpscale2Frag,
  TilegenUpscale3Frag,
  CrosshairVert,
  CrosshairFrag,
  Count
};

// A fixed set of stages plus one vertex input layout.
enum class Program : uint16_t
{
  R3DTriangle = 0,
  R3DQuad,
  ScrollFog,
  FrameBufferBase,
  FrameBufferBaseScaled,
  FrameBufferTrans,
  FrameBufferTransScaled,
  TilegenUpscale0,
  TilegenUpscale1,
  TilegenUpscale2,
  TilegenUpscale3,
  PostProcess,
  Crosshair,
  Count
};

// Tilegen variant for a ::UpscaleMode value.
inline Program TilegenProgram(int upscaleMode)
{
  const int mode = (upscaleMode >= 0 && upscaleMode < kTilegenUpscaleModeCount) ? upscaleMode : 0;
  return static_cast<Program>(static_cast<uint16_t>(Program::TilegenUpscale0) +
                              static_cast<uint16_t>(mode));
}

// The R3D triangle and quad programs deliberately share one.
enum class SetLayoutId : uint32_t
{
  R3D = 0,
  ScrollFog,
  FrameBufferBase,
  FrameBufferTrans,
  Tilegen,
  PostProcess,
  Crosshair,
  Count
};

// The scaled framebuffer composites need their own for the push constant range.
enum class PipelineLayoutId : uint32_t
{
  R3D = 0,
  ScrollFog,
  FrameBufferBase,
  FrameBufferBaseScaled,
  FrameBufferTrans,
  FrameBufferTransScaled,
  Tilegen,
  PostProcess,
  Crosshair,
  Count
};

// Packed, trivially comparable pipeline state. Explicit padding keeps memcmp
// and a byte-wise hash well defined.
struct PipelineKey
{
  uint64_t renderPass = 0;             // VkRenderPass, as an integer
  uint32_t colourFormat = 0;           // VkFormat of colour attachment 0
  uint32_t depthStencilFormat = 0;     // VkFormat, 0 when there is no depth
  uint16_t program = 0;                // Program
  uint8_t subpass = 0;
  uint8_t colourAttachmentCount = 0;
  uint8_t topology = 0;                // VkPrimitiveTopology
  uint8_t cullMode = 0;                // VkCullModeFlags
  uint8_t frontFace = 0;               // VkFrontFace
  uint8_t blendEnable = 0;
  uint8_t srcColorBlendFactor = 0;     // VkBlendFactor
  uint8_t dstColorBlendFactor = 0;
  uint8_t colorBlendOp = 0;            // VkBlendOp, basic values only
  uint8_t srcAlphaBlendFactor = 0;
  uint8_t dstAlphaBlendFactor = 0;
  uint8_t alphaBlendOp = 0;
  uint8_t depthTestEnable = 0;
  uint8_t depthWriteEnable = 0;
  uint8_t depthCompareOp = 0;          // VkCompareOp
  uint8_t stencilTestEnable = 0;
  uint8_t stencilCompareOp = 0;        // VkCompareOp
  uint8_t stencilFailOp = 0;           // VkStencilOp
  uint8_t stencilPassOp = 0;
  uint8_t stencilDepthFailOp = 0;
  uint8_t stencilCompareMask = 0;
  uint8_t stencilWriteMask = 0;
  uint8_t stencilReference = 0;
  // Indexed by kR3DAttachmentColour/Trans0/Trans1/Los.
  uint8_t colourWriteMask[4] = { 0, 0, 0, 0 };
  uint8_t _pad[3] = { 0, 0, 0 };

  bool operator==(const PipelineKey &other) const;
  bool operator!=(const PipelineKey &other) const { return !(*this == other); }
};

static_assert(sizeof(PipelineKey) == 48, "PipelineKey must have no implicit padding");
static_assert(alignof(PipelineKey) == 8, "PipelineKey alignment");
static_assert(offsetof(PipelineKey, renderPass) == 0, "PipelineKey::renderPass offset");
static_assert(offsetof(PipelineKey, program) == 16, "PipelineKey::program offset");
static_assert(offsetof(PipelineKey, colourWriteMask) == 41, "PipelineKey::colourWriteMask offset");

struct PipelineKeyHash
{
  size_t operator()(const PipelineKey &key) const;
};

// Filled in from SceneTargets. The render passes may be any of that class's load
// variants, since they are render-pass compatible.
struct TargetInfo
{
  VkRenderPass scenePass = VK_NULL_HANDLE;
  // As scenePass with the LOS attachment unused. Not compatible with scenePass,
  // so the colour layer and scroll fog need pipelines for both.
  VkRenderPass scenePassNoLos = VK_NULL_HANDLE;
  // Compatible with scenePass except for their unused attachment references, so
  // pipelines for the translucency layers must be built against these.
  VkRenderPass scenePassTransFirst = VK_NULL_HANDLE;
  VkRenderPass scenePassTransSecond = VK_NULL_HANDLE;
  VkRenderPass sceneColourPass = VK_NULL_HANDLE;
  VkRenderPass outputPass = VK_NULL_HANDLE;
  VkFormat colourFormat = VK_FORMAT_UNDEFINED;
  VkFormat depthStencilFormat = VK_FORMAT_UNDEFINED;
  // False makes Prebuild() skip the quad program instead of failing.
  bool geometryShader = true;
};

class PipelineCache
{
public:
  PipelineCache() = default;
  ~PipelineCache();

  PipelineCache(const PipelineCache &) = delete;
  PipelineCache &operator=(const PipelineCache &) = delete;

  // Shader modules are created on demand.
  bool Create(const DeviceContext &device, std::string *error);

  // Safe at any time, including after a failed Create().
  void Destroy(const DeviceContext &device);

  bool IsValid() const { return m_created; }

  VkDescriptorSetLayout SetLayout(SetLayoutId id) const;
  VkPipelineLayout PipelineLayout(PipelineLayoutId id) const;

  VkPipelineLayout LayoutFor(Program program) const;
  static PipelineLayoutId LayoutIdFor(Program program);

  VkPipelineCache Handle() const { return m_pipelineCache; }

  // Creates on a miss. A hit allocates nothing.
  VkPipeline Get(const DeviceContext &device, const PipelineKey &key, std::string *error);

  bool Warm(const DeviceContext &device, const PipelineKey *keys, size_t count, std::string *error);

  // Builds every key the renderer uses in steady state, so Get() never compiles
  // mid-frame.
  bool Prebuild(const DeviceContext &device, const TargetInfo &targets, std::string *error);

  // Leaves the built pipelines alone; Get() recreates modules as needed.
  void ReleaseShaderModules(const DeviceContext &device);

  size_t PipelineCount() const { return m_pipelines.size(); }

  // Key factories, shared with Prebuild() so a warmed cache is guaranteed to hit.

  // program must be Program::R3DTriangle or Program::R3DQuad. losActive selects
  // the render pass variant; false is the one with the LOS attachment unused.
  static PipelineKey MakeR3DKey(const TargetInfo &targets, Program program, New3D::Layer layer,
                                bool layeredMesh, bool noLosReturn, bool losActive);

  static PipelineKey MakeScrollFogKey(const TargetInfo &targets, bool losActive);

  // program must be one of the four FrameBuffer values.
  static PipelineKey MakeFrameBufferKey(const TargetInfo &targets, Program program);

  static PipelineKey MakeTilegenKey(const TargetInfo &targets, int upscaleMode, bool blend);

  static PipelineKey MakePostProcessKey(const TargetInfo &targets);

  static PipelineKey MakeCrosshairKey(const TargetInfo &targets, bool bitmap);

private:
  struct ProgramInfo
  {
    ShaderModuleId vertex;
    ShaderModuleId fragment;
    ShaderModuleId geometry;   // Count when there is none
    PipelineLayoutId layout;
    bool r3dVertexInput;
    bool crosshairVertexInput;
  };

  static const ProgramInfo &InfoFor(Program program);

  bool CreateSetLayouts(const DeviceContext &device, std::string *error);
  bool CreateSetLayout(const DeviceContext &device, SetLayoutId id,
                       const VkDescriptorSetLayoutBinding *bindings, uint32_t count,
                       std::string *error);
  bool CreatePipelineLayouts(const DeviceContext &device, std::string *error);
  bool CreatePipelineLayout(const DeviceContext &device, PipelineLayoutId id, SetLayoutId setLayout,
                            const VkPushConstantRange *pushConstants, std::string *error);
  bool CreatePipelineCacheObject(const DeviceContext &device, std::string *error);

  bool ShaderModuleHandle(const DeviceContext &device, ShaderModuleId id, VkShaderModule *out,
                          std::string *error);
  bool CreatePipeline(const DeviceContext &device, const PipelineKey &key, VkPipeline *out,
                      std::string *error);
  bool ValidateKey(const PipelineKey &key, std::string *error) const;

  static bool SetError(std::string *error, const std::string &message);

  DeviceContext m_deviceContext{};
  bool m_created = false;
  bool m_independentBlend = false;
  bool m_geometryShader = false;

  std::array<VkDescriptorSetLayout, static_cast<size_t>(SetLayoutId::Count)> m_setLayouts{};
  std::array<VkPipelineLayout, static_cast<size_t>(PipelineLayoutId::Count)> m_pipelineLayouts{};
  std::array<VkShaderModule, static_cast<size_t>(ShaderModuleId::Count)> m_shaderModules{};

  VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
  std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> m_pipelines;
};

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
#endif // INCLUDED_VULKAN_PIPELINES_H
