#include "SphericalHarmonicsContracts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ch26::spherical_harmonics
{
namespace
{

inline constexpr double kDirectionTolerance = 1.0e-9;
inline constexpr double kWeightTolerance = 1.0e-8;
inline constexpr std::uint32_t kRotationPolarSamples = 12U;
inline constexpr std::uint32_t kRotationAzimuthSamples = 32U;

[[nodiscard]] bool IsFinite(Float3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] double Dot(Float3 left, Float3 right) noexcept
{
    return (left.x * right.x) + (left.y * right.y) + (left.z * right.z);
}

[[nodiscard]] bool IsUnitDirection(Float3 direction) noexcept
{
    return IsFinite(direction) && std::abs(Dot(direction, direction) - 1.0) <= kDirectionTolerance;
}

[[nodiscard]] double FactorialRatio(std::uint32_t numeratorStart, std::uint32_t denominatorEnd) noexcept
{
    double ratio = 1.0;
    for (std::uint32_t value = numeratorStart; value <= denominatorEnd; ++value)
    {
        ratio /= static_cast<double>(value);
    }
    return ratio;
}

[[nodiscard]] double AssociatedLegendre(std::uint32_t band, std::uint32_t absoluteOrder, double cosine) noexcept
{
    double pmm = 1.0;
    if (absoluteOrder > 0U)
    {
        double const sine = std::sqrt(std::max(0.0, 1.0 - (cosine * cosine)));
        double factor = 1.0;
        for (std::uint32_t order = 1U; order <= absoluteOrder; ++order)
        {
            pmm *= -factor * sine;
            factor += 2.0;
        }
    }
    if (band == absoluteOrder)
    {
        return pmm;
    }

    double pmmp1 = cosine * static_cast<double>((2U * absoluteOrder) + 1U) * pmm;
    if (band == absoluteOrder + 1U)
    {
        return pmmp1;
    }

    double previous = pmm;
    double current = pmmp1;
    for (std::uint32_t degree = absoluteOrder + 2U; degree <= band; ++degree)
    {
        double const next = ((static_cast<double>((2U * degree) - 1U) * cosine * current) -
                             (static_cast<double>(degree + absoluteOrder - 1U) * previous)) /
                            static_cast<double>(degree - absoluteOrder);
        previous = current;
        current = next;
    }
    return current;
}

[[nodiscard]] std::expected<std::uint32_t, ContractError> ValidateCoefficients(
    Coefficients const &coefficients) noexcept
{
    auto const count = CoefficientCount(coefficients.maximumBand);
    if (!count)
    {
        return std::unexpected(count.error());
    }
    for (std::uint32_t index = 0U; index < *count; ++index)
    {
        if (!std::isfinite(coefficients.values[index]))
        {
            return std::unexpected(ContractError::NonFinite);
        }
    }
    return *count;
}

[[nodiscard]] std::expected<double, ContractError> ValidateSamples(std::span<WeightedSample const> samples) noexcept
{
    if (samples.empty())
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }

    double weightSum{};
    for (WeightedSample const &sample : samples)
    {
        if (!IsUnitDirection(sample.direction))
        {
            return std::unexpected(IsFinite(sample.direction) ? ContractError::InvalidDirection
                                                              : ContractError::NonFinite);
        }
        if (!std::isfinite(sample.value))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (!std::isfinite(sample.solidAngle) || sample.solidAngle <= 0.0)
        {
            return std::unexpected(ContractError::InvalidSampleWeight);
        }
        weightSum += sample.solidAngle;
    }
    if (!std::isfinite(weightSum) || std::abs(weightSum - (4.0 * kPi)) > kWeightTolerance)
    {
        return std::unexpected(ContractError::InvalidWeightSum);
    }
    return weightSum;
}

[[nodiscard]] double Determinant(Matrix3 const &matrix) noexcept
{
    auto const &m = matrix.values;
    return (m[0] * ((m[4] * m[8]) - (m[5] * m[7]))) - (m[1] * ((m[3] * m[8]) - (m[5] * m[6]))) +
           (m[2] * ((m[3] * m[7]) - (m[4] * m[6])));
}

[[nodiscard]] bool IsRotation(Matrix3 const &matrix) noexcept
{
    for (double const value : matrix.values)
    {
        if (!std::isfinite(value))
        {
            return false;
        }
    }
    Matrix3 const product = Multiply(matrix, Transpose(matrix));
    Matrix3 const identity = IdentityRotation();
    for (std::size_t index = 0U; index < product.values.size(); ++index)
    {
        if (std::abs(product.values[index] - identity.values[index]) > 1.0e-9)
        {
            return false;
        }
    }
    return std::abs(Determinant(matrix) - 1.0) <= 1.0e-9;
}

struct GaussLegendreRule final
{
    std::array<double, kRotationPolarSamples> nodes{};
    std::array<double, kRotationPolarSamples> weights{};
};

[[nodiscard]] GaussLegendreRule BuildGaussLegendreRule() noexcept
{
    GaussLegendreRule rule{};
    constexpr std::uint32_t halfCount = (kRotationPolarSamples + 1U) / 2U;
    for (std::uint32_t rootIndex = 0U; rootIndex < halfCount; ++rootIndex)
    {
        double root = std::cos(kPi * (static_cast<double>(rootIndex) + 0.75) /
                               (static_cast<double>(kRotationPolarSamples) + 0.5));
        double derivative{};
        for (std::uint32_t iteration = 0U; iteration < 32U; ++iteration)
        {
            double previous = 1.0;
            double current = root;
            for (std::uint32_t degree = 2U; degree <= kRotationPolarSamples; ++degree)
            {
                double const next = ((static_cast<double>((2U * degree) - 1U) * root * current) -
                                     (static_cast<double>(degree - 1U) * previous)) /
                                    static_cast<double>(degree);
                previous = current;
                current = next;
            }
            derivative =
                static_cast<double>(kRotationPolarSamples) * ((root * current) - previous) / ((root * root) - 1.0);
            double const nextRoot = root - (current / derivative);
            if (std::abs(nextRoot - root) <= 1.0e-15)
            {
                root = nextRoot;
                break;
            }
            root = nextRoot;
        }
        double const weight = 2.0 / ((1.0 - (root * root)) * derivative * derivative);
        rule.nodes[rootIndex] = -root;
        rule.nodes[kRotationPolarSamples - 1U - rootIndex] = root;
        rule.weights[rootIndex] = weight;
        rule.weights[kRotationPolarSamples - 1U - rootIndex] = weight;
    }
    return rule;
}

} // namespace

