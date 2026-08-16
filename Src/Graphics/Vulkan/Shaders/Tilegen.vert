// Supermodel - s_vertexShader equivalent from Src/Graphics/Shaders2D.h.
//
// The texture coordinate table is deliberately flipped vertically, compensating
// for the tilegen surface being uploaded top row first.

#version 450 core

// outputs
layout(location = 0) out vec2 fsTexCoord;

void main(void)
{
	const vec4 vertices[] = vec4[](vec4(-1.0, -1.0, 0.0, 1.0),
									vec4(-1.0,  1.0, 0.0, 1.0),
									vec4( 1.0, -1.0, 0.0, 1.0),
									vec4( 1.0,  1.0, 0.0, 1.0));

	const vec2 texCoords[] = vec2[](vec2(0.0, 1.0), // flip upside down
									vec2(0.0, 0.0),
									vec2(1.0, 1.0),
									vec2(1.0, 0.0));

	fsTexCoord = texCoords[gl_VertexIndex % 4];
	gl_Position = vertices[gl_VertexIndex % 4];
}
