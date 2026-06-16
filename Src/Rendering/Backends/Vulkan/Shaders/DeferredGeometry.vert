#version 450

#define MAX_FORWARD_LIGHTS 8

layout(set = 0, binding = 0) uniform CameraConstants
{
	mat4 WorldViewProjection;
	mat4 ViewProjection;
	mat4 World;
	mat4 WorldInverseTranspose;
	vec4 CameraPosition;
	vec4 BenchmarkParams;
	vec4 LightDirection;
	vec4 LightColorIntensity;
	vec4 AmbientSpecular;
	vec4 MaterialBaseColor;
	vec4 MaterialSpecularShininess;
	vec4 MaterialEmissiveMetallic;
	vec4 MaterialRoughnessFlags;
	vec4 MaterialTextureFlags;
	vec4 MaterialTextureFlags2;
	vec4 AmbientColorIntensity;
	vec4 ExposureDebug;
	vec4 LightCountParams;
	vec4 LightPositionType[MAX_FORWARD_LIGHTS];
	vec4 LightDirectionRange[MAX_FORWARD_LIGHTS];
	vec4 LightColorIntensityData[MAX_FORWARD_LIGHTS];
	vec4 LightSpotAnglesEnabled[MAX_FORWARD_LIGHTS];
	mat4 ShadowViewProjection;
	vec4 ShadowParams;
	vec4 ShadowDirection;
} cameraConstants;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec3 inTangent;
layout(location = 5) in float inTangentSign;

layout(location = 0) out vec2 outTexCoord;
layout(location = 1) out vec4 outColor;
layout(location = 2) out vec3 outWorldPosition;
layout(location = 3) out vec3 outNormalWorld;
layout(location = 4) out vec3 outTangentWorld;
layout(location = 5) out float outTangentSign;

void main()
{
	const vec4 worldPosition = cameraConstants.World * vec4(inPosition, 1.0);
	gl_Position = cameraConstants.WorldViewProjection * vec4(inPosition, 1.0);
	outTexCoord = inTexCoord;
	outColor = inColor;
	outWorldPosition = worldPosition.xyz;
	outNormalWorld = normalize((cameraConstants.WorldInverseTranspose * vec4(inNormal, 0.0)).xyz);
	outTangentWorld = normalize((cameraConstants.WorldInverseTranspose * vec4(inTangent, 0.0)).xyz);
	outTangentSign = inTangentSign == 0.0 ? 1.0 : inTangentSign;
}