std::expected<std::uint32_t, ContractError> CoefficientCount(std::uint32_t maximumBand) noexcept
{
    if (maximumBand > kMaximumBand)
    {
        return std::unexpected(ContractError::InvalidBand);
    }
    return (maximumBand + 1U) * (maximumBand + 1U);
}

std::expected<std::uint32_t, ContractError> CoefficientIndex(std::uint32_t band, std::int32_t order) noexcept
{
    if (band > kMaximumBand)
    {
        return std::unexpected(ContractError::InvalidBand);
    }
    if (order < -static_cast<std::int32_t>(band) || order > static_cast<std::int32_t>(band))
    {
        return std::unexpected(ContractError::InvalidOrder);
    }
    return (band * (band + 1U)) + static_cast<std::uint32_t>(order + static_cast<std::int32_t>(band)) - band;
}

std::expected<BandOrder, ContractError> DecodeCoefficientIndex(std::uint32_t index) noexcept
{
    if (index >= kMaximumCoefficientCount)
    {
        return std::unexpected(ContractError::InvalidCoefficientIndex);
    }
    std::uint32_t const band = static_cast<std::uint32_t>(std::floor(std::sqrt(static_cast<double>(index))));
    return BandOrder{.band = band,
                     .order = static_cast<std::int32_t>(index) - static_cast<std::int32_t>(band * (band + 1U))};
}

