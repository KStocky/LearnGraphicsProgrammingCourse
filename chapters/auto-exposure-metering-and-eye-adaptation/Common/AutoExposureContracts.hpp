#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

// Chapter 31 teaching contracts for auto-exposure metering and eye adaptation.
//
// Scope and honesty rules for this phase:
//
//   * Everything here is a deterministic CPU reference model of the parts of an auto-exposure system that are
//     normally hidden inside two compute dispatches and a single persistent float: how a stored pre-exposed sample
//     becomes an absolute luminance, how that luminance lands in a bounded log2 histogram, which reduction of that
//     histogram is called "the" scene luminance, how a target exposure follows from a declared middle-grey
//     calibration, how the committed exposure walks toward that target over time, and how the committed exposure
//     becomes the next frame's pre-exposure.
//   * There is no universally correct metering policy. An arithmetic mean, a log average, a percentile window, a
//     centre weighting, and an artist-supplied mask each answer a different question, and each is wrong somewhere.
//     The chapter therefore implements all of them, publishes the diagnostics needed to tell them apart, and
//     refuses to nominate a winner. What is *not* negotiable is that the measurement is taken in absolute
//     luminance, that the exposure is applied exactly once, and that the value applied to the frame the viewer sees
//     is the value the previous frame committed.
//   * The one-frame feedback delay is modelled in the type system, not in a comment. Three different C++ types
//     carry three different exposures: DisplayedExposure is applied to this frame, MeasuredTargetExposure is what
//     this frame's measurement asks for, and CommittedExposure is what the next frame will use. None converts to
//     another, and only DisplayedExposure can be handed to ApplyDisplayExposure, so a stack cannot accidentally
//     apply the exposure it just measured to the frame it just measured it from.
//   * Sample weights are integer fixed-point, and the histogram accumulates integer weights and integer counts.
//     Integer addition is associative and commutative, so a CPU scan, a WARP dispatch, and a real GPU wave
//     reduction agree bit for bit no matter what order they visit samples in. The reference deliberately avoids
//     designing anything that would need floating-point atomics.
//   * Real-valued reductions are declared as what they are. A histogram of bin *counts* cannot reproduce a mean
//     luminance exactly, because a bin only records how much weight landed inside it, not where inside it. Two
//     exact sufficient statistics are therefore accumulated alongside the bins, and every estimator publishes
//     whether it used an exact statistic or a bin-centre reconstruction, together with the worst-case bin
//     quantization error in stops and whether that bound is even valid for the data it saw.
//
// Units and sign conventions:
//
//   * Scene samples are pre-exposed scene-linear Rec.709 RGB. The stored value is preExposure times the radiance
//     the renderer meant. Metering divides by the recorded pre-exposure exactly once, in AbsoluteLuminance, and
//     nothing downstream divides again.
//   * Luminance is relative: it is Rec.709 luminance of scene-linear RGB in whatever radiometric unit the renderer
//     chose. It is *not* calibrated to cd/m^2.
//   * Exposure is a positive multiplicative scale applied to absolute scene luminance. Its base-2 logarithm is
//     called `stops` throughout. stops = log2(scale), so +1 stop doubles the displayed brightness and -1 stop
//     halves it. exposureCompensationStops follows the same convention: positive brightens.
//   * That sign convention is the opposite of photographic exposure value, where +1 EV means a darker picture, and
//     the two must not be confused. This chapter never uses the term EV100 for its own quantities. A real EV100
//     would need an absolute photometric luminance in cd/m^2 and a named reflected-light calibration constant
//     (K = 12.5 in the common convention) to write EV100 = log2(L * 100 / K); nothing here supplies either, so
//     calling log2 of a relative luminance an EV100 would be a unit error dressed up as a name.
//
// Deliberately out of scope, and not approximated here: tone-curve design, which Chapter 4 owns; the compositing
// order that consumes the exposure, which Chapter 30 owns; local or spatially varying exposure; sensor noise and
// ISO models; flicker-suppression heuristics tuned per game; and the GPU dispatch shape, wave reduction strategy,
// and persistent-buffer lifetime, which the lab phase owns.

