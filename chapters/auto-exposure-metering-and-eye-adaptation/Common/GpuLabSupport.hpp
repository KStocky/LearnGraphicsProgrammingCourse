#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AutoExposureContracts.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Chapter 31 paired GPU lab: the auto-exposure feedback loop as an explicit sequence of dispatches.
//
// What the two variants do, and why they are structured as a pair:
//
//   * The Starter renders the same analytic scene under a *declared* exposure. It stores pre-exposed scene-linear
//     radiance, removes the pre-exposure exactly once, applies the declared display exposure exactly once, tone
//     maps, encodes, and composites the UI. It runs no histogram, no meter, and no adaptation, and it does not
//     call a shared helper that quietly performs one. The evidence it publishes is what makes that claim checkable
//     rather than a promise: the executed stage list, an empty histogram, an empty meter record, and a displayed
//     exposure that is bit-identical for two scenes whose luminance differs by four orders of magnitude.
//   * The Solution adds the loop: a fixed-point log2 histogram built with integer atomics, a deterministic
//     fixed-order reduction for the two exact sufficient statistics, a meter stage that reduces the distribution
//     under a declared policy, a target exposure derived from a declared middle grey, and a temporal adaptation
//     step in log exposure that commits a value for the *next* frame.
//
// The one-frame delay is the point of the chapter, so it is a property of the pass graph rather than a comment.
// The exposure the compose pass applies is read out of the persistent history slot the *previous* frame wrote.
// This frame's measurement can only reach the image through the value the adaptation stage commits, which the next
// frame reads. The single exception is a declared reset, where there is no previous value to reuse and the loop
// seeds itself from this frame's target; every reset publishes the flags that say why it happened.
//
// This header owns the ABI, device setup, configuration validation, and the order the passes are submitted in. It
// deliberately owns none of the algorithms: the histogram, the reduction, the metering policies, the target
// exposure, and the adaptation step live in the learner-owned HLSL of each variant, because they are the lesson.

