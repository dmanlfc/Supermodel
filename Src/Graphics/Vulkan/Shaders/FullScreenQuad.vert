// Supermodel - vertexless full screen quad. Draw four vertices as a triangle
// strip with no vertex buffer bound.
//
// No texture coordinates: the paired fragment shaders index the source with
// gl_FragCoord, which is orientation neutral.

#version 450 core

void main(void)
{
	const vec4 vertices[] = vec4[](vec4(-1.0, -1.0, 0.0, 1.0),
									vec4(-1.0,  1.0, 0.0, 1.0),
									vec4( 1.0, -1.0, 0.0, 1.0),
									vec4( 1.0,  1.0, 0.0, 1.0));

	gl_Position = vertices[gl_VertexIndex % 4];
}
