#pragma once

#include <cstdint>
#include <expected>
#include <span>

namespace ch16::monte_carlo
{

inline constexpr double kPi = 3.141592653589793238462643383279502884;
inline constexpr std::uint32_t kMaximumReferenceSampleCount = 1U << 24U;

enum class ContractError : std::uint8_t
{
    NonFinite,
    InvalidExponent,
    InvalidUnitSample,
    InvalidDirection,
    InvalidDensity,
    InvalidSampleCount,
    CountOverflow,
    InvalidMomentState,
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
[[nodiscard]] std::expected<double, ContractError> EvaluatePowerCosineIntegrand(double cosine,
                                                                                double exponent) noexcept;
[[nodiscard]] std::expected<double, ContractError> EvaluateUniformHemisphereContribution(Float2 unitSample,
                                                                                         double exponent) noexcept;
[[nodiscard]] std::expected<double, ContractError> ExactPowerCosineIntegral(double exponent) noexcept;
[[nodiscard]] std::expected<double, ContractError> UniformHemisphereContributionVariance(double exponent) noexcept;

[[nodiscard]] std::expected<OnlineMoments, ContractError> PushSample(OnlineMoments moments, double sample) noexcept;
[[nodiscard]] std::expected<OnlineMoments, ContractError> AccumulateSamples(std::span<double const> samples) noexcept;
[[nodiscard]] std::expected<EstimateSummary, ContractError> Summarize(OnlineMoments moments) noexcept;

[[nodiscard]] std::uint32_t PcgHash(std::uint32_t value) noexcept;
[[nodiscard]] double UnitFloatFromBits(std::uint32_t bits) noexcept;
[[nodiscard]] Float2 DeterministicUnitSample(std::uint32_t seed, std::uint32_t sampleIndex) noexcept;
[[nodiscard]] std::expected<EstimateSummary, ContractError> BuildDeterministicEstimate(double exponent,
                                                                                       std::uint32_t sampleCount,
                                                                                       std::uint32_t seed) noexcept;

} // namespace ch16::monte_carlo