namespace ch31::auto_exposure::gpu
{

// Display bounds. They are small enough that a WARP frame is quick and large enough that a 128-bin histogram has
// several hundred samples per occupied bin.
inline constexpr std::uint32_t kMaximumWidth = 320U;
inline constexpr std::uint32_t kMaximumHeight = 192U;
inline constexpr std::uint32_t kHistogramGroupWidth = 8U;
inline constexpr std::uint32_t kHistogramGroupHeight = 8U;
inline constexpr std::uint32_t kHistogramGroupThreads = kHistogramGroupWidth * kHistogramGroupHeight;
inline constexpr std::uint32_t kMaximumTileCount =
    ((kMaximumWidth + kHistogramGroupWidth - 1U) / kHistogramGroupWidth) *
    ((kMaximumHeight + kHistogramGroupHeight - 1U) / kHistogramGroupHeight);

// The lab accumulates weight in 32-bit integers because every accumulator is bounded by the display extent above.
// The bound is asserted rather than assumed: a wider extent would need a wider accumulator, and silently wrapping
// one would produce a histogram that looks entirely plausible.
inline constexpr std::uint64_t kMaximumLabTotalWeight =
    static_cast<std::uint64_t>(kMaximumWidth) * kMaximumHeight * kWeightOne;
static_assert(kMaximumLabTotalWeight <= 0xFFFF'FFFFULL);
static_assert(kMaximumLabTotalWeight <= kMaximumTotalWeight);
// One tile contributes at most this much, which is what the group-shared accumulation is checked against.
inline constexpr std::uint32_t kMaximumTileWeight = kHistogramGroupThreads * kWeightOne;

// The contract refuses the last representable frame index because it has no successor. The lab carries the index
// in 32 bits, so it refuses the last 32-bit value for exactly the same reason.
inline constexpr std::uint32_t kMaximumLabFrameIndex = 0xFFFF'FFFEU;

// Mirrors Chapter 30's kMaximumSceneLinearValue through this chapter's own copy of it, so the radiance the lab
// stores is always a value the compositing seam accepts.
inline constexpr double kMaximumLabSceneLinearValue = kMaximumSceneLinearValue;

inline constexpr std::uint32_t kAbiMarker = 0x4145'3331U;

enum class LabVariant : std::uint8_t
{
    Starter,
    Solution,
};

// Which analytic picture the scene pass publishes. Every variant is a controlled luminance distribution whose
// answer under each metering policy is different, which is what makes a policy comparison an observation rather
// than an opinion.
enum class SceneVariant : std::uint32_t
{
    // Animated, with a declared cut frame. The picture the chapter teaches with.
    CameraLab = 0U,
    // One luminance everywhere. Every policy must agree, and the target must land exactly on middle grey.
    ConstantField = 1U,
    // A dark majority plus a declared number of extremely bright columns. The arithmetic mean chases the outlier,
    // the log average is dragged less, and a percentile window above the outlier fraction ignores it entirely.
    SplitField = 2U,
    // A bright centre rectangle inside a dark surround, so centre weighting changes the measured luminance.
    CentreEdgeField = 3U,
    // A distinct luminance inside the mask rectangle, so an artist mask changes the measured luminance.
    MaskedField = 4U,
    // Column x lands in bin x % binCount, at a declared fraction above that bin's lower edge. Nothing sits on a
    // bin centre, which is what makes a bin-centre reconstruction distinguishable from an exact statistic.
    LadderField = 5U,
    // Every sample is exactly zero. Metering must refuse it rather than report the darkest representable bin.
    BlackField = 6U,
    // A valid field with declared invalid probes in the first row: NaN, a negative channel, an infinity, a radiance
    // outside the declared domain, an exact zero, and below-range and above-range luminances.
    InvalidProbeField = 7U,
};

inline constexpr std::uint32_t kSceneVariantCount = static_cast<std::uint32_t>(SceneVariant::InvalidProbeField) + 1U;

// The metering mask. Both variants are defined on integer pixel comparisons and take values that are exact
// multiples of 1 / kWeightOne, so the CPU and the shader quantize them to the same integers and the mask identity
// the configuration declares is the identity the dispatch actually measured.
enum class MaskVariant : std::uint32_t
{
    FullFrame = 0U,
    CentreRectangle = 1U,
    // Half weight on the left half of the frame, full weight on the right. Exercises the fixed-point composition of
    // a mask with a centre weighting at a value that is not zero or one.
    HalfLeft = 2U,
};

inline constexpr std::uint32_t kMaskVariantCount = static_cast<std::uint32_t>(MaskVariant::HalfLeft) + 1U;

enum class TransferFunction : std::uint32_t
{
    Linear = 0U,
    Srgb = 1U,
    Gamma22 = 2U,
};

enum class DebugView : std::uint32_t
{
    Final = 0U,
    SceneLinearBaseline = 1U,
    AbsoluteLuminance = 2U,
    SampleWeights = 3U,
    HistogramOccupancy = 4U,
    PercentileSelection = 5U,
    MeteredLuminance = 6U,
    ExposureStops = 7U,
    AdaptationDirection = 8U,
    ClippingAndOutliers = 9U,
    ResetFacts = 10U,
    PreExposureAndNetScale = 11U,
    DisplayEncoded = 12U,
};

inline constexpr std::uint32_t kDebugViewCount = static_cast<std::uint32_t>(DebugView::DisplayEncoded) + 1U;

// The passes the renderer submits, in submission order. The word the shader stamps into every record is built from
// this list, so a test reads the order that actually ran rather than the order this comment claims.
enum class LabStage : std::uint32_t
{
    Clear = 0U,
    Scene = 1U,
    Histogram = 2U,
    Reduce = 3U,
    Meter = 4U,
    // The Starter's exposure stage: it declares the exposure it is about to apply and measures nothing.
    DeclaredExposure = 5U,
    // The Solution's exposure stage: it turns this frame's measurement into a value the *next* frame will display.
    Adapt = 6U,
    Compose = 7U,
};

inline constexpr std::uint32_t kMaximumStageCount = 7U;

// Per-pixel status bits, mirrored by AutoExposureShared.hlsli.
inline constexpr std::uint32_t kStatusScene = 1U << 0U;
inline constexpr std::uint32_t kStatusWeighted = 1U << 1U;
inline constexpr std::uint32_t kStatusClassified = 1U << 2U;
inline constexpr std::uint32_t kStatusPreExposureRemoved = 1U << 3U;
inline constexpr std::uint32_t kStatusExposure = 1U << 4U;
inline constexpr std::uint32_t kStatusToneMap = 1U << 5U;
inline constexpr std::uint32_t kStatusDisplayEncode = 1U << 6U;
inline constexpr std::uint32_t kStatusUi = 1U << 7U;
// The compose stage refused the stored sample: it was not finite, not non-negative, or outside the compositing
// seam. A refused pixel applies no exposure at all and is published as magenta, so a broken frame buffer is visible
// in the image instead of being tone mapped into something plausible.
inline constexpr std::uint32_t kStatusExposureRefused = 1U << 8U;

// Frame-level meter status bits. A refusal is a published fact, not a silently substituted number.
inline constexpr std::uint32_t kMeterEvaluated = 1U << 0U;
inline constexpr std::uint32_t kMeterNoAcceptedSamples = 1U << 1U;
inline constexpr std::uint32_t kMeterNoPositiveLuminance = 1U << 2U;
inline constexpr std::uint32_t kMeterDegenerateWindow = 1U << 3U;
inline constexpr std::uint32_t kMeterUsedExactStatistic = 1U << 4U;
inline constexpr std::uint32_t kMeterBinCentreBounded = 1U << 5U;

// Frame-level exposure status bits.
inline constexpr std::uint32_t kExposureEvaluated = 1U << 0U;
inline constexpr std::uint32_t kExposureReusedHistory = 1U << 1U;
inline constexpr std::uint32_t kExposureHeldOnRefusedMeasurement = 1U << 2U;
inline constexpr std::uint32_t kExposureSeededFromTarget = 1U << 3U;
inline constexpr std::uint32_t kExposureSeededFromDeclaredStops = 1U << 4U;
inline constexpr std::uint32_t kExposureManualMode = 1U << 5U;

// Accumulator overflow. Nothing is clamped: the frame reports that a bounded accumulator was exceeded so the
// readback is refused instead of quietly describing a histogram no dispatch produced.
inline constexpr std::uint32_t kOverflowTileWeight = 1U << 0U;
inline constexpr std::uint32_t kOverflowBinWeight = 1U << 1U;
inline constexpr std::uint32_t kOverflowTotalWeight = 1U << 2U;

struct LabConfiguration final
{
    DebugView debugView{DebugView::Final};
    SceneVariant sceneVariant{SceneVariant::CameraLab};
    MaskVariant maskVariant{MaskVariant::CentreRectangle};
    TransferFunction transferFunction{TransferFunction::Srgb};
    MeteringPolicy meteringPolicy{MeteringPolicy::LogAverage};
    ExposureMode exposureMode{ExposureMode::Automatic};
    PreExposureMode preExposureMode{PreExposureMode::MatchCommittedExposure};
    BlackSamplePolicy blackSamples{BlackSamplePolicy::CountInLowestBin};
    RangeSamplePolicy belowRange{RangeSamplePolicy::CountInEndBin};
    RangeSamplePolicy aboveRange{RangeSamplePolicy::CountInEndBin};

    std::uint32_t binCount{kDefaultBinCount};
    // The frame index the exposure loop reasons about. Tests drive it directly, which is what makes a repeated,
    // skipped, or rewound producer frame reproducible rather than a race.
    std::uint32_t frameIndex{};
    std::uint32_t animationFrame{};
    // Columns of SplitField that carry outlierLuminance, counted from the right edge.
    std::uint32_t outlierColumnCount{4U};

    float minimumLog2Luminance{static_cast<float>(kDefaultMinimumLog2Luminance)};
    float maximumLog2Luminance{static_cast<float>(kDefaultMaximumLog2Luminance)};
    float centreWeight{1.0F};
    float edgeWeight{0.0F};
    float centreFalloffPower{2.0F};
    float lowerPercentile{0.5F};
    float upperPercentile{0.95F};
    float middleGreyLuminance{0.18F};
    float exposureCompensationStops{0.0F};
    float minimumExposureStops{-16.0F};
    float maximumExposureStops{16.0F};
    float exposureIncreaseSpeedPerSecond{1.0F};
    float exposureDecreaseSpeedPerSecond{2.5F};
    float manualExposureStops{0.0F};
    // The exposure a reset frame adopts when this frame's measurement was refused, so a first frame over a black
    // scene still has a declared answer instead of an invented one.
    float resetSeedStops{0.0F};
    float initialPreExposure{1.0F};
    float fixedPreExposure{1.0F};
    float minimumPreExposure{1.0e-4F};
    float maximumPreExposure{1.0e4F};
    float frameDeltaSeconds{1.0F / 60.0F};

    float baseLuminance{0.18F};
    float outlierLuminance{4096.0F};
    float centreLuminance{2.0F};
    float edgeLuminance{0.02F};
    float maskedLuminance{1.0F};
    float backgroundLuminance{0.05F};
    // Where a LadderField sample sits inside its bin, as a fraction of the bin width above the lower edge.
    float ladderBinOffset{0.1F};
    float uiAlpha{0.5F};
    float uiColorR{0.85F};
    float uiColorG{0.85F};
    float uiColorB{0.20F};

    bool useCentreWeighting{false};
    bool useMask{false};
    bool cameraCut{false};
    bool uiEnabled{false};
    bool resetHistory{false};

    [[nodiscard]] bool operator==(LabConfiguration const &) const noexcept = default;
};

// Why the renderer had to discard the derived state it caches. Reported as flags so a test can change one field and
// observe exactly what that field invalidated.
//
// Every field of LabConfiguration is accounted for here, because a diagnostic that is silently incomplete is worse
// than no diagnostic at all. The classification is field-level rather than variant-level: a field that only some
// scene variant reads still reports its bit whenever it changes, so the rule can be stated without knowing which
// picture is selected. The fields that deliberately report nothing are:
//
//   * debugView, which selects which quantity the display pass draws and rebuilds nothing;
//   * frameIndex, frameDeltaSeconds and cameraCut, which are per-frame facts rather than configuration: the
//     adaptation resets they cause are published in ExposureRecord::resets, which is where a reset of the *stored
//     exposure* belongs;
//   * the coherent exposure settings the contract names as coherent - middleGreyLuminance,
//     exposureCompensationStops, minimumExposureStops, maximumExposureStops, the two adaptation speeds,
//     manualExposureStops and resetSeedStops - each of which moves the target or the rate without changing what a
//     stored exposure means;
//   * the pre-exposure settings preExposureMode, initialPreExposure, fixedPreExposure, minimumPreExposure and
//     maximumPreExposure, because the measurement is invariant under the storage scale and nothing cached depends
//     on it.
//
// All of those are asserted, not assumed: the invalidation table test mutates every field of the configuration and
// checks the exact bits, harmless fields included.
enum class InvalidationReason : std::uint32_t
{
    None = 0U,
    FirstFrame = 1U << 0U,
    ExplicitReset = 1U << 1U,
    ExtentChanged = 1U << 2U,
    HistogramLayoutChanged = 1U << 3U,
    WeightingChanged = 1U << 4U,
    MeteringPolicyChanged = 1U << 5U,
    ExposureModeChanged = 1U << 6U,
    SceneChanged = 1U << 7U,
    DisplayEncodingChanged = 1U << 8U,
};

[[nodiscard]] constexpr InvalidationReason operator|(InvalidationReason left, InvalidationReason right) noexcept
{
    return static_cast<InvalidationReason>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr InvalidationReason &operator|=(InvalidationReason &left, InvalidationReason right) noexcept
{
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool HasInvalidation(InvalidationReason reasons, InvalidationReason reason) noexcept
{
    return (static_cast<std::uint32_t>(reasons) & static_cast<std::uint32_t>(reason)) != 0U;
}

// One display pixel of byte-inspectable evidence. Every float precedes every integer so a finiteness sweep can read
// the whole float block at once, and every value before absoluteR is either stored radiance or a metering decision
// taken on it.
struct PixelRecord final
{
    float sceneR{};
    float sceneG{};
    float sceneB{};
    float storedLuminance{};
    float absoluteLuminance{};
    float log2Luminance{};
    float preExposure{};
    float maskValue{};
    float absoluteR{};
    float absoluteG{};
    float absoluteB{};
    float exposedR{};
    float exposedG{};
    float exposedB{};
    float toneMappedR{};
    float toneMappedG{};
    float toneMappedB{};
    float encodedR{};
    float encodedG{};
    float encodedB{};
    float uiR{};
    float uiG{};
    float uiB{};
    float uiAlpha{};
    float finalR{};
    float finalG{};
    float finalB{};
    float displayedExposureScale{};
    float displayedExposureStops{};
    // exposure.scale / preExposure, the single net multiply from stored to exposed. Published so a test can prove
    // no stage applied the exposure a second time.
    float netScaleFromStored{};
    float centreWeightUnit{};
    float sampleWeightUnit{};
    std::uint32_t centreWeight{};
    std::uint32_t maskWeight{};
    std::uint32_t sampleWeight{};
    std::uint32_t bin{};
    std::uint32_t classification{};
    std::uint32_t rejection{};
    std::uint32_t accepted{};
    std::uint32_t hasLog2Luminance{};
    std::uint32_t exposureApplicationCount{};
    std::uint32_t stageOrderWord{};
    std::uint32_t status{};
    std::uint32_t abiMarker{};

    [[nodiscard]] bool operator==(PixelRecord const &) const noexcept = default;
};
static_assert(sizeof(PixelRecord) == 176U);
static_assert(alignof(PixelRecord) == 4U);
static_assert(offsetof(PixelRecord, absoluteLuminance) == 16U);
static_assert(offsetof(PixelRecord, absoluteR) == 32U);
static_assert(offsetof(PixelRecord, toneMappedR) == 56U);
static_assert(offsetof(PixelRecord, finalR) == 96U);
static_assert(offsetof(PixelRecord, centreWeight) == 128U);
static_assert(offsetof(PixelRecord, abiMarker) == 172U);

struct SceneRecord final
{
    float radianceR{};
    float radianceG{};
    float radianceB{};
    float preExposure{};
    std::uint32_t sampleWeight{};
    std::uint32_t centreWeight{};
    std::uint32_t maskWeight{};
    std::uint32_t padding{};

    [[nodiscard]] bool operator==(SceneRecord const &) const noexcept = default;
};
static_assert(sizeof(SceneRecord) == 32U);

struct HistogramBin final
{
    std::uint32_t weight{};
    std::uint32_t sampleCount{};

    [[nodiscard]] bool operator==(HistogramBin const &) const noexcept = default;
};
static_assert(sizeof(HistogramBin) == 8U);

// One tile's contribution to the two real-valued sufficient statistics, summed inside the tile in thread-index
// order by a single thread. The final reduction visits tiles in tile-index order, so the whole accumulation has one
// declared order and no floating-point atomic anywhere.
struct TilePartial final
{
    float weightedLuminanceSum{};
    float weightedLog2LuminanceSum{};
    float minimumObservedLuminance{};
    float maximumObservedLuminance{};
    std::uint32_t hasAcceptedSample{};
    std::uint32_t log2AccumulatedWeight{};
    std::uint32_t acceptedWeight{};
    std::uint32_t acceptedSampleCount{};

    [[nodiscard]] bool operator==(TilePartial const &) const noexcept = default;
};
static_assert(sizeof(TilePartial) == 32U);

struct HistogramStatisticsRecord final
{
    std::uint32_t acceptedSampleCount{};
    std::uint32_t acceptedWeight{};
    std::uint32_t rejectedSampleCount{};
    std::uint32_t rejectedWeight{};
    std::uint32_t belowRangeSampleCount{};
    std::uint32_t belowRangeWeight{};
    std::uint32_t aboveRangeSampleCount{};
    std::uint32_t aboveRangeWeight{};
    std::uint32_t zeroLuminanceSampleCount{};
    std::uint32_t zeroLuminanceWeight{};
    std::uint32_t log2AccumulatedWeight{};
    std::uint32_t hasAcceptedSample{};
    // Rejection tallies in SampleRejection order, named individually because a structured-buffer array would put
    // the ABI at the mercy of a packing rule the chapter never states.
    std::uint32_t rejectedNone{};
    std::uint32_t rejectedNonFiniteChannel{};
    std::uint32_t rejectedNegativeChannel{};
    std::uint32_t rejectedLuminanceOutOfDomain{};
    std::uint32_t rejectedZeroWeight{};
    std::uint32_t rejectedZeroLuminance{};
    std::uint32_t rejectedBelowRange{};
    std::uint32_t rejectedAboveRange{};
    std::uint32_t tileCount{};
    std::uint32_t overflowFlags{};
    std::uint32_t abiMarker{};
    std::uint32_t padding{};
    float weightedLuminanceSum{};
    float weightedLog2LuminanceSum{};
    float minimumObservedLuminance{};
    float maximumObservedLuminance{};

    [[nodiscard]] bool operator==(HistogramStatisticsRecord const &) const noexcept = default;
};
static_assert(sizeof(HistogramStatisticsRecord) == 112U);

struct MeterRecord final
{
    std::uint32_t policy{};
    std::uint32_t status{};
    std::uint32_t firstWindowBin{};
    std::uint32_t lastWindowBin{};
    std::uint32_t windowWeight{};
    std::uint32_t lowerTargetWeight{};
    std::uint32_t upperTargetWeight{};
    std::uint32_t logAverageWeight{};
    std::uint32_t occupiedBinCount{};
    std::uint32_t lowestOccupiedBin{};
    std::uint32_t highestOccupiedBin{};
    std::uint32_t hasOccupiedBin{};
    std::uint32_t clippedIntoLowestBin{};
    std::uint32_t clippedIntoHighestBin{};
    std::uint32_t excludedZeroLuminanceFromLogAverage{};
    std::uint32_t abiMarker{};
    float meteredLuminance{};
    float meteredLog2Luminance{};
    // The same estimator evaluated only from bin centres. It is published beside the exact value precisely so a
    // test can prove the meter did not substitute one for the other.
    float binCentreLog2Luminance{};
    float binQuantizationLog2Bound{};
    float windowLowerLog2Luminance{};
    float windowUpperLog2Luminance{};
    float minimumObservedLuminance{};
    float maximumObservedLuminance{};

    [[nodiscard]] bool operator==(MeterRecord const &) const noexcept = default;
};
static_assert(sizeof(MeterRecord) == 96U);

struct ExposureRecord final
{
    std::uint32_t frameIndex{};
    std::uint32_t producerFrameIndex{};
    std::uint32_t resets{};
    std::uint32_t direction{};
    std::uint32_t status{};
    std::uint32_t mode{};
    std::uint32_t historyValid{};
    std::uint32_t reusedHistory{};
    std::uint32_t clampedToMinimum{};
    std::uint32_t clampedToMaximum{};
    std::uint32_t targetClampedToMinimum{};
    std::uint32_t targetClampedToMaximum{};
    std::uint32_t preExposureClampedToMinimum{};
    std::uint32_t preExposureClampedToMaximum{};
    std::uint32_t configurationIdentityLow{};
    std::uint32_t configurationIdentityHigh{};
    std::uint32_t historyIdentityLow{};
    std::uint32_t historyIdentityHigh{};
    std::uint32_t abiMarker{};
    std::uint32_t padding{};
    // The exposure applied to the image the viewer sees this frame. On a reused-history frame it is exactly the
    // value the previous frame committed and owes nothing to this frame's measurement.
    float displayedStops{};
    float displayedScale{};
    float targetStops{};
    float targetUnclampedStops{};
    float targetScale{};
    float exposedMeteredLuminance{};
    // The exposure the next frame will display. It is never applied to this frame.
    float committedStops{};
    float committedScale{};
    float currentStops{};
    float alpha{};
    float meteredLuminance{};
    float preExposure{};
    float nextPreExposure{};
    float previousToNextScale{};
    float netScaleFromStored{};
    float padding2{};

    [[nodiscard]] bool operator==(ExposureRecord const &) const noexcept = default;
};
static_assert(sizeof(ExposureRecord) == 144U);

// The persistent state of the feedback loop. Two slots are kept so that the frame writing next frame's value never
// writes the slot the current frame is reading, which makes the producer relationship legible in the resource set
// rather than only in the frame index arithmetic.
struct ExposureHistorySlot final
{
    std::uint32_t valid{};
    std::uint32_t producerFrameIndex{};
    std::uint32_t configurationIdentityLow{};
    std::uint32_t configurationIdentityHigh{};
    std::uint32_t mode{};
    std::uint32_t preExposureValid{};
    float committedStops{};
    float nextPreExposure{};

    [[nodiscard]] bool operator==(ExposureHistorySlot const &) const noexcept = default;
};
static_assert(sizeof(ExposureHistorySlot) == 32U);
inline constexpr std::uint32_t kHistorySlotCount = 2U;

struct FrameReadback final
{
    LabConfiguration configuration{};
    lgp::framework::Extent2D displaySize{};
    std::vector<PixelRecord> pixels{};
    std::vector<HistogramBin> bins{};
    HistogramStatisticsRecord statistics{};
    MeterRecord meter{};
    ExposureRecord exposure{};
    std::array<ExposureHistorySlot, kHistorySlotCount> history{};
    // The CPU-side mirrors of the three exposures the GPU published, in the contract's own types. They exist so a
    // test states its expectations in the vocabulary the contract defines and cannot hand a committed exposure to
    // something that only accepts a displayed one.
    DisplayedExposure displayed{};
    CommittedExposure committed{};
    MeteringConfiguration meteringConfiguration{};
    HistogramLayoutFacts layoutFacts{};
    std::uint64_t configurationIdentity{};
    std::uint64_t maskIdentity{};
    std::array<LabStage, kMaximumStageCount> executedStages{};
    std::uint32_t executedStageCount{};
    std::uint32_t stageOrderWord{};
    std::uint32_t expectedStatus{};
    std::uint32_t historyReadSlot{};
    std::uint32_t historyWriteSlot{};
    std::uint32_t tileCount{};
    InvalidationReason invalidation{InvalidationReason::None};
    std::uint32_t frameSlot{};
};

struct BufferBarrierState final
{
    D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};
    D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};
};

[[nodiscard]] lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration,
                                                              LabVariant variant);
// The configuration a variant starts from. The Starter owns no histogram, no meter, and no adaptation, so its
// defaults declare a manual exposure and a fixed pre-exposure, and its validation refuses to switch the loop on.
[[nodiscard]] LabConfiguration DefaultConfiguration(LabVariant variant) noexcept;

// The CPU reference inputs the renderer derives from a configuration. Tests reuse them so a contract replay is
// driven by the same numbers the dispatch received rather than by a second transcription of them.
[[nodiscard]] HistogramLayout MakeHistogramLayout(LabConfiguration const &configuration) noexcept;
[[nodiscard]] HistogramSettings MakeHistogramSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] CentreWeightSettings MakeCentreWeightSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] MeteringSettings MakeMeteringSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] TargetExposureSettings MakeTargetExposureSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] AdaptationSettings MakeAdaptationSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] PreExposureSettings MakePreExposureSettings(LabConfiguration const &configuration) noexcept;

