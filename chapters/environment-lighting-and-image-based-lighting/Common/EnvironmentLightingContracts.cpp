#include "EnvironmentLightingContracts.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ch27::environment_lighting
{
namespace
{

[[nodiscard]] bool IsFinite(double value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool IsFinite(Float2 value) noexcept
{
    return IsFinite(value.x) && IsFinite(value.y);
}

[[nodiscard]] bool IsFinite(Float3 value) noexcept
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

[[nodiscard]] bool IsFinite(Rgb value) noexcept
{
    return IsFinite(value.r) && IsFinite(value.g) && IsFinite(value.b);
}

[[nodiscard]] double Dot(Float3 left, Float3 right) noexcept
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] Float3 Cross(Float3 left, Float3 right) noexcept
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

[[nodiscard]] Float3 Scale(Float3 value, double scale) noexcept
{
    return {value.x * scale, value.y * scale, value.z * scale};
}

[[nodiscard]] Float3 Add(Float3 left, Float3 right) noexcept
{
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] std::expected<Float3, ContractError> UnitDirection(Float3 direction) noexcept
{
    if (!IsFinite(direction))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    double const lengthSquared = Dot(direction, direction);
    if (!IsFinite(lengthSquared))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (std::abs(lengthSquared - 1.0) > 1.0e-6)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    double const inverseLength = 1.0 / std::sqrt(lengthSquared);
    Float3 const normalized = Scale(direction, inverseLength);
    if (!IsFinite(normalized))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return normalized;
}

[[nodiscard]] double WrapUnit(double value) noexcept
{
    double wrapped = value - std::floor(value);
    if (wrapped >= 1.0)
    {
        wrapped = 0.0;
    }
    return wrapped;
}

[[nodiscard]] std::expected<std::size_t, ContractError> PixelCount(std::uint32_t width, std::uint32_t height) noexcept
{
    if (width == 0U || height == 0U || width > kMaximumEnvironmentDimension || height > kMaximumEnvironmentDimension)
    {
        return std::unexpected(ContractError::InvalidDimensions);
    }
    std::size_t const widthValue = static_cast<std::size_t>(width);
    std::size_t const heightValue = static_cast<std::size_t>(height);
    if (widthValue > std::numeric_limits<std::size_t>::max() / heightValue)
    {
        return std::unexpected(ContractError::CountOverflow);
    }
    return widthValue * heightValue;
}

[[nodiscard]] Rgb Add(Rgb left, Rgb right) noexcept
{
    return {left.r + right.r, left.g + right.g, left.b + right.b};
}

[[nodiscard]] Rgb Scale(Rgb value, double scale) noexcept
{
    return {value.r * scale, value.g * scale, value.b * scale};
}

[[nodiscard]] Rgb Lerp(Rgb first, Rgb second, double amount) noexcept
{
    return Add(Scale(first, 1.0 - amount), Scale(second, amount));
}

[[nodiscard]] std::expected<void, ContractError> ValidateRoughness(double roughness) noexcept
{
    if (!IsFinite(roughness))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (roughness < 0.0 || roughness > 1.0)
    {
        return std::unexpected(ContractError::InvalidRoughness);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateSampleCount(std::uint32_t sampleCount) noexcept
{
    if (sampleCount == 0U || sampleCount > kMaximumReferenceSampleCount)
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }
    return {};
}

[[nodiscard]] std::expected<std::pair<Float3, Float3>, ContractError> BuildTangentFrame(Float3 normal) noexcept
{
    auto const unitNormal = UnitDirection(normal);
    if (!unitNormal)
    {
        return std::unexpected(unitNormal.error());
    }
    Float3 tangent{};
    if (std::abs(unitNormal->y) > 0.999)
    {
        Float3 const candidate = Cross(*unitNormal, {0.0, 0.0, 1.0});
        tangent = Scale(candidate, 1.0 / std::sqrt(Dot(candidate, candidate)));
    }
    else
    {
        Float3 const candidate = Cross({0.0, 1.0, 0.0}, *unitNormal);
        double const inverseLength = 1.0 / std::sqrt(Dot(candidate, candidate));
        tangent = Scale(candidate, inverseLength);
    }
    Float3 const bitangent = Cross(tangent, *unitNormal);
    if (!IsFinite(tangent) || !IsFinite(bitangent))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return std::pair{tangent, bitangent};
}

[[nodiscard]] std::expected<Float3, ContractError> LocalToWorld(Float3 local, Float3 normal) noexcept
{
    auto const frame = BuildTangentFrame(normal);
    if (!frame)
    {
        return std::unexpected(frame.error());
    }
    Float3 const world = Add(Add(Scale(frame->first, local.x), Scale(normal, local.y)), Scale(frame->second, local.z));
    return UnitDirection(world);
}

[[nodiscard]] double RadicalInverseBase2(std::uint32_t bits) noexcept
{
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    return static_cast<double>(bits) * (1.0 / 4'294'967'296.0);
}

[[nodiscard]] bool IsAccumulationFinite(Rgb value, double weight) noexcept
{
    return IsFinite(value) && IsFinite(weight);
}

[[nodiscard]] Rgb SampleBilinearValidated(EnvironmentImageView image, Float2 uv) noexcept
{
    double const x = WrapUnit(uv.x) * static_cast<double>(image.width) - 0.5;
    double const y = std::clamp(uv.y, 0.0, 1.0) * static_cast<double>(image.height) - 0.5;
    double const xFloor = std::floor(x);
    double const yFloor = std::floor(y);
    double const xFraction = x - xFloor;
    double const yFraction = y - yFloor;

    auto const wrapColumn = [width = image.width](std::int64_t column)
    {
        std::int64_t const signedWidth = static_cast<std::int64_t>(width);
        std::int64_t wrapped = column % signedWidth;
        if (wrapped < 0)
        {
            wrapped += signedWidth;
        }
        return static_cast<std::uint32_t>(wrapped);
    };
    auto const clampRow = [height = image.height](std::int64_t row)
    { return static_cast<std::uint32_t>(std::clamp(row, std::int64_t{0}, static_cast<std::int64_t>(height - 1U))); };
    auto const pixel = [&image](std::uint32_t column, std::uint32_t row)
    { return image.pixels[static_cast<std::size_t>(row) * image.width + column]; };

    std::int64_t const x0 = static_cast<std::int64_t>(xFloor);
    std::int64_t const y0 = static_cast<std::int64_t>(yFloor);
    std::uint32_t const column0 = wrapColumn(x0);
    std::uint32_t const column1 = wrapColumn(x0 + 1);
    std::uint32_t const row0 = clampRow(y0);
    std::uint32_t const row1 = clampRow(y0 + 1);
    Rgb const top = Lerp(pixel(column0, row0), pixel(column1, row0), xFraction);
    Rgb const bottom = Lerp(pixel(column0, row1), pixel(column1, row1), xFraction);
    return Lerp(top, bottom, yFraction);
}

} // namespace

std::expected<void, ContractError> ValidateEnvironment(EnvironmentImageView image) noexcept
{
    auto const pixelCount = PixelCount(image.width, image.height);
    if (!pixelCount)
    {
        return std::unexpected(pixelCount.error());
    }
    if (image.pixels.size() != *pixelCount)
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    for (Rgb const pixel : image.pixels)
    {
        if (!IsFinite(pixel))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (pixel.r < 0.0 || pixel.g < 0.0 || pixel.b < 0.0)
        {
            return std::unexpected(ContractError::NegativeRadiance);
        }
    }
    return {};
}

std::expected<Float2, ContractError> DirectionToLatLongUv(Float3 direction) noexcept
{
    auto const unitDirection = UnitDirection(direction);
    if (!unitDirection)
    {
        return std::unexpected(unitDirection.error());
    }
    double phi = std::atan2(unitDirection->z, unitDirection->x);
    if (phi < 0.0)
    {
        phi += 2.0 * kPi;
    }
    Float2 const uv{
        .x = phi / (2.0 * kPi),
        .y = std::acos(std::clamp(unitDirection->y, -1.0, 1.0)) / kPi,
    };
    if (!IsFinite(uv))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return uv;
}

std::expected<Float3, ContractError> LatLongUvToDirection(Float2 uv) noexcept
{
    if (!IsFinite(uv))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    double const theta = std::clamp(uv.y, 0.0, 1.0) * kPi;
    double const phi = WrapUnit(uv.x) * 2.0 * kPi;
    double const sinTheta = std::sin(theta);
    Float3 const direction{
        .x = sinTheta * std::cos(phi),
        .y = std::cos(theta),
        .z = sinTheta * std::sin(phi),
    };
    if (!IsFinite(direction))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return direction;
}

std::expected<double, ContractError> LatLongTexelSolidAngle(std::uint32_t width, std::uint32_t height,
                                                            std::uint32_t row) noexcept
{
    auto const pixelCount = PixelCount(width, height);
    if (!pixelCount)
    {
        return std::unexpected(pixelCount.error());
    }
    if (row >= height)
    {
        return std::unexpected(ContractError::InvalidDimensions);
    }
    double const theta0 = kPi * static_cast<double>(row) / static_cast<double>(height);
    double const theta1 = kPi * static_cast<double>(row + 1U) / static_cast<double>(height);
    double const solidAngle = (2.0 * kPi / static_cast<double>(width)) * (std::cos(theta0) - std::cos(theta1));
    if (!IsFinite(solidAngle) || solidAngle <= 0.0)
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return solidAngle;
}

std::expected<Rgb, ContractError> SampleNearest(EnvironmentImageView image, Float2 uv) noexcept
{
    auto const valid = ValidateEnvironment(image);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (!IsFinite(uv))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    double const u = WrapUnit(uv.x);
    double const v = std::clamp(uv.y, 0.0, 1.0);
    std::uint32_t const column =
        std::min(static_cast<std::uint32_t>(u * static_cast<double>(image.width)), image.width - 1U);
    std::uint32_t const row =
        std::min(static_cast<std::uint32_t>(v * static_cast<double>(image.height)), image.height - 1U);
    return image.pixels[static_cast<std::size_t>(row) * image.width + column];
}

std::expected<Rgb, ContractError> SampleBilinear(EnvironmentImageView image, Float2 uv) noexcept
{
    auto const valid = ValidateEnvironment(image);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (!IsFinite(uv))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    Rgb const result = SampleBilinearValidated(image, uv);
    if (!IsFinite(result))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return result;
}

std::expected<Rgb, ContractError> IntegrateDiffuseIrradiance(EnvironmentImageView image, Float3 surfaceNormal) noexcept
{
    auto const valid = ValidateEnvironment(image);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    auto const normal = UnitDirection(surfaceNormal);
    if (!normal)
    {
        return std::unexpected(normal.error());
    }

    Rgb irradiance{};
    for (std::uint32_t row = 0U; row < image.height; ++row)
    {
        auto const solidAngle = LatLongTexelSolidAngle(image.width, image.height, row);
        if (!solidAngle)
        {
            return std::unexpected(solidAngle.error());
        }
        double const v = (static_cast<double>(row) + 0.5) / static_cast<double>(image.height);
        for (std::uint32_t column = 0U; column < image.width; ++column)
        {
            double const u = (static_cast<double>(column) + 0.5) / static_cast<double>(image.width);
            auto const direction = LatLongUvToDirection({u, v});
            if (!direction)
            {
                return std::unexpected(direction.error());
            }
            double const cosine = std::max(0.0, Dot(*normal, *direction));
            Rgb const contribution =
                Scale(image.pixels[static_cast<std::size_t>(row) * image.width + column], cosine * *solidAngle);
            irradiance = Add(irradiance, contribution);
            if (!IsFinite(irradiance))
            {
                return std::unexpected(ContractError::NonFinite);
            }
        }
    }
    return irradiance;
}

std::expected<double, ContractError> PerceptualRoughnessToAlpha(double roughness) noexcept
{
    auto const valid = ValidateRoughness(roughness);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    return std::max(roughness * roughness, kMinimumGgxAlpha);
}

std::expected<double, ContractError> EvaluateGgxNdf(double nDotH, double roughness) noexcept
{
    if (!IsFinite(nDotH))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (nDotH < 0.0 || nDotH > 1.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    auto const alpha = PerceptualRoughnessToAlpha(roughness);
    if (!alpha)
    {
        return std::unexpected(alpha.error());
    }
    double const alphaSquared = *alpha * *alpha;
    double const denominatorTerm = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
    double const ndf = alphaSquared / (kPi * denominatorTerm * denominatorTerm);
    if (!IsFinite(ndf) || ndf < 0.0)
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return ndf;
}

std::expected<double, ContractError> EvaluateSmithG1(double nDotDirection, double roughness) noexcept
{
    if (!IsFinite(nDotDirection))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (nDotDirection < 0.0 || nDotDirection > 1.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    auto const alpha = PerceptualRoughnessToAlpha(roughness);
    if (!alpha)
    {
        return std::unexpected(alpha.error());
    }
    if (nDotDirection == 0.0)
    {
        return 0.0;
    }
    double const alphaSquared = *alpha * *alpha;
    double const root = std::sqrt(alphaSquared + (1.0 - alphaSquared) * nDotDirection * nDotDirection);
    double const result = (2.0 * nDotDirection) / (nDotDirection + root);
    if (!IsFinite(result))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return result;
}

std::expected<Float2, ContractError> HammersleySample(std::uint32_t sampleIndex, std::uint32_t sampleCount) noexcept
{
    auto const valid = ValidateSampleCount(sampleCount);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (sampleIndex >= sampleCount)
    {
        return std::unexpected(ContractError::InvalidSampleIndex);
    }
    return Float2{
        .x = (static_cast<double>(sampleIndex) + 0.5) / static_cast<double>(sampleCount),
        .y = RadicalInverseBase2(sampleIndex),
    };
}

std::expected<GgxDirectionSample, ContractError> SampleGgxReflection(Float3 normal, Float3 viewDirection,
                                                                     double roughness, Float2 unitSample) noexcept
{
    auto const unitNormal = UnitDirection(normal);
    if (!unitNormal)
    {
        return std::unexpected(unitNormal.error());
    }
    auto const unitView = UnitDirection(viewDirection);
    if (!unitView)
    {
        return std::unexpected(unitView.error());
    }
    if (!IsFinite(unitSample))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (unitSample.x < 0.0 || unitSample.x >= 1.0 || unitSample.y < 0.0 || unitSample.y >= 1.0)
    {
        return std::unexpected(ContractError::InvalidUnitSample);
    }
    auto const alpha = PerceptualRoughnessToAlpha(roughness);
    if (!alpha)
    {
        return std::unexpected(alpha.error());
    }
    double const nDotView = Dot(*unitNormal, *unitView);
    if (nDotView <= 0.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }

    double const alphaSquared = *alpha * *alpha;
    double const cosineTheta = std::sqrt((1.0 - unitSample.y) / (1.0 + (alphaSquared - 1.0) * unitSample.y));
    double const sineTheta = std::sqrt(std::max(0.0, 1.0 - cosineTheta * cosineTheta));
    double const phi = 2.0 * kPi * unitSample.x;
    auto const halfVector =
        LocalToWorld({sineTheta * std::cos(phi), cosineTheta, sineTheta * std::sin(phi)}, *unitNormal);
    if (!halfVector)
    {
        return std::unexpected(halfVector.error());
    }
    double const vDotH = Dot(*unitView, *halfVector);
    Float3 const lightDirection = Add(Scale(*halfVector, 2.0 * vDotH), Scale(*unitView, -1.0));
    auto const unitLight = UnitDirection(lightDirection);
    if (!unitLight)
    {
        return std::unexpected(unitLight.error());
    }
    // Both inputs are unit length, but an arbitrary-frame reconstruction can
    // round a theoretically exact cosine a few ulps above one. Clamp the
    // derived cosine before passing it back through the public [0,1] contract.
    double const nDotH = std::clamp(Dot(*unitNormal, *halfVector), 0.0, 1.0);
    double const nDotL = Dot(*unitNormal, *unitLight);
    auto const ndf = EvaluateGgxNdf(nDotH, roughness);
    if (!ndf)
    {
        return std::unexpected(ndf.error());
    }
    double const halfPdf = *ndf * nDotH;
    double const lightPdf = vDotH == 0.0 ? 0.0 : halfPdf / (4.0 * std::abs(vDotH));
    if (!IsFinite(halfPdf) || !IsFinite(lightPdf) || halfPdf < 0.0 || lightPdf < 0.0)
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return GgxDirectionSample{
        .unitSample = unitSample,
        .halfVector = *halfVector,
        .lightDirection = *unitLight,
        .alpha = *alpha,
        .nDotH = nDotH,
        .vDotH = vDotH,
        .nDotL = nDotL,
        .ndf = *ndf,
        .halfVectorPdf = halfPdf,
        .lightDirectionPdf = lightPdf,
    };
}

std::expected<double, ContractError> EvaluateGgxReflectionPdf(Float3 normal, Float3 viewDirection,
                                                              Float3 lightDirection, double roughness) noexcept
{
    auto const unitNormal = UnitDirection(normal);
    auto const unitView = UnitDirection(viewDirection);
    auto const unitLight = UnitDirection(lightDirection);
    if (!unitNormal)
    {
        return std::unexpected(unitNormal.error());
    }
    if (!unitView)
    {
        return std::unexpected(unitView.error());
    }
    if (!unitLight)
    {
        return std::unexpected(unitLight.error());
    }
    auto const roughnessValid = ValidateRoughness(roughness);
    if (!roughnessValid)
    {
        return std::unexpected(roughnessValid.error());
    }
    if (Dot(*unitNormal, *unitView) <= 0.0 || Dot(*unitNormal, *unitLight) <= 0.0)
    {
        return 0.0;
    }
    Float3 const halfSum = Add(*unitView, *unitLight);
    double const halfLengthSquared = Dot(halfSum, halfSum);
    if (!IsFinite(halfLengthSquared))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (halfLengthSquared <= 1.0e-24)
    {
        return 0.0;
    }
    Float3 const halfVector = Scale(halfSum, 1.0 / std::sqrt(halfLengthSquared));
    double const nDotH = std::clamp(Dot(*unitNormal, halfVector), 0.0, 1.0);
    double const vDotH = std::abs(Dot(*unitView, halfVector));
    if (vDotH == 0.0)
    {
        return 0.0;
    }
    auto const ndf = EvaluateGgxNdf(nDotH, roughness);
    if (!ndf)
    {
        return std::unexpected(ndf.error());
    }
    double const pdf = (*ndf * nDotH) / (4.0 * vDotH);
    if (!IsFinite(pdf) || pdf < 0.0)
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return pdf;
}

std::expected<SpecularPrefilterDiagnostics, ContractError> PrefilterSpecular(EnvironmentImageView image,
                                                                             Float3 reflectionDirection,
                                                                             double roughness,
                                                                             std::uint32_t sampleCount) noexcept
{
    auto const validImage = ValidateEnvironment(image);
    if (!validImage)
    {
        return std::unexpected(validImage.error());
    }
    auto const normal = UnitDirection(reflectionDirection);
    if (!normal)
    {
        return std::unexpected(normal.error());
    }
    auto const validRoughness = ValidateRoughness(roughness);
    if (!validRoughness)
    {
        return std::unexpected(validRoughness.error());
    }
    auto const validCount = ValidateSampleCount(sampleCount);
    if (!validCount)
    {
        return std::unexpected(validCount.error());
    }

    Rgb accumulated{};
    double accumulatedWeight = 0.0;
    double minimumPdf = std::numeric_limits<double>::max();
    double maximumPdf = 0.0;
    std::uint32_t accepted = 0U;
    std::uint32_t rejected = 0U;
    for (std::uint32_t index = 0U; index < sampleCount; ++index)
    {
        auto const unitSample = HammersleySample(index, sampleCount);
        if (!unitSample)
        {
            return std::unexpected(unitSample.error());
        }
        auto const sample = SampleGgxReflection(*normal, *normal, roughness, *unitSample);
        if (!sample)
        {
            return std::unexpected(sample.error());
        }
        if (sample->nDotL <= 0.0)
        {
            ++rejected;
            continue;
        }
        auto const uv = DirectionToLatLongUv(sample->lightDirection);
        if (!uv)
        {
            return std::unexpected(uv.error());
        }
        Rgb const radiance = SampleBilinearValidated(image, *uv);
        accumulated = Add(accumulated, Scale(radiance, sample->nDotL));
        accumulatedWeight += sample->nDotL;
        minimumPdf = std::min(minimumPdf, sample->lightDirectionPdf);
        maximumPdf = std::max(maximumPdf, sample->lightDirectionPdf);
        ++accepted;
        if (!IsAccumulationFinite(accumulated, accumulatedWeight))
        {
            return std::unexpected(ContractError::NonFinite);
        }
    }
    if (accepted == 0U || accumulatedWeight <= 0.0)
    {
        return std::unexpected(ContractError::NoAcceptedSamples);
    }
    Rgb const result = Scale(accumulated, 1.0 / accumulatedWeight);
    if (!IsFinite(result))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return SpecularPrefilterDiagnostics{
        .result = result,
        .requestedSampleCount = sampleCount,
        .acceptedSampleCount = accepted,
        .rejectedBackfacingSampleCount = rejected,
        .accumulatedWeight = accumulatedWeight,
        .minimumLightPdf = minimumPdf,
        .maximumLightPdf = maximumPdf,
    };
}

std::expected<SplitSumBrdfDiagnostics, ContractError> IntegrateSplitSumBrdf(double nDotView, double roughness,
                                                                            std::uint32_t sampleCount) noexcept
{
    if (!IsFinite(nDotView))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (nDotView <= 0.0 || nDotView > 1.0)
    {
        return std::unexpected(ContractError::InvalidViewCosine);
    }
    auto const validRoughness = ValidateRoughness(roughness);
    if (!validRoughness)
    {
        return std::unexpected(validRoughness.error());
    }
    auto const validCount = ValidateSampleCount(sampleCount);
    if (!validCount)
    {
        return std::unexpected(validCount.error());
    }

    Float3 const normal{0.0, 1.0, 0.0};
    Float3 const view{std::sqrt(std::max(0.0, 1.0 - nDotView * nDotView)), nDotView, 0.0};
    auto const gView = EvaluateSmithG1(nDotView, roughness);
    if (!gView)
    {
        return std::unexpected(gView.error());
    }
    double scaleA = 0.0;
    double biasB = 0.0;
    std::uint32_t accepted = 0U;
    std::uint32_t rejected = 0U;
    for (std::uint32_t index = 0U; index < sampleCount; ++index)
    {
        auto const unitSample = HammersleySample(index, sampleCount);
        if (!unitSample)
        {
            return std::unexpected(unitSample.error());
        }
        auto const sample = SampleGgxReflection(normal, view, roughness, *unitSample);
        if (!sample)
        {
            return std::unexpected(sample.error());
        }
        if (sample->nDotL <= 0.0 || sample->nDotH <= 0.0 || sample->vDotH <= 0.0)
        {
            ++rejected;
            continue;
        }
        auto const gLight = EvaluateSmithG1(sample->nDotL, roughness);
        if (!gLight)
        {
            return std::unexpected(gLight.error());
        }
        double const geometryVisibility = (*gView * *gLight * sample->vDotH) / (sample->nDotH * nDotView);
        double const fresnelComplement = std::pow(1.0 - sample->vDotH, 5.0);
        scaleA += (1.0 - fresnelComplement) * geometryVisibility;
        biasB += fresnelComplement * geometryVisibility;
        ++accepted;
        if (!IsFinite(scaleA) || !IsFinite(biasB))
        {
            return std::unexpected(ContractError::NonFinite);
        }
    }
    scaleA /= static_cast<double>(sampleCount);
    biasB /= static_cast<double>(sampleCount);
    if (!IsFinite(scaleA) || !IsFinite(biasB))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return SplitSumBrdfDiagnostics{
        .scaleA = scaleA,
        .biasB = biasB,
        .requestedSampleCount = sampleCount,
        .acceptedSampleCount = accepted,
        .rejectedBackfacingSampleCount = rejected,
    };
}

std::expected<double, ContractError> SelectEnvironmentMip(std::uint32_t width, std::uint32_t height,
                                                          Float3 lightDirection, double lightPdf,
                                                          std::uint32_t sampleCount, double roughness,
                                                          std::uint32_t mipCount) noexcept
{
    auto const pixelCount = PixelCount(width, height);
    if (!pixelCount)
    {
        return std::unexpected(pixelCount.error());
    }
    auto const direction = UnitDirection(lightDirection);
    if (!direction)
    {
        return std::unexpected(direction.error());
    }
    if (!IsFinite(lightPdf))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (lightPdf <= 0.0)
    {
        return std::unexpected(ContractError::InvalidDensity);
    }
    auto const validCount = ValidateSampleCount(sampleCount);
    if (!validCount)
    {
        return std::unexpected(validCount.error());
    }
    auto const validRoughness = ValidateRoughness(roughness);
    if (!validRoughness)
    {
        return std::unexpected(validRoughness.error());
    }
    if (mipCount == 0U || mipCount > 32U)
    {
        return std::unexpected(ContractError::InvalidMipCount);
    }
    if (roughness == 0.0)
    {
        return 0.0;
    }

    auto const uv = DirectionToLatLongUv(*direction);
    if (!uv)
    {
        return std::unexpected(uv.error());
    }
    std::uint32_t const row = std::min(static_cast<std::uint32_t>(uv->y * static_cast<double>(height)), height - 1U);
    auto const texelSolidAngle = LatLongTexelSolidAngle(width, height, row);
    if (!texelSolidAngle)
    {
        return std::unexpected(texelSolidAngle.error());
    }
    double const mip =
        0.5 * (-std::log2(static_cast<double>(sampleCount)) - std::log2(lightPdf) - std::log2(*texelSolidAngle));
    if (!IsFinite(mip))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return std::clamp(mip, 0.0, static_cast<double>(mipCount - 1U));
}

} // namespace ch27::environment_lighting
