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

struct GpuLightData
{
	vec4 PositionType;
	vec4 DirectionRange;
	vec4 ColorIntensity;
	vec4 SpotAnglesEnabled;
};

layout(std430, set = 0, binding = 11) readonly buffer DeferredLightBuffer
{
	GpuLightData deferredLights[];
};

layout(set = 0, binding = 1) uniform sampler2D baseColorTexture;
layout(set = 0, binding = 2) uniform sampler2D normalTexture;
layout(set = 0, binding = 3) uniform sampler2D metallicTexture;
layout(set = 0, binding = 4) uniform sampler2D roughnessTexture;
layout(set = 0, binding = 5) uniform sampler2D metallicRoughnessTexture;
layout(set = 0, binding = 6) uniform sampler2D aoTexture;
layout(set = 0, binding = 7) uniform sampler2D emissiveTexture;
layout(set = 0, binding = 8) uniform sampler2D opacityTexture;
layout(set = 0, binding = 9) uniform sampler2D specularTexture;
layout(set = 0, binding = 10) uniform sampler2D shininessTexture;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inWorldPosition;
layout(location = 3) in vec3 inNormalWorld;
layout(location = 4) in vec3 inTangentWorld;
layout(location = 5) in float inTangentSign;
layout(location = 0) out vec4 outColor;

vec3 tonemapAces(vec3 color)
{
	color = max(color, vec3(0.0));
	return clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), vec3(0.0), vec3(1.0));
}

vec4 applyToneMapping(vec4 color)
{
	color.rgb = tonemapAces(color.rgb * max(cameraConstants.ExposureDebug.x, 0.0));
	color.rgb = pow(color.rgb, vec3(1.0 / 2.2));
	return color;
}

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

float getAo()
{
	return cameraConstants.MaterialTextureFlags.z > 0.5 ? texture(aoTexture, inTexCoord).r : 1.0;
}

vec3 getEmissive()
{
	return cameraConstants.MaterialEmissiveMetallic.rgb
		+ (cameraConstants.MaterialTextureFlags.w > 0.5 ? texture(emissiveTexture, inTexCoord).rgb : vec3(0.0));
}

bool resolveLightData(GpuLightData light, vec3 worldPosition, out vec3 lightDirection, out vec3 lightColor)
{
	const vec4 positionType = light.PositionType;
	const vec4 directionRange = light.DirectionRange;
	const vec4 colorIntensity = light.ColorIntensity;
	const vec4 spotEnabled = light.SpotAnglesEnabled;
	if (spotEnabled.z < 0.5 || colorIntensity.w <= 0.0)
	{
		lightDirection = vec3(0.0, 1.0, 0.0);
		lightColor = vec3(0.0);
		return false;
	}

	const float type = positionType.w;
	float attenuation = 1.0;
	if (type < 0.5)
	{
		lightDirection = normalize(directionRange.xyz);
	}
	else
	{
		const vec3 toLight = positionType.xyz - worldPosition;
		const float distanceToLight = length(toLight);
		lightDirection = distanceToLight > 0.0001 ? toLight / distanceToLight : vec3(0.0, 1.0, 0.0);
		const float range = max(directionRange.w, 0.001);
		const float rangeFactor = clamp(1.0 - distanceToLight / range, 0.0, 1.0);
		attenuation = rangeFactor * rangeFactor;

		if (type > 1.5)
		{
			const vec3 spotDirection = normalize(directionRange.xyz);
			const float cosTheta = dot(normalize(-lightDirection), spotDirection);
			const float cone = clamp((cosTheta - spotEnabled.y) / max(spotEnabled.x - spotEnabled.y, 0.0001), 0.0, 1.0);
			attenuation *= cone * cone;
		}
	}

	lightColor = max(colorIntensity.rgb * colorIntensity.w * attenuation, vec3(0.0));
	return dot(lightColor, lightColor) > 0.000001;
}

