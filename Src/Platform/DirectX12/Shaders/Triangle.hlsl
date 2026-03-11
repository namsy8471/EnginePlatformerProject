// DirectX12 Triangle Shader
// 기본 삼각형 렌더링을 위한 정점 셰이더와 픽셀 셰이더

cbuffer CameraConstants : register(b0)
{
    row_major float4x4 ViewProjection;
    float4 CameraPosition;
    float4 DebugOptions;
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

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = mul(float4(input.Position, 1.0f), ViewProjection);
    output.TexCoord = input.TexCoord;
    output.Color = input.Color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    const float4 sampledColor = DiffuseTexture.Sample(DiffuseSampler, input.TexCoord);
    if (sampledColor.a < 0.5f)
    {
        discard;
    }

    if (DebugOptions.x > 0.5f)
    {
        const float2 wrappedUv = frac(input.TexCoord);
        const bool isOutOfRange = input.TexCoord.x < 0.0f || input.TexCoord.x > 1.0f || input.TexCoord.y < 0.0f || input.TexCoord.y > 1.0f;
        return float4(wrappedUv.x, wrappedUv.y, isOutOfRange ? 1.0f : 0.0f, 1.0f);
    }

    return sampledColor * input.Color;
}
