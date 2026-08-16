// Supermodel - crosshair fragment shader, from Src/OSD/SDL/Crosshair.cpp.
//
// The push constant block is shared with the vertex stage, so it must be declared
// identically in both. When isBitmap is false the sampler is never read, but a
// valid descriptor must still be bound.

#version 450 core

layout(push_constant) uniform CrosshairPushConstants
{
	layout(offset =  0) mat4 mvp;
	layout(offset = 64) vec4 colour;
	layout(offset = 80) uint b_isBitmap;
};

layout(set = 0, binding = 0) uniform sampler2D CrosshairTexture;

layout(location = 0) in vec2 UV;

layout(location = 0) out vec4 fragColour;

void main(void)
{
	if (b_isBitmap == 0u)
		fragColour = colour;
	else
		fragColour = colour * texture(CrosshairTexture, UV);
}
