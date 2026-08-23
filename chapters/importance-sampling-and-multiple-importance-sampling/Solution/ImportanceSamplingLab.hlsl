static const float Pi = 3.14159265358979323846;
static const float UniformPdf = 1.0 / (2.0 * Pi);

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

EstimatorStatistics Summarize(EstimatorMoments moments, uint evaluationsPerSample)
{
    EstimatorStatistics statistics;
    statistics.estimate = moments.mean;
    statistics.sampleVariance =
        moments.sampleCount > 1u ? moments.sumSquaredDifferences / float(moments.sampleCount - 1u) : 0.0;
    statistics.standardError =
        moments.sampleCount > 0u ? sqrt(max(statistics.sampleVariance, 0.0) / float(moments.sampleCount)) : 0.0;
    statistics.integrandEvaluationCount = moments.sampleCount * evaluationsPerSample;
    return statistics;
}

float PowerCosinePdf(float cosine, float proposalExponent)
{
    return ((proposalExponent + 1.0) / (2.0 * Pi)) * pow(cosine, proposalExponent);
}

float PowerCosineContribution(float unitSample, float targetExponent, float proposalExponent)
{
    float cosine = pow(unitSample, 1.0 / (proposalExponent + 1.0));
    float target = pow(cosine, targetExponent);
    return target / PowerCosinePdf(cosine, proposalExponent);
}

float MisWeight(float selectedDensity, float competingDensity)
{
    if (MisHeuristic == 0u)
    {
        return selectedDensity / (selectedDensity + competingDensity);
    }

    float densityScale = max(selectedDensity, competingDensity);
    float selected = selectedDensity / densityScale;
    float competing = competingDensity / densityScale;
    float selectedSquared = selected * selected;
    float competingSquared = competing * competing;
    return selectedSquared / (selectedSquared + competingSquared);
}

float PairedMisContribution(uint pixelIndex, uint pairIndex)
{
    float uniformCosine = UnitSample(pixelIndex, pairIndex, 3u).x;
    float uniformTarget = pow(uniformCosine, TargetExponent);
    float proposalAtUniform = PowerCosinePdf(uniformCosine, ProposalExponent);
    float uniformContribution =
        MisWeight(UniformPdf, proposalAtUniform) * (uniformTarget / UniformPdf);

    float proposalUnitSample = UnitSample(pixelIndex, pairIndex, 4u).x;
    float proposalCosine = pow(proposalUnitSample, 1.0 / (ProposalExponent + 1.0));
    float proposalTarget = pow(proposalCosine, TargetExponent);
    float selectedProposalPdf = PowerCosinePdf(proposalCosine, ProposalExponent);
    float proposalContribution =
        MisWeight(selectedProposalPdf, UniformPdf) * (proposalTarget / selectedProposalPdf);

    return uniformContribution + proposalContribution;
}

bool IsFiniteStatistics(EstimatorStatistics statistics)
{
    return all(isfinite(float3(statistics.estimate, statistics.sampleVariance, statistics.standardError))) &&
           statistics.sampleVariance >= 0.0 && statistics.standardError >= 0.0 &&
           statistics.integrandEvaluationCount > 0u;
}

