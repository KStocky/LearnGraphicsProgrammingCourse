Texture2D<float4> gTexture : register(t0);
SamplerState gPointSampler : register(s0);
SamplerState gBilinearSampler : register(s1);
SamplerState gTrilinearSampler : register(s2);
SamplerState gAnisotropicSampler : register(s3);

cbuffer DrawConstants : register(b0)
{
    uint gSamplerMode;
    uint gVisualizeMip;
    float gLodBias;
    uint gPadding;
};

struct VSInput
{
    float4 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = input.position;
    output.uv = input.uv;
    return output;
}

float4 SampleTexture(float2 uv)
{
    if (gSamplerMode == 0U) {
        return gTexture.SampleBias(gPointSampler, uv, gLodBias);
    }
    if (gSamplerMode == 1U) {
        return gTexture.SampleBias(gBilinearSampler, uv, gLodBias);
    }
    if (gSamplerMode == 2U) {
        return gTexture.SampleBias(gTrilinearSampler, uv, gLodBias);
    }
    return gTexture.SampleBias(gAnisotropicSampler, uv, gLodBias);
}

float4 PSMain(PSInput input) : SV_Target0
{
    if (gVisualizeMip != 0U) {
        float lod = gTexture.CalculateLevelOfDetail(gAnisotropicSampler, input.uv) + gLodBias;
        float normalized = saturate(lod / 6.0F);
        return float4(normalized, frac(lod), 1.0F - normalized, 1.0F);
    }

    float4 sampled = SampleTexture(input.uv);
    return float4(sampled.rgb, 1.0F);
}
