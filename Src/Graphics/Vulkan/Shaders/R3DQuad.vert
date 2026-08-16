// Supermodel - vertexShaderR3DQuads equivalent from R3DShaderQuads.h.
//
// Identical to R3DTriangle.vert except that the attributes are forwarded to the
// geometry stage in a block rather than straight to the fragment stage.

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

// outputs to geometry shader
layout(location = 0) out VS_OUT
{
	vec3	viewVertex;
	vec3	viewNormal;		// per vertex normal vector
	vec2	texCoord;
	vec4	color;
	float	fixedShade;
	float	discardPoly;	// can't have varying bool (glsl spec)
	float	LODBase;
} vs_out;

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
	vs_out.viewVertex	= (modelMat * inVertex).xyz;
	vs_out.viewNormal	= (mat3(modelMat) / modelScale) * inNormal;
	vs_out.discardPoly	= CalcBackFace(vs_out.viewVertex);
	vs_out.color		= GetColour(inColour);
	vs_out.texCoord		= inTexCoord;
	vs_out.fixedShade	= inFixedShade;
	vs_out.LODBase		= vs_out.discardPoly * -cota * inTextureNP;
	gl_Position			= (projMat * modelMat) * inVertex;
}
