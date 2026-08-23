static const float Pi = 3.14159265358979323846;

struct PixelStatistics
{
    float estimate;
    float exact;
    float sampleVariance;
    float standardError;
    uint sampleCount;
    uint status;
    float lastCosine;
    float lastAzimuth;
};

cbuffer DispatchConstants : register(b0)
{
    uint Width;
    uint Height;
    uint SamplesPerDispatch;
    uint PreviousSampleCount;
    float Exponent;
    uint Seed;
    uint Reset;
    uint Reserved;
};

RWStructuredBuffer<float4> Moments : register(u0);
RWStructuredBuffer<PixelStatistics> Statistics : register(u1);

uint PcgHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float UnitFloat(uint bits)
{
    return (float(bits) + 0.5) * (1.0 / 4294967296.0);
}

float2 UnitSample(uint pixelIndex, uint sampleIndex)
{
    uint base = Seed ^ (pixelIndex * 0xD1B54A35u) ^ (sampleIndex * 0x9E3779B9u);
    return float2(UnitFloat(PcgHash(base ^ 0xA511E9B3u)),
                  UnitFloat(PcgHash(base ^ 0x63D83595u)));
}

[numthreads(8, 8, 1)]
void SampleCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Width || dispatchThreadId.y >= Height)
    {
        return;
    }

    uint pixelIndex = dispatchThreadId.y * Width + dispatchThreadId.x;
    float4 moments = Reset != 0u ? float4(0.0, 0.0, 0.0, 0.0) : Moments[pixelIndex];
    uint sampleCount = Reset != 0u ? 0u : PreviousSampleCount;
    float lastCosine = 0.0;
    float lastAzimuth = 0.0;

    for (uint localSample = 0u; localSample < SamplesPerDispatch; ++localSample)
    {
        uint sampleIndex = sampleCount;
        float2 unitSample = UnitSample(pixelIndex, sampleIndex);
        lastCosine = unitSample.x;
        lastAzimuth = 2.0 * Pi * unitSample.y;

        float contribution = 2.0 * Pi * pow(lastCosine, Exponent);
        sampleCount += 1u;
        float delta = contribution - moments.x;
        moments.x += delta / float(sampleCount);
        float deltaAfterMeanUpdate = contribution - moments.x;
        moments.y += delta * deltaAfterMeanUpdate;
    }

    moments.z = float(sampleCount);
    moments.w = lastCosine;
    Moments[pixelIndex] = moments;

    float sampleVariance = sampleCount > 1u ? moments.y / float(sampleCount - 1u) : 0.0;
    PixelStatistics statistics;
    statistics.estimate = moments.x;
    statistics.exact = 2.0 * Pi / (Exponent + 1.0);
    statistics.sampleVariance = sampleVariance;
    statistics.standardError = sqrt(sampleVariance / float(sampleCount));
    statistics.sampleCount = sampleCount;
    statistics.status = 1u;
    statistics.lastCosine = lastCosine;
    statistics.lastAzimuth = lastAzimuth;
    Statistics[pixelIndex] = statistics;
}

struct FullscreenVertex
{
    float4 position : SV_Position;
};

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenVertex output;
    output.position = float4(vertexId == 2u ? 3.0 : -1.0,
                             vertexId == 1u ? 3.0 : -1.0,
                             0.0,
                             1.0);
    return output;
}

cbuffer DisplayConstants : register(b0)
{
    uint DisplayWidth;
    uint DisplayHeight;
    uint DebugView;
};

StructuredBuffer<PixelStatistics> DisplayStatistics : register(t0);

float3 HeatMap(float value)
{
    float t = saturate(value);
    return saturate(float3(1.5 * t, 1.5 - abs(3.0 * t - 1.5), 1.5 * (1.0 - t)));
}

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
    uint2 pixel = min(uint2(position.xy), uint2(DisplayWidth - 1u, DisplayHeight - 1u));
    PixelStatistics statistics = DisplayStatistics[pixel.y * DisplayWidth + pixel.x];
    float relativeError = abs(statistics.estimate - statistics.exact) / max(statistics.exact, 1.0e-6);

    float3 color = statistics.estimate.xxx / max(2.0 * statistics.exact, 1.0e-6);
    if (DebugView == 1u)
    {
        color = HeatMap(relativeError * 4.0);
    }
    else if (DebugView == 2u)
    {
        color = HeatMap(statistics.standardError / max(statistics.exact, 1.0e-6) * 8.0);
    }
    else if (DebugView == 3u)
    {
        color = HeatMap(log2(float(statistics.sampleCount) + 1.0) / 12.0);
    }
    return float4(color, 1.0);
}