namespace ch31::auto_exposure
{

// ---------------------------------------------------------------------------------------------------------------
// Bounds. Every limit exists so that a malformed configuration is rejected instead of producing a plausible number.
// ---------------------------------------------------------------------------------------------------------------

inline constexpr std::uint32_t kMaximumSampleDimension = 4'096U;
// Exactly kMaximumSampleDimension squared, so a frame that satisfies the per-axis limit is never rejected by the
// total instead.
inline constexpr std::uint32_t kMaximumSampleCount = 16'777'216U;

inline constexpr std::uint32_t kMinimumBinCount = 8U;
inline constexpr std::uint32_t kMaximumBinCount = 256U;
// 128 bins over 24 stops is 0.1875 stops per bin, which is finer than the smallest exposure step a viewer notices
// and small enough to keep the whole histogram in one group-shared array on the GPU.
inline constexpr std::uint32_t kDefaultBinCount = 128U;
inline constexpr double kDefaultMinimumLog2Luminance = -10.0;
inline constexpr double kDefaultMaximumLog2Luminance = 14.0;
inline constexpr double kMinimumLog2LuminanceSpan = 1.0;
inline constexpr double kMaximumLog2LuminanceSpan = 96.0;
inline constexpr double kLog2LuminanceLimit = 64.0;

// Fixed-point one for a sample weight. A power of two so that w / kWeightOne is exact in binary floating point and
// so that the fixed-point product below is a shift, not a divide.
inline constexpr std::uint32_t kWeightOne = 1'024U;
// kMaximumSampleCount samples at kWeightOne each is 2^34; the cap leaves 2^6 headroom for merging tile histograms
// and still keeps totalWeight * kPercentileOne inside 64 bits: 2^40 * 10^6 is about 1.1e18, comfortably under the
// 1.8e19 a uint64 holds. Every accumulator that a histogram publishes is checked against this cap before it is
// used, because the percentile multiply is the one place where exceeding it produces a plausible wrong window
// rather than an obviously wrong number.
inline constexpr std::uint64_t kMaximumTotalWeight = 1'099'511'627'776ULL;
// Fixed-point one for a percentile, so that a percentile boundary is an exact integer comparison rather than a
// float comparison that lands on either side depending on the compiler.
inline constexpr std::uint32_t kPercentileOne = 1'000'000U;

// ---------------------------------------------------------------------------------------------------------------
// The integration seam. Three of this chapter's outputs are consumed by Chapter 30's ApplyExposure and
// ValidatePostFrame: the stored pre-exposed scene-linear colour, the pre-exposure, and the exposure scale. Chapter
// 30 rejects any of the three above 1e6, so a Chapter 31 legal domain wider than that would let this chapter
// publish a number the next stage refuses, and the contract would be lying about what it produces.
//
// The three limits below are therefore a *mirror* of Chapter 30's kMaximumSceneLinearValue, not an independent
// choice. They are duplicated rather than included because the two chapters are separate teaching units with no
// build dependency between them; the mirrored value is pinned by a test so that the two cannot drift apart
// silently. The metering domain further down is deliberately *wider*, because a metered luminance never crosses
// this seam.
// ---------------------------------------------------------------------------------------------------------------

inline constexpr double kMaximumSceneLinearValue = 1.0e6;
inline constexpr double kMinimumPreExposure = 1.0e-6;
inline constexpr double kMaximumPreExposure = 1.0e6;
inline constexpr double kMaximumExposureScale = 1.0e6;
// stops = log2(scale), so the stop limit is the base-2 logarithm of the scale limit and nothing else. The literal
// is exactly the double std::log2(kMaximumExposureScale) returns, and it rounds *down*, so exp2 of it is just
// under 1e6 and a round trip through either direction stays inside the seam. Writing a round number such as 64
// here instead would declare a legal exposure of 1.8e19, which Chapter 30 would refuse.
inline constexpr double kMaximumExposureStops = 19.931568569324174;

// The largest absolute luminance the contracts will reason about. It sits well below 2^kLog2LuminanceLimit so that
// its logarithm is always inside the range a layout may declare, and well below the point where a squared or
// summed intermediate could leave double precision. It is far wider than the seam above on purpose: an absolute
// luminance is a measurement that stays inside this chapter, and clamping the measurement to the compositing
// domain would make a legitimately bright scene unmeasurable. A stored sample inside the seam divided by a legal
// pre-exposure reaches at most kMaximumSceneLinearValue / kMinimumPreExposure = 1e12, so the limit below is a
// guard for callers that supply a luminance from somewhere other than AbsoluteLuminance rather than a limit any
// legal frame can reach.
inline constexpr double kMaximumLuminance = 1.0e18;
inline constexpr double kMaximumCompensationStops = 32.0;
inline constexpr double kMaximumAdaptationSpeedPerSecond = 1.0e3;
inline constexpr double kMaximumFrameDeltaSeconds = 10.0;
inline constexpr double kMaximumCentreFalloffPower = 8.0;
inline constexpr double kMinimumCentreFalloffPower = 0.25;

enum class ContractError : std::uint8_t
{
    NonFinite,
    NegativeRadiance,
    RadianceTooLarge,
    NegativeLuminance,
    LuminanceTooLarge,
    NonPositiveLuminance,
    InvalidExtent,
    ExtentTooLarge,
    SizeMismatch,
    InvalidPixel,
    InvalidPreExposure,
    InvalidExposureScale,
    InvalidExposureStops,
    InvalidBinCount,
    InvalidLog2LuminanceRange,
    InvalidBinIndex,
    LayoutMismatch,
    InconsistentHistogram,
    InvalidWeight,
    WeightOverflow,
    InvalidMaskValue,
    // The mask that was supplied does not hash to the identity the configuration declared, so the configuration
    // identity that adaptation reuses would describe a different measurement than the one actually taken.
    MaskIdentityMismatch,
    InvalidCentreProfile,
    InvalidPercentile,
    InvalidPercentileRange,
    InvalidMiddleGrey,
    InvalidCompensation,
    InvalidExposureBounds,
    InvalidAdaptationSpeed,
    InvalidFrameDelta,
    InvalidFrameIndex,
    InvalidPolicy,
    InvalidPreExposureBounds,
    NoAcceptedSamples,
    NoPositiveLuminance,
};

struct Extent2D final
{
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] bool operator==(Extent2D const &) const noexcept = default;
};

struct PixelCoordinate final
{
    std::uint32_t x{};
    std::uint32_t y{};

    [[nodiscard]] bool operator==(PixelCoordinate const &) const noexcept = default;
};

// Scene-linear RGB with Rec.709 primaries, already multiplied by the frame pre-exposure.
struct Rgb final
{
    double r{};
    double g{};
    double b{};

    [[nodiscard]] bool operator==(Rgb const &) const noexcept = default;
};

[[nodiscard]] std::expected<std::uint32_t, ContractError> PixelCount(Extent2D extent) noexcept;

// Rec.709 luminance of a scene-linear colour, in the colour's own radiometric unit. Negative channels are rejected
// rather than clamped: a negative radiance is a bug in whatever produced the sample, and silently raising it to
// zero would hide a broken light, a bad filter kernel, or a decoded NaN. Exactly zero is a legal luminance and is
// handled everywhere without ever evaluating log2(0).
[[nodiscard]] std::expected<double, ContractError> SceneLinearLuminance(Rgb color) noexcept;

// Removes the frame pre-exposure exactly once. Every metering decision in this chapter is taken on the value this
// function returns, which is why the same scene metered under two different pre-exposures must reach the same
// exposure: the pre-exposure is a storage scale, not a property of the light.
[[nodiscard]] std::expected<double, ContractError> AbsoluteLuminance(Rgb preExposedSceneLinear,
                                                                     double preExposure) noexcept;

// stops = log2(scale). ExposureScaleFromStops is its inverse, and the two round-trip to within floating-point
// rounding for every stops value inside +/- kMaximumExposureStops. Both directions also check the *scale* against
// kMaximumExposureScale, so a stops value that is inside its own limit but converts to a scale the integration
// seam would refuse is rejected at the conversion rather than at the seam.
[[nodiscard]] std::expected<double, ContractError> ExposureStopsFromScale(double exposureScale) noexcept;
[[nodiscard]] std::expected<double, ContractError> ExposureScaleFromStops(double exposureStops) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// A. Sample weights
// ---------------------------------------------------------------------------------------------------------------

