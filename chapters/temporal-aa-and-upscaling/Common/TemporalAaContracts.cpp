#include "TemporalAaContracts.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ch28::temporal_aa
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

[[nodiscard]] bool IsFinite(YCoCg value) noexcept
{
    return IsFinite(value.y) && IsFinite(value.co) && IsFinite(value.cg);
}

[[nodiscard]] bool IsUnitInterval(double value) noexcept
{
    return IsFinite(value) && value >= 0.0 && value <= 1.0;
}

[[nodiscard]] std::expected<void, ContractError> ValidateSceneLinearColor(Rgb color) noexcept
{
    if (!IsFinite(color))
    {
        return std::unexpected(ContractError::InvalidColor);
    }
    if (color.r < 0.0 || color.g < 0.0 || color.b < 0.0 || color.r > kMaximumSceneLinearValue ||
        color.g > kMaximumSceneLinearValue || color.b > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::ColorOutOfRange);
    }
    return {};
}

[[nodiscard]] double RadicalInverse(std::uint32_t base, std::uint32_t index) noexcept
{
    double inversePower = 1.0 / static_cast<double>(base);
    double result = 0.0;
    while (index != 0U)
    {
        std::uint32_t const digit = index % base;
        result += static_cast<double>(digit) * inversePower;
        index /= base;
        inversePower /= static_cast<double>(base);
    }
    return result;
}

[[nodiscard]] Float2 RawHaltonJitter(std::uint32_t phaseIndex) noexcept
{
    std::uint32_t const sequenceIndex = phaseIndex + 1U;
    return {
        .x = RadicalInverse(2U, sequenceIndex),
        .y = RadicalInverse(3U, sequenceIndex),
    };
}

[[nodiscard]] std::expected<void, ContractError> ValidateUnitNormal(Float3 normal) noexcept
{
    if (!IsFinite(normal))
    {
        return std::unexpected(ContractError::InvalidNormal);
    }
    double const length = std::hypot(normal.x, normal.y, normal.z);
    if (!IsFinite(length) || std::abs(length - 1.0) > 1.0e-4)
    {
        return std::unexpected(ContractError::InvalidNormal);
    }
    return {};
}

[[nodiscard]] double Dot(Float3 left, Float3 right) noexcept
{
    return (left.x * right.x) + (left.y * right.y) + (left.z * right.z);
}

[[nodiscard]] Rgb ClampRgb(Rgb value, Rgb minimum, Rgb maximum) noexcept
{
    return {
        .r = std::clamp(value.r, minimum.r, maximum.r),
        .g = std::clamp(value.g, minimum.g, maximum.g),
        .b = std::clamp(value.b, minimum.b, maximum.b),
    };
}

[[nodiscard]] bool EqualRgb(Rgb left, Rgb right) noexcept
{
    return left.r == right.r && left.g == right.g && left.b == right.b;
}

