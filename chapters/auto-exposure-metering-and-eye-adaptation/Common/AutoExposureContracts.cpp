#include "AutoExposureContracts.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ch31::auto_exposure
{
namespace
{

constexpr std::uint64_t kFnvOffsetBasis = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

constexpr double kRec709RedWeight = 0.2126;
constexpr double kRec709GreenWeight = 0.7152;
constexpr double kRec709BlueWeight = 0.0722;

[[nodiscard]] bool IsFinite(double value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool IsFinite(Rgb value) noexcept
{
    return IsFinite(value.r) && IsFinite(value.g) && IsFinite(value.b);
}

[[nodiscard]] std::uint64_t HashByte(std::uint64_t hash, std::uint8_t byte) noexcept
{
    return (hash ^ static_cast<std::uint64_t>(byte)) * kFnvPrime;
}

[[nodiscard]] std::uint64_t HashUint64(std::uint64_t hash, std::uint64_t value) noexcept
{
    std::uint64_t result = hash;
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U)
    {
        result = HashByte(result, static_cast<std::uint8_t>((value >> shift) & 0xFFULL));
    }
    return result;
}

// Hashing the bit pattern is what makes an identity stable across compilers and runs. Negative zero is folded onto
// positive zero first, because the two compare equal and must not produce different identities.
[[nodiscard]] std::uint64_t HashDouble(std::uint64_t hash, double value) noexcept
{
    double const canonical = (value == 0.0) ? 0.0 : value;
    return HashUint64(hash, std::bit_cast<std::uint64_t>(canonical));
}

[[nodiscard]] std::uint64_t HashBool(std::uint64_t hash, bool value) noexcept
{
    return HashByte(hash, value ? 1U : 0U);
}

[[nodiscard]] std::uint64_t HashEnum(std::uint64_t hash, std::uint8_t value) noexcept
{
    return HashByte(hash, value);
}

[[nodiscard]] std::size_t RejectionIndex(SampleRejection rejection) noexcept
{
    return static_cast<std::size_t>(static_cast<std::uint8_t>(rejection));
}

[[nodiscard]] std::expected<void, ContractError> ValidateSampleExtent(Extent2D extent) noexcept
{
    if (extent.width == 0U || extent.height == 0U)
    {
        return std::unexpected(ContractError::InvalidExtent);
    }
    if (extent.width > kMaximumSampleDimension || extent.height > kMaximumSampleDimension)
    {
        return std::unexpected(ContractError::ExtentTooLarge);
    }
    std::uint64_t const count = static_cast<std::uint64_t>(extent.width) * static_cast<std::uint64_t>(extent.height);
    if (count > static_cast<std::uint64_t>(kMaximumSampleCount))
    {
        return std::unexpected(ContractError::ExtentTooLarge);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidatePreExposure(double preExposure) noexcept
{
    if (!IsFinite(preExposure))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (preExposure < kMinimumPreExposure || preExposure > kMaximumPreExposure)
    {
        return std::unexpected(ContractError::InvalidPreExposure);
    }
    return {};
}

// A caller may hand back a facts value it constructed itself, so the derived quantities are re-checked instead of
// trusted. The check is the same one ValidateHistogramLayout performs, expressed on the derived fields.
[[nodiscard]] std::expected<void, ContractError> ValidateLayoutFacts(HistogramLayoutFacts const &facts) noexcept
{
    if (facts.binCount < kMinimumBinCount || facts.binCount > kMaximumBinCount)
    {
        return std::unexpected(ContractError::InvalidBinCount);
    }
    if (!IsFinite(facts.minimumLog2Luminance) || !IsFinite(facts.maximumLog2Luminance) ||
        !IsFinite(facts.log2LuminanceSpan))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    double const span = facts.maximumLog2Luminance - facts.minimumLog2Luminance;
    if (span < kMinimumLog2LuminanceSpan || span > kMaximumLog2LuminanceSpan || facts.log2LuminanceSpan != span)
    {
        return std::unexpected(ContractError::InvalidLog2LuminanceRange);
    }
    return {};
}

[[nodiscard]] double LowerEdge(HistogramLayoutFacts const &facts, std::uint32_t binEdge) noexcept
{
    return facts.minimumLog2Luminance +
           (facts.log2LuminanceSpan * static_cast<double>(binEdge)) / static_cast<double>(facts.binCount);
}

[[nodiscard]] double CentreOf(HistogramLayoutFacts const &facts, std::uint32_t bin) noexcept
{
    double const numerator = facts.log2LuminanceSpan * (2.0 * static_cast<double>(bin) + 1.0);
    return facts.minimumLog2Luminance + (numerator / (2.0 * static_cast<double>(facts.binCount)));
}

[[nodiscard]] std::expected<void, ContractError> ValidateCentreSettings(CentreWeightSettings const &settings) noexcept
{
    if (!IsFinite(settings.centreWeight) || !IsFinite(settings.edgeWeight) || !IsFinite(settings.falloffPower))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.centreWeight < 0.0 || settings.centreWeight > 1.0 || settings.edgeWeight < 0.0 ||
        settings.edgeWeight > 1.0)
    {
        return std::unexpected(ContractError::InvalidWeight);
    }
    if (settings.falloffPower < kMinimumCentreFalloffPower || settings.falloffPower > kMaximumCentreFalloffPower)
    {
        return std::unexpected(ContractError::InvalidCentreProfile);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateTargetSettings(TargetExposureSettings const &settings) noexcept
{
    if (!IsFinite(settings.calibration.middleGreyLuminance) || !IsFinite(settings.exposureCompensationStops) ||
        !IsFinite(settings.minimumExposureStops) || !IsFinite(settings.maximumExposureStops))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.calibration.middleGreyLuminance <= 0.0 ||
        settings.calibration.middleGreyLuminance > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidMiddleGrey);
    }
    if (std::abs(settings.exposureCompensationStops) > kMaximumCompensationStops)
    {
        return std::unexpected(ContractError::InvalidCompensation);
    }
    if (std::abs(settings.minimumExposureStops) > kMaximumExposureStops ||
        std::abs(settings.maximumExposureStops) > kMaximumExposureStops ||
        settings.minimumExposureStops > settings.maximumExposureStops)
    {
        return std::unexpected(ContractError::InvalidExposureBounds);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateAdaptationSettings(AdaptationSettings const &settings) noexcept
{
    if (!IsFinite(settings.exposureIncreaseSpeedPerSecond) || !IsFinite(settings.exposureDecreaseSpeedPerSecond))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.exposureIncreaseSpeedPerSecond < 0.0 ||
        settings.exposureIncreaseSpeedPerSecond > kMaximumAdaptationSpeedPerSecond ||
        settings.exposureDecreaseSpeedPerSecond < 0.0 ||
        settings.exposureDecreaseSpeedPerSecond > kMaximumAdaptationSpeedPerSecond)
    {
        return std::unexpected(ContractError::InvalidAdaptationSpeed);
    }
    return {};
}

[[nodiscard]] std::expected<std::uint32_t, ContractError> QuantizePercentile(double percentile) noexcept
{
    if (!IsFinite(percentile))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (percentile < 0.0 || percentile > 1.0)
    {
        return std::unexpected(ContractError::InvalidPercentile);
    }
    double const scaled = percentile * static_cast<double>(kPercentileOne);
    return static_cast<std::uint32_t>(std::floor(scaled + 0.5));
}

[[nodiscard]] std::expected<void, ContractError> ValidateMeteringSettings(MeteringSettings const &settings) noexcept
{
    if (settings.policy != MeteringPolicy::ArithmeticMean && settings.policy != MeteringPolicy::LogAverage &&
        settings.policy != MeteringPolicy::PercentileWindow)
    {
        return std::unexpected(ContractError::InvalidPolicy);
    }
    if (settings.policy != MeteringPolicy::PercentileWindow)
    {
        return {};
    }
    auto const lower = QuantizePercentile(settings.window.lowerPercentile);
    if (!lower)
    {
        return std::unexpected(lower.error());
    }
    auto const upper = QuantizePercentile(settings.window.upperPercentile);
    if (!upper)
    {
        return std::unexpected(upper.error());
    }
    if (*upper < *lower)
    {
        return std::unexpected(ContractError::InvalidPercentileRange);
    }
    return {};
}

[[nodiscard]] double WeightToReal(std::uint64_t weight) noexcept
{
    return static_cast<double>(weight) / static_cast<double>(kWeightOne);
}

// The mask identity is folded in two pieces so that the standalone MaskIdentity and the recomputation inside the
// histogram build cannot drift apart. One function seeds from the extent, the other folds in one already quantized
// weight, and both callers visit the weights in the same row-major order.
[[nodiscard]] std::uint64_t MaskIdentitySeed(Extent2D extent) noexcept
{
    std::uint64_t const hash = HashUint64(kFnvOffsetBasis, static_cast<std::uint64_t>(extent.width));
    return HashUint64(hash, static_cast<std::uint64_t>(extent.height));
}

[[nodiscard]] std::uint64_t FoldMaskWeight(std::uint64_t hash, std::uint32_t quantizedWeight) noexcept
{
    return HashUint64(hash, static_cast<std::uint64_t>(quantizedWeight));
}

// Adds a weight to a running total that has a declared cap, so that a malformed external histogram is refused
// rather than wrapped into a plausible small number.
[[nodiscard]] bool AddCappedWeight(std::uint64_t &total, std::uint64_t addend) noexcept
{
    if (addend > kMaximumTotalWeight || total > kMaximumTotalWeight - addend)
    {
        return false;
    }
    total += addend;
    return true;
}

[[nodiscard]] bool FitsInUint32(std::uint64_t value) noexcept
{
    return value <= static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
}

// Sums two uint32 counters in 64 bits and reports whether the result still fits the counter it will be stored in.
[[nodiscard]] bool AddCountedSamples(std::uint32_t left, std::uint32_t right, std::uint32_t &result) noexcept
{
    std::uint64_t const sum = static_cast<std::uint64_t>(left) + static_cast<std::uint64_t>(right);
    if (!FitsInUint32(sum))
    {
        return false;
    }
    result = static_cast<std::uint32_t>(sum);
    return true;
}

} // namespace

std::expected<std::uint32_t, ContractError> PixelCount(Extent2D extent) noexcept
{
    auto const extentCheck = ValidateSampleExtent(extent);
    if (!extentCheck)
    {
        return std::unexpected(extentCheck.error());
    }
    return extent.width * extent.height;
}

std::expected<double, ContractError> SceneLinearLuminance(Rgb color) noexcept
{
    if (!IsFinite(color))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (color.r < 0.0 || color.g < 0.0 || color.b < 0.0)
    {
        return std::unexpected(ContractError::NegativeRadiance);
    }
    if (color.r > kMaximumSceneLinearValue || color.g > kMaximumSceneLinearValue || color.b > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::RadianceTooLarge);
    }
    return (kRec709RedWeight * color.r) + (kRec709GreenWeight * color.g) + (kRec709BlueWeight * color.b);
}

std::expected<double, ContractError> AbsoluteLuminance(Rgb preExposedSceneLinear, double preExposure) noexcept
{
    auto const preExposureCheck = ValidatePreExposure(preExposure);
    if (!preExposureCheck)
    {
        return std::unexpected(preExposureCheck.error());
    }
    auto const stored = SceneLinearLuminance(preExposedSceneLinear);
    if (!stored)
    {
        return std::unexpected(stored.error());
    }
    double const absolute = *stored / preExposure;
    if (!IsFinite(absolute))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (absolute > kMaximumLuminance)
    {
        return std::unexpected(ContractError::LuminanceTooLarge);
    }
    return absolute;
}

std::expected<double, ContractError> ExposureStopsFromScale(double exposureScale) noexcept
{
    if (!IsFinite(exposureScale))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (exposureScale <= 0.0 || exposureScale > kMaximumExposureScale)
    {
        return std::unexpected(ContractError::InvalidExposureScale);
    }
    double const stops = std::log2(exposureScale);
    if (!IsFinite(stops) || std::abs(stops) > kMaximumExposureStops)
    {
        return std::unexpected(ContractError::InvalidExposureStops);
    }
    return stops;
}

std::expected<double, ContractError> ExposureScaleFromStops(double exposureStops) noexcept
{
    if (!IsFinite(exposureStops))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (std::abs(exposureStops) > kMaximumExposureStops)
    {
        return std::unexpected(ContractError::InvalidExposureStops);
    }
    double const scale = std::exp2(exposureStops);
    // The stop limit is derived from the scale limit, so this second check never fires for a legal stops value. It
    // is kept because the limit that matters downstream is the scale, and a future edit to either constant must be
    // caught here rather than at the integration seam.
    if (!IsFinite(scale) || scale <= 0.0 || scale > kMaximumExposureScale)
    {
        return std::unexpected(ContractError::InvalidExposureScale);
    }
    return scale;
}

std::expected<std::uint32_t, ContractError> QuantizeUnitWeight(double weight) noexcept
{
    if (!IsFinite(weight))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (weight < 0.0 || weight > 1.0)
    {
        return std::unexpected(ContractError::InvalidWeight);
    }
    double const scaled = (weight * static_cast<double>(kWeightOne)) + 0.5;
    return static_cast<std::uint32_t>(std::floor(scaled));
}

std::expected<std::uint32_t, ContractError> CombineWeights(std::uint32_t left, std::uint32_t right) noexcept
{
    if (left > kWeightOne || right > kWeightOne)
    {
        return std::unexpected(ContractError::InvalidWeight);
    }
    std::uint32_t const product = left * right;
    return (product + (kWeightOne / 2U)) / kWeightOne;
}

std::expected<std::uint32_t, ContractError> CentreWeight(Extent2D extent, PixelCoordinate pixel,
                                                         CentreWeightSettings const &settings) noexcept
{
    auto const extentCheck = ValidateSampleExtent(extent);
    if (!extentCheck)
    {
        return std::unexpected(extentCheck.error());
    }
    if (pixel.x >= extent.width || pixel.y >= extent.height)
    {
        return std::unexpected(ContractError::InvalidPixel);
    }
    auto const settingsCheck = ValidateCentreSettings(settings);
    if (!settingsCheck)
    {
        return std::unexpected(settingsCheck.error());
    }

    double const halfWidth = 0.5 * static_cast<double>(extent.width);
    double const halfHeight = 0.5 * static_cast<double>(extent.height);
    double const dx = (static_cast<double>(pixel.x) + 0.5) - halfWidth;
    double const dy = (static_cast<double>(pixel.y) + 0.5) - halfHeight;
    // The corner *pixel centre*, not the corner of the image, is what normalizes the radius. Using the image
    // corner instead would make the corner pixel's weight depend on the resolution, which is exactly the kind of
    // silent resolution dependence a metering contract must not have.
    double const cornerX = halfWidth - 0.5;
    double const cornerY = halfHeight - 0.5;
    double const cornerDistance = std::hypot(cornerX, cornerY);
    double radius = 0.0;
    if (cornerDistance > 0.0)
    {
        radius = std::hypot(dx, dy) / cornerDistance;
    }
    radius = std::clamp(radius, 0.0, 1.0);

    double const profile = std::pow(1.0 - radius, settings.falloffPower);
    double const weight = settings.edgeWeight + ((settings.centreWeight - settings.edgeWeight) * profile);
    return QuantizeUnitWeight(std::clamp(weight, 0.0, 1.0));
}

std::expected<std::uint64_t, ContractError> MaskIdentity(MaskView mask) noexcept
{
    auto const count = PixelCount(mask.extent);
    if (!count)
    {
        return std::unexpected(count.error());
    }
    if (mask.weights.size() != static_cast<std::size_t>(*count))
    {
        return std::unexpected(ContractError::SizeMismatch);
    }

    std::uint64_t hash = MaskIdentitySeed(mask.extent);
    for (double const value : mask.weights)
    {
        if (!IsFinite(value))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (value < 0.0 || value > 1.0)
        {
            return std::unexpected(ContractError::InvalidMaskValue);
        }
        auto const quantized = QuantizeUnitWeight(value);
        if (!quantized)
        {
            return std::unexpected(quantized.error());
        }
        hash = FoldMaskWeight(hash, *quantized);
    }
    return hash;
}

std::expected<HistogramLayoutFacts, ContractError> ValidateHistogramLayout(HistogramLayout layout) noexcept
{
    if (layout.binCount < kMinimumBinCount || layout.binCount > kMaximumBinCount)
    {
        return std::unexpected(ContractError::InvalidBinCount);
    }
    if (!IsFinite(layout.minimumLog2Luminance) || !IsFinite(layout.maximumLog2Luminance))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (std::abs(layout.minimumLog2Luminance) > kLog2LuminanceLimit ||
        std::abs(layout.maximumLog2Luminance) > kLog2LuminanceLimit)
    {
        return std::unexpected(ContractError::InvalidLog2LuminanceRange);
    }
    double const span = layout.maximumLog2Luminance - layout.minimumLog2Luminance;
    if (span < kMinimumLog2LuminanceSpan || span > kMaximumLog2LuminanceSpan)
    {
        return std::unexpected(ContractError::InvalidLog2LuminanceRange);
    }

    HistogramLayoutFacts facts{};
    facts.binCount = layout.binCount;
    facts.minimumLog2Luminance = layout.minimumLog2Luminance;
    facts.maximumLog2Luminance = layout.maximumLog2Luminance;
    facts.log2LuminanceSpan = span;
    facts.log2LuminancePerBin = span / static_cast<double>(layout.binCount);
    facts.binsPerLog2Luminance = static_cast<double>(layout.binCount) / span;
    facts.minimumLuminance = std::exp2(layout.minimumLog2Luminance);
    facts.maximumLuminance = std::exp2(layout.maximumLog2Luminance);
    std::uint64_t hash = HashUint64(kFnvOffsetBasis, static_cast<std::uint64_t>(layout.binCount));
    hash = HashDouble(hash, layout.minimumLog2Luminance);
    facts.identity = HashDouble(hash, layout.maximumLog2Luminance);
    return facts;
}

std::expected<double, ContractError> BinLowerLog2Luminance(HistogramLayoutFacts const &facts,
                                                           std::uint32_t binEdge) noexcept
{
    auto const factsCheck = ValidateLayoutFacts(facts);
    if (!factsCheck)
    {
        return std::unexpected(factsCheck.error());
    }
    if (binEdge > facts.binCount)
    {
        return std::unexpected(ContractError::InvalidBinIndex);
    }
    return LowerEdge(facts, binEdge);
}

std::expected<double, ContractError> BinCentreLog2Luminance(HistogramLayoutFacts const &facts,
                                                            std::uint32_t bin) noexcept
{
    auto const factsCheck = ValidateLayoutFacts(facts);
    if (!factsCheck)
    {
        return std::unexpected(factsCheck.error());
    }
    if (bin >= facts.binCount)
    {
        return std::unexpected(ContractError::InvalidBinIndex);
    }
    return CentreOf(facts, bin);
}

std::expected<double, ContractError> BinCentreLuminance(HistogramLayoutFacts const &facts, std::uint32_t bin) noexcept
{
    auto const centre = BinCentreLog2Luminance(facts, bin);
    if (!centre)
    {
        return std::unexpected(centre.error());
    }
    return std::exp2(*centre);
}

std::expected<LuminanceBin, ContractError> ClassifyLog2Luminance(HistogramLayoutFacts const &facts,
                                                                 double log2Luminance) noexcept
{
    auto const factsCheck = ValidateLayoutFacts(facts);
    if (!factsCheck)
    {
        return std::unexpected(factsCheck.error());
    }
    if (!IsFinite(log2Luminance))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    LuminanceBin result{};
    result.log2Luminance = log2Luminance;
    result.hasLog2Luminance = true;
    if (log2Luminance < facts.minimumLog2Luminance)
    {
        result.classification = BinClass::BelowRange;
        result.bin = 0U;
        return result;
    }
    if (log2Luminance >= facts.maximumLog2Luminance)
    {
        result.classification = BinClass::AboveRange;
        result.bin = facts.binCount - 1U;
        return result;
    }

    double const scaled = (log2Luminance - facts.minimumLog2Luminance) * facts.binsPerLog2Luminance;
    double const floored = std::floor(scaled);
    std::uint32_t bin = facts.binCount - 1U;
    if (floored >= 0.0 && floored < static_cast<double>(facts.binCount))
    {
        bin = static_cast<std::uint32_t>(floored);
    }
    // The published edges, not the scaled position, define which bin owns a value. Rounding in the multiply above
    // can put a value that sits exactly on an edge one bin to either side, so the candidate is walked back onto the
    // bin whose half-open edge interval actually contains it. The edges are strictly increasing, so this
    // terminates, and afterwards edge(bin) <= log2Luminance < edge(bin + 1) holds exactly.
    while (bin > 0U && log2Luminance < LowerEdge(facts, bin))
    {
        --bin;
    }
    while (bin + 1U < facts.binCount && log2Luminance >= LowerEdge(facts, bin + 1U))
    {
        ++bin;
    }
    result.classification = BinClass::Interior;
    result.bin = bin;
    return result;
}

std::expected<LuminanceBin, ContractError> ClassifyLuminance(HistogramLayoutFacts const &facts,
                                                             double luminance) noexcept
{
    auto const factsCheck = ValidateLayoutFacts(facts);
    if (!factsCheck)
    {
        return std::unexpected(factsCheck.error());
    }
    if (!IsFinite(luminance))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (luminance < 0.0)
    {
        return std::unexpected(ContractError::NegativeLuminance);
    }
    if (luminance > kMaximumLuminance)
    {
        return std::unexpected(ContractError::LuminanceTooLarge);
    }
    if (luminance == 0.0)
    {
        // log2(0) is never evaluated. Black is classified structurally, which is the only way to keep a black sky
        // out of the floating-point domain while still letting a policy decide what to do with it.
        LuminanceBin result{};
        result.classification = BinClass::Zero;
        result.bin = 0U;
        result.log2Luminance = 0.0;
        result.hasLog2Luminance = false;
        return result;
    }
    return ClassifyLog2Luminance(facts, std::log2(luminance));
}

std::expected<HistogramSample, ContractError> ClassifySample(Rgb preExposedSceneLinear, double preExposure,
                                                             std::uint32_t weight,
                                                             HistogramSettings const &settings) noexcept
{
    auto const preExposureCheck = ValidatePreExposure(preExposure);
    if (!preExposureCheck)
    {
        return std::unexpected(preExposureCheck.error());
    }
    if (weight > kWeightOne)
    {
        return std::unexpected(ContractError::InvalidWeight);
    }
    auto const facts = ValidateHistogramLayout(settings.layout);
    if (!facts)
    {
        return std::unexpected(facts.error());
    }

    HistogramSample sample{};
    sample.weight = weight;

    auto const absolute = AbsoluteLuminance(preExposedSceneLinear, preExposure);
    if (!absolute)
    {
        switch (absolute.error())
        {
        case ContractError::NonFinite:
            sample.rejection = SampleRejection::NonFiniteChannel;
            return sample;
        case ContractError::NegativeRadiance:
            sample.rejection = SampleRejection::NegativeChannel;
            return sample;
        default:
            sample.rejection = SampleRejection::LuminanceOutOfDomain;
            return sample;
        }
    }
    sample.absoluteLuminance = *absolute;

    auto const bin = ClassifyLuminance(*facts, *absolute);
    if (!bin)
    {
        return std::unexpected(bin.error());
    }
    sample.classification = bin->classification;
    sample.bin = bin->bin;
    sample.log2Luminance = bin->log2Luminance;
    sample.hasLog2Luminance = bin->hasLog2Luminance;

    if (weight == 0U)
    {
        sample.rejection = SampleRejection::ZeroWeight;
        return sample;
    }

    switch (bin->classification)
    {
    case BinClass::Zero:
        if (settings.blackSamples == BlackSamplePolicy::Reject)
        {
            sample.rejection = SampleRejection::ZeroLuminance;
            return sample;
        }
        break;
    case BinClass::BelowRange:
        if (settings.belowRange == RangeSamplePolicy::Reject)
        {
            sample.rejection = SampleRejection::BelowRange;
            return sample;
        }
        break;
    case BinClass::AboveRange:
        if (settings.aboveRange == RangeSamplePolicy::Reject)
        {
            sample.rejection = SampleRejection::AboveRange;
            return sample;
        }
        break;
    case BinClass::Interior:
        break;
    }

    sample.accepted = true;
    sample.rejection = SampleRejection::None;
    return sample;
}

std::expected<LuminanceHistogram, ContractError> BuildLuminanceHistogram(
    LuminanceSampleView samples, MaskView mask, double preExposure, HistogramBuildSettings const &settings) noexcept
{
    auto const sampleCount = PixelCount(samples.extent);
    if (!sampleCount)
    {
        return std::unexpected(sampleCount.error());
    }
    if (samples.pixels.size() != static_cast<std::size_t>(*sampleCount))
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    auto const preExposureCheck = ValidatePreExposure(preExposure);
    if (!preExposureCheck)
    {
        return std::unexpected(preExposureCheck.error());
    }
    auto const facts = ValidateHistogramLayout(settings.histogram.layout);
    if (!facts)
    {
        return std::unexpected(facts.error());
    }
    if (settings.weighting.useCentreWeighting)
    {
        auto const centreCheck = ValidateCentreSettings(settings.weighting.centre);
        if (!centreCheck)
        {
            return std::unexpected(centreCheck.error());
        }
    }
    if (settings.weighting.useMask)
    {
        if (mask.extent != samples.extent || mask.weights.size() != samples.pixels.size())
        {
            return std::unexpected(ContractError::SizeMismatch);
        }
    }
    else if (!mask.weights.empty() || mask.extent != Extent2D{})
    {
        // A mask that was supplied but not requested is a configuration mistake, not something to ignore quietly.
        return std::unexpected(ContractError::SizeMismatch);
    }

    LuminanceHistogram histogram{};
    histogram.layout = settings.histogram.layout;
    histogram.statistics.minimumObservedLuminance = 0.0;
    histogram.statistics.maximumObservedLuminance = 0.0;

    // Recomputed from the same quantized weights the pass already reads, so proving the supplied mask is the one
    // the configuration identity describes costs one hash fold per pixel and no second traversal.
    std::uint64_t maskHash = settings.weighting.useMask ? MaskIdentitySeed(mask.extent) : 0ULL;

    for (std::uint32_t y = 0U; y < samples.extent.height; ++y)
    {
        for (std::uint32_t x = 0U; x < samples.extent.width; ++x)
        {
            std::size_t const index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(samples.extent.width)) +
                                      static_cast<std::size_t>(x);

            std::uint32_t weight = kWeightOne;
            if (settings.weighting.useCentreWeighting)
            {
                auto const centre = CentreWeight(samples.extent, {.x = x, .y = y}, settings.weighting.centre);
                if (!centre)
                {
                    return std::unexpected(centre.error());
                }
                auto const combined = CombineWeights(weight, *centre);
                if (!combined)
                {
                    return std::unexpected(combined.error());
                }
                weight = *combined;
            }
            if (settings.weighting.useMask)
            {
                double const maskValue = mask.weights[index];
                if (!IsFinite(maskValue))
                {
                    return std::unexpected(ContractError::NonFinite);
                }
                if (maskValue < 0.0 || maskValue > 1.0)
                {
                    return std::unexpected(ContractError::InvalidMaskValue);
                }
                auto const quantized = QuantizeUnitWeight(maskValue);
                if (!quantized)
                {
                    return std::unexpected(quantized.error());
                }
                maskHash = FoldMaskWeight(maskHash, *quantized);
                auto const combined = CombineWeights(weight, *quantized);
                if (!combined)
                {
                    return std::unexpected(combined.error());
                }
                weight = *combined;
            }

            auto const sample = ClassifySample(samples.pixels[index], preExposure, weight, settings.histogram);
            if (!sample)
            {
                return std::unexpected(sample.error());
            }

            HistogramStatistics &statistics = histogram.statistics;
            if (!sample->accepted)
            {
                ++statistics.rejectedSampleCount;
                statistics.rejectedWeight += static_cast<std::uint64_t>(sample->weight);
                ++statistics.rejectedSampleCountsByReason[RejectionIndex(sample->rejection)];
                continue;
            }

            std::uint64_t const weight64 = static_cast<std::uint64_t>(sample->weight);
            if (statistics.acceptedWeight > kMaximumTotalWeight - weight64)
            {
                return std::unexpected(ContractError::WeightOverflow);
            }

            histogram.binWeights[sample->bin] += weight64;
            ++histogram.binSampleCounts[sample->bin];
            ++statistics.acceptedSampleCount;
            statistics.acceptedWeight += weight64;
            statistics.weightedLuminanceSum += WeightToReal(weight64) * sample->absoluteLuminance;
            if (sample->hasLog2Luminance)
            {
                statistics.weightedLog2LuminanceSum += WeightToReal(weight64) * sample->log2Luminance;
                statistics.log2AccumulatedWeight += weight64;
            }
            switch (sample->classification)
            {
            case BinClass::Zero:
                ++statistics.zeroLuminanceSampleCount;
                statistics.zeroLuminanceWeight += weight64;
                break;
            case BinClass::BelowRange:
                ++statistics.belowRangeSampleCount;
                statistics.belowRangeWeight += weight64;
                break;
            case BinClass::AboveRange:
                ++statistics.aboveRangeSampleCount;
                statistics.aboveRangeWeight += weight64;
                break;
            case BinClass::Interior:
                break;
            }
            if (!statistics.hasAcceptedSample)
            {
                statistics.hasAcceptedSample = true;
                statistics.minimumObservedLuminance = sample->absoluteLuminance;
                statistics.maximumObservedLuminance = sample->absoluteLuminance;
            }
            else
            {
                statistics.minimumObservedLuminance =
                    std::min(statistics.minimumObservedLuminance, sample->absoluteLuminance);
                statistics.maximumObservedLuminance =
                    std::max(statistics.maximumObservedLuminance, sample->absoluteLuminance);
            }
        }
    }

    if (settings.weighting.useMask && maskHash != settings.weighting.maskIdentity)
    {
        // The declared identity is what MeteringConfigurationIdentity hashes, so accepting a histogram built from a
        // different mask would let the metering region change without AdaptationReset::ConfigurationChanged ever
        // firing. The frame is refused rather than metered under a stale identity.
        return std::unexpected(ContractError::MaskIdentityMismatch);
    }

    return histogram;
}

std::expected<LuminanceHistogram, ContractError> MergeHistograms(LuminanceHistogram const &left,
                                                                 LuminanceHistogram const &right) noexcept
{
    auto const facts = ValidateHistogramLayout(left.layout);
    if (!facts)
    {
        return std::unexpected(facts.error());
    }
    if (left.layout != right.layout)
    {
        return std::unexpected(ContractError::LayoutMismatch);
    }

    LuminanceHistogram merged{};
    merged.layout = left.layout;
    for (std::uint32_t bin = facts->binCount; bin < kMaximumBinCount; ++bin)
    {
        if (left.binWeights[bin] != 0U || left.binSampleCounts[bin] != 0U || right.binWeights[bin] != 0U ||
            right.binSampleCounts[bin] != 0U)
        {
            // Weight parked beyond the declared bin count is invisible to every estimator, so merging it would
            // launder an inconsistent tile into a histogram that SummarizeHistogram would then accept. The same
            // rule and the same error are used at both ends of the pipeline on purpose.
            return std::unexpected(ContractError::InconsistentHistogram);
        }
    }
    for (std::uint32_t bin = 0U; bin < facts->binCount; ++bin)
    {
        std::uint64_t weight = left.binWeights[bin];
        if (!AddCappedWeight(weight, right.binWeights[bin]))
        {
            return std::unexpected(ContractError::WeightOverflow);
        }
        std::uint32_t count = 0U;
        if (!AddCountedSamples(left.binSampleCounts[bin], right.binSampleCounts[bin], count))
        {
            return std::unexpected(ContractError::WeightOverflow);
        }
        merged.binWeights[bin] = weight;
        merged.binSampleCounts[bin] = count;
    }

    HistogramStatistics &statistics = merged.statistics;
    HistogramStatistics const &a = left.statistics;
    HistogramStatistics const &b = right.statistics;

    // Every counter, including the ones that only a diagnostic overlay reads. A counter that wraps is worse than a
    // counter that is missing, because it reads as a small plausible number.
    if (!AddCountedSamples(a.acceptedSampleCount, b.acceptedSampleCount, statistics.acceptedSampleCount) ||
        !AddCountedSamples(a.rejectedSampleCount, b.rejectedSampleCount, statistics.rejectedSampleCount) ||
        !AddCountedSamples(a.belowRangeSampleCount, b.belowRangeSampleCount, statistics.belowRangeSampleCount) ||
        !AddCountedSamples(a.aboveRangeSampleCount, b.aboveRangeSampleCount, statistics.aboveRangeSampleCount) ||
        !AddCountedSamples(a.zeroLuminanceSampleCount, b.zeroLuminanceSampleCount, statistics.zeroLuminanceSampleCount))
    {
        return std::unexpected(ContractError::WeightOverflow);
    }
    // The aggregate SummarizeHistogram will publish as totalSampleCount is guarded here as well, so a merge cannot
    // hand on a pair of counters that only overflows once they are added together.
    if (!FitsInUint32(static_cast<std::uint64_t>(statistics.acceptedSampleCount) +
                      static_cast<std::uint64_t>(statistics.rejectedSampleCount)))
    {
        return std::unexpected(ContractError::WeightOverflow);
    }
    for (std::size_t reason = 0U; reason < kSampleRejectionCount; ++reason)
    {
        if (!AddCountedSamples(a.rejectedSampleCountsByReason[reason], b.rejectedSampleCountsByReason[reason],
                               statistics.rejectedSampleCountsByReason[reason]))
        {
            return std::unexpected(ContractError::WeightOverflow);
        }
    }

    struct WeightMerge final
    {
        std::uint64_t left{};
        std::uint64_t right{};
        std::uint64_t *destination{};
    };
    std::array<WeightMerge, 6U> const weightMerges{
        WeightMerge{.left = a.acceptedWeight, .right = b.acceptedWeight, .destination = &statistics.acceptedWeight},
        WeightMerge{.left = a.rejectedWeight, .right = b.rejectedWeight, .destination = &statistics.rejectedWeight},
        WeightMerge{
            .left = a.belowRangeWeight, .right = b.belowRangeWeight, .destination = &statistics.belowRangeWeight},
        WeightMerge{
            .left = a.aboveRangeWeight, .right = b.aboveRangeWeight, .destination = &statistics.aboveRangeWeight},
        WeightMerge{.left = a.zeroLuminanceWeight,
                    .right = b.zeroLuminanceWeight,
                    .destination = &statistics.zeroLuminanceWeight},
        WeightMerge{.left = a.log2AccumulatedWeight,
                    .right = b.log2AccumulatedWeight,
                    .destination = &statistics.log2AccumulatedWeight}};
    for (WeightMerge const &merge : weightMerges)
    {
        std::uint64_t total = 0U;
        if (!AddCappedWeight(total, merge.left) || !AddCappedWeight(total, merge.right))
        {
            return std::unexpected(ContractError::WeightOverflow);
        }
        *merge.destination = total;
    }

    statistics.weightedLuminanceSum = a.weightedLuminanceSum + b.weightedLuminanceSum;
    statistics.weightedLog2LuminanceSum = a.weightedLog2LuminanceSum + b.weightedLog2LuminanceSum;
    statistics.hasAcceptedSample = a.hasAcceptedSample || b.hasAcceptedSample;
    if (a.hasAcceptedSample && b.hasAcceptedSample)
    {
        statistics.minimumObservedLuminance = std::min(a.minimumObservedLuminance, b.minimumObservedLuminance);
        statistics.maximumObservedLuminance = std::max(a.maximumObservedLuminance, b.maximumObservedLuminance);
    }
    else if (a.hasAcceptedSample)
    {
        statistics.minimumObservedLuminance = a.minimumObservedLuminance;
        statistics.maximumObservedLuminance = a.maximumObservedLuminance;
    }
    else if (b.hasAcceptedSample)
    {
        statistics.minimumObservedLuminance = b.minimumObservedLuminance;
        statistics.maximumObservedLuminance = b.maximumObservedLuminance;
    }
    return merged;
}

std::expected<HistogramDiagnostics, ContractError> SummarizeHistogram(LuminanceHistogram const &histogram) noexcept
{
    auto const facts = ValidateHistogramLayout(histogram.layout);
    if (!facts)
    {
        return std::unexpected(facts.error());
    }

    HistogramDiagnostics diagnostics{};
    diagnostics.binCount = facts->binCount;

    // Step 2 of the declared precedence: bins outside the declared count are structural nonsense and are decided
    // before anything is accumulated, so the answer does not depend on how large the in-range weights happen to be.
    for (std::uint32_t bin = facts->binCount; bin < kMaximumBinCount; ++bin)
    {
        if (histogram.binWeights[bin] != 0U || histogram.binSampleCounts[bin] != 0U)
        {
            // Weight parked beyond the declared bin count would be invisible to every estimator, so a histogram
            // carrying any is rejected rather than metered as if the extra light were not there.
            return std::unexpected(ContractError::InconsistentHistogram);
        }
    }

    // Step 3: every accumulator against its declared cap. The bin sum is guarded term by term because a forged or
    // corrupted readback can carry values near the top of a uint64, and an unguarded sum of those wraps into a
    // small plausible total that the percentile multiply below would then use.
    std::uint64_t totalWeight = 0U;
    std::uint64_t totalBinSamples = 0U;
    for (std::uint32_t bin = 0U; bin < facts->binCount; ++bin)
    {
        if (!AddCappedWeight(totalWeight, histogram.binWeights[bin]))
        {
            return std::unexpected(ContractError::WeightOverflow);
        }
        totalBinSamples += static_cast<std::uint64_t>(histogram.binSampleCounts[bin]);
        if (histogram.binSampleCounts[bin] == 0U)
        {
            continue;
        }
        if (!diagnostics.hasOccupiedBin)
        {
            diagnostics.hasOccupiedBin = true;
            diagnostics.lowestOccupiedBin = bin;
        }
        diagnostics.highestOccupiedBin = bin;
        ++diagnostics.occupiedBinCount;
    }
    if (!FitsInUint32(totalBinSamples))
    {
        return std::unexpected(ContractError::WeightOverflow);
    }
    for (std::uint64_t const weight :
         {histogram.statistics.acceptedWeight, histogram.statistics.rejectedWeight,
          histogram.statistics.belowRangeWeight, histogram.statistics.aboveRangeWeight,
          histogram.statistics.zeroLuminanceWeight, histogram.statistics.log2AccumulatedWeight})
    {
        if (weight > kMaximumTotalWeight)
        {
            return std::unexpected(ContractError::WeightOverflow);
        }
    }
    // The published total sample count is a uint32, and accepted plus rejected can leave it even when neither
    // counter does on its own. Two tiles of three billion samples each is the case a plausible tile decomposition
    // would reach first.
    std::uint32_t totalSampleCount = 0U;
    if (!AddCountedSamples(histogram.statistics.acceptedSampleCount, histogram.statistics.rejectedSampleCount,
                           totalSampleCount))
    {
        return std::unexpected(ContractError::WeightOverflow);
    }

    // Step 4: the statistics must agree with the bins and with each other.
    if (totalWeight != histogram.statistics.acceptedWeight ||
        totalBinSamples != static_cast<std::uint64_t>(histogram.statistics.acceptedSampleCount))
    {
        return std::unexpected(ContractError::InconsistentHistogram);
    }
    if (histogram.statistics.log2AccumulatedWeight + histogram.statistics.zeroLuminanceWeight !=
        histogram.statistics.acceptedWeight)
    {
        return std::unexpected(ContractError::InconsistentHistogram);
    }
    // Black, below-range and above-range are three disjoint classifications of *accepted* samples, so their
    // counters can never together exceed the accepted totals. The caps checked above make both sums safe to form.
    if (histogram.statistics.belowRangeWeight + histogram.statistics.aboveRangeWeight +
            histogram.statistics.zeroLuminanceWeight >
        histogram.statistics.acceptedWeight)
    {
        return std::unexpected(ContractError::InconsistentHistogram);
    }
    if (static_cast<std::uint64_t>(histogram.statistics.belowRangeSampleCount) +
            static_cast<std::uint64_t>(histogram.statistics.aboveRangeSampleCount) +
            static_cast<std::uint64_t>(histogram.statistics.zeroLuminanceSampleCount) >
        static_cast<std::uint64_t>(histogram.statistics.acceptedSampleCount))
    {
        return std::unexpected(ContractError::InconsistentHistogram);
    }

    diagnostics.totalWeight = totalWeight;
    diagnostics.acceptedSampleCount = histogram.statistics.acceptedSampleCount;
    diagnostics.rejectedSampleCount = histogram.statistics.rejectedSampleCount;
    diagnostics.totalSampleCount = totalSampleCount;
    diagnostics.acceptedWeight = histogram.statistics.acceptedWeight;
    diagnostics.rejectedWeight = histogram.statistics.rejectedWeight;
    diagnostics.rejectedSampleCountsByReason = histogram.statistics.rejectedSampleCountsByReason;
    diagnostics.belowRangeSampleCount = histogram.statistics.belowRangeSampleCount;
    diagnostics.aboveRangeSampleCount = histogram.statistics.aboveRangeSampleCount;
    diagnostics.zeroLuminanceSampleCount = histogram.statistics.zeroLuminanceSampleCount;
    diagnostics.belowRangeWeight = histogram.statistics.belowRangeWeight;
    diagnostics.aboveRangeWeight = histogram.statistics.aboveRangeWeight;
    diagnostics.zeroLuminanceWeight = histogram.statistics.zeroLuminanceWeight;
    diagnostics.minimumObservedLuminance = histogram.statistics.minimumObservedLuminance;
    diagnostics.maximumObservedLuminance = histogram.statistics.maximumObservedLuminance;
    diagnostics.hasAcceptedSample = histogram.statistics.hasAcceptedSample;
    diagnostics.clippedIntoLowestBin = histogram.statistics.belowRangeWeight > 0U;
    diagnostics.clippedIntoHighestBin = histogram.statistics.aboveRangeWeight > 0U;
    return diagnostics;
}

namespace
{

struct PercentileSelection final
{
    std::uint32_t firstBin{};
    std::uint32_t lastBin{};
    std::uint64_t windowWeight{};
    std::uint64_t lowerTargetWeight{};
    std::uint64_t upperTargetWeight{};
    double log2Luminance{};
    bool degenerate{};
};

// Percentile boundaries are exact integer positions in the cumulative weight, so a boundary that falls precisely on
// a bin edge resolves the same way on every machine. Working in doubles here would make the selected bin depend on
// the last bit of a division.
[[nodiscard]] std::expected<PercentileSelection, ContractError> SelectPercentileWindow(
    LuminanceHistogram const &histogram, HistogramLayoutFacts const &facts, PercentileWindow const &window,
    std::uint64_t totalWeight) noexcept
{
    auto const lowerQuantized = QuantizePercentile(window.lowerPercentile);
    if (!lowerQuantized)
    {
        return std::unexpected(lowerQuantized.error());
    }
    auto const upperQuantized = QuantizePercentile(window.upperPercentile);
    if (!upperQuantized)
    {
        return std::unexpected(upperQuantized.error());
    }
    if (*upperQuantized < *lowerQuantized)
    {
        return std::unexpected(ContractError::InvalidPercentileRange);
    }
    if (totalWeight == 0U)
    {
        return std::unexpected(ContractError::NoAcceptedSamples);
    }
    // The one multiply in this file that can leave 64 bits. SummarizeHistogram has already refused anything above
    // the cap, so this is a restatement of that invariant at the site that depends on it rather than a second
    // policy; without it a total weight of 2^46 would wrap and select a window that looks entirely reasonable.
    if (totalWeight > kMaximumTotalWeight)
    {
        return std::unexpected(ContractError::WeightOverflow);
    }

    PercentileSelection selection{};
    selection.lowerTargetWeight =
        (totalWeight * static_cast<std::uint64_t>(*lowerQuantized)) / static_cast<std::uint64_t>(kPercentileOne);
    selection.upperTargetWeight =
        (totalWeight * static_cast<std::uint64_t>(*upperQuantized)) / static_cast<std::uint64_t>(kPercentileOne);

    if (selection.upperTargetWeight == selection.lowerTargetWeight)
    {
        std::uint64_t const position = std::min(selection.lowerTargetWeight, totalWeight - 1U);
        std::uint64_t cumulative = 0U;
        for (std::uint32_t bin = 0U; bin < facts.binCount; ++bin)
        {
            cumulative += histogram.binWeights[bin];
            if (cumulative > position)
            {
                selection.firstBin = bin;
                selection.lastBin = bin;
                selection.windowWeight = histogram.binWeights[bin];
                selection.log2Luminance = CentreOf(facts, bin);
                selection.degenerate = true;
                return selection;
            }
        }
        return std::unexpected(ContractError::NoAcceptedSamples);
    }

    double weightedLog2Sum = 0.0;
    std::uint64_t cumulative = 0U;
    bool hasFirst = false;
    for (std::uint32_t bin = 0U; bin < facts.binCount; ++bin)
    {
        std::uint64_t const binStart = cumulative;
        cumulative += histogram.binWeights[bin];
        std::uint64_t const overlapLow = std::max(binStart, selection.lowerTargetWeight);
        std::uint64_t const overlapHigh = std::min(cumulative, selection.upperTargetWeight);
        if (overlapHigh <= overlapLow)
        {
            continue;
        }
        std::uint64_t const overlap = overlapHigh - overlapLow;
        if (!hasFirst)
        {
            hasFirst = true;
            selection.firstBin = bin;
        }
        selection.lastBin = bin;
        selection.windowWeight += overlap;
        weightedLog2Sum += static_cast<double>(overlap) * CentreOf(facts, bin);
    }
    if (!hasFirst || selection.windowWeight == 0U)
    {
        return std::unexpected(ContractError::NoAcceptedSamples);
    }
    selection.log2Luminance = weightedLog2Sum / static_cast<double>(selection.windowWeight);
    return selection;
}

} // namespace

std::expected<MeteringResult, ContractError> MeterHistogram(LuminanceHistogram const &histogram,
                                                            MeteringSettings const &settings) noexcept
{
    auto const settingsCheck = ValidateMeteringSettings(settings);
    if (!settingsCheck)
    {
        return std::unexpected(settingsCheck.error());
    }
    auto const facts = ValidateHistogramLayout(histogram.layout);
    if (!facts)
    {
        return std::unexpected(facts.error());
    }
    auto const diagnostics = SummarizeHistogram(histogram);
    if (!diagnostics)
    {
        return std::unexpected(diagnostics.error());
    }
    if (!diagnostics->hasAcceptedSample || diagnostics->acceptedWeight == 0U)
    {
        return std::unexpected(ContractError::NoAcceptedSamples);
    }
    // No accepted sample carried a positive luminance, which under the default CountInLowestBin policy is exactly
    // what an all-black frame looks like: accepted samples, accepted weight, and an occupied lowest bin. Every
    // policy refuses it here, before any estimator has a chance to fabricate the lowest bin's centre luminance as
    // though a measurement had happened. The test is exact and takes no logarithm of zero.
    if (histogram.statistics.log2AccumulatedWeight == 0U)
    {
        return std::unexpected(ContractError::NoPositiveLuminance);
    }

    MeteringResult result{};
    result.policy = settings.policy;
    result.diagnostics = *diagnostics;
    result.binQuantizationLog2Bound = 0.5 * facts->log2LuminancePerBin;
    // Every accepted sample lies within half a bin of its recorded bin centre *unless* it was black, or unless it
    // was saturated into an end bin. Either fact breaks the bound, so it is published as invalid rather than
    // quietly quoted anyway.
    result.binCentreEstimateBounded = histogram.statistics.zeroLuminanceWeight == 0U &&
                                      histogram.statistics.belowRangeWeight == 0U &&
                                      histogram.statistics.aboveRangeWeight == 0U;
    result.logAverageWeight = histogram.statistics.log2AccumulatedWeight;
    result.excludedZeroLuminanceFromLogAverage = histogram.statistics.zeroLuminanceWeight > 0U;

    double const totalRealWeight = WeightToReal(diagnostics->acceptedWeight);

    if (settings.policy == MeteringPolicy::PercentileWindow)
    {
        auto const selection = SelectPercentileWindow(histogram, *facts, settings.window, diagnostics->acceptedWeight);
        if (!selection)
        {
            return std::unexpected(selection.error());
        }
        result.usesExactSufficientStatistic = false;
        result.meteredLog2Luminance = selection->log2Luminance;
        result.meteredLuminance = std::exp2(selection->log2Luminance);
        result.binCentreLog2Luminance = selection->log2Luminance;
        result.firstWindowBin = selection->firstBin;
        result.lastWindowBin = selection->lastBin;
        result.windowWeight = selection->windowWeight;
        result.lowerTargetWeight = selection->lowerTargetWeight;
        result.upperTargetWeight = selection->upperTargetWeight;
        result.degenerateWindow = selection->degenerate;
        result.windowLowerLog2Luminance = LowerEdge(*facts, selection->firstBin);
        result.windowUpperLog2Luminance = LowerEdge(*facts, selection->lastBin + 1U);
        if (!IsFinite(result.meteredLuminance) || result.meteredLuminance <= 0.0)
        {
            return std::unexpected(ContractError::NonPositiveLuminance);
        }
        return result;
    }

    result.usesExactSufficientStatistic = true;
    if (settings.policy == MeteringPolicy::ArithmeticMean)
    {
        double binCentreSum = 0.0;
        for (std::uint32_t bin = 0U; bin < facts->binCount; ++bin)
        {
            if (histogram.binWeights[bin] == 0U)
            {
                continue;
            }
            binCentreSum += WeightToReal(histogram.binWeights[bin]) * std::exp2(CentreOf(*facts, bin));
        }
        double const binCentreMean = binCentreSum / totalRealWeight;
        result.binCentreLog2Luminance = (binCentreMean > 0.0) ? std::log2(binCentreMean) : 0.0;

        double const mean = histogram.statistics.weightedLuminanceSum / totalRealWeight;
        if (!IsFinite(mean))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (mean <= 0.0)
        {
            // Every accepted sample was black. There is no exposure that makes a black frame middle grey, so the
            // caller is told there is nothing to meter instead of being handed the darkest representable value.
            return std::unexpected(ContractError::NonPositiveLuminance);
        }
        result.meteredLuminance = mean;
        result.meteredLog2Luminance = std::log2(mean);
        return result;
    }

    if (histogram.statistics.log2AccumulatedWeight == 0U)
    {
        // Unreachable, because the same condition is refused for every policy above. It is kept as a local
        // precondition for the divide below, so that moving the hoisted check later would produce this error rather
        // than a division by zero.
        return std::unexpected(ContractError::NoPositiveLuminance);
    }
    double binCentreLog2Sum = 0.0;
    for (std::uint32_t bin = 0U; bin < facts->binCount; ++bin)
    {
        if (histogram.binWeights[bin] == 0U)
        {
            continue;
        }
        binCentreLog2Sum += WeightToReal(histogram.binWeights[bin]) * CentreOf(*facts, bin);
    }
    result.binCentreLog2Luminance = binCentreLog2Sum / totalRealWeight;

    double const logAverage =
        histogram.statistics.weightedLog2LuminanceSum / WeightToReal(histogram.statistics.log2AccumulatedWeight);
    if (!IsFinite(logAverage))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    result.meteredLog2Luminance = logAverage;
    result.meteredLuminance = std::exp2(logAverage);
    if (!IsFinite(result.meteredLuminance) || result.meteredLuminance <= 0.0)
    {
        return std::unexpected(ContractError::NonPositiveLuminance);
    }
    return result;
}

std::expected<MeasuredTargetExposure, ContractError> ComputeTargetExposure(
    double meteredLuminance, TargetExposureSettings const &settings) noexcept
{
    auto const settingsCheck = ValidateTargetSettings(settings);
    if (!settingsCheck)
    {
        return std::unexpected(settingsCheck.error());
    }
    if (!IsFinite(meteredLuminance))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (meteredLuminance < 0.0)
    {
        return std::unexpected(ContractError::NegativeLuminance);
    }
    if (meteredLuminance == 0.0)
    {
        return std::unexpected(ContractError::NonPositiveLuminance);
    }
    if (meteredLuminance > kMaximumLuminance)
    {
        return std::unexpected(ContractError::LuminanceTooLarge);
    }

    // The difference of logarithms, not the logarithm of the ratio: middleGrey / meteredLuminance can underflow or
    // overflow for a legitimate metered value, and its logarithm would then be infinite for a configuration that is
    // perfectly representable in stops.
    double const unclamped = (std::log2(settings.calibration.middleGreyLuminance) - std::log2(meteredLuminance)) +
                             settings.exposureCompensationStops;
    if (!IsFinite(unclamped))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    MeasuredTargetExposure target{};
    target.meteredLuminance = meteredLuminance;
    target.unclampedStops = unclamped;
    target.stops = std::clamp(unclamped, settings.minimumExposureStops, settings.maximumExposureStops);
    target.clampedToMinimum = unclamped < settings.minimumExposureStops;
    target.clampedToMaximum = unclamped > settings.maximumExposureStops;
    auto const scale = ExposureScaleFromStops(target.stops);
    if (!scale)
    {
        return std::unexpected(scale.error());
    }
    target.scale = *scale;
    target.exposedMeteredLuminance = meteredLuminance * *scale;
    if (!IsFinite(target.exposedMeteredLuminance))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return target;
}

std::expected<double, ContractError> SmoothingAlpha(double speedPerSecond, double deltaSeconds) noexcept
{
    if (!IsFinite(speedPerSecond) || !IsFinite(deltaSeconds))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (speedPerSecond < 0.0 || speedPerSecond > kMaximumAdaptationSpeedPerSecond)
    {
        return std::unexpected(ContractError::InvalidAdaptationSpeed);
    }
    if (deltaSeconds < 0.0 || deltaSeconds > kMaximumFrameDeltaSeconds)
    {
        return std::unexpected(ContractError::InvalidFrameDelta);
    }
    // -expm1(-x) is 1 - exp(-x) evaluated without the cancellation that destroys the first significant digits for
    // the small products a 240 Hz frame produces.
    double const alpha = -std::expm1(-(speedPerSecond * deltaSeconds));
    return std::clamp(alpha, 0.0, 1.0);
}

std::expected<std::uint64_t, ContractError> MeteringConfigurationIdentity(
    MeteringConfiguration const &configuration) noexcept
{
    auto const facts = ValidateHistogramLayout(configuration.histogram.layout);
    if (!facts)
    {
        return std::unexpected(facts.error());
    }
    auto const meteringCheck = ValidateMeteringSettings(configuration.metering);
    if (!meteringCheck)
    {
        return std::unexpected(meteringCheck.error());
    }
    if (configuration.weighting.useCentreWeighting)
    {
        auto const centreCheck = ValidateCentreSettings(configuration.weighting.centre);
        if (!centreCheck)
        {
            return std::unexpected(centreCheck.error());
        }
    }
    if (configuration.mode != ExposureMode::Automatic && configuration.mode != ExposureMode::Manual)
    {
        return std::unexpected(ContractError::InvalidPolicy);
    }

    std::uint64_t hash = HashUint64(kFnvOffsetBasis, facts->identity);
    hash = HashEnum(hash, static_cast<std::uint8_t>(configuration.histogram.blackSamples));
    hash = HashEnum(hash, static_cast<std::uint8_t>(configuration.histogram.belowRange));
    hash = HashEnum(hash, static_cast<std::uint8_t>(configuration.histogram.aboveRange));
    hash = HashBool(hash, configuration.weighting.useCentreWeighting);
    // Centre profile values only enter the identity when centre weighting is on. Tweaking a disabled profile
    // changes no measurement, and resetting adaptation for it would be a visible flicker caused by nothing.
    if (configuration.weighting.useCentreWeighting)
    {
        hash = HashDouble(hash, configuration.weighting.centre.centreWeight);
        hash = HashDouble(hash, configuration.weighting.centre.edgeWeight);
        hash = HashDouble(hash, configuration.weighting.centre.falloffPower);
    }
    hash = HashBool(hash, configuration.weighting.useMask);
    if (configuration.weighting.useMask)
    {
        hash = HashUint64(hash, configuration.weighting.maskIdentity);
    }
    hash = HashEnum(hash, static_cast<std::uint8_t>(configuration.metering.policy));
    // Likewise the percentile window is only part of the identity for the policy that reads it.
    if (configuration.metering.policy == MeteringPolicy::PercentileWindow)
    {
        hash = HashDouble(hash, configuration.metering.window.lowerPercentile);
        hash = HashDouble(hash, configuration.metering.window.upperPercentile);
    }
    return HashEnum(hash, static_cast<std::uint8_t>(configuration.mode));
}

std::expected<ExposureFrameFacts, ContractError> ValidateExposureFrame(ExposureFrame const &frame) noexcept
{
    auto const preExposureCheck = ValidatePreExposure(frame.preExposure);
    if (!preExposureCheck)
    {
        return std::unexpected(preExposureCheck.error());
    }
    // Every reuse decision asks whether this frame follows the one that produced the stored state. A frame index at
    // the end of the range has no successor, so the sequence would wrap and a stale state would look current.
    if (frame.frameIndex == std::numeric_limits<std::uint64_t>::max())
    {
        return std::unexpected(ContractError::InvalidFrameIndex);
    }
    if (!IsFinite(frame.frameDeltaSeconds))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (frame.frameDeltaSeconds < 0.0 || frame.frameDeltaSeconds > kMaximumFrameDeltaSeconds)
    {
        return std::unexpected(ContractError::InvalidFrameDelta);
    }

    ExposureFrameFacts facts{};
    facts.frameIndex = frame.frameIndex;
    facts.preExposure = frame.preExposure;
    facts.inversePreExposure = 1.0 / frame.preExposure;
    facts.frameDeltaSeconds = frame.frameDeltaSeconds;
    facts.isFirstFrame = frame.frameIndex == 0U;
    facts.cameraCut = frame.cameraCut;
    if (!IsFinite(facts.inversePreExposure))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return facts;
}

std::expected<ExposureUpdate, ContractError> UpdateAutoExposure(ExposureUpdateInput const &input,
                                                                AutoExposureSettings const &settings) noexcept
{
    auto const frameFacts = ValidateExposureFrame(input.frame);
    if (!frameFacts)
    {
        return std::unexpected(frameFacts.error());
    }
    auto const identity = MeteringConfigurationIdentity(settings.configuration);
    if (!identity)
    {
        return std::unexpected(identity.error());
    }
    auto const targetCheck = ValidateTargetSettings(settings.target);
    if (!targetCheck)
    {
        return std::unexpected(targetCheck.error());
    }
    auto const adaptationCheck = ValidateAdaptationSettings(settings.adaptation);
    if (!adaptationCheck)
    {
        return std::unexpected(adaptationCheck.error());
    }

    MeasuredTargetExposure target{};
    if (settings.configuration.mode == ExposureMode::Manual)
    {
        if (!IsFinite(settings.manualExposureStops))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (std::abs(settings.manualExposureStops) > kMaximumExposureStops)
        {
            return std::unexpected(ContractError::InvalidExposureStops);
        }
        target.unclampedStops = settings.manualExposureStops;
        target.stops = std::clamp(settings.manualExposureStops, settings.target.minimumExposureStops,
                                  settings.target.maximumExposureStops);
        target.clampedToMinimum = settings.manualExposureStops < settings.target.minimumExposureStops;
        target.clampedToMaximum = settings.manualExposureStops > settings.target.maximumExposureStops;
        auto const scale = ExposureScaleFromStops(target.stops);
        if (!scale)
        {
            return std::unexpected(scale.error());
        }
        target.scale = *scale;
    }
    else
    {
        auto const measured = ComputeTargetExposure(input.meteredLuminance, settings.target);
        if (!measured)
        {
            return std::unexpected(measured.error());
        }
        target = *measured;
    }

    AdaptationReset resets = AdaptationReset::None;
    if (!input.history.valid)
    {
        resets |= AdaptationReset::NoHistory;
    }
    if (frameFacts->isFirstFrame)
    {
        resets |= AdaptationReset::FirstFrame;
    }
    if (frameFacts->cameraCut)
    {
        resets |= AdaptationReset::CameraCut;
    }
    if (input.history.valid)
    {
        bool const sequential =
            input.frame.frameIndex != 0U && (input.frame.frameIndex - 1U) == input.history.producerFrameIndex;
        if (!sequential)
        {
            resets |= AdaptationReset::NonSequentialProducerFrame;
        }
        if (input.history.configurationIdentity != *identity)
        {
            resets |= AdaptationReset::ConfigurationChanged;
        }
        if (input.history.mode != settings.configuration.mode)
        {
            resets |= AdaptationReset::ExposureModeChanged;
        }
        if (!IsFinite(input.history.committedStops) || std::abs(input.history.committedStops) > kMaximumExposureStops)
        {
            resets |= AdaptationReset::InvalidHistoryValue;
        }
    }

    bool const reset = resets != AdaptationReset::None;
    double const currentStops = reset ? target.stops : input.history.committedStops;

    AdaptationDirection direction = AdaptationDirection::Steady;
    if (target.stops > currentStops)
    {
        direction = AdaptationDirection::ExposureIncreasing;
    }
    else if (target.stops < currentStops)
    {
        direction = AdaptationDirection::ExposureDecreasing;
    }
    double const speed = (direction == AdaptationDirection::ExposureDecreasing)
                             ? settings.adaptation.exposureDecreaseSpeedPerSecond
                             : settings.adaptation.exposureIncreaseSpeedPerSecond;

    double alpha = 1.0;
    if (!reset)
    {
        auto const smoothed = SmoothingAlpha(speed, frameFacts->frameDeltaSeconds);
        if (!smoothed)
        {
            return std::unexpected(smoothed.error());
        }
        alpha = *smoothed;
    }

    // Adaptation happens in stops, so moving one stop takes the same time whether the scene is a candle or a
    // desert. Smoothing the linear scale instead would make dark-to-bright and bright-to-dark transitions take
    // wildly different times for the same perceptual distance.
    double const blended = currentStops + (alpha * (target.stops - currentStops));
    if (!IsFinite(blended))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    double const committedStops =
        std::clamp(blended, settings.target.minimumExposureStops, settings.target.maximumExposureStops);

    auto const committedScale = ExposureScaleFromStops(committedStops);
    if (!committedScale)
    {
        return std::unexpected(committedScale.error());
    }
    auto const displayedScale = ExposureScaleFromStops(currentStops);
    if (!displayedScale)
    {
        return std::unexpected(displayedScale.error());
    }

    ExposureUpdate update{};
    update.displayed = {.scale = *displayedScale, .stops = currentStops};
    update.target = target;
    update.committed = {.scale = *committedScale, .stops = committedStops};
    update.nextState = {.producerFrameIndex = input.frame.frameIndex,
                        .committedStops = committedStops,
                        .configurationIdentity = *identity,
                        .mode = settings.configuration.mode,
                        .valid = true};
    update.resets = resets;
    update.alpha = alpha;
    update.direction = direction;
    update.currentStops = currentStops;
    update.targetStops = target.stops;
    update.committedStops = committedStops;
    update.reusedHistory = !reset;
    update.clampedToMinimum = blended < settings.target.minimumExposureStops;
    update.clampedToMaximum = blended > settings.target.maximumExposureStops;
    return update;
}

std::expected<NextFramePreExposure, ContractError> ComputeNextFramePreExposure(
    CommittedExposure committed, double currentPreExposure, PreExposureSettings const &settings) noexcept
{
    if (!IsFinite(committed.scale) || !IsFinite(committed.stops))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (committed.scale <= 0.0 || committed.scale > kMaximumExposureScale)
    {
        // A committed exposure outside the seam is not something this chapter can produce, so it is refused rather
        // than clamped into a pre-exposure that would look like a deliberate bound.
        return std::unexpected(ContractError::InvalidExposureScale);
    }
    auto const currentCheck = ValidatePreExposure(currentPreExposure);
    if (!currentCheck)
    {
        return std::unexpected(currentCheck.error());
    }
    if (!IsFinite(settings.fixedPreExposure) || !IsFinite(settings.minimumPreExposure) ||
        !IsFinite(settings.maximumPreExposure))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.minimumPreExposure < kMinimumPreExposure || settings.maximumPreExposure > kMaximumPreExposure ||
        settings.minimumPreExposure > settings.maximumPreExposure)
    {
        return std::unexpected(ContractError::InvalidPreExposureBounds);
    }
    if (settings.mode != PreExposureMode::MatchCommittedExposure && settings.mode != PreExposureMode::Fixed)
    {
        return std::unexpected(ContractError::InvalidPolicy);
    }
    if (settings.mode == PreExposureMode::Fixed &&
        (settings.fixedPreExposure < kMinimumPreExposure || settings.fixedPreExposure > kMaximumPreExposure))
    {
        return std::unexpected(ContractError::InvalidPreExposure);
    }

    double const requested = (settings.mode == PreExposureMode::Fixed) ? settings.fixedPreExposure : committed.scale;
    NextFramePreExposure result{};
    result.preExposure = std::clamp(requested, settings.minimumPreExposure, settings.maximumPreExposure);
    result.clampedToMinimum = requested < settings.minimumPreExposure;
    result.clampedToMaximum = requested > settings.maximumPreExposure;
    result.previousToNextScale = result.preExposure / currentPreExposure;
    if (!IsFinite(result.previousToNextScale) || result.previousToNextScale <= 0.0)
    {
        return std::unexpected(ContractError::InvalidPreExposure);
    }
    return result;
}

