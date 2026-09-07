#include "BlueNoiseContracts.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ch25::blue_noise
{
namespace
{

[[nodiscard]] bool IsFinite(Float2 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] bool IsUnitInterval(double value) noexcept
{
    return std::isfinite(value) && value >= 0.0 && value < 1.0;
}

// Wraps a value into [0, period) using floored (not truncated) division, so
// negative inputs wrap correctly.
[[nodiscard]] double WrapToPeriod(double value, double period) noexcept
{
    double const remainder = std::fmod(value, period);
    return remainder < 0.0 ? remainder + period : remainder;
}

// --- Void-and-cluster ranked blue-noise tile generation -------------------
//
// Parameters follow Ulichney's original void-and-cluster proposal: a Gaussian
// energy kernel with sigma ~= 1.5, truncated once contributions become
// negligible. Truncating at radius 4 keeps the per-swap update cost small
// (a 9x9 = 81 cell window) while still capturing the meaningful part of the
// kernel (weight at radius 4 is ~0.03 of the center weight).
inline constexpr double kGaussianSigma = 1.5;
inline constexpr std::int32_t kGaussianRadius = 4;
inline constexpr std::uint32_t kGenerationSeed = 0xB1075001U;
inline constexpr std::uint32_t kMaxBalancingIterations = kBlueNoiseTileCellCount;

[[nodiscard]] std::uint32_t WrapTileIndex(std::int32_t x, std::int32_t y) noexcept
{
    std::uint32_t const wrappedX = static_cast<std::uint32_t>(x) & kBlueNoiseTileMask;
    std::uint32_t const wrappedY = static_cast<std::uint32_t>(y) & kBlueNoiseTileMask;
    return (wrappedY * kBlueNoiseTileSize) + wrappedX;
}

[[nodiscard]] double GaussianKernelWeight(std::int32_t dx, std::int32_t dy) noexcept
{
    double const distanceSquared = static_cast<double>((dx * dx) + (dy * dy));
    return std::exp(-distanceSquared / (2.0 * kGaussianSigma * kGaussianSigma));
}

// Adds (sign = +1) or removes (sign = -1) the Gaussian contribution of a point
// at (x, y) to every cell within kGaussianRadius, wrapping toroidally.
void ApplyEnergyContribution(std::array<double, kBlueNoiseTileCellCount> &energy, std::int32_t x, std::int32_t y,
                             double sign) noexcept
{
    for (std::int32_t dy = -kGaussianRadius; dy <= kGaussianRadius; ++dy)
    {
        for (std::int32_t dx = -kGaussianRadius; dx <= kGaussianRadius; ++dx)
        {
            std::uint32_t const index = WrapTileIndex(x + dx, y + dy);
            energy[index] += sign * GaussianKernelWeight(dx, dy);
        }
    }
}

struct TilePosition final
{
    std::int32_t x{};
    std::int32_t y{};
};

[[nodiscard]] TilePosition TilePositionFromIndex(std::uint32_t index) noexcept
{
    return {
        .x = static_cast<std::int32_t>(index % kBlueNoiseTileSize),
        .y = static_cast<std::int32_t>(index / kBlueNoiseTileSize),
    };
}

// Finds the highest-energy cell whose `isOne` flag matches `wantOne`, breaking
// ties at the lowest cell index for full determinism.
[[nodiscard]] std::uint32_t FindExtremeEnergyCell(std::array<double, kBlueNoiseTileCellCount> const &energy,
                                                  std::array<bool, kBlueNoiseTileCellCount> const &isOne, bool wantOne,
                                                  bool wantMaximum) noexcept
{
    std::uint32_t bestIndex = kBlueNoiseTileCellCount;
    double bestEnergy = wantMaximum ? -1.0 : 1.0;
    for (std::uint32_t index = 0U; index < kBlueNoiseTileCellCount; ++index)
    {
        if (isOne[index] != wantOne)
        {
            continue;
        }
        double const candidate = energy[index];
        bool const better =
            bestIndex == kBlueNoiseTileCellCount || (wantMaximum ? candidate > bestEnergy : candidate < bestEnergy);
        if (better)
        {
            bestEnergy = candidate;
            bestIndex = index;
        }
    }
    return bestIndex;
}

struct InitialPattern final
{
    std::array<double, kBlueNoiseTileCellCount> energy{};
    std::array<bool, kBlueNoiseTileCellCount> isOne{};
    std::uint32_t initialOnesCount{};
    BlueNoiseGenerationDiagnostics diagnostics{};
};

// Builds the deterministic seed pattern and balances it with Ulichney's
// void-and-cluster relocation loop:
//   1. Remove the tightest cluster (the "one" with the highest local energy).
//   2. In that updated state, find the largest void (the "zero" with the
//      lowest local energy) and insert a "one" there.
//   3. Recompute the tightest cluster in the fully updated state. If it is
//      the cell just inserted, no single relocation can improve the pattern
//      further, so balancing stops; otherwise the loop continues with a new
//      tightest cluster.
// Every iteration removes exactly one cell and inserts exactly one cell (the
// same cell when the pattern is already balanced), so initialOnesCount is
// invariant across the loop.
[[nodiscard]] InitialPattern BuildBalancedInitialPattern() noexcept
{
    InitialPattern pattern{};

    // Deterministic initial pattern: shuffle cell indices with a fixed-seed
    // PCG stream and take the first n0 as the seed "ones".
    std::array<std::uint32_t, kBlueNoiseTileCellCount> order{};
    for (std::uint32_t index = 0U; index < kBlueNoiseTileCellCount; ++index)
    {
        order[index] = index;
    }
    std::uint32_t randomState = kGenerationSeed;
    auto nextRandom = [&randomState]() noexcept -> std::uint32_t
    {
        randomState += 0x9E3779B9U;
        return PcgHash(randomState);
    };
    for (std::uint32_t index = kBlueNoiseTileCellCount - 1U; index > 0U; --index)
    {
        std::uint32_t const swapIndex = nextRandom() % (index + 1U);
        std::swap(order[index], order[swapIndex]);
    }

    pattern.initialOnesCount = kBlueNoiseTileCellCount / 10U;
    for (std::uint32_t seedIndex = 0U; seedIndex < pattern.initialOnesCount; ++seedIndex)
    {
        std::uint32_t const cellIndex = order[seedIndex];
        pattern.isOne[cellIndex] = true;
        TilePosition const position = TilePositionFromIndex(cellIndex);
        ApplyEnergyContribution(pattern.energy, position.x, position.y, 1.0);
    }

    // Phase 1: balance the initial pattern (see BuildBalancedInitialPattern's
    // comment for the exact relocation and stop rule).
    for (std::uint32_t iteration = 0U; iteration < kMaxBalancingIterations; ++iteration)
    {
        std::uint32_t const tightestCluster = FindExtremeEnergyCell(pattern.energy, pattern.isOne, true, true);

        TilePosition const clusterPosition = TilePositionFromIndex(tightestCluster);
        ApplyEnergyContribution(pattern.energy, clusterPosition.x, clusterPosition.y, -1.0);
        pattern.isOne[tightestCluster] = false;

        std::uint32_t const largestVoid = FindExtremeEnergyCell(pattern.energy, pattern.isOne, false, false);

        TilePosition const voidPosition = TilePositionFromIndex(largestVoid);
        ApplyEnergyContribution(pattern.energy, voidPosition.x, voidPosition.y, 1.0);
        pattern.isOne[largestVoid] = true;

        std::uint32_t const newTightestCluster = FindExtremeEnergyCell(pattern.energy, pattern.isOne, true, true);
        pattern.diagnostics.balancingIterationCount = iteration + 1U;
        if (newTightestCluster == largestVoid)
        {
            pattern.diagnostics.reachedIterationCap = false;
            return pattern;
        }
    }
    pattern.diagnostics.reachedIterationCap = true;
    return pattern;
}

} // namespace

std::uint32_t PcgHash(std::uint32_t value) noexcept
{
    std::uint32_t state = (value * 747796405U) + 2891336453U;
    std::uint32_t const shift = (state >> 28U) + 4U;
    std::uint32_t const word = ((state >> shift) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

double UnitFloatFromBits(std::uint32_t bits) noexcept
{
    constexpr double scale = 1.0 / 4294967296.0;
    return (static_cast<double>(bits) + 0.5) * scale;
}

Float2 WhiteNoiseSample2D(std::uint32_t pixelX, std::uint32_t pixelY, std::uint32_t frameIndex,
                          std::uint32_t seed) noexcept
{
    std::uint32_t const base = seed ^ (pixelX * 0x9E3779B9U) ^ (pixelY * 0x85EBCA6BU) ^ (frameIndex * 0xC2B2AE35U);
    return {
        .x = UnitFloatFromBits(PcgHash(base ^ 0xA511E9B3U)),
        .y = UnitFloatFromBits(PcgHash(base ^ 0x63D83595U)),
    };
}

std::expected<double, ContractError> RadicalInverse(std::uint32_t base, std::uint32_t index) noexcept
{
    if (base < 2U)
    {
        return std::unexpected(ContractError::InvalidBase);
    }

    double result = 0.0;
    double denominator = 1.0;
    double const inverseBase = 1.0 / static_cast<double>(base);
    std::uint32_t remaining = index;
    while (remaining != 0U)
    {
        denominator *= inverseBase;
        std::uint32_t const digit = remaining % base;
        result += static_cast<double>(digit) * denominator;
        remaining /= base;
    }
    return result;
}

double RadicalInverseBase2(std::uint32_t index) noexcept
{
    std::uint32_t reversed = index;
    reversed = (reversed << 16U) | (reversed >> 16U);
    reversed = ((reversed & 0x00FF00FFU) << 8U) | ((reversed & 0xFF00FF00U) >> 8U);
    reversed = ((reversed & 0x0F0F0F0FU) << 4U) | ((reversed & 0xF0F0F0F0U) >> 4U);
    reversed = ((reversed & 0x33333333U) << 2U) | ((reversed & 0xCCCCCCCCU) >> 2U);
    reversed = ((reversed & 0x55555555U) << 1U) | ((reversed & 0xAAAAAAAAU) >> 1U);
    constexpr double scale = 1.0 / 4294967296.0;
    return static_cast<double>(reversed) * scale;
}

std::expected<Float2, ContractError> HaltonSample2D(std::uint32_t index, std::uint32_t baseX,
                                                    std::uint32_t baseY) noexcept
{
    if (baseX < 2U || baseY < 2U)
    {
        return std::unexpected(ContractError::InvalidBase);
    }
    if (baseX == baseY)
    {
        return std::unexpected(ContractError::InvalidBase);
    }

    auto const x = baseX == 2U ? RadicalInverseBase2(index) : *RadicalInverse(baseX, index);
    auto const y = baseY == 2U ? RadicalInverseBase2(index) : *RadicalInverse(baseY, index);
    return Float2{.x = x, .y = y};
}

std::array<std::uint16_t, kBlueNoiseTileCellCount> GenerateBlueNoiseRankTile() noexcept
{
    std::array<std::uint16_t, kBlueNoiseTileCellCount> rank{};

    InitialPattern const balanced = BuildBalancedInitialPattern();

    // Phase 2: rank the initial ones downward by repeatedly removing the
    // tightest remaining cluster.
    {
        std::array<double, kBlueNoiseTileCellCount> phaseEnergy = balanced.energy;
        std::array<bool, kBlueNoiseTileCellCount> phaseIsOne = balanced.isOne;
        for (std::uint32_t remaining = balanced.initialOnesCount; remaining > 0U; --remaining)
        {
            std::uint32_t const tightestCluster = FindExtremeEnergyCell(phaseEnergy, phaseIsOne, true, true);
            rank[tightestCluster] = static_cast<std::uint16_t>(remaining - 1U);
            TilePosition const position = TilePositionFromIndex(tightestCluster);
            ApplyEnergyContribution(phaseEnergy, position.x, position.y, -1.0);
            phaseIsOne[tightestCluster] = false;
        }
    }

    // Phase 3: rank every remaining cell upward by repeatedly filling the
    // largest remaining void.
    {
        std::array<double, kBlueNoiseTileCellCount> phaseEnergy = balanced.energy;
        std::array<bool, kBlueNoiseTileCellCount> phaseIsOne = balanced.isOne;
        for (std::uint32_t nextRank = balanced.initialOnesCount; nextRank < kBlueNoiseTileCellCount; ++nextRank)
        {
            std::uint32_t const largestVoid = FindExtremeEnergyCell(phaseEnergy, phaseIsOne, false, false);
            rank[largestVoid] = static_cast<std::uint16_t>(nextRank);
            TilePosition const position = TilePositionFromIndex(largestVoid);
            ApplyEnergyContribution(phaseEnergy, position.x, position.y, 1.0);
            phaseIsOne[largestVoid] = true;
        }
    }

    return rank;
}

BlueNoiseGenerationDiagnostics DescribeBlueNoiseGenerationBalancing() noexcept
{
    return BuildBalancedInitialPattern().diagnostics;
}

std::span<std::uint16_t const, kBlueNoiseTileCellCount> BlueNoiseTileRanks() noexcept
{
    static std::array<std::uint16_t, kBlueNoiseTileCellCount> const tile = GenerateBlueNoiseRankTile();
    return std::span<std::uint16_t const, kBlueNoiseTileCellCount>(tile);
}

std::uint16_t SampleBlueNoiseTileRank(std::int32_t x, std::int32_t y) noexcept
{
    return BlueNoiseTileRanks()[WrapTileIndex(x, y)];
}

double SampleBlueNoiseTileUnit(std::int32_t x, std::int32_t y) noexcept
{
    double const rank = static_cast<double>(SampleBlueNoiseTileRank(x, y));
    return (rank + 0.5) / static_cast<double>(kBlueNoiseTileCellCount);
}

std::expected<std::uint16_t, ContractError> ScrambleRank(std::uint16_t rank, std::uint32_t frameIndex) noexcept
{
    if (rank >= kBlueNoiseTileCellCount)
    {
        return std::unexpected(ContractError::InvalidRank);
    }

    std::uint32_t const rankMask = kBlueNoiseTileCellCount - 1U; // low 12 bits
    std::uint32_t const frameMask = PcgHash(frameIndex ^ 0x9E3779B9U) & rankMask;
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(rank) ^ frameMask);
}

std::expected<std::uint16_t, ContractError> UnscrambleRank(std::uint16_t scrambledRank,
                                                           std::uint32_t frameIndex) noexcept
{
    // XOR with a fixed mask is its own inverse, so unscrambling is the same
    // operation as scrambling for the same frameIndex.
    return ScrambleRank(scrambledRank, frameIndex);
}

std::expected<Float2, ContractError> ApplyToroidalShift(Float2 unitSample, Float2 shift) noexcept
{
    if (!IsUnitInterval(unitSample.x) || !IsUnitInterval(unitSample.y))
    {
        return std::unexpected(ContractError::InvalidUnitSample);
    }
    if (!IsFinite(shift))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    return Float2{
        .x = WrapToPeriod(unitSample.x + shift.x, 1.0),
        .y = WrapToPeriod(unitSample.y + shift.y, 1.0),
    };
}

std::expected<double, ContractError> EvaluateKernelWeight(ReconstructionKernel kernel, Float2 offset,
                                                          double radius) noexcept
{
    if (!IsFinite(offset))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (!std::isfinite(radius) || radius <= 0.0)
    {
        return std::unexpected(ContractError::InvalidKernelRadius);
    }

    switch (kernel)
    {
    case ReconstructionKernel::Box:
    {
        double const distance = std::sqrt((offset.x * offset.x) + (offset.y * offset.y));
        return distance <= radius ? 1.0 : 0.0;
    }
    case ReconstructionKernel::Tent:
    {
        double const weightX = std::max(0.0, 1.0 - (std::abs(offset.x) / radius));
        double const weightY = std::max(0.0, 1.0 - (std::abs(offset.y) / radius));
        return weightX * weightY;
    }
    case ReconstructionKernel::Gaussian:
    {
        double const distanceSquared = (offset.x * offset.x) + (offset.y * offset.y);
        return std::exp(-distanceSquared / (2.0 * radius * radius));
    }
    default:
        return std::unexpected(ContractError::InvalidKernel);
    }
}

std::expected<ReconstructionWeights, ContractError> NormalizeReconstructionWeights(ReconstructionKernel kernel,
                                                                                   std::span<Float2 const> offsets,
                                                                                   double radius) noexcept
{
    if (offsets.empty())
    {
        return std::unexpected(ContractError::EmptyKernelFootprint);
    }
    if (offsets.size() > kMaxReconstructionKernelSamples)
    {
        return std::unexpected(ContractError::TooManyKernelSamples);
    }

    ReconstructionWeights result{};
    result.sampleCount = static_cast<std::uint32_t>(offsets.size());

    double weightSum = 0.0;
    for (std::uint32_t index = 0U; index < result.sampleCount; ++index)
    {
        auto const weight = EvaluateKernelWeight(kernel, offsets[index], radius);
        if (!weight)
        {
            return std::unexpected(weight.error());
        }
        result.samples[index] = {.offset = offsets[index], .weight = *weight};
        weightSum += *weight;
    }

    if (!(weightSum > 0.0))
    {
        return std::unexpected(ContractError::ZeroKernelWeightSum);
    }

    for (std::uint32_t index = 0U; index < result.sampleCount; ++index)
    {
        result.samples[index].weight /= weightSum;
    }
    return result;
}

std::expected<double, ContractError> ToroidalDistance(Float2 a, Float2 b, double domainSize) noexcept
{
    if (!IsFinite(a) || !IsFinite(b))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (!std::isfinite(domainSize) || domainSize <= 0.0)
    {
        return std::unexpected(ContractError::InvalidDomainSize);
    }

    double const rawDx = std::abs(a.x - b.x);
    double const rawDy = std::abs(a.y - b.y);
    double const dx = std::min(rawDx, domainSize - rawDx);
    double const dy = std::min(rawDy, domainSize - rawDy);
    return std::sqrt((dx * dx) + (dy * dy));
}

std::expected<double, ContractError> MinimumToroidalNearestNeighborDistance(std::span<Float2 const> points,
                                                                            double domainSize) noexcept
{
    if (points.size() < 2U)
    {
        return std::unexpected(ContractError::InvalidPointCount);
    }
    if (points.size() > kMaxPatternPointCount)
    {
        return std::unexpected(ContractError::TooManyPoints);
    }
    if (!std::isfinite(domainSize) || domainSize <= 0.0)
    {
        return std::unexpected(ContractError::InvalidDomainSize);
    }

    double minimumDistance = domainSize; // an upper bound on any toroidal distance
    for (std::size_t i = 0U; i < points.size(); ++i)
    {
        for (std::size_t j = i + 1U; j < points.size(); ++j)
        {
            auto const distance = ToroidalDistance(points[i], points[j], domainSize);
            if (!distance)
            {
                return std::unexpected(distance.error());
            }
            minimumDistance = std::min(minimumDistance, *distance);
        }
    }
    return minimumDistance;
}

std::expected<double, ContractError> LowFrequencyEnergy(std::span<Float2 const> points, double domainSize,
                                                        Float2 frequency) noexcept
{
    if (points.empty())
    {
        return std::unexpected(ContractError::InvalidPointCount);
    }
    if (points.size() > kMaxPatternPointCount)
    {
        return std::unexpected(ContractError::TooManyPoints);
    }
    if (!std::isfinite(domainSize) || domainSize <= 0.0)
    {
        return std::unexpected(ContractError::InvalidDomainSize);
    }
    if (!IsFinite(frequency) || (frequency.x == 0.0 && frequency.y == 0.0))
    {
        return std::unexpected(ContractError::InvalidFrequency);
    }

    double realSum = 0.0;
    double imaginarySum = 0.0;
    for (Float2 const &point : points)
    {
        if (!IsFinite(point))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        double const phase = -2.0 * kPi * (((frequency.x * point.x) + (frequency.y * point.y)) / domainSize);
        realSum += std::cos(phase);
        imaginarySum += std::sin(phase);
    }

    double const count = static_cast<double>(points.size());
    double const magnitudeSquared = (realSum * realSum) + (imaginarySum * imaginarySum);
    return magnitudeSquared / (count * count);
}

} // namespace ch25::blue_noise