[[nodiscard]] std::expected<void, ContractError> ValidateStatistics(NeighborhoodStatistics const &statistics) noexcept
{
    if (statistics.sampleCount == 0U || statistics.sampleCount > kMaximumNeighborhoodSampleCount)
    {
        return std::unexpected(ContractError::InvalidStatistics);
    }
    if (!ValidateSceneLinearColor(statistics.rgbMinimum) || !ValidateSceneLinearColor(statistics.rgbMaximum) ||
        !IsFinite(statistics.mean) || !IsFinite(statistics.variance))
    {
        return std::unexpected(ContractError::InvalidStatistics);
    }
    if (statistics.rgbMinimum.r > statistics.rgbMaximum.r || statistics.rgbMinimum.g > statistics.rgbMaximum.g ||
        statistics.rgbMinimum.b > statistics.rgbMaximum.b || statistics.variance.y < 0.0 ||
        statistics.variance.co < 0.0 || statistics.variance.cg < 0.0 || statistics.mean.y < 0.0 ||
        statistics.mean.y > kMaximumSceneLinearValue || std::abs(statistics.mean.co) > kMaximumSceneLinearValue ||
        std::abs(statistics.mean.cg) > kMaximumSceneLinearValue ||
        statistics.variance.y > kMaximumSceneLinearValue * kMaximumSceneLinearValue ||
        statistics.variance.co > kMaximumSceneLinearValue * kMaximumSceneLinearValue ||
        statistics.variance.cg > kMaximumSceneLinearValue * kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidStatistics);
    }
    Rgb const meanSceneLinear{
        .r = statistics.mean.y + statistics.mean.co - statistics.mean.cg,
        .g = statistics.mean.y + statistics.mean.cg,
        .b = statistics.mean.y - statistics.mean.co - statistics.mean.cg,
    };
    if (!ValidateSceneLinearColor(meanSceneLinear))
    {
        return std::unexpected(ContractError::InvalidStatistics);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateConstraintSettings(
    NeighborhoodConstraintSettings const &settings) noexcept
{
    if (!IsFinite(settings.luminanceSigmaScale) || !IsFinite(settings.chromaSigmaScale) ||
        settings.luminanceSigmaScale < 0.0 || settings.luminanceSigmaScale > 10.0 || settings.chromaSigmaScale < 0.0 ||
        settings.chromaSigmaScale > 10.0)
    {
        return std::unexpected(ContractError::InvalidThreshold);
    }
    return {};
}

[[nodiscard]] double ClipAxisScale(double center, double value, double radius) noexcept
{
    double const delta = value - center;
    if (delta == 0.0)
    {
        return 1.0;
    }
    return std::min(1.0, radius / std::abs(delta));
}

[[nodiscard]] std::uint32_t ClampIndex(std::int64_t index, std::uint32_t extent) noexcept
{
    std::int64_t const maximum = static_cast<std::int64_t>(extent) - 1;
    return static_cast<std::uint32_t>(std::clamp(index, std::int64_t{0}, maximum));
}

[[nodiscard]] double CatmullRomWeight(double distance) noexcept
{
    double const x = std::abs(distance);
    if (x < 1.0)
    {
        return ((1.5 * x - 2.5) * x * x) + 1.0;
    }
    if (x < 2.0)
    {
        return (((-0.5 * x + 2.5) * x - 4.0) * x) + 2.0;
    }
    return 0.0;
}

[[nodiscard]] std::expected<double, ContractError> ValidateAndSumWeights(
    ReconstructionFootprint const &footprint) noexcept
{
    switch (footprint.kernel)
    {
    case ReconstructionKernel::Bilinear:
        if (footprint.tapCount != 4U)
        {
            return std::unexpected(ContractError::InvalidKernel);
        }
        break;
    case ReconstructionKernel::CatmullRom:
        if (footprint.tapCount != 16U)
        {
            return std::unexpected(ContractError::InvalidKernel);
        }
        break;
    default:
        return std::unexpected(ContractError::InvalidKernel);
    }
    if (footprint.tapCount > kMaximumReconstructionTapCount)
    {
        return std::unexpected(ContractError::InvalidKernel);
    }

    double sum = 0.0;
    bool observedNegativeWeight = false;
    for (std::uint32_t index = 0U; index < footprint.tapCount; ++index)
    {
        ReconstructionTap const &tap = footprint.taps[index];
        if (!IsFinite(tap.weight) || !IsFinite(tap.offsetFromCenterTexel))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (footprint.kernel == ReconstructionKernel::Bilinear && tap.weight < 0.0)
        {
            return std::unexpected(ContractError::InvalidKernel);
        }
        observedNegativeWeight = observedNegativeWeight || tap.weight < 0.0;
        sum += tap.weight;
    }
    if (!IsFinite(sum) || std::abs(sum - 1.0) > 1.0e-9 || observedNegativeWeight != footprint.hasNegativeWeights)
    {
        return std::unexpected(ContractError::InvalidKernel);
    }
    return sum;
}

[[nodiscard]] std::expected<void, ContractError> ValidateSharpeningInput(SharpeningInput const &input) noexcept
{
    auto const resolved = ValidateSceneLinearColor(input.resolvedSceneLinear);
    if (!resolved)
    {
        return std::unexpected(resolved.error());
    }
    auto const average = ValidateSceneLinearColor(input.neighborAverageSceneLinear);
    if (!average)
    {
        return std::unexpected(average.error());
    }
    auto const minimum = ValidateSceneLinearColor(input.neighborhoodMinimumSceneLinear);
    if (!minimum)
    {
        return std::unexpected(minimum.error());
    }
    auto const maximum = ValidateSceneLinearColor(input.neighborhoodMaximumSceneLinear);
    if (!maximum)
    {
        return std::unexpected(maximum.error());
    }
    if (input.neighborhoodMinimumSceneLinear.r > input.neighborhoodMaximumSceneLinear.r ||
        input.neighborhoodMinimumSceneLinear.g > input.neighborhoodMaximumSceneLinear.g ||
        input.neighborhoodMinimumSceneLinear.b > input.neighborhoodMaximumSceneLinear.b)
    {
        return std::unexpected(ContractError::InvalidSharpening);
    }
    return {};
}

} // namespace

std::expected<std::uint32_t, ContractError> PixelCount(Extent2D extent) noexcept
{
    if (extent.width == 0U || extent.height == 0U)
    {
        return std::unexpected(ContractError::InvalidDimensions);
    }
    std::uint64_t const count = static_cast<std::uint64_t>(extent.width) * static_cast<std::uint64_t>(extent.height);
    if (count > std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected(ContractError::CountOverflow);
    }
    if (extent.width > kMaximumDimension || extent.height > kMaximumDimension)
    {
        return std::unexpected(ContractError::DimensionsTooLarge);
    }
    return static_cast<std::uint32_t>(count);
}

std::expected<Float2, ContractError> PixelJitterToNdc(Float2 pixelOffset, Extent2D renderExtent) noexcept
{
    auto const pixelCount = PixelCount(renderExtent);
    if (!pixelCount)
    {
        return std::unexpected(pixelCount.error());
    }
    if (!IsFinite(pixelOffset))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    Float2 const result{
        .x = (2.0 * pixelOffset.x) / static_cast<double>(renderExtent.width),
        .y = (-2.0 * pixelOffset.y) / static_cast<double>(renderExtent.height),
    };
    if (!IsFinite(result))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return result;
}

std::expected<JitterSample, ContractError> GenerateJitter(std::uint64_t frameIndex, std::uint32_t phasePeriod,
                                                          Extent2D renderExtent) noexcept
{
    auto const pixelCount = PixelCount(renderExtent);
    if (!pixelCount)
    {
        return std::unexpected(pixelCount.error());
    }
    if (phasePeriod == 0U || phasePeriod > kMaximumJitterPhasePeriod)
    {
        return std::unexpected(ContractError::InvalidPhasePeriod);
    }

    Float2 centroid{};
    for (std::uint32_t phase = 0U; phase < phasePeriod; ++phase)
    {
        Float2 const sample = RawHaltonJitter(phase);
        centroid.x += sample.x;
        centroid.y += sample.y;
    }
    centroid.x /= static_cast<double>(phasePeriod);
    centroid.y /= static_cast<double>(phasePeriod);

    std::uint32_t const phaseIndex = static_cast<std::uint32_t>(frameIndex % phasePeriod);
    Float2 const raw = RawHaltonJitter(phaseIndex);
    Float2 const pixelOffset{.x = raw.x - centroid.x, .y = raw.y - centroid.y};
    auto const ndcOffset = PixelJitterToNdc(pixelOffset, renderExtent);
    if (!ndcOffset)
    {
        return std::unexpected(ndcOffset.error());
    }
    return JitterSample{
        .phaseIndex = phaseIndex,
        .phasePeriod = phasePeriod,
        .pixelOffset = pixelOffset,
        .ndcOffset = *ndcOffset,
    };
}