std::expected<double, ContractError> EvaluateRealBasis(std::uint32_t band, std::int32_t order,
                                                       Float3 direction) noexcept
{
    auto const index = CoefficientIndex(band, order);
    if (!index)
    {
        return std::unexpected(index.error());
    }
    if (!IsUnitDirection(direction))
    {
        return std::unexpected(IsFinite(direction) ? ContractError::InvalidDirection : ContractError::NonFinite);
    }

    std::uint32_t const absoluteOrder = static_cast<std::uint32_t>(std::abs(order));
    double const factorialRatio = FactorialRatio(band - absoluteOrder + 1U, band + absoluteOrder);
    double const normalization = std::sqrt((static_cast<double>((2U * band) + 1U) / (4.0 * kPi)) * factorialRatio);
    double const polynomial = AssociatedLegendre(band, absoluteOrder, direction.y);
    if (order == 0)
    {
        return normalization * polynomial;
    }

    double const azimuth = std::atan2(direction.z, direction.x);
    double const angular = order > 0 ? std::cos(static_cast<double>(absoluteOrder) * azimuth)
                                     : std::sin(static_cast<double>(absoluteOrder) * azimuth);
    return std::numbers::sqrt2 * normalization * polynomial * angular;
}

std::expected<std::array<double, kMaximumCoefficientCount>, ContractError> EvaluateRealBasis(std::uint32_t maximumBand,
                                                                                             Float3 direction) noexcept
{
    auto const count = CoefficientCount(maximumBand);
    if (!count)
    {
        return std::unexpected(count.error());
    }

    std::array<double, kMaximumCoefficientCount> basis{};
    for (std::uint32_t index = 0U; index < *count; ++index)
    {
        auto const bandOrder = DecodeCoefficientIndex(index);
        auto const value = EvaluateRealBasis(bandOrder->band, bandOrder->order, direction);
        if (!value)
        {
            return std::unexpected(value.error());
        }
        basis[index] = *value;
    }
    return basis;
}

std::expected<std::vector<WeightedSample>, ContractError> BuildEqualAreaSphereSamples(std::uint32_t latitudeStrata,
                                                                                      std::uint32_t longitudeStrata,
                                                                                      std::span<double const> values)
{
    if (latitudeStrata == 0U || longitudeStrata == 0U)
    {
        return std::unexpected(ContractError::InvalidDimensions);
    }
    std::uint64_t const sampleCount = static_cast<std::uint64_t>(latitudeStrata) * longitudeStrata;
    if (sampleCount != values.size())
    {
        return std::unexpected(ContractError::SizeMismatch);
    }

    std::vector<WeightedSample> samples;
    samples.reserve(values.size());
    double const solidAngle = (4.0 * kPi) / static_cast<double>(sampleCount);
    for (std::uint32_t latitude = 0U; latitude < latitudeStrata; ++latitude)
    {
        double const y = 1.0 - (2.0 * (static_cast<double>(latitude) + 0.5) / static_cast<double>(latitudeStrata));
        double const radius = std::sqrt(std::max(0.0, 1.0 - (y * y)));
        for (std::uint32_t longitude = 0U; longitude < longitudeStrata; ++longitude)
        {
            std::size_t const index = static_cast<std::size_t>(latitude) * longitudeStrata + longitude;
            if (!std::isfinite(values[index]))
            {
                return std::unexpected(ContractError::NonFinite);
            }
            double const azimuth =
                2.0 * kPi * (static_cast<double>(longitude) + 0.5) / static_cast<double>(longitudeStrata);
            samples.push_back({
                .direction = {radius * std::cos(azimuth), y, radius * std::sin(azimuth)},
                .value = values[index],
                .solidAngle = solidAngle,
            });
        }
    }
    return samples;
}

