// Supermodel - push constant block for the Real3D pipelines. Mirrored by
// Vulkan::R3DModelPushConstants in VulkanShaderInterface.h.
//
// Included only by R3DTriangle.vert and R3DQuad.vert, and deliberately kept out
// of R3DShaderUniforms.glsl: the range is registered for the vertex stage only,
// so a declaration in another stage would disagree with the pipeline layout.
//
// modelMat changes once per culling node, far more often than anything in the
// uniform block, which is why this state is not in the uniform buffer.

#ifndef SUPERMODEL_R3D_MODEL_PUSH_CONSTANTS_GLSL
#define SUPERMODEL_R3D_MODEL_PUSH_CONSTANTS_GLSL

layout(push_constant) uniform R3DModelPushConstants
{
	layout(offset =  0) mat4  modelMat;
	layout(offset = 64) float modelScale;
	layout(offset = 68) float nodeAlpha;
	layout(offset = 72) uint  b_translatorMap;
};

// GL `uniform bool translatorMap`; SPIR-V forbids booleans in Vulkan interface
// blocks, so it is stored as a uint.
#define translatorMap bool(b_translatorMap)

#endif // SUPERMODEL_R3D_MODEL_PUSH_CONSTANTS_GLSL