// Weights are integer fixed point in [0, kWeightOne]. The GPU histogram pass adds them with integer atomics, so
// the accumulated weight is independent of dispatch order, wave size, and tile decomposition. A float weight would
// make the same histogram depend on which thread got there first.
//
// Rounding is half away from zero, which is the one rule that does not depend on the current floating-point
// rounding mode and is trivial to reproduce in HLSL.
[[nodiscard]] std::expected<std::uint32_t, ContractError> QuantizeUnitWeight(double weight) noexcept;

// Fixed-point product of two weights, rounded half up, saturating at kWeightOne by construction because both
// operands are already bounded by it. Centre weighting and the artist mask compose through this function and are
// deliberately *not* normalized here: normalization belongs in the estimators, which divide by the total weight
// they actually accumulated, and normalizing earlier would throw away the fact that a mask covers only part of the
// frame.
[[nodiscard]] std::expected<std::uint32_t, ContractError> CombineWeights(std::uint32_t left,
                                                                         std::uint32_t right) noexcept;

// A radially symmetric centre weighting. The normalized radius r of a pixel is its distance from the frame centre
// divided by the half diagonal, so r is 0 at the centre and exactly 1 at each corner. The profile is
//
//     weight(r) = edgeWeight + (centreWeight - edgeWeight) * (1 - r)^falloffPower
//
// which is exactly centreWeight at the centre and exactly edgeWeight at the corners for every falloffPower.
struct CentreWeightSettings final
{
    double centreWeight{1.0};
    double edgeWeight{0.0};
    double falloffPower{2.0};

    [[nodiscard]] bool operator==(CentreWeightSettings const &) const noexcept = default;
};

[[nodiscard]] std::expected<std::uint32_t, ContractError> CentreWeight(Extent2D extent, PixelCoordinate pixel,
                                                                       CentreWeightSettings const &settings) noexcept;

// A metering mask supplied by whoever authored the shot. Values are in [0, 1] and are quantized with the same rule
// as every other weight.
struct MaskView final
{
    Extent2D extent{};
    std::span<double const> weights{};
};

// A stable identity for the mask *contents*, so that swapping one mask for another is detectable. It is computed
// from the quantized weights rather than the raw doubles, because two masks that quantize identically produce
// identical histograms and must not be treated as different configurations.
[[nodiscard]] std::expected<std::uint64_t, ContractError> MaskIdentity(MaskView mask) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// B. Histogram layout
// ---------------------------------------------------------------------------------------------------------------

struct HistogramLayout final
{
    std::uint32_t binCount{kDefaultBinCount};
    double minimumLog2Luminance{kDefaultMinimumLog2Luminance};
    double maximumLog2Luminance{kDefaultMaximumLog2Luminance};

    [[nodiscard]] bool operator==(HistogramLayout const &) const noexcept = default;
};

struct HistogramLayoutFacts final
{
    std::uint32_t binCount{};
    double minimumLog2Luminance{};
    double maximumLog2Luminance{};
    double log2LuminanceSpan{};
    // Width of one bin in stops. Also the quantization step of every bin-centre estimate.
    double log2LuminancePerBin{};
    double binsPerLog2Luminance{};
    double minimumLuminance{};
    double maximumLuminance{};
    std::uint64_t identity{};

    [[nodiscard]] bool operator==(HistogramLayoutFacts const &) const noexcept = default;
};

[[nodiscard]] std::expected<HistogramLayoutFacts, ContractError> ValidateHistogramLayout(
    HistogramLayout layout) noexcept;

// Edge b, for b in [0, binCount]. The edge is computed as minimum + span * b / binCount rather than by repeatedly
// adding a bin width, so edge 0 is exactly the declared minimum, edge binCount is exactly the declared maximum, and
// no drift accumulates along the array. Every classification decision in this file compares against these exact
// values, so bin b contains exactly the log2 luminances in [edge(b), edge(b + 1)).
[[nodiscard]] std::expected<double, ContractError> BinLowerLog2Luminance(HistogramLayoutFacts const &facts,
                                                                         std::uint32_t binEdge) noexcept;
[[nodiscard]] std::expected<double, ContractError> BinCentreLog2Luminance(HistogramLayoutFacts const &facts,
                                                                          std::uint32_t bin) noexcept;
[[nodiscard]] std::expected<double, ContractError> BinCentreLuminance(HistogramLayoutFacts const &facts,
                                                                      std::uint32_t bin) noexcept;

enum class BinClass : std::uint8_t
{
    // The sample's log2 luminance lies inside the declared range.
    Interior = 0U,
    // Below the declared minimum. Whether that is an accepted sample in bin 0 or a rejection is a declared policy.
    BelowRange,
    AboveRange,
    // Exactly zero luminance. log2(0) is never evaluated; a black sample is classified structurally.
    Zero,
};

struct LuminanceBin final
{
    BinClass classification{BinClass::Zero};
    // The bin a counted sample lands in. BelowRange and Zero map to bin 0, AboveRange maps to binCount - 1. The
    // value is meaningful even when the policy rejects the sample, so a diagnostic can still say where it would
    // have gone.
    std::uint32_t bin{};
    double log2Luminance{};
    // False only for BinClass::Zero, where log2Luminance carries no information.
    bool hasLog2Luminance{};

    [[nodiscard]] bool operator==(LuminanceBin const &) const noexcept = default;
};

[[nodiscard]] std::expected<LuminanceBin, ContractError> ClassifyLog2Luminance(HistogramLayoutFacts const &facts,
                                                                               double log2Luminance) noexcept;
[[nodiscard]] std::expected<LuminanceBin, ContractError> ClassifyLuminance(HistogramLayoutFacts const &facts,
                                                                           double luminance) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// C. Histogram construction
// ---------------------------------------------------------------------------------------------------------------

// A frame that is mostly black sky is the case where this policy is visible. Counting the black in the lowest bin
// keeps a percentile window anchored in the dark part of the distribution, so the exposure does not chase the few
// bright pixels. Rejecting the black instead meters only the lit part of the frame, which is what a mask usually
// wants, and makes a fully black frame report that it measured nothing rather than reporting the darkest
// representable luminance as if it were a measurement.
enum class BlackSamplePolicy : std::uint8_t
{
    CountInLowestBin = 0U,
    Reject,
};

