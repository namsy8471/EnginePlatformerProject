#version 450

#define DEFERRED_LIGHT_TILE_SIZE 32u

layout(set = 0, binding = 0) uniform DeferredLightingConstants
{
	vec4 CameraPosition;
	vec4 AmbientColorIntensity;
	vec4 ExposureDebug;
	vec4 LightCountParams;
	vec4 ScreenSize;
	mat4 ShadowViewProjection;
	vec4 ShadowParams;
	vec4 ShadowDirection;
	vec4 SkyCameraRightTanX;
	vec4 SkyCameraUpTanY;
	vec4 SkyCameraForwardEnabled;
	vec4 SkyZenithColorIntensity;
	vec4 SkyHorizonColorBlend;
	vec4 SkyGroundColorHorizon;
	vec4 SkySunDirectionSize;
	vec4 SkySunColorIntensity;
} lightingConstants;

struct GpuLightData
{
	vec4 PositionType;
	vec4 DirectionRange;
	vec4 ColorIntensity;
	vec4 SpotAnglesEnabled;
};

struct TileLightRange
{
	uint Offset;
	uint Count;
	uint Padding0;
	uint Padding1;
};

layout(set = 0, binding = 1) uniform sampler2D gBufferAlbedo;
layout(set = 0, binding = 2) uniform sampler2D gBufferNormal;
layout(set = 0, binding = 3) uniform sampler2D gBufferMaterial;
layout(set = 0, binding = 4) uniform sampler2D gBufferWorldPosition;
layout(set = 0, binding = 5) uniform sampler2DShadow shadowMap;
layout(std430, set = 0, binding = 10) readonly buffer DeferredLightBuffer
{
	GpuLightData deferredLights[];
};
layout(std430, set = 0, binding = 11) readonly buffer DeferredTileLightRangeBuffer
{
	TileLightRange deferredTileLightRanges[];
};
layout(std430, set = 0, binding = 12) readonly buffer DeferredTileLightIndexBuffer
{
	uint deferredTileLightIndices[];
};

layout(location = 0) out vec4 outColor;

vec3 tonemapAces(vec3 color)
{
	color = max(color, vec3(0.0));
	return clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), vec3(0.0), vec3(1.0));
}

vec4 applyToneMapping(vec4 color)
{
	return color;
}

TileLightRange getPixelTileLightRange(vec4 pixelPosition)
{
	const uint totalLightCount = uint(lightingConstants.LightCountParams.x);
	if (lightingConstants.LightCountParams.w < 0.5 || totalLightCount == 0u)
	{
		TileLightRange range;
		range.Offset = 0u;
		range.Count = totalLightCount;
		range.Padding0 = 0u;
		range.Padding1 = 0u;
		return range;
	}

	const uint tileCountX = max(uint(lightingConstants.LightCountParams.y), 1u);
	const uint tileCountY = max(uint(lightingConstants.LightCountParams.z), 1u);
	const uint tileX = min(uint(pixelPosition.x / float(DEFERRED_LIGHT_TILE_SIZE)), tileCountX - 1u);
	const uint tileY = min(uint(pixelPosition.y / float(DEFERRED_LIGHT_TILE_SIZE)), tileCountY - 1u);
	return deferredTileLightRanges[tileY * tileCountX + tileX];
}

uint getDeferredLightIndex(uint iterationIndex, TileLightRange range)
{
	return lightingConstants.LightCountParams.w > 0.5
		? deferredTileLightIndices[range.Offset + iterationIndex]
		: iterationIndex;
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

float computeDirectionalShadowVisibility(vec3 worldPosition, GpuLightData light)
{
	if (lightingConstants.ShadowParams.x < 0.5 || light.PositionType.w > 0.5)
	{
		return 1.0;
	}

	const float directionMatch = dot(normalize(light.DirectionRange.xyz), normalize(lightingConstants.ShadowDirection.xyz));
	if (directionMatch < 0.92)
	{
		return 1.0;
	}

	const vec4 shadowClip = lightingConstants.ShadowViewProjection * vec4(worldPosition, 1.0);
	if (shadowClip.w <= 0.0001)
	{
		return 1.0;
	}

	const vec3 shadowNdc = shadowClip.xyz / shadowClip.w;
	const vec2 shadowUv = shadowNdc.xy * vec2(0.5, -0.5) + vec2(0.5);
	if (shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0 || shadowNdc.z <= 0.0 || shadowNdc.z >= 1.0)
	{
		return 1.0;
	}

	const vec2 texelSize = vec2(1.0) / vec2(max(textureSize(shadowMap, 0), ivec2(1)));
	const float compareDepth = shadowNdc.z - lightingConstants.ShadowParams.y;
	float visibility = 0.0;
	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			visibility += texture(shadowMap, vec3(shadowUv + vec2(x, y) * texelSize, compareDepth));
		}
	}
	visibility /= 9.0;
	return mix(1.0, visibility, clamp(lightingConstants.ShadowParams.w, 0.0, 1.0));
}