std::expected<std::vector<WeightedSample>, ContractError> BuildLatitudeLongitudeSamples(std::uint32_t width,
                                                                                        std::uint32_t height,
                                                                                        std::span<double const> values)
{
    if (width == 0U || height == 0U)
    {
        return std::unexpected(ContractError::InvalidDimensions);
    }
    std::uint64_t const sampleCount = static_cast<std::uint64_t>(width) * height;
    if (sampleCount != values.size())
    {
        return std::unexpected(ContractError::SizeMismatch);
    }

    std::vector<WeightedSample> samples;
    samples.reserve(values.size());
    double const azimuthWidth = (2.0 * kPi) / static_cast<double>(width);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        double const theta0 = kPi * static_cast<double>(row) / static_cast<double>(height);
        double const theta1 = kPi * static_cast<double>(row + 1U) / static_cast<double>(height);
        double const theta = 0.5 * (theta0 + theta1);
        double const y = std::cos(theta);
        double const radius = std::sin(theta);
        double const solidAngle = azimuthWidth * (std::cos(theta0) - std::cos(theta1));
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            std::size_t const index = static_cast<std::size_t>(row) * width + column;
            if (!std::isfinite(values[index]))
            {
                return std::unexpected(ContractError::NonFinite);
            }
            double const azimuth = 2.0 * kPi * (static_cast<double>(column) + 0.5) / static_cast<double>(width);
            samples.push_back({
                .direction = {radius * std::cos(azimuth), y, radius * std::sin(azimuth)},
                .value = values[index],
                .solidAngle = solidAngle,
            });
        }
    }
    return samples;
}

std::expected<Coefficients, ContractError> Project(std::uint32_t maximumBand,
                                                   std::span<WeightedSample const> samples) noexcept
{
    auto const count = CoefficientCount(maximumBand);
    if (!count)
    {
        return std::unexpected(count.error());
    }
    auto const weightSum = ValidateSamples(samples);
    if (!weightSum)
    {
        return std::unexpected(weightSum.error());
    }

    Coefficients coefficients{.maximumBand = maximumBand};
    for (WeightedSample const &sample : samples)
    {
        auto const basis = EvaluateRealBasis(maximumBand, sample.direction);
        if (!basis)
        {
            return std::unexpected(basis.error());
        }
        for (std::uint32_t index = 0U; index < *count; ++index)
        {
            double const next = coefficients.values[index] + (sample.value * (*basis)[index] * sample.solidAngle);
            if (!std::isfinite(next))
            {
                return std::unexpected(ContractError::NonFinite);
            }
            coefficients.values[index] = next;
        }
    }
    return coefficients;
}

std::expected<double, ContractError> Reconstruct(Coefficients const &coefficients, Float3 direction) noexcept
{
    auto const count = ValidateCoefficients(coefficients);
    if (!count)
    {
        return std::unexpected(count.error());
    }
    auto const basis = EvaluateRealBasis(coefficients.maximumBand, direction);
    if (!basis)
    {
        return std::unexpected(basis.error());
    }

    double result{};
    for (std::uint32_t index = 0U; index < *count; ++index)
    {
        result += coefficients.values[index] * (*basis)[index];
        if (!std::isfinite(result))
        {
            return std::unexpected(ContractError::NonFinite);
        }
    }
    return result;
}

std::expected<ReconstructionDiagnostics, ContractError> MeasureReconstruction(
    Coefficients const &coefficients, std::span<WeightedSample const> referenceSamples) noexcept
{
    auto const count = ValidateCoefficients(coefficients);
    if (!count)
    {
        return std::unexpected(count.error());
    }
    auto const weightSum = ValidateSamples(referenceSamples);
    if (!weightSum)
    {
        return std::unexpected(weightSum.error());
    }

    ReconstructionDiagnostics diagnostics{};
    diagnostics.minimumReconstruction = std::numeric_limits<double>::infinity();
    for (std::uint32_t index = 0U; index < *count; ++index)
    {
        auto const bandOrder = DecodeCoefficientIndex(index);
        diagnostics.bandEnergy[bandOrder->band] += coefficients.values[index] * coefficients.values[index];
        if (!std::isfinite(diagnostics.bandEnergy[bandOrder->band]))
        {
            return std::unexpected(ContractError::NonFinite);
        }
    }

    double squaredErrorIntegral{};
    double negativeSolidAngle{};
    for (WeightedSample const &sample : referenceSamples)
    {
        auto const reconstructed = Reconstruct(coefficients, sample.direction);
        if (!reconstructed)
        {
            return std::unexpected(reconstructed.error());
        }
        double const error = *reconstructed - sample.value;
        double const squaredErrorContribution = error * error * sample.solidAngle;
        if (!std::isfinite(error) || !std::isfinite(squaredErrorContribution))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        squaredErrorIntegral += squaredErrorContribution;
        if (!std::isfinite(squaredErrorIntegral))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        diagnostics.maximumAbsoluteError = std::max(diagnostics.maximumAbsoluteError, std::abs(error));
        diagnostics.minimumReconstruction = std::min(diagnostics.minimumReconstruction, *reconstructed);
        if (*reconstructed < 0.0)
        {
            negativeSolidAngle += sample.solidAngle;
        }
    }
    diagnostics.rootMeanSquareError = std::sqrt(squaredErrorIntegral / *weightSum);
    diagnostics.negativeSolidAngleFraction = negativeSolidAngle / *weightSum;
    return diagnostics;
}

