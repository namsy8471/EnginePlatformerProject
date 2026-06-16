// DirectX12 static mesh shader

#define MAX_FORWARD_LIGHTS 8

cbuffer CameraConstants : register(b0)
{
    row_major float4x4 WorldViewProjection;
    row_major float4x4 ViewProjection;
    row_major float4x4 World;
    row_major float4x4 WorldInverseTranspose;
    float4 CameraPosition;
    float4 BenchmarkParams; // x: instance count, y: local scale, z: fovY, w: aspect
    float4 LightDirection; // legacy first light direction
    float4 LightColorIntensity; // legacy first light color/intensity
    float4 AmbientSpecular; // legacy: x ambient, y specular strength, z exposure, w lighting enabled
    float4 MaterialBaseColor;
    float4 MaterialSpecularShininess;
    float4 MaterialEmissiveMetallic;
    float4 MaterialRoughnessFlags; // x roughness, y shading model, z has MR, w has metallic
    float4 MaterialTextureFlags; // x roughness, y normal, z AO, w emissive
    float4 MaterialTextureFlags2; // x opacity, y specular, z shininess, w use vertex color
    float4 AmbientColorIntensity; // rgb ambient color, w intensity
    float4 ExposureDebug; // x exposure, y debug view, z normal Y flip, w unused
    float4 LightCountParams; // x forward capped count, z deferred active count, w use deferred light list
    float4 LightPositionType[MAX_FORWARD_LIGHTS]; // xyz position, w type: 0 dir, 1 point, 2 spot
    float4 LightDirectionRange[MAX_FORWARD_LIGHTS]; // xyz dir, w range
    float4 LightColorIntensityData[MAX_FORWARD_LIGHTS]; // rgb color, w intensity
    float4 LightSpotAnglesEnabled[MAX_FORWARD_LIGHTS]; // x inner cos, y outer cos, z enabled
    row_major float4x4 ShadowViewProjection;
    float4 ShadowParams; // x enabled, y depth bias, z normal bias, w strength
    float4 ShadowDirection; // xyz direction to light
};

struct GpuLightData
{
    float4 PositionType;
    float4 DirectionRange;
    float4 ColorIntensity;
    float4 SpotAnglesEnabled;
};

Texture2D BaseColorTexture : register(t0);
Texture2D NormalTexture : register(t1);
Texture2D MetallicTexture : register(t2);
Texture2D RoughnessTexture : register(t3);
Texture2D MetallicRoughnessTexture : register(t4);
Texture2D AOTexture : register(t5);
Texture2D EmissiveTexture : register(t6);
Texture2D OpacityTexture : register(t7);
Texture2D SpecularTexture : register(t8);
Texture2D ShininessTexture : register(t9);
StructuredBuffer<GpuLightData> DeferredLightBuffer : register(t10);
SamplerState MaterialSampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Color : COLOR0;
    float3 Tangent : TANGENT;
    float TangentSign : TANGENTSIGN;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
    float4 Color : COLOR0;
    float3 WorldPosition : TEXCOORD1;
    float3 NormalWorld : TEXCOORD2;
    float3 TangentWorld : TEXCOORD3;
    float TangentSign : TEXCOORD4;
};

float Hash01(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return (float)(value & 0x00ffffffu) / 16777215.0f;
}

float3 MakeBenchmarkCenter(uint instanceId)
{
    const uint objectCount = max((uint)BenchmarkParams.x, 1u);
    const float aspect = max(BenchmarkParams.w, 0.1f);
    const uint columnCount = max((uint)ceil(sqrt((float)objectCount * aspect)), 1u);
    const uint rowCount = max((objectCount + columnCount - 1u) / columnCount, 1u);
    const uint row = instanceId / columnCount;
    const uint column = instanceId - row * columnCount;

    const float jitterX = lerp(0.12f, 0.88f, Hash01(instanceId * 1664525u + 1013904223u));
    const float jitterY = lerp(0.12f, 0.88f, Hash01(instanceId * 22695477u + 1u));
    const float normalizedX = (((float)column + jitterX) / (float)columnCount) * 2.0f - 1.0f;
    const float normalizedY = (((float)row + jitterY) / (float)rowCount) * 2.0f - 1.0f;

    const float nearDistance = 18.0f;
    const float farDistance = 120.0f;
    const float depthLerp = ((float)(instanceId % rowCount) + Hash01(instanceId * 747796405u + 2891336453u)) / (float)rowCount;
    const float depth = lerp(nearDistance, farDistance, depthLerp);
    const float tanHalfFovY = tan(BenchmarkParams.z * 0.5f);
    const float viewX = normalizedX * tanHalfFovY * aspect * depth * 0.88f;
    const float viewY = normalizedY * tanHalfFovY * depth * 0.82f;
    return float3(viewX, viewY, depth);
}

