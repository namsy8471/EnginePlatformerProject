// DirectX12 directional shadow depth pass.

cbuffer CameraConstants : register(b0)
{
    row_major float4x4 WorldViewProjection;
};

struct VSInput
{
    float3 Position : POSITION;
};

struct VSOutput
{
    float4 Position : SV_Position;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = mul(float4(input.Position, 1.0f), WorldViewProjection);
    return output;
}
