#ifndef LGP_CH19_PROFILING_LAB_HLSLI
#define LGP_CH19_PROFILING_LAB_HLSLI

// Shared, variant-independent shader logic for the Chapter 19 profiling lab.
//
// The baseline and candidate pipelines run byte-for-byte identical arithmetic; only the way the wave
// parameters are *fetched* differs (constant-buffer broadcast versus a structured-buffer load). Keeping the
// math here, in one shared include, is what guarantees the two variants produce an identical output that a
// controlled timing comparison can be run against.

#ifndef LGP_MAX_WAVES
#define LGP_MAX_WAVES 64
#endif

struct WaveParam
{
    float4 a; // xy = direction, z = frequency, w = phase
    float4 b; // x = amplitude, yz = center, w = falloff weight
};

cbuffer RenderConstants : register(b0)
{
    uint gWidth;
    uint gHeight;
    uint gWaveCount;
    uint gIterations;
};

// One wave's contribution at a pixel. Pure, deterministic, and shared by both variants so the accumulated
// field is identical no matter how the wave was fetched.
float WaveContribution(WaveParam wave, float2 uv, uint iteration)
{
    float2 direction = wave.a.xy;
    float frequency = wave.a.z;
    float phase = wave.a.w;
    float amplitude = wave.b.x;
    float2 center = wave.b.yz;
    float weight = wave.b.w;

    float wavefront = sin(dot(direction, uv) * frequency + phase + float(iteration) * 0.10000000149011612);
    float distanceToCenter = distance(uv, center);
    return amplitude * wavefront * exp(-weight * distanceToCenter);
}

// Maps the accumulated field to a banded, visually nontrivial color. Banding makes the output robust to
// sub-quantum floating point noise while still producing contour-like structure with many distinct colors.
float4 FieldToColor(float field)
{
    float normalized = frac(field * 0.5 + 0.5);
    float bands = 12.0;
    float banded = floor(normalized * bands) / bands;
    float3 phases = float3(0.0, 2.0943951023931953, 4.1887902047863905);
    float3 color = 0.5 + 0.5 * sin(6.2831853071795862 * banded + phases);
    return float4(color, 1.0);
}

float2 ScreenUv(float2 position)
{
    return (position / float2(float(gWidth), float(gHeight))) * 2.0 - 1.0;
}

struct FullScreenVertex
{
    float4 position : SV_Position;
};

// Standard oversized full-screen triangle covering the whole render target from three vertices.
FullScreenVertex FullScreenVS(uint vertexId : SV_VertexID)
{
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0),
    };
    FullScreenVertex output;
    output.position = float4(positions[min(vertexId, 2u)], 0.0, 1.0);
    return output;
}

// The accumulation loop is expressed once, as a macro, so every variant instantiates *identical* arithmetic.
// A variant only chooses how a wave is fetched (FETCH), never how the field is computed. That is the property
// a controlled A/B timing experiment depends on: byte-for-byte identical output with a single isolated change.
//
// LGP_DEFINE_PROFILING_FIELD(FieldName, FETCH) defines "float FieldName(float2 uv)" where FETCH(i) yields the
// i-th WaveParam from the variant's chosen resource.
#define LGP_DEFINE_PROFILING_FIELD(FieldName, FETCH)                                                                    \
    float FieldName(float2 uv)                                                                                         \
    {                                                                                                                  \
        float field = 0.0;                                                                                             \
        uint iterations = max(gIterations, 1u);                                                                        \
        uint waveCount = min(gWaveCount, uint(LGP_MAX_WAVES));                                                         \
        [loop] for (uint iteration = 0u; iteration < iterations; ++iteration)                                         \
        {                                                                                                              \
            [loop] for (uint waveIndex = 0u; waveIndex < waveCount; ++waveIndex)                                      \
            {                                                                                                          \
                field += WaveContribution(FETCH(waveIndex), uv, iteration);                                           \
            }                                                                                                          \
        }                                                                                                              \
        return field / float(iterations);                                                                             \
    }

#endif // LGP_CH19_PROFILING_LAB_HLSLI