GpuLightData getActiveLight(uint index)
{
	if (cameraConstants.LightCountParams.w > 0.5)
	{
		return deferredLights[index];
	}

	GpuLightData light;
	light.PositionType = cameraConstants.LightPositionType[index];
	light.DirectionRange = cameraConstants.LightDirectionRange[index];
	light.ColorIntensity = cameraConstants.LightColorIntensityData[index];
	light.SpotAnglesEnabled = cameraConstants.LightSpotAnglesEnabled[index];
	return light;
}

uint getActiveLightCount()
{
	return cameraConstants.LightCountParams.w > 0.5
		? uint(cameraConstants.LightCountParams.z)
		: min(uint(cameraConstants.LightCountParams.x), uint(MAX_FORWARD_LIGHTS));
}

float distributionGGX(vec3 normal, vec3 halfVector, float roughness)
{
	const float a = roughness * roughness;
	const float a2 = a * a;
	const float nDotH = max(dot(normal, halfVector), 0.0);
	const float nDotH2 = nDotH * nDotH;
	const float denom = nDotH2 * (a2 - 1.0) + 1.0;
	return a2 / max(3.14159265 * denom * denom, 0.000001);
}

float geometrySchlickGGX(float nDotV, float roughness)
{
	const float r = roughness + 1.0;
	const float k = (r * r) / 8.0;
	return nDotV / max(nDotV * (1.0 - k) + k, 0.000001);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0)
{
	return f0 + (1.0 - f0) * pow(1.0 - cosTheta, 5.0);
}

