// Supermodel - fragmentShaderR3D equivalent from R3DShaderTriangles.h.
//
// Deviation from GL: the LOS attachment, previously GLES-only, is always present.
//
// gl_FragDepth uses the reversed-Z formula, which disables early depth testing as
// it did in GL. layout(depth_any) is kept rather than depth_greater because the
// written value is not monotonic in the interpolated one.

#version 450 core

#extension GL_GOOGLE_include_directive : require

#include "R3DShaderUniforms.glsl"

// interpolated inputs from vertex shader
layout(location = 0) in vec3  fsViewVertex;
layout(location = 1) in vec3  fsViewNormal;		// per vertex normal vector
layout(location = 2) in vec2  fsTexCoord;
layout(location = 3) in vec4  fsColor;
layout(location = 4) in float fsFixedShade;
layout(location = 5) in float fsDiscardPoly;
layout(location = 6) in float fsLODBase;

// outputs
layout(location = 0) out vec4  out0;		// opaque
layout(location = 1) out vec4  out1;		// trans layer 1
layout(location = 2) out vec4  out2;		// trans layer 2
layout(location = 3) out uvec2 outLos;		// line-of-sight depth bits and no-return flag

#include "R3DShaderCommon.glsl"

void main()
{
	vec4 tex1Data;
	vec4 colData;
	vec4 finalData;
	vec4 fogData;

	if(fsDiscardPoly > 0.0) {
		discard;		//emulate back face culling here
	}

	gl_FragDepth = EncodeDepth(projMat[3][2] * gl_FragCoord.w);

	fogData = vec4(fogColour.rgb * fogAmbient, CalcFog());
	tex1Data = vec4(1.0, 1.0, 1.0, 1.0);

	if(textureEnabled) {
		tex1Data = GetTextureValue();
	}

	colData = fsColor;
	Step15Luminous(colData);			// no-op for step 2.0+
	finalData = tex1Data * colData;

	if (finalData.a < (1.0/32.0)) {		// basically chuck out any totally transparent pixels value = 1/16 the smallest transparency level h/w supports
		discard;
	}

	float lobeEffect;
	float lobeFogEffect;
	ComputeSpotlight(spotEllipse, spotRange, fsViewVertex.z, lobeEffect, lobeFogEffect);

	if (lightEnabled) {
		vec3   lightIntensity;
		vec3   sunVector;     // sun lighting vector (as reflecting away from vertex)
		float  sunFactor;     // sun light projection along vertex normal (0.0 to 1.0)

		// Sun angle
		sunVector = lighting[0].xyz;

		// Compute diffuse factor for sunlight
		if(fixedShading) {
			sunFactor = fsFixedShade;
		}
		else {
			sunFactor = dot(sunVector, fsViewNormal);
		}

		// Clamp ceil, fix for upscaled models without "modelScale" defined
		sunFactor = clamp(sunFactor,-1.0,1.0);

		// Optional clamping, value is allowed to be negative
		// We suspect that translucent polygons are always clamped (e.g. lasers in Daytona 2)
		if(sunClamp || finalData.a<0.999) {
			sunFactor = max(sunFactor,0.0);
		}

		// Total light intensity: sum of all components
		lightIntensity = vec3(sunFactor*lighting[1].x + lighting[1].y);   // diffuse + ambient

		lightIntensity += spotColor.rgb*lobeEffect;

		// Upper clamp is optional, step 1.5+ games will drive brightness beyond 100%
		if(intensityClamp) {
			lightIntensity = min(lightIntensity,1.0);
		}

		finalData.rgb *= lightIntensity;

		// for now assume fixed shading doesn't work with specular
		if (specularEnabled) {

			float specularFactor;

			if (smoothShading)
			{
				// Always clamp floor to zero
				float NdotL = max(0.0, sunFactor);

				const float expIndex[4]  = float[4](8.0, 16.0, 32.0, 64.0);
				const float multIndex[4] = float[4](1.6, 1.6, 2.4, 3.2);
				float exponent = expIndex[int(shininess)];

				specularFactor = pow(NdotL, exponent);
				specularFactor *= multIndex[int(shininess)];
			}
			else
			{
				// flat shaded polys use Phong reflection model (R dot V) without any exponent or multiplier
				// V = (0.0, 0.0, 1.0) is used by Model 3 as a fast approximation, so R dot V = R.z
				vec3 R = reflect(-sunVector, fsViewNormal);
				specularFactor = max(0.0, R.z);
			}

			specularFactor *= specularValue;
			specularFactor *= lighting[1].x;

			if (colData.a < 1.0) {
				/// Specular hi-light affects translucent polygons alpha channel ///
				finalData.a = max(finalData.a, specularFactor);
			}

			finalData.rgb += specularFactor;
		}
	}

	// Final clamp: we need it for proper shading in dimmed light and dark ambients
	finalData.rgb = min(finalData.rgb, vec3(1.0));

	// Spotlight on fog
	vec3 lSpotFogColor = spotFogColor.rgb * fogAttenuation * fogColour.rgb * lobeFogEffect;

	// Fog & spotlight applied
	finalData.rgb = mix(finalData.rgb, fogData.rgb + lSpotFogColor, fogData.a);

	// Write outputs to colour buffers
	WriteOutputs(finalData,colourLayer);

	outLos = uvec2(floatBitsToUint(gLinearDepth), noLosReturn ? 1u : 0u);
}
