// Supermodel - R3DFrameBuffers::AllocShaderBase equivalent. Pair with
// FullScreenQuad.vert.

#version 450 core

// inputs
layout(set = 0, binding = 0) uniform sampler2D tex1;			// base tex

// outputs
layout(location = 0) out vec4 fragColor;

void main()
{
	ivec2 tc = ivec2(gl_FragCoord.xy /*-vec2(0.5)*/);
	fragColor = texelFetch(tex1, tc, 0);
}
