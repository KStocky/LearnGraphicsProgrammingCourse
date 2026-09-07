// Chapter 26 Starter: order-3 real spherical-harmonics baseline.
//
// Every pixel maps to a Y-up, right-handed unit direction (an equirectangular
// projection: row 0 lies near the north pole, and increasing azimuth sweeps
// from +X toward +Z -- see PixelDirection below). The analytic RGB
// Environment() at that direction is a smooth sky/ground gradient plus a single
// bounded, angularly narrow "sun" lobe; order-3 SH cannot represent that lobe faithfully, so its
// reconstruction shows visible truncation/ringing/negative lobes -- that
// truncation error is exactly what this baseline is meant to expose.
//
// This file intentionally only renders the *unrotated* baseline: the source
// environment, its order-3 SH reconstruction, and the absolute error between
// them. It never populates the rotated/irradiance/interpolated/band-contribution
// fields of PixelStatistics: see Solution for those techniques. The SH
// coefficients themselves are never authored here: they arrive in the
// Coefficients cbuffer below, uploaded every frame by the CPU after calling
// ch26::spherical_harmonics::Project (see Common/GpuLabSupport.cpp).
static const float Pi = 3.14159265358979323846f;

struct PixelStatistics
{
    float sourceR;
    float sourceG;
    float sourceB;
    float reconstructionR;
    float reconstructionG;
    float reconstructionB;
    float absoluteErrorR;
    float absoluteErrorG;
    float absoluteErrorB;
    float rotatedReconstructionR;
    float rotatedReconstructionG;
    float rotatedReconstructionB;
    float irradianceR;
    float irradianceG;
    float irradianceB;
    float interpolatedR;
    float interpolatedG;
    float interpolatedB;
    float probeBlendFactor;
    float bandContribution;
    float bandEnergy;
    uint activeBand;
    uint status;
};

// Mirrors gpu::SHCoefficientSetRGB exactly: 16 real-SH coefficients per
// channel, packed 4-per-lane in ascending coefficient-index order.
struct SHCoefficientSetRGB
{
    float4 r[4];
    float4 g[4];
    float4 b[4];
};

cbuffer DispatchConstants : register(b0)
{
    uint Width;
    uint Height;
    uint ActiveBand;
    uint ReservedDispatch;
};

// The CPU uploads the full gpu::SHCoefficientBundle (base/rotated/irradiance)
// every frame so the Starter and Solution share one root signature and one
// upload path; this baseline only ever reads the leading Base member.
cbuffer Coefficients : register(b1)
{
    SHCoefficientSetRGB Base;
    SHCoefficientSetRGB Rotated;
    SHCoefficientSetRGB Irradiance;
    float4 BandEnergy;
};

RWStructuredBuffer<PixelStatistics> Statistics : register(u0);

// Maps a screen pixel to a Y-up, right-handed unit direction using the same
// equirectangular convention as ch26::spherical_harmonics::BuildLatitudeLongitudeSamples
// and gpu::PixelDirection (see Common/GpuLabSupport.cpp/hpp). Half-texel
// offsets keep every pixel center away from both poles; increasing azimuth
// sweeps from +X toward +Z.
float3 PixelDirection(uint x, uint y, uint width, uint height)
{
    float theta = Pi * ((float) y + 0.5) / (float) height;
    float azimuth = 2.0 * Pi * ((float) x + 0.5) / (float) width;
    float cosTheta = cos(theta);
    float sinTheta = sin(theta);
    return float3(sinTheta * cos(azimuth), cosTheta, sinTheta * sin(azimuth));
}

float SmoothStepD(float edge0, float edge1, float value)
{
    float t = saturate((value - edge0) / (edge1 - edge0));
    return t * t * (3.0 - (2.0 * t));
}

// Mirrors gpu::EvaluateEnvironment exactly (float precision here; the CPU
// side uses double precision to build the projection samples).
float3 Environment(float3 direction)
{
    static const float3 SkyZenith = float3(0.30, 0.50, 0.85);
    static const float3 SkyHorizon = float3(0.80, 0.80, 0.70);
    static const float3 GroundColor = float3(0.20, 0.17, 0.14);
    static const float3 SunColor = float3(1.00, 0.86, 0.68);
    static const float SunIntensity = 8.0;
    static const float SunSharpness = 48.0;

    float skyFactor = SmoothStepD(-0.2, 0.6, direction.y);
    float3 color = lerp(SkyHorizon, SkyZenith, skyFactor);
    float groundFactor = SmoothStepD(0.05, -0.05, direction.y);
    color = lerp(color, GroundColor, groundFactor);

    float3 sunDirection = normalize(float3(0.35, 0.65, -0.60));
    float cosineToSun = saturate(dot(direction, sunDirection));
    float lobe = pow(cosineToSun, SunSharpness);
    color += SunColor * SunIntensity * lobe;
    return color;
}

