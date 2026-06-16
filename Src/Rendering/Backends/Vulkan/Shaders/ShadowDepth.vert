#version 450

layout(set = 0, binding = 0) uniform CameraConstants
{
	mat4 WorldViewProjection;
} cameraConstants;

layout(location = 0) in vec3 inPosition;

void main()
{
	gl_Position = cameraConstants.WorldViewProjection * vec4(inPosition, 1.0);
}
