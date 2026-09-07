#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>

namespace ch25::blue_noise
{

// ===========================================================================
// Chapter contract: white noise vs. low-discrepancy vs. ranked blue noise.
// ---------------------------------------------------------------------------
// This library is the CPU-testable mathematical core shared with the HLSL lab.
// It stays deliberately small and explicit so every algorithm here can be
// re-read as pseudocode for a compute shader:
//   - deterministic integer hashing turns (pixel, frame, seed) into "white"
//     noise with no structure across pixels or frames;
//   - a radical-inverse / Halton sequence gives a low-discrepancy alternative
//     with no per-pixel spatial awareness;
//   - a 64x64 ranked blue-noise tile (generated with an explicit void-and-
//     cluster style algorithm, not an ordered/Bayer dither matrix) gives a
//     spatially-aware sequence: every unit-noise pixel read is one wrapped
//     array lookup, and the tests demonstrate that thresholding the tile at
//     a representative rank produces a point set with larger minimum
//     spacing and less low-frequency energy than white noise of the same
//     count (not a universal proof for every threshold or metric);
//   - per-frame rank scrambling (a bijective XOR permutation over the tile's
//     12-bit rank space) decorrelates the pattern frame-to-frame for temporal
//     accumulation without disturbing the tile's rank histogram;
//   - a small reconstruction kernel library turns scattered offsets into
//     normalized weights, the last step shared by every technique above.
// ===========================================================================

inline constexpr double kPi = 3.141592653589793238462643383279502884;

inline constexpr std::uint32_t kBlueNoiseTileSize = 64U;
inline constexpr std::uint32_t kBlueNoiseTileCellCount = kBlueNoiseTileSize * kBlueNoiseTileSize;
inline constexpr std::uint32_t kBlueNoiseTileMask = kBlueNoiseTileSize - 1U;
inline constexpr std::uint32_t kBlueNoiseTileBitDepth = 12U; // log2(kBlueNoiseTileCellCount)
inline constexpr std::uint32_t kMaxPatternPointCount = kBlueNoiseTileCellCount;
inline constexpr std::uint32_t kMaxReconstructionKernelSamples = 25U; // 5x5 gather footprint

enum class ContractError : std::uint8_t
{
    NonFinite,
    InvalidUnitSample,
    InvalidBase,
    InvalidRank,
    InvalidKernel,
    InvalidKernelRadius,
    EmptyKernelFootprint,
    TooManyKernelSamples,
    ZeroKernelWeightSum,
    InvalidPointCount,
    TooManyPoints,
    InvalidDomainSize,
    InvalidFrequency,
};

enum class ReconstructionKernel : std::uint8_t
{
    Box,
    Tent,
    Gaussian,
};

struct Float2 final
{
    double x{};
    double y{};

    [[nodiscard]] bool operator==(Float2 const &) const noexcept = default;
};

struct KernelSample final
{
    Float2 offset{};
    double weight{};

    [[nodiscard]] bool operator==(KernelSample const &) const noexcept = default;
};

struct ReconstructionWeights final
{
    std::uint32_t sampleCount{};
    std::array<KernelSample, kMaxReconstructionKernelSamples> samples{};

    [[nodiscard]] bool operator==(ReconstructionWeights const &) const noexcept = default;
};

// Diagnostics for the void-and-cluster initial-pattern balancing loop (see
// GenerateBlueNoiseRankTile). Exposed so the balancing behavior for the fixed
// generation seed is directly testable rather than an unobserved implementation
// detail: in particular, that it converges before the hard iteration cap
// instead of silently running the worst-case number of swaps.
struct BlueNoiseGenerationDiagnostics final
{
    std::uint32_t balancingIterationCount{};
    bool reachedIterationCap{};