// Out-of-range samples are either saturated into the end bin or rejected. Saturating keeps the total weight equal
// to the sample count, which percentile metering depends on; rejecting keeps a single specular pixel from dragging
// the top of the window, at the cost of a percentile that no longer refers to the whole frame.
enum class RangeSamplePolicy : std::uint8_t
{
    CountInEndBin = 0U,
    Reject,
};

enum class SampleRejection : std::uint8_t
{
    None = 0U,
    NonFiniteChannel,
    NegativeChannel,
    // A channel, or the absolute luminance that follows from it, is finite but outside the domain these contracts
    // declare. It is data rather than a crash, and it is reported separately from a NaN so that a scene authored in
    // the wrong units is distinguishable from a broken shader.
    LuminanceOutOfDomain,
    // The composed centre and mask weight quantized to zero. A masked-out sample is not an error; it is a sample
    // the shot said not to meter, and it is counted so that an accidentally empty mask is visible.
    ZeroWeight,
    ZeroLuminance,
    BelowRange,
    AboveRange,
};

inline constexpr std::size_t kSampleRejectionCount = 8U;

struct HistogramSettings final
{
    HistogramLayout layout{};
    BlackSamplePolicy blackSamples{BlackSamplePolicy::CountInLowestBin};
    RangeSamplePolicy belowRange{RangeSamplePolicy::CountInEndBin};
    RangeSamplePolicy aboveRange{RangeSamplePolicy::CountInEndBin};

    [[nodiscard]] bool operator==(HistogramSettings const &) const noexcept = default;
};

struct MeteringWeighting final
{
    bool useCentreWeighting{};
    CentreWeightSettings centre{};
    bool useMask{};
    // Must be the value MaskIdentity returned for the mask that will be supplied. It participates in the
    // configuration identity so that changing the mask is detectable without hashing it every frame.
    std::uint64_t maskIdentity{};

    [[nodiscard]] bool operator==(MeteringWeighting const &) const noexcept = default;
};

struct HistogramBuildSettings final
{
    HistogramSettings histogram{};
    MeteringWeighting weighting{};

    [[nodiscard]] bool operator==(HistogramBuildSettings const &) const noexcept = default;
};

// The per-sample decision, exposed on its own because it is exactly what one GPU thread does. A malformed
// *configuration* is an error; a malformed *sample* is data, and comes back as a rejection reason with an accepted
// flag of false.
//
// The sample's values are inspected before its weight, so a NaN inside a masked-out region is still reported. A
// frame buffer with NaNs in it is a rendering bug, and metering is the cheapest place in the frame to notice one.
struct HistogramSample final
{
    bool accepted{};
    SampleRejection rejection{SampleRejection::None};
    BinClass classification{BinClass::Zero};
    std::uint32_t bin{};
    std::uint32_t weight{};
    double absoluteLuminance{};
    double log2Luminance{};
    bool hasLog2Luminance{};

    [[nodiscard]] bool operator==(HistogramSample const &) const noexcept = default;
};

[[nodiscard]] std::expected<HistogramSample, ContractError> ClassifySample(Rgb preExposedSceneLinear,
                                                                           double preExposure, std::uint32_t weight,
                                                                           HistogramSettings const &settings) noexcept;

struct HistogramStatistics final
{
    std::uint32_t acceptedSampleCount{};
    std::uint64_t acceptedWeight{};
    std::uint32_t rejectedSampleCount{};
    std::uint64_t rejectedWeight{};
    std::array<std::uint32_t, kSampleRejectionCount> rejectedSampleCountsByReason{};
    // Samples that were classified out of range and *accepted* anyway, because the policy saturates into an end
    // bin. A sample the policy rejected instead appears only under rejectedSampleCountsByReason.
    std::uint32_t belowRangeSampleCount{};
    std::uint64_t belowRangeWeight{};
    std::uint32_t aboveRangeSampleCount{};
    std::uint64_t aboveRangeWeight{};
    std::uint32_t zeroLuminanceSampleCount{};
    std::uint64_t zeroLuminanceWeight{};
    // Exact sufficient statistics, accumulated alongside the bins because the bins cannot reproduce them. A bin
    // records how much weight fell inside it, not where inside it, so any mean recovered from bin centres is a
    // quantized estimate. These two sums are what make the arithmetic mean and the log average exact.
    //
    // Both are real-valued, so unlike the bin weights they depend on summation order in floating point. The CPU
    // reference declares its order: samples in row-major order, then histogram merges in the order they are
    // merged. A GPU implementation must reproduce a fixed reduction order and must not use floating-point atomics;
    // that is a lab-phase obligation, not something this contract can hide.
    double weightedLuminanceSum{};
    double weightedLog2LuminanceSum{};
    // Weight that contributed to weightedLog2LuminanceSum. It is smaller than acceptedWeight exactly when black
    // samples were accepted into the lowest bin, because log2(0) has no value to contribute.
    std::uint64_t log2AccumulatedWeight{};
    double minimumObservedLuminance{};
    double maximumObservedLuminance{};
    bool hasAcceptedSample{};

    [[nodiscard]] bool operator==(HistogramStatistics const &) const noexcept = default;
};

struct LuminanceHistogram final
{
    HistogramLayout layout{};
    std::array<std::uint64_t, kMaximumBinCount> binWeights{};
    std::array<std::uint32_t, kMaximumBinCount> binSampleCounts{};
    HistogramStatistics statistics{};

    [[nodiscard]] bool operator==(LuminanceHistogram const &) const noexcept = default;
};

struct LuminanceSampleView final
{
    Extent2D extent{};
    std::span<Rgb const> pixels{};
};

// Builds the histogram for one frame. Pass an empty mask when settings.weighting.useMask is false; supplying a
// mask that the weighting did not ask for, or asking for a mask and not supplying one, is an error rather than a
// silently ignored argument.
//
// When a mask is used, its identity is recomputed from the quantized weights during the same pass that reads them
// and compared with settings.weighting.maskIdentity. A mismatch is MaskIdentityMismatch, not a warning: the
// declared identity is what MeteringConfigurationIdentity hashes and what AdaptationReset::ConfigurationChanged
// watches, so a histogram built from one mask under another mask's identity would swap the metering region
// without the adaptation loop noticing. Per-sample value errors in the mask are reported first, because a NaN or
// an out-of-range weight makes the identity meaningless. When useMask is false the declared identity is not read
// at all, and a stale value left in it changes neither the histogram nor the configuration identity.
[[nodiscard]] std::expected<LuminanceHistogram, ContractError> BuildLuminanceHistogram(
    LuminanceSampleView samples, MaskView mask, double preExposure, HistogramBuildSettings const &settings) noexcept;

