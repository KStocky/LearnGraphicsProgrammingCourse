static const float Pi = 3.14159265358979323846;

struct EstimatorMoments
{
    float mean;
    float sumSquaredDifferences;
    uint sampleCount;
    uint reserved;
};

struct PixelMoments
{
    EstimatorMoments uniformEstimator;
    EstimatorMoments matched;
    EstimatorMoments mismatched;
    EstimatorMoments mis;
};

struct EstimatorStatistics
{
    float estimate;
    float sampleVariance;
    float standardError;
    uint integrandEvaluationCount;
};

struct PixelStatistics
{
    EstimatorStatistics uniformEstimator;
    EstimatorStatistics matched;
    EstimatorStatistics mismatched;
    EstimatorStatistics mis;
    float exact;
    uint misPairCount;
    uint status;
    uint reserved;
};

cbuffer DispatchConstants : register(b0)
{
    uint Width;
    uint Height;
    uint IntegrandEvaluationsPerDispatch;
    uint Seed;
    float TargetExponent;
    float ProposalExponent;
    uint Reset;
    uint MisHeuristic;
};

RWStructuredBuffer<PixelMoments> Moments : register(u0);
RWStructuredBuffer<PixelStatistics> Statistics : register(u1);

uint PcgHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float UnitFloat(uint bits)
{
    return (float(bits >> 8u) + 0.5) * (1.0 / 16777216.0);
}

float2 UnitSample(uint pixelIndex, uint sampleIndex, uint dimensionPair)
{
    uint base = Seed ^ (pixelIndex * 0xD1B54A35u) ^ (sampleIndex * 0x9E3779B9u) ^
                (dimensionPair * 0x85EBCA6Bu);
    return float2(UnitFloat(PcgHash(base ^ 0xA511E9B3u)),
                  UnitFloat(PcgHash(base ^ 0x63D83595u)));
}

EstimatorMoments EmptyMoments()
{
    EstimatorMoments moments;
    moments.mean = 0.0;
    moments.sumSquaredDifferences = 0.0;
    moments.sampleCount = 0u;
    moments.reserved = 0u;
    return moments;
}

void PushContribution(inout EstimatorMoments moments, float contribution)
{
    moments.sampleCount += 1u;
    float delta = contribution - moments.mean;
    moments.mean += delta / float(moments.sampleCount);
    float deltaAfterMeanUpdate = contribution - moments.mean;
    moments.sumSquaredDifferences += delta * deltaAfterMeanUpdate;
}

EstimatorStatistics Summarize(EstimatorMoments moments)
{
    EstimatorStatistics statistics;
    statistics.estimate = moments.mean;
    statistics.sampleVariance =
        moments.sampleCount > 1u ? moments.sumSquaredDifferences / float(moments.sampleCount - 1u) : 0.0;
    statistics.standardError =
        moments.sampleCount > 0u ? sqrt(max(statistics.sampleVariance, 0.0) / float(moments.sampleCount)) : 0.0;
    statistics.integrandEvaluationCount = moments.sampleCount;
    return statistics;
}

[numthreads(8, 8, 1)]
void SampleCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Width || dispatchThreadId.y >= Height)
    {
        return;
    }

    uint pixelIndex = dispatchThreadId.y * Width + dispatchThreadId.x;
    PixelMoments moments;
    if (Reset != 0u)
    {
        moments.uniformEstimator = EmptyMoments();
        moments.matched = EmptyMoments();
        moments.mismatched = EmptyMoments();
        moments.mis = EmptyMoments();
    }
    else
    {
        moments = Moments[pixelIndex];
        bool countWouldOverflow =
            moments.uniformEstimator.sampleCount > 0xFFFFFFFFu - IntegrandEvaluationsPerDispatch;
        if (countWouldOverflow)
        {
            moments.uniformEstimator = EmptyMoments();
            moments.matched = EmptyMoments();
            moments.mismatched = EmptyMoments();
            moments.mis = EmptyMoments();
        }
    }

    for (uint localEvaluation = 0u; localEvaluation < IntegrandEvaluationsPerDispatch; ++localEvaluation)
    {
        uint sampleIndex = moments.uniformEstimator.sampleCount;
        float cosine = UnitSample(pixelIndex, sampleIndex, 0u).x;
        float contribution = 2.0 * Pi * pow(cosine, TargetExponent);
        PushContribution(moments.uniformEstimator, contribution);
    }
    Moments[pixelIndex] = moments;

    PixelStatistics statistics;
    statistics.uniformEstimator = Summarize(moments.uniformEstimator);
    statistics.matched = Summarize(moments.matched);
    statistics.mismatched = Summarize(moments.mismatched);
    statistics.mis = Summarize(moments.mis);
    statistics.exact = 2.0 * Pi / (TargetExponent + 1.0);
    statistics.misPairCount = 0u;
    statistics.status =
        all(isfinite(float3(statistics.uniformEstimator.estimate,
                            statistics.uniformEstimator.sampleVariance,
                            statistics.uniformEstimator.standardError)))
            ? 1u
            : 0u;
    statistics.reserved = 0u;
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
    uint ExpectedStatus;
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
    if ((statistics.status & ExpectedStatus) != ExpectedStatus)
    {
        return float4(1.0, 0.0, 1.0, 1.0);
    }

    float exact = max(statistics.exact, 1.0e-6);
    float3 color = saturate(statistics.uniformEstimator.estimate / (2.0 * exact)).xxx;
    if (DebugView == 4u)
    {
        float relativeError = abs(statistics.uniformEstimator.estimate - statistics.exact) / exact;
        color = HeatMap(relativeError * 4.0);
    }
    else if (DebugView == 5u)
    {
        color = HeatMap(statistics.uniformEstimator.standardError / exact * 8.0);
    }
    else if (DebugView == 6u)
    {
        color = HeatMap(log2(float(statistics.uniformEstimator.integrandEvaluationCount) + 1.0) / 12.0);
    }
    return float4(color, 1.0);
}