float4 MakeBenchmarkTint(uint instanceId)
{
    return float4(
        lerp(0.25f, 1.0f, Hash01(instanceId * 9781u + 17u)),
        lerp(0.25f, 1.0f, Hash01(instanceId * 6271u + 93u)),
        lerp(0.25f, 1.0f, Hash01(instanceId * 3253u + 191u)),
        1.0f);
}

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    VSOutput output;
    output.TangentSign = input.TangentSign == 0.0f ? 1.0f : input.TangentSign;
    if (BenchmarkParams.x > 0.5f)
    {
        const float3 center = MakeBenchmarkCenter(instanceId);
        const float3 localPosition = input.Position * BenchmarkParams.y;
        output.Position = mul(float4(center + localPosition, 1.0f), WorldViewProjection);
        output.TexCoord = input.TexCoord;
        output.Color = input.Color * MakeBenchmarkTint(instanceId);
        output.WorldPosition = center + localPosition;
        output.NormalWorld = normalize(input.Normal);
        output.TangentWorld = normalize(input.Tangent);
        return output;
    }

    const float4 worldPosition = mul(float4(input.Position, 1.0f), World);
    output.Position = mul(float4(input.Position, 1.0f), WorldViewProjection);
    output.TexCoord = input.TexCoord;
    output.Color = input.Color;
    output.WorldPosition = worldPosition.xyz;
    output.NormalWorld = normalize(mul(float4(input.Normal, 0.0f), WorldInverseTranspose).xyz);
    output.TangentWorld = normalize(mul(float4(input.Tangent, 0.0f), WorldInverseTranspose).xyz);
    return output;
}

float3 TonemapAces(float3 color)
{
    color = max(color, float3(0.0f, 0.0f, 0.0f));
    return saturate((color * (2.51f * color + 0.03f)) / (color * (2.43f * color + 0.59f) + 0.14f));
}

float4 ApplyToneMapping(float4 color)
{
    color.rgb = TonemapAces(color.rgb * max(ExposureDebug.x, 0.0f));
    color.rgb = pow(color.rgb, 1.0f / 2.2f);
    return color;
}

float3 ResolveNormal(VSOutput input)
{
    float3 normal = input.NormalWorld;
    if (dot(normal, normal) < 0.000001f)
    {
        normal = float3(0.0f, 1.0f, 0.0f);
    }
    normal = normalize(normal);

    if (MaterialTextureFlags.y > 0.5f)
    {
        float3 tangent = input.TangentWorld;
        if (dot(tangent, tangent) < 0.000001f)
        {
            tangent = float3(1.0f, 0.0f, 0.0f);
        }
        tangent = normalize(tangent - normal * dot(tangent, normal));
        const float3 bitangent = normalize(cross(normal, tangent)) * (input.TangentSign < 0.0f ? -1.0f : 1.0f);
        float3 sampledNormal = NormalTexture.Sample(MaterialSampler, input.TexCoord).xyz * 2.0f - 1.0f;
        if (ExposureDebug.z > 0.5f)
        {
            sampledNormal.y = -sampledNormal.y;
        }
        normal = normalize(sampledNormal.x * tangent + sampledNormal.y * bitangent + sampledNormal.z * normal);
    }

    return normal;
}

float GetAo(float2 texCoord)
{
    return MaterialTextureFlags.z > 0.5f ? AOTexture.Sample(MaterialSampler, texCoord).r : 1.0f;
}