// The 16 real-SH basis values at `d`, packed into the same 4-lane layout as
// SHCoefficientSetRGB so reconstruction can use dot(). Each constant/expression
// below mirrors ch26::spherical_harmonics::EvaluateRealBasis (see
// SphericalHarmonicsContracts.cpp): these are the well-known, universal
// real-SH normalization constants, not per-environment "magic" coefficients
// -- the environment's own SH coefficients always come from the CPU
// projection contract, never from this basis evaluation.
struct SHBasisLanes
{
    float4 lane0;
    float4 lane1;
    float4 lane2;
    float4 lane3;
};

SHBasisLanes EvaluateBasisLanes(float3 d)
{
    float x = d.x;
    float y = d.y;
    float z = d.z;

    SHBasisLanes basis;
    basis.lane0 = float4(
        0.282095,
        -0.488603 * z,
        0.488603 * y,
        -0.488603 * x);
    basis.lane1 = float4(
        1.092548 * x * z,
        -1.092548 * y * z,
        0.315392 * ((3.0 * y * y) - 1.0),
        -1.092548 * x * y);
    basis.lane2 = float4(
        0.546274 * ((x * x) - (z * z)),
        -0.590044 * z * ((3.0 * x * x) - (z * z)),
        2.890611 * x * y * z,
        0.457046 * z * (1.0 - (5.0 * y * y)));
    basis.lane3 = float4(
        0.373176 * y * ((5.0 * y * y) - 3.0),
        0.457046 * x * (1.0 - (5.0 * y * y)),
        1.445306 * y * ((x * x) - (z * z)),
        -0.590044 * x * ((x * x) - (3.0 * z * z)));
    return basis;
}

float FullReconstructChannel(float4 coefficientLanes[4], SHBasisLanes basis)
{
    return dot(coefficientLanes[0], basis.lane0) + dot(coefficientLanes[1], basis.lane1) +
           dot(coefficientLanes[2], basis.lane2) + dot(coefficientLanes[3], basis.lane3);
}

[numthreads(8, 8, 1)]
void SampleCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Width || dispatchThreadId.y >= Height)
    {
        return;
    }

    uint x = dispatchThreadId.x;
    uint y = dispatchThreadId.y;
    uint pixelIndex = y * Width + x;

    float3 direction = PixelDirection(x, y, Width, Height);
    float3 source = Environment(direction);
    SHBasisLanes basis = EvaluateBasisLanes(direction);

    float3 reconstruction = float3(
        FullReconstructChannel(Base.r, basis),
        FullReconstructChannel(Base.g, basis),
        FullReconstructChannel(Base.b, basis));
    float3 absoluteError = abs(reconstruction - source);

    bool valid = all(isfinite(source)) && all(isfinite(reconstruction)) && all(isfinite(absoluteError));

    PixelStatistics statistics;
    statistics.sourceR = source.x;
    statistics.sourceG = source.y;
    statistics.sourceB = source.z;
    statistics.reconstructionR = reconstruction.x;
    statistics.reconstructionG = reconstruction.y;
    statistics.reconstructionB = reconstruction.z;
    statistics.absoluteErrorR = absoluteError.x;
    statistics.absoluteErrorG = absoluteError.y;
    statistics.absoluteErrorB = absoluteError.z;
    statistics.rotatedReconstructionR = 0.0;
    statistics.rotatedReconstructionG = 0.0;
    statistics.rotatedReconstructionB = 0.0;
    statistics.irradianceR = 0.0;
    statistics.irradianceG = 0.0;
    statistics.irradianceB = 0.0;
    statistics.interpolatedR = 0.0;
    statistics.interpolatedG = 0.0;
    statistics.interpolatedB = 0.0;
    statistics.probeBlendFactor = 0.0;
    statistics.bandContribution = 0.0;
    statistics.bandEnergy = 0.0;
    statistics.activeBand = ActiveBand;
    statistics.status = valid ? (1u | 2u | 4u) : 0u; // kSourceValid | kReconstructionValid | kErrorValid.
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

static const uint ViewSource = 0u;
static const uint ViewReconstruction = 1u;
static const uint ViewAbsoluteError = 2u;

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
    // The Starter never computes the rotated/irradiance/interpolated/
    // band-contribution views: rather than silently rendering a placeholder
    // for data it never wrote, it explicitly flags any other requested view
    // as invalid.
    if (DebugView != ViewSource && DebugView != ViewReconstruction && DebugView != ViewAbsoluteError)
    {
        return float4(1.0, 0.0, 1.0, 1.0);
    }

    uint2 pixel = min(uint2(position.xy), uint2(DisplayWidth - 1u, DisplayHeight - 1u));
    PixelStatistics statistics = DisplayStatistics[pixel.y * DisplayWidth + pixel.x];
    if ((statistics.status & ExpectedStatus) != ExpectedStatus)
    {
        return float4(1.0, 0.0, 1.0, 1.0);
    }

    if (DebugView == ViewSource)
    {
        return float4(statistics.sourceR, statistics.sourceG, statistics.sourceB, 1.0);
    }
    if (DebugView == ViewReconstruction)
    {
        return float4(statistics.reconstructionR, statistics.reconstructionG, statistics.reconstructionB, 1.0);
    }
    return float4(saturate(float3(statistics.absoluteErrorR, statistics.absoluteErrorG, statistics.absoluteErrorB)),
                 1.0);
}