std::expected<DisplayToRenderMapping, ContractError> MapDisplayPixelToRender(PixelCoordinate displayPixel,
                                                                             Extent2D displayExtent,
                                                                             Extent2D renderExtent) noexcept
{
    auto const displayCount = PixelCount(displayExtent);
    if (!displayCount)
    {
        return std::unexpected(displayCount.error());
    }
    auto const renderCount = PixelCount(renderExtent);
    if (!renderCount)
    {
        return std::unexpected(renderCount.error());
    }
    if (displayPixel.x >= displayExtent.width || displayPixel.y >= displayExtent.height)
    {
        return std::unexpected(ContractError::PixelOutOfBounds);
    }

    double const displayX = static_cast<double>(displayPixel.x);
    double const displayY = static_cast<double>(displayPixel.y);
    double const displayWidth = static_cast<double>(displayExtent.width);
    double const displayHeight = static_cast<double>(displayExtent.height);
    double const renderWidth = static_cast<double>(renderExtent.width);
    double const renderHeight = static_cast<double>(renderExtent.height);
    Float2 const centerUv{
        .x = (displayX + 0.5) / displayWidth,
        .y = (displayY + 0.5) / displayHeight,
    };
    return DisplayToRenderMapping{
        .displayPixel = displayPixel,
        .displayCenterUv = centerUv,
        .renderCenterTexel =
            {
                .x = (centerUv.x * renderWidth) - 0.5,
                .y = (centerUv.y * renderHeight) - 0.5,
            },
        .renderFootprintMinimumTexel =
            {
                .x = ((displayX / displayWidth) * renderWidth) - 0.5,
                .y = ((displayY / displayHeight) * renderHeight) - 0.5,
            },
        .renderFootprintMaximumTexel =
            {
                .x = (((displayX + 1.0) / displayWidth) * renderWidth) - 0.5,
                .y = (((displayY + 1.0) / displayHeight) * renderHeight) - 0.5,
            },
        .renderTexelsPerDisplayPixel =
            {
                .x = renderWidth / displayWidth,
                .y = renderHeight / displayHeight,
            },
    };
}

std::expected<ReprojectionResult, ContractError> ReprojectToHistory(ReprojectionInput const &input) noexcept
{
    if (!IsFinite(input.currentJitteredUv) || !IsFinite(input.motionPreviousMinusCurrentUv) ||
        !IsFinite(input.currentJitterUv) || !IsFinite(input.previousJitterUv))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    ReprojectionResult const result{
        .currentJitteredUv = input.currentJitteredUv,
        .currentUnjitteredUv =
            {
                .x = input.currentJitteredUv.x - input.currentJitterUv.x,
                .y = input.currentJitteredUv.y - input.currentJitterUv.y,
            },
        .motionPreviousMinusCurrentUv = input.motionPreviousMinusCurrentUv,
        .jitterDeltaPreviousMinusCurrentUv =
            {
                .x = input.previousJitterUv.x - input.currentJitterUv.x,
                .y = input.previousJitterUv.y - input.currentJitterUv.y,
            },
        .previousHistoryUv =
            {
                .x = input.currentJitteredUv.x + input.motionPreviousMinusCurrentUv.x + input.previousJitterUv.x -
                     input.currentJitterUv.x,
                .y = input.currentJitteredUv.y + input.motionPreviousMinusCurrentUv.y + input.previousJitterUv.y -
                     input.currentJitterUv.y,
            },
    };
    if (!IsFinite(result.currentUnjitteredUv) || !IsFinite(result.previousHistoryUv))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return result;
}

std::expected<BilinearFootprint, ContractError> BuildBilinearHistoryFootprint(Float2 historyUv,
                                                                              Extent2D historyExtent) noexcept
{
    auto const count = PixelCount(historyExtent);
    if (!count)
    {
        return std::unexpected(count.error());
    }
    if (!IsFinite(historyUv))
    {
        return std::unexpected(ContractError::InvalidUv);
    }

    double const width = static_cast<double>(historyExtent.width);
    double const height = static_cast<double>(historyExtent.height);
    double const minimumU = 0.5 / width;
    double const minimumV = 0.5 / height;
    double const maximumU = 1.0 - minimumU;
    double const maximumV = 1.0 - minimumV;
    if (historyUv.x < minimumU || historyUv.x > maximumU || historyUv.y < minimumV || historyUv.y > maximumV)
    {
        return std::unexpected(ContractError::HistoryFootprintOutOfBounds);
    }

    Float2 const coordinate{
        .x = (historyUv.x * width) - 0.5,
        .y = (historyUv.y * height) - 0.5,
    };
    double const floorX = std::floor(coordinate.x);
    double const floorY = std::floor(coordinate.y);
    std::int64_t const x0 = static_cast<std::int64_t>(floorX);
    std::int64_t const y0 = static_cast<std::int64_t>(floorY);
    double const fractionX = coordinate.x - floorX;
    double const fractionY = coordinate.y - floorY;
    std::uint32_t const firstX = ClampIndex(x0, historyExtent.width);
    std::uint32_t const secondX = ClampIndex(x0 + 1, historyExtent.width);
    std::uint32_t const firstY = ClampIndex(y0, historyExtent.height);
    std::uint32_t const secondY = ClampIndex(y0 + 1, historyExtent.height);

    BilinearFootprint const footprint{
        .historyUv = historyUv,
        .texelCoordinate = coordinate,
        .taps =
            {
                BilinearTap{.pixel = {.x = firstX, .y = firstY}, .weight = (1.0 - fractionX) * (1.0 - fractionY)},
                BilinearTap{.pixel = {.x = secondX, .y = firstY}, .weight = fractionX * (1.0 - fractionY)},
                BilinearTap{.pixel = {.x = firstX, .y = secondY}, .weight = (1.0 - fractionX) * fractionY},
                BilinearTap{.pixel = {.x = secondX, .y = secondY}, .weight = fractionX * fractionY},
            },
    };
    return footprint;
}

