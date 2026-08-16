// Supermodel - vertexShaderR3D equivalent from R3DShaderTriangles.h.
//
// gl_Position is written exactly as in GL. Both differences from GL clip space
// are handled outside the shader: Y by the pipeline's negative-height viewport,
// and depth by the reversed-Z clear and GREATER_OR_EQUAL compare op.

#version 450 core

#extension GL_GOOGLE_include_directive : require

#include "R3DShaderUniforms.glsl"
#include "R3DModelPushConstants.glsl"

// attributes - see VulkanShaderInterface.h for the New3D::FVertex offsets
layout(location = 0) in vec4  inVertex;
layout(location = 1) in vec3  inNormal;
layout(location = 2) in vec2  inTexCoord;
layout(location = 3) in vec3  inFaceNormal;		// used to emulate r3d culling
layout(location = 4) in float inFixedShade;
layout(location = 5) in vec4  inColour;
layout(location = 6) in float inTextureNP;

// outputs to fragment shader
layout(location = 0) out vec3  fsViewVertex;
layout(location = 1) out vec3  fsViewNormal;	// per vertex normal vector
layout(location = 2) out vec2  fsTexCoord;
layout(location = 3) out vec4  fsColor;
layout(location = 4) out float fsFixedShade;
layout(location = 5) out float fsDiscardPoly;	// can't have varying bool (glsl spec)
layout(location = 6) out float fsLODBase;

vec4 GetColour(vec4 colour)
{
	vec4 c = colour;

	if(translatorMap) {
		c.rgb *= 16.0;
	}

	c.a *= nodeAlpha;

	return c;
}

float CalcBackFace(in vec3 viewVertex)
{
	vec3 vt = viewVertex; // - vec3(0.0);
	vec3 vn = mat3(modelMat) * inFaceNormal;

	// dot product of face normal with view direction
	return dot(vt, vn);
}

void main(void)
{
	fsViewVertex	= (modelMat * inVertex).xyz;
	fsViewNormal	= (mat3(modelMat) / modelScale) * inNormal;
	fsDiscardPoly	= CalcBackFace(fsViewVertex);
	fsColor			= GetColour(inColour);
	fsTexCoord		= inTexCoord;
	fsFixedShade	= inFixedShade;
	fsLODBase		= fsDiscardPoly * -cota * inTextureNP;
	gl_Position		= (projMat * modelMat) * inVertex;
}
