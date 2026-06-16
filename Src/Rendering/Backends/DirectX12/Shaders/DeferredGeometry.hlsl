// DirectX12 deferred geometry pass.

#define MAX_FORWARD_LIGHTS 8

cbuffer CameraConstants : register(b0)
{
    row_major float4x4 WorldViewProjection;
    row_major float4x4 ViewProjection;
    row_major float4x4 World;
    row_major float4x4 WorldInverseTranspose;
    float4 CameraPosition;
    float4 BenchmarkParams;
    float4 LightDirection;
    float4 LightColorIntensity;
    float4 AmbientSpecular;
    float4 MaterialBaseColor;
    float4 MaterialSpecularShininess;
    float4 MaterialEmissiveMetallic;
    float4 MaterialRoughnessFlags;
    float4 MaterialTextureFlags;
    float4 MaterialTextureFlags2;
    float4 AmbientColorIntensity;
    float4 ExposureDebug;
    float4 LightCountParams;
    float4 LightPositionType[MAX_FORWARD_LIGHTS];
    float4 LightDirectionRange[MAX_FORWARD_LIGHTS];
    float4 LightColorIntensityData[MAX_FORWARD_LIGHTS];
    float4 LightSpotAnglesEnabled[MAX_FORWARD_LIGHTS];
    row_major float4x4 ShadowViewProjection;
    float4 ShadowParams;
    float4 ShadowDirection;
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

struct PSOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Material : SV_Target2;
    float4 WorldPosition : SV_Target3;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    const float4 worldPosition = mul(float4(input.Position, 1.0f), World);
    output.Position = mul(float4(input.Position, 1.0f), WorldViewProjection);
    output.TexCoord = input.TexCoord;
    output.Color = input.Color;
    output.WorldPosition = worldPosition.xyz;
    output.NormalWorld = normalize(mul(float4(input.Normal, 0.0f), WorldInverseTranspose).xyz);
    output.TangentWorld = normalize(mul(float4(input.Tangent, 0.0f), WorldInverseTranspose).xyz);
    output.TangentSign = input.TangentSign == 0.0f ? 1.0f : input.TangentSign;
    return output;
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

PSOutput PSMain(VSOutput input)
{
    const bool useVertexColor = MaterialTextureFlags2.w > 0.5f;
    const float4 vertexColor = useVertexColor ? input.Color : float4(1.0f, 1.0f, 1.0f, input.Color.a);
    float4 baseColor = BaseColorTexture.Sample(MaterialSampler, input.TexCoord) * vertexColor * MaterialBaseColor;
    if (MaterialTextureFlags2.x > 0.5f)
    {
        baseColor.a *= OpacityTexture.Sample(MaterialSampler, input.TexCoord).r;
    }
    clip(baseColor.a - 0.1f);

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
    const float ao = MaterialTextureFlags.z > 0.5f ? AOTexture.Sample(MaterialSampler, input.TexCoord).r : 1.0f;

    PSOutput output;
    output.Albedo = baseColor;
    output.Normal = float4(ResolveNormal(input), MaterialSpecularShininess.w);
    output.Material = float4(roughness, metallic, MaterialRoughnessFlags.y, ao);
    output.WorldPosition = float4(input.WorldPosition, 1.0f);
    return output;
}
