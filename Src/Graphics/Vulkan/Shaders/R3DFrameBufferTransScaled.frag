// Supermodel - R3DFrameBuffers::AllocShaderTransScaled equivalent. Pair with
// FullScreenQuadUV.vert.

#version 450 core

layout(location = 0) in vec2 texCoord;

layout(set = 0, binding = 0) uniform sampler2D tex1;
layout(set = 0, binding = 1) uniform sampler2D tex2;

layout(push_constant) uniform R3DFrameBufferPushConstants
{
	layout(offset = 0) ivec2 sourceSize;
};

layout(location = 0) out vec4 fragColor;

void main()
{
	ivec2 tc = clamp(ivec2(texCoord * vec2(sourceSize)), ivec2(0), sourceSize - ivec2(1));
	vec4 colTrans1 = texelFetch(tex1, tc, 0);
	vec4 colTrans2 = texelFetch(tex2, tc, 0);

	if (colTrans1.a * colTrans2.a > 0.0) {
		vec3 mixCol = mix(colTrans1.rgb, colTrans2.rgb, (colTrans2.a + (1.0 - colTrans1.a)) / 2.0);
		fragColor = vec4(mixCol, 1.0);
	}
	else if (colTrans1.a > 0.0) {
		fragColor = colTrans1;
	}
	else {
		fragColor = colTrans2;
	}
}
