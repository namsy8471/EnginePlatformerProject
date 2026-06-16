// DirectX12 deferred fullscreen lighting pass.

#define DEFERRED_LIGHT_TILE_SIZE 32u

cbuffer DeferredLightingConstants : register(b0)
{
    float4 CameraPosition;
    float4 AmbientColorIntensity;
    float4 ExposureDebug;
    float4 LightCountParams;
    float4 ScreenSize;
    row_major float4x4 ShadowViewProjection;
    float4 ShadowParams;
    float4 ShadowDirection;
};

struct GpuLightData
{
    float4 PositionType;
    float4 DirectionRange;
    float4 ColorIntensity;
    float4 SpotAnglesEnabled;
};

struct TileLightRange
{
    uint Offset;
    uint Count;
    uint Padding0;
    uint Padding1;
};

Texture2D GBufferAlbedo : register(t0);
Texture2D GBufferNormal : register(t1);
Texture2D GBufferMaterial : register(t2);
Texture2D GBufferWorldPosition : register(t3);
Texture2D<float> ShadowMap : register(t4);
StructuredBuffer<GpuLightData> DeferredLightBuffer : register(t10);
StructuredBuffer<TileLightRange> DeferredTileLightRanges : register(t11);
StructuredBuffer<uint> DeferredTileLightIndices : register(t12);
SamplerState GBufferSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

struct VSOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2(-1.0f, 3.0f),
        float2(3.0f, -1.0f)
    };
    output.Position = float4(positions[vertexId], 0.0f, 1.0f);
    output.TexCoord = output.Position.xy * float2(0.5f, -0.5f) + 0.5f;
    return output;
}

float3 TonemapAces(float3 color)
{
    color = max(color, float3(0.0f, 0.0f, 0.0f));
    return saturate((color * (2.51f * color + 0.03f)) / (color * (2.43f * color + 0.59f) + 0.14f));
}

float4 ApplyToneMapping(float4 color)
{
    // DX12 deferred now writes linear HDR into an intermediate target.
    // The dedicated ToneMap pass applies exposure, ACES, and gamma.
    return color;
}

TileLightRange GetPixelTileLightRange(float4 pixelPosition)
{
    const uint totalLightCount = (uint)LightCountParams.x;
    if (LightCountParams.w < 0.5f || totalLightCount == 0u)
    {
        TileLightRange range;
        range.Offset = 0u;
        range.Count = totalLightCount;
        range.Padding0 = 0u;
        range.Padding1 = 0u;
        return range;
    }

    const uint tileCountX = max((uint)LightCountParams.y, 1u);
    const uint tileCountY = max((uint)LightCountParams.z, 1u);
    const uint tileX = min((uint)(pixelPosition.x / (float)DEFERRED_LIGHT_TILE_SIZE), tileCountX - 1u);
    const uint tileY = min((uint)(pixelPosition.y / (float)DEFERRED_LIGHT_TILE_SIZE), tileCountY - 1u);
    return DeferredTileLightRanges[tileY * tileCountX + tileX];
}

uint GetDeferredLightIndex(uint iterationIndex, TileLightRange range)
{
    return LightCountParams.w > 0.5f
        ? DeferredTileLightIndices[range.Offset + iterationIndex]
        : iterationIndex;
}

bool ResolveLightData(GpuLightData light, float3 worldPosition, out float3 lightDirection, out float3 lightColor)
{
    const float4 positionType = light.PositionType;
    const float4 directionRange = light.DirectionRange;
    const float4 colorIntensity = light.ColorIntensity;
    const float4 spotEnabled = light.SpotAnglesEnabled;
    if (spotEnabled.z < 0.5f || colorIntensity.w <= 0.0f)
    {
        lightDirection = float3(0.0f, 1.0f, 0.0f);
        lightColor = float3(0.0f, 0.0f, 0.0f);
        return false;
    }

    const float type = positionType.w;
    float attenuation = 1.0f;
    if (type < 0.5f)
    {
        lightDirection = normalize(directionRange.xyz);
    }
    else
    {
        const float3 toLight = positionType.xyz - worldPosition;
        const float distanceToLight = length(toLight);
        lightDirection = distanceToLight > 0.0001f ? toLight / distanceToLight : float3(0.0f, 1.0f, 0.0f);
        const float range = max(directionRange.w, 0.001f);
        const float rangeFactor = saturate(1.0f - distanceToLight / range);
        attenuation = rangeFactor * rangeFactor;

        if (type > 1.5f)
        {
            const float3 spotDirection = normalize(directionRange.xyz);
            const float cosTheta = dot(normalize(-lightDirection), spotDirection);
            const float cone = saturate((cosTheta - spotEnabled.y) / max(spotEnabled.x - spotEnabled.y, 0.0001f));
            attenuation *= cone * cone;
        }
    }

    lightColor = max(colorIntensity.rgb * colorIntensity.w * attenuation, float3(0.0f, 0.0f, 0.0f));
    return dot(lightColor, lightColor) > 0.000001f;
}

