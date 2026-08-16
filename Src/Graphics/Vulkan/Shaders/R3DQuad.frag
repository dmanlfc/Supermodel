// Supermodel - fragmentShaderR3DQuads equivalent from R3DShaderQuads.h.
//
// As in R3DQuad.geom the `noperspective` interpolants are used directly, so the
// GLES clip-W recovery path is not needed. See R3DTriangle.frag for the rest.

#version 450 core

#extension GL_GOOGLE_include_directive : require

#include "R3DShaderUniforms.glsl"

//interpolated inputs from geometry shader
layout(location = 0) in GS_OUT
{
	noperspective vec2 v[4];
	noperspective float area[4];
	flat float oneOverW[4];

	//our regular attributes
	flat vec3	viewVertex[4];
	flat vec3	viewNormal[4];		// per vertex normal vector
	flat vec2	texCoord[4];
	flat vec4	color;
	flat float	fixedShade[4];
	flat float	LODBase;
	vec3 barycentricCoords;
} fs_in;

//our calculated vertex attributes from the above
vec3	fsViewVertex;
vec3	fsViewNormal;
vec2	fsTexCoord;
vec4	fsColor;
float	fsFixedShade;
float	fsLODBase;

//outputs
layout(location = 0) out vec4  out0;		// opaque
layout(location = 1) out vec4  out1;		// trans layer 1
layout(location = 2) out vec4  out2;		// trans layer 2
layout(location = 3) out uvec2 outLos;		// line-of-sight depth bits and no-return flag

#include "R3DShaderCommon.glsl"

void QuadraticInterpolation()
{
	// init interpolated vertex attributes
	fsViewVertex	= vec3(0.0);
	fsViewNormal	= vec3(0.0);
	fsTexCoord		= vec2(0.0);
	fsFixedShade	= 0.0;

	float interp_oneOverW	= 0.0;
	float uSum				= 0.0;

	// Vulkan GLSL provides noperspective interpolation directly, so the
	// interpolants can be used as-is.
	vec2 screenV[4];
	float screenArea[4];
	for (int i=0; i<4; i++) {
		screenV[i] = fs_in.v[i];
		screenArea[i] = fs_in.area[i];
	}

	const float hpFMin		= 0.00006103515625;		// half precision FLT_MIN
	bool extreme_edge		= (fs_in.barycentricCoords.x <= hpFMin || fs_in.barycentricCoords.z <= hpFMin);

	// As the coordinates get closer to the poly edge the interpolated areas can approach zero which can blow up the maths leading to bad interpolation.
	// In this case we fall back to 3 point triangle interpolation (really 2 because 1 coord will be zero).
	// This code will only get triggered in rarer corner cases

	float u[4];
	if(extreme_edge)
	{
		for (int i=0; i<4; i++) {
			int b = (i == 3) ? 1  // 2nd triangle
			                 : i; // 1st triangle
			float baryCoord	= abs(fs_in.barycentricCoords[b]);
			u[i] = baryCoord / fs_in.oneOverW[i];
			if((i == 1 && !(fs_in.barycentricCoords.y>0.0)) // ignore u[1] if 2nd triangle
			 ||(i == 3 &&  (fs_in.barycentricCoords.y>0.0)) // ignore u[3] if 1st triangle
			 ||(baryCoord <= hpFMin))
			{
				u[i] = 0.0;
			}
		}
	}
	else
	{
		for (int i=0; i<4; i++) {
			u[i] = length(screenV[i]) * sign(fs_in.oneOverW[i]); // is w[i] negative?
		}

		precise float t[4];
		for (int i=0; i<4; i++) {
			int i_next = (i+1)%4;
			if(screenArea[i]==0.0) t[i] = 0.0; // check for zero area to avoid div by zero
			else                   t[i] = fma(u[i],u[i_next], -dot(screenV[i],screenV[i_next])) / screenArea[i];
		}

		int lambdaSignCount = 0; // to discard fragments if all the weights are neither all negative nor all positive (=outside the convex/concave/crossed quad).

		for (uint i=0u; i<4u; i++) {
			uint i_prev = (i-1u)%4u;
			u[i] = (t[i_prev] + t[i]) / u[i];
			lambdaSignCount += (t[i_prev] < -t[i]) ? -1 : 1;
		}

		if (lambdaSignCount == 0) { // one can either check for == 0 or abs(...) != 4, both should(!) be equivalent (but in practice its not due to precision issues, but these cases are extremely rare)
			if(!gl_HelperInvocation) {
				discard;
			}
		}

		for (int i=0; i<4; i++) {
			interp_oneOverW	= fma(fs_in.oneOverW[i], u[i], interp_oneOverW);
			uSum           += u[i];
		}
	}

	// shared computations
	for (int i=0; i<4; i++) {
		fsViewVertex	= fma(fs_in.viewVertex[i], vec3(u[i]), fsViewVertex);
		fsViewNormal	= fma(fs_in.viewNormal[i], vec3(u[i]), fsViewNormal);
		fsTexCoord		= fma(fs_in.texCoord[i], vec2(u[i]), fsTexCoord);
		fsFixedShade	= fma(fs_in.fixedShade[i], u[i], fsFixedShade);
	}

	// finalize the 2 cases
	if(extreme_edge)
	{
		float w = (projMat * vec4(fsViewVertex,1.0)).w;

		interp_oneOverW = 1.0 / w;
		uSum = 1.0;
	}
	else
	{
		float inv = 1.0/interp_oneOverW;
		fsViewVertex *= inv;
		fsViewNormal *= inv;
		fsTexCoord   *= inv;
		fsFixedShade *= inv;
	}

	vec4 vertex;
	float depth;

	// dirty hack for co-planar polys that really need 100% identical values to depth test correctly
	// the reason we waste cycles and calculate depth value here is because we have run out of vertex attribs
	if(fs_in.oneOverW[0]==fs_in.oneOverW[1] &&
	   fs_in.oneOverW[1]==fs_in.oneOverW[2] &&
	   fs_in.oneOverW[2]==fs_in.oneOverW[3]) {

		fsViewVertex.z	= fs_in.viewVertex[0].z / fs_in.oneOverW[0];
		vertex			= projMat * vec4(fsViewVertex,1.0);
		depth			= vertex.z / vertex.w;
	}
	else {
		vertex.z		= projMat[2][2] * fsViewVertex.z + projMat[3][2];		// standard projMat * vertex - but just using Z components
		depth			= vertex.z * (interp_oneOverW/uSum);
	}

	fsColor			= fs_in.color;
	fsLODBase		= fs_in.LODBase;

	gl_FragDepth = EncodeDepth(depth);
}

void main()
{
	vec4 tex1Data;
	vec4 colData;
	vec4 finalData;
	vec4 fogData;

	QuadraticInterpolation();	// calculate our vertex attributes

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
