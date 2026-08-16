// Supermodel - crosshair vertex shader, from Src/OSD/SDL/Crosshair.cpp.
//
// Everything is per-draw and tiny, so this pipeline uses push constants only and
// has no uniform block.

#version 450 core

layout(push_constant) uniform CrosshairPushConstants
{
	layout(offset =  0) mat4 mvp;
	layout(offset = 64) vec4 colour;
	layout(offset = 80) uint b_isBitmap;
};

layout(location = 0) in vec3 inVertices;
layout(location = 1) in vec2 vertexUV;

layout(location = 0) out vec2 UV;

void main(void)
{
	gl_Position = mvp * vec4(inVertices,1.0);
	UV = vertexUV;
}
