#pragma once

#include <cstdint>
#include <expected>

namespace ch17::importance_sampling
{

inline constexpr double kPi = 3.141592653589793238462643383279502884;
inline constexpr std::uint32_t kMaximumReferenceSampleCount = 1U << 24U;
inline constexpr std::uint32_t kUniformDimensionPair = 0U;
inline constexpr std::uint32_t kImportanceDimensionPair = 1U;

enum class ContractError : std::uint8_t
{
    NonFinite,
    InvalidExponent,
    InvalidUnitSample,
    InvalidDirection,
    InvalidTargetValue,
    InvalidDensity,
    ZeroDensityForNonzeroTarget,
    ZeroCombinedDensity,
    InvalidSampleCount,
    CountOverflow,
    InvalidMomentState,
    NonFiniteVariance,
    InvalidHeuristicExponent,
    InvalidHeuristic,
};

enum class MisHeuristic : std::uint8_t
{
    Balance,
    Power,
};

struct Float2 final
{
    double x{};
    double y{};

    [[nodiscard]] bool operator==(Float2 const &) const noexcept = default;
};

struct Float3 final
{
    double x{};
    double y{};
    double z{};

    [[nodiscard]] bool operator==(Float3 const &) const noexcept = default;
};

struct DirectionSample final
{
    Float3 direction{};
    double density{};

    [[nodiscard]] bool operator==(DirectionSample const &) const noexcept = default;
};

struct ProposalDensity final
{
    double density{};
    std::uint32_t sampleCount{};

    [[nodiscard]] bool operator==(ProposalDensity const &) const noexcept = default;
};

struct MisWeights final
{
    double first{};
    double second{};

    [[nodiscard]] bool operator==(MisWeights const &) const noexcept = default;
};

struct MisTechniqueDiagnostic final
{
    Float2 unitSample{};
    Float3 direction{};
    double targetValue{};
    double selectedDensity{};
    double competingDensity{};
    double weight{};
    double unweightedContribution{};
    double weightedContribution{};

    [[nodiscard]] bool operator==(MisTechniqueDiagnostic const &) const noexcept = default;
};

struct MisEstimateDiagnostics final
{
    MisTechniqueDiagnostic uniform{};
    MisTechniqueDiagnostic importance{};
    double estimate{};

    [[nodiscard]] bool operator==(MisEstimateDiagnostics const &) const noexcept = default;
};

struct OnlineMoments final
{
    std::uint64_t count{};
    double mean{};
    double sumSquaredDifferences{};

    [[nodiscard]] bool operator==(OnlineMoments const &) const noexcept = default;
};

struct EstimateSummary final
{
    std::uint64_t sampleCount{};
    double mean{};
    double sampleVariance{};
    double standardError{};

    [[nodiscard]] bool operator==(EstimateSummary const &) const noexcept = default;
};

[[nodiscard]] std::expected<DirectionSample, ContractError> MapUniformHemisphere(Float2 unitSample) noexcept;
[[nodiscard]] std::expected<double, ContractError> UniformHemispherePdf(Float3 direction) noexcept;
[[nodiscard]] std::expected<DirectionSample, ContractError> MapPowerCosineHemisphere(Float2 unitSample,
                                                                                     double proposalExponent) noexcept;
[[nodiscard]] std::expected<double, ContractError> PowerCosineHemispherePdf(Float3 direction,
                                                                            double proposalExponent) noexcept;

[[nodiscard]] std::expected<double, ContractError> EvaluatePowerCosineTarget(double cosine,
                                                                             double targetExponent) noexcept;
[[nodiscard]] std::expected<double, ContractError> ExactPowerCosineIntegral(double targetExponent) noexcept;
[[nodiscard]] std::expected<double, ContractError> EvaluateProposalContribution(double targetValue,
                                                                                double selectedDensity) noexcept;
[[nodiscard]] std::expected<double, ContractError> EvaluateUniformContribution(Float2 unitSample,
                                                                               double targetExponent) noexcept;
[[nodiscard]] std::expected<double, ContractError> EvaluateImportanceContribution(Float2 unitSample,
                                                                                  double targetExponent,
                                                                                  double proposalExponent) noexcept;
[[nodiscard]] std::expected<double, ContractError> PowerCosineContributionVariance(double targetExponent,
                                                                                   double proposalExponent) noexcept;

[[nodiscard]] std::expected<MisWeights, ContractError> BalanceHeuristicWeights(ProposalDensity first,
                                                                               ProposalDensity second) noexcept;
[[nodiscard]] std::expected<MisWeights, ContractError> PowerHeuristicWeights(ProposalDensity first,
                                                                             ProposalDensity second,
                                                                             double beta) noexcept;
[[nodiscard]] std::expected<MisEstimateDiagnostics, ContractError> EvaluateMisEstimate(
    Float2 uniformUnitSample, Float2 importanceUnitSample, double targetExponent, double proposalExponent,
    MisHeuristic heuristic, double powerBeta) noexcept;

[[nodiscard]] std::expected<OnlineMoments, ContractError> PushSample(OnlineMoments moments, double sample) noexcept;
[[nodiscard]] std::expected<EstimateSummary, ContractError> Summarize(OnlineMoments moments) noexcept;

[[nodiscard]] std::uint32_t PcgHash(std::uint32_t value) noexcept;
[[nodiscard]] double UnitFloatFromBits(std::uint32_t bits) noexcept;
[[nodiscard]] Float2 DeterministicUnitSample(std::uint32_t seed, std::uint32_t sampleIndex,
                                             std::uint32_t dimensionPair) noexcept;
[[nodiscard]] std::expected<EstimateSummary, ContractError> BuildDeterministicUniformPopulation(
    double targetExponent, std::uint32_t sampleCount, std::uint32_t seed) noexcept;
[[nodiscard]] std::expected<EstimateSummary, ContractError> BuildDeterministicImportancePopulation(
    double targetExponent, double proposalExponent, std::uint32_t sampleCount, std::uint32_t seed) noexcept;
[[nodiscard]] std::expected<EstimateSummary, ContractError> BuildDeterministicMisPopulation(
    double targetExponent, double proposalExponent, MisHeuristic heuristic, double powerBeta,
    std::uint32_t pairedSampleCount, std::uint32_t seed) noexcept;

} // namespace ch17::importance_sampling