[numthreads(8, 8, 1)]
void SampleCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Width || dispatchThreadId.y >= Height)
    {
        return;
    }

    uint pixelIndex = dispatchThreadId.y * Width + dispatchThreadId.x;
    uint pairCountPerDispatch = IntegrandEvaluationsPerDispatch / 2u;
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
        bool simpleCountWouldOverflow =
            moments.uniformEstimator.sampleCount > 0xFFFFFFFFu - IntegrandEvaluationsPerDispatch ||
            moments.matched.sampleCount > 0xFFFFFFFFu - IntegrandEvaluationsPerDispatch ||
            moments.mismatched.sampleCount > 0xFFFFFFFFu - IntegrandEvaluationsPerDispatch;
        bool misCountWouldOverflow =
            moments.mis.sampleCount > (0xFFFFFFFFu / 2u) - pairCountPerDispatch;
        if (simpleCountWouldOverflow || misCountWouldOverflow)
        {
            moments.uniformEstimator = EmptyMoments();
            moments.matched = EmptyMoments();
            moments.mismatched = EmptyMoments();
            moments.mis = EmptyMoments();
        }
    }

    for (uint localEvaluation = 0u; localEvaluation < IntegrandEvaluationsPerDispatch; ++localEvaluation)
    {
        uint uniformSampleIndex = moments.uniformEstimator.sampleCount;
        float uniformCosine = UnitSample(pixelIndex, uniformSampleIndex, 0u).x;
        PushContribution(moments.uniformEstimator, 2.0 * Pi * pow(uniformCosine, TargetExponent));

        uint matchedSampleIndex = moments.matched.sampleCount;
        float matchedUnitSample = UnitSample(pixelIndex, matchedSampleIndex, 1u).x;
        PushContribution(
            moments.matched,
            PowerCosineContribution(matchedUnitSample, TargetExponent, TargetExponent));

        uint mismatchedSampleIndex = moments.mismatched.sampleCount;
        float mismatchedUnitSample = UnitSample(pixelIndex, mismatchedSampleIndex, 2u).x;
        PushContribution(
            moments.mismatched,
            PowerCosineContribution(mismatchedUnitSample, TargetExponent, ProposalExponent));
    }

    for (uint localPair = 0u; localPair < pairCountPerDispatch; ++localPair)
    {
        uint pairIndex = moments.mis.sampleCount;
        PushContribution(moments.mis, PairedMisContribution(pixelIndex, pairIndex));
    }
    Moments[pixelIndex] = moments;

    PixelStatistics statistics;
    statistics.uniformEstimator = Summarize(moments.uniformEstimator, 1u);
    statistics.matched = Summarize(moments.matched, 1u);
    statistics.mismatched = Summarize(moments.mismatched, 1u);
    statistics.mis = Summarize(moments.mis, 2u);
    statistics.exact = 2.0 * Pi / (TargetExponent + 1.0);
    statistics.misPairCount = moments.mis.sampleCount;
    statistics.status = 0u;
    statistics.status |= IsFiniteStatistics(statistics.uniformEstimator) ? 1u : 0u;
    statistics.status |= IsFiniteStatistics(statistics.matched) ? 2u : 0u;
    statistics.status |= IsFiniteStatistics(statistics.mismatched) ? 4u : 0u;
    statistics.status |= IsFiniteStatistics(statistics.mis) ? 8u : 0u;
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

EstimatorStatistics SelectEstimator(PixelStatistics statistics, uint estimatorIndex)
{
    if (estimatorIndex == 0u)
    {
        return statistics.uniformEstimator;
    }
    if (estimatorIndex == 1u)
    {
        return statistics.matched;
    }
    if (estimatorIndex == 2u)
    {
        return statistics.mismatched;
    }
    return statistics.mis;
}

float3 EstimatorTint(uint estimatorIndex)
{
    if (estimatorIndex == 0u)
    {
        return float3(1.0, 0.55, 0.25);
    }
    if (estimatorIndex == 1u)
    {
        return float3(0.25, 1.0, 0.55);
    }
    if (estimatorIndex == 2u)
    {
        return float3(0.35, 0.55, 1.0);
    }
    return float3(1.0, 0.35, 0.9);
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
    if (DebugView <= 3u)
    {
        EstimatorStatistics selected = SelectEstimator(statistics, DebugView);
        float value = saturate(selected.estimate / (2.0 * exact));
        return float4(value * EstimatorTint(DebugView), 1.0);
    }

    uint band = min(3u, (pixel.x * 4u) / DisplayWidth);
    EstimatorStatistics comparison = SelectEstimator(statistics, band);
    float3 color;
    if (DebugView == 4u)
    {
        float relativeError = abs(comparison.estimate - statistics.exact) / exact;
        color = HeatMap(relativeError * 4.0);
    }
    else if (DebugView == 5u)
    {
        color = HeatMap(comparison.standardError / exact * 8.0);
    }
    else
    {
        float work = log2(float(comparison.integrandEvaluationCount) + 1.0) / 12.0;
        color = HeatMap(work) * EstimatorTint(band);
    }
    return float4(color, 1.0);
}