Matrix3 IdentityRotation() noexcept
{
    return {};
}

Matrix3 RotationX(double radians) noexcept
{
    double const cosine = std::cos(radians);
    double const sine = std::sin(radians);
    return {.values = {1.0, 0.0, 0.0, 0.0, cosine, -sine, 0.0, sine, cosine}};
}

Matrix3 RotationY(double radians) noexcept
{
    double const cosine = std::cos(radians);
    double const sine = std::sin(radians);
    return {.values = {cosine, 0.0, sine, 0.0, 1.0, 0.0, -sine, 0.0, cosine}};
}

Matrix3 RotationZ(double radians) noexcept
{
    double const cosine = std::cos(radians);
    double const sine = std::sin(radians);
    return {.values = {cosine, -sine, 0.0, sine, cosine, 0.0, 0.0, 0.0, 1.0}};
}

Matrix3 Multiply(Matrix3 const &left, Matrix3 const &right) noexcept
{
    Matrix3 result{.values = {}};
    for (std::uint32_t row = 0U; row < 3U; ++row)
    {
        for (std::uint32_t column = 0U; column < 3U; ++column)
        {
            for (std::uint32_t inner = 0U; inner < 3U; ++inner)
            {
                result.values[(row * 3U) + column] +=
                    left.values[(row * 3U) + inner] * right.values[(inner * 3U) + column];
            }
        }
    }
    return result;
}

Matrix3 Transpose(Matrix3 const &matrix) noexcept
{
    return {.values = {
                matrix.values[0],
                matrix.values[3],
                matrix.values[6],
                matrix.values[1],
                matrix.values[4],
                matrix.values[7],
                matrix.values[2],
                matrix.values[5],
                matrix.values[8],
            }};
}

std::expected<Float3, ContractError> TransformDirection(Matrix3 const &matrix, Float3 direction) noexcept
{
    if (!IsRotation(matrix))
    {
        return std::unexpected(ContractError::InvalidRotation);
    }
    if (!IsUnitDirection(direction))
    {
        return std::unexpected(IsFinite(direction) ? ContractError::InvalidDirection : ContractError::NonFinite);
    }
    auto const &m = matrix.values;
    Float3 const transformed{
        .x = (m[0] * direction.x) + (m[1] * direction.y) + (m[2] * direction.z),
        .y = (m[3] * direction.x) + (m[4] * direction.y) + (m[5] * direction.z),
        .z = (m[6] * direction.x) + (m[7] * direction.y) + (m[8] * direction.z),
    };
    double const length = std::sqrt(Dot(transformed, transformed));
    if (!std::isfinite(length) || length <= std::numeric_limits<double>::min())
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    return Float3{
        .x = transformed.x / length,
        .y = transformed.y / length,
        .z = transformed.z / length,
    };
}