// The analytic mask, evaluated exactly as the shader evaluates it: integer pixel comparisons and weights that are
// exact multiples of 1 / kWeightOne, so the two never disagree about a quantized weight.
[[nodiscard]] double MaskValueAt(LabConfiguration const &configuration, lgp::framework::Extent2D extent,
                                 std::uint32_t x, std::uint32_t y) noexcept;
[[nodiscard]] std::vector<double> BuildMaskImage(LabConfiguration const &configuration,
                                                 lgp::framework::Extent2D extent);
[[nodiscard]] std::expected<std::uint64_t, lgp::framework::Error> ComputeMaskIdentity(
    LabConfiguration const &configuration, lgp::framework::Extent2D extent);
[[nodiscard]] std::expected<MeteringConfiguration, lgp::framework::Error> MakeMeteringConfiguration(
    LabConfiguration const &configuration, lgp::framework::Extent2D extent);

// The stage sequence the renderer submits, in submission order, with the stages the variant does not own left out.
[[nodiscard]] std::uint32_t BuildExecutedStages(LabConfiguration const &configuration, LabVariant variant,
                                                std::span<LabStage> stages) noexcept;
// Four bits per submitted stage, holding the stage enumerator plus one so an unused nibble reads as zero.
[[nodiscard]] std::uint32_t EncodeStageOrder(std::span<LabStage const> stages) noexcept;
[[nodiscard]] std::uint32_t ExpectedStatus(LabConfiguration const &configuration, LabVariant variant) noexcept;
// Pure: the reasons a change from one configuration to another forces derived state to be rebuilt.
//
// explicitReset is the renderer's declared restart: configuration.resetHistory or a RequestReset. It is passed in
// rather than read from the configuration so that the two sources of a restart report the same bit.
[[nodiscard]] InvalidationReason ComputeInvalidation(LabConfiguration const &previous, LabConfiguration const &current,
                                                     bool firstFrame, bool extentChanged, bool explicitReset) noexcept;

