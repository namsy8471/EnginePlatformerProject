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

layout(set = 0, binding = 1) uniform sampler2D baseColorTexture;
layout(set = 0, binding = 2) uniform sampler2D normalTexture;
layout(set = 0, binding = 3) uniform sampler2D metallicTexture;
layout(set = 0, binding = 4) uniform sampler2D roughnessTexture;
layout(set = 0, binding = 5) uniform sampler2D metallicRoughnessTexture;
layout(set = 0, binding = 6) uniform sampler2D aoTexture;
layout(set = 0, binding = 8) uniform sampler2D opacityTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inWorldPosition;
layout(location = 3) in vec3 inNormalWorld;
layout(location = 4) in vec3 inTangentWorld;
layout(location = 5) in float inTangentSign;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec4 outWorldPosition;

vec3 resolveNormal()
{
	vec3 normal = inNormalWorld;
	if (dot(normal, normal) < 0.000001)
	{
		normal = vec3(0.0, 1.0, 0.0);
	}
	normal = normalize(normal);

	if (cameraConstants.MaterialTextureFlags.y > 0.5)
	{
		vec3 tangent = inTangentWorld;
		if (dot(tangent, tangent) < 0.000001)
		{
			tangent = vec3(1.0, 0.0, 0.0);
		}
		tangent = normalize(tangent - normal * dot(tangent, normal));
		const vec3 bitangent = normalize(cross(normal, tangent)) * (inTangentSign < 0.0 ? -1.0 : 1.0);
		vec3 sampledNormal = texture(normalTexture, inTexCoord).xyz * 2.0 - 1.0;
		if (cameraConstants.ExposureDebug.z > 0.5)
		{
			sampledNormal.y = -sampledNormal.y;
		}
		normal = normalize(sampledNormal.x * tangent + sampledNormal.y * bitangent + sampledNormal.z * normal);
	}

	return normal;
}

void main()
{
	const bool useVertexColor = cameraConstants.MaterialTextureFlags2.w > 0.5;
	const vec4 vertexColor = useVertexColor ? inColor : vec4(1.0, 1.0, 1.0, inColor.a);
	vec4 baseColor = texture(baseColorTexture, inTexCoord) * vertexColor * cameraConstants.MaterialBaseColor;
	if (cameraConstants.MaterialTextureFlags2.x > 0.5)
	{
		baseColor.a *= texture(opacityTexture, inTexCoord).r;
	}
	if (baseColor.a < 0.1)
	{
		discard;
	}

	float metallic = clamp(cameraConstants.MaterialEmissiveMetallic.w, 0.0, 1.0);
	float roughness = clamp(cameraConstants.MaterialRoughnessFlags.x, 0.02, 1.0);
	if (cameraConstants.MaterialRoughnessFlags.z > 0.5)
	{
		const vec4 mr = texture(metallicRoughnessTexture, inTexCoord);
		roughness = clamp(mr.g, 0.02, 1.0);
		metallic = clamp(mr.b, 0.0, 1.0);
	}
	if (cameraConstants.MaterialRoughnessFlags.w > 0.5)
	{
		metallic = clamp(texture(metallicTexture, inTexCoord).r, 0.0, 1.0);
	}
	if (cameraConstants.MaterialTextureFlags.x > 0.5)
	{
		roughness = clamp(texture(roughnessTexture, inTexCoord).r, 0.02, 1.0);
	}
	const float ao = cameraConstants.MaterialTextureFlags.z > 0.5 ? texture(aoTexture, inTexCoord).r : 1.0;

	outAlbedo = baseColor;
	outNormal = vec4(resolveNormal(), cameraConstants.MaterialSpecularShininess.w);
	outMaterial = vec4(roughness, metallic, cameraConstants.MaterialRoughnessFlags.y, ao);
	outWorldPosition = vec4(inWorldPosition, 1.0);
}
