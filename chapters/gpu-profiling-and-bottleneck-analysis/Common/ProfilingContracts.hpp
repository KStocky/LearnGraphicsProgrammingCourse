#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace ch19::gpu_profiling
{

// These contracts model the *reasoning* a profiler must perform on CPU-side timing and GPU timestamp
// data. They deliberately never fabricate hardware counter values: when evidence is missing or a counter
// is unavailable the contracts report that honestly instead of inventing a diagnosis.
enum class ContractError : std::uint8_t
{
    InvalidTimestampFrequency = 0U,
    NoGpuIntervals,
    IntervalEndBeforeBegin,
    CalibrationQueueMismatch,
    CalibrationTimestampOutOfRange,
    NonFiniteCpuTiming,
    NegativeCpuTiming,
    InvalidLimiterTolerance,
    InvalidIdleTolerance,
    ArithmeticOverflow,
    InvalidMinimumSampleCount,
    InvalidStabilityThreshold,
    EmptySampleSeries,
    InsufficientMeasuredSamples,
    MeasuredSampleCountOverflow,
    UnknownSampleMetric,
    UnequalWorkloadIdentity,
    UnequalOutputFingerprint,
    MismatchedSamplePolicy,
    MismatchedMeasuredSampleCount,
    MeasuredSampleCountBelowPolicy,
    InvalidNoiseThreshold,
    InvalidBaselineMedian,
    InvalidCandidateMedian,
    InvalidObservationStatistics,
    DuplicateEvidenceKind,
    UnknownHypothesis,
    EmptyPixEventName,
    PixEventNameTooLong,
    InvalidPixEventCharacter,
    PixRegionEndBeforeBegin,
    StrayPixScopeEnd,
    UnterminatedPixScope,
};

enum class QueueKind : std::uint8_t
{
    Graphics = 0U,
    Compute,
    Copy,
};

// ---------------------------------------------------------------------------
// 1. Cross-queue timestamp calibration.
//
// D3D12 timestamp counters are NOT directly comparable across queues: each queue has its own timestamp
// epoch and, on real hardware, may run at its own timestamp frequency. Aligning two queues therefore needs
// `ID3D12CommandQueue::GetClockCalibration`, which samples a GPU tick and the matching CPU/QPC tick at the
// same instant. Only after mapping every raw tick onto a single common CPU/QPC timeline can spans from
// different queues be unioned or overlapped. These types make that pipeline explicit and teachable.
// ---------------------------------------------------------------------------

// A raw GPU timestamp span measured on one queue, expressed in that queue's own timestamp domain. Raw ticks
// from two different queues are NOT comparable; they must be calibrated first.
struct RawTimestampInterval final
{
    QueueKind queue{QueueKind::Graphics};
    std::uint64_t rawBeginTick{};
    std::uint64_t rawEndTick{};

    [[nodiscard]] bool operator==(RawTimestampInterval const &) const noexcept = default;
};

// One `GetClockCalibration` sample for a single queue: a GPU tick paired with the CPU/QPC tick captured at
// the same instant, plus both clocks' frequencies. All four frequency/tick fields must be validated nonzero
// before use. Different queues may legitimately report different `gpuTimestampFrequencyHz`.
struct QueueClockCalibration final
{
    QueueKind queue{QueueKind::Graphics};
    std::uint64_t gpuCalibrationTick{};
    std::uint64_t qpcCalibrationTick{};
    std::uint64_t gpuTimestampFrequencyHz{};
    std::uint64_t qpcFrequencyHz{};

    [[nodiscard]] bool operator==(QueueClockCalibration const &) const noexcept = default;
};

// A GPU span already mapped onto the common CPU/QPC timeline. `beginQpcTick`/`endQpcTick` are counts of the
// single QPC clock shared by every queue, so two calibrated intervals are directly comparable.
struct CalibratedGpuInterval final
{
    QueueKind queue{QueueKind::Graphics};
    std::uint64_t beginQpcTick{};
    std::uint64_t endQpcTick{};

    [[nodiscard]] bool operator==(CalibratedGpuInterval const &) const noexcept = default;
};

// Pure conversion mapping a raw per-queue interval onto the common CPU/QPC timeline using one queue's clock
// calibration. It handles raw ticks recorded before or after the calibration sample (the GPU-tick delta is
// signed), performs the scale with full-width integer math to avoid overflow/underflow, and rounds
// conservatively so the calibrated span never understates real work: the begin edge rounds down and the end
// edge rounds up. Conversions that would land before QPC zero, overflow the QPC range, or scale beyond
// 64 bits are rejected rather than silently wrapped.
[[nodiscard]] std::expected<CalibratedGpuInterval, ContractError> CalibrateRawInterval(
    RawTimestampInterval const &interval, QueueClockCalibration const &calibration) noexcept;

// ---------------------------------------------------------------------------
// 2. Canonical measurement sample.
// ---------------------------------------------------------------------------

// One repeat of a measured workload. Every interval here is ALREADY calibrated onto the single common QPC
// domain described by `qpcFrequencyHz`; that shared domain is the invariant that makes cross-queue union and
// overlap math meaningful. Raw per-queue ticks must never be placed here directly.
struct MeasurementSample final
{
    // CPU wall-clock time spent recording this frame's command list (measured around command recording only).
    // It deliberately EXCLUDES the framework's BeginFrame fence wait and the EndFrame execute/signal cost, so
    // it is a directional screening input, not a complete CPU submission measurement.
    double cpuRecordingMilliseconds{};
    std::uint64_t qpcFrequencyHz{};
    std::vector<CalibratedGpuInterval> calibratedIntervals{};

    [[nodiscard]] bool operator==(MeasurementSample const &) const noexcept = default;
};

// ---------------------------------------------------------------------------
// 3. Per-sample derived metrics.
// ---------------------------------------------------------------------------

// A cautious screening heuristic, NOT a categorical verdict. It compares one frame's GPU busy-union against the
// CPU command-recording time only; that CPU figure EXCLUDES the framework's BeginFrame fence wait and the
// EndFrame execute/signal cost, so the comparison is a directional screening observation, never a complete CPU
// submission measurement. Because CPU submission and GPU execution pipeline across frames, this can only
// *suggest* a direction. When the GPU timeline is riddled with idle/serialization bubbles the comparison is
// meaningless, so the classification is forced to Indeterminate. Real synchronization diagnosis lives in the
// evidence model, not here.
enum class LimiterHeuristic : std::uint8_t
{
    LikelyCpuLimited = 0U,
    LikelyGpuLimited,
    Indeterminate,
};

struct QueueBusyMilliseconds final
{
    QueueKind queue{QueueKind::Graphics};
    double milliseconds{};

    [[nodiscard]] bool operator==(QueueBusyMilliseconds const &) const noexcept = default;
};

struct SampleMetrics final
{
    // CPU command-recording time only (excludes framework fence waits and EndFrame execute/signal cost).
    double cpuRecordingMilliseconds{};
    // Wall-clock span from the first interval begin to the last interval end. This INCLUDES idle gaps and is
    // therefore a timeline extent, not a critical path: it must never be read as GPU execution time.
    double gpuTimelineSpanMilliseconds{};
    // Time during which at least one queue was busy (concurrent queues counted once).
    double gpuBusyUnionMilliseconds{};
    // Actual wall-clock duration during which at least two distinct queues were simultaneously busy. Half-open
    // [begin, end) semantics: queues that merely touch at a tick boundary do not overlap.
    double gpuMultiQueueBusyMilliseconds{};
    // Timeline span with no queue busy.
    double gpuIdleMilliseconds{};
    std::vector<QueueBusyMilliseconds> perQueueBusyMilliseconds{};
    LimiterHeuristic limiterHeuristic{LimiterHeuristic::Indeterminate};

    [[nodiscard]] bool operator==(SampleMetrics const &) const noexcept = default;
};

inline constexpr double kDefaultLimiterToleranceRatio = 0.05;
// Above this fraction of idle time inside the GPU timeline span the limiter heuristic refuses to guess.
inline constexpr double kDefaultTimelineIdleToleranceRatio = 0.05;

[[nodiscard]] std::expected<SampleMetrics, ContractError> ComputeSampleMetrics(
    MeasurementSample const &sample, double limiterToleranceRatio = kDefaultLimiterToleranceRatio,
    double idleToleranceRatio = kDefaultTimelineIdleToleranceRatio);

// ---------------------------------------------------------------------------
// 4. Warm-up / repeat aggregation.
// ---------------------------------------------------------------------------

// Which scalar metric a series aggregation extracts from each sample.
enum class SampleMetric : std::uint8_t
{
    CpuRecordingMilliseconds = 0U,
    GpuTimelineSpanMilliseconds,
    GpuBusyUnionMilliseconds,
    GpuMultiQueueBusyMilliseconds,
};

struct SeriesPolicy final
{
    std::uint32_t warmupSampleCount{};
    std::uint32_t minimumMeasuredSamples{1U};
    double stabilityRelativeSpreadThreshold{0.1};

    [[nodiscard]] bool operator==(SeriesPolicy const &) const noexcept = default;
};

enum class Stability : std::uint8_t
{
    Stable = 0U,
    Unstable,
};

// Robust statistics over the measured (post-warm-up) samples. Median and MAD resist outliers, but MAD alone
// hides a single catastrophic spike, so `tailRelativeSpread` (the maximum absolute deviation from the median,
// relative to it) captures the tail. Stability is classified from the *larger* of the two spreads.
struct SeriesStatistics final
{
    std::uint32_t measuredSampleCount{};
    double median{};
    double medianAbsoluteDeviation{};
    double minimum{};
    double maximum{};
    double range{};
    // Robust central spread: MAD / |median|.
    double robustRelativeSpread{};
    // Tail spread: max_i |x_i - median| / |median|.
    double tailRelativeSpread{};
    Stability stability{Stability::Unstable};

    [[nodiscard]] bool operator==(SeriesStatistics const &) const noexcept = default;
};

[[nodiscard]] std::expected<SeriesStatistics, ContractError> AggregateSeries(
    SeriesPolicy const &policy, std::span<MeasurementSample const> samples, SampleMetric metric,
    double limiterToleranceRatio = kDefaultLimiterToleranceRatio,
    double idleToleranceRatio = kDefaultTimelineIdleToleranceRatio);

// ---------------------------------------------------------------------------
// 5. Controlled experiment comparison.
// ---------------------------------------------------------------------------

// A baseline or candidate run: its identity (workload + output fingerprint), the sampling policy used, and
// the resulting statistics. Comparisons fail closed unless identity and policy match, both observations are
// internally consistent, and the change clears the noise the observations themselves reveal.
struct ExperimentObservation final
{
    std::uint64_t workloadId{};
    std::uint64_t outputFingerprint{};
    SeriesPolicy policy{};
    SeriesStatistics statistics{};

    [[nodiscard]] bool operator==(ExperimentObservation const &) const noexcept = default;
};

enum class ExperimentOutcome : std::uint8_t
{
    Improvement = 0U,
    Regression,
    Inconclusive,
};

struct ExperimentComparison final
{
    double baselineMedian{};
    double candidateMedian{};
    double relativeChange{};
    // The threshold actually applied: max(user noise threshold, baseline observed spread, candidate observed
    // spread). A delta smaller than the noise the runs themselves exhibit is never called significant.
    double effectiveThreshold{};
    ExperimentOutcome outcome{ExperimentOutcome::Inconclusive};

    [[nodiscard]] bool operator==(ExperimentComparison const &) const noexcept = default;
};

inline constexpr double kDefaultExperimentNoiseThreshold = 0.02;

[[nodiscard]] std::expected<ExperimentComparison, ContractError> CompareExperiment(
    ExperimentObservation const &baseline, ExperimentObservation const &candidate,
    double noiseRelativeThreshold = kDefaultExperimentNoiseThreshold);

// ---------------------------------------------------------------------------
// 6. Evidence and bottleneck hypotheses.
// ---------------------------------------------------------------------------

enum class BottleneckHypothesis : std::uint8_t
{
    CpuSubmission = 0U,
    SynchronizationSerialization,
    ArithmeticThroughput,
    TextureCache,
    MemoryBandwidthLatency,
    OccupancyLatencyHiding,
    FixedFunction,
    InsufficientWork,
};

enum class EvidenceKind : std::uint8_t
{
    CpuSubmissionTiming = 0U,
    QueueTimeline,
    GpuTimestampSpan,
    ArithmeticUtilizationCounter,
    TextureCacheCounter,
    MemoryBandwidthCounter,
    OccupancyCounter,
    FixedFunctionCounter,
    WorkAmount,
    CompilerResourceReport,
    // A controlled A/B experiment whose two variants produce byte-identical output, isolating the change under
    // test. A single utilization counter can never establish causation on its own; this is the corroboration.
    EqualOutputControlledExperiment,
};

enum class EvidenceAvailability : std::uint8_t
{
    Measured = 0U,
    CounterUnavailable,
    NotCollected,
};

// A single observed piece of evidence. `supportsHypothesis` records the analyst's *observation* (does the
// measured value point toward the hypothesis) and is kept separate from availability so that causation is
// never inferred from a mere presence of data.
struct EvidenceRecord final
{
    EvidenceKind kind{EvidenceKind::CpuSubmissionTiming};
    EvidenceAvailability availability{EvidenceAvailability::NotCollected};
    bool supportsHypothesis{false};

    [[nodiscard]] bool operator==(EvidenceRecord const &) const noexcept = default;
};

enum class HypothesisConclusion : std::uint8_t
{
    Supported = 0U,
    NotSupported,
    NeedsEvidence,
    CounterUnavailable,
};

struct HypothesisAssessment final
{
    BottleneckHypothesis hypothesis{BottleneckHypothesis::CpuSubmission};
    HypothesisConclusion conclusion{HypothesisConclusion::NeedsEvidence};
    std::vector<EvidenceKind> missingEvidence{};
    std::vector<EvidenceKind> unavailableCounters{};

    [[nodiscard]] bool operator==(HypothesisAssessment const &) const noexcept = default;
};

// The evidence combination a hypothesis requires before it can be Supported or NotSupported. Every hypothesis
// requires an equal-output controlled experiment: no diagnosis is Supported from one utilization counter.
[[nodiscard]] std::span<EvidenceKind const> RequiredEvidence(BottleneckHypothesis hypothesis) noexcept;

[[nodiscard]] std::expected<HypothesisAssessment, ContractError> ClassifyHypothesis(
    BottleneckHypothesis hypothesis, std::span<EvidenceRecord const> evidence);

enum class NextExperimentReason : std::uint8_t
{
    CollectMissingEvidence = 0U,
    FindAlternativeForUnavailableCounter,
};

struct NextExperiment final
{
    EvidenceKind evidence{EvidenceKind::CpuSubmissionTiming};
    NextExperimentReason reason{NextExperimentReason::CollectMissingEvidence};
    std::uint32_t priority{};

    [[nodiscard]] bool operator==(NextExperiment const &) const noexcept = default;
};

// Deterministic, transparent ranking of the experiments still needed to resolve a hypothesis. Missing
// evidence is ranked ahead of unavailable counters; ordering follows the hypothesis' required-evidence list.
[[nodiscard]] std::vector<NextExperiment> SuggestNextExperiments(HypothesisAssessment const &assessment);

// ---------------------------------------------------------------------------
// 7. PIX event validation.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaximumPixEventNameLength = 255U;

// The printable-ASCII restriction below is a deliberate *course* naming policy for legible, portable markers;
// the real PIX API accepts wide strings and does not impose this.
[[nodiscard]] std::expected<void, ContractError> ValidatePixEventName(std::string_view name) noexcept;

enum class PixScopeMarkerKind : std::uint8_t
{
    Begin = 0U,
    End,
};

// A PIX scope marker. Names live on Begin markers; like `PIXEndEvent`, an End marker is purely positional and
// closes the innermost open scope, so its `name` is expected to be empty and is never required to match.
struct PixScopeMarker final
{
    PixScopeMarkerKind kind{PixScopeMarkerKind::Begin};
    std::string_view name{};

    [[nodiscard]] bool operator==(PixScopeMarker const &) const noexcept = default;
};

// Validates that begin/end scope markers nest as a balanced tree and returns the maximum depth reached. A
// End with no open scope is a stray end; any scope still open at the end is unterminated.
[[nodiscard]] std::expected<std::uint32_t, ContractError> ValidatePixScopes(std::span<PixScopeMarker const> markers);

struct PixEventRegion final
{
    std::string_view name{};
    std::uint64_t beginTick{};
    std::uint64_t endTick{};

    [[nodiscard]] bool operator==(PixEventRegion const &) const noexcept = default;
};

[[nodiscard]] std::expected<void, ContractError> ValidatePixEventRegion(PixEventRegion const &region) noexcept;

} // namespace ch19::gpu_profiling