// Tile-parallel construction is only useful if merging is exact. Integer weights and counts add exactly; the two
// real-valued sufficient statistics add in the declared left-then-right order. Layouts must match exactly, because
// two histograms over different ranges describe different quantities and adding them would silently reinterpret
// one of them.
//
// Every integer accumulator is guarded, not just the ones that happen to be large in the common case: a merge that
// would carry a counter past its width, or any weight past kMaximumTotalWeight, is WeightOverflow. Nothing is
// clamped, because a clamped tile histogram is indistinguishable from a correct one at every later stage.
//
// The error precedence is fixed and observable: layout validity, then LayoutMismatch, then InconsistentHistogram
// for either operand carrying weight or samples outside its declared bin count, then WeightOverflow.
[[nodiscard]] std::expected<LuminanceHistogram, ContractError> MergeHistograms(
    LuminanceHistogram const &left, LuminanceHistogram const &right) noexcept;

struct HistogramDiagnostics final
{
    std::uint32_t binCount{};
    std::uint32_t occupiedBinCount{};
    std::uint32_t lowestOccupiedBin{};
    std::uint32_t highestOccupiedBin{};
    bool hasOccupiedBin{};
    std::uint64_t totalWeight{};
    std::uint32_t totalSampleCount{};
    std::uint32_t acceptedSampleCount{};
    std::uint32_t rejectedSampleCount{};
    std::uint64_t acceptedWeight{};
    std::uint64_t rejectedWeight{};
    std::array<std::uint32_t, kSampleRejectionCount> rejectedSampleCountsByReason{};
    std::uint32_t belowRangeSampleCount{};
    std::uint32_t aboveRangeSampleCount{};
    std::uint32_t zeroLuminanceSampleCount{};
    std::uint64_t belowRangeWeight{};
    std::uint64_t aboveRangeWeight{};
    std::uint64_t zeroLuminanceWeight{};
    double minimumObservedLuminance{};
    double maximumObservedLuminance{};
    bool hasAcceptedSample{};
    // True when some accepted weight was saturated into an end bin, which is the fact that invalidates the
    // bin-centre error bound and usually means the declared range is wrong for this scene.
    bool clippedIntoLowestBin{};
    bool clippedIntoHighestBin{};

    [[nodiscard]] bool operator==(HistogramDiagnostics const &) const noexcept = default;
};

// Checks a histogram against everything it claims about itself and publishes the derived facts. A histogram that
// arrives from a GPU readback, from a merge, or from a test fixture is not trusted: it is validated. Nothing is
// repaired or clamped, because a silently repaired histogram meters to a plausible number that no dispatch
// produced.
//
// The error precedence is fixed and observable, and every caller that summarizes before metering inherits it:
//
//   1. Layout validity, which decides how many bins even exist.
//   2. InconsistentHistogram for weight or samples parked outside the declared bin count, which no estimator would
//      ever look at.
//   3. WeightOverflow for any accumulator past its declared cap: a bin sum or a published weight above
//      kMaximumTotalWeight, a bin sample total or an accepted-plus-rejected total past what a uint32 holds.
//   4. InconsistentHistogram for statistics that disagree with the bins or with each other.
//
// Caps come before agreement because a value past a cap cannot be reasoned about at all, while a disagreement is
// still a statement about two well-defined numbers.
[[nodiscard]] std::expected<HistogramDiagnostics, ContractError> SummarizeHistogram(
    LuminanceHistogram const &histogram) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// D. Metering policies
// ---------------------------------------------------------------------------------------------------------------

// Three reductions of the same weighted distribution. They are genuinely different questions, and none of them is
// the right answer everywhere:
//
//   * ArithmeticMean is the mean of luminance. It conserves total light, which is the physically meaningful thing
//     to say about a frame, and it is what a photographer's averaging meter does. It is also dominated by area
//     times brightness, so one small sun in an otherwise dark frame pulls it up by orders of magnitude.
//   * LogAverage is the mean of log2 luminance, that is, the geometric mean. Equal stop differences count equally,
//     which matches how exposure is authored and how a viewer perceives brightness ratios, and it is the classic
//     key-value input to a Reinhard curve. It ignores how much *light* is present, so a frame that is half black
//     and half bright meters far darker than its energy suggests, and it has no value at all if every sample is
//     black.
//   * PercentileWindow takes the weighted geometric mean of the bins between two cumulative-weight boundaries. It
//     is the robust choice: a bright outlier above the upper percentile and a black region below the lower
//     percentile are both excluded by construction. It is also the only one of the three that cannot be exact,
//     because a percentile is defined by position in the distribution and the distribution is only known to bin
//     resolution.
enum class MeteringPolicy : std::uint8_t
{
    ArithmeticMean = 0U,
    LogAverage,
    PercentileWindow,
};

// Centre weighting and the artist mask are deliberately *not* members of MeteringPolicy. They decide which samples
// count and how much, and they compose with all three reductions; a policy decides how the surviving distribution
// is reduced to one number. Folding them together would make "centre-weighted percentile" unrepresentable.
//
// A percentile boundary is an integer position inside the accumulated weight, which makes it exactly reproducible
// for a given histogram but *not* exactly independent of how much weight the frame accumulated. Changing the
// sample count, or a mask, moves a boundary by less than one sample's weight and can clip a partial bin slightly
// differently. The arithmetic mean and the log average have no such dependence.
struct PercentileWindow final
{
    double lowerPercentile{0.5};
    double upperPercentile{0.95};

    [[nodiscard]] bool operator==(PercentileWindow const &) const noexcept = default;
};

struct MeteringSettings final
{
    MeteringPolicy policy{MeteringPolicy::LogAverage};
    PercentileWindow window{};

    [[nodiscard]] bool operator==(MeteringSettings const &) const noexcept = default;
};