std::expected<HistoryValidationResult, ContractError> ValidateHistory(
    HistoryValidationInput const &input, HistoryValidationSettings const &settings) noexcept
{
    auto const historyCount = PixelCount(input.historyExtent);
    if (!historyCount)
    {
        return std::unexpected(historyCount.error());
    }
    if (!IsFinite(input.previousHistoryUv))
    {
        return std::unexpected(ContractError::InvalidUv);
    }
    if (!IsFinite(input.expectedPreviousViewDepth) || !IsFinite(input.sampledPreviousViewDepth) ||
        input.expectedPreviousViewDepth <= 0.0 || input.sampledPreviousViewDepth <= 0.0)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }
    if (!ValidateUnitNormal(input.currentNormal) || !ValidateUnitNormal(input.sampledPreviousNormal))
    {
        return std::unexpected(ContractError::InvalidNormal);
    }
    if (!IsUnitInterval(input.reactiveMask) || !IsUnitInterval(input.disocclusionEvidence))
    {
        return std::unexpected(ContractError::InvalidMask);
    }
    if (!IsFinite(input.currentPreExposure) || !IsFinite(input.previousPreExposure) ||
        input.currentPreExposure <= 0.0 || input.previousPreExposure <= 0.0)
    {
        return std::unexpected(ContractError::InvalidExposure);
    }
    if (!IsFinite(input.currentLuminance) || !IsFinite(input.sampledHistoryLuminance) || input.currentLuminance < 0.0 ||
        input.sampledHistoryLuminance < 0.0 || input.currentLuminance > kMaximumSceneLinearValue ||
        input.sampledHistoryLuminance > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidColor);
    }

    bool const validThresholds =
        IsFinite(settings.absoluteDepthThreshold) && settings.absoluteDepthThreshold >= 0.0 &&
        IsFinite(settings.relativeDepthThreshold) && settings.relativeDepthThreshold >= 0.0 &&
        IsFinite(settings.minimumNormalCosine) && settings.minimumNormalCosine >= -1.0 &&
        settings.minimumNormalCosine <= 1.0 && IsUnitInterval(settings.reactiveRejectThreshold) &&
        IsUnitInterval(settings.disocclusionRejectThreshold) && IsFinite(settings.maximumExposureRatio) &&
        settings.maximumExposureRatio >= 1.0 && IsFinite(settings.maximumLuminanceRatio) &&
        settings.maximumLuminanceRatio >= 1.0 && IsFinite(settings.luminanceEpsilon) && settings.luminanceEpsilon > 0.0;
    if (!validThresholds)
    {
        return std::unexpected(ContractError::InvalidThreshold);
    }

    HistoryValidationResult result{};
    if (!input.hasHistory)
    {
        result.reasons |= HistoryRejectReason::NoHistory;
    }
    if (input.resetRequested)
    {
        result.reasons |= HistoryRejectReason::Reset;
    }
    if (!BuildBilinearHistoryFootprint(input.previousHistoryUv, input.historyExtent))
    {
        result.reasons |= HistoryRejectReason::FootprintOutOfBounds;
    }

    double const depthScale = std::max(input.expectedPreviousViewDepth, input.sampledPreviousViewDepth);
    result.depthTolerance = std::max(settings.absoluteDepthThreshold, settings.relativeDepthThreshold * depthScale);
    if (!IsFinite(result.depthTolerance))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    if (std::abs(input.sampledPreviousViewDepth - input.expectedPreviousViewDepth) > result.depthTolerance)
    {
        result.reasons |= HistoryRejectReason::DepthMismatch;
    }

    result.normalAgreement = std::clamp(Dot(input.currentNormal, input.sampledPreviousNormal), -1.0, 1.0);
    if (result.normalAgreement < settings.minimumNormalCosine)
    {
        result.reasons |= HistoryRejectReason::NormalMismatch;
    }
    if (input.currentObjectId != input.sampledPreviousObjectId)
    {
        result.reasons |= HistoryRejectReason::ObjectMismatch;
    }
    if (input.reactiveMask >= settings.reactiveRejectThreshold)
    {
        result.reasons |= HistoryRejectReason::Reactive;
    }
    if (input.disocclusionEvidence >= settings.disocclusionRejectThreshold)
    {
        result.reasons |= HistoryRejectReason::Disocclusion;
    }

    result.historyToCurrentExposureScale = input.currentPreExposure / input.previousPreExposure;
    if (!IsFinite(result.historyToCurrentExposureScale) || result.historyToCurrentExposureScale <= 0.0)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    result.exposureRatio = std::max(result.historyToCurrentExposureScale, 1.0 / result.historyToCurrentExposureScale);
    if (!IsFinite(result.exposureRatio))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    if (result.exposureRatio > settings.maximumExposureRatio)
    {
        result.reasons |= HistoryRejectReason::Exposure;
    }

    double const scaledHistoryLuminance = input.sampledHistoryLuminance * result.historyToCurrentExposureScale;
    if (!IsFinite(scaledHistoryLuminance))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    double const brighter = std::max(input.currentLuminance, scaledHistoryLuminance);
    double const darker = std::min(input.currentLuminance, scaledHistoryLuminance);
    result.luminanceRatio = (brighter + settings.luminanceEpsilon) / (darker + settings.luminanceEpsilon);
    if (!IsFinite(result.luminanceRatio))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    if (result.luminanceRatio > settings.maximumLuminanceRatio)
    {
        result.reasons |= HistoryRejectReason::LuminanceChange;
    }
    return result;
}