class BufferResource final
{
  public:
    BufferResource() = default;
    BufferResource(BufferResource &&other) noexcept;
    BufferResource &operator=(BufferResource &&other) noexcept;
    BufferResource(BufferResource const &) = delete;
    BufferResource &operator=(BufferResource const &) = delete;
    ~BufferResource();

    [[nodiscard]] ID3D12Resource *Get() const noexcept
    {
        return resource_.Get();
    }
    [[nodiscard]] std::uint64_t size_in_bytes() const noexcept
    {
        return sizeInBytes_;
    }
    [[nodiscard]] std::byte const *mapped_data() const noexcept
    {
        return mappedData_;
    }
    [[nodiscard]] std::byte *mapped_data() noexcept
    {
        return mappedData_;
    }

  private:
    friend std::expected<BufferResource, lgp::framework::Error> CreateBuffer(ID3D12Device10 &, std::uint64_t,
                                                                             D3D12_HEAP_TYPE, D3D12_RESOURCE_FLAGS,
                                                                             std::wstring_view, bool);
    void Reset() noexcept;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::uint64_t sizeInBytes_{};
    std::byte *mappedData_{};
};

[[nodiscard]] std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
    ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
    std::wstring_view name, bool mapPersistently = false);

class RendererCore : public lgp::framework::IChapterRenderer
{
  public:
    RendererCore(std::filesystem::path shaderPath, LabVariant variant);
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void ConfigureHeadlessTest(LabConfiguration const &configuration) noexcept;
    // Overwrites one persistent history slot before the next headless frame reads it.
    //
    // The loop's own arithmetic can never store a committed exposure outside the contract's stop limit, because the
    // value it writes is clamped to authored bounds that validation already refused to place outside that limit. The
    // shader still checks, because a history buffer is persistent state that an aliasing bug or a partially executed
    // frame can corrupt, and a silently reused corrupt value would produce an exposure with no explanation. This
    // hook is how that defence is *observed* rather than assumed: it declares a slot in the same type the shader
    // reads, so a test can prove ResetInvalidHistoryValue is reported instead of trusting a comment. It is honoured
    // only for a headless renderer, and it is consumed by the next submitted frame. A frame that declares a reset
    // clears the history slots before reading them, so an injection into such a frame is discarded, exactly as a
    // genuinely corrupt slot would be.
    void InjectHistorySlotForTest(std::uint32_t slotIndex, ExposureHistorySlot const &slot) noexcept;
    void RequestReset() noexcept;
    [[nodiscard]] std::expected<FrameReadback, lgp::framework::Error> ReadBackOutputs();

