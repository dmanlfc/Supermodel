// Supermodel - geometryShaderR3DQuads equivalent from R3DShaderQuads.h.
//
// `noperspective` is used unconditionally, so unlike the GL build there is no
// GLES fallback pre-multiplying v and area by clip W. The pipeline requires
// VkPhysicalDeviceFeatures::geometryShader; the renderer must fall back to the
// triangle pipeline where it is absent.
//
// This stage reads no uniforms, so it deliberately declares neither the
// R3DUniforms block nor the push constant block, keeping
// VK_SHADER_STAGE_GEOMETRY_BIT off the descriptor set layout.

#version 450 core

layout (lines_adjacency) in;
layout (triangle_strip, max_vertices = 4) out;

layout(location = 0) in VS_OUT
{
	vec3	viewVertex;
	vec3	viewNormal;		// per vertex normal vector
	vec2	texCoord;
	vec4	color;
	float	fixedShade;
	float	discardPoly;	// can't have varying bool (glsl spec)
	float	LODBase;
} gs_in[4];

layout(location = 0) out GS_OUT
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
} gs_out;

//a*b - c*d, computed in a stable fashion (Kahan)
float DifferenceOfProducts(float a, float b, float c, float d)
{
    precise float cd = c * d;
    precise float err = fma(-c, d, cd);
    precise float dop = fma(a, b, -cd);
    return dop + err;
}

void main(void)
{
	if(gs_in[0].discardPoly > 0.0) {
		return;					//emulate back face culling here (all vertices in poly have same value)
	}

	vec2 v[4];

	for (int i=0; i<4; i++) {
		float oneOverW		= 1.0 / gl_in[i].gl_Position.w;
		gs_out.oneOverW[i]	= oneOverW;
		v[i]				= gl_in[i].gl_Position.xy * oneOverW;

		// our regular vertex attribs
		gs_out.viewVertex[i]	= gs_in[i].viewVertex * oneOverW;
		gs_out.viewNormal[i]	= gs_in[i].viewNormal * oneOverW;
		gs_out.texCoord[i]		= gs_in[i].texCoord   * oneOverW;
		gs_out.fixedShade[i]	= gs_in[i].fixedShade * oneOverW;
	}

	// flat attributes
	gs_out.color = gs_in[0].color;
	gs_out.LODBase = gs_in[0].LODBase;

	// precompute crossproducts for all vertex combinations to be looked up in loop below for area computation
	precise float cross[4][4];
	for (int i=0; i<4; i++)
	{
		cross[i][i] = 0.0;
		for (int j=i+1; j<4; j++)
			cross[i][j] = DifferenceOfProducts(gl_in[i].gl_Position.x, gl_in[j].gl_Position.y, gl_in[j].gl_Position.x, gl_in[i].gl_Position.y) / (gl_in[i].gl_Position.w * gl_in[j].gl_Position.w);
	}
	for (int i=1; i<4; i++)
		for (int j=0; j<i; j++)
			cross[i][j] = -cross[j][i];

	for (int i=0; i<4; i++) {
		// Remaps lines-adjacency quad vertex order to triangle strip order.
		const int reorder[4] = int[4]( 1, 0, 2, 3 );
		int ii = reorder[i];

		for (int j=0; j<4; j++) {
			vec2 screenV = v[j] - v[ii];
			int j_next = (j+1) % 4;
			// Compute area via the shoelace algorithm, divided by W afterwards
			// to improve precision, and use Kahan's algorithm for the 2D crossproducts.
			float screenArea = cross[j][j_next] + cross[j_next][ii] + cross[ii][j];
			gs_out.v[j] = screenV;
			gs_out.area[j] = screenArea;
		}

		const vec3 bary[4] = vec3[](
			vec3(1.0, 0.0, 0.0),
			vec3(0.0, 1.0, 0.0),
			vec3(0.0, 0.0, 1.0),
			vec3(0.0,-1.0, 0.0)		// make the last coordinate negative so we can tell the difference between the triangles
		);

		gs_out.barycentricCoords = bary[ii];

		gl_Position = gl_in[ii].gl_Position;

		EmitVertex();
	}
}
