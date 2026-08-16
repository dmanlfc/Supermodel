// Supermodel - R3DFrameBuffers::AllocShaderTrans equivalent. Pair with
// FullScreenQuad.vert.

#version 450 core

layout(set = 0, binding = 0) uniform sampler2D tex1;			// trans layer 1
layout(set = 0, binding = 1) uniform sampler2D tex2;			// trans layer 2

// outputs
layout(location = 0) out vec4 fragColor;

void main()
{
	ivec2 tc = ivec2(gl_FragCoord.xy /*-vec2(0.5)*/);
	vec4 colTrans1 = texelFetch(tex1, tc, 0);
	vec4 colTrans2 = texelFetch(tex2, tc, 0);

	// if both transparency layers overlap, the result is opaque
	if (colTrans1.a * colTrans2.a > 0.0) {
		vec3 mixCol = mix(colTrans1.rgb, colTrans2.rgb, (colTrans2.a + (1.0 - colTrans1.a)) / 2.0);
		fragColor = vec4(mixCol, 1.0);
	}
	else if (colTrans1.a > 0.0) {
		fragColor = colTrans1;
	}
	else {
		fragColor = colTrans2;		// if alpha is zero it will have no effect anyway
	}
}