std::expected<FinalExposureResult, ContractError> ApplyDisplayExposure(FinalExposureInput const &input) noexcept
{
    auto const preExposureCheck = ValidatePreExposure(input.preExposure);
    if (!preExposureCheck)
    {
        return std::unexpected(preExposureCheck.error());
    }
    if (!IsFinite(input.exposure.scale) || !IsFinite(input.exposure.stops))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (input.exposure.scale <= 0.0 || input.exposure.scale > kMaximumExposureScale)
    {
        return std::unexpected(ContractError::InvalidExposureScale);
    }
    // Validating the stored colour through the same luminance contract keeps a negative or oversized channel from
    // reaching the tone curve, and costs nothing because the check is the one metering already performs.
    auto const storedLuminance = SceneLinearLuminance(input.preExposedSceneLinear);
    if (!storedLuminance)
    {
        return std::unexpected(storedLuminance.error());
    }

    double const netScale = input.exposure.scale / input.preExposure;
    if (!IsFinite(netScale) || netScale <= 0.0)
    {
        return std::unexpected(ContractError::InvalidExposureScale);
    }

    FinalExposureResult result{};
    result.absoluteSceneLinear = {.r = input.preExposedSceneLinear.r / input.preExposure,
                                  .g = input.preExposedSceneLinear.g / input.preExposure,
                                  .b = input.preExposedSceneLinear.b / input.preExposure};
    result.exposedSceneLinear = {.r = result.absoluteSceneLinear.r * input.exposure.scale,
                                 .g = result.absoluteSceneLinear.g * input.exposure.scale,
                                 .b = result.absoluteSceneLinear.b * input.exposure.scale};
    result.appliedExposureScale = input.exposure.scale;
    result.netScaleFromStored = netScale;
    if (!IsFinite(result.absoluteSceneLinear) || !IsFinite(result.exposedSceneLinear))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (result.absoluteSceneLinear.r > kMaximumSceneLinearValue ||
        result.absoluteSceneLinear.g > kMaximumSceneLinearValue ||
        result.absoluteSceneLinear.b > kMaximumSceneLinearValue)
    {
        // Chapter 30's ApplyExposure validates the radiance it recovers as well as the radiance it exposes, so a
        // stored sample and a pre-exposure that are each legal here but recover an out-of-domain absolute radiance
        // are reported at this seam rather than at the next one.
        return std::unexpected(ContractError::RadianceTooLarge);
    }
    if (result.exposedSceneLinear.r > kMaximumSceneLinearValue ||
        result.exposedSceneLinear.g > kMaximumSceneLinearValue ||
        result.exposedSceneLinear.b > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::RadianceTooLarge);
    }
    return result;
}

} // namespace ch31::auto_exposure
