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
 * VulkanShaderInterface.h
 *
 * Contract between the shaders in Src/Graphics/Vulkan/Shaders/ and the C++ that
 * drives them: descriptor bindings, push constant ranges, vertex inputs and
 * std140 block mirrors.
 *
 * Blocks are UNIFORM_BUFFER_DYNAMIC suballocated from a ring, so offsets round
 * up to minUniformBufferOffsetAlignment, not sizeof(struct). SPIR-V forbids
 * OpTypeBool in a Block, so a GL `uniform bool` becomes a uint32_t with a `b_`
 * prefix; use ShaderBool().
 *
 * Depth is reversed-Z over 0..1 as in GL: cleared to 0, compared
 * GREATER_OR_EQUAL, gl_FragDepth = projMat[3][2] * gl_FragCoord.w, which rules
 * out layout(depth_greater). Every pipeline uses a negative-height viewport so
 * gl_Position matches the GL original; gl_FragCoord.y does not follow, so GL
 * screen coordinates (spotEllipse.xy) flip back through fragCoordYTransform.
 */

#ifndef INCLUDED_VULKAN_SHADER_INTERFACE_H
#define INCLUDED_VULKAN_SHADER_INTERFACE_H

#ifdef SUPERMODEL_VULKAN

#include <array>
#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan.h>

#include "Graphics/New3D/Model.h"