std::expected<YCoCg, ContractError> SceneLinearRgbToYCoCg(Rgb color) noexcept
{
    auto const validColor = ValidateSceneLinearColor(color);
    if (!validColor)
    {
        return std::unexpected(validColor.error());
    }
    YCoCg const result{
        .y = (0.25 * color.r) + (0.5 * color.g) + (0.25 * color.b),
        .co = (0.5 * color.r) - (0.5 * color.b),
        .cg = (-0.25 * color.r) + (0.5 * color.g) - (0.25 * color.b),
    };
    if (!IsFinite(result))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return result;
}

std::expected<Rgb, ContractError> YCoCgToSceneLinearRgb(YCoCg color) noexcept
{
    if (!IsFinite(color))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    Rgb const result{
        .r = color.y + color.co - color.cg,
        .g = color.y + color.cg,
        .b = color.y - color.co - color.cg,
    };
    if (!IsFinite(result))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    auto const validResult = ValidateSceneLinearColor(result);
    if (!validResult)
    {
        return std::unexpected(validResult.error());
    }
    return result;
}

std::expected<NeighborhoodStatistics, ContractError> ComputeNeighborhoodStatistics(
    std::span<Rgb const> sceneLinearSamples) noexcept
{
    if (sceneLinearSamples.empty())
    {
        return std::unexpected(ContractError::EmptyNeighborhood);
    }
    if (sceneLinearSamples.size() > kMaximumNeighborhoodSampleCount)
    {
        return std::unexpected(ContractError::TooManyNeighborhoodSamples);
    }

    NeighborhoodStatistics result{
        .sampleCount = static_cast<std::uint32_t>(sceneLinearSamples.size()),
        .rgbMinimum = sceneLinearSamples.front(),
        .rgbMaximum = sceneLinearSamples.front(),
    };
    YCoCg runningMean{};
    YCoCg sumSquaredDeviation{};
    std::uint32_t processedCount = 0U;
    for (Rgb const sample : sceneLinearSamples)
    {
        auto const converted = SceneLinearRgbToYCoCg(sample);
        if (!converted)
        {
            return std::unexpected(converted.error());
        }
        result.rgbMinimum = {
            .r = std::min(result.rgbMinimum.r, sample.r),
            .g = std::min(result.rgbMinimum.g, sample.g),
            .b = std::min(result.rgbMinimum.b, sample.b),
        };
        result.rgbMaximum = {
            .r = std::max(result.rgbMaximum.r, sample.r),
            .g = std::max(result.rgbMaximum.g, sample.g),
            .b = std::max(result.rgbMaximum.b, sample.b),
        };
        ++processedCount;
        double const inverseProcessedCount = 1.0 / static_cast<double>(processedCount);
        YCoCg const delta{
            .y = converted->y - runningMean.y,
            .co = converted->co - runningMean.co,
            .cg = converted->cg - runningMean.cg,
        };
        runningMean.y += delta.y * inverseProcessedCount;
        runningMean.co += delta.co * inverseProcessedCount;
        runningMean.cg += delta.cg * inverseProcessedCount;
        YCoCg const updatedDelta{
            .y = converted->y - runningMean.y,
            .co = converted->co - runningMean.co,
            .cg = converted->cg - runningMean.cg,
        };
        sumSquaredDeviation.y += delta.y * updatedDelta.y;
        sumSquaredDeviation.co += delta.co * updatedDelta.co;
        sumSquaredDeviation.cg += delta.cg * updatedDelta.cg;
    }

    double const inverseCount = 1.0 / static_cast<double>(result.sampleCount);
    result.mean = runningMean;
    result.variance = {
        .y = std::max(0.0, sumSquaredDeviation.y * inverseCount),
        .co = std::max(0.0, sumSquaredDeviation.co * inverseCount),
        .cg = std::max(0.0, sumSquaredDeviation.cg * inverseCount),
    };
    if (!IsFinite(result.mean) || !IsFinite(result.variance))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return result;
}

