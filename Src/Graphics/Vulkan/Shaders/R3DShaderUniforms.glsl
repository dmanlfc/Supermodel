// Supermodel - Real3D shader uniform state, included by R3DTriangle.{vert,frag}
// and R3DQuad.{vert,geom,frag}. Mirrored by VulkanShaderInterface.h, which
// static_asserts every offset declared here, so the two must agree byte for byte.
//
// The block is declared without an instance name so its members land in global
// scope, keeping the shader bodies textually identical to the OpenGL originals.
//
// SPIR-V forbids OpTypeBool in a Block, so every GL `uniform bool` is a `uint`
// named with a `b_` prefix, exposed through a #define restoring the original
// spelling.

#ifndef SUPERMODEL_R3D_SHADER_UNIFORMS_GLSL
#define SUPERMODEL_R3D_SHADER_UNIFORMS_GLSL

// Visible to the vertex, geometry and fragment stages. Total size 320 bytes.
layout(std140, set = 0, binding = 0) uniform R3DUniforms
{
	// --- 16-byte aligned region ------------------------------------------
	layout(offset =   0) mat4  projMat;             // projection matrix; [2][2] and [3][2] are also read by the fragment stage
	layout(offset =  64) vec4  fogColour;           // .rgb = GL `fogColour`; .a unused by R3D (kept for vec4 alignment)
	layout(offset =  80) vec4  spotEllipse;         // .xy = centre in framebuffer coords, .zw = half extents
	layout(offset =  96) vec4  spotColor;           // .rgb = GL `spotColor`
	layout(offset = 112) vec4  spotFogColor;        // .rgb = GL `spotFogColor`
	layout(offset = 128) vec4  lighting[2];         // [0].xyz = sun direction, [1].x = diffuse, [1].y = ambient (GL `lighting[2]` as vec3[2])
	layout(offset = 160) ivec4 baseTexInfo;         // .xy = position in the texture sheet, .zw = width/height

	// --- 8-byte aligned region -------------------------------------------
	layout(offset = 176) vec2  spotRange;           // .x = start (view space), .y = limit
	layout(offset = 184) ivec2 textureWrapMode;     // per-axis Mesh::TexWrapMode
	layout(offset = 192) vec2  fragCoordYTransform; // see ScreenFragCoord() below

	// --- 4-byte scalar region --------------------------------------------
	layout(offset = 200) float cota;                // cotangent of half the vertical FOV; feeds the LOD base
	layout(offset = 204) float fogIntensity;
	layout(offset = 208) float fogDensity;
	layout(offset = 212) float fogStart;
	layout(offset = 216) float fogAttenuation;
	layout(offset = 220) float fogAmbient;
	layout(offset = 224) float specularValue;
	layout(offset = 228) float shininess;
	layout(offset = 232) float microTextureMinLOD;
	layout(offset = 236) int   baseTexType;
	layout(offset = 240) int   microTextureID;
	layout(offset = 244) int   texturePage;
	layout(offset = 248) int   hardwareStep;        // 0x10, 0x15, 0x20, 0x21
	layout(offset = 252) int   colourLayer;         // LayerColour / LayerTrans0 / LayerTrans1
	layout(offset = 256) uint  b_textureEnabled;
	layout(offset = 260) uint  b_microTexture;
	layout(offset = 264) uint  b_textureInverted;
	layout(offset = 268) uint  b_textureAlpha;
	layout(offset = 272) uint  b_alphaTest;
	layout(offset = 276) uint  b_discardAlpha;
	layout(offset = 280) uint  b_lightEnabled;
	layout(offset = 284) uint  b_sunClamp;
	layout(offset = 288) uint  b_intensityClamp;
	layout(offset = 292) uint  b_specularEnabled;
	layout(offset = 296) uint  b_fixedShading;
	layout(offset = 300) uint  b_smoothShading;
	layout(offset = 304) uint  b_polyAlpha;         // carried over from GL, where it is also currently unused
	layout(offset = 308) uint  b_noLosReturn;       // line-of-sight "no return" flag
	layout(offset = 312) uint  b_encodeDepth;       // 0 raw, 1 encode
	// std140 rounds the block up to a multiple of 16, so 316..319 is implicit
	// tail padding. The C++ mirror declares it explicitly.
};

#define textureEnabled  bool(b_textureEnabled)
#define microTexture    bool(b_microTexture)
#define textureInverted bool(b_textureInverted)
#define textureAlpha    bool(b_textureAlpha)
#define alphaTest       bool(b_alphaTest)
#define discardAlpha    bool(b_discardAlpha)
#define lightEnabled    bool(b_lightEnabled)
#define sunClamp        bool(b_sunClamp)
#define intensityClamp  bool(b_intensityClamp)
#define specularEnabled bool(b_specularEnabled)
#define fixedShading    bool(b_fixedShading)
#define smoothShading   bool(b_smoothShading)
#define polyAlpha       bool(b_polyAlpha)
#define noLosReturn     bool(b_noLosReturn)
#define depthEncodeMode b_encodeDepth

// One binding holding two combined image/samplers. Only constant indices are ever
// used (see texBiLinearBank in R3DShaderCommon.glsl), which keeps the shader valid
// without the shaderSampledImageArrayDynamicIndexing feature.
layout(set = 0, binding = 1) uniform usampler2D textureBank[2];

#endif // SUPERMODEL_R3D_SHADER_UNIFORMS_GLSL
