#include "ImportanceSamplingContracts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ch17::importance_sampling
{
namespace
{

inline constexpr double kDirectionLengthTolerance = 1.0e-9;

struct ScaledWeightedDensities final
{
    double first{};
    double second{};
};

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

[[nodiscard]] std::expected<double, ContractError> ValidateHemisphereDirection(Float3 direction) noexcept
{
    if (!IsFinite(direction))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    double const lengthSquared =
        (direction.x * direction.x) + (direction.y * direction.y) + (direction.z * direction.z);
    if (!std::isfinite(lengthSquared) || direction.z < 0.0 || direction.z > 1.0 ||
        std::abs(lengthSquared - 1.0) > kDirectionLengthTolerance)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    return direction.z;
}

[[nodiscard]] std::expected<double, ContractError> PowerCosineDensityFromCosine(double cosine,
                                                                                double proposalExponent) noexcept
{
    if (!IsValidExponent(proposalExponent))
    {
        return std::unexpected(ContractError::InvalidExponent);
    }
    if (!std::isfinite(cosine))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (cosine < 0.0 || cosine > 1.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }

    double cosinePower = 1.0;
    if (cosine == 0.0 && proposalExponent > 0.0)
    {
        cosinePower = 0.0;
    }
    else if (proposalExponent > 0.0)
    {
        cosinePower = std::pow(cosine, proposalExponent);
    }

    double const density = ((proposalExponent + 1.0) / (2.0 * kPi)) * cosinePower;
    if (!std::isfinite(density) || density < 0.0)
    {
        return std::unexpected(ContractError::InvalidDensity);
    }
    return density;
}

[[nodiscard]] std::expected<ScaledWeightedDensities, ContractError> ScaleWeightedDensities(
    ProposalDensity first, ProposalDensity second) noexcept
{
    if (!std::isfinite(first.density) || first.density < 0.0 || !std::isfinite(second.density) || second.density < 0.0)
    {
        return std::unexpected(ContractError::InvalidDensity);
    }
    if (first.sampleCount == 0U || second.sampleCount == 0U)
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }

    double const densityScale = std::max(first.density, second.density);
    if (densityScale == 0.0)
    {
        return std::unexpected(ContractError::ZeroCombinedDensity);
    }

    double const firstWeighted = static_cast<double>(first.sampleCount) * (first.density / densityScale);
    double const secondWeighted = static_cast<double>(second.sampleCount) * (second.density / densityScale);
    if (!std::isfinite(firstWeighted) || firstWeighted < 0.0 || !std::isfinite(secondWeighted) ||
        secondWeighted < 0.0 || (firstWeighted == 0.0 && secondWeighted == 0.0))
    {
        return std::unexpected(ContractError::ZeroCombinedDensity);
    }
    return ScaledWeightedDensities{.first = firstWeighted, .second = secondWeighted};
}

[[nodiscard]] std::expected<MisWeights, ContractError> NormalizeWeights(double first, double second) noexcept
{
    double const sum = first + second;
    if (!std::isfinite(first) || first < 0.0 || !std::isfinite(second) || second < 0.0 || !std::isfinite(sum) ||
        sum <= 0.0)
    {
        return std::unexpected(ContractError::ZeroCombinedDensity);
    }

    double const firstWeight = first / sum;
    double const secondWeight = 1.0 - firstWeight;
    if (!std::isfinite(firstWeight) || firstWeight < 0.0 || firstWeight > 1.0 || !std::isfinite(secondWeight) ||
        secondWeight < 0.0 || secondWeight > 1.0)
    {
        return std::unexpected(ContractError::ZeroCombinedDensity);
    }
    return MisWeights{.first = firstWeight, .second = secondWeight};
}

[[nodiscard]] std::expected<MisWeights, ContractError> SelectMisWeights(MisHeuristic heuristic, ProposalDensity first,
                                                                        ProposalDensity second,
                                                                        double powerBeta) noexcept
{
    switch (heuristic)
    {
    case MisHeuristic::Balance:
        return BalanceHeuristicWeights(first, second);
    case MisHeuristic::Power:
        return PowerHeuristicWeights(first, second, powerBeta);
    default:
        return std::unexpected(ContractError::InvalidHeuristic);
    }
}

[[nodiscard]] std::expected<void, ContractError> ValidateReferenceSampleCount(std::uint32_t sampleCount) noexcept
{
    if (sampleCount == 0U || sampleCount > kMaximumReferenceSampleCount)
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateMisConfiguration(MisHeuristic heuristic,
                                                                          double powerBeta) noexcept
{
    switch (heuristic)
    {
    case MisHeuristic::Balance:
        return {};
    case MisHeuristic::Power:
        if (!std::isfinite(powerBeta) || powerBeta <= 0.0)
        {
            return std::unexpected(ContractError::InvalidHeuristicExponent);
        }
        return {};
    default:
        return std::unexpected(ContractError::InvalidHeuristic);
    }
}

} // namespace

std::expected<DirectionSample, ContractError> MapUniformHemisphere(Float2 unitSample) noexcept
{
    if (!IsUnitInterval(unitSample.x) || !IsUnitInterval(unitSample.y))
    {
        return std::unexpected(ContractError::InvalidUnitSample);
    }

    double const z = unitSample.x;
    double const radiusSquared = 1.0 - (z * z);
    if (radiusSquared < 0.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }

    double const radius = std::sqrt(radiusSquared);
    double const phi = 2.0 * kPi * unitSample.y;
    return DirectionSample{
        .direction = {radius * std::cos(phi), radius * std::sin(phi), z},
        .density = 1.0 / (2.0 * kPi),
    };
}

std::expected<double, ContractError> UniformHemispherePdf(Float3 direction) noexcept
{
    auto const cosine = ValidateHemisphereDirection(direction);
    if (!cosine)
    {
        return std::unexpected(cosine.error());
    }
    return 1.0 / (2.0 * kPi);
}

std::expected<DirectionSample, ContractError> MapPowerCosineHemisphere(Float2 unitSample,
                                                                       double proposalExponent) noexcept
{
    if (!IsUnitInterval(unitSample.x) || !IsUnitInterval(unitSample.y))
    {
        return std::unexpected(ContractError::InvalidUnitSample);
    }
    if (!IsValidExponent(proposalExponent))
    {
        return std::unexpected(ContractError::InvalidExponent);
    }

    double const inverseExponent = 1.0 / (proposalExponent + 1.0);
    double const z = unitSample.x == 0.0 ? 0.0 : std::pow(unitSample.x, inverseExponent);
    if (!std::isfinite(z) || z < 0.0 || z > 1.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }

    double const radiusSquared = 1.0 - (z * z);
    if (radiusSquared < 0.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }

    auto const density = PowerCosineDensityFromCosine(z, proposalExponent);
    if (!density)
    {
        return std::unexpected(density.error());
    }

    double const radius = std::sqrt(radiusSquared);
    double const phi = 2.0 * kPi * unitSample.y;
    return DirectionSample{
        .direction = {radius * std::cos(phi), radius * std::sin(phi), z},
        .density = *density,
    };
}

std::expected<double, ContractError> PowerCosineHemispherePdf(Float3 direction, double proposalExponent) noexcept
{
    auto const cosine = ValidateHemisphereDirection(direction);
    if (!cosine)
    {
        return std::unexpected(cosine.error());
    }
    return PowerCosineDensityFromCosine(*cosine, proposalExponent);
}

std::expected<double, ContractError> EvaluatePowerCosineTarget(double cosine, double targetExponent) noexcept
{
    if (!std::isfinite(cosine))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (!IsValidExponent(targetExponent))
    {
        return std::unexpected(ContractError::InvalidExponent);
    }
    if (cosine < 0.0 || cosine > 1.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    if (targetExponent == 0.0)
    {
        return 1.0;
    }
    if (cosine == 0.0)
    {
        return 0.0;
    }

    double const targetValue = std::pow(cosine, targetExponent);
    if (!std::isfinite(targetValue) || targetValue < 0.0)
    {
        return std::unexpected(ContractError::InvalidTargetValue);
    }
    return targetValue;
}

std::expected<double, ContractError> ExactPowerCosineIntegral(double targetExponent) noexcept
{
    if (!IsValidExponent(targetExponent))
    {
        return std::unexpected(ContractError::InvalidExponent);
    }
    return (2.0 * kPi) / (targetExponent + 1.0);
}

std::expected<double, ContractError> EvaluateProposalContribution(double targetValue, double selectedDensity) noexcept
{
    if (!std::isfinite(targetValue))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (targetValue < 0.0)
    {
        return std::unexpected(ContractError::InvalidTargetValue);
    }
    if (!std::isfinite(selectedDensity) || selectedDensity < 0.0)
    {
        return std::unexpected(ContractError::InvalidDensity);
    }
    if (selectedDensity == 0.0)
    {
        if (targetValue > 0.0)
        {
            return std::unexpected(ContractError::ZeroDensityForNonzeroTarget);
        }
        return 0.0;
    }

    double const contribution = targetValue / selectedDensity;
    if (!std::isfinite(contribution))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return contribution;
}

std::expected<double, ContractError> EvaluateUniformContribution(Float2 unitSample, double targetExponent) noexcept
{
    auto const mapped = MapUniformHemisphere(unitSample);
    if (!mapped)
    {
        return std::unexpected(mapped.error());
    }

    auto const targetValue = EvaluatePowerCosineTarget(mapped->direction.z, targetExponent);
    if (!targetValue)
    {
        return std::unexpected(targetValue.error());
    }
    return EvaluateProposalContribution(*targetValue, mapped->density);
}

std::expected<double, ContractError> EvaluateImportanceContribution(Float2 unitSample, double targetExponent,
                                                                    double proposalExponent) noexcept
{
    auto const mapped = MapPowerCosineHemisphere(unitSample, proposalExponent);
    if (!mapped)
    {
        return std::unexpected(mapped.error());
    }

    auto const targetValue = EvaluatePowerCosineTarget(mapped->direction.z, targetExponent);
    if (!targetValue)
    {
        return std::unexpected(targetValue.error());
    }
    return EvaluateProposalContribution(*targetValue, mapped->density);
}

std::expected<double, ContractError> PowerCosineContributionVariance(double targetExponent,
                                                                     double proposalExponent) noexcept
{
    if (!IsValidExponent(targetExponent) || !IsValidExponent(proposalExponent))
    {
        return std::unexpected(ContractError::InvalidExponent);
    }

    double const finiteVarianceBoundary = (proposalExponent - 1.0) * 0.5;
    if (!(targetExponent > finiteVarianceBoundary))
    {
        return std::unexpected(ContractError::NonFiniteVariance);
    }

    double const tailExponent = ((2.0 * targetExponent) - proposalExponent) + 1.0;
    if (std::isfinite(tailExponent) && tailExponent <= 0.0)
    {
        return std::unexpected(ContractError::NonFiniteVariance);
    }
    if (std::isnan(tailExponent) || tailExponent == -std::numeric_limits<double>::infinity())
    {
        return std::unexpected(ContractError::NonFiniteVariance);
    }

    double secondMoment = (4.0 * kPi * kPi) / (proposalExponent + 1.0);
    if (tailExponent == std::numeric_limits<double>::infinity())
    {
        secondMoment = 0.0;
    }
    else
    {
        secondMoment /= tailExponent;
    }

    auto const exact = ExactPowerCosineIntegral(targetExponent);
    if (!exact)
    {
        return std::unexpected(exact.error());
    }
    double const exactSquared = *exact * *exact;
    if (!std::isfinite(secondMoment) || secondMoment < 0.0 || !std::isfinite(exactSquared))
    {
        return std::unexpected(ContractError::NonFiniteVariance);
    }

    double variance = secondMoment - exactSquared;
    if (!std::isfinite(variance))
    {
        return std::unexpected(ContractError::NonFiniteVariance);
    }
    if (variance < 0.0)
    {
        double const roundoffTolerance =
            64.0 * std::numeric_limits<double>::epsilon() * std::max(secondMoment, exactSquared);
        if (-variance > roundoffTolerance)
        {
            return std::unexpected(ContractError::NonFiniteVariance);
        }
        variance = 0.0;
    }
    return variance;
}

std::expected<MisWeights, ContractError> BalanceHeuristicWeights(ProposalDensity first, ProposalDensity second) noexcept
{
    auto const weighted = ScaleWeightedDensities(first, second);
    if (!weighted)
    {
        return std::unexpected(weighted.error());
    }
    return NormalizeWeights(weighted->first, weighted->second);
}

std::expected<MisWeights, ContractError> PowerHeuristicWeights(ProposalDensity first, ProposalDensity second,
                                                               double beta) noexcept
{
    if (!std::isfinite(beta) || beta <= 0.0)
    {
        return std::unexpected(ContractError::InvalidHeuristicExponent);
    }

    auto const weighted = ScaleWeightedDensities(first, second);
    if (!weighted)
    {
        return std::unexpected(weighted.error());
    }

    double const weightedScale = std::max(weighted->first, weighted->second);
    double const firstPower = std::pow(weighted->first / weightedScale, beta);
    double const secondPower = std::pow(weighted->second / weightedScale, beta);
    return NormalizeWeights(firstPower, secondPower);
}

std::expected<MisEstimateDiagnostics, ContractError> EvaluateMisEstimate(Float2 uniformUnitSample,
                                                                         Float2 importanceUnitSample,
                                                                         double targetExponent, double proposalExponent,
                                                                         MisHeuristic heuristic,
                                                                         double powerBeta) noexcept
{
    auto const uniformSample = MapUniformHemisphere(uniformUnitSample);
    if (!uniformSample)
    {
        return std::unexpected(uniformSample.error());
    }
    auto const uniformTarget = EvaluatePowerCosineTarget(uniformSample->direction.z, targetExponent);
    if (!uniformTarget)
    {
        return std::unexpected(uniformTarget.error());
    }
    auto const uniformCompetingDensity = PowerCosineHemispherePdf(uniformSample->direction, proposalExponent);
    if (!uniformCompetingDensity)
    {
        return std::unexpected(uniformCompetingDensity.error());
    }
    auto const uniformContribution = EvaluateProposalContribution(*uniformTarget, uniformSample->density);
    if (!uniformContribution)
    {
        return std::unexpected(uniformContribution.error());
    }
    auto const uniformWeights =
        SelectMisWeights(heuristic, {uniformSample->density, 1U}, {*uniformCompetingDensity, 1U}, powerBeta);
    if (!uniformWeights)
    {
        return std::unexpected(uniformWeights.error());
    }

    auto const importanceSample = MapPowerCosineHemisphere(importanceUnitSample, proposalExponent);
    if (!importanceSample)
    {
        return std::unexpected(importanceSample.error());
    }
    auto const importanceTarget = EvaluatePowerCosineTarget(importanceSample->direction.z, targetExponent);
    if (!importanceTarget)
    {
        return std::unexpected(importanceTarget.error());
    }
    auto const importanceCompetingDensity = UniformHemispherePdf(importanceSample->direction);
    if (!importanceCompetingDensity)
    {
        return std::unexpected(importanceCompetingDensity.error());
    }
    auto const importanceContribution = EvaluateProposalContribution(*importanceTarget, importanceSample->density);
    if (!importanceContribution)
    {
        return std::unexpected(importanceContribution.error());
    }
    auto const importanceWeights =
        SelectMisWeights(heuristic, {*importanceCompetingDensity, 1U}, {importanceSample->density, 1U}, powerBeta);
    if (!importanceWeights)
    {
        return std::unexpected(importanceWeights.error());
    }

    double const weightedUniformContribution = uniformWeights->first * *uniformContribution;
    double const weightedImportanceContribution = importanceWeights->second * *importanceContribution;
    double const estimate = weightedUniformContribution + weightedImportanceContribution;
    if (!std::isfinite(weightedUniformContribution) || !std::isfinite(weightedImportanceContribution) ||
        !std::isfinite(estimate))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    return MisEstimateDiagnostics{
        .uniform =
            {
                .unitSample = uniformUnitSample,
                .direction = uniformSample->direction,
                .targetValue = *uniformTarget,
                .selectedDensity = uniformSample->density,
                .competingDensity = *uniformCompetingDensity,
                .weight = uniformWeights->first,
                .unweightedContribution = *uniformContribution,
                .weightedContribution = weightedUniformContribution,
            },
        .importance =
            {
                .unitSample = importanceUnitSample,
                .direction = importanceSample->direction,
                .targetValue = *importanceTarget,
                .selectedDensity = importanceSample->density,
                .competingDensity = *importanceCompetingDensity,
                .weight = importanceWeights->second,
                .unweightedContribution = *importanceContribution,
                .weightedContribution = weightedImportanceContribution,
            },
        .estimate = estimate,
    };
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
    double const standardError = std::sqrt(sampleVariance / static_cast<double>(moments.count));
    if (!std::isfinite(sampleVariance) || sampleVariance < 0.0 || !std::isfinite(standardError))
    {
        return std::unexpected(ContractError::InvalidMomentState);
    }
    return EstimateSummary{
        .sampleCount = moments.count,
        .mean = moments.mean,
        .sampleVariance = sampleVariance,
        .standardError = standardError,
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

Float2 DeterministicUnitSample(std::uint32_t seed, std::uint32_t sampleIndex, std::uint32_t dimensionPair) noexcept
{
    std::uint32_t const indexedSeed = seed ^ (sampleIndex * 0x9E3779B9U);
    std::uint32_t const dimensionSeed = indexedSeed ^ (dimensionPair * 0x85EBCA6BU);
    return {
        UnitFloatFromBits(PcgHash(dimensionSeed ^ 0xA511E9B3U)),
        UnitFloatFromBits(PcgHash(dimensionSeed ^ 0x63D83595U)),
    };
}

std::expected<EstimateSummary, ContractError> BuildDeterministicUniformPopulation(double targetExponent,
                                                                                  std::uint32_t sampleCount,
                                                                                  std::uint32_t seed) noexcept
{
    if (!IsValidExponent(targetExponent))
    {
        return std::unexpected(ContractError::InvalidExponent);
    }
    auto const validSampleCount = ValidateReferenceSampleCount(sampleCount);
    if (!validSampleCount)
    {
        return std::unexpected(validSampleCount.error());
    }

    OnlineMoments moments{};
    for (std::uint32_t sampleIndex = 0U; sampleIndex < sampleCount; ++sampleIndex)
    {
        auto const contribution = EvaluateUniformContribution(
            DeterministicUnitSample(seed, sampleIndex, kUniformDimensionPair), targetExponent);
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

std::expected<EstimateSummary, ContractError> BuildDeterministicImportancePopulation(double targetExponent,
                                                                                     double proposalExponent,
                                                                                     std::uint32_t sampleCount,
                                                                                     std::uint32_t seed) noexcept
{
    if (!IsValidExponent(targetExponent) || !IsValidExponent(proposalExponent))
    {
        return std::unexpected(ContractError::InvalidExponent);
    }
    auto const validSampleCount = ValidateReferenceSampleCount(sampleCount);
    if (!validSampleCount)
    {
        return std::unexpected(validSampleCount.error());
    }

    OnlineMoments moments{};
    for (std::uint32_t sampleIndex = 0U; sampleIndex < sampleCount; ++sampleIndex)
    {
        auto const contribution = EvaluateImportanceContribution(
            DeterministicUnitSample(seed, sampleIndex, kImportanceDimensionPair), targetExponent, proposalExponent);
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

std::expected<EstimateSummary, ContractError> BuildDeterministicMisPopulation(double targetExponent,
                                                                              double proposalExponent,
                                                                              MisHeuristic heuristic, double powerBeta,
                                                                              std::uint32_t pairedSampleCount,
                                                                              std::uint32_t seed) noexcept
{
    if (!IsValidExponent(targetExponent) || !IsValidExponent(proposalExponent))
    {
        return std::unexpected(ContractError::InvalidExponent);
    }
    auto const validConfiguration = ValidateMisConfiguration(heuristic, powerBeta);
    if (!validConfiguration)
    {
        return std::unexpected(validConfiguration.error());
    }
    auto const validSampleCount = ValidateReferenceSampleCount(pairedSampleCount);
    if (!validSampleCount)
    {
        return std::unexpected(validSampleCount.error());
    }

    OnlineMoments moments{};
    for (std::uint32_t sampleIndex = 0U; sampleIndex < pairedSampleCount; ++sampleIndex)
    {
        Float2 const uniformSample = DeterministicUnitSample(seed, sampleIndex, kUniformDimensionPair);
        Float2 const importanceSample = DeterministicUnitSample(seed, sampleIndex, kImportanceDimensionPair);
        auto const diagnostics = EvaluateMisEstimate(uniformSample, importanceSample, targetExponent, proposalExponent,
                                                     heuristic, powerBeta);
        if (!diagnostics)
        {
            return std::unexpected(diagnostics.error());
        }
        auto const updated = PushSample(moments, diagnostics->estimate);
        if (!updated)
        {
            return std::unexpected(updated.error());
        }
        moments = *updated;
    }
    return Summarize(moments);
}

} // namespace ch17::importance_sampling