float ComputeDirectionalShadowVisibility(float3 worldPosition, GpuLightData light)
{
    if (ShadowParams.x < 0.5f || light.PositionType.w > 0.5f)
    {
        return 1.0f;
    }

    const float directionMatch = dot(normalize(light.DirectionRange.xyz), normalize(ShadowDirection.xyz));
    if (directionMatch < 0.92f)
    {
        return 1.0f;
    }

    const float4 shadowClip = mul(float4(worldPosition, 1.0f), ShadowViewProjection);
    if (shadowClip.w <= 0.0001f)
    {
        return 1.0f;
    }

    const float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    const float2 shadowUv = shadowNdc.xy * float2(0.5f, -0.5f) + 0.5f;
    if (shadowUv.x < 0.0f || shadowUv.x > 1.0f || shadowUv.y < 0.0f || shadowUv.y > 1.0f || shadowNdc.z <= 0.0f || shadowNdc.z >= 1.0f)
    {
        return 1.0f;
    }

    uint width = 1u;
    uint height = 1u;
    ShadowMap.GetDimensions(width, height);
    const float2 texelSize = 1.0f / float2(max(width, 1u), max(height, 1u));
    const float compareDepth = shadowNdc.z - ShadowParams.y;

    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            visibility += ShadowMap.SampleCmpLevelZero(ShadowSampler, shadowUv + float2(x, y) * texelSize, compareDepth);
        }
    }
    visibility /= 9.0f;
    return lerp(1.0f, visibility, saturate(ShadowParams.w));
}

float ComputeSceneShadowDebugValue(float3 worldPosition)
{
    const uint lightCount = (uint)LightCountParams.x;
    [loop]
    for (uint lightIndex = 0; lightIndex < lightCount; ++lightIndex)
    {
        const GpuLightData light = DeferredLightBuffer[lightIndex];
        if (light.PositionType.w <= 0.5f)
        {
            return ComputeDirectionalShadowVisibility(worldPosition, light);
        }
    }
    return 1.0f;
}

float DistributionGGX(float3 normal, float3 halfVector, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float nDotH = max(dot(normal, halfVector), 0.0f);
    const float nDotH2 = nDotH * nDotH;
    const float denom = nDotH2 * (a2 - 1.0f) + 1.0f;
    return a2 / max(3.14159265f * denom * denom, 0.000001f);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
    const float r = roughness + 1.0f;
    const float k = (r * r) / 8.0f;
    return nDotV / max(nDotV * (1.0f - k) + k, 0.000001f);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - cosTheta, 5.0f);
}

float3 ApplyPbr(float3 baseColor, float3 normal, float3 worldPosition, float roughness, float metallic, float ao, float4 pixelPosition)
{
    float3 viewDirection = CameraPosition.xyz - worldPosition;
    viewDirection = dot(viewDirection, viewDirection) > 0.000001f ? normalize(viewDirection) : float3(0.0f, 0.0f, 1.0f);

    const float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
    const float nDotV = max(dot(normal, viewDirection), 0.0f);
    float3 directLighting = float3(0.0f, 0.0f, 0.0f);
    const TileLightRange tileRange = GetPixelTileLightRange(pixelPosition);
    const uint lightCount = min(tileRange.Count, (uint)LightCountParams.x);
    [loop]
    for (uint iterationIndex = 0; iterationIndex < lightCount; ++iterationIndex)
    {
        const uint lightIndex = GetDeferredLightIndex(iterationIndex, tileRange);
        const GpuLightData light = DeferredLightBuffer[lightIndex];
        float3 lightDirection;
        float3 lightColor;
        if (!ResolveLightData(light, worldPosition, lightDirection, lightColor))
        {
            continue;
        }

        const float shadowVisibility = ComputeDirectionalShadowVisibility(worldPosition, light);
        const float3 halfVector = normalize(viewDirection + lightDirection);
        const float nDotL = max(dot(normal, lightDirection), 0.0f);
        const float ndf = DistributionGGX(normal, halfVector, roughness);
        const float geometry = GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
        const float3 fresnel = FresnelSchlick(max(dot(halfVector, viewDirection), 0.0f), f0);
        const float3 specular = (ndf * geometry * fresnel) / max(4.0f * nDotV * nDotL, 0.0001f);
        const float3 kd = (1.0f - fresnel) * (1.0f - metallic);
        directLighting += (kd * baseColor / 3.14159265f + specular) * lightColor * nDotL * shadowVisibility;
    }

    const float3 ambient = baseColor * AmbientColorIntensity.rgb * AmbientColorIntensity.w * ao;
    return directLighting + ambient;
}

