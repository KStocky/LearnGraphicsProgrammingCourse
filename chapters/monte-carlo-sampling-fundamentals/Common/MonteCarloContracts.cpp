#include "MonteCarloContracts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ch16::monte_carlo
{
namespace
{

[[nodiscard]] bool IsFinite(Float3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool IsUnitInterval(double value) noexcept
{
    return std::isfinite(value) && value >= 0.0 && value < 1.0;
}

[[nodiscard]] bool IsValidExponent(double exponent) noexcept
{
    return std::isfinite(exponent) && exponent >= 0.0;
}

[[nodiscard]] bool IsValidMoments(OnlineMoments moments) noexcept
{
    if (!std::isfinite(moments.mean) || !std::isfinite(moments.sumSquaredDifferences) ||
        moments.sumSquaredDifferences < 0.0)
    {
        return false;
    }
    if (moments.count == 0U)
    {
        return moments.mean == 0.0 && moments.sumSquaredDifferences == 0.0;
    }
    return true;
}

} // namespace

std::expected<DirectionSample, ContractError> MapUniformHemisphere(Float2 unitSample) noexcept
{
    if (!IsUnitInterval(unitSample.x) || !IsUnitInterval(unitSample.y))
    {
        return std::unexpected(ContractError::InvalidUnitSample);
    }

    double const z = unitSample.x;
    double const radius = std::sqrt(std::max(0.0, 1.0 - (z * z)));
    double const phi = 2.0 * kPi * unitSample.y;
    return DirectionSample{
        .direction = {radius * std::cos(phi), radius * std::sin(phi), z},
        .density = 1.0 / (2.0 * kPi),
    };
}

std::expected<double, ContractError> UniformHemispherePdf(Float3 direction) noexcept
{
    if (!IsFinite(direction))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    double const lengthSquared =
        (direction.x * direction.x) + (direction.y * direction.y) + (direction.z * direction.z);
    if (direction.z < 0.0 || std::abs(lengthSquared - 1.0) > 1.0e-9)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    return 1.0 / (2.0 * kPi);
}

std::expected<double, ContractError> EvaluatePowerCosineIntegrand(double cosine, double exponent) noexcept
{
    if (!std::isfinite(cosine))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (!IsValidExponent(exponent))
    {
        return std::unexpected(ContractError::InvalidExponent);
    }
    if (cosine < 0.0 || cosine > 1.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    return std::pow(cosine, exponent);
}

std::expected<double, ContractError> EvaluateUniformHemisphereContribution(Float2 unitSample, double exponent) noexcept
{
    auto const mapped = MapUniformHemisphere(unitSample);
    if (!mapped)
    {
        return std::unexpected(mapped.error());
    }
    auto const integrand = EvaluatePowerCosineIntegrand(mapped->direction.z, exponent);
    if (!integrand)
    {
        return std::unexpected(integrand.error());
    }
    if (!std::isfinite(mapped->density) || mapped->density <= 0.0)
    {
        return std::unexpected(ContractError::InvalidDensity);
    }
    return *integrand / mapped->density;
}

std::expected<double, ContractError> ExactPowerCosineIntegral(double exponent) noexcept
{
    if (!IsValidExponent(exponent))
    {
        return std::unexpected(ContractError::InvalidExponent);
    }
    return (2.0 * kPi) / (exponent + 1.0);
}

std::expected<double, ContractError> UniformHemisphereContributionVariance(double exponent) noexcept
{
    if (!IsValidExponent(exponent))
    {
        return std::unexpected(ContractError::InvalidExponent);
    }

    double const secondMoment = (4.0 * kPi * kPi) / ((2.0 * exponent) + 1.0);
    double const exact = (2.0 * kPi) / (exponent + 1.0);
    return std::max(0.0, secondMoment - (exact * exact));
}

std::expected<OnlineMoments, ContractError> PushSample(OnlineMoments moments, double sample) noexcept
{
    if (!IsValidMoments(moments))
    {
        return std::unexpected(ContractError::InvalidMomentState);
    }
    if (!std::isfinite(sample))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (moments.count == std::numeric_limits<std::uint64_t>::max())
    {
        return std::unexpected(ContractError::CountOverflow);
    }

    ++moments.count;
    double const delta = sample - moments.mean;
    moments.mean += delta / static_cast<double>(moments.count);
    double const deltaAfterMeanUpdate = sample - moments.mean;
    moments.sumSquaredDifferences += delta * deltaAfterMeanUpdate;
    if (!IsValidMoments(moments))
    {
        return std::unexpected(ContractError::InvalidMomentState);
    }
    return moments;
}

std::expected<OnlineMoments, ContractError> AccumulateSamples(std::span<double const> samples) noexcept
{
    if (samples.empty())
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }

    OnlineMoments moments{};
    for (double const sample : samples)
    {
        auto const updated = PushSample(moments, sample);
        if (!updated)
        {
            return std::unexpected(updated.error());
        }
        moments = *updated;
    }
    return moments;
}

std::expected<EstimateSummary, ContractError> Summarize(OnlineMoments moments) noexcept
{
    if (!IsValidMoments(moments))
    {
        return std::unexpected(ContractError::InvalidMomentState);
    }
    if (moments.count == 0U)
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }

    double const sampleVariance =
        moments.count > 1U ? moments.sumSquaredDifferences / static_cast<double>(moments.count - 1U) : 0.0;
    return EstimateSummary{
        .sampleCount = moments.count,
        .mean = moments.mean,
        .sampleVariance = sampleVariance,
        .standardError = std::sqrt(sampleVariance / static_cast<double>(moments.count)),
    };
}

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

Float2 DeterministicUnitSample(std::uint32_t seed, std::uint32_t sampleIndex) noexcept
{
    std::uint32_t const base = seed ^ (sampleIndex * 0x9E3779B9U);
    return {
        UnitFloatFromBits(PcgHash(base ^ 0xA511E9B3U)),
        UnitFloatFromBits(PcgHash(base ^ 0x63D83595U)),
    };
}

std::expected<EstimateSummary, ContractError> BuildDeterministicEstimate(double exponent, std::uint32_t sampleCount,
                                                                         std::uint32_t seed) noexcept
{
    if (!IsValidExponent(exponent))
    {
        return std::unexpected(ContractError::InvalidExponent);
    }
    if (sampleCount == 0U || sampleCount > kMaximumReferenceSampleCount)
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }

    OnlineMoments moments{};
    for (std::uint32_t sampleIndex = 0U; sampleIndex < sampleCount; ++sampleIndex)
    {
        auto const contribution =
            EvaluateUniformHemisphereContribution(DeterministicUnitSample(seed, sampleIndex), exponent);
        if (!contribution)
        {
            return std::unexpected(contribution.error());
        }
        auto const updated = PushSample(moments, *contribution);
        if (!updated)
        {
            return std::unexpected(updated.error());
        }
        moments = *updated;
    }
    return Summarize(moments);
}

} // namespace ch16::monte_carlo