struct MeteringResult final
{
    MeteringPolicy policy{};
    double meteredLuminance{};
    double meteredLog2Luminance{};
    // The same estimator evaluated only from bin centres, which is all a GPU pass that keeps no extra sums can do.
    // For PercentileWindow it is the metered value itself.
    double binCentreLog2Luminance{};
    // Half a bin, in stops. When binCentreEstimateBounded is true, |binCentreLog2Luminance - meteredLog2Luminance|
    // is at most this value, because every accepted sample lies within half a bin of its own bin centre.
    double binQuantizationLog2Bound{};
    // False when black samples were counted in the lowest bin or when weight was saturated into an end bin. In
    // either case a sample's true luminance is not within half a bin of its recorded bin, so the bound above does
    // not apply and must not be quoted.
    bool binCentreEstimateBounded{};
    // True for ArithmeticMean and LogAverage, which read the exact sufficient statistics. False for
    // PercentileWindow, whose answer is quantized to bin centres by definition.
    bool usesExactSufficientStatistic{};
    // Weight that the log average was normalized by. Smaller than acceptedWeight when black samples were accepted,
    // because those samples have no log2 to average.
    std::uint64_t logAverageWeight{};
    bool excludedZeroLuminanceFromLogAverage{};
    // Percentile facts. firstWindowBin and lastWindowBin are inclusive; windowWeight is the weight actually inside
    // the window after clipping partial bins at both ends.
    std::uint32_t firstWindowBin{};
    std::uint32_t lastWindowBin{};
    std::uint64_t windowWeight{};
    std::uint64_t lowerTargetWeight{};
    std::uint64_t upperTargetWeight{};
    double windowLowerLog2Luminance{};
    double windowUpperLog2Luminance{};
    // True when the two percentile boundaries quantized to the same integer position inside the accumulated
    // weight. That happens whenever lowerPercentile equals upperPercentile, and it also happens for two *distinct*
    // percentiles whose separation is smaller than one unit of weight: the boundaries are
    // floor(totalWeight * percentile / kPercentileOne), so any window narrower than kPercentileOne / totalWeight
    // collapses onto a single position. The estimator then reports the bin that owns that cumulative position
    // instead of failing, which is how a plain median is requested, and windowWeight is that bin's whole weight.
    // The collapse therefore depends on how much weight the frame accumulated, which is the same weight dependence
    // the percentile policy carries everywhere and is not a defect in this flag.
    bool degenerateWindow{};
    HistogramDiagnostics diagnostics{};

    [[nodiscard]] bool operator==(MeteringResult const &) const noexcept = default;
};

// Reduces a validated histogram to one luminance under the requested policy.
//
// The error precedence is fixed and observable: the metering settings, then the layout, then everything
// SummarizeHistogram decides in its own declared order, then NoAcceptedSamples for a histogram that accepted
// nothing, then NoPositiveLuminance for one that accepted only black, and only then anything the chosen estimator
// reports.
//
// Every policy refuses a frame in which no accepted sample carries a positive luminance, and refuses it with the
// same error, NoPositiveLuminance. That case is not the same as NoAcceptedSamples: an all-black frame under the
// default CountInLowestBin policy has accepted samples, accepted weight, and an occupied lowest bin, so a
// percentile window would happily report the lowest bin's centre luminance and a target exposure would follow from
// a measurement that never saw any light. There is no exposure that makes a black frame middle grey, and log2(0)
// is never evaluated to pretend otherwise. The test for the condition is exact rather than floating point:
// log2AccumulatedWeight is zero exactly when every unit of accepted weight came from a black sample.
//
// The bounded limitation, stated rather than papered over: when black samples and *positive* samples that
// underflowed the declared range are both saturated into the lowest bin, the histogram records only their combined
// weight, not which part of that bin's weight is black. A percentile window that selects the lowest bin then
// reports the bin centre for a position that may be owned mostly by black weight. Distinguishing the two would
// need a per-bin zero-luminance weight, which is a whole extra array on the GPU for a case that
// binCentreEstimateBounded already flags as unbounded and that clippedIntoLowestBin already reports. The contract
// therefore refuses only the case it can decide exactly, and publishes the diagnostics for the case it cannot.
[[nodiscard]] std::expected<MeteringResult, ContractError> MeterHistogram(LuminanceHistogram const &histogram,
                                                                          MeteringSettings const &settings) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// E. Target exposure
// ---------------------------------------------------------------------------------------------------------------

// Middle grey is a calibration choice, not a constant of nature. 0.18 is the reflectance of a photographic grey
// card and the value most tone curves are fitted around, but a curve whose neutral sits elsewhere needs its own
// value here, and getting it wrong shifts every frame by a fixed number of stops.
struct ExposureCalibration final
{
    double middleGreyLuminance{0.18};

    [[nodiscard]] bool operator==(ExposureCalibration const &) const noexcept = default;
};

struct TargetExposureSettings final
{
    ExposureCalibration calibration{};
    // Positive brightens, matching stops = log2(scale). This is the artist's offset from the metered result, and it
    // is applied in the log domain so that it is a true stop offset rather than a multiply that depends on where
    // the meter landed.
    double exposureCompensationStops{0.0};
    double minimumExposureStops{-16.0};
    double maximumExposureStops{16.0};

    [[nodiscard]] bool operator==(TargetExposureSettings const &) const noexcept = default;
};

// What this frame's measurement asks for. It is never the exposure applied to this frame; only the adaptation step
// may turn it into a CommittedExposure, and only the previous frame's CommittedExposure becomes a
// DisplayedExposure.
struct MeasuredTargetExposure final
{
    double scale{1.0};
    double stops{0.0};
    double unclampedStops{0.0};
    // Zero in manual mode, where nothing was measured and the target came from the authored stops instead.
    double meteredLuminance{};
    // meteredLuminance * scale. Equal to middleGreyLuminance exactly when compensation is zero and neither bound
    // clamped, which is the sanity case that pins the whole calibration.
    double exposedMeteredLuminance{};
    bool clampedToMinimum{};
    bool clampedToMaximum{};

    [[nodiscard]] bool operator==(MeasuredTargetExposure const &) const noexcept = default;
};

[[nodiscard]] std::expected<MeasuredTargetExposure, ContractError> ComputeTargetExposure(
    double meteredLuminance, TargetExposureSettings const &settings) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// F. Temporal adaptation
