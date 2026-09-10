#include "TransparencyContracts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ch32::transparency
{
namespace
{

constexpr double kRec709RedWeight = 0.2126;
constexpr double kRec709GreenWeight = 0.7152;
constexpr double kRec709BlueWeight = 0.0722;

constexpr double kMinimumOitDepthScaleMetres = 1.0e-3;
constexpr double kOitDepthWeightNumerator = 10.0;
constexpr double kOitDepthWeightBias = 1.0e-5;

constexpr std::uint64_t kGoldenGammaOdd = 0x9E37'79B9'7F4A'7C15ULL;
constexpr std::uint64_t kMixMultiplierA = 0xBF58'476D'1CE4'E5B9ULL;
constexpr std::uint64_t kMixMultiplierB = 0x94D0'49BB'1331'11EBULL;
constexpr std::uint64_t kPixelYSalt = 0xD119'5B27'4B3C'8A11ULL;
constexpr std::uint64_t kSampleSalt = 0xC2B2'AE3D'27D4'EB4FULL;
constexpr std::uint64_t kFrameSalt = 0x1656'67B1'9E37'79F9ULL;

constexpr std::uint32_t kFrameStageCount = 8U;

[[nodiscard]] bool IsFinite(double value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool IsFinite(Rgb value) noexcept
{
    return IsFinite(value.r) && IsFinite(value.g) && IsFinite(value.b);
}

[[nodiscard]] std::expected<void, ContractError> ValidateUnitValue(double value, ContractError rangeError) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (value < 0.0 || value > 1.0)
    {
        return std::unexpected(rangeError);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateRadiance(Rgb color) noexcept
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
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateViewDepth(double viewDepthMetres) noexcept
{
    if (!IsFinite(viewDepthMetres))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (viewDepthMetres <= 0.0 || viewDepthMetres > kMaximumViewDepthMetres)
    {
        return std::unexpected(ContractError::InvalidViewDepth);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateOpaqueDepthState(OpaqueDepthState opaque) noexcept
{
    if (!opaque.hasSurface)
    {
        return {};
    }
    return ValidateViewDepth(opaque.viewDepthMetres);
}

[[nodiscard]] Rgb Scale(Rgb color, double scale) noexcept
{
    return {.r = color.r * scale, .g = color.g * scale, .b = color.b * scale};
}

[[nodiscard]] Rgb Add(Rgb left, Rgb right) noexcept
{
    return {.r = left.r + right.r, .g = left.g + right.g, .b = left.b + right.b};
}

[[nodiscard]] double MaximumChannel(Rgb color) noexcept
{
    return std::max(std::max(color.r, color.g), color.b);
}

[[nodiscard]] std::uint64_t Mix(std::uint64_t value) noexcept
{
    std::uint64_t result = value + kGoldenGammaOdd;
    result = (result ^ (result >> 30U)) * kMixMultiplierA;
    result = (result ^ (result >> 27U)) * kMixMultiplierB;
    return result ^ (result >> 31U);
}

[[nodiscard]] std::expected<std::uint64_t, ContractError> CheckedAdd(std::uint64_t left, std::uint64_t right) noexcept
{
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
    {
        return std::unexpected(ContractError::CounterOverflow);
    }
    return left + right;
}

[[nodiscard]] std::expected<std::uint32_t, ContractError> CheckedAdd32(std::uint32_t left, std::uint32_t right) noexcept
{
    if (left > std::numeric_limits<std::uint32_t>::max() - right)
    {
        return std::unexpected(ContractError::CounterOverflow);
    }
    return left + right;
}

[[nodiscard]] std::expected<void, ContractError> ValidateFragment(TransparentFragment const &fragment) noexcept
{
    auto const color = ValidatePremultipliedColor(fragment.color);
    if (!color)
    {
        return std::unexpected(color.error());
    }
    auto const depth = ValidateViewDepth(fragment.viewDepthMetres);
    if (!depth)
    {
        return std::unexpected(depth.error());
    }
    // A coverage decision has already resolved to present or absent by the time a fragment carries it, so its alpha
    // is exactly one or exactly zero. A fractional value would ask source-over to blend the fragment partially and
    // ask the depth policy to let it occlude everything behind it, which are contradictory claims about the same
    // sample.
    if (fragment.mode != CoverageMode::AlphaBlend && fragment.color.alpha.value != 0.0 &&
        fragment.color.alpha.value != 1.0)
    {
        return std::unexpected(ContractError::FractionalCoverageAlpha);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateFragments(
    std::span<TransparentFragment const> fragments) noexcept
{
    if (fragments.size() > static_cast<std::size_t>(kMaximumFragmentsPerPixel))
    {
        return std::unexpected(ContractError::InvalidFragmentCount);
    }
    for (TransparentFragment const &fragment : fragments)
    {
        auto const valid = ValidateFragment(fragment);
        if (!valid)
        {
            return std::unexpected(valid.error());
        }
    }
    return {};
}

// The declared total order, back to front. The third key is the input index, so the comparison is a strict total
// order on the fragment set and the result cannot depend on whether the sort algorithm is stable.
[[nodiscard]] bool IsFartherFirst(TransparentFragment const &left, std::uint32_t leftIndex,
                                  TransparentFragment const &right, std::uint32_t rightIndex) noexcept
{
    if (left.viewDepthMetres != right.viewDepthMetres)
    {
        return left.viewDepthMetres > right.viewDepthMetres;
    }
    if (left.drawOrder != right.drawOrder)
    {
        return left.drawOrder < right.drawOrder;
    }
    return leftIndex < rightIndex;
}

// The declared submission order. drawOrder is the submission identity, so it, and not the position a caller
// happened to gather the fragment into, is what "first submitted" means; the input index is the tie-break so that
// two fragments submitted at the same index still have a total order.
[[nodiscard]] bool IsEarlierSubmitted(TransparentFragment const &left, std::uint32_t leftIndex,
                                      TransparentFragment const &right, std::uint32_t rightIndex) noexcept
{
    if (left.drawOrder != right.drawOrder)
    {
        return left.drawOrder < right.drawOrder;
    }
    return leftIndex < rightIndex;
}

[[nodiscard]] std::expected<void, ContractError> ValidateOitWeightSettings(OitWeightSettings const &settings) noexcept
{
    if (!IsFinite(settings.uniformWeight) || !IsFinite(settings.nearScaleMetres) ||
        !IsFinite(settings.farScaleMetres) || !IsFinite(settings.minimumWeight) || !IsFinite(settings.maximumWeight))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.minimumWeight < kMinimumOitWeight || settings.maximumWeight < settings.minimumWeight ||
        settings.maximumWeight > kMaximumOitWeight)
    {
        return std::unexpected(ContractError::InvalidWeightSettings);
    }
    if (settings.nearScaleMetres < kMinimumOitDepthScaleMetres ||
        settings.nearScaleMetres > kMaximumOitDepthScaleMetres ||
        settings.farScaleMetres < kMinimumOitDepthScaleMetres || settings.farScaleMetres > kMaximumOitDepthScaleMetres)
    {
        return std::unexpected(ContractError::InvalidWeightSettings);
    }
    // The uniform weight is bounded by the published limits whichever function is selected, because it is a weight
    // the settings declare rather than a value the settings ignore, and a stored weight below kMinimumOitWeight
    // describes a fragment the technique has already said it will not let vanish from the average.
    if (settings.uniformWeight < kMinimumOitWeight || settings.uniformWeight > kMaximumOitWeight)
    {
        return std::unexpected(ContractError::InvalidWeightSettings);
    }
    if (settings.function == OitWeightFunction::Uniform &&
        (settings.uniformWeight < settings.minimumWeight || settings.uniformWeight > settings.maximumWeight))
    {
        return std::unexpected(ContractError::InvalidWeightSettings);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateAccumulator(WeightedOitAccumulator const &accumulator) noexcept
{
    if (!IsFinite(accumulator.weightedColorSum) || !IsFinite(accumulator.weightedAlphaSum) ||
        !IsFinite(accumulator.revealage))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (accumulator.weightedColorSum.r < 0.0 || accumulator.weightedColorSum.g < 0.0 ||
        accumulator.weightedColorSum.b < 0.0 || accumulator.weightedAlphaSum < 0.0)
    {
        return std::unexpected(ContractError::NegativeRadiance);
    }
    if (accumulator.revealage < 0.0 || accumulator.revealage > 1.0)
    {
        return std::unexpected(ContractError::TransmissionOutOfRange);
    }
    if (MaximumChannel(accumulator.weightedColorSum) > kMaximumWeightedAccumulation ||
        accumulator.weightedAlphaSum > kMaximumWeightedAccumulation)
    {
        return std::unexpected(ContractError::AccumulationOverflow);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateUv(Float2 uv) noexcept
{
    if (!IsFinite(uv.x) || !IsFinite(uv.y))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    {
        return std::unexpected(ContractError::InvalidUv);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateFogSettings(FogSettings const &settings) noexcept
{
    if (!IsFinite(settings.densityPerMetre))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.densityPerMetre < 0.0 || settings.densityPerMetre > kMaximumFogDensityPerMetre)
    {
        return std::unexpected(ContractError::InvalidFogSettings);
    }
    return ValidateRadiance(settings.inscatteringRadiance);
}

[[nodiscard]] double ProbabilisticOr(double left, double right) noexcept
{
    return (left + right) - (left * right);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// A. Quantities
// ---------------------------------------------------------------------------------------------------------------

std::expected<Coverage, ContractError> MakeCoverage(double value) noexcept
{
    auto const valid = ValidateUnitValue(value, ContractError::CoverageOutOfRange);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    return Coverage{.value = value};
}

std::expected<Opacity, ContractError> MakeOpacity(double value) noexcept
{
    auto const valid = ValidateUnitValue(value, ContractError::OpacityOutOfRange);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    return Opacity{.value = value};
}

std::expected<Transmission, ContractError> MakeTransmission(double value) noexcept
{
    auto const valid = ValidateUnitValue(value, ContractError::TransmissionOutOfRange);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    return Transmission{.value = value};
}

std::expected<BlendAlpha, ContractError> MakeBlendAlpha(double value) noexcept
{
    auto const valid = ValidateUnitValue(value, ContractError::AlphaOutOfRange);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    return BlendAlpha{.value = value};
}

std::expected<ReactiveMask, ContractError> MakeReactiveMask(double value) noexcept
{
    auto const valid = ValidateUnitValue(value, ContractError::ReactiveMaskOutOfRange);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    return ReactiveMask{.value = value};
}

std::expected<BlendAlpha, ContractError> BlendAlphaFromCoverage(Coverage coverage, Opacity opacity) noexcept
{
    auto const validCoverage = ValidateUnitValue(coverage.value, ContractError::CoverageOutOfRange);
    if (!validCoverage)
    {
        return std::unexpected(validCoverage.error());
    }
    auto const validOpacity = ValidateUnitValue(opacity.value, ContractError::OpacityOutOfRange);
    if (!validOpacity)
    {
        return std::unexpected(validOpacity.error());
    }
    return BlendAlpha{.value = coverage.value * opacity.value};
}

std::expected<Transmission, ContractError> TransmissionFromAlpha(BlendAlpha alpha) noexcept
{
    auto const valid = ValidateUnitValue(alpha.value, ContractError::AlphaOutOfRange);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    return Transmission{.value = 1.0 - alpha.value};
}

std::expected<BlendAlpha, ContractError> AlphaFromTransmission(Transmission transmission) noexcept
{
    auto const valid = ValidateUnitValue(transmission.value, ContractError::TransmissionOutOfRange);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    return BlendAlpha{.value = 1.0 - transmission.value};
}

std::expected<double, ContractError> SceneLinearLuminance(Rgb color) noexcept
{
    auto const valid = ValidateRadiance(color);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    return (kRec709RedWeight * color.r) + (kRec709GreenWeight * color.g) + (kRec709BlueWeight * color.b);
}

// ---------------------------------------------------------------------------------------------------------------
// B. Straight and premultiplied colour
// ---------------------------------------------------------------------------------------------------------------

std::expected<void, ContractError> ValidateStraightColor(StraightColor const &color) noexcept
{
    auto const radiance = ValidateRadiance(color.rgb);
    if (!radiance)
    {
        return std::unexpected(radiance.error());
    }
    return ValidateUnitValue(color.alpha.value, ContractError::AlphaOutOfRange);
}

std::expected<void, ContractError> ValidatePremultipliedColor(PremultipliedColor const &color) noexcept
{
    if (!IsFinite(color.rgb) || !IsFinite(color.alpha.value))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (color.rgb.r < 0.0 || color.rgb.g < 0.0 || color.rgb.b < 0.0)
    {
        return std::unexpected(ContractError::NegativeRadiance);
    }
    if (color.alpha.value < 0.0 || color.alpha.value > 1.0)
    {
        return std::unexpected(ContractError::AlphaOutOfRange);
    }
    if (MaximumChannel(color.rgb) > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::RadianceTooLarge);
    }
    double const bound = color.alpha.value * kMaximumSceneLinearValue * kPremultipliedBoundSlack;
    if (MaximumChannel(color.rgb) > bound)
    {
        return std::unexpected(ContractError::NotPremultiplied);
    }
    return {};
}

std::expected<PremultipliedColor, ContractError> Premultiply(StraightColor const &color) noexcept
{
    auto const valid = ValidateStraightColor(color);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    return PremultipliedColor{.rgb = Scale(color.rgb, color.alpha.value), .alpha = color.alpha};
}

std::expected<StraightColor, ContractError> Unpremultiply(PremultipliedColor const &color) noexcept
{
    auto const valid = ValidatePremultipliedColor(color);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (color.alpha.value == 0.0)
    {
        // Validation already refused every zero-alpha colour except transparent black.
        return StraightColor{.rgb = {}, .alpha = color.alpha};
    }
    return StraightColor{.rgb = Scale(color.rgb, 1.0 / color.alpha.value), .alpha = color.alpha};
}

// ---------------------------------------------------------------------------------------------------------------
// C. Source-over
// ---------------------------------------------------------------------------------------------------------------

std::expected<PremultipliedColor, ContractError> SourceOver(PremultipliedColor const &source,
                                                            PremultipliedColor const &destination) noexcept
{
    auto const validSource = ValidatePremultipliedColor(source);
    if (!validSource)
    {
        return std::unexpected(validSource.error());
    }
    auto const validDestination = ValidatePremultipliedColor(destination);
    if (!validDestination)
    {
        return std::unexpected(validDestination.error());
    }

    double const destinationWeight = 1.0 - source.alpha.value;
    return PremultipliedColor{
        .rgb = Add(source.rgb, Scale(destination.rgb, destinationWeight)),
        .alpha = BlendAlpha{.value = source.alpha.value + (destinationWeight * destination.alpha.value)}};
}

std::expected<Rgb, ContractError> CompositeOverOpaqueBackground(PremultipliedColor const &layer,
                                                                Rgb background) noexcept
{
    auto const validLayer = ValidatePremultipliedColor(layer);
    if (!validLayer)
    {
        return std::unexpected(validLayer.error());
    }
    auto const validBackground = ValidateRadiance(background);
    if (!validBackground)
    {
        return std::unexpected(validBackground.error());
    }
    return Add(layer.rgb, Scale(background, 1.0 - layer.alpha.value));
}

// ---------------------------------------------------------------------------------------------------------------
// D. Coverage decisions
// ---------------------------------------------------------------------------------------------------------------

std::expected<AlphaTestResult, ContractError> EvaluateAlphaTest(BlendAlpha alpha,
                                                                AlphaTestSettings const &settings) noexcept
{
    auto const validAlpha = ValidateUnitValue(alpha.value, ContractError::AlphaOutOfRange);
    if (!validAlpha)
    {
        return std::unexpected(validAlpha.error());
    }
    auto const validThreshold = ValidateUnitValue(settings.threshold, ContractError::InvalidThreshold);
    if (!validThreshold)
    {
        return std::unexpected(validThreshold.error());
    }

    bool const passed = alpha.value >= settings.threshold;
    return AlphaTestResult{.passed = passed,
                           .alpha = alpha.value,
                           .threshold = settings.threshold,
                           .resolvedAlpha = BlendAlpha{.value = passed ? 1.0 : 0.0}};
}

std::expected<std::uint64_t, ContractError> StochasticSampleHash(SampleIdentity identity,
                                                                 StochasticCoverageSettings const &settings) noexcept
{
    if (settings.sampleCount == 0U || settings.sampleCount > kMaximumStochasticSampleCount)
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }
    if (identity.pixelX >= kMaximumRenderDimension || identity.pixelY >= kMaximumRenderDimension)
    {
        return std::unexpected(ContractError::InvalidPixelCoordinate);
    }
    if (identity.sampleIndex >= settings.sampleCount)
    {
        return std::unexpected(ContractError::InvalidSampleIndex);
    }

    std::uint64_t hash = Mix(static_cast<std::uint64_t>(identity.pixelX));
    hash = Mix(hash ^ (static_cast<std::uint64_t>(identity.pixelY) * kPixelYSalt));
    hash = Mix(hash ^ (static_cast<std::uint64_t>(identity.sampleIndex) * kSampleSalt));
    if (settings.animateWithFrameIndex)
    {
        hash = Mix(hash ^ (static_cast<std::uint64_t>(identity.frameIndex) * kFrameSalt));
    }
    return Mix(hash ^ static_cast<std::uint64_t>(settings.seed));
}

std::expected<double, ContractError> StochasticSampleThreshold(SampleIdentity identity,
                                                               StochasticCoverageSettings const &settings) noexcept
{
    auto const hash = StochasticSampleHash(identity, settings);
    if (!hash)
    {
        return std::unexpected(hash.error());
    }
    // The top 53 bits are the mantissa of a double, so the quotient is exact and lands in [0, 1).
    return static_cast<double>(*hash >> 11U) / static_cast<double>(1ULL << 53U);
}

std::expected<StochasticCoverageResult, ContractError> EvaluateStochasticCoverage(
    BlendAlpha alpha, PixelCoordinate pixel, std::uint32_t frameIndex,
    StochasticCoverageSettings const &settings) noexcept
{
    auto const validAlpha = ValidateUnitValue(alpha.value, ContractError::AlphaOutOfRange);
    if (!validAlpha)
    {
        return std::unexpected(validAlpha.error());
    }
    if (settings.sampleCount == 0U || settings.sampleCount > kMaximumStochasticSampleCount)
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }
    if (pixel.x >= kMaximumRenderDimension || pixel.y >= kMaximumRenderDimension)
    {
        return std::unexpected(ContractError::InvalidPixelCoordinate);
    }

    std::uint32_t accepted = 0U;
    std::uint64_t mask = 0ULL;
    for (std::uint32_t sample = 0U; sample < settings.sampleCount; ++sample)
    {
        SampleIdentity const identity{
            .pixelX = pixel.x, .pixelY = pixel.y, .sampleIndex = sample, .frameIndex = frameIndex};
        auto const threshold = StochasticSampleThreshold(identity, settings);
        if (!threshold)
        {
            return std::unexpected(threshold.error());
        }
        if (alpha.value > *threshold)
        {
            ++accepted;
            mask |= 1ULL << sample;
        }
    }

    double const step = 1.0 / static_cast<double>(settings.sampleCount);
    double const fraction = static_cast<double>(accepted) * step;
    return StochasticCoverageResult{.sampleCount = settings.sampleCount,
                                    .acceptedSampleCount = accepted,
                                    .acceptedSampleMask = mask,
                                    .requestedAlpha = alpha.value,
                                    .achievedCoverage = Coverage{.value = fraction},
                                    .quantizationStep = step,
                                    .coverageError = fraction - alpha.value,
                                    .fullyAccepted = accepted == settings.sampleCount,
                                    .fullyRejected = accepted == 0U};
}

// ---------------------------------------------------------------------------------------------------------------
// E. Ordering and the opaque depth test
// ---------------------------------------------------------------------------------------------------------------

std::expected<FragmentOrder, ContractError> SortFragmentsBackToFront(
    std::span<TransparentFragment const> fragments) noexcept
{
    auto const valid = ValidateFragments(fragments);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }

    FragmentOrder order{};
    order.count = static_cast<std::uint32_t>(fragments.size());
    for (std::uint32_t index = 0U; index < order.count; ++index)
    {
        order.indices[index] = index;
    }

    // Insertion sort over at most kMaximumFragmentsPerPixel entries. The bound is small, the algorithm allocates
    // nothing, and the comparison is written out so that the declared tie-break is visible rather than delegated.
    for (std::uint32_t position = 1U; position < order.count; ++position)
    {
        std::uint32_t const candidate = order.indices[position];
        std::uint32_t hole = position;
        while (hole > 0U && IsFartherFirst(fragments[candidate], candidate, fragments[order.indices[hole - 1U]],
                                           order.indices[hole - 1U]))
        {
            order.indices[hole] = order.indices[hole - 1U];
            --hole;
        }
        order.indices[hole] = candidate;
    }

    for (std::uint32_t position = 1U; position < order.count; ++position)
    {
        if (fragments[order.indices[position]].viewDepthMetres ==
            fragments[order.indices[position - 1U]].viewDepthMetres)
        {
            ++order.depthTieCount;
        }
    }
    order.strictlyOrdered = order.depthTieCount == 0U;
    return order;
}

std::expected<bool, ContractError> PassesOpaqueDepthTest(double fragmentViewDepthMetres,
                                                         OpaqueDepthState opaque) noexcept
{
    auto const validFragment = ValidateViewDepth(fragmentViewDepthMetres);
    if (!validFragment)
    {
        return std::unexpected(validFragment.error());
    }
    auto const validOpaque = ValidateOpaqueDepthState(opaque);
    if (!validOpaque)
    {
        return std::unexpected(validOpaque.error());
    }
    if (!opaque.hasSurface)
    {
        return true;
    }
    return fragmentViewDepthMetres < opaque.viewDepthMetres;
}

// ---------------------------------------------------------------------------------------------------------------
// F. Bounded storage
// ---------------------------------------------------------------------------------------------------------------

std::expected<BoundedFragmentList, ContractError> CollectBoundedFragments(
    std::span<TransparentFragment const> fragments, BoundedFragmentSettings const &settings) noexcept
{
    if (settings.capacity == 0U || settings.capacity > kMaximumStoredFragmentCount)
    {
        return std::unexpected(ContractError::InvalidCapacity);
    }
    auto const order = SortFragmentsBackToFront(fragments);
    if (!order)
    {
        return std::unexpected(order.error());
    }

    BoundedFragmentList list{};
    list.capacity = settings.capacity;
    list.attemptedCount = order->count;
    std::uint32_t const kept = std::min(order->count, settings.capacity);
    list.count = kept;
    list.droppedCount = order->count - kept;
    list.overflowed = list.droppedCount > 0U;

    if (settings.policy == FragmentOverflowPolicy::KeepNearest)
    {
        // The sorted order runs far to near, so the nearest kept fragments are the tail of it.
        std::uint32_t const first = order->count - kept;
        for (std::uint32_t slot = 0U; slot < kept; ++slot)
        {
            list.fragments[slot] = fragments[order->indices[first + slot]];
        }
        return list;
    }

    // KeepFirstSubmitted decides membership by the declared submission identity, drawOrder with the input index as
    // the tie-break, and then restores the declared back-to-front order, so the stored list still composites
    // correctly even though the *choice* of what to store ignored geometry entirely. Selecting on the array
    // position instead would make the kept set depend on the order the caller gathered the fragments in, which is
    // exactly the rasterization-order dependence drawOrder exists to remove.
    std::array<std::uint32_t, kMaximumFragmentsPerPixel> submitted{};
    for (std::uint32_t index = 0U; index < order->count; ++index)
    {
        submitted[index] = index;
    }
    for (std::uint32_t position = 1U; position < order->count; ++position)
    {
        std::uint32_t const candidate = submitted[position];
        std::uint32_t hole = position;
        while (hole > 0U && IsEarlierSubmitted(fragments[candidate], candidate, fragments[submitted[hole - 1U]],
                                               submitted[hole - 1U]))
        {
            submitted[hole] = submitted[hole - 1U];
            --hole;
        }
        submitted[hole] = candidate;
    }

    std::array<bool, kMaximumFragmentsPerPixel> keep{};
    for (std::uint32_t slot = 0U; slot < kept; ++slot)
    {
        keep[submitted[slot]] = true;
    }

    std::uint32_t stored = 0U;
    for (std::uint32_t position = 0U; position < order->count && stored < kept; ++position)
    {
        std::uint32_t const index = order->indices[position];
        if (keep[index])
        {
            list.fragments[stored] = fragments[index];
            ++stored;
        }
    }
    list.count = stored;
    return list;
}

// ---------------------------------------------------------------------------------------------------------------
// G. Sorted forward reference composition
// ---------------------------------------------------------------------------------------------------------------

std::expected<CompositionStatistics, ContractError> AccumulateCompositionStatistics(
    CompositionStatistics const &left, CompositionStatistics const &right) noexcept
{
    std::array<std::uint64_t, 7U> const leftFields{left.inputFragmentCount,
                                                   left.compositedFragmentCount,
                                                   left.depthRejectedFragmentCount,
                                                   left.fullyTransparentFragmentCount,
                                                   left.droppedFragmentCount,
                                                   left.depthTieCount,
                                                   left.depthWriteCount};
    std::array<std::uint64_t, 7U> const rightFields{right.inputFragmentCount,
                                                    right.compositedFragmentCount,
                                                    right.depthRejectedFragmentCount,
                                                    right.fullyTransparentFragmentCount,
                                                    right.droppedFragmentCount,
                                                    right.depthTieCount,
                                                    right.depthWriteCount};

    std::array<std::uint64_t, 7U> sums{};
    for (std::size_t field = 0U; field < sums.size(); ++field)
    {
        auto const sum = CheckedAdd(leftFields[field], rightFields[field]);
        if (!sum)
        {
            return std::unexpected(sum.error());
        }
        sums[field] = *sum;
    }

    return CompositionStatistics{.inputFragmentCount = sums[0],
                                 .compositedFragmentCount = sums[1],
                                 .depthRejectedFragmentCount = sums[2],
                                 .fullyTransparentFragmentCount = sums[3],
                                 .droppedFragmentCount = sums[4],
                                 .depthTieCount = sums[5],
                                 .depthWriteCount = sums[6]};
}

std::expected<SortedCompositionResult, ContractError> CompositeSortedFragments(
    SortedCompositionInput const &input, SortedCompositionSettings const &settings) noexcept
{
    auto const order = SortFragmentsBackToFront(input.fragments);
    if (!order)
    {
        return std::unexpected(order.error());
    }
    auto const validOpaque = ValidateOpaqueDepthState(input.opaque);
    if (!validOpaque)
    {
        return std::unexpected(validOpaque.error());
    }
    auto const validBackground = ValidateRadiance(input.backgroundRadiance);
    if (!validBackground)
    {
        return std::unexpected(validBackground.error());
    }

    SortedCompositionResult result{};
    result.order = *order;
    result.fragmentCount = order->count;
    result.layer = TransparentBlack();
    result.revealage = 1.0;
    result.statistics.inputFragmentCount = order->count;
    result.statistics.droppedFragmentCount = input.droppedFragmentCount;
    result.statistics.depthTieCount = order->depthTieCount;
    result.complete = input.droppedFragmentCount == 0U;

    for (std::uint32_t position = 0U; position < order->count; ++position)
    {
        std::uint32_t const index = order->indices[position];
        TransparentFragment const &fragment = input.fragments[index];
        FragmentEvidence evidence{};
        evidence.inputIndex = index;
        evidence.sortedPosition = position;
        evidence.viewDepthMetres = fragment.viewDepthMetres;
        evidence.alpha = fragment.color.alpha.value;
        evidence.revealageBefore = result.revealage;
        evidence.revealageAfter = result.revealage;
        evidence.tiedWithPrevious =
            position > 0U && fragment.viewDepthMetres == input.fragments[order->indices[position - 1U]].viewDepthMetres;

        bool passesDepth = true;
        if (settings.testAgainstOpaqueDepth)
        {
            auto const passed = PassesOpaqueDepthTest(fragment.viewDepthMetres, input.opaque);
            if (!passed)
            {
                return std::unexpected(passed.error());
            }
            passesDepth = *passed;
        }

        if (!passesDepth)
        {
            evidence.outcome = FragmentOutcome::RejectedByOpaqueDepth;
            ++result.statistics.depthRejectedFragmentCount;
        }
        else if (fragment.color.alpha.value == 0.0)
        {
            evidence.outcome = FragmentOutcome::RejectedAsFullyTransparent;
            ++result.statistics.fullyTransparentFragmentCount;
        }
        else
        {
            auto const composited = SourceOver(fragment.color, result.layer);
            if (!composited)
            {
                return std::unexpected(composited.error());
            }
            result.layer = *composited;
            result.revealage *= 1.0 - fragment.color.alpha.value;
            evidence.revealageAfter = result.revealage;
            evidence.outcome = FragmentOutcome::Composited;
            ++result.statistics.compositedFragmentCount;

            if (settings.depthWrite == DepthWritePolicy::CoverageDecided && fragment.mode != CoverageMode::AlphaBlend)
            {
                evidence.writesDepth = true;
                ++result.statistics.depthWriteCount;
                result.depthWritten = true;
                // The sorted walk runs far to near, so the last writer is the nearest surviving fragment and its
                // depth is what a later pass reads.
                result.writtenViewDepthMetres = fragment.viewDepthMetres;
            }
        }

        result.fragments[position] = evidence;
    }

    // Survival is a fact about the fragments *in front*, so it is only known after the walk finishes.
    double transmittance = 1.0;
    for (std::uint32_t reverse = order->count; reverse > 0U; --reverse)
    {
        std::uint32_t const position = reverse - 1U;
        result.fragments[position].transmittanceInFront = transmittance;
        if (result.fragments[position].outcome == FragmentOutcome::Composited)
        {
            transmittance *= 1.0 - result.fragments[position].alpha;
        }
    }

    auto const overBackground = CompositeOverOpaqueBackground(result.layer, input.backgroundRadiance);
    if (!overBackground)
    {
        return std::unexpected(overBackground.error());
    }
    result.overBackground = *overBackground;
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// H. Weighted blended order-independent compositing
// ---------------------------------------------------------------------------------------------------------------

std::expected<double, ContractError> OitFragmentWeight(double viewDepthMetres,
                                                       OitWeightSettings const &settings) noexcept
{
    auto const validDepth = ValidateViewDepth(viewDepthMetres);
    if (!validDepth)
    {
        return std::unexpected(validDepth.error());
    }
    auto const validSettings = ValidateOitWeightSettings(settings);
    if (!validSettings)
    {
        return std::unexpected(validSettings.error());
    }

    if (settings.function == OitWeightFunction::Uniform)
    {
        return settings.uniformWeight;
    }

    double const near = viewDepthMetres / settings.nearScaleMetres;
    double const far = viewDepthMetres / settings.farScaleMetres;
    double const farSquared = far * far;
    double const denominator = kOitDepthWeightBias + (near * near) + (farSquared * farSquared * farSquared);
    if (!IsFinite(denominator) || denominator <= 0.0)
    {
        return std::unexpected(ContractError::AccumulationOverflow);
    }
    double const base = kOitDepthWeightNumerator / denominator;
    return std::clamp(base, settings.minimumWeight, settings.maximumWeight);
}

std::expected<WeightedOitAccumulator, ContractError> AccumulateWeightedOitFragment(
    WeightedOitAccumulator const &accumulator, TransparentFragment const &fragment,
    OitWeightSettings const &settings) noexcept
{
    auto const validAccumulator = ValidateAccumulator(accumulator);
    if (!validAccumulator)
    {
        return std::unexpected(validAccumulator.error());
    }
    auto const validFragment = ValidateFragment(fragment);
    if (!validFragment)
    {
        return std::unexpected(validFragment.error());
    }
    auto const weight = OitFragmentWeight(fragment.viewDepthMetres, settings);
    if (!weight)
    {
        return std::unexpected(weight.error());
    }
    auto const count = CheckedAdd32(accumulator.fragmentCount, 1U);
    if (!count)
    {
        return std::unexpected(count.error());
    }

    WeightedOitAccumulator result{};
    result.weightedColorSum = Add(accumulator.weightedColorSum, Scale(fragment.color.rgb, *weight));
    result.weightedAlphaSum = accumulator.weightedAlphaSum + (fragment.color.alpha.value * *weight);
    result.revealage = accumulator.revealage * (1.0 - fragment.color.alpha.value);
    result.fragmentCount = *count;

    auto const validResult = ValidateAccumulator(result);
    if (!validResult)
    {
        return std::unexpected(validResult.error());
    }
    return result;
}

std::expected<WeightedOitAccumulator, ContractError> AccumulateWeightedOit(
    std::span<TransparentFragment const> fragments, OitWeightSettings const &settings) noexcept
{
    auto const valid = ValidateFragments(fragments);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }

    WeightedOitAccumulator accumulator{};
    for (TransparentFragment const &fragment : fragments)
    {
        auto const next = AccumulateWeightedOitFragment(accumulator, fragment, settings);
        if (!next)
        {
            return std::unexpected(next.error());
        }
        accumulator = *next;
    }
    return accumulator;
}

std::expected<WeightedOitAccumulator, ContractError> MergeWeightedOitAccumulators(
    WeightedOitAccumulator const &left, WeightedOitAccumulator const &right) noexcept
{
    auto const validLeft = ValidateAccumulator(left);
    if (!validLeft)
    {
        return std::unexpected(validLeft.error());
    }
    auto const validRight = ValidateAccumulator(right);
    if (!validRight)
    {
        return std::unexpected(validRight.error());
    }
    auto const count = CheckedAdd32(left.fragmentCount, right.fragmentCount);
    if (!count)
    {
        return std::unexpected(count.error());
    }

    WeightedOitAccumulator result{};
    result.weightedColorSum = Add(left.weightedColorSum, right.weightedColorSum);
    result.weightedAlphaSum = left.weightedAlphaSum + right.weightedAlphaSum;
    result.revealage = left.revealage * right.revealage;
    result.fragmentCount = *count;

    auto const validResult = ValidateAccumulator(result);
    if (!validResult)
    {
        return std::unexpected(validResult.error());
    }
    return result;
}

std::expected<WeightedOitResult, ContractError> ResolveWeightedOit(WeightedOitAccumulator const &accumulator,
                                                                   Rgb background,
                                                                   WeightedOitResolveSettings const &settings) noexcept
{
    auto const validAccumulator = ValidateAccumulator(accumulator);
    if (!validAccumulator)
    {
        return std::unexpected(validAccumulator.error());
    }
    auto const validBackground = ValidateRadiance(background);
    if (!validBackground)
    {
        return std::unexpected(validBackground.error());
    }
    if (!IsFinite(settings.alphaEpsilon))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.alphaEpsilon < kMinimumOitAlphaEpsilon || settings.alphaEpsilon > kMaximumOitAlphaEpsilon)
    {
        return std::unexpected(ContractError::InvalidAlphaEpsilon);
    }

    bool const guarded = accumulator.weightedAlphaSum < settings.alphaEpsilon;
    double const divisor = std::max(accumulator.weightedAlphaSum, settings.alphaEpsilon);
    Rgb const average = Scale(accumulator.weightedColorSum, 1.0 / divisor);
    double const layerAlpha = 1.0 - accumulator.revealage;

    WeightedOitResult result{};
    result.averageColor = average;
    result.layer = PremultipliedColor{.rgb = Scale(average, layerAlpha), .alpha = BlendAlpha{.value = layerAlpha}};
    result.revealage = accumulator.revealage;
    result.weightedAlphaSum = accumulator.weightedAlphaSum;
    result.usedAlphaEpsilon = guarded;
    result.fullyRevealed = accumulator.revealage == 1.0;
    result.fullyOccluded = accumulator.revealage == 0.0;
    result.fragmentCount = accumulator.fragmentCount;

    auto const overBackground = CompositeOverOpaqueBackground(result.layer, background);
    if (!overBackground)
    {
        return std::unexpected(overBackground.error());
    }
    result.overBackground = *overBackground;
    return result;
}

std::expected<OitComparison, ContractError> CompareWeightedOitToSorted(WeightedOitResult const &weighted,
                                                                       SortedCompositionResult const &sorted) noexcept
{
    auto const weightedLuminance = SceneLinearLuminance(weighted.overBackground);
    if (!weightedLuminance)
    {
        return std::unexpected(weightedLuminance.error());
    }
    auto const sortedLuminance = SceneLinearLuminance(sorted.overBackground);
    if (!sortedLuminance)
    {
        return std::unexpected(sortedLuminance.error());
    }
    auto const validWeightedAlpha = ValidateUnitValue(weighted.layer.alpha.value, ContractError::AlphaOutOfRange);
    if (!validWeightedAlpha)
    {
        return std::unexpected(validWeightedAlpha.error());
    }
    auto const validSortedAlpha = ValidateUnitValue(sorted.layer.alpha.value, ContractError::AlphaOutOfRange);
    if (!validSortedAlpha)
    {
        return std::unexpected(validSortedAlpha.error());
    }

    double const redError = std::abs(weighted.overBackground.r - sorted.overBackground.r);
    double const greenError = std::abs(weighted.overBackground.g - sorted.overBackground.g);
    double const blueError = std::abs(weighted.overBackground.b - sorted.overBackground.b);
    double const channelError = std::max(std::max(redError, greenError), blueError);
    double const scale = std::max(1.0, MaximumChannel(sorted.overBackground));
    double const alphaError = std::abs(weighted.layer.alpha.value - sorted.layer.alpha.value);

    return OitComparison{.maximumChannelError = channelError,
                         .luminanceError = std::abs(*weightedLuminance - *sortedLuminance),
                         .alphaError = alphaError,
                         .sortedLuminance = *sortedLuminance,
                         .weightedLuminance = *weightedLuminance,
                         .colorExact = channelError <= kCompositeExactnessTolerance * scale,
                         .alphaExact = alphaError <= kCompositeExactnessTolerance};
}

// ---------------------------------------------------------------------------------------------------------------
// I. Transparent lighting seam
// ---------------------------------------------------------------------------------------------------------------

std::expected<void, ContractError> ValidateClusterGrid(ClusterGridDescription const &grid) noexcept
{
    if (grid.tileCountX == 0U || grid.tileCountY == 0U || grid.sliceCount == 0U)
    {
        return std::unexpected(ContractError::InvalidClusterGrid);
    }
    std::uint64_t const clusters = static_cast<std::uint64_t>(grid.tileCountX) *
                                   static_cast<std::uint64_t>(grid.tileCountY) *
                                   static_cast<std::uint64_t>(grid.sliceCount);
    if (clusters > static_cast<std::uint64_t>(kMaximumClusterCount))
    {
        return std::unexpected(ContractError::InvalidClusterGrid);
    }
    if (static_cast<std::uint64_t>(grid.clusterCount) != clusters)
    {
        return std::unexpected(ContractError::InvalidClusterGrid);
    }
    return {};
}

std::expected<std::uint32_t, ContractError> ClusterIndex(ClusterGridDescription const &grid, std::uint32_t tileX,
                                                         std::uint32_t tileY, std::uint32_t sliceIndex) noexcept
{
    auto const valid = ValidateClusterGrid(grid);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (tileX >= grid.tileCountX || tileY >= grid.tileCountY)
    {
        return std::unexpected(ContractError::InvalidTileCoordinate);
    }
    if (sliceIndex >= grid.sliceCount)
    {
        return std::unexpected(ContractError::InvalidSliceIndex);
    }
    return tileX + (grid.tileCountX * (tileY + (grid.tileCountY * sliceIndex)));
}

std::expected<TransparentLightBindingEvidence, ContractError> ValidateTransparentLightBinding(
    TransparentLightBinding const &binding, ClusterGridDescription const &grid,
    std::span<std::uint32_t const> lightIndices, std::uint32_t sceneLightCount) noexcept
{
    // Finite first: a NaN depth is not a depth that is out of range, and reporting the range error for it would
    // send a reader looking for a number that was never there.
    if (!IsFinite(binding.fragmentViewDepthMetres) ||
        (binding.opaque.hasSurface && !IsFinite(binding.opaque.viewDepthMetres)))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    // Structural next: the grid, the coordinates that index it, and the buffers the binding claims to read. None of
    // these depend on what the fragment means, and all of them have to hold before the cluster identity or the
    // light list can be interpreted at all.
    auto const expected = ClusterIndex(grid, binding.tileX, binding.tileY, binding.fragmentSliceIndex);
    if (!expected)
    {
        return std::unexpected(expected.error());
    }
    std::uint32_t opaqueClusterIndex = 0U;
    if (binding.opaque.hasSurface)
    {
        auto const opaqueCluster = ClusterIndex(grid, binding.tileX, binding.tileY, binding.opaqueSliceIndex);
        if (!opaqueCluster)
        {
            return std::unexpected(opaqueCluster.error());
        }
        opaqueClusterIndex = *opaqueCluster;
    }
    if (sceneLightCount == 0U || sceneLightCount > kMaximumSceneLightCount)
    {
        return std::unexpected(ContractError::InvalidLightCount);
    }
    if (lightIndices.size() > static_cast<std::size_t>(kMaximumLightIndexCount))
    {
        return std::unexpected(ContractError::MalformedLightRange);
    }
    std::uint64_t const end =
        static_cast<std::uint64_t>(binding.lightOffset) + static_cast<std::uint64_t>(binding.lightCount);
    if (end > static_cast<std::uint64_t>(lightIndices.size()))
    {
        return std::unexpected(ContractError::MalformedLightRange);
    }
    for (std::uint32_t offset = 0U; offset < binding.lightCount; ++offset)
    {
        if (lightIndices[binding.lightOffset + offset] >= sceneLightCount)
        {
            return std::unexpected(ContractError::InvalidLightIndex);
        }
    }

    // Semantic last: what the numbers mean rather than whether they exist.
    auto const validDepth = ValidateViewDepth(binding.fragmentViewDepthMetres);
    if (!validDepth)
    {
        return std::unexpected(validDepth.error());
    }
    auto const validOpaque = ValidateOpaqueDepthState(binding.opaque);
    if (!validOpaque)
    {
        return std::unexpected(validOpaque.error());
    }

    TransparentLightBindingEvidence evidence{};
    evidence.expectedClusterIndex = *expected;
    evidence.consumedOpaqueDepth = binding.opaque.hasSurface;
    if (binding.opaque.hasSurface)
    {
        evidence.opaqueClusterIndex = opaqueClusterIndex;
        evidence.hasOpaqueCluster = true;
        evidence.clusterAmbiguous = binding.opaqueSliceIndex == binding.fragmentSliceIndex;
    }

    if (binding.declaredClusterIndex != *expected)
    {
        if (evidence.hasOpaqueCluster && binding.declaredClusterIndex == evidence.opaqueClusterIndex &&
            !evidence.clusterAmbiguous)
        {
            return std::unexpected(ContractError::ClusterFromOpaqueDepth);
        }
        return std::unexpected(ContractError::ClusterIndexMismatch);
    }

    auto const passed = PassesOpaqueDepthTest(binding.fragmentViewDepthMetres, binding.opaque);
    if (!passed)
    {
        return std::unexpected(passed.error());
    }

    evidence.lightOffset = binding.lightOffset;
    evidence.lightCount = binding.lightCount;
    evidence.lightIndicesValid = true;
    evidence.depthRejected = !*passed;
    return evidence;
}

// ---------------------------------------------------------------------------------------------------------------
// J. Screen-space refraction input
// ---------------------------------------------------------------------------------------------------------------

std::expected<RefractionSample, ContractError> ComputeRefractionSample(RefractionInput const &input,
                                                                       RefractionSettings const &settings) noexcept
{
    // Finite first, over every double the call reads. Checking only some of them meant that a NaN surface UV was
    // reported as whichever structural fault happened to be checked before the UV validation reached it.
    if (!IsFinite(input.surfaceUv.x) || !IsFinite(input.surfaceUv.y) || !IsFinite(input.requestedOffsetUv.x) ||
        !IsFinite(input.requestedOffsetUv.y) || !IsFinite(input.surfaceViewDepthMetres) ||
        (input.offsetOpaque.hasSurface && !IsFinite(input.offsetOpaque.viewDepthMetres)) ||
        !IsFinite(settings.maximumOffsetUv))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    // Structural next: the texture that will be read, the coordinate that addresses it, and the bound the offset is
    // allowed to move within.
    if (input.sourceExtent.width == 0U || input.sourceExtent.height == 0U ||
        input.sourceExtent.width > kMaximumRenderDimension || input.sourceExtent.height > kMaximumRenderDimension)
    {
        return std::unexpected(ContractError::InvalidExtent);
    }
    auto const validSurfaceUv = ValidateUv(input.surfaceUv);
    if (!validSurfaceUv)
    {
        return std::unexpected(validSurfaceUv.error());
    }
    if (settings.maximumOffsetUv <= 0.0 || settings.maximumOffsetUv > kMaximumRefractionOffsetUv)
    {
        return std::unexpected(ContractError::InvalidOffsetLimit);
    }

    // Semantic last: the depths that decide whether the sampled surface is behind the glass at all.
    auto const validDepth = ValidateViewDepth(input.surfaceViewDepthMetres);
    if (!validDepth)
    {
        return std::unexpected(validDepth.error());
    }
    auto const validOpaque = ValidateOpaqueDepthState(input.offsetOpaque);
    if (!validOpaque)
    {
        return std::unexpected(validOpaque.error());
    }

    RefractionSample sample{};
    sample.requestedOffsetLength = std::hypot(input.requestedOffsetUv.x, input.requestedOffsetUv.y);
    Float2 applied = input.requestedOffsetUv;
    if (sample.requestedOffsetLength > settings.maximumOffsetUv)
    {
        double const scale = settings.maximumOffsetUv / sample.requestedOffsetLength;
        applied = {.x = input.requestedOffsetUv.x * scale, .y = input.requestedOffsetUv.y * scale};
        sample.clampedOffset = true;
    }

    Float2 const candidate{.x = input.surfaceUv.x + applied.x, .y = input.surfaceUv.y + applied.y};
    bool const outOfBounds = candidate.x < 0.0 || candidate.x > 1.0 || candidate.y < 0.0 || candidate.y > 1.0;
    bool const foregroundOccluder = settings.rejectForegroundOccluders && input.offsetOpaque.hasSurface &&
                                    input.offsetOpaque.viewDepthMetres < input.surfaceViewDepthMetres;

    if (outOfBounds)
    {
        sample.fallback = RefractionFallback::OutOfBounds;
    }
    else if (foregroundOccluder)
    {
        sample.fallback = RefractionFallback::ForegroundOccluder;
    }

    sample.usedFallback = sample.fallback != RefractionFallback::None;
    sample.appliedOffsetUv = sample.usedFallback ? Float2{} : applied;
    sample.sampledUv = sample.usedFallback ? input.surfaceUv : candidate;
    sample.appliedOffsetLength = std::hypot(sample.appliedOffsetUv.x, sample.appliedOffsetUv.y);

    double const texelX = std::floor(sample.sampledUv.x * static_cast<double>(input.sourceExtent.width));
    double const texelY = std::floor(sample.sampledUv.y * static_cast<double>(input.sourceExtent.height));
    sample.sampledTexel = {.x = std::min(input.sourceExtent.width - 1U, static_cast<std::uint32_t>(texelX)),
                           .y = std::min(input.sourceExtent.height - 1U, static_cast<std::uint32_t>(texelY))};
    return sample;
}

// ---------------------------------------------------------------------------------------------------------------
// K. Fog and frame ordering
// ---------------------------------------------------------------------------------------------------------------

std::expected<double, ContractError> FogTransmittance(double viewDepthMetres, FogSettings const &settings) noexcept
{
    auto const validDepth = ValidateViewDepth(viewDepthMetres);
    if (!validDepth)
    {
        return std::unexpected(validDepth.error());
    }
    auto const validSettings = ValidateFogSettings(settings);
    if (!validSettings)
    {
        return std::unexpected(validSettings.error());
    }
    return std::exp(-settings.densityPerMetre * viewDepthMetres);
}

std::expected<Rgb, ContractError> ApplyFogToRadiance(Rgb radiance, double viewDepthMetres,
                                                     FogSettings const &settings) noexcept
{
    auto const validRadiance = ValidateRadiance(radiance);
    if (!validRadiance)
    {
        return std::unexpected(validRadiance.error());
    }
    auto const transmittance = FogTransmittance(viewDepthMetres, settings);
    if (!transmittance)
    {
        return std::unexpected(transmittance.error());
    }
    return Add(Scale(radiance, *transmittance), Scale(settings.inscatteringRadiance, 1.0 - *transmittance));
}

std::expected<PremultipliedColor, ContractError> ApplyFogToFragment(PremultipliedColor const &color,
                                                                    double viewDepthMetres,
                                                                    FogSettings const &settings) noexcept
{
    auto const validColor = ValidatePremultipliedColor(color);
    if (!validColor)
    {
        return std::unexpected(validColor.error());
    }
    auto const transmittance = FogTransmittance(viewDepthMetres, settings);
    if (!transmittance)
    {
        return std::unexpected(transmittance.error());
    }
    double const inscatteringWeight = (1.0 - *transmittance) * color.alpha.value;
    return PremultipliedColor{
        .rgb = Add(Scale(color.rgb, *transmittance), Scale(settings.inscatteringRadiance, inscatteringWeight)),
        .alpha = color.alpha};
}

std::expected<TransparencyOrderPlan, ContractError> PlanTransparencyOrder(
    std::span<FrameStage const> stages, TransparencyPipelinePolicy const &policy) noexcept
{
    if (stages.empty())
    {
        return std::unexpected(ContractError::InvalidStage);
    }
    if (stages.size() > static_cast<std::size_t>(kMaximumStageCount))
    {
        return std::unexpected(ContractError::TooManyStages);
    }

    TransparencyOrderPlan plan{};
    plan.stageCount = static_cast<std::uint32_t>(stages.size());

    std::array<bool, kFrameStageCount> seen{};
    std::array<std::uint32_t, kFrameStageCount> position{};
    for (std::uint32_t index = 0U; index < plan.stageCount; ++index)
    {
        FrameStage const stage = stages[index];
        auto const slot = static_cast<std::uint32_t>(stage);
        if (slot >= kFrameStageCount)
        {
            return std::unexpected(ContractError::InvalidStage);
        }
        plan.stages[index] = stage;
        if (seen[slot])
        {
            plan.violations |= OrderViolation::DuplicateStage;
            continue;
        }
        seen[slot] = true;
        position[slot] = index;
    }

    auto const slotOf = [](FrameStage stage) noexcept { return static_cast<std::uint32_t>(stage); };
    bool const hasLighting = seen[slotOf(FrameStage::OpaqueLighting)];
    bool const hasComposite = seen[slotOf(FrameStage::TransparentComposite)];
    bool const hasRefractionSource = seen[slotOf(FrameStage::RefractionSourceCopy)];
    bool const hasFog = seen[slotOf(FrameStage::ScreenSpaceFog)];
    bool const hasTemporal = seen[slotOf(FrameStage::TemporalResolve)];
    bool const hasExposure = seen[slotOf(FrameStage::Exposure)];
    bool const hasToneMap = seen[slotOf(FrameStage::ToneMap)];

    plan.hasTransparentComposite = hasComposite;
    plan.transparentCompositePosition = hasComposite ? position[slotOf(FrameStage::TransparentComposite)] : 0U;
    plan.fogFoldedIntoFragments = policy.fog == FogApplication::PerFragmentBeforeComposite;

    if (!hasLighting)
    {
        plan.violations |= OrderViolation::MissingOpaqueLighting;
    }
    if (!hasComposite)
    {
        plan.violations |= OrderViolation::MissingTransparentComposite;
    }
    if (policy.requiresRefractionSource && !hasRefractionSource)
    {
        plan.violations |= OrderViolation::MissingRefractionSource;
    }
    if (policy.requiresTemporalResolve && !hasTemporal)
    {
        plan.violations |= OrderViolation::MissingTemporalResolve;
    }

    if (hasComposite && hasRefractionSource)
    {
        std::uint32_t const refractionPosition = position[slotOf(FrameStage::RefractionSourceCopy)];
        plan.refractionSourcePrecedesComposite = refractionPosition < plan.transparentCompositePosition;
        if (!plan.refractionSourcePrecedesComposite)
        {
            plan.violations |= OrderViolation::RefractionSourceAfterTransparentComposite;
        }
    }
    if (hasLighting && hasRefractionSource &&
        position[slotOf(FrameStage::RefractionSourceCopy)] < position[slotOf(FrameStage::OpaqueLighting)])
    {
        plan.violations |= OrderViolation::RefractionSourceBeforeOpaqueLighting;
    }
    if (hasLighting && hasComposite && plan.transparentCompositePosition < position[slotOf(FrameStage::OpaqueLighting)])
    {
        plan.violations |= OrderViolation::TransparentCompositeBeforeOpaqueLighting;
    }
    if (hasTemporal && hasComposite &&
        plan.transparentCompositePosition > position[slotOf(FrameStage::TemporalResolve)])
    {
        plan.violations |= OrderViolation::TransparentCompositeAfterTemporalResolve;
    }
    if (hasExposure && hasComposite && plan.transparentCompositePosition > position[slotOf(FrameStage::Exposure)])
    {
        plan.violations |= OrderViolation::TransparentCompositeAfterExposure;
    }
    if (hasToneMap && hasComposite && plan.transparentCompositePosition > position[slotOf(FrameStage::ToneMap)])
    {
        plan.violations |= OrderViolation::TransparentCompositeAfterToneMap;
    }

    if (hasFog && hasComposite)
    {
        plan.screenSpaceFogUsesOpaqueDepth =
            position[slotOf(FrameStage::ScreenSpaceFog)] > plan.transparentCompositePosition;
    }

    if (policy.fog == FogApplication::PerFragmentBeforeComposite && hasFog)
    {
        plan.violations |= OrderViolation::FogPolicyMismatch;
    }
    if (policy.fog == FogApplication::ScreenSpaceAfterComposite &&
        (!hasFog || (hasComposite && !plan.screenSpaceFogUsesOpaqueDepth)))
    {
        // A screen-space fog pass that runs before the composite fogs the opaque scene and is then overwritten by
        // unfogged, scene-linear transparent radiance, so the transparent layer is never fogged by anything. The
        // policy declared an approximation the submitted order does not perform, and reporting the plan as clean
        // would let the frame claim a cost it never paid.
        plan.violations |= OrderViolation::FogPolicyMismatch;
    }

    plan.valid = plan.violations == OrderViolation::None;
    return plan;
}

std::expected<TransparencyOrderPlan, ContractError> CanonicalTransparencyOrder(
    TransparencyPipelinePolicy const &policy) noexcept
{
    std::array<FrameStage, kMaximumStageCount> stages{};
    std::uint32_t count = 0U;
    stages[count++] = FrameStage::OpaqueGBuffer;
    stages[count++] = FrameStage::OpaqueLighting;
    if (policy.requiresRefractionSource)
    {
        stages[count++] = FrameStage::RefractionSourceCopy;
    }
    stages[count++] = FrameStage::TransparentComposite;
    if (policy.fog == FogApplication::ScreenSpaceAfterComposite)
    {
        stages[count++] = FrameStage::ScreenSpaceFog;
    }
    if (policy.requiresTemporalResolve)
    {
        stages[count++] = FrameStage::TemporalResolve;
    }
    stages[count++] = FrameStage::Exposure;
    stages[count++] = FrameStage::ToneMap;

    return PlanTransparencyOrder(std::span<FrameStage const>{stages.data(), count}, policy);
}

// ---------------------------------------------------------------------------------------------------------------
// L. Temporal reactive mask
// ---------------------------------------------------------------------------------------------------------------

std::expected<ReactiveMaskResult, ContractError> ComputeReactiveMask(ReactiveMaskInput const &input,
                                                                     ReactiveMaskSettings const &settings) noexcept
{
    auto const validAlpha = ValidateUnitValue(input.transparentAlpha, ContractError::AlphaOutOfRange);
    if (!validAlpha)
    {
        return std::unexpected(validAlpha.error());
    }
    if (!IsFinite(input.refractionOffsetLengthUv))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (input.refractionOffsetLengthUv < 0.0 || input.refractionOffsetLengthUv > kMaximumRefractionOffsetUv)
    {
        return std::unexpected(ContractError::InvalidOffsetLimit);
    }
    if (!IsFinite(settings.alphaWeight) || !IsFinite(settings.refractionWeight) ||
        !IsFinite(settings.stochasticWeight) || !IsFinite(settings.referenceOffsetUv) ||
        !IsFinite(settings.maximumMask))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.alphaWeight < 0.0 || settings.alphaWeight > 1.0 || settings.refractionWeight < 0.0 ||
        settings.refractionWeight > 1.0 || settings.stochasticWeight < 0.0 || settings.stochasticWeight > 1.0 ||
        settings.maximumMask < 0.0 || settings.maximumMask > kFullyReactiveMask || settings.referenceOffsetUv <= 0.0 ||
        settings.referenceOffsetUv > kMaximumRefractionOffsetUv)
    {
        return std::unexpected(ContractError::InvalidReactiveSettings);
    }

    ReactiveMaskResult result{};
    if (!input.wroteTransparentMotionVector)
    {
        result.alphaTerm = settings.alphaWeight * input.transparentAlpha;
    }
    if (input.usedRefraction)
    {
        // A fallback is not a quiet degradation: the sample jumps between the refracted and the unrefracted
        // position as the surface moves, so it is at least as reactive as the offset it replaced.
        double const normalized =
            input.refractionFallback ? 1.0 : std::min(1.0, input.refractionOffsetLengthUv / settings.referenceOffsetUv);
        result.refractionTerm = settings.refractionWeight * normalized;
    }
    if (input.animatedStochasticCoverage)
    {
        result.stochasticTerm = settings.stochasticWeight;
    }

    double const combined =
        ProbabilisticOr(ProbabilisticOr(result.alphaTerm, result.refractionTerm), result.stochasticTerm);
    result.saturated = combined > settings.maximumMask;
    double const masked = std::min(combined, settings.maximumMask);
    result.mask = ReactiveMask{.value = masked};
    result.anyRisk = masked > 0.0;
    return result;
}

} // namespace ch32::transparency