  private:
    // Everything the frame produces is frame-slot-owned, because two frames in flight must not share a readback
    // staging buffer. The exposure history is the one exception and is deliberately sequence-owned: the value the
    // loop carries belongs to the frame sequence, not to a back buffer, and giving each back buffer its own copy
    // would silently halve the adaptation rate.
    struct FrameSlotResources final
    {
        BufferResource records{};
        BufferResource scene{};
        BufferResource histogram{};
        BufferResource partials{};
        BufferResource statistics{};
        BufferResource meter{};
        BufferResource exposure{};
        BufferResource recordsReadback{};
        BufferResource frameReadback{};
        lgp::framework::DescriptorAllocation descriptors{};
        bool initialized{};
    };

    struct PendingHistoryInjection final
    {
        std::uint32_t slotIndex{};
        ExposureHistorySlot slot{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateResources(lgp::framework::Extent2D size);
    void DestroyResources(lgp::framework::DeviceResources &deviceResources) noexcept;
    [[nodiscard]] LabConfiguration ActiveConfiguration() const noexcept;

    std::filesystem::path shaderPath_{};
    LabVariant variant_{};
    bool headless_{};
    bool hasRendered_{};
    bool forceReset_{true};
    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader clearShader_{};
    lgp::framework::CompiledShader sceneShader_{};
    lgp::framework::CompiledShader histogramShader_{};
    lgp::framework::CompiledShader reduceShader_{};
    lgp::framework::CompiledShader meterShader_{};
    lgp::framework::CompiledShader exposureShader_{};
    lgp::framework::CompiledShader composeShader_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> clearPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> scenePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> histogramPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> reducePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> meterPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> exposurePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> composePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
    std::vector<FrameSlotResources> frameSlots_{};
    BufferResource history_{};
    // Staging for InjectHistorySlotForTest. It exists only so the declared slot reaches the GPU through the same
    // copy the rest of the frame uses, rather than through a second path with its own state rules.
    BufferResource historyUpload_{};
    std::optional<PendingHistoryInjection> pendingHistoryInjection_{};
    bool historyInitialized_{};
    lgp::framework::Extent2D size_{};
    std::optional<LabConfiguration> headlessConfiguration_{};
    LabConfiguration interactiveConfiguration_{};
    LabConfiguration lastRenderedConfiguration_{};
    lgp::framework::Extent2D lastRenderedExtent_{};
    MeteringConfiguration lastMeteringConfiguration_{};
    HistogramLayoutFacts lastLayoutFacts_{};
    std::uint64_t lastConfigurationIdentity_{};
    std::uint64_t lastMaskIdentity_{};
    std::array<LabStage, kMaximumStageCount> lastStages_{};
    std::uint32_t lastStageCount_{};
    std::uint32_t lastStageOrderWord_{};
    std::uint32_t lastExpectedStatus_{};
    std::uint32_t lastHistoryReadSlot_{};
    std::uint32_t lastHistoryWriteSlot_{};
    std::uint32_t lastTileCount_{};
    InvalidationReason lastInvalidation_{InvalidationReason::None};
    std::uint32_t lastRenderedFrameSlot_{};
};

} // namespace ch31::auto_exposure::gpu