std::expected<CoefficientRotation, ContractError> BuildCoefficientRotation(std::uint32_t maximumBand,
                                                                           Matrix3 const &rotation) noexcept
{
    auto const count = CoefficientCount(maximumBand);
    if (!count)
    {
        return std::unexpected(count.error());
    }
    if (!IsRotation(rotation))
    {
        return std::unexpected(ContractError::InvalidRotation);
    }

    CoefficientRotation coefficientRotation{};
    coefficientRotation.maximumBand_ = maximumBand;
    Matrix3 const inverse = Transpose(rotation);
    GaussLegendreRule const polarRule = BuildGaussLegendreRule();
    double const azimuthWeight = (2.0 * kPi) / static_cast<double>(kRotationAzimuthSamples);
    for (std::uint32_t polarIndex = 0U; polarIndex < kRotationPolarSamples; ++polarIndex)
    {
        double const y = polarRule.nodes[polarIndex];
        double const radius = std::sqrt(std::max(0.0, 1.0 - (y * y)));
        for (std::uint32_t azimuthIndex = 0U; azimuthIndex < kRotationAzimuthSamples; ++azimuthIndex)
        {
            double const azimuth =
                2.0 * kPi * (static_cast<double>(azimuthIndex) + 0.5) / static_cast<double>(kRotationAzimuthSamples);
            Float3 const outputDirection{radius * std::cos(azimuth), y, radius * std::sin(azimuth)};
            auto const inputDirection = TransformDirection(inverse, outputDirection);
            if (!inputDirection)
            {
                return std::unexpected(inputDirection.error());
            }
            auto const outputBasis = EvaluateRealBasis(maximumBand, outputDirection);
            if (!outputBasis)
            {
                return std::unexpected(outputBasis.error());
            }
            auto const inputBasis = EvaluateRealBasis(maximumBand, *inputDirection);
            if (!inputBasis)
            {
                return std::unexpected(inputBasis.error());
            }
            double const weight = polarRule.weights[polarIndex] * azimuthWeight;
            for (std::uint32_t row = 0U; row < *count; ++row)
            {
                for (std::uint32_t column = 0U; column < *count; ++column)
                {
                    auto const rowBand = DecodeCoefficientIndex(row);
                    auto const columnBand = DecodeCoefficientIndex(column);
                    if (rowBand->band == columnBand->band)
                    {
                        coefficientRotation.values_[(row * kMaximumCoefficientCount) + column] +=
                            (*outputBasis)[row] * (*inputBasis)[column] * weight;
                    }
                }
            }
        }
    }
    return coefficientRotation;
}

std::expected<Coefficients, ContractError> Rotate(Coefficients const &coefficients,
                                                  CoefficientRotation const &rotation) noexcept
{
    auto const count = ValidateCoefficients(coefficients);
    if (!count)
    {
        return std::unexpected(count.error());
    }
    if (coefficients.maximumBand != rotation.maximumBand_)
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    Coefficients result{.maximumBand = coefficients.maximumBand};
    for (std::uint32_t row = 0U; row < *count; ++row)
    {
        for (std::uint32_t column = 0U; column < *count; ++column)
        {
            double const matrixValue = rotation.values_[(row * kMaximumCoefficientCount) + column];
            double const next = result.values[row] + (matrixValue * coefficients.values[column]);
            if (!std::isfinite(next))
            {
                return std::unexpected(ContractError::NonFinite);
            }
            result.values[row] = next;
        }
    }
    return result;
}

std::expected<Coefficients, ContractError> ConvolveClampedCosine(Coefficients const &radiance) noexcept
{
    auto const count = ValidateCoefficients(radiance);
    if (!count)
    {
        return std::unexpected(count.error());
    }
    constexpr std::array<double, kMaximumBand + 1U> factors{
        kPi,
        (2.0 * kPi) / 3.0,
        kPi / 4.0,
        0.0,
    };
    Coefficients irradiance{.maximumBand = radiance.maximumBand};
    for (std::uint32_t index = 0U; index < *count; ++index)
    {
        auto const bandOrder = DecodeCoefficientIndex(index);
        double const value = radiance.values[index] * factors[bandOrder->band];
        if (!std::isfinite(value))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        irradiance.values[index] = value;
    }
    return irradiance;
}

} // namespace ch26::spherical_harmonics
