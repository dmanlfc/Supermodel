// Supermodel - fragmentShaderFog equivalent from R3DScrollFog.cpp. Pair with
// FullScreenQuad.vert.
//
// This shader declares no LOS output, so the scroll fog pipeline must set
// colorWriteMask = 0 for attachment 3, or Vulkan writes undefined values into it.

#version 450 core

layout(std140, set = 0, binding = 0) uniform ScrollFogUniforms
{
	layout(offset =  0) vec4  fogColour;            // .rgb colour, .a density
	layout(offset = 16) vec4  spotEllipse;          // .xy centre, .zw half extents
	layout(offset = 32) vec4  spotFogColor;         // .rgb used
	layout(offset = 48) vec2  fragCoordYTransform;  // see ScreenFragCoord() below
	layout(offset = 56) float fogAttenuation;
	layout(offset = 60) float fogAmbient;
};

// Spotlight on fog
float	ellipse;
vec2	position, size;
vec3	lSpotFogColor;

// Scroll fog
vec3	lFogColor;
vec4	scrollFog;

// outputs
layout(location = 0) out vec4 out0;		// opaque
layout(location = 1) out vec4 out1;		// trans layer 1
layout(location = 2) out vec4 out2;		// trans layer 2

// spotEllipse.xy is in GL's lower-left convention, so fragCoordYTransform flips
// it back against Vulkan's upper-left-origin gl_FragCoord. See
// R3DShaderCommon.glsl.
vec2 ScreenFragCoord()
{
	return vec2(gl_FragCoord.x, fragCoordYTransform.y + fragCoordYTransform.x * gl_FragCoord.y);
}

void WriteOutputs(vec4 colour)
{
	vec4 blank = vec4(0.0);

	out0 = colour;
	out1 = blank;
	out2 = blank;
}

void main()
{
	// Scroll fog base color
	lFogColor = fogColour.rgb * fogAmbient;

	// Spotlight on fog (area)
	position = spotEllipse.xy;
	size = spotEllipse.zw;
	ellipse = length((ScreenFragCoord() - position) / size);
	ellipse = ellipse * ellipse;			// decay rate = square of distance from center
	ellipse = 1.0 - ellipse;				// invert
	ellipse = max(0.0, ellipse);			// clamp

	// Spotlight on fog (color)
	lSpotFogColor = mix(vec3(0.0), spotFogColor.rgb * ellipse * fogColour.rgb, fogAttenuation);

	// Scroll fog density
	scrollFog = vec4(lFogColor + lSpotFogColor, fogColour.a);

	// Final Color
	WriteOutputs(scrollFog);
}