float3 GetEmissive(float2 texCoord)
{
    return MaterialEmissiveMetallic.rgb
        + (MaterialTextureFlags.w > 0.5f ? EmissiveTexture.Sample(MaterialSampler, texCoord).rgb : float3(0.0f, 0.0f, 0.0f));
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

GpuLightData GetActiveLight(uint index)
{
    if (LightCountParams.w > 0.5f)
    {
        return DeferredLightBuffer[index];
    }

    GpuLightData light;
    light.PositionType = LightPositionType[index];
    light.DirectionRange = LightDirectionRange[index];
    light.ColorIntensity = LightColorIntensityData[index];
    light.SpotAnglesEnabled = LightSpotAnglesEnabled[index];
    return light;
}

uint GetActiveLightCount()
{
    return LightCountParams.w > 0.5f
        ? (uint)LightCountParams.z
        : min((uint)LightCountParams.x, (uint)MAX_FORWARD_LIGHTS);
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

float4 ApplyPhongLighting(VSOutput input, float4 baseColor)
{
    if (BenchmarkParams.x > 0.5f || AmbientSpecular.w < 0.5f)
    {
        return baseColor;
    }

    const float3 normal = ResolveNormal(input);
    float3 viewDirection = CameraPosition.xyz - input.WorldPosition;
    viewDirection = dot(viewDirection, viewDirection) > 0.000001f ? normalize(viewDirection) : float3(0.0f, 0.0f, 1.0f);

    float3 specularColor = MaterialSpecularShininess.rgb;
    if (MaterialTextureFlags2.y > 0.5f)
    {
        specularColor *= SpecularTexture.Sample(MaterialSampler, input.TexCoord).rgb;
    }
    float specularPower = max(MaterialSpecularShininess.w, 1.0f);
    if (MaterialTextureFlags2.z > 0.5f)
    {
        specularPower = max(ShininessTexture.Sample(MaterialSampler, input.TexCoord).r * 256.0f, 1.0f);
    }

    float3 diffuseLighting = float3(0.0f, 0.0f, 0.0f);
    float3 specularLighting = float3(0.0f, 0.0f, 0.0f);
    const uint lightCount = GetActiveLightCount();
    [loop]
    for (uint lightIndex = 0; lightIndex < lightCount; ++lightIndex)
    {
        float3 lightDirection;
        float3 lightColor;
        if (!ResolveLightData(GetActiveLight(lightIndex), input.WorldPosition, lightDirection, lightColor))
        {
            continue;
        }
        const float diffuse = max(dot(normal, lightDirection), 0.0f);
        const float3 reflectedLight = reflect(-lightDirection, normal);
        const float specular = pow(max(dot(viewDirection, reflectedLight), 0.0f), specularPower) * max(AmbientSpecular.y, 0.0f);
        diffuseLighting += diffuse * lightColor;
        specularLighting += specular * specularColor * lightColor;
    }

    const float ao = GetAo(input.TexCoord);
    const float3 ambient = AmbientColorIntensity.rgb * AmbientColorIntensity.w * ao;
    const float3 litColor = baseColor.rgb * (ambient + diffuseLighting) + specularLighting + GetEmissive(input.TexCoord);
    return float4(litColor, baseColor.a);
}

float4 ApplyPbrLighting(VSOutput input, float4 baseColor)
{
    if (BenchmarkParams.x > 0.5f || AmbientSpecular.w < 0.5f)
    {
        return baseColor;
    }

    const float3 normal = ResolveNormal(input);
    float3 viewDirection = CameraPosition.xyz - input.WorldPosition;
    viewDirection = dot(viewDirection, viewDirection) > 0.000001f ? normalize(viewDirection) : float3(0.0f, 0.0f, 1.0f);

    float metallic = saturate(MaterialEmissiveMetallic.w);
    float roughness = clamp(MaterialRoughnessFlags.x, 0.02f, 1.0f);
    if (MaterialRoughnessFlags.z > 0.5f)
    {
        const float4 mr = MetallicRoughnessTexture.Sample(MaterialSampler, input.TexCoord);
        roughness = clamp(mr.g, 0.02f, 1.0f);
        metallic = saturate(mr.b);
    }
    if (MaterialRoughnessFlags.w > 0.5f)
    {
        metallic = saturate(MetallicTexture.Sample(MaterialSampler, input.TexCoord).r);
    }
    if (MaterialTextureFlags.x > 0.5f)
    {
        roughness = clamp(RoughnessTexture.Sample(MaterialSampler, input.TexCoord).r, 0.02f, 1.0f);
    }

    const float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor.rgb, metallic);
    const float nDotV = max(dot(normal, viewDirection), 0.0f);
    float3 directLighting = float3(0.0f, 0.0f, 0.0f);
    const uint lightCount = GetActiveLightCount();
    [loop]
    for (uint lightIndex = 0; lightIndex < lightCount; ++lightIndex)
    {
        float3 lightDirection;
        float3 lightColor;
        if (!ResolveLightData(GetActiveLight(lightIndex), input.WorldPosition, lightDirection, lightColor))
        {
            continue;
        }

        const float3 halfVector = normalize(viewDirection + lightDirection);
        const float nDotL = max(dot(normal, lightDirection), 0.0f);
        const float ndf = DistributionGGX(normal, halfVector, roughness);
        const float geometry = GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
        const float3 fresnel = FresnelSchlick(max(dot(halfVector, viewDirection), 0.0f), f0);
        const float3 specular = (ndf * geometry * fresnel) / max(4.0f * nDotV * nDotL, 0.0001f);
        const float3 kd = (1.0f - fresnel) * (1.0f - metallic);
        directLighting += (kd * baseColor.rgb / 3.14159265f + specular) * lightColor * nDotL;
    }

    const float ao = GetAo(input.TexCoord);
    const float3 ambient = baseColor.rgb * AmbientColorIntensity.rgb * AmbientColorIntensity.w * ao;
    return float4(directLighting + ambient + GetEmissive(input.TexCoord), baseColor.a);
}

float4 PSMain(VSOutput input) : SV_Target
{
    const bool useVertexColor = BenchmarkParams.x > 0.5f || MaterialTextureFlags2.w > 0.5f;
    const float4 vertexColor = useVertexColor ? input.Color : float4(1.0f, 1.0f, 1.0f, input.Color.a);
    float4 baseColor = vertexColor * MaterialBaseColor;
    if (input.TexCoord.x >= 0.0f)
    {
        baseColor = BaseColorTexture.Sample(MaterialSampler, input.TexCoord) * vertexColor * MaterialBaseColor;
    }

    if (MaterialTextureFlags2.x > 0.5f)
    {
        baseColor.a *= OpacityTexture.Sample(MaterialSampler, input.TexCoord).r;
    }
    clip(baseColor.a - 0.1f);

    const uint debugView = (uint)(ExposureDebug.y + 0.5f);
    if (debugView == 1u) return ApplyToneMapping(float4(baseColor.rgb, baseColor.a));
    if (debugView == 2u) return ApplyToneMapping(float4(ResolveNormal(input) * 0.5f + 0.5f, baseColor.a));
    if (debugView == 3u) return ApplyToneMapping(float4(MaterialRoughnessFlags.w > 0.5f ? MetallicTexture.Sample(MaterialSampler, input.TexCoord).rrr : MaterialEmissiveMetallic.www, baseColor.a));
    if (debugView == 4u) return ApplyToneMapping(float4(MaterialTextureFlags.x > 0.5f ? RoughnessTexture.Sample(MaterialSampler, input.TexCoord).rrr : MaterialRoughnessFlags.xxx, baseColor.a));
    if (debugView == 5u)
    {
        const float ao = GetAo(input.TexCoord);
        return ApplyToneMapping(float4(ao, ao, ao, baseColor.a));
    }
    if (debugView == 6u) return ApplyToneMapping(float4(GetEmissive(input.TexCoord), baseColor.a));
    if (debugView == 8u) return ApplyToneMapping(input.Color);

    float4 litColor;
    if (MaterialRoughnessFlags.y > 1.5f)
    {
        litColor = baseColor;
    }
    else if (MaterialRoughnessFlags.y > 0.5f)
    {
        litColor = ApplyPbrLighting(input, debugView == 7u ? float4(1.0f, 1.0f, 1.0f, baseColor.a) : baseColor);
    }
    else
    {
        litColor = ApplyPhongLighting(input, debugView == 7u ? float4(1.0f, 1.0f, 1.0f, baseColor.a) : baseColor);
    }
    return ApplyToneMapping(litColor);
}
