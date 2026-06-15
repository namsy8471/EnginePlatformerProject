// DirectX12 Triangle Shader
// 기본 삼각형 렌더링을 위한 정점 셰이더와 픽셀 셰이더

cbuffer CameraConstants : register(b0)
{
    row_major float4x4 WorldViewProjection;
    row_major float4x4 ViewProjection;
    float4 CameraPosition;
    float4 BenchmarkParams; // x: instance count, y: local scale, z: fovY, w: aspect
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
        return output;
    }

    output.Position = mul(float4(input.Position, 1.0f), WorldViewProjection);
    output.TexCoord = input.TexCoord;
    output.Color = input.Color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    if (input.TexCoord.x < 0.0f)
    {
        return input.Color;
    }

    const float4 sampledColor = DiffuseTexture.Sample(DiffuseSampler, input.TexCoord);
    clip(sampledColor.a - 0.1f);
    return sampledColor * input.Color;
}