// ---------------------------------------------------------------------------------------------------------------

enum class ExposureMode : std::uint8_t
{
    Automatic = 0U,
    // The target comes from AutoExposureSettings::manualExposureStops and no measurement is consulted. Adaptation
    // still runs, so a manual change still eases in rather than snapping.
    Manual,
};

// Named from what the *exposure* does, not from what the scene does. "Dark to bright" is ambiguous: a scene going
// dark makes the exposure go up. Naming the rates after the exposure removes the ambiguity, and the direction
// tests pin it so a later rename cannot silently swap them.
struct AdaptationSettings final
{
    // Used when the exposure scale must rise, which is what happens when the scene got darker and the displayed
    // image must brighten. Eyes adapt to darkness slowly, so this is usually the slower of the two.
    double exposureIncreaseSpeedPerSecond{1.0};
    // Used when the exposure scale must fall, which is what happens when the scene got brighter.
    double exposureDecreaseSpeedPerSecond{2.5};

    [[nodiscard]] bool operator==(AdaptationSettings const &) const noexcept = default;
};

enum class AdaptationDirection : std::uint8_t
{
    Steady = 0U,
    ExposureIncreasing,
    ExposureDecreasing,
};

// alpha = 1 - exp(-speed * dt). Zero dt gives exactly zero, so a paused frame changes nothing; a large dt
// approaches but never exceeds one; a zero speed freezes the exposure. Because the residual after dt is
// exp(-speed * dt), splitting a step into two halves multiplies the residuals and lands on the same place: the
// smoothing is frame-rate independent by construction, not by tuning.
[[nodiscard]] std::expected<double, ContractError> SmoothingAlpha(double speedPerSecond, double deltaSeconds) noexcept;

// Everything whose change invalidates a stored adaptation value. Exposure compensation, exposure bounds, and the
// adaptation rates are deliberately absent: each of them changes the target or the rate coherently, and adaptation
// walks to the new answer without a discontinuity. Frame resolution is absent too, because the histogram describes
// a distribution over luminance, not over pixels, and a resize does not invalidate a measured exposure.
struct MeteringConfiguration final
{
    HistogramSettings histogram{};
    MeteringWeighting weighting{};
    MeteringSettings metering{};
    ExposureMode mode{ExposureMode::Automatic};

    [[nodiscard]] bool operator==(MeteringConfiguration const &) const noexcept = default;
};

[[nodiscard]] std::expected<std::uint64_t, ContractError> MeteringConfigurationIdentity(
    MeteringConfiguration const &configuration) noexcept;

struct AutoExposureSettings final
{
    // Identity-bearing. A change here means the stored exposure was produced by a different measurement, so it is
    // discarded.
    MeteringConfiguration configuration{};
    // Coherent. A change here shifts the target, and adaptation eases toward it.
    TargetExposureSettings target{};
    AdaptationSettings adaptation{};
    double manualExposureStops{0.0};

    [[nodiscard]] bool operator==(AutoExposureSettings const &) const noexcept = default;
};

enum class AdaptationReset : std::uint32_t
{
    None = 0U,
    // No state has ever been committed.
    NoHistory = 1U << 0U,
    // frameIndex is zero, so there is no previous frame that could have committed anything.
    FirstFrame = 1U << 1U,
    // The camera teleported; the previous frame's luminance says nothing about this one.
    CameraCut = 1U << 2U,
    // The stored state was produced by a frame other than the immediately preceding one. A stale value fed through
    // a dt-based smoother produces an adaptation that silently depends on how long the stall was.
    NonSequentialProducerFrame = 1U << 3U,
    // The measurement changed meaning: a different histogram layout, black or range policy, weighting, mask, or
    // reduction.
    ConfigurationChanged = 1U << 4U,
    // Automatic and manual are different control laws, so a value produced by one is not a starting point for the
    // other. The mode is part of the configuration identity as well, so this flag never appears alone; it exists to
    // name the cause instead of leaving the learner to guess which part of the configuration moved.
    ExposureModeChanged = 1U << 5U,
    // The stored value is not a finite in-range number of stops.
    InvalidHistoryValue = 1U << 6U,
};

