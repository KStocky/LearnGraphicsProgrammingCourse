#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace ch26::spherical_harmonics
{

inline constexpr double kPi = 3.141592653589793238462643383279502884;
inline constexpr std::uint32_t kMaximumBand = 3U;
inline constexpr std::uint32_t kMaximumCoefficientCount = 16U;

enum class ContractError : std::uint8_t
{
    NonFinite,
    InvalidBand,
    InvalidOrder,
    InvalidCoefficientIndex,
    InvalidDirection,
    InvalidDimensions,
    InvalidSampleCount,
    InvalidSampleWeight,
    InvalidWeightSum,
    SizeMismatch,
    InvalidRotation,
};

struct Float3 final
{
    double x{};
    double y{};
    double z{};

    [[nodiscard]] bool operator==(Float3 const &) const noexcept = default;
};

struct BandOrder final
{
    std::uint32_t band{};
    std::int32_t order{};

    [[nodiscard]] bool operator==(BandOrder const &) const noexcept = default;
};

struct WeightedSample final
{
    Float3 direction{};
    double value{};
    double solidAngle{};

    [[nodiscard]] bool operator==(WeightedSample const &) const noexcept = default;
};

struct Coefficients final
{
    std::uint32_t maximumBand{};
    std::array<double, kMaximumCoefficientCount> values{};

    [[nodiscard]] bool operator==(Coefficients const &) const noexcept = default;
};

struct Matrix3 final
{
    std::array<double, 9U> values{
        1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0,
    };

    [[nodiscard]] bool operator==(Matrix3 const &) const noexcept = default;
};

class CoefficientRotation final
{
  public:
    CoefficientRotation(CoefficientRotation const &) = default;
    CoefficientRotation(CoefficientRotation &&) noexcept = default;
    CoefficientRotation &operator=(CoefficientRotation const &) = default;
    CoefficientRotation &operator=(CoefficientRotation &&) noexcept = default;
    ~CoefficientRotation() = default;

    [[nodiscard]] bool operator==(CoefficientRotation const &) const noexcept = default;

  private:
    friend std::expected<CoefficientRotation, ContractError> BuildCoefficientRotation(std::uint32_t maximumBand,
                                                                                      Matrix3 const &rotation) noexcept;
    friend std::expected<Coefficients, ContractError> Rotate(Coefficients const &coefficients,
                                                             CoefficientRotation const &rotation) noexcept;

    CoefficientRotation() = default;

    std::uint32_t maximumBand_{};
    std::array<double, kMaximumCoefficientCount * kMaximumCoefficientCount> values_{};
};

struct ReconstructionDiagnostics final
{
    std::array<double, kMaximumBand + 1U> bandEnergy{};
    double rootMeanSquareError{};
    double maximumAbsoluteError{};
    double minimumReconstruction{};
    double negativeSolidAngleFraction{};

    [[nodiscard]] bool operator==(ReconstructionDiagnostics const &) const noexcept = default;
};

[[nodiscard]] std::expected<std::uint32_t, ContractError> CoefficientCount(std::uint32_t maximumBand) noexcept;
[[nodiscard]] std::expected<std::uint32_t, ContractError> CoefficientIndex(std::uint32_t band,
                                                                           std::int32_t order) noexcept;
[[nodiscard]] std::expected<BandOrder, ContractError> DecodeCoefficientIndex(std::uint32_t index) noexcept;
[[nodiscard]] std::expected<double, ContractError> EvaluateRealBasis(std::uint32_t band, std::int32_t order,
                                                                     Float3 direction) noexcept;
[[nodiscard]] std::expected<std::array<double, kMaximumCoefficientCount>, ContractError> EvaluateRealBasis(
    std::uint32_t maximumBand, Float3 direction) noexcept;

[[nodiscard]] std::expected<std::vector<WeightedSample>, ContractError> BuildEqualAreaSphereSamples(
    std::uint32_t latitudeStrata, std::uint32_t longitudeStrata, std::span<double const> values);
[[nodiscard]] std::expected<std::vector<WeightedSample>, ContractError> BuildLatitudeLongitudeSamples(
    std::uint32_t width, std::uint32_t height, std::span<double const> values);
[[nodiscard]] std::expected<Coefficients, ContractError> Project(std::uint32_t maximumBand,
                                                                 std::span<WeightedSample const> samples) noexcept;
[[nodiscard]] std::expected<double, ContractError> Reconstruct(Coefficients const &coefficients,
                                                               Float3 direction) noexcept;
[[nodiscard]] std::expected<ReconstructionDiagnostics, ContractError> MeasureReconstruction(
    Coefficients const &coefficients, std::span<WeightedSample const> referenceSamples) noexcept;

[[nodiscard]] Matrix3 IdentityRotation() noexcept;
[[nodiscard]] Matrix3 RotationX(double radians) noexcept;
[[nodiscard]] Matrix3 RotationY(double radians) noexcept;
[[nodiscard]] Matrix3 RotationZ(double radians) noexcept;
[[nodiscard]] Matrix3 Multiply(Matrix3 const &left, Matrix3 const &right) noexcept;
[[nodiscard]] Matrix3 Transpose(Matrix3 const &matrix) noexcept;
[[nodiscard]] std::expected<Float3, ContractError> TransformDirection(Matrix3 const &matrix, Float3 direction) noexcept;
[[nodiscard]] std::expected<CoefficientRotation, ContractError> BuildCoefficientRotation(
    std::uint32_t maximumBand, Matrix3 const &rotation) noexcept;
[[nodiscard]] std::expected<Coefficients, ContractError> Rotate(Coefficients const &coefficients,
                                                                CoefficientRotation const &rotation) noexcept;

[[nodiscard]] std::expected<Coefficients, ContractError> ConvolveClampedCosine(Coefficients const &radiance) noexcept;

} // namespace ch26::spherical_harmonics