float3 ApplyPhong(float3 baseColor, float3 normal, float3 worldPosition, float shininess, float ao, float4 pixelPosition)
{
    float3 viewDirection = CameraPosition.xyz - worldPosition;
    viewDirection = dot(viewDirection, viewDirection) > 0.000001f ? normalize(viewDirection) : float3(0.0f, 0.0f, 1.0f);

    float3 diffuseLighting = float3(0.0f, 0.0f, 0.0f);
    float3 specularLighting = float3(0.0f, 0.0f, 0.0f);
    const TileLightRange tileRange = GetPixelTileLightRange(pixelPosition);
    const uint lightCount = min(tileRange.Count, (uint)LightCountParams.x);
    [loop]
    for (uint iterationIndex = 0; iterationIndex < lightCount; ++iterationIndex)
    {
        const uint lightIndex = GetDeferredLightIndex(iterationIndex, tileRange);
        const GpuLightData light = DeferredLightBuffer[lightIndex];
        float3 lightDirection;
        float3 lightColor;
        if (!ResolveLightData(light, worldPosition, lightDirection, lightColor))
        {
            continue;
        }

        const float shadowVisibility = ComputeDirectionalShadowVisibility(worldPosition, light);
        const float diffuse = max(dot(normal, lightDirection), 0.0f);
        const float3 reflectedLight = reflect(-lightDirection, normal);
        const float specular = pow(max(dot(viewDirection, reflectedLight), 0.0f), max(shininess, 1.0f)) * 0.35f;
        diffuseLighting += diffuse * lightColor * shadowVisibility;
        specularLighting += specular * lightColor * shadowVisibility;
    }

    const float3 ambient = AmbientColorIntensity.rgb * AmbientColorIntensity.w * ao;
    return baseColor * (ambient + diffuseLighting) + specularLighting;
}

float4 PSMain(VSOutput input) : SV_Target
{
    const float2 texCoord = input.Position.xy * ScreenSize.zw;
    const float4 albedo = GBufferAlbedo.Sample(GBufferSampler, texCoord);
    clip(albedo.a - 0.001f);

    const float4 normalPacked = GBufferNormal.Sample(GBufferSampler, texCoord);
    const float4 material = GBufferMaterial.Sample(GBufferSampler, texCoord);
    const float3 worldPosition = GBufferWorldPosition.Sample(GBufferSampler, texCoord).xyz;
    const float3 normal = normalize(normalPacked.xyz);
    const float roughness = clamp(material.x, 0.02f, 1.0f);
    const float metallic = saturate(material.y);
    const float shadingModel = material.z;
    const float ao = material.w;
    const uint debugView = (uint)(ExposureDebug.y + 0.5f);

    if (debugView == 1u) return ApplyToneMapping(float4(albedo.rgb, albedo.a));
    if (debugView == 2u) return ApplyToneMapping(float4(normal * 0.5f + 0.5f, albedo.a));
    if (debugView == 3u) return ApplyToneMapping(float4(metallic, metallic, metallic, albedo.a));
    if (debugView == 4u) return ApplyToneMapping(float4(roughness, roughness, roughness, albedo.a));
    if (debugView == 5u) return ApplyToneMapping(float4(ao, ao, ao, albedo.a));
    if (debugView == 7u) return ApplyToneMapping(float4(AmbientColorIntensity.rgb * AmbientColorIntensity.w, albedo.a));
    if (debugView == 9u)
    {
        const float shadow = ComputeSceneShadowDebugValue(worldPosition);
        return ApplyToneMapping(float4(shadow, shadow, shadow, albedo.a));
    }
    if (debugView == 10u)
    {
        const TileLightRange tileRange = GetPixelTileLightRange(input.Position);
        const float normalizedCount = saturate((float)tileRange.Count / 16.0f);
        return ApplyToneMapping(float4(normalizedCount, normalizedCount * normalizedCount, 1.0f - normalizedCount, albedo.a));
    }

    float3 litColor = shadingModel > 0.5f
        ? ApplyPbr(albedo.rgb, normal, worldPosition, roughness, metallic, ao, input.Position)
        : ApplyPhong(albedo.rgb, normal, worldPosition, normalPacked.a, ao, input.Position);
    return ApplyToneMapping(float4(litColor, albedo.a));
}
