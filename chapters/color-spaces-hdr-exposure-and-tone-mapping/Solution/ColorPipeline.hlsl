struct FullscreenVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct OutputConstants
{
    float exposureValue;
    uint toneMapper;
    uint bypassSrgbEncode;
};

ConstantBuffer<OutputConstants> outputConstants : register(b0);
Texture2D<float4> hdrInput : register(t0);
SamplerState linearClampSampler : register(s0);

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);

    FullscreenVertex output;
    output.position = float4((uv * float2(2.0, -2.0)) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

float3 AnalyticHdrColor(float2 uv)
{
    float3 color = float3(
        lerp(0.05, 3.60, uv.x),
        lerp(0.10, 2.40, uv.y),
        0.18 + (1.55 * uv.x * uv.y));

    if (uv.x < 0.30 && uv.y < 0.34)
    {
        color = float3(4.0, 0.24, 0.10);
    }
    else if (uv.x > 0.70 && uv.y < 0.34)
    {
        color = float3(0.16, 3.5, 0.28);
    }
    else if (uv.x > 0.36 && uv.x < 0.64 && uv.y > 0.38 && uv.y < 0.66)
    {
        color = float3(2.6, 2.6, 2.6);
    }
    else if (uv.x < 0.30 && uv.y > 0.68)
    {
        color = float3(0.18, 0.34, 3.8);
    }

    return color;
}

float4 HdrAnalyticPS(FullscreenVertex input) : SV_Target
{
    return float4(AnalyticHdrColor(input.uv), 1.0);
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

float3 Reinhard(float3 color)
{
    return color / (1.0 + color);
}

float3 RrtAndOdtFit(float3 value)
{
    float3 numerator = (value * (value + 0.0245786)) - 0.000090537;
    float3 denominator = (value * ((0.983729 * value) + 0.4329510)) + 0.238081;
    return numerator / denominator;
}

float3 AcesFitted(float3 color)
{
    static const float3x3 acesInput = {
        0.59719, 0.35458, 0.04823,
        0.07600, 0.90834, 0.01566,
        0.02840, 0.13383, 0.83777
    };
    static const float3x3 acesOutput = {
        1.60475, -0.53108, -0.07367,
        -0.10208, 1.10813, -0.00605,
        -0.00327, -0.07276, 1.07602
    };

    color = mul(acesInput, color);
    color = RrtAndOdtFit(color);
    return mul(acesOutput, color);
}

float3 ApplyToneMapper(float3 color, uint toneMapper)
{
    if (toneMapper == 1)
    {
        return Reinhard(color);
    }
    if (toneMapper == 2)
    {
        return AcesFitted(color);
    }
    return color;
}

float4 OutputPS(FullscreenVertex input) : SV_Target
{
    float3 hdr = hdrInput.SampleLevel(linearClampSampler, input.uv, 0.0).rgb;
    float3 exposed = hdr * exp2(outputConstants.exposureValue);
    float3 toneMapped = ApplyToneMapper(exposed, outputConstants.toneMapper);
    float3 display = outputConstants.bypassSrgbEncode != 0 ? toneMapped : SrgbOetf(toneMapped);
    return float4(saturate(display), 1.0);
}