float computeSceneShadowDebugValue(vec3 worldPosition)
{
	const uint lightCount = uint(lightingConstants.LightCountParams.x);
	for (uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
	{
		const GpuLightData light = deferredLights[lightIndex];
		if (light.PositionType.w <= 0.5)
		{
			return computeDirectionalShadowVisibility(worldPosition, light);
		}
	}
	return 1.0;
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

vec3 applyPbr(vec3 baseColor, vec3 normal, vec3 worldPosition, float roughness, float metallic, float ao, vec4 pixelPosition)
{
	vec3 viewDirection = lightingConstants.CameraPosition.xyz - worldPosition;
	viewDirection = dot(viewDirection, viewDirection) > 0.000001 ? normalize(viewDirection) : vec3(0.0, 0.0, 1.0);

	const vec3 f0 = mix(vec3(0.04), baseColor, metallic);
	const float nDotV = max(dot(normal, viewDirection), 0.0);
	vec3 directLighting = vec3(0.0);
	const TileLightRange tileRange = getPixelTileLightRange(pixelPosition);
	const uint lightCount = min(tileRange.Count, uint(lightingConstants.LightCountParams.x));
	for (uint iterationIndex = 0u; iterationIndex < lightCount; ++iterationIndex)
	{
		const uint lightIndex = getDeferredLightIndex(iterationIndex, tileRange);
		const GpuLightData light = deferredLights[lightIndex];
		vec3 lightDirection;
		vec3 lightColor;
		if (!resolveLightData(light, worldPosition, lightDirection, lightColor))
		{
			continue;
		}

		const float shadowVisibility = computeDirectionalShadowVisibility(worldPosition, light);
		const vec3 halfVector = normalize(viewDirection + lightDirection);
		const float nDotL = max(dot(normal, lightDirection), 0.0);
		const float ndf = distributionGGX(normal, halfVector, roughness);
		const float geometry = geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
		const vec3 fresnel = fresnelSchlick(max(dot(halfVector, viewDirection), 0.0), f0);
		const vec3 specular = (ndf * geometry * fresnel) / max(4.0 * nDotV * nDotL, 0.0001);
		const vec3 kd = (1.0 - fresnel) * (1.0 - metallic);
		directLighting += (kd * baseColor / 3.14159265 + specular) * lightColor * nDotL * shadowVisibility;
	}

	const vec3 ambient = baseColor * lightingConstants.AmbientColorIntensity.rgb * lightingConstants.AmbientColorIntensity.w * ao;
	return directLighting + ambient;
}

vec3 applyPhong(vec3 baseColor, vec3 normal, vec3 worldPosition, float shininess, float ao, vec4 pixelPosition)
{
	vec3 viewDirection = lightingConstants.CameraPosition.xyz - worldPosition;
	viewDirection = dot(viewDirection, viewDirection) > 0.000001 ? normalize(viewDirection) : vec3(0.0, 0.0, 1.0);

	vec3 diffuseLighting = vec3(0.0);
	vec3 specularLighting = vec3(0.0);
	const TileLightRange tileRange = getPixelTileLightRange(pixelPosition);
	const uint lightCount = min(tileRange.Count, uint(lightingConstants.LightCountParams.x));
	for (uint iterationIndex = 0u; iterationIndex < lightCount; ++iterationIndex)
	{
		const uint lightIndex = getDeferredLightIndex(iterationIndex, tileRange);
		const GpuLightData light = deferredLights[lightIndex];
		vec3 lightDirection;
		vec3 lightColor;
		if (!resolveLightData(light, worldPosition, lightDirection, lightColor))
		{
			continue;
		}

		const float shadowVisibility = computeDirectionalShadowVisibility(worldPosition, light);
		const float diffuse = max(dot(normal, lightDirection), 0.0);
		const vec3 reflectedLight = reflect(-lightDirection, normal);
		const float specular = pow(max(dot(viewDirection, reflectedLight), 0.0), max(shininess, 1.0)) * 0.35;
		diffuseLighting += diffuse * lightColor * shadowVisibility;
		specularLighting += specular * lightColor * shadowVisibility;
	}

	const vec3 ambient = lightingConstants.AmbientColorIntensity.rgb * lightingConstants.AmbientColorIntensity.w * ao;
	return baseColor * (ambient + diffuseLighting) + specularLighting;
}

vec3 buildSkyDirection(vec2 texCoord)
{
	const vec2 ndc = vec2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
	return normalize(
		lightingConstants.SkyCameraForwardEnabled.xyz +
		lightingConstants.SkyCameraRightTanX.xyz * ndc.x * lightingConstants.SkyCameraRightTanX.w +
		lightingConstants.SkyCameraUpTanY.xyz * ndc.y * lightingConstants.SkyCameraUpTanY.w);
}

vec3 evaluateSky(vec2 texCoord)
{
	if (lightingConstants.SkyCameraForwardEnabled.w < 0.5)
	{
		return vec3(0.025, 0.027, 0.032);
	}

	const vec3 direction = buildSkyDirection(texCoord);
	const float horizonHeight = clamp(lightingConstants.SkyGroundColorHorizon.w, -0.95, 0.95);
	const float blend = max(lightingConstants.SkyHorizonColorBlend.w, 0.05);
	const float height = direction.y;
	const float topFactor = clamp((height - horizonHeight) / max(1.0 - horizonHeight, 0.001), 0.0, 1.0);
	const float bottomFactor = clamp((horizonHeight - height) / max(1.0 + horizonHeight, 0.001), 0.0, 1.0);
	vec3 color = mix(lightingConstants.SkyHorizonColorBlend.rgb, lightingConstants.SkyZenithColorIntensity.rgb, pow(topFactor, blend));
	color = mix(color, lightingConstants.SkyGroundColorHorizon.rgb, pow(bottomFactor, blend));
	color *= lightingConstants.SkyZenithColorIntensity.w;

	const vec3 sunDirection = normalize(lightingConstants.SkySunDirectionSize.xyz);
	const float sunDot = clamp(dot(direction, sunDirection), 0.0, 1.0);
	const float sunSize = clamp(lightingConstants.SkySunDirectionSize.w, 0.001, 0.35);
	const float sunCore = smoothstep(cos(sunSize * 1.45), cos(sunSize), sunDot);
	const float sunGlow = pow(sunDot, max(2.0, 0.18 / sunSize));
	color += lightingConstants.SkySunColorIntensity.rgb * lightingConstants.SkySunColorIntensity.w * (sunCore + sunGlow * 0.18);
	return color;
}

void main()
{
	const vec2 texCoord = gl_FragCoord.xy * lightingConstants.ScreenSize.zw;
	const vec4 albedo = texture(gBufferAlbedo, texCoord);
	if (albedo.a < 0.001)
	{
		outColor = vec4(evaluateSky(texCoord), 1.0);
		return;
	}

	const vec4 normalPacked = texture(gBufferNormal, texCoord);
	const vec4 material = texture(gBufferMaterial, texCoord);
	const vec3 worldPosition = texture(gBufferWorldPosition, texCoord).xyz;
	const vec3 normal = normalize(normalPacked.xyz);
	const float roughness = clamp(material.x, 0.02, 1.0);
	const float metallic = clamp(material.y, 0.0, 1.0);
	const float shadingModel = material.z;
	const float ao = material.w;
	const uint debugView = uint(lightingConstants.ExposureDebug.y + 0.5);

	if (debugView == 1u) { outColor = applyToneMapping(vec4(albedo.rgb, albedo.a)); return; }
	if (debugView == 2u) { outColor = applyToneMapping(vec4(normal * 0.5 + 0.5, albedo.a)); return; }
	if (debugView == 3u) { outColor = applyToneMapping(vec4(vec3(metallic), albedo.a)); return; }
	if (debugView == 4u) { outColor = applyToneMapping(vec4(vec3(roughness), albedo.a)); return; }
	if (debugView == 5u) { outColor = applyToneMapping(vec4(vec3(ao), albedo.a)); return; }
	if (debugView == 7u) { outColor = applyToneMapping(vec4(lightingConstants.AmbientColorIntensity.rgb * lightingConstants.AmbientColorIntensity.w, albedo.a)); return; }
	if (debugView == 9u)
	{
		const float shadow = computeSceneShadowDebugValue(worldPosition);
		outColor = applyToneMapping(vec4(vec3(shadow), albedo.a));
		return;
	}
	if (debugView == 10u)
	{
		const TileLightRange tileRange = getPixelTileLightRange(gl_FragCoord);
		const float normalizedCount = clamp(float(tileRange.Count) / 16.0, 0.0, 1.0);
		outColor = applyToneMapping(vec4(normalizedCount, normalizedCount * normalizedCount, 1.0 - normalizedCount, albedo.a));
		return;
	}

	const vec3 litColor = shadingModel > 0.5
		? applyPbr(albedo.rgb, normal, worldPosition, roughness, metallic, ao, gl_FragCoord)
		: applyPhong(albedo.rgb, normal, worldPosition, normalPacked.a, ao, gl_FragCoord);
	outColor = applyToneMapping(vec4(litColor, albedo.a));
}