namespace Vulkan
{

inline constexpr uint32_t ShaderBool(bool value)
{
  return value ? 1u : 0u;
}

inline constexpr float kReversedZClearDepth = 0.0f;
inline constexpr VkCompareOp kReversedZCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

// Negative-height viewport reproducing GL's clip space. The rectangle is in
// top-left origin framebuffer coordinates.
inline VkViewport MakeViewport(float x, float y, float width, float height)
{
  VkViewport viewport{};
  viewport.x = x;
  viewport.y = y + height;
  viewport.width = width;
  viewport.height = -height;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  return viewport;
}

// Flips gl_FragCoord.y back to a lower-left origin. Pass {1, 0} instead to
// pre-flip on the CPU.
inline constexpr std::array<float, 2> MakeFragCoordYTransform(float viewportHeight)
{
  return { -1.0f, viewportHeight };
}

// R3D pipelines: R3DTriangle.{vert,frag}, R3DQuad.{vert,geom,frag}. Both share
// one descriptor set layout, push constant range and vertex input layout.
// R3DQuad.geom reads no uniforms, hence no VK_SHADER_STAGE_GEOMETRY_BIT below.
inline constexpr uint32_t kR3DDescriptorSet = 0;
inline constexpr uint32_t kR3DBindingUniforms = 0;
inline constexpr uint32_t kR3DBindingTextureBank = 1;
inline constexpr uint32_t kR3DTextureBankCount = 2;

inline constexpr uint32_t kR3DAttachmentColour = 0;
inline constexpr uint32_t kR3DAttachmentTrans0 = 1;
inline constexpr uint32_t kR3DAttachmentTrans1 = 2;
inline constexpr uint32_t kR3DAttachmentLos = 3;
inline constexpr VkFormat kR3DLosFormat = VK_FORMAT_R32G32_UINT;

// Mirrors LayerColour/LayerTrans0/LayerTrans1 in R3DShaderCommon.glsl.
inline constexpr int32_t kR3DLayerColour = 0x0;
inline constexpr int32_t kR3DLayerTrans0 = 0x1;
inline constexpr int32_t kR3DLayerTrans1 = 0x2;

inline constexpr std::array<VkDescriptorSetLayoutBinding, 2> kR3DDescriptorBindings = { {
  { kR3DBindingUniforms, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
  { kR3DBindingTextureBank, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kR3DTextureBankCount,
    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
} };

// Byte-exact mirror of R3DUniforms in Shaders/R3DShaderUniforms.glsl. vec3
// members occupy four floats (std140 aligns vec3 to 16). `lighting` widens GL's
// vec3 lighting[2]: [0].xyz sun direction, [1].x/.y diffuse/ambient intensity.
struct alignas(16) R3DUniforms
{
  float projMat[16];                  //   0  column-major
  float fogColour[4];                 //  64  .rgb used
  float spotEllipse[4];               //  80  .xy centre (framebuffer coords), .zw half extents
  float spotColor[4];                 //  96  .rgb used
  float spotFogColor[4];              // 112  .rgb used
  float lighting[2][4];               // 128
  int32_t baseTexInfo[4];             // 160  .xy sheet position, .zw size

  float spotRange[2];                 // 176  .x start (view space), .y limit
  int32_t textureWrapMode[2];         // 184  per axis Mesh::TexWrapMode
  float fragCoordYTransform[2];       // 192  see MakeFragCoordYTransform

  float cota;                         // 200
  float fogIntensity;                 // 204
  float fogDensity;                   // 208
  float fogStart;                     // 212
  float fogAttenuation;               // 216
  float fogAmbient;                   // 220
  float specularValue;                // 224
  float shininess;                    // 228
  float microTextureMinLOD;           // 232
  int32_t baseTexType;                // 236
  int32_t microTextureID;             // 240
  int32_t texturePage;                // 244
  int32_t hardwareStep;               // 248  0x10, 0x15, 0x20, 0x21
  int32_t colourLayer;                // 252  kR3DLayer*
  uint32_t b_textureEnabled;          // 256
  uint32_t b_microTexture;            // 260
  uint32_t b_textureInverted;         // 264
  uint32_t b_textureAlpha;            // 268
  uint32_t b_alphaTest;               // 272
  uint32_t b_discardAlpha;            // 276
  uint32_t b_lightEnabled;            // 280
  uint32_t b_sunClamp;                // 284
  uint32_t b_intensityClamp;          // 288
  uint32_t b_specularEnabled;         // 292
  uint32_t b_fixedShading;            // 296
  uint32_t b_smoothShading;           // 300
  uint32_t b_polyAlpha;               // 304  unused, as in GL
  uint32_t b_noLosReturn;             // 308
  // Set when the depth attachment is a normalised integer format: reversed-Z
  // crowds all useful depth near zero, so the shader spreads it monotonically.
  uint32_t b_encodeDepth;             // 312
  uint32_t _pad;                      // 316  std140 rounds the block up to 320
};

static_assert(sizeof(R3DUniforms) == 320, "R3DUniforms must match the std140 block size");
static_assert(alignof(R3DUniforms) == 16, "R3DUniforms must be 16-byte aligned");
static_assert(offsetof(R3DUniforms, projMat) == 0, "R3DUniforms::projMat offset");
static_assert(offsetof(R3DUniforms, fogColour) == 64, "R3DUniforms::fogColour offset");
static_assert(offsetof(R3DUniforms, spotEllipse) == 80, "R3DUniforms::spotEllipse offset");
static_assert(offsetof(R3DUniforms, spotColor) == 96, "R3DUniforms::spotColor offset");
static_assert(offsetof(R3DUniforms, spotFogColor) == 112, "R3DUniforms::spotFogColor offset");
static_assert(offsetof(R3DUniforms, lighting) == 128, "R3DUniforms::lighting offset");
static_assert(offsetof(R3DUniforms, baseTexInfo) == 160, "R3DUniforms::baseTexInfo offset");
static_assert(offsetof(R3DUniforms, spotRange) == 176, "R3DUniforms::spotRange offset");
static_assert(offsetof(R3DUniforms, textureWrapMode) == 184, "R3DUniforms::textureWrapMode offset");
static_assert(offsetof(R3DUniforms, fragCoordYTransform) == 192, "R3DUniforms::fragCoordYTransform offset");
static_assert(offsetof(R3DUniforms, cota) == 200, "R3DUniforms::cota offset");
static_assert(offsetof(R3DUniforms, fogIntensity) == 204, "R3DUniforms::fogIntensity offset");
static_assert(offsetof(R3DUniforms, fogDensity) == 208, "R3DUniforms::fogDensity offset");
static_assert(offsetof(R3DUniforms, fogStart) == 212, "R3DUniforms::fogStart offset");
static_assert(offsetof(R3DUniforms, fogAttenuation) == 216, "R3DUniforms::fogAttenuation offset");
static_assert(offsetof(R3DUniforms, fogAmbient) == 220, "R3DUniforms::fogAmbient offset");
static_assert(offsetof(R3DUniforms, specularValue) == 224, "R3DUniforms::specularValue offset");
static_assert(offsetof(R3DUniforms, shininess) == 228, "R3DUniforms::shininess offset");
static_assert(offsetof(R3DUniforms, microTextureMinLOD) == 232, "R3DUniforms::microTextureMinLOD offset");
static_assert(offsetof(R3DUniforms, baseTexType) == 236, "R3DUniforms::baseTexType offset");
static_assert(offsetof(R3DUniforms, microTextureID) == 240, "R3DUniforms::microTextureID offset");
static_assert(offsetof(R3DUniforms, texturePage) == 244, "R3DUniforms::texturePage offset");
static_assert(offsetof(R3DUniforms, hardwareStep) == 248, "R3DUniforms::hardwareStep offset");
static_assert(offsetof(R3DUniforms, colourLayer) == 252, "R3DUniforms::colourLayer offset");
static_assert(offsetof(R3DUniforms, b_textureEnabled) == 256, "R3DUniforms::b_textureEnabled offset");
static_assert(offsetof(R3DUniforms, b_microTexture) == 260, "R3DUniforms::b_microTexture offset");
static_assert(offsetof(R3DUniforms, b_textureInverted) == 264, "R3DUniforms::b_textureInverted offset");
static_assert(offsetof(R3DUniforms, b_textureAlpha) == 268, "R3DUniforms::b_textureAlpha offset");
static_assert(offsetof(R3DUniforms, b_alphaTest) == 272, "R3DUniforms::b_alphaTest offset");
static_assert(offsetof(R3DUniforms, b_discardAlpha) == 276, "R3DUniforms::b_discardAlpha offset");
static_assert(offsetof(R3DUniforms, b_lightEnabled) == 280, "R3DUniforms::b_lightEnabled offset");
static_assert(offsetof(R3DUniforms, b_sunClamp) == 284, "R3DUniforms::b_sunClamp offset");
static_assert(offsetof(R3DUniforms, b_intensityClamp) == 288, "R3DUniforms::b_intensityClamp offset");
static_assert(offsetof(R3DUniforms, b_specularEnabled) == 292, "R3DUniforms::b_specularEnabled offset");
static_assert(offsetof(R3DUniforms, b_fixedShading) == 296, "R3DUniforms::b_fixedShading offset");
static_assert(offsetof(R3DUniforms, b_smoothShading) == 300, "R3DUniforms::b_smoothShading offset");
static_assert(offsetof(R3DUniforms, b_polyAlpha) == 304, "R3DUniforms::b_polyAlpha offset");
static_assert(offsetof(R3DUniforms, b_noLosReturn) == 308, "R3DUniforms::b_noLosReturn offset");
static_assert(offsetof(R3DUniforms, b_encodeDepth) == 312, "R3DUniforms::b_encodeDepth offset");

// Byte-exact mirror of the R3DModelPushConstants block.
struct alignas(16) R3DModelPushConstants
{
  float modelMat[16];        //  0  column-major
  float modelScale;          // 64
  float nodeAlpha;           // 68
  uint32_t b_translatorMap;  // 72
  uint32_t _pad;             // 76
};

static_assert(sizeof(R3DModelPushConstants) == 80, "R3DModelPushConstants size");
static_assert(alignof(R3DModelPushConstants) == 16, "R3DModelPushConstants alignment");
static_assert(offsetof(R3DModelPushConstants, modelMat) == 0, "R3DModelPushConstants::modelMat offset");
static_assert(offsetof(R3DModelPushConstants, modelScale) == 64, "R3DModelPushConstants::modelScale offset");
static_assert(offsetof(R3DModelPushConstants, nodeAlpha) == 68, "R3DModelPushConstants::nodeAlpha offset");
static_assert(offsetof(R3DModelPushConstants, b_translatorMap) == 72, "R3DModelPushConstants::b_translatorMap offset");
static_assert(sizeof(R3DModelPushConstants) <= 128, "push constants must fit the guaranteed 128-byte range");

inline constexpr VkPushConstantRange kR3DPushConstantRange = {
  VK_SHADER_STAGE_VERTEX_BIT, 0, static_cast<uint32_t>(sizeof(R3DModelPushConstants))
};

// Vertex input matching New3D::FVertex. inColour is the only normalized attribute.
inline constexpr uint32_t kR3DVertexBinding = 0;

inline constexpr uint32_t kR3DLocationVertex = 0;
inline constexpr uint32_t kR3DLocationNormal = 1;
inline constexpr uint32_t kR3DLocationTexCoord = 2;
inline constexpr uint32_t kR3DLocationFaceNormal = 3;
inline constexpr uint32_t kR3DLocationFixedShade = 4;
inline constexpr uint32_t kR3DLocationColour = 5;
inline constexpr uint32_t kR3DLocationTextureNP = 6;

inline constexpr VkVertexInputBindingDescription kR3DVertexBindingDescription = {
  kR3DVertexBinding, static_cast<uint32_t>(sizeof(New3D::FVertex)), VK_VERTEX_INPUT_RATE_VERTEX
};

inline constexpr std::array<VkVertexInputAttributeDescription, 7> kR3DVertexAttributes = { {
  { kR3DLocationVertex,     kR3DVertexBinding, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(New3D::FVertex, base.pos) },
  { kR3DLocationNormal,     kR3DVertexBinding, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(New3D::FVertex, base.normal) },
  { kR3DLocationTexCoord,   kR3DVertexBinding, VK_FORMAT_R32G32_SFLOAT,       offsetof(New3D::FVertex, base.texcoords) },
  { kR3DLocationFaceNormal, kR3DVertexBinding, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(New3D::FVertex, faceNormal) },
  { kR3DLocationFixedShade, kR3DVertexBinding, VK_FORMAT_R32_SFLOAT,          offsetof(New3D::FVertex, base.fixedShade) },
  { kR3DLocationColour,     kR3DVertexBinding, VK_FORMAT_R8G8B8A8_UNORM,      offsetof(New3D::FVertex, faceColour) },
  { kR3DLocationTextureNP,  kR3DVertexBinding, VK_FORMAT_R32_SFLOAT,          offsetof(New3D::FVertex, textureNP) },
} };

// Catch a change in FVertex's shape here rather than in a driver.
static_assert(sizeof(New3D::Vertex) == 40, "New3D::Vertex layout changed");
static_assert(sizeof(New3D::FVertex) == 60, "New3D::FVertex layout changed");
static_assert(offsetof(New3D::FVertex, base.pos) == 0, "FVertex::base.pos offset");
static_assert(offsetof(New3D::FVertex, base.normal) == 16, "FVertex::base.normal offset");
static_assert(offsetof(New3D::FVertex, base.texcoords) == 28, "FVertex::base.texcoords offset");
static_assert(offsetof(New3D::FVertex, base.fixedShade) == 36, "FVertex::base.fixedShade offset");
static_assert(offsetof(New3D::FVertex, faceNormal) == 40, "FVertex::faceNormal offset");
static_assert(offsetof(New3D::FVertex, faceColour) == 52, "FVertex::faceColour offset");
static_assert(offsetof(New3D::FVertex, textureNP) == 56, "FVertex::textureNP offset");

// Scroll fog: FullScreenQuad.vert + ScrollFog.frag. Writes the three R3D colour
// attachments but no LOS output, so attachment 3 needs colorWriteMask = 0.
inline constexpr uint32_t kScrollFogDescriptorSet = 0;
inline constexpr uint32_t kScrollFogBindingUniforms = 0;

inline constexpr std::array<VkDescriptorSetLayoutBinding, 1> kScrollFogDescriptorBindings = { {
  { kScrollFogBindingUniforms, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
} };

struct alignas(16) ScrollFogUniforms
{
  float fogColour[4];            //  0  .rgb colour, .a density
  float spotEllipse[4];          // 16  .xy centre, .zw half extents
  float spotFogColor[4];         // 32  .rgb used
  float fragCoordYTransform[2];  // 48
  float fogAttenuation;          // 56
  float fogAmbient;              // 60
};

static_assert(sizeof(ScrollFogUniforms) == 64, "ScrollFogUniforms size");
static_assert(alignof(ScrollFogUniforms) == 16, "ScrollFogUniforms alignment");
static_assert(offsetof(ScrollFogUniforms, fogColour) == 0, "ScrollFogUniforms::fogColour offset");
static_assert(offsetof(ScrollFogUniforms, spotEllipse) == 16, "ScrollFogUniforms::spotEllipse offset");
static_assert(offsetof(ScrollFogUniforms, spotFogColor) == 32, "ScrollFogUniforms::spotFogColor offset");
static_assert(offsetof(ScrollFogUniforms, fragCoordYTransform) == 48, "ScrollFogUniforms::fragCoordYTransform offset");
static_assert(offsetof(ScrollFogUniforms, fogAttenuation) == 56, "ScrollFogUniforms::fogAttenuation offset");
static_assert(offsetof(ScrollFogUniforms, fogAmbient) == 60, "ScrollFogUniforms::fogAmbient offset");

// R3D framebuffer composite. Base takes tex1 only, Trans both trans layers; the
// Scaled variants use FullScreenQuadUV.vert and the push constant range below.
inline constexpr uint32_t kFrameBufferDescriptorSet = 0;
inline constexpr uint32_t kFrameBufferBindingTex1 = 0;
inline constexpr uint32_t kFrameBufferBindingTex2 = 1;

inline constexpr std::array<VkDescriptorSetLayoutBinding, 1> kFrameBufferBaseDescriptorBindings = { {
  { kFrameBufferBindingTex1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
} };

inline constexpr std::array<VkDescriptorSetLayoutBinding, 2> kFrameBufferTransDescriptorBindings = { {
  { kFrameBufferBindingTex1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
  { kFrameBufferBindingTex2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
} };

// Only the scaled variants declare this range.
struct R3DFrameBufferPushConstants
{
  int32_t sourceSize[2];  // 0
};

static_assert(sizeof(R3DFrameBufferPushConstants) == 8, "R3DFrameBufferPushConstants size");
static_assert(alignof(R3DFrameBufferPushConstants) == 4, "R3DFrameBufferPushConstants alignment");
static_assert(offsetof(R3DFrameBufferPushConstants, sourceSize) == 0, "R3DFrameBufferPushConstants::sourceSize offset");
static_assert(sizeof(R3DFrameBufferPushConstants) <= 128, "push constants must fit the guaranteed 128-byte range");

inline constexpr VkPushConstantRange kR3DFrameBufferScaledPushConstantRange = {
  VK_SHADER_STAGE_FRAGMENT_BIT, 0, static_cast<uint32_t>(sizeof(R3DFrameBufferPushConstants))
};

// 2D tilegen: Tilegen.vert + Tilegen.frag. UPSCALEMODE is a compile-time define,
// so the Makefile builds four modules; Nearest and Bilinear share code and differ
// only in the sampler filter, see TilegenSamplerFilter.
inline constexpr uint32_t kTilegenDescriptorSet = 0;
inline constexpr uint32_t kTilegenBindingTex1 = 0;

inline constexpr std::array<VkDescriptorSetLayoutBinding, 1> kTilegenDescriptorBindings = { {
  { kTilegenBindingTex1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
} };

// Mirrors ::UpscaleMode, as plain integers to keep this header independent of the
// OpenGL renderer.
inline constexpr int kTilegenUpscaleNearest = 0;
inline constexpr int kTilegenUpscaleBiquintic = 1;
inline constexpr int kTilegenUpscaleBilinear = 2;
inline constexpr int kTilegenUpscaleBicubic = 3;
inline constexpr int kTilegenUpscaleModeCount = 4;

inline constexpr VkFilter TilegenSamplerFilter(int upscaleMode)
{
  return upscaleMode == kTilegenUpscaleNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

// Post processing: FullScreenQuad.vert + PostProcess.frag. aa and crtColorMode
// are fixed at construction, so they live in the uniform block, not push constants.
inline constexpr uint32_t kPostProcessDescriptorSet = 0;
inline constexpr uint32_t kPostProcessBindingUniforms = 0;
inline constexpr uint32_t kPostProcessBindingTex1 = 1;

inline constexpr std::array<VkDescriptorSetLayoutBinding, 2> kPostProcessDescriptorBindings = { {
  { kPostProcessBindingUniforms, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
  { kPostProcessBindingTex1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
} };

struct alignas(16) PostProcessUniforms
{
  int32_t aa;            //  0  supersampling factor
  int32_t crtColorMode;  //  4  ::CRTcolor, 0 None .. 5 BT601EA
  int32_t _pad[2];       //  8
};

static_assert(sizeof(PostProcessUniforms) == 16, "PostProcessUniforms size");
static_assert(alignof(PostProcessUniforms) == 16, "PostProcessUniforms alignment");
static_assert(offsetof(PostProcessUniforms, aa) == 0, "PostProcessUniforms::aa offset");
static_assert(offsetof(PostProcessUniforms, crtColorMode) == 4, "PostProcessUniforms::crtColorMode offset");

// Crosshair: Crosshair.vert + Crosshair.frag. The texture descriptor must be
// bound even for the vector crosshair, which never samples it.
inline constexpr uint32_t kCrosshairDescriptorSet = 0;
inline constexpr uint32_t kCrosshairBindingTexture = 0;

inline constexpr std::array<VkDescriptorSetLayoutBinding, 1> kCrosshairDescriptorBindings = { {
  { kCrosshairBindingTexture, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
} };

struct alignas(16) CrosshairPushConstants
{
  float mvp[16];          //  0  column-major
  float colour[4];        // 64
  uint32_t b_isBitmap;    // 80
  uint32_t _pad[3];       // 84
};

static_assert(sizeof(CrosshairPushConstants) == 96, "CrosshairPushConstants size");
static_assert(alignof(CrosshairPushConstants) == 16, "CrosshairPushConstants alignment");
static_assert(offsetof(CrosshairPushConstants, mvp) == 0, "CrosshairPushConstants::mvp offset");
static_assert(offsetof(CrosshairPushConstants, colour) == 64, "CrosshairPushConstants::colour offset");
static_assert(offsetof(CrosshairPushConstants, b_isBitmap) == 80, "CrosshairPushConstants::b_isBitmap offset");
static_assert(sizeof(CrosshairPushConstants) <= 128, "push constants must fit the guaranteed 128-byte range");

inline constexpr VkPushConstantRange kCrosshairPushConstantRange = {
  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
  static_cast<uint32_t>(sizeof(CrosshairPushConstants))
};

// Two bindings rather than one interleaved, matching GL's separate position and
// UV array buffers. Strides are spelled out rather than sizeof() to avoid
// including Src/OSD/SDL/Crosshair.h here. Deviation from GL: the position
// attribute is three components, not GL's four over a 12-byte stride.
inline constexpr uint32_t kCrosshairVertexBindingPosition = 0;
inline constexpr uint32_t kCrosshairVertexBindingUV = 1;

inline constexpr uint32_t kCrosshairLocationPosition = 0;
inline constexpr uint32_t kCrosshairLocationUV = 1;

inline constexpr std::array<VkVertexInputBindingDescription, 2> kCrosshairVertexBindings = { {
  { kCrosshairVertexBindingPosition, 3 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX },  // BasicVertex{float x, y, z}
  { kCrosshairVertexBindingUV, 2 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX },
} };

inline constexpr std::array<VkVertexInputAttributeDescription, 2> kCrosshairVertexAttributes = { {
  { kCrosshairLocationPosition, kCrosshairVertexBindingPosition, VK_FORMAT_R32G32B32_SFLOAT, 0 },
  { kCrosshairLocationUV, kCrosshairVertexBindingUV, VK_FORMAT_R32G32_SFLOAT, 0 },
} };

} // namespace Vulkan

#endif // SUPERMODEL_VULKAN
#endif // INCLUDED_VULKAN_SHADER_INTERFACE_H
