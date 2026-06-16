// DirectX12 fullscreen tone mapping pass.

cbuffer PostProcessConstants : register(b0)
{
    float4 ExposureDebug; // x exposure, y material debug view
};

Texture2D HdrColor : register(t0);
SamplerState LinearSampler : register(s0);

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

float4 PSMain(VSOutput input) : SV_Target
{
    float4 color = HdrColor.Sample(LinearSampler, input.TexCoord);
    color.rgb = TonemapAces(color.rgb * max(ExposureDebug.x, 0.0f));
    color.rgb = pow(color.rgb, 1.0f / 2.2f);
    return color;
}
