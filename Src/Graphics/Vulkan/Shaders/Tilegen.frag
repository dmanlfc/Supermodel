// Supermodel - s_fragmentShader equivalent from Src/Graphics/Shaders2D.h.
// Pair with Tilegen.vert.
//
// UPSCALEMODE is a compile-time define, so the Makefile builds this file four
// times: 0 Nearest, 1 Biquintic, 2 Bilinear, 3 Bicubic. Modes 0 and 2 share the
// same body and differ only in the sampler filter, which
// VulkanShaderInterface.h::TilegenSamplerFilter picks.

#version 450 core

#ifndef UPSCALEMODE
#error "UPSCALEMODE must be defined to 0 (nearest), 1 (biquintic), 2 (bilinear) or 3 (bicubic)"
#endif

// inputs
layout(set = 0, binding = 0) uniform sampler2D tex1;			// texture
layout(location = 0) in vec2 fsTexCoord;

// outputs
layout(location = 0) out vec4 fragColor;

#if (UPSCALEMODE == 1)
// smoothstep instead of linear, remapping to a bilinear lookup
vec4 biquintic(sampler2D samp, vec2 uv)
{
	vec2 ts = vec2(textureSize(samp,0));
	uv = uv*ts + 0.5;

	vec2 i = floor(uv);
	vec2 f = uv - i;
	f = f*f*f*(f*(f*6.0-15.0)+10.0) - 0.5;

	uv = (i + f)/ts;
	return texture(samp, uv);
}
#endif

#if (UPSCALEMODE == 3)
// 4x4 bicubic filter using 4 bilinear texture lookups
vec4 cubic(float v) {
	vec4 n = vec4(1.0, 2.0, 3.0, 4.0) - v;
	vec4 s = n * n * n;
	float x = s.x;
	float y = s.y - 4.0 * s.x;
	float z = s.z - 4.0 * s.y + 6.0 * s.x;
	float w = 6.0 - x - y - z;
	return vec4(x, y, z, w) * (1.0 / 6.0);
}
vec4 bicubic(sampler2D samp, vec2 uv) {
	vec2 invTexSize = 1.0 / vec2(textureSize(samp,0));
	uv = uv * vec2(textureSize(samp,0)) - 0.5;
	vec2 uvi = floor(uv);
	vec2 fxy = uv-uvi;
	vec4 xcubic = cubic(fxy.x);
	vec4 ycubic = cubic(fxy.y);
	vec4 c = uvi.xxyy + vec2(-0.5, 1.5).xyxy;
	vec4 s = vec4(xcubic.xz + xcubic.yw, ycubic.xz + ycubic.yw);
	vec4 offset = c + vec4(xcubic.yw, ycubic.yw) / s;
	offset *= invTexSize.xxyy;
	vec4 sample0 = texture(samp, offset.xz);
	vec4 sample1 = texture(samp, offset.yz);
	vec4 sample2 = texture(samp, offset.xw);
	vec4 sample3 = texture(samp, offset.yw);
	float sx = s.x / (s.x + s.y);
	float sy = s.z / (s.z + s.w);
	return mix(mix(sample3, sample2, sx), mix(sample1, sample0, sx), sy);
}
#endif

void main()
{
	#if (UPSCALEMODE == 3)
	fragColor = bicubic(tex1, fsTexCoord);
	#elif (UPSCALEMODE == 1)
	fragColor = vec4(biquintic(tex1, fsTexCoord).rgb, texture(tex1, fsTexCoord).a);
	#else
	fragColor = texture(tex1, fsTexCoord);
	#endif
}
