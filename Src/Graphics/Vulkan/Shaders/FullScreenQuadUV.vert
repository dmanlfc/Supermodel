// Supermodel - FullScreenQuad.vert plus the texture coordinates the scaled
// composite variants need.
//
// The texture coordinates are flipped vertically relative to the GL original:
// with Vulkan's upper-left origin and the negative-height viewport, source row 0
// is the top row, so without the flip the image is drawn upside down.
// Tilegen.vert flips its table for the same reason.

#version 450 core

layout(location = 0) out vec2 texCoord;

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

	gl_Position = vertices[gl_VertexIndex % 4];
	texCoord = texCoords[gl_VertexIndex % 4];
}