std::expected<NeighborhoodConstraintResult, ContractError> ConstrainHistory(
    Rgb historySceneLinear, NeighborhoodStatistics const &statistics,
    NeighborhoodConstraintSettings const &settings) noexcept
{
    auto const validHistory = ValidateSceneLinearColor(historySceneLinear);
    if (!validHistory)
    {
        return std::unexpected(validHistory.error());
    }
    auto const validStatistics = ValidateStatistics(statistics);
    if (!validStatistics)
    {
        return std::unexpected(validStatistics.error());
    }
    auto const validSettings = ValidateConstraintSettings(settings);
    if (!validSettings)
    {
        return std::unexpected(validSettings.error());
    }

    auto const historyYCoCg = SceneLinearRgbToYCoCg(historySceneLinear);
    if (!historyYCoCg)
    {
        return std::unexpected(historyYCoCg.error());
    }
    double const luminanceRadius = settings.luminanceSigmaScale * std::sqrt(statistics.variance.y);
    double const coRadius = settings.chromaSigmaScale * std::sqrt(statistics.variance.co);
    double const cgRadius = settings.chromaSigmaScale * std::sqrt(statistics.variance.cg);
    double const clipScale = std::min({ClipAxisScale(statistics.mean.y, historyYCoCg->y, luminanceRadius),
                                       ClipAxisScale(statistics.mean.co, historyYCoCg->co, coRadius),
                                       ClipAxisScale(statistics.mean.cg, historyYCoCg->cg, cgRadius)});
    YCoCg const clippedYCoCg{
        .y = statistics.mean.y + ((historyYCoCg->y - statistics.mean.y) * clipScale),
        .co = statistics.mean.co + ((historyYCoCg->co - statistics.mean.co) * clipScale),
        .cg = statistics.mean.cg + ((historyYCoCg->cg - statistics.mean.cg) * clipScale),
    };
    Rgb varianceClipped = historySceneLinear;
    if (clipScale < 1.0)
    {
        auto const convertedClip = YCoCgToSceneLinearRgb(clippedYCoCg);
        if (!convertedClip)
        {
            return std::unexpected(convertedClip.error());
        }
        varianceClipped = *convertedClip;
    }
    Rgb const constrained = ClampRgb(varianceClipped, statistics.rgbMinimum, statistics.rgbMaximum);
    return NeighborhoodConstraintResult{
        .inputHistory = historySceneLinear,
        .varianceClippedHistory = varianceClipped,
        .constrainedHistory = constrained,
        .varianceClipScale = clipScale,
        .rgbBoxClamped = !EqualRgb(varianceClipped, constrained),
    };
}

std::expected<FeedbackResult, ContractError> ComputeHistoryFeedback(FeedbackInput const &input,
                                                                    FeedbackSettings const &settings) noexcept
{
    if (!IsFinite(input.motionPreviousMinusCurrentUv) || !IsUnitInterval(input.reactiveMask) ||
        !IsUnitInterval(input.disocclusionEvidence))
    {
        return std::unexpected(ContractError::InvalidFeedback);
    }
    bool const validSettings =
        IsUnitInterval(settings.minimumFeedback) && IsUnitInterval(settings.maximumFeedback) &&
        settings.minimumFeedback <= settings.maximumFeedback && IsFinite(settings.motionForZeroFeedbackUv) &&
        settings.motionForZeroFeedbackUv > 0.0 && IsUnitInterval(settings.reactiveInfluence) &&
        IsUnitInterval(settings.disocclusionInfluence) && settings.fullConfidenceSampleCount > 0U &&
        settings.maximumSampleCount > 0U && settings.fullConfidenceSampleCount <= settings.maximumSampleCount &&
        settings.maximumSampleCount <= kMaximumHistorySampleCount;
    if (!validSettings)
    {
        return std::unexpected(ContractError::InvalidFeedback);
    }
    if (input.previousSampleCount > settings.maximumSampleCount)
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }

    double const motionMagnitude =
        std::hypot(input.motionPreviousMinusCurrentUv.x, input.motionPreviousMinusCurrentUv.y);
    if (!IsFinite(motionMagnitude))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    FeedbackResult result{
        .validityFactor = input.rejectionReasons == HistoryRejectReason::None ? 1.0 : 0.0,
        .sampleCountFactor = std::min(1.0, static_cast<double>(input.previousSampleCount) /
                                               static_cast<double>(settings.fullConfidenceSampleCount)),
        .motionFactor = std::max(0.0, 1.0 - (motionMagnitude / settings.motionForZeroFeedbackUv)),
        .reactiveFactor = 1.0 - (settings.reactiveInfluence * input.reactiveMask),
        .disocclusionFactor = 1.0 - (settings.disocclusionInfluence * input.disocclusionEvidence),
    };
    result.unconstrainedFeedback =
        settings.minimumFeedback + ((settings.maximumFeedback - settings.minimumFeedback) * result.sampleCountFactor);
    result.historyFeedback = result.validityFactor * result.unconstrainedFeedback * result.motionFactor *
                             result.reactiveFactor * result.disocclusionFactor;
    if (!IsFinite(result.historyFeedback))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return result;
}

