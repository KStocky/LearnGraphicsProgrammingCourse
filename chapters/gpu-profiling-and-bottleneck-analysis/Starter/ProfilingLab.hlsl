#include "ProfilingLab.hlsli"

// Starter: the lab renders a single correct variant. Wave parameters arrive through a constant buffer that is
// broadcast to every pixel invocation.
cbuffer WaveConstants : register(b1)
{
    WaveParam gWaves[LGP_MAX_WAVES];
};

#define FETCH_BASELINE(i) gWaves[(i)]

LGP_DEFINE_PROFILING_FIELD(ComputeBaselineField, FETCH_BASELINE)

float4 ProfilingPS(FullScreenVertex input) : SV_Target0
{
    return FieldToColor(ComputeBaselineField(ScreenUv(input.position.xy)));
}
