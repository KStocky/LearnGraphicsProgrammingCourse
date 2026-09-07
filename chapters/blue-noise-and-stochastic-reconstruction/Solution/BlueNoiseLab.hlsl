// Chapter 25 Solution: equal-cost sample-address strategies compared.
//
// Every pixel independently draws exactly ONE Bernoulli observation
// (sample < p(x, y)) per technique per frame -- white noise, a temporally
// low-discrepancy sequence, and a deterministic 64x64 ranked blue-noise tile.
// All three cost exactly one evaluation per pixel; blue noise does not
// reduce a single pixel's own Monte Carlo variance, it only changes *where*
// the resulting error is spatially placed relative to its neighbors. That
// only becomes visible after the shared normalized tent-filter reconstruction
// step, which this file also computes for the white and blue techniques.
static const float TwoPi = 6.28318530717958647693;
static const uint TileSize = 64u;
static const uint TileMask = 63u;
static const uint TileCellCount = 4096u;

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
StructuredBuffer<uint> BlueNoiseTile : register(t0);

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

// Bit-reversal radical inverse in base 2 (mirrors
// ch25::blue_noise::RadicalInverseBase2). Converting the reversed 32-bit
// pattern straight to a float loses precision below float's 24-bit mantissa;
// that is an accepted, common GPU approximation (as used for Hammersley
// sequences), not a bit-exact match to the double-precision CPU contract.
float RadicalInverseBase2(uint index)
{
    index = (index << 16u) | (index >> 16u);
    index = ((index & 0x00FF00FFu) << 8u) | ((index & 0xFF00FF00u) >> 8u);
    index = ((index & 0x0F0F0F0Fu) << 4u) | ((index & 0xF0F0F0F0u) >> 4u);
    index = ((index & 0x33333333u) << 2u) | ((index & 0xCCCCCCCCu) >> 2u);
    index = ((index & 0x55555555u) << 1u) | ((index & 0xAAAAAAAAu) >> 1u);
    return float(index) * (1.0 / 4294967296.0);
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

uint WrapCoordinate(uint value, uint offset)
{
    return (value + offset) & TileMask;
}

// Deterministic per-frame toroidal translation of the ranked tile: this
// preserves the tile's spatial pattern and rank histogram (every rank still
// appears exactly once per 64x64 tile) while moving which cell lands on each
// pixel from frame to frame. This is temporal *translation* of a fixed 2D
// pattern -- it is not a true spatiotemporal blue-noise (STBN) sequence, and
// XOR rank-scrambling is deliberately not used here to fake one: the CPU
// contract only proves that scrambling is a bijection that preserves the
// rank histogram, not that arbitrary threshold subsets stay blue-noise-like
// under it.
uint BlueRankAt(uint x, uint y, uint frame)
{
    uint offsetX = (frame * 7u) & TileMask;
    uint offsetY = (frame * 13u) & TileMask;
    uint tileX = WrapCoordinate(x, offsetX);
    uint tileY = WrapCoordinate(y, offsetY);
    return BlueNoiseTile[tileY * TileSize + tileX];
}

float RawWhiteHitAt(uint x, uint y, uint width, uint height, uint frame, uint seed)
{
    uint pixelIndex = y * width + x;
    float unitSample = WhiteUnit(pixelIndex, frame, seed, 0u);
    float exact = ExactProbability(x, y, width, height);
    return unitSample < exact ? 1.0 : 0.0;
}

float RawBlueHitAt(uint x, uint y, uint width, uint height, uint frame)
{
    uint rank = BlueRankAt(x, y, frame);
    float unitSample = (float(rank) + 0.5) / float(TileCellCount);
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

    // Temporal low-discrepancy sequence: Van der Corput indexed by the
    // animation frame, decorrelated per pixel with a fixed (frame-
    // independent) Cranley-Patterson spatial phase.
    float spatialPhase = WhiteUnit(pixelIndex, 0u, Seed, 99u);
    float lowDiscrepancyUnit = frac(RadicalInverseBase2(AnimationFrame) + spatialPhase);
    float lowDiscrepancyHit = lowDiscrepancyUnit < exact ? 1.0 : 0.0;

    uint blueRank = BlueRankAt(x, y, AnimationFrame);
    float blueUnit = (float(blueRank) + 0.5) / float(TileCellCount);
    float blueHit = blueUnit < exact ? 1.0 : 0.0;

    // Normalized 3x3 tent filter, evaluated on the fly by recomputing each
    // clamped-to-edge neighbor's own independent hit for the same technique
    // and frame. This keeps the compute pass single-dispatch with no
    // ping-pong buffer, at the cost of recomputing each neighbor's hash.
    float filteredWhiteSum = 0.0;
    float filteredBlueSum = 0.0;
    float weightSum = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            uint nx = uint(clamp(int(x) + dx, 0, int(Width) - 1));
            uint ny = uint(clamp(int(y) + dy, 0, int(Height) - 1));
            float weight = TentWeight1D(dx) * TentWeight1D(dy);
            filteredWhiteSum += weight * RawWhiteHitAt(nx, ny, Width, Height, AnimationFrame, Seed);
            filteredBlueSum += weight * RawBlueHitAt(nx, ny, Width, Height, AnimationFrame);
            weightSum += weight;
        }
    }
    float filteredWhite = filteredWhiteSum / weightSum;
    float filteredBlue = filteredBlueSum / weightSum;

    bool valid = all(isfinite(float4(exact, whiteUnit, lowDiscrepancyUnit, blueUnit))) &&
                 all(isfinite(float4(whiteHit, lowDiscrepancyHit, blueHit, filteredWhite))) &&
                 isfinite(filteredBlue);

    PixelStatistics statistics;
    statistics.exact = exact;
    statistics.rawWhiteUnit = whiteUnit;
    statistics.rawLowDiscrepancyUnit = lowDiscrepancyUnit;
    statistics.rawBlueUnit = blueUnit;
    statistics.rawWhiteHit = whiteHit;
    statistics.rawLowDiscrepancyHit = lowDiscrepancyHit;
    statistics.rawBlueHit = blueHit;
    statistics.filteredWhite = filteredWhite;
    statistics.filteredBlue = filteredBlue;
    statistics.blueTileRank = blueRank;
    statistics.animationFrame = AnimationFrame;
    statistics.status = valid ? 0x3Fu : 0u; // all six kSolutionValidStatus bits
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