std::expected<TemporalResolveResult, ContractError> ResolveTemporal(
    TemporalResolveInput const &input, NeighborhoodConstraintSettings const &constraintSettings,
    FeedbackSettings const &feedbackSettings) noexcept
{
    auto const validCurrent = ValidateSceneLinearColor(input.currentSceneLinear);
    if (!validCurrent)
    {
        return std::unexpected(validCurrent.error());
    }
    auto const validHistory = ValidateSceneLinearColor(input.historySceneLinearPreviousExposure);
    if (!validHistory)
    {
        return std::unexpected(validHistory.error());
    }
    auto const validStatistics = ValidateStatistics(input.neighborhood);
    if (!validStatistics)
    {
        return std::unexpected(validStatistics.error());
    }
    auto const validConstraintSettings = ValidateConstraintSettings(constraintSettings);
    if (!validConstraintSettings)
    {
        return std::unexpected(validConstraintSettings.error());
    }
    if (!IsFinite(input.validation.historyToCurrentExposureScale) ||
        input.validation.historyToCurrentExposureScale <= 0.0)
    {
        return std::unexpected(ContractError::InvalidExposure);
    }

    FeedbackInput const feedbackInput{
        .rejectionReasons = input.validation.reasons,
        .motionPreviousMinusCurrentUv = input.motionPreviousMinusCurrentUv,
        .reactiveMask = input.reactiveMask,
        .disocclusionEvidence = input.disocclusionEvidence,
        .previousSampleCount = input.previousSampleCount,
    };
    auto const feedback = ComputeHistoryFeedback(feedbackInput, feedbackSettings);
    if (!feedback)
    {
        return std::unexpected(feedback.error());
    }

    TemporalResolveResult result{
        .feedback = *feedback,
        .rejectionReasons = input.validation.reasons,
    };
    if (!input.validation.IsValid())
    {
        result.exposureAdjustedHistory = input.currentSceneLinear;
        result.constraint = {
            .inputHistory = input.currentSceneLinear,
            .varianceClippedHistory = input.currentSceneLinear,
            .constrainedHistory = input.currentSceneLinear,
        };
        result.outputSceneLinear = input.currentSceneLinear;
        result.nextSampleCount = 1U;
        return result;
    }

    result.exposureAdjustedHistory = {
        .r = input.historySceneLinearPreviousExposure.r * input.validation.historyToCurrentExposureScale,
        .g = input.historySceneLinearPreviousExposure.g * input.validation.historyToCurrentExposureScale,
        .b = input.historySceneLinearPreviousExposure.b * input.validation.historyToCurrentExposureScale,
    };
    if (!IsFinite(result.exposureAdjustedHistory) || result.exposureAdjustedHistory.r > kMaximumSceneLinearValue ||
        result.exposureAdjustedHistory.g > kMaximumSceneLinearValue ||
        result.exposureAdjustedHistory.b > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }

    auto const constraint = ConstrainHistory(result.exposureAdjustedHistory, input.neighborhood, constraintSettings);
    if (!constraint)
    {
        return std::unexpected(constraint.error());
    }
    result.constraint = *constraint;
    double const historyWeight = feedback->historyFeedback;
    double const currentWeight = 1.0 - historyWeight;
    result.outputSceneLinear = {
        .r = (input.currentSceneLinear.r * currentWeight) + (constraint->constrainedHistory.r * historyWeight),
        .g = (input.currentSceneLinear.g * currentWeight) + (constraint->constrainedHistory.g * historyWeight),
        .b = (input.currentSceneLinear.b * currentWeight) + (constraint->constrainedHistory.b * historyWeight),
    };
    if (!IsFinite(result.outputSceneLinear))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    result.nextSampleCount = std::min(input.previousSampleCount + 1U, feedbackSettings.maximumSampleCount);
    return result;
}

std::expected<ReconstructionFootprint, ContractError> BuildReconstructionFootprint(
    DisplayToRenderMapping const &mapping, Extent2D renderExtent, ReconstructionKernel kernel,
    double kernelRadius) noexcept
{
    auto const count = PixelCount(renderExtent);
    if (!count)
    {
        return std::unexpected(count.error());
    }
    if (!IsFinite(mapping.renderCenterTexel) || mapping.renderCenterTexel.x < -0.5 ||
        mapping.renderCenterTexel.y < -0.5 ||
        mapping.renderCenterTexel.x > static_cast<double>(renderExtent.width) - 0.5 ||
        mapping.renderCenterTexel.y > static_cast<double>(renderExtent.height) - 0.5)
    {
        return std::unexpected(ContractError::InvalidUv);
    }
    if (!IsFinite(kernelRadius))
    {
        return std::unexpected(ContractError::InvalidKernelRadius);
    }

    ReconstructionFootprint result{.kernel = kernel};
    double const floorX = std::floor(mapping.renderCenterTexel.x);
    double const floorY = std::floor(mapping.renderCenterTexel.y);
    std::int64_t const baseX = static_cast<std::int64_t>(floorX);
    std::int64_t const baseY = static_cast<std::int64_t>(floorY);

    switch (kernel)
    {
    case ReconstructionKernel::Bilinear:
    {
        if (kernelRadius != 1.0)
        {
            return std::unexpected(ContractError::InvalidKernelRadius);
        }
        result.tapCount = 4U;
        double const fractionX = mapping.renderCenterTexel.x - floorX;
        double const fractionY = mapping.renderCenterTexel.y - floorY;
        std::array<double, 2U> const weightsX{1.0 - fractionX, fractionX};
        std::array<double, 2U> const weightsY{1.0 - fractionY, fractionY};
        std::uint32_t tapIndex = 0U;
        for (std::uint32_t y = 0U; y < 2U; ++y)
        {
            for (std::uint32_t x = 0U; x < 2U; ++x)
            {
                std::int64_t const sourceX = baseX + static_cast<std::int64_t>(x);
                std::int64_t const sourceY = baseY + static_cast<std::int64_t>(y);
                result.taps[tapIndex] = {
                    .pixel =
                        {
                            .x = ClampIndex(sourceX, renderExtent.width),
                            .y = ClampIndex(sourceY, renderExtent.height),
                        },
                    .offsetFromCenterTexel =
                        {
                            .x = static_cast<double>(sourceX) - mapping.renderCenterTexel.x,
                            .y = static_cast<double>(sourceY) - mapping.renderCenterTexel.y,
                        },
                    .weight = weightsX[x] * weightsY[y],
                };
                ++tapIndex;
            }
        }
        break;
    }
    case ReconstructionKernel::CatmullRom:
    {
        if (kernelRadius != 2.0)
        {
            return std::unexpected(ContractError::InvalidKernelRadius);
        }
        result.tapCount = 16U;
        std::uint32_t tapIndex = 0U;
        double weightSum = 0.0;
        for (std::int64_t yOffset = -1; yOffset <= 2; ++yOffset)
        {
            for (std::int64_t xOffset = -1; xOffset <= 2; ++xOffset)
            {
                std::int64_t const sourceX = baseX + xOffset;
                std::int64_t const sourceY = baseY + yOffset;
                double const offsetX = static_cast<double>(sourceX) - mapping.renderCenterTexel.x;
                double const offsetY = static_cast<double>(sourceY) - mapping.renderCenterTexel.y;
                double const weight = CatmullRomWeight(offsetX) * CatmullRomWeight(offsetY);
                result.taps[tapIndex] = {
                    .pixel =
                        {
                            .x = ClampIndex(sourceX, renderExtent.width),
                            .y = ClampIndex(sourceY, renderExtent.height),
                        },
                    .offsetFromCenterTexel = {.x = offsetX, .y = offsetY},
                    .weight = weight,
                };
                result.hasNegativeWeights = result.hasNegativeWeights || weight < 0.0;
                weightSum += weight;
                ++tapIndex;
            }
        }
        if (!IsFinite(weightSum) || std::abs(weightSum) < 1.0e-12)
        {
            return std::unexpected(ContractError::ArithmeticOverflow);
        }
        for (std::uint32_t index = 0U; index < result.tapCount; ++index)
        {
            result.taps[index].weight /= weightSum;
        }
        break;
    }
    default:
        return std::unexpected(ContractError::InvalidKernel);
    }

    auto const weightSum = ValidateAndSumWeights(result);
    if (!weightSum)
    {
        return std::unexpected(weightSum.error());
    }
    return result;
}

