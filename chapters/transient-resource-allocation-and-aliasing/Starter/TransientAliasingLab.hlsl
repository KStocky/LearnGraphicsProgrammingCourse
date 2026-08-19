struct FullscreenVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D<float4> inputTextures[2] : register(t0);
SamplerState pointClampSampler : register(s0);

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    FullscreenVertex output;
    output.position = float4((uv * float2(2.0, -2.0)) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

uint BandIndex(float coordinate)
{
    return min(7u, (uint)(coordinate * 8.0));
}

float3 AnalyticHdrColor(float2 uv)
{
    uint xBand = BandIndex(uv.x);
    uint yBand = BandIndex(uv.y);
    if ((xBand >= 3u && xBand <= 4u) && (yBand >= 3u && yBand <= 4u))
    {
        return float3(4.0, 4.0, 4.0);
    }
    if (xBand < 4u && yBand < 4u)
    {
        return float3(4.0, 0.25 + (0.125 * yBand), 0.125 + (0.125 * (xBand & 1u)));
    }
    if (xBand >= 4u && yBand < 4u)
    {
        return float3(0.25 + (0.25 * (xBand - 4u)), 3.5, 0.5 + (0.125 * yBand));
    }
    if (xBand < 4u && yBand >= 4u)
    {
        return float3(0.25, 0.5 + (0.125 * xBand), 3.75);
    }
    return float3(
        0.75 + (0.25 * (xBand - 4u)),
        0.5 + (0.25 * (yBand - 4u)),
        1.0 + (0.125 * ((xBand - 4u) + (yBand - 4u))));
}

float4 AnalyticPS(FullscreenVertex input) : SV_Target
{
    return float4(AnalyticHdrColor(input.uv), 1.0);
}

float4 CopyPS(FullscreenVertex input) : SV_Target
{
    return inputTextures[0].SampleLevel(pointClampSampler, input.uv, 0.0);
}

float4 AccentMaskPS(FullscreenVertex input) : SV_Target
{
    uint xBand = BandIndex(input.uv.x);
    uint yBand = BandIndex(input.uv.y);
    float mask = (xBand == yBand || (xBand + yBand) == 7u) ? 1.0 : 0.0;
    return float4(mask, mask, mask, 1.0);
}

float SrgbOetf(float linearValue)
{
    return linearValue <= 0.0031308
        ? linearValue * 12.92
        : (1.055 * pow(linearValue, 1.0 / 2.4)) - 0.055;
}

float3 SrgbOetf(float3 linearColor)
{
    return float3(SrgbOetf(linearColor.r), SrgbOetf(linearColor.g), SrgbOetf(linearColor.b));
}

float3 ToneMap(float3 hdrColor)
{
    float3 exposed = hdrColor * 0.5;
    return exposed / (1.0 + exposed);
}

float4 CompositePS(FullscreenVertex input) : SV_Target
{
    float3 copiedHdr = inputTextures[0].SampleLevel(pointClampSampler, input.uv, 0.0).rgb;
    float accentMask = inputTextures[1].SampleLevel(pointClampSampler, input.uv, 0.0).r;
    // The bounded mask adds a cyan-biased HDR accent before display mapping.
    float3 accentedHdr = copiedHdr + (accentMask * float3(0.0, 0.75, 1.5));
    return float4(SrgbOetf(saturate(ToneMap(accentedHdr))), 1.0);
}
