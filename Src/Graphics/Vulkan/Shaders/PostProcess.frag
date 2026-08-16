// Supermodel - SuperAA resolve and CRT colour shader, from Src/Graphics/SuperAA.cpp.
// Pair with FullScreenQuad.vert.
//
// The supersampling factor and CRT colour mode were baked into the GL shader
// source; here they are uniforms, since they are chosen once at init.
//
// `finalColor *= colmatrix` keeps GL's row-vector-times-matrix ordering, and the
// mat3 literals are column-major as they were in GL.
//
// CRTcolor values match Src/Supermodel.h:
//   0 None, 1 ARI, 2 PVM, 3 BT601JP, 4 BT601US, 5 BT601EA

#version 450 core

layout(std140, set = 0, binding = 0) uniform PostProcessUniforms
{
	layout(offset = 0) int aa;			// supersampling factor, GL `const int aa`
	layout(offset = 4) int crtColorMode;	// CRTcolor, GL `CRTCOLORS`
};

// inputs
layout(set = 0, binding = 1) uniform sampler2D tex1;			// base tex

// outputs
layout(location = 0) out vec4 fragColor;

// see https://www.retrorgb.com/colour-malarkey.html
// and accompanying matrices https://github.com/danmons/colour_matrix_adaptations/tree/main/csv

// ARI(D93/9300K) (JP) -> sRGB(D65/6500K) // what can be found in some of the sega or corresponding CRT manuals of the time is a range of ~9700K-10600K, so even cooler than D93
const mat3 colmatrixARI = mat3(1.3272128334714093,
								-0.3802108879412366,
								-0.1003607696463202,
								-0.0241848600268417,
								0.9544506228550125,
								0.0807358585208939,
								-0.0239585810555497,
								-0.0409736005706461,
								1.4076563728858242);

// PVM_20M2U(D93/9300K) (JP) -> sRGB(D65/6500K), gamma 2.25
const mat3 colmatrixPVM = mat3(0.9241392201737613,
								-0.0690701363506526,
								-0.0084279079392561,
								0.0231962420140721,
								0.9729221778325590,
								0.0148832015024337,
								-0.0054875731998806,
								0.0098220960740544,
								1.3383896683854550);

// BT601_525_D93 (JP) -> sRGB(D65/6500K), gamma sRGB
const mat3 colmatrixBT601JP = mat3(0.7822490684754086,
								0.0506168576073398,
								0.0137752498011040,
								0.0147968947158740,
								0.9741745313046067,
								0.0220301953285839,
								-0.0013501205469208,
								-0.0044076726925543,
								1.3484819844991036);

// BT601_525 (US) -> sRGB(D65/6500K), gamma sRGB // extremely subtle
const mat3 colmatrixBT601US = mat3(0.9395420637732393,
								0.0501813568598678,
								0.0102765793668928,
								0.0177722231435608,
								0.9657928624969044,
								0.0164349143595347,
								-0.0016215999431855,
								-0.0043697496597356,
								1.0059913496029214);

// BT601_625 (EUR,AUS) -> sRGB(D65/6500K), gamma sRGB // barely noticeable at all
const mat3 colmatrixBT601EA = mat3(1.0440432087628346,
								-0.0440432087628348,
								0.0000000000000001,
								0.0000000000000000,
								1.0000000000000002,
								0.0000000000000000,
								0.0000000000000000,
								0.0117933782840052,
								0.9882066217159947);

// Matches the GL `cgamma` constant selection. -1.0 means "input is sRGB
// encoded", which routes through invsRGB() instead of a plain power function.
float CRTGamma(int mode)
{
	if (mode == 1) {
		return 2.5;		// what is the 'real' gamma/EOTF of a japanese arcade CRT of that time? 2.4 or 2.5 is what the net so far agrees on
	}
	if (mode == 2) {
		return 2.25;
	}
	return -1.0;
}

mat3 CRTMatrix(int mode)
{
	if (mode == 1) { return colmatrixARI; }
	if (mode == 2) { return colmatrixPVM; }
	if (mode == 3) { return colmatrixBT601JP; }
	if (mode == 4) { return colmatrixBT601US; }
	return colmatrixBT601EA;
}

vec3 GetTextureValue(sampler2D s)
{
	ivec2 texPos	= ivec2(gl_FragCoord.xy /*-vec2(0.5)*/) * aa;

	// GL baked this factor in as `const int aa`, so with no supersampling the loop
	// below disappeared at compile time. Here it is a uniform and so cannot be
	// unrolled, leaving real per-pixel loop overhead in the common case. This branch
	// is uniform across the draw, so it is close to free, and one fetch scaled by
	// 1.0 is bit for bit what the loop produces when aa is 1.
	if (aa == 1) {
		return texelFetch(s, texPos, 0).rgb;
	}

	vec3 texColour	= vec3(0.0);

	for(int i=0; i < aa; i++) {
		for(int j=0; j < aa; j++) {
			texColour += texelFetch(s,ivec2(texPos.x+i,texPos.y+j),0).rgb;
		}
	}

	texColour *= 1.0/float(aa * aa);

	return texColour;
}

float sRGB(float color)
{
	if (color <= 0.0031308)
		return 12.92 * color;
	else
		return 1.055 * pow(color, 1.0/2.4) - 0.055;
}

float invsRGB(float color)
{
	if (color <= 0.04045) // 0.03928 ?
		return color * (1.0/12.92);
	else
		return pow(color * (1.0/1.055) + (0.055/1.055), 2.4);
}

void main()
{
	vec3 finalColor = GetTextureValue(tex1);

	if (crtColorMode != 0)
	{
		float cgamma = CRTGamma(crtColorMode);
		mat3 colmatrix = CRTMatrix(crtColorMode);

		// transform input space to linear space
		if (cgamma != -1.0)
			finalColor = pow(finalColor, vec3(cgamma));
		else
			finalColor = vec3(invsRGB(finalColor.r),invsRGB(finalColor.g),invsRGB(finalColor.b));
		// do color transformation
		finalColor *= colmatrix;
		// transform from linear to sRGB output space
		finalColor = vec3(sRGB(finalColor.r),sRGB(finalColor.g),sRGB(finalColor.b));
	}

	fragColor = vec4(finalColor, 1.0);
}
