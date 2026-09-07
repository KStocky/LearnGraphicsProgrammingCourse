// Chapter 26 Solution: order-3 real spherical-harmonics lighting.
//
// Every pixel maps to a Y-up, right-handed unit direction (an equirectangular
// projection: row 0 lies near the north pole, and increasing azimuth sweeps
// from +X toward +Z -- see PixelDirection below). The analytic RGB
// Environment() at that direction is a smooth sky/ground gradient plus a single
// bounded, angularly narrow "sun" lobe; order-3 SH cannot represent that lobe faithfully, so its
// reconstruction shows visible truncation/ringing/negative lobes by design.
//
// The SH coefficients themselves are never authored here: they arrive in the
// Coefficients cbuffer below, uploaded every frame by the CPU after calling
// ch26::spherical_harmonics::Project/Rotate/ConvolveClampedCosine (see
// Common/GpuLabSupport.cpp). This file only evaluates the real-SH basis and
// reconstructs colors from those coefficients.
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

// Mirrors gpu::SHCoefficientBundle exactly: base (unrotated, "probe A"),
// rotated (CPU coefficient-space rotation via ch26::spherical_harmonics::Rotate,
// "probe B"), and irradiance (ConvolveClampedCosine(base)) coefficient sets.
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
// side uses double precision to build the projection samples). A smooth
// sky/ground gradient (effectively axisymmetric about Y) plus a bounded,
// angularly narrow sun lobe placed off-axis so its energy spreads into every
// band once projected.
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
// SHCoefficientSetRGB so full/partial reconstructions can use dot(). Each
// constant/expression below mirrors ch26::spherical_harmonics::EvaluateRealBasis
// (see SphericalHarmonicsContracts.cpp): these are the well-known, universal
// real-SH normalization constants (identical to the ones cross-checked
// against that contract's own unit tests for bands 0-1), not per-environment
// "magic" coefficients -- the environment's own SH coefficients always come
// from the CPU projection contract, never from this basis evaluation.
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
    // Band 0 (idx 0) + band 1 (idx 1..3).
    basis.lane0 = float4(
        0.282095,
        -0.488603 * z,
        0.488603 * y,
        -0.488603 * x);
    // Band 2, idx 4..7.
    basis.lane1 = float4(
        1.092548 * x * z,
        -1.092548 * y * z,
        0.315392 * ((3.0 * y * y) - 1.0),
        -1.092548 * x * y);
    // Band 2 idx 8 + band 3 idx 9..11.
    basis.lane2 = float4(
        0.546274 * ((x * x) - (z * z)),
        -0.590044 * z * ((3.0 * x * x) - (z * z)),
        2.890611 * x * y * z,
        0.457046 * z * (1.0 - (5.0 * y * y)));
    // Band 3, idx 12..15.
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