    [[nodiscard]] bool operator==(BlueNoiseGenerationDiagnostics const &) const noexcept = default;
};

// --- Deterministic integer hashing and white noise ------------------------

[[nodiscard]] std::uint32_t PcgHash(std::uint32_t value) noexcept;
[[nodiscard]] double UnitFloatFromBits(std::uint32_t bits) noexcept;

// Hashes (pixel, frame, seed) into an unstructured 2D sample in [0, 1)^2. Every
// uint32 input is valid: there is no invalid white-noise query.
[[nodiscard]] Float2 WhiteNoiseSample2D(std::uint32_t pixelX, std::uint32_t pixelY, std::uint32_t frameIndex,
                                        std::uint32_t seed) noexcept;

// --- Low-discrepancy sampling ---------------------------------------------

// Van der Corput radical inverse of `index` in the given base. Requires
// base >= 2.
[[nodiscard]] std::expected<double, ContractError> RadicalInverse(std::uint32_t base, std::uint32_t index) noexcept;

// Bit-reversal fast path for base 2; always valid.
[[nodiscard]] double RadicalInverseBase2(std::uint32_t index) noexcept;

// Halton sample pairing two distinct radical-inverse bases (2 and 3 by
// default). Requires baseX != baseY so the two coordinates are not perfectly
// correlated copies of the same sequence.
[[nodiscard]] std::expected<Float2, ContractError> HaltonSample2D(std::uint32_t index, std::uint32_t baseX = 2U,
                                                                  std::uint32_t baseY = 3U) noexcept;

// --- Ranked blue-noise tile -------------------------------------------------

// Generates the deterministic 64x64 ranked blue-noise tile with an explicit
// void-and-cluster style algorithm. A Gaussian "energy" field over the current
// binary pattern is used to balance an initial random pattern (see
// DescribeBlueNoiseGenerationBalancing for that loop's convergence), then to
// rank cells by cluster tightness (below the initial density) and by void size
// (above it). Every cell receives a unique rank in [0, kBlueNoiseTileCellCount).
// This is deterministic (fixed internal seed, no external randomness) and is
// not an ordered/Bayer dither matrix; see the pattern-metric tests for what is
// actually established about the resulting spatial distribution.
[[nodiscard]] std::array<std::uint16_t, kBlueNoiseTileCellCount> GenerateBlueNoiseRankTile() noexcept;

// Runs the same initial-pattern balancing loop as GenerateBlueNoiseRankTile
// (same fixed seed) but returns only its convergence diagnostics. This is
// cheap relative to full tile generation, since it skips the O(N^2) ranking
// phases.
[[nodiscard]] BlueNoiseGenerationDiagnostics DescribeBlueNoiseGenerationBalancing() noexcept;

// Cached accessor: generates the tile once (function-local static) and
// returns a view over it.
[[nodiscard]] std::span<std::uint16_t const, kBlueNoiseTileCellCount> BlueNoiseTileRanks() noexcept;

// Wraps arbitrary (including negative) integer coordinates onto the tile via a
// power-of-two bitmask and returns the rank stored there. Every int32
// coordinate is valid.
[[nodiscard]] std::uint16_t SampleBlueNoiseTileRank(std::int32_t x, std::int32_t y) noexcept;

// Normalized unit-interval form of the tile lookup: (rank + 0.5) / cellCount.
[[nodiscard]] double SampleBlueNoiseTileUnit(std::int32_t x, std::int32_t y) noexcept;

// --- Temporal scrambling and offsetting -----------------------------------

// Permutes a tile rank with a per-frame bijective XOR mask over the tile's
// 12-bit rank space. Because XOR-with-a-fixed-mask is an involution on a
// power-of-two range, applying the same frameIndex twice returns the original
// rank, and the set of scrambled ranks over a full tile is still exactly
// [0, kBlueNoiseTileCellCount): the rank histogram is unchanged, only the
// spatial assignment of ranks to cells changes frame to frame.
[[nodiscard]] std::expected<std::uint16_t, ContractError> ScrambleRank(std::uint16_t rank,
                                                                       std::uint32_t frameIndex) noexcept;

// Inverse of ScrambleRank for the same frameIndex (documented self-inverse).
[[nodiscard]] std::expected<std::uint16_t, ContractError> UnscrambleRank(std::uint16_t scrambledRank,
                                                                         std::uint32_t frameIndex) noexcept;

// Cranley-Patterson toroidal rotation: wraps (unitSample + shift) back into
// [0, 1)^2. Used to re-randomize a low-discrepancy or blue-noise unit sample
// frame to frame without changing its discrepancy properties.
[[nodiscard]] std::expected<Float2, ContractError> ApplyToroidalShift(Float2 unitSample, Float2 shift) noexcept;

// --- Reconstruction kernel weights ----------------------------------------

// Raw (unnormalized) kernel weight for a sample offset from the reconstruction
// center, given a positive filter radius. Box is a flat footprint out to
// radius; Tent is a separable linear falloff to zero at radius; Gaussian uses
// radius as the standard deviation.
[[nodiscard]] std::expected<double, ContractError> EvaluateKernelWeight(ReconstructionKernel kernel, Float2 offset,
                                                                        double radius) noexcept;

// Evaluates EvaluateKernelWeight for every offset and normalizes so the
// weights sum to 1. Fails if the footprint is empty, oversized, or every
// offset falls outside the kernel's support (a zero weight sum).
[[nodiscard]] std::expected<ReconstructionWeights, ContractError> NormalizeReconstructionWeights(
    ReconstructionKernel kernel, std::span<Float2 const> offsets, double radius) noexcept;

// --- Pattern metrics --------------------------------------------------------

// Toroidal (wrap-around) Euclidean distance between two points in a square
// domain of the given size.
[[nodiscard]] std::expected<double, ContractError> ToroidalDistance(Float2 a, Float2 b, double domainSize) noexcept;

// Smallest pairwise toroidal distance across a point set. A larger minimum
// distance indicates a better-separated (blue-noise-like) point set; white
// noise samples of the same count typically cluster and score lower.
[[nodiscard]] std::expected<double, ContractError> MinimumToroidalNearestNeighborDistance(
    std::span<Float2 const> points, double domainSize) noexcept;

// Normalized squared magnitude of the discrete Fourier sum of a point set at a
// single non-zero (fx, fy) frequency, in [0, 1]. Blue-noise point sets suppress
// low-frequency energy relative to white noise of the same count, which is the
// spectral signature this metric is designed to expose.
[[nodiscard]] std::expected<double, ContractError> LowFrequencyEnergy(std::span<Float2 const> points, double domainSize,
                                                                      Float2 frequency) noexcept;

} // namespace ch25::blue_noise