float3 HeatMap(float value)
{
    float t = saturate(value);
    return saturate(float3(1.5 * t, 1.5 - abs(3.0 * t - 1.5), 1.5 * (1.0 - t)));
}

static const uint ViewExact = 0u;
static const uint ViewRawWhite = 1u;
static const uint ViewRawLowDiscrepancy = 2u;
static const uint ViewRawBlue = 3u;
static const uint ViewFilteredWhite = 4u;
static const uint ViewFilteredBlue = 5u;
static const uint ViewRawComparison = 6u;
static const uint ViewFilteredComparison = 7u;
static const uint ViewAbsoluteErrorComparison = 8u;
static const uint ViewPatternDiagnostic = 9u;

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
    uint2 pixel = min(uint2(position.xy), uint2(DisplayWidth - 1u, DisplayHeight - 1u));
    PixelStatistics statistics = DisplayStatistics[pixel.y * DisplayWidth + pixel.x];
    if ((statistics.status & ExpectedStatus) != ExpectedStatus)
    {
        return float4(1.0, 0.0, 1.0, 1.0);
    }

    float exact = max(statistics.exact, 1.0e-4);

    if (DebugView == ViewExact)
    {
        return float4(statistics.exact.xxx, 1.0);
    }
    if (DebugView == ViewRawWhite)
    {
        return float4(statistics.rawWhiteHit.xxx, 1.0);
    }
    if (DebugView == ViewRawLowDiscrepancy)
    {
        return float4(statistics.rawLowDiscrepancyHit.xxx, 1.0);
    }
    if (DebugView == ViewRawBlue)
    {
        return float4(statistics.rawBlueHit.xxx, 1.0);
    }
    if (DebugView == ViewFilteredWhite)
    {
        return float4(statistics.filteredWhite.xxx, 1.0);
    }
    if (DebugView == ViewFilteredBlue)
    {
        return float4(statistics.filteredBlue.xxx, 1.0);
    }
    if (DebugView == ViewRawComparison)
    {
        uint band = min(2u, (pixel.x * 3u) / DisplayWidth);
        uint sourceX = pixel.x * 3u - band * DisplayWidth;
        statistics = DisplayStatistics[pixel.y * DisplayWidth + sourceX];
        float value =
            band == 0u ? statistics.rawWhiteHit : (band == 1u ? statistics.rawLowDiscrepancyHit : statistics.rawBlueHit);
        return float4(value.xxx, 1.0);
    }
    if (DebugView == ViewFilteredComparison)
    {
        uint band = min(1u, (pixel.x * 2u) / DisplayWidth);
        uint sourceX = pixel.x * 2u - band * DisplayWidth;
        statistics = DisplayStatistics[pixel.y * DisplayWidth + sourceX];
        float value = band == 0u ? statistics.filteredWhite : statistics.filteredBlue;
        return float4(value.xxx, 1.0);
    }
    if (DebugView == ViewAbsoluteErrorComparison)
    {
        uint band = min(1u, (pixel.x * 2u) / DisplayWidth);
        uint sourceX = pixel.x * 2u - band * DisplayWidth;
        statistics = DisplayStatistics[pixel.y * DisplayWidth + sourceX];
        float value = band == 0u ? statistics.filteredWhite : statistics.filteredBlue;
        exact = max(statistics.exact, 1.0e-4);
        float error = abs(value - statistics.exact) / exact;
        return float4(HeatMap(error * 4.0), 1.0);
    }

    // ViewPatternDiagnostic: a fixed-size threshold membership field,
    // independent of p(x, y) -- left half is the blue-tile rank thresholded
    // at ThresholdRank, right half is an equal-density white-noise threshold
    // using the same per-pixel unit sample already computed for the raw
    // white technique. This is purely a visual aid; the numeric low-frequency
    // energy comparison in the GPU tests uses an exact equal-count selection
    // from the readback, not this density-matched approximation.
    bool leftHalf = pixel.x * 2u < DisplayWidth;
    uint band = leftHalf ? 0u : 1u;
    uint sourceX = pixel.x * 2u - band * DisplayWidth;
    statistics = DisplayStatistics[pixel.y * DisplayWidth + sourceX];
    if (leftHalf)
    {
        float member = statistics.blueTileRank < ThresholdRank ? 1.0 : 0.0;
        return float4(member.xxx, 1.0);
    }
    float density = float(ThresholdRank) / float(TileCellCount);
    float member = statistics.rawWhiteUnit < density ? 1.0 : 0.0;
    return float4(member.xxx, 1.0);
}