vec4 applyPhongLighting(vec4 baseColor)
{
	if (cameraConstants.BenchmarkParams.x > 0.5 || cameraConstants.AmbientSpecular.w < 0.5)
	{
		return baseColor;
	}

	const vec3 normal = resolveNormal();
	vec3 viewDirection = cameraConstants.CameraPosition.xyz - inWorldPosition;
	viewDirection = dot(viewDirection, viewDirection) > 0.000001 ? normalize(viewDirection) : vec3(0.0, 0.0, 1.0);

	vec3 specularColor = cameraConstants.MaterialSpecularShininess.rgb;
	if (cameraConstants.MaterialTextureFlags2.y > 0.5)
	{
		specularColor *= texture(specularTexture, inTexCoord).rgb;
	}
	float specularPower = max(cameraConstants.MaterialSpecularShininess.w, 1.0);
	if (cameraConstants.MaterialTextureFlags2.z > 0.5)
	{
		specularPower = max(texture(shininessTexture, inTexCoord).r * 256.0, 1.0);
	}

	vec3 diffuseLighting = vec3(0.0);
	vec3 specularLighting = vec3(0.0);
	const uint lightCount = getActiveLightCount();
	for (uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
	{
		vec3 lightDirection;
		vec3 lightColor;
		if (!resolveLightData(getActiveLight(lightIndex), inWorldPosition, lightDirection, lightColor))
		{
			continue;
		}
		const float diffuse = max(dot(normal, lightDirection), 0.0);
		const vec3 reflectedLight = reflect(-lightDirection, normal);
		const float specular = pow(max(dot(viewDirection, reflectedLight), 0.0), specularPower) * max(cameraConstants.AmbientSpecular.y, 0.0);
		diffuseLighting += diffuse * lightColor;
		specularLighting += specular * specularColor * lightColor;
	}

	const float ao = getAo();
	const vec3 ambient = cameraConstants.AmbientColorIntensity.rgb * cameraConstants.AmbientColorIntensity.w * ao;
	const vec3 litColor = baseColor.rgb * (ambient + diffuseLighting) + specularLighting + getEmissive();
	return vec4(litColor, baseColor.a);
}

vec4 applyPbrLighting(vec4 baseColor)
{
	if (cameraConstants.BenchmarkParams.x > 0.5 || cameraConstants.AmbientSpecular.w < 0.5)
	{
		return baseColor;
	}

	const vec3 normal = resolveNormal();
	vec3 viewDirection = cameraConstants.CameraPosition.xyz - inWorldPosition;
	viewDirection = dot(viewDirection, viewDirection) > 0.000001 ? normalize(viewDirection) : vec3(0.0, 0.0, 1.0);

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

	const vec3 f0 = mix(vec3(0.04), baseColor.rgb, metallic);
	const float nDotV = max(dot(normal, viewDirection), 0.0);
	vec3 directLighting = vec3(0.0);
	const uint lightCount = getActiveLightCount();
	for (uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
	{
		vec3 lightDirection;
		vec3 lightColor;
		if (!resolveLightData(getActiveLight(lightIndex), inWorldPosition, lightDirection, lightColor))
		{
			continue;
		}

		const vec3 halfVector = normalize(viewDirection + lightDirection);
		const float nDotL = max(dot(normal, lightDirection), 0.0);
		const float ndf = distributionGGX(normal, halfVector, roughness);
		const float geometry = geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
		const vec3 fresnel = fresnelSchlick(max(dot(halfVector, viewDirection), 0.0), f0);
		const vec3 specular = (ndf * geometry * fresnel) / max(4.0 * nDotV * nDotL, 0.0001);
		const vec3 kd = (1.0 - fresnel) * (1.0 - metallic);
		directLighting += (kd * baseColor.rgb / 3.14159265 + specular) * lightColor * nDotL;
	}

	const float ao = getAo();
	const vec3 ambient = baseColor.rgb * cameraConstants.AmbientColorIntensity.rgb * cameraConstants.AmbientColorIntensity.w * ao;
	return vec4(directLighting + ambient + getEmissive(), baseColor.a);
}

void main()
{
	const bool useVertexColor = cameraConstants.BenchmarkParams.x > 0.5 || cameraConstants.MaterialTextureFlags2.w > 0.5;
	const vec4 vertexColor = useVertexColor ? inColor : vec4(1.0, 1.0, 1.0, inColor.a);
	vec4 baseColor = vertexColor * cameraConstants.MaterialBaseColor;
	if (inTexCoord.x >= 0.0)
	{
		baseColor = texture(baseColorTexture, inTexCoord) * vertexColor * cameraConstants.MaterialBaseColor;
	}

	if (cameraConstants.MaterialTextureFlags2.x > 0.5)
	{
		baseColor.a *= texture(opacityTexture, inTexCoord).r;
	}
	if (baseColor.a < 0.1)
	{
		discard;
	}

	const uint debugView = uint(cameraConstants.ExposureDebug.y + 0.5);
	if (debugView == 1u) { outColor = applyToneMapping(vec4(baseColor.rgb, baseColor.a)); return; }
	if (debugView == 2u) { outColor = applyToneMapping(vec4(resolveNormal() * 0.5 + 0.5, baseColor.a)); return; }
	if (debugView == 3u) { outColor = applyToneMapping(vec4(cameraConstants.MaterialRoughnessFlags.w > 0.5 ? texture(metallicTexture, inTexCoord).rrr : cameraConstants.MaterialEmissiveMetallic.www, baseColor.a)); return; }
	if (debugView == 4u) { outColor = applyToneMapping(vec4(cameraConstants.MaterialTextureFlags.x > 0.5 ? texture(roughnessTexture, inTexCoord).rrr : cameraConstants.MaterialRoughnessFlags.xxx, baseColor.a)); return; }
	if (debugView == 5u) { outColor = applyToneMapping(vec4(vec3(getAo()), baseColor.a)); return; }
	if (debugView == 6u) { outColor = applyToneMapping(vec4(getEmissive(), baseColor.a)); return; }
	if (debugView == 8u) { outColor = applyToneMapping(inColor); return; }

	vec4 litColor;
	if (cameraConstants.MaterialRoughnessFlags.y > 1.5)
	{
		litColor = baseColor;
	}
	else if (cameraConstants.MaterialRoughnessFlags.y > 0.5)
	{
		litColor = applyPbrLighting(debugView == 7u ? vec4(1.0, 1.0, 1.0, baseColor.a) : baseColor);
	}
	else
	{
		litColor = applyPhongLighting(debugView == 7u ? vec4(1.0, 1.0, 1.0, baseColor.a) : baseColor);
	}
	outColor = applyToneMapping(litColor);
}