// Isolates a single band's contribution (its own subset of the 16 terms),
// so DebugView::BandContribution can visualize/measure each band's energy
// independently. Summing all four bands' contributions must reproduce
// FullReconstructChannel exactly (linearity), which the GPU tests verify.
float BandContributionChannel(float4 coefficientLanes[4], SHBasisLanes basis, uint band)
{
    if (band == 0u)
    {
        return coefficientLanes[0].x * basis.lane0.x;
    }
    if (band == 1u)
    {
        return dot(coefficientLanes[0].yzw, basis.lane0.yzw);
    }
    if (band == 2u)
    {
        return dot(coefficientLanes[1], basis.lane1) + (coefficientLanes[2].x * basis.lane2.x);
    }
    return dot(coefficientLanes[2].yzw, basis.lane2.yzw) + dot(coefficientLanes[3], basis.lane3);
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

    float3 rotatedReconstruction = float3(
        FullReconstructChannel(Rotated.r, basis),
        FullReconstructChannel(Rotated.g, basis),
        FullReconstructChannel(Rotated.b, basis));

    float3 irradiance = float3(
        FullReconstructChannel(Irradiance.r, basis),
        FullReconstructChannel(Irradiance.g, basis),
        FullReconstructChannel(Irradiance.b, basis));

    // Two-probe interpolation: linearly blend the base and rotated
    // *coefficient sets* (before any HLSL evaluation), with an obvious
    // bounded screen-space experiment -- the blend factor sweeps 0..1 across
    // the width of the image, left = probe A (base), right = probe B
    // (rotated).
    float blendFactor = saturate(((float) x + 0.5) / (float) Width);
    float4 blendedR[4];
    float4 blendedG[4];
    float4 blendedB[4];
    [unroll]
    for (uint lane = 0u; lane < 4u; ++lane)
    {
        blendedR[lane] = lerp(Base.r[lane], Rotated.r[lane], blendFactor);
        blendedG[lane] = lerp(Base.g[lane], Rotated.g[lane], blendFactor);
        blendedB[lane] = lerp(Base.b[lane], Rotated.b[lane], blendFactor);
    }
    float3 interpolated = float3(
        FullReconstructChannel(blendedR, basis),
        FullReconstructChannel(blendedG, basis),
        FullReconstructChannel(blendedB, basis));

    float bandR = BandContributionChannel(Base.r, basis, ActiveBand);
    float bandG = BandContributionChannel(Base.g, basis, ActiveBand);
    float bandB = BandContributionChannel(Base.b, basis, ActiveBand);
    float bandContribution = dot(float3(bandR, bandG, bandB), float3(0.2126, 0.7152, 0.0722));

    bool valid = all(isfinite(source)) && all(isfinite(reconstruction)) && all(isfinite(absoluteError)) &&
                 all(isfinite(rotatedReconstruction)) && all(isfinite(irradiance)) && all(isfinite(interpolated)) &&
                 isfinite(bandContribution) && isfinite(blendFactor);

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
    statistics.rotatedReconstructionR = rotatedReconstruction.x;
    statistics.rotatedReconstructionG = rotatedReconstruction.y;
    statistics.rotatedReconstructionB = rotatedReconstruction.z;
    statistics.irradianceR = irradiance.x;
    statistics.irradianceG = irradiance.y;
    statistics.irradianceB = irradiance.z;
    statistics.interpolatedR = interpolated.x;
    statistics.interpolatedG = interpolated.y;
    statistics.interpolatedB = interpolated.z;
    statistics.probeBlendFactor = blendFactor;
    statistics.bandContribution = bandContribution;
    statistics.bandEnergy = BandEnergy[ActiveBand];
    statistics.activeBand = ActiveBand;
    statistics.status = valid ? 0x7Fu : 0u; // kSolutionValidStatus: all seven bits.
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
static const uint ViewRotatedReconstruction = 3u;
static const uint ViewIrradiance = 4u;
static const uint ViewInterpolated = 5u;
static const uint ViewBandContribution = 6u;

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
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
    if (DebugView == ViewAbsoluteError)
    {
        return float4(saturate(float3(statistics.absoluteErrorR, statistics.absoluteErrorG, statistics.absoluteErrorB)),
                     1.0);
    }
    if (DebugView == ViewRotatedReconstruction)
    {
        return float4(statistics.rotatedReconstructionR, statistics.rotatedReconstructionG,
                     statistics.rotatedReconstructionB, 1.0);
    }
    if (DebugView == ViewIrradiance)
    {
        return float4(statistics.irradianceR, statistics.irradianceG, statistics.irradianceB, 1.0);
    }
    if (DebugView == ViewInterpolated)
    {
        return float4(statistics.interpolatedR, statistics.interpolatedG, statistics.interpolatedB, 1.0);
    }
    // ViewBandContribution: the left 75% is a signed spatial contribution
    // around neutral gray; the right 25% is the selected band's scalar
    // luminance-coefficient energy.
    if (position.x < 0.75 * (float) DisplayWidth)
    {
        return float4(saturate(0.5 + (0.25 * statistics.bandContribution)).xxx, 1.0);
    }
    float energy = 1.0 - exp(-statistics.bandEnergy);
    return float4(energy.xxx, 1.0);
}
