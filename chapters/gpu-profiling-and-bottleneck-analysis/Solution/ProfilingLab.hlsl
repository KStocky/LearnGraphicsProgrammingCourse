#include "ProfilingLab.hlsli"

// Solution: two equal-output variants. The ONLY difference is how a wave is fetched.
//
//   * Baseline reads wave parameters from a constant buffer (broadcast).
//   * Candidate reads the same bytes from a structured buffer (per-lane load).
//
// Both instantiate the shared field macro, so the accumulated field - and therefore the rendered output - is
// byte-for-byte identical. The isolated fetch difference is what a controlled A/B timing experiment measures.

cbuffer WaveConstants : register(b1)
{
    WaveParam gWavesConstant[LGP_MAX_WAVES];
};

StructuredBuffer<WaveParam> gWavesStructured : register(t0);

#define FETCH_BASELINE(i) gWavesConstant[(i)]
#define FETCH_CANDIDATE(i) gWavesStructured[(i)]

LGP_DEFINE_PROFILING_FIELD(ComputeBaselineField, FETCH_BASELINE)
LGP_DEFINE_PROFILING_FIELD(ComputeCandidateField, FETCH_CANDIDATE)

float4 BaselinePS(FullScreenVertex input) : SV_Target0
{
    return FieldToColor(ComputeBaselineField(ScreenUv(input.position.xy)));
}

float4 CandidatePS(FullScreenVertex input) : SV_Target0
{
    return FieldToColor(ComputeCandidateField(ScreenUv(input.position.xy)));
}