std::expected<Rgb, ContractError> ReconstructSceneLinear(SceneLinearImageView image,
                                                         ReconstructionFootprint const &footprint) noexcept
{
    auto const expectedCount = PixelCount(image.extent);
    if (!expectedCount)
    {
        return std::unexpected(expectedCount.error());
    }
    if (image.pixels.size() != *expectedCount)
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    auto const weightSum = ValidateAndSumWeights(footprint);
    if (!weightSum)
    {
        return std::unexpected(weightSum.error());
    }

    Rgb result{};
    for (std::uint32_t index = 0U; index < footprint.tapCount; ++index)
    {
        ReconstructionTap const &tap = footprint.taps[index];
        if (tap.pixel.x >= image.extent.width || tap.pixel.y >= image.extent.height)
        {
            return std::unexpected(ContractError::PixelOutOfBounds);
        }
        std::size_t const pixelIndex = static_cast<std::size_t>(tap.pixel.y) * image.extent.width + tap.pixel.x;
        Rgb const color = image.pixels[pixelIndex];
        auto const validColor = ValidateSceneLinearColor(color);
        if (!validColor)
        {
            return std::unexpected(validColor.error());
        }
        result.r += color.r * tap.weight;
        result.g += color.g * tap.weight;
        result.b += color.b * tap.weight;
    }
    if (!IsFinite(result))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return ClampRgb(result, {}, {kMaximumSceneLinearValue, kMaximumSceneLinearValue, kMaximumSceneLinearValue});
}

std::expected<SharpeningResult, ContractError> SharpenResolved(SharpeningInput const &input,
                                                               SharpeningSettings const &settings) noexcept
{
    auto const validInput = ValidateSharpeningInput(input);
    if (!validInput)
    {
        return std::unexpected(validInput.error());
    }
    bool const validSettings = IsFinite(settings.strength) && settings.strength >= 0.0 && settings.strength <= 2.0 &&
                               IsUnitInterval(settings.negativeLobeLimit) && IsUnitInterval(settings.overshootLimit);
    if (!validSettings)
    {
        return std::unexpected(ContractError::InvalidSharpening);
    }

    SharpeningResult result{
        .requestedDetail =
            {
                .r = settings.strength * (input.resolvedSceneLinear.r - input.neighborAverageSceneLinear.r),
                .g = settings.strength * (input.resolvedSceneLinear.g - input.neighborAverageSceneLinear.g),
                .b = settings.strength * (input.resolvedSceneLinear.b - input.neighborAverageSceneLinear.b),
            },
    };

    auto sharpenChannel = [settings](double center, double minimum, double maximum,
                                     double requestedDetail) noexcept -> std::array<double, 2U>
    {
        double const range = maximum - minimum;
        double const lower = std::max(0.0, minimum - (settings.negativeLobeLimit * range));
        double const upper = maximum + (settings.overshootLimit * range);
        double const output = std::clamp(center + requestedDetail, lower, upper);
        return {output - center, output};
    };
    std::array<double, 2U> const red =
        sharpenChannel(input.resolvedSceneLinear.r, input.neighborhoodMinimumSceneLinear.r,
                       input.neighborhoodMaximumSceneLinear.r, result.requestedDetail.r);
    std::array<double, 2U> const green =
        sharpenChannel(input.resolvedSceneLinear.g, input.neighborhoodMinimumSceneLinear.g,
                       input.neighborhoodMaximumSceneLinear.g, result.requestedDetail.g);
    std::array<double, 2U> const blue =
        sharpenChannel(input.resolvedSceneLinear.b, input.neighborhoodMinimumSceneLinear.b,
                       input.neighborhoodMaximumSceneLinear.b, result.requestedDetail.b);
    result.appliedDetail = {.r = red[0U], .g = green[0U], .b = blue[0U]};
    result.outputSceneLinear = {.r = red[1U], .g = green[1U], .b = blue[1U]};
    if (!IsFinite(result.outputSceneLinear) || result.outputSceneLinear.r > kMaximumSceneLinearValue ||
        result.outputSceneLinear.g > kMaximumSceneLinearValue || result.outputSceneLinear.b > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return result;
}

} // namespace ch28::temporal_aa
