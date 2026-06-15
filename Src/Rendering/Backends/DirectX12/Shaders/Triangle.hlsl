// DirectX12 Triangle Shader
// 기본 삼각형 렌더링을 위한 정점 셰이더와 픽셀 셰이더

cbuffer CameraConstants : register(b0)
{
    row_major float4x4 WorldViewProjection;
    row_major float4x4 ViewProjection;
    row_major float4x4 World;
    float4 CameraPosition;
    float4 BenchmarkParams; // x: instance count, y: local scale, z: fovY, w: aspect
    float4 LightDirection; // xyz: normalized vector from surface to directional light, w: enabled
    float4 LightColorIntensity; // rgb: light color, w: intensity
    float4 AmbientSpecular; // x: ambient, y: specular strength, z: shininess, w: lighting enabled
};

Texture2D DiffuseTexture : register(t0);
SamplerState DiffuseSampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Color : COLOR0;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
    float4 Color : COLOR0;
    float3 WorldPosition : TEXCOORD1;
    float3 NormalWorld : TEXCOORD2;
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
    if (BenchmarkParams.x > 0.5f)
    {
        const float3 center = MakeBenchmarkCenter(instanceId);
        const float3 localPosition = input.Position * BenchmarkParams.y;
        output.Position = mul(float4(center + localPosition, 1.0f), WorldViewProjection);
        output.TexCoord = input.TexCoord;
        output.Color = input.Color * MakeBenchmarkTint(instanceId);
        output.WorldPosition = center + localPosition;
        output.NormalWorld = normalize(input.Normal);
        return output;
    }

    const float4 worldPosition = mul(float4(input.Position, 1.0f), World);
    output.Position = mul(float4(input.Position, 1.0f), WorldViewProjection);
    output.TexCoord = input.TexCoord;
    output.Color = input.Color;
    output.WorldPosition = worldPosition.xyz;
    output.NormalWorld = normalize(mul(float4(input.Normal, 0.0f), World).xyz);
    return output;
}

float4 ApplyPhongLighting(VSOutput input, float4 baseColor)
{
    if (BenchmarkParams.x > 0.5f || AmbientSpecular.w < 0.5f)
    {
        return baseColor;
    }

    float3 normal = input.NormalWorld;
    if (dot(normal, normal) < 0.000001f)
    {
        normal = float3(0.0f, 1.0f, 0.0f);
    }
    normal = normalize(normal);

    float3 lightDirection = LightDirection.xyz;
    if (dot(lightDirection, lightDirection) < 0.000001f)
    {
        lightDirection = float3(0.0f, 1.0f, 0.0f);
    }
    lightDirection = normalize(lightDirection);

    float3 viewDirection = CameraPosition.xyz - input.WorldPosition;
    if (dot(viewDirection, viewDirection) < 0.000001f)
    {
        viewDirection = float3(0.0f, 0.0f, 1.0f);
    }
    viewDirection = normalize(viewDirection);

    const float ambient = saturate(AmbientSpecular.x);
    const float diffuse = max(dot(normal, lightDirection), 0.0f);
    const float3 reflectedLight = reflect(-lightDirection, normal);
    const float specularPower = max(AmbientSpecular.z, 1.0f);
    const float specularStrength = max(AmbientSpecular.y, 0.0f);
    const float specular = pow(max(dot(viewDirection, reflectedLight), 0.0f), specularPower) * specularStrength;
    const float3 lightColor = max(LightColorIntensity.rgb * LightColorIntensity.w, float3(0.0f, 0.0f, 0.0f));
    const float3 litColor = baseColor.rgb * (ambient + diffuse * lightColor) + specular * lightColor;
    return float4(saturate(litColor), baseColor.a);
}

float4 PSMain(VSOutput input) : SV_Target
{
    float4 baseColor = input.Color;
    if (input.TexCoord.x >= 0.0f)
    {
        const float4 sampledColor = DiffuseTexture.Sample(DiffuseSampler, input.TexCoord);
        clip(sampledColor.a - 0.1f);
        baseColor = sampledColor * input.Color;
    }

    return ApplyPhongLighting(input, baseColor);
}
