// DirectX12 Triangle Shader
// 기본 삼각형 렌더링을 위한 정점 셰이더와 픽셀 셰이더

cbuffer CameraConstants : register(b0)
{
    row_major float4x4 ViewProjection;
    float4 CameraPosition;
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
    const float sampledLuminance = dot(sampledColor.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    return sampledLuminance < 0.001f ? input.Color : sampledColor;
}
