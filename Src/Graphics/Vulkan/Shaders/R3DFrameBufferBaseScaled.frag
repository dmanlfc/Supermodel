// Supermodel - R3DFrameBuffers::AllocShaderBaseScaled equivalent. Pair with
// FullScreenQuadUV.vert.
//
// sourceSize is in push constants rather than a uniform buffer: it is per-draw,
// two words wide, and changes with the window size.

#version 450 core

layout(location = 0) in vec2 texCoord;

layout(set = 0, binding = 0) uniform sampler2D tex1;

layout(push_constant) uniform R3DFrameBufferPushConstants
{
	layout(offset = 0) ivec2 sourceSize;
};

layout(location = 0) out vec4 fragColor;

void main()
{
	ivec2 tc = clamp(ivec2(texCoord * vec2(sourceSize)), ivec2(0), sourceSize - ivec2(1));
	fragColor = texelFetch(tex1, tc, 0);
}
