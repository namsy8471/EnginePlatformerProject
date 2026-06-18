cbuffer SkyboxConstants : register(b0)
{
    float4 CameraRightTanX;
    float4 CameraUpTanY;
    float4 CameraForwardEnabled;
    float4 ZenithColorIntensity;
    float4 HorizonColorBlend;
    float4 GroundColorHorizon;
    float4 SunDirectionSize;
    float4 SunColorIntensity;
};

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

float3 BuildSkyDirection(float2 texCoord)
{
    const float2 ndc = float2(texCoord.x * 2.0f - 1.0f, 1.0f - texCoord.y * 2.0f);
    return normalize(
        CameraForwardEnabled.xyz +
        CameraRightTanX.xyz * ndc.x * CameraRightTanX.w +
        CameraUpTanY.xyz * ndc.y * CameraUpTanY.w);
}

float3 EvaluateSky(float2 texCoord)
{
    if (CameraForwardEnabled.w < 0.5f)
    {
        return float3(0.025f, 0.027f, 0.032f);
    }

    const float3 direction = BuildSkyDirection(texCoord);
    const float horizonHeight = clamp(GroundColorHorizon.w, -0.95f, 0.95f);
    const float blend = max(HorizonColorBlend.w, 0.05f);
    const float height = direction.y;
    const float topFactor = saturate((height - horizonHeight) / max(1.0f - horizonHeight, 0.001f));
    const float bottomFactor = saturate((horizonHeight - height) / max(1.0f + horizonHeight, 0.001f));
    float3 color = lerp(HorizonColorBlend.rgb, ZenithColorIntensity.rgb, pow(topFactor, blend));
    color = lerp(color, GroundColorHorizon.rgb, pow(bottomFactor, blend));
    color *= ZenithColorIntensity.w;

    const float3 sunDirection = normalize(SunDirectionSize.xyz);
    const float sunDot = saturate(dot(direction, sunDirection));
    const float sunSize = clamp(SunDirectionSize.w, 0.001f, 0.35f);
    const float sunCore = smoothstep(cos(sunSize * 1.45f), cos(sunSize), sunDot);
    const float sunGlow = pow(sunDot, max(2.0f, 0.18f / sunSize));
    color += SunColorIntensity.rgb * SunColorIntensity.w * (sunCore + sunGlow * 0.18f);
    return color;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float3 color = EvaluateSky(input.TexCoord);
    color = TonemapAces(color);
    color = pow(color, 1.0f / 2.2f);
    return float4(color, 1.0f);
}