[[nodiscard]] constexpr AdaptationReset operator|(AdaptationReset left, AdaptationReset right) noexcept
{
    return static_cast<AdaptationReset>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr AdaptationReset &operator|=(AdaptationReset &left, AdaptationReset right) noexcept
{
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool HasReset(AdaptationReset resets, AdaptationReset reset) noexcept
{
    return (static_cast<std::uint32_t>(resets) & static_cast<std::uint32_t>(reset)) != 0U;
}

// The exposure scale applied to the image the viewer sees this frame. It was committed by the previous frame and
// is the only exposure type ApplyDisplayExposure accepts.
struct DisplayedExposure final
{
    double scale{1.0};
    double stops{0.0};

    [[nodiscard]] bool operator==(DisplayedExposure const &) const noexcept = default;
};

// The exposure committed for the next frame, and the value the next frame's pre-exposure is derived from.
struct CommittedExposure final
{
    double scale{1.0};
    double stops{0.0};

    [[nodiscard]] bool operator==(CommittedExposure const &) const noexcept = default;
};

// The persistent state of the feedback loop: one number, plus everything needed to decide whether that number is
// still meaningful.
struct AdaptationState final
{
    // The frame that produced this state. The exposure it holds is applied to frame producerFrameIndex + 1, which
    // is the whole of the one-frame delay written down.
    std::uint64_t producerFrameIndex{};
    double committedStops{};
    std::uint64_t configurationIdentity{};
    ExposureMode mode{ExposureMode::Automatic};
    bool valid{};

    [[nodiscard]] bool operator==(AdaptationState const &) const noexcept = default;
};

struct ExposureFrame final
{
    std::uint64_t frameIndex{};
    // The scale baked into every stored scene-linear sample of this frame.
    double preExposure{1.0};
    double frameDeltaSeconds{};
    bool cameraCut{};

    [[nodiscard]] bool operator==(ExposureFrame const &) const noexcept = default;
};

struct ExposureFrameFacts final
{
    std::uint64_t frameIndex{};
    double preExposure{1.0};
    double inversePreExposure{1.0};
    double frameDeltaSeconds{};
    bool isFirstFrame{};
    bool cameraCut{};

    [[nodiscard]] bool operator==(ExposureFrameFacts const &) const noexcept = default;
};

[[nodiscard]] std::expected<ExposureFrameFacts, ContractError> ValidateExposureFrame(
    ExposureFrame const &frame) noexcept;

struct ExposureUpdateInput final
{
    ExposureFrame frame{};
    AdaptationState history{};
    // The luminance this frame measured. Ignored, and not validated, in manual mode.
    double meteredLuminance{};

    [[nodiscard]] bool operator==(ExposureUpdateInput const &) const noexcept = default;
};

struct ExposureUpdate final
{
    // Applied to the frame the viewer sees now. On a normal frame it is exactly the previous frame's committed
    // value and owes nothing to this frame's measurement.
    //
    // It is a mistake to read displayed == committed as "this was a reset frame". The two coincide on a reset,
    // because a reset adopts the target immediately with alpha exactly one, but they also coincide whenever the
    // smoothing did not move: alpha is zero for a paused frame or a zero adaptation rate, and the blend is a no-op
    // whenever direction is Steady because the target already equals the current value. Rounding can collapse the
    // two as well once the exposure has settled to within an ulp of its target. The facts that diagnose a reset are
    // reusedHistory and resets, which say what happened rather than what the numbers happen to look like.
    DisplayedExposure displayed{};
    MeasuredTargetExposure target{};
    CommittedExposure committed{};
    AdaptationState nextState{};
    AdaptationReset resets{AdaptationReset::None};
    // The smoothing factor actually used. It is exactly one on a reset frame, because a reset has no history to
    // smooth from. When the direction is Steady the target and the current value are equal, so the delta is zero
    // and the rate cannot matter; the increase rate is the one reported in that case.
    double alpha{};
    AdaptationDirection direction{AdaptationDirection::Steady};
    double currentStops{};
    double targetStops{};
    double committedStops{};
    bool reusedHistory{};
    bool clampedToMinimum{};
    bool clampedToMaximum{};

    [[nodiscard]] bool operator==(ExposureUpdate const &) const noexcept = default;
};

// Runs one frame of the feedback loop: decides whether the stored state may be reused, turns the measurement into
// a target, smooths in the log2 exposure domain so that equal stop differences behave the same at every brightness,
// and reports every fact a diagnostic overlay needs.
[[nodiscard]] std::expected<ExposureUpdate, ContractError> UpdateAutoExposure(
    ExposureUpdateInput const &input, AutoExposureSettings const &settings) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// G. Pre-exposure integration and final application
// ---------------------------------------------------------------------------------------------------------------

// Pre-exposure exists so that stored radiance sits near one and a 16-bit float render target keeps its mantissa.
//
// MatchCommittedExposure stores radiance already multiplied by the exposure the frame will be displayed with, so a
// correctly exposed pixel is stored near middle grey whatever the scene's absolute scale is. Note the direction:
// the stored value is luminance times exposure, not luminance divided by it. Writing the reciprocal here is the
// classic bug, and it makes the storage scale move the wrong way in exactly the scenes where it matters.
//
// Fixed keeps a constant pre-exposure, which is simpler, reproducible frame to frame, and correct for a 32-bit
// target or a scene whose absolute scale is already near one.
enum class PreExposureMode : std::uint8_t
{
    MatchCommittedExposure = 0U,
    Fixed,
};

struct PreExposureSettings final
{
    PreExposureMode mode{PreExposureMode::MatchCommittedExposure};
    double fixedPreExposure{1.0};
    double minimumPreExposure{1.0e-6};
    double maximumPreExposure{1.0e6};

    [[nodiscard]] bool operator==(PreExposureSettings const &) const noexcept = default;
};

struct NextFramePreExposure final
{
    double preExposure{1.0};
    // preExposure / currentPreExposure. A history buffer stored under the current pre-exposure is brought into the
    // next frame's storage scale by multiplying by this, which is the only place a pre-exposure change may be
    // applied.
    double previousToNextScale{1.0};
    bool clampedToMinimum{};
    bool clampedToMaximum{};

    [[nodiscard]] bool operator==(NextFramePreExposure const &) const noexcept = default;
};

// Errors reject a committed exposure that is not a positive scale inside kMaximumExposureScale, a current
// pre-exposure outside the legal domain, bounds outside [kMinimumPreExposure, kMaximumPreExposure], and a fixed
// mode whose authored pre-exposure is out of domain. The pre-exposure it returns is always a value Chapter 30's
// ValidatePostFrame accepts, which is the point of mirroring that chapter's limit here.
[[nodiscard]] std::expected<NextFramePreExposure, ContractError> ComputeNextFramePreExposure(
    CommittedExposure committed, double currentPreExposure, PreExposureSettings const &settings) noexcept;

struct FinalExposureInput final
{
    Rgb preExposedSceneLinear{};
    double preExposure{1.0};
    DisplayedExposure exposure{};

    [[nodiscard]] bool operator==(FinalExposureInput const &) const noexcept = default;
};

struct FinalExposureResult final
{
    // The radiance the renderer meant, with the storage scale removed.
    Rgb absoluteSceneLinear{};
    // The tone curve's input: absolute radiance multiplied by the camera exposure exactly once.
    Rgb exposedSceneLinear{};
    double appliedExposureScale{};
    // exposure.scale / preExposure, the single net multiply from stored to exposed. It is published so that a test
    // can prove no helper applied the exposure a second time.
    double netScaleFromStored{};

    [[nodiscard]] bool operator==(FinalExposureResult const &) const noexcept = default;
};

// The only function in this chapter that applies a camera exposure, and it accepts only a DisplayedExposure. A
// MeasuredTargetExposure or a CommittedExposure will not compile here, which is what makes "applied exactly once,
// one frame late" a property of the code rather than a rule in a document.
//
// Both published colours are checked against kMaximumSceneLinearValue, not just the exposed one, because Chapter
// 30's ApplyExposure validates its recovered absolute radiance too. A stored sample and a pre-exposure that are
// each individually legal here can still recover an absolute radiance the compositing stage refuses, and this is
// where that is reported rather than three stages later.
[[nodiscard]] std::expected<FinalExposureResult, ContractError> ApplyDisplayExposure(
    FinalExposureInput const &input) noexcept;

} // namespace ch31::auto_exposure
