// Chapter 25 Starter: an honest one-sample-per-pixel baseline.
//
// Every pixel independently draws ONE white-noise Bernoulli observation
// (sample < p(x, y)) per frame -- there is no accumulation across frames and
// no blue-noise or low-discrepancy sampling here. The only reconstruction
// technique available is a normalized 3x3 tent filter applied to the raw
// white-noise hit field. This file intentionally does not populate the
// low-discrepancy or blue-noise fields of PixelStatistics: see Solution for
// those techniques.
static const float TwoPi = 6.28318530717958647693;

struct PixelStatistics
{
    float exact;
    float rawWhiteUnit;
    float rawLowDiscrepancyUnit;
    float rawBlueUnit;
    float rawWhiteHit;
    float rawLowDiscrepancyHit;
    float rawBlueHit;
    float filteredWhite;
    float filteredBlue;
    uint blueTileRank;
    uint animationFrame;
    uint status;
    uint reserved;
};

cbuffer DispatchConstants : register(b0)
{
    uint Width;
    uint Height;
    uint Seed;
    uint AnimationFrame;
};

RWStructuredBuffer<PixelStatistics> Statistics : register(u0);

uint PcgHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float UnitFloat(uint bits)
{
    return (float(bits >> 9u) + 0.5) * (1.0 / 8388608.0);
}

// Mirrors ch25::blue_noise::WhiteNoiseSample2D's hashing shape: an
// independent PCG draw keyed by pixel index, animation frame, seed, and a
// salt so different draws for the same pixel/frame don't correlate.
float WhiteUnit(uint pixelIndex, uint frame, uint seed, uint salt)
{
    uint base = seed ^ (pixelIndex * 0x9E3779B9u) ^ (frame * 0xC2B2AE35u) ^ (salt * 0x85EBCA6Bu);
    return UnitFloat(PcgHash(base ^ 0xA511E9B3u));
}

// The analytically known, slowly varying target field. Every technique
// evaluates this exact function, so the exact expectation is always
// available for error measurement: p in [0.1, 0.9] over the whole image.
float ExactProbability(uint x, uint y, uint width, uint height)
{
    float u = (float(x) + 0.5) / float(width);
    float v = (float(y) + 0.5) / float(height);
    return 0.5 + 0.4 * sin(TwoPi * u) * cos(TwoPi * v);
}

float RawWhiteHitAt(uint x, uint y, uint width, uint height, uint frame, uint seed)
{
    uint pixelIndex = y * width + x;
    float unitSample = WhiteUnit(pixelIndex, frame, seed, 0u);
    float exact = ExactProbability(x, y, width, height);
    return unitSample < exact ? 1.0 : 0.0;
}

float TentWeight1D(int offset)
{
    return offset == 0 ? 2.0 : 1.0;
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

    float exact = ExactProbability(x, y, Width, Height);
    float whiteUnit = WhiteUnit(pixelIndex, AnimationFrame, Seed, 0u);
    float whiteHit = whiteUnit < exact ? 1.0 : 0.0;

    // Normalized 3x3 tent filter, evaluated on the fly by recomputing each
    // clamped-to-edge neighbor's own independent white-noise hit. This keeps
    // the compute pass single-dispatch with no ping-pong buffer.
    float filteredSum = 0.0;
    float weightSum = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            uint nx = uint(clamp(int(x) + dx, 0, int(Width) - 1));
            uint ny = uint(clamp(int(y) + dy, 0, int(Height) - 1));
            float weight = TentWeight1D(dx) * TentWeight1D(dy);
            filteredSum += weight * RawWhiteHitAt(nx, ny, Width, Height, AnimationFrame, Seed);
            weightSum += weight;
        }
    }
    float filteredWhite = filteredSum / weightSum;

    bool valid = all(isfinite(float4(exact, whiteUnit, whiteHit, filteredWhite)));

    PixelStatistics statistics;
    statistics.exact = exact;
    statistics.rawWhiteUnit = whiteUnit;
    statistics.rawLowDiscrepancyUnit = 0.0;
    statistics.rawBlueUnit = 0.0;
    statistics.rawWhiteHit = whiteHit;
    statistics.rawLowDiscrepancyHit = 0.0;
    statistics.rawBlueHit = 0.0;
    statistics.filteredWhite = filteredWhite;
    statistics.filteredBlue = 0.0;
    statistics.blueTileRank = 0u;
    statistics.animationFrame = AnimationFrame;
    statistics.status = valid ? (1u | 2u | 16u) : 0u; // kExactValid | kWhiteValid | kFilteredWhiteValid
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
    uint ThresholdRank;
};

StructuredBuffer<PixelStatistics> DisplayStatistics : register(t0);

static const uint ViewExact = 0u;
static const uint ViewRawWhite = 1u;
static const uint ViewFilteredWhite = 4u;

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
    // The Starter never computes low-discrepancy, blue-noise, or comparison
    // views: rather than silently rendering a placeholder for data it never
    // wrote, it explicitly flags any other requested view as invalid.
    if (DebugView != ViewExact && DebugView != ViewRawWhite && DebugView != ViewFilteredWhite)
    {
        return float4(1.0, 0.0, 1.0, 1.0);
    }

    uint2 pixel = min(uint2(position.xy), uint2(DisplayWidth - 1u, DisplayHeight - 1u));
    PixelStatistics statistics = DisplayStatistics[pixel.y * DisplayWidth + pixel.x];
    if ((statistics.status & ExpectedStatus) != ExpectedStatus)
    {
        return float4(1.0, 0.0, 1.0, 1.0);
    }

    if (DebugView == ViewExact)
    {
        return float4(statistics.exact.xxx, 1.0);
    }
    if (DebugView == ViewRawWhite)
    {
        return float4(statistics.rawWhiteHit.xxx, 1.0);
    }
    return float4(statistics.filteredWhite.xxx, 1.0);
}
