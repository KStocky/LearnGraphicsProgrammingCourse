#include "ProfilingContracts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <utility>

namespace ch19::gpu_profiling
{
namespace
{

using TickSpan = std::pair<std::uint64_t, std::uint64_t>;

[[nodiscard]] std::expected<std::uint64_t, ContractError> CheckedAdd(std::uint64_t const left,
                                                                     std::uint64_t const right) noexcept
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return left + right;
}

// A 128-bit unsigned value split into high and low 64-bit halves.
struct UInt128 final
{
    std::uint64_t high{};
    std::uint64_t low{};
};

// Exact 128-bit product of two 64-bit values via 32-bit schoolbook multiplication. This is portable and
// deterministic on every compiler, so calibration math never depends on `__int128` (which clang-cl lowers to
// a compiler-rt helper that is not linked here) or MSVC-only intrinsics.
[[nodiscard]] UInt128 WideMultiply(std::uint64_t const left, std::uint64_t const right) noexcept
{
    std::uint64_t const leftLow = left & 0xFFFF'FFFFU;
    std::uint64_t const leftHigh = left >> 32U;
    std::uint64_t const rightLow = right & 0xFFFF'FFFFU;
    std::uint64_t const rightHigh = right >> 32U;

    std::uint64_t const lowLow = leftLow * rightLow;
    std::uint64_t const lowHigh = leftLow * rightHigh;
    std::uint64_t const highLow = leftHigh * rightLow;
    std::uint64_t const highHigh = leftHigh * rightHigh;

    std::uint64_t const middle = (lowLow >> 32U) + (lowHigh & 0xFFFF'FFFFU) + (highLow & 0xFFFF'FFFFU);
    std::uint64_t const low = (lowLow & 0xFFFF'FFFFU) | (middle << 32U);
    std::uint64_t const high = highHigh + (lowHigh >> 32U) + (highLow >> 32U) + (middle >> 32U);
    return UInt128{.high = high, .low = low};
}

// floor(value * multiplier / divisor) computed through the exact 128-bit product, plus whether that product
// carried a nonzero remainder (needed to round the calibrated end edge up). `divisor` must be nonzero.
// Overflow of the 64-bit quotient is reported rather than wrapped.
struct WideQuotient final
{
    std::uint64_t quotient{};
    bool hasRemainder{};
};

[[nodiscard]] std::expected<WideQuotient, ContractError> MultiplyThenDivide(std::uint64_t const value,
                                                                            std::uint64_t const multiplier,
                                                                            std::uint64_t const divisor) noexcept
{
    UInt128 const product = WideMultiply(value, multiplier);
    // A high half that is not below the divisor means the quotient needs more than 64 bits.
    if (product.high >= divisor)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }

    // Binary long division of the 128-bit product by the 64-bit divisor. The running remainder stays strictly
    // below the divisor, so it always fits in 64 bits; the saved top bit handles the shift that would
    // otherwise overflow when the divisor exceeds 2^63.
    std::uint64_t quotient = 0U;
    std::uint64_t remainder = product.high;
    for (int bit = 63; bit >= 0; --bit)
    {
        std::uint64_t const nextBit = (product.low >> static_cast<unsigned>(bit)) & 1U;
        std::uint64_t const remainderTopBit = remainder >> 63U;
        remainder = (remainder << 1U) | nextBit;
        if (remainderTopBit != 0U || remainder >= divisor)
        {
            remainder -= divisor;
            quotient |= (std::uint64_t{1U} << static_cast<unsigned>(bit));
        }
    }
    return WideQuotient{.quotient = quotient, .hasRemainder = remainder != 0U};
}

enum class RoundingDirection : std::uint8_t
{
    Down = 0U,
    Up,
};

// Maps one GPU tick onto the common QPC timeline:
//   qpc = qpcCalibrationTick + (gpuTick - gpuCalibrationTick) * qpcFrequencyHz / gpuTimestampFrequencyHz
// The GPU delta is signed (ticks may predate the calibration sample). The exact rational result is rounded
// toward -infinity (Down) or +infinity (Up); results below QPC zero or above the QPC range are rejected.
[[nodiscard]] std::expected<std::uint64_t, ContractError> ConvertGpuTickToQpc(std::uint64_t const gpuTick,
                                                                              QueueClockCalibration const &calibration,
                                                                              RoundingDirection const rounding) noexcept
{
    bool const before = gpuTick < calibration.gpuCalibrationTick;
    std::uint64_t const magnitude =
        before ? calibration.gpuCalibrationTick - gpuTick : gpuTick - calibration.gpuCalibrationTick;

    auto const scaled = MultiplyThenDivide(magnitude, calibration.qpcFrequencyHz, calibration.gpuTimestampFrequencyHz);
    if (!scaled)
    {
        return std::unexpected(scaled.error());
    }
    std::uint64_t const whole = scaled->quotient;
    bool const hasFraction = scaled->hasRemainder;

    if (!before)
    {
        // qpc = qpcCalibrationTick + whole + fraction; round the fraction away only when rounding Up.
        auto const base = CheckedAdd(calibration.qpcCalibrationTick, whole);
        if (!base)
        {
            return std::unexpected(ContractError::CalibrationTimestampOutOfRange);
        }
        if (rounding == RoundingDirection::Up && hasFraction)
        {
            auto const rounded = CheckedAdd(*base, 1U);
            if (!rounded)
            {
                return std::unexpected(ContractError::CalibrationTimestampOutOfRange);
            }
            return *rounded;
        }
        return *base;
    }

    // qpc = qpcCalibrationTick - whole - fraction. Rounding Down borrows an extra tick when a fraction exists;
    // rounding Up (ceil of a negative offset) discards it.
    std::uint64_t subtrahend = whole;
    if (rounding == RoundingDirection::Down && hasFraction)
    {
        if (subtrahend == std::numeric_limits<std::uint64_t>::max())
        {
            return std::unexpected(ContractError::CalibrationTimestampOutOfRange);
        }
        subtrahend += 1U;
    }
    if (subtrahend > calibration.qpcCalibrationTick)
    {
        return std::unexpected(ContractError::CalibrationTimestampOutOfRange);
    }
    return calibration.qpcCalibrationTick - subtrahend;
}

// Merges a queue's spans into disjoint, ascending intervals. Assumes each span already has end >= begin.
[[nodiscard]] std::vector<TickSpan> MergeSpans(std::vector<TickSpan> spans)
{
    std::vector<TickSpan> merged;
    if (spans.empty())
    {
        return merged;
    }
    std::sort(spans.begin(), spans.end());
    std::uint64_t currentBegin = spans.front().first;
    std::uint64_t currentEnd = spans.front().second;
    for (std::size_t index = 1U; index < spans.size(); ++index)
    {
        auto const [begin, end] = spans[index];
        if (begin > currentEnd)
        {
            merged.emplace_back(currentBegin, currentEnd);
            currentBegin = begin;
            currentEnd = end;
        }
        else
        {
            currentEnd = std::max(currentEnd, end);
        }
    }
    merged.emplace_back(currentBegin, currentEnd);
    return merged;
}

[[nodiscard]] std::expected<std::uint64_t, ContractError> SumDisjointSpanTicks(std::vector<TickSpan> const &merged)
{
    std::uint64_t total = 0U;
    for (auto const &[begin, end] : merged)
    {
        auto const extended = CheckedAdd(total, end - begin);
        if (!extended)
        {
            return std::unexpected(extended.error());
        }
        total = *extended;
    }
    return total;
}

// Wall-clock ticks during which at least two distinct queues are simultaneously busy. Each queue's spans are
// merged first (so one queue can contribute at most one covering interval at any instant); a sweep over the
// combined half-open [begin, end) events then credits only segments whose coverage is two or more queues.
[[nodiscard]] std::expected<std::uint64_t, ContractError> MultiQueueBusyTicks(
    std::vector<TickSpan> const &mergedAllQueues)
{
    struct Event final
    {
        std::uint64_t tick{};
        int delta{};
    };
    std::vector<Event> events;
    events.reserve(mergedAllQueues.size() * 2U);
    for (auto const &[begin, end] : mergedAllQueues)
    {
        events.push_back({.tick = begin, .delta = 1});
        events.push_back({.tick = end, .delta = -1});
    }
    if (events.empty())
    {
        return std::uint64_t{0U};
    }
    std::sort(events.begin(), events.end(),
              [](Event const &left, Event const &right) noexcept { return left.tick < right.tick; });

    std::uint64_t total = 0U;
    std::uint64_t previousTick = events.front().tick;
    int active = 0;
    std::size_t index = 0U;
    while (index < events.size())
    {
        std::uint64_t const tick = events[index].tick;
        if (active >= 2)
        {
            auto const extended = CheckedAdd(total, tick - previousTick);
            if (!extended)
            {
                return std::unexpected(extended.error());
            }
            total = *extended;
        }
        while (index < events.size() && events[index].tick == tick)
        {
            active += events[index].delta;
            ++index;
        }
        previousTick = tick;
    }
    return total;
}

// ticks -> milliseconds. Uses `double` throughout: on MSVC `long double` is identical to `double`, so no
// extended precision is available and the result is simply the nearest double, not an exact value. `frequency`
// must be nonzero (validated by every caller).
[[nodiscard]] double TicksToMilliseconds(std::uint64_t const ticks, std::uint64_t const frequency) noexcept
{
    return (static_cast<double>(ticks) / static_cast<double>(frequency)) * 1000.0;
}

[[nodiscard]] bool IsFiniteNonNegative(double const value) noexcept
{
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] std::expected<void, ContractError> ValidateCpuTiming(double const value) noexcept
{
    if (!std::isfinite(value))
    {
        return std::unexpected(ContractError::NonFiniteCpuTiming);
    }
    if (value < 0.0)
    {
        return std::unexpected(ContractError::NegativeCpuTiming);
    }
    return {};
}

[[nodiscard]] double MedianOfSorted(std::vector<double> const &sorted) noexcept
{
    std::size_t const count = sorted.size();
    if ((count % 2U) == 1U)
    {
        return sorted[count / 2U];
    }
    // std::midpoint avoids the overflow of (a + b) * 0.5 when both samples are large.
    return std::midpoint(sorted[(count / 2U) - 1U], sorted[count / 2U]);
}

[[nodiscard]] std::expected<double, ContractError> SelectMetric(SampleMetrics const &metrics,
                                                                SampleMetric const metric) noexcept
{
    switch (metric)
    {
    case SampleMetric::CpuRecordingMilliseconds:
        return metrics.cpuRecordingMilliseconds;
    case SampleMetric::GpuTimelineSpanMilliseconds:
        return metrics.gpuTimelineSpanMilliseconds;
    case SampleMetric::GpuBusyUnionMilliseconds:
        return metrics.gpuBusyUnionMilliseconds;
    case SampleMetric::GpuMultiQueueBusyMilliseconds:
        return metrics.gpuMultiQueueBusyMilliseconds;
    }
    // Fail closed: an out-of-range enum never silently resolves to a default metric.
    return std::unexpected(ContractError::UnknownSampleMetric);
}

constexpr std::array<QueueKind, 3U> kQueueOrder{QueueKind::Graphics, QueueKind::Compute, QueueKind::Copy};

} // namespace

std::expected<CalibratedGpuInterval, ContractError> CalibrateRawInterval(
    RawTimestampInterval const &interval, QueueClockCalibration const &calibration) noexcept
{
    if (interval.queue != calibration.queue)
    {
        return std::unexpected(ContractError::CalibrationQueueMismatch);
    }
    if (calibration.gpuTimestampFrequencyHz == 0U || calibration.qpcFrequencyHz == 0U)
    {
        return std::unexpected(ContractError::InvalidTimestampFrequency);
    }
    if (interval.rawEndTick < interval.rawBeginTick)
    {
        return std::unexpected(ContractError::IntervalEndBeforeBegin);
    }

    auto const beginQpc = ConvertGpuTickToQpc(interval.rawBeginTick, calibration, RoundingDirection::Down);
    if (!beginQpc)
    {
        return std::unexpected(beginQpc.error());
    }
    auto const endQpc = ConvertGpuTickToQpc(interval.rawEndTick, calibration, RoundingDirection::Up);
    if (!endQpc)
    {
        return std::unexpected(endQpc.error());
    }
    return CalibratedGpuInterval{.queue = interval.queue, .beginQpcTick = *beginQpc, .endQpcTick = *endQpc};
}

std::expected<SampleMetrics, ContractError> ComputeSampleMetrics(MeasurementSample const &sample,
                                                                 double const limiterToleranceRatio,
                                                                 double const idleToleranceRatio)
{
    if (!IsFiniteNonNegative(limiterToleranceRatio))
    {
        return std::unexpected(ContractError::InvalidLimiterTolerance);
    }
    if (!IsFiniteNonNegative(idleToleranceRatio))
    {
        return std::unexpected(ContractError::InvalidIdleTolerance);
    }
    if (auto const cpuRecording = ValidateCpuTiming(sample.cpuRecordingMilliseconds); !cpuRecording)
    {
        return std::unexpected(cpuRecording.error());
    }
    if (sample.qpcFrequencyHz == 0U)
    {
        return std::unexpected(ContractError::InvalidTimestampFrequency);
    }
    if (sample.calibratedIntervals.empty())
    {
        return std::unexpected(ContractError::NoGpuIntervals);
    }
    for (CalibratedGpuInterval const &interval : sample.calibratedIntervals)
    {
        if (interval.endQpcTick < interval.beginQpcTick)
        {
            return std::unexpected(ContractError::IntervalEndBeforeBegin);
        }
    }

    std::uint64_t minimumBegin = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maximumEnd = 0U;
    std::vector<TickSpan> allSpans;
    allSpans.reserve(sample.calibratedIntervals.size());
    for (CalibratedGpuInterval const &interval : sample.calibratedIntervals)
    {
        minimumBegin = std::min(minimumBegin, interval.beginQpcTick);
        maximumEnd = std::max(maximumEnd, interval.endQpcTick);
        allSpans.emplace_back(interval.beginQpcTick, interval.endQpcTick);
    }

    auto const busyUnionMerged = MergeSpans(std::move(allSpans));
    auto const busyUnionTicks = SumDisjointSpanTicks(busyUnionMerged);
    if (!busyUnionTicks)
    {
        return std::unexpected(busyUnionTicks.error());
    }
    std::uint64_t const spanTicks = maximumEnd - minimumBegin;
    std::uint64_t const idleTicks = spanTicks - *busyUnionTicks;

    SampleMetrics metrics{};
    metrics.cpuRecordingMilliseconds = sample.cpuRecordingMilliseconds;

    std::vector<TickSpan> mergedAcrossQueues;
    for (QueueKind const queue : kQueueOrder)
    {
        std::vector<TickSpan> queueSpans;
        for (CalibratedGpuInterval const &interval : sample.calibratedIntervals)
        {
            if (interval.queue == queue)
            {
                queueSpans.emplace_back(interval.beginQpcTick, interval.endQpcTick);
            }
        }
        if (queueSpans.empty())
        {
            continue;
        }
        auto const queueMerged = MergeSpans(std::move(queueSpans));
        auto const queueBusyTicks = SumDisjointSpanTicks(queueMerged);
        if (!queueBusyTicks)
        {
            return std::unexpected(queueBusyTicks.error());
        }
        mergedAcrossQueues.insert(mergedAcrossQueues.end(), queueMerged.begin(), queueMerged.end());
        metrics.perQueueBusyMilliseconds.push_back(
            {.queue = queue, .milliseconds = TicksToMilliseconds(*queueBusyTicks, sample.qpcFrequencyHz)});
    }

    auto const multiQueueTicks = MultiQueueBusyTicks(mergedAcrossQueues);
    if (!multiQueueTicks)
    {
        return std::unexpected(multiQueueTicks.error());
    }

    metrics.gpuTimelineSpanMilliseconds = TicksToMilliseconds(spanTicks, sample.qpcFrequencyHz);
    metrics.gpuBusyUnionMilliseconds = TicksToMilliseconds(*busyUnionTicks, sample.qpcFrequencyHz);
    metrics.gpuMultiQueueBusyMilliseconds = TicksToMilliseconds(*multiQueueTicks, sample.qpcFrequencyHz);
    metrics.gpuIdleMilliseconds = TicksToMilliseconds(idleTicks, sample.qpcFrequencyHz);

    // Screening heuristic only. Compare the CPU command-RECORDING time (which excludes the framework's
    // BeginFrame fence wait and the EndFrame execute/signal cost, so it is a directional screening input, not a
    // complete CPU submission measurement) against the GPU BUSY UNION (never the idle-inflated span). When the
    // timeline is peppered with idle/serialization bubbles above tolerance the comparison cannot separate a
    // limit from pipelining, so we refuse to guess.
    double const idleFraction = spanTicks > 0U ? static_cast<double>(idleTicks) / static_cast<double>(spanTicks) : 0.0;
    double const cpu = metrics.cpuRecordingMilliseconds;
    double const busy = metrics.gpuBusyUnionMilliseconds;
    if (idleFraction <= idleToleranceRatio)
    {
        if (cpu > busy * (1.0 + limiterToleranceRatio))
        {
            metrics.limiterHeuristic = LimiterHeuristic::LikelyCpuLimited;
        }
        else if (busy > cpu * (1.0 + limiterToleranceRatio))
        {
            metrics.limiterHeuristic = LimiterHeuristic::LikelyGpuLimited;
        }
    }

    return metrics;
}

std::expected<SeriesStatistics, ContractError> AggregateSeries(SeriesPolicy const &policy,
                                                               std::span<MeasurementSample const> samples,
                                                               SampleMetric const metric,
                                                               double const limiterToleranceRatio,
                                                               double const idleToleranceRatio)
{
    if (policy.minimumMeasuredSamples == 0U)
    {
        return std::unexpected(ContractError::InvalidMinimumSampleCount);
    }
    if (!IsFiniteNonNegative(policy.stabilityRelativeSpreadThreshold))
    {
        return std::unexpected(ContractError::InvalidStabilityThreshold);
    }
    if (samples.empty())
    {
        return std::unexpected(ContractError::EmptySampleSeries);
    }
    if (samples.size() <= policy.warmupSampleCount ||
        (samples.size() - policy.warmupSampleCount) < policy.minimumMeasuredSamples)
    {
        return std::unexpected(ContractError::InsufficientMeasuredSamples);
    }

    std::vector<double> values;
    values.reserve(samples.size() - policy.warmupSampleCount);
    for (std::size_t index = 0U; index < samples.size(); ++index)
    {
        auto const metrics = ComputeSampleMetrics(samples[index], limiterToleranceRatio, idleToleranceRatio);
        if (!metrics)
        {
            return std::unexpected(metrics.error());
        }
        if (index < policy.warmupSampleCount)
        {
            continue;
        }
        auto const value = SelectMetric(*metrics, metric);
        if (!value)
        {
            return std::unexpected(value.error());
        }
        values.push_back(*value);
    }

    if (values.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected(ContractError::MeasuredSampleCountOverflow);
    }

    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    double const median = MedianOfSorted(sorted);

    std::vector<double> deviations;
    deviations.reserve(sorted.size());
    double maximumAbsoluteDeviation = 0.0;
    for (double const value : sorted)
    {
        double const deviation = std::abs(value - median);
        deviations.push_back(deviation);
        maximumAbsoluteDeviation = std::max(maximumAbsoluteDeviation, deviation);
    }
    std::sort(deviations.begin(), deviations.end());
    double const medianAbsoluteDeviation = MedianOfSorted(deviations);

    double const minimum = sorted.front();
    double const maximum = sorted.back();
    double const range = maximum - minimum;

    // Median zero is handled explicitly: relative spreads are zero when every deviation is zero, and infinite
    // otherwise, rather than dividing by zero into NaN.
    double robustRelativeSpread = 0.0;
    double tailRelativeSpread = 0.0;
    double const absoluteMedian = std::abs(median);
    if (absoluteMedian > 0.0)
    {
        robustRelativeSpread = medianAbsoluteDeviation / absoluteMedian;
        tailRelativeSpread = maximumAbsoluteDeviation / absoluteMedian;
    }
    else
    {
        robustRelativeSpread = medianAbsoluteDeviation > 0.0 ? std::numeric_limits<double>::infinity() : 0.0;
        tailRelativeSpread = maximumAbsoluteDeviation > 0.0 ? std::numeric_limits<double>::infinity() : 0.0;
    }
    // MAD hides a lone catastrophic spike; classify from the larger of robust and tail spread.
    double const classifyingSpread = std::max(robustRelativeSpread, tailRelativeSpread);

    SeriesStatistics statistics{};
    statistics.measuredSampleCount = static_cast<std::uint32_t>(values.size());
    statistics.median = median;
    statistics.medianAbsoluteDeviation = medianAbsoluteDeviation;
    statistics.minimum = minimum;
    statistics.maximum = maximum;
    statistics.range = range;
    statistics.robustRelativeSpread = robustRelativeSpread;
    statistics.tailRelativeSpread = tailRelativeSpread;
    statistics.stability =
        classifyingSpread <= policy.stabilityRelativeSpreadThreshold ? Stability::Stable : Stability::Unstable;
    return statistics;
}

namespace
{

[[nodiscard]] std::expected<void, ContractError> ValidateObservation(ExperimentObservation const &observation)
{
    SeriesPolicy const &policy = observation.policy;
    if (policy.minimumMeasuredSamples == 0U)
    {
        return std::unexpected(ContractError::InvalidMinimumSampleCount);
    }
    if (!IsFiniteNonNegative(policy.stabilityRelativeSpreadThreshold))
    {
        return std::unexpected(ContractError::InvalidStabilityThreshold);
    }

    SeriesStatistics const &statistics = observation.statistics;
    if (statistics.measuredSampleCount < policy.minimumMeasuredSamples)
    {
        return std::unexpected(ContractError::MeasuredSampleCountBelowPolicy);
    }

    std::array<double, 7U> const scalars{
        statistics.median, statistics.medianAbsoluteDeviation, statistics.minimum,           statistics.maximum,
        statistics.range,  statistics.robustRelativeSpread,    statistics.tailRelativeSpread};
    for (double const scalar : scalars)
    {
        if (!std::isfinite(scalar))
        {
            return std::unexpected(ContractError::InvalidObservationStatistics);
        }
    }
    bool const orderedExtremes = statistics.minimum <= statistics.median && statistics.median <= statistics.maximum;
    bool const consistentRange = std::abs(statistics.range - (statistics.maximum - statistics.minimum)) <=
                                 (1e-9 * (1.0 + std::abs(statistics.maximum)));
    bool const nonNegativeSpreads = statistics.minimum >= 0.0 && statistics.medianAbsoluteDeviation >= 0.0 &&
                                    statistics.robustRelativeSpread >= 0.0 && statistics.tailRelativeSpread >= 0.0;
    if (!orderedExtremes || !consistentRange || !nonNegativeSpreads)
    {
        return std::unexpected(ContractError::InvalidObservationStatistics);
    }
    return {};
}

[[nodiscard]] double ObservedRelativeSpread(SeriesStatistics const &statistics) noexcept
{
    return std::max(statistics.robustRelativeSpread, statistics.tailRelativeSpread);
}

} // namespace

std::expected<ExperimentComparison, ContractError> CompareExperiment(ExperimentObservation const &baseline,
                                                                     ExperimentObservation const &candidate,
                                                                     double const noiseRelativeThreshold)
{
    if (!IsFiniteNonNegative(noiseRelativeThreshold))
    {
        return std::unexpected(ContractError::InvalidNoiseThreshold);
    }
    if (baseline.workloadId != candidate.workloadId)
    {
        return std::unexpected(ContractError::UnequalWorkloadIdentity);
    }
    if (baseline.outputFingerprint != candidate.outputFingerprint)
    {
        return std::unexpected(ContractError::UnequalOutputFingerprint);
    }
    if (!(baseline.policy == candidate.policy))
    {
        return std::unexpected(ContractError::MismatchedSamplePolicy);
    }
    if (baseline.statistics.measuredSampleCount != candidate.statistics.measuredSampleCount)
    {
        return std::unexpected(ContractError::MismatchedMeasuredSampleCount);
    }
    if (auto const validBaseline = ValidateObservation(baseline); !validBaseline)
    {
        return std::unexpected(validBaseline.error());
    }
    if (auto const validCandidate = ValidateObservation(candidate); !validCandidate)
    {
        return std::unexpected(validCandidate.error());
    }
    // Relative change is defined only against a positive baseline; a non-positive candidate is reported with
    // its own error rather than being blamed on the baseline.
    if (baseline.statistics.median <= 0.0)
    {
        return std::unexpected(ContractError::InvalidBaselineMedian);
    }
    if (candidate.statistics.median <= 0.0)
    {
        return std::unexpected(ContractError::InvalidCandidateMedian);
    }

    double const relativeChange =
        (candidate.statistics.median - baseline.statistics.median) / baseline.statistics.median;
    // A change is only significant if it clears the noise the runs themselves reveal, not just the user knob.
    double const effectiveThreshold = std::max({noiseRelativeThreshold, ObservedRelativeSpread(baseline.statistics),
                                                ObservedRelativeSpread(candidate.statistics)});

    ExperimentComparison comparison{};
    comparison.baselineMedian = baseline.statistics.median;
    comparison.candidateMedian = candidate.statistics.median;
    comparison.relativeChange = relativeChange;
    comparison.effectiveThreshold = effectiveThreshold;

    bool const eitherUnstable =
        baseline.statistics.stability == Stability::Unstable || candidate.statistics.stability == Stability::Unstable;
    if (!eitherUnstable)
    {
        if (relativeChange < -effectiveThreshold)
        {
            comparison.outcome = ExperimentOutcome::Improvement;
        }
        else if (relativeChange > effectiveThreshold)
        {
            comparison.outcome = ExperimentOutcome::Regression;
        }
    }
    return comparison;
}

std::span<EvidenceKind const> RequiredEvidence(BottleneckHypothesis const hypothesis) noexcept
{
    // Every hypothesis pairs its direct signal with an equal-output controlled experiment: correlation from a
    // counter is never causation until an A/B test with identical output isolates the change.
    static constexpr std::array<EvidenceKind, 3U> kCpuSubmission{EvidenceKind::CpuSubmissionTiming,
                                                                 EvidenceKind::GpuTimestampSpan,
                                                                 EvidenceKind::EqualOutputControlledExperiment};
    static constexpr std::array<EvidenceKind, 3U> kSynchronization{
        EvidenceKind::QueueTimeline, EvidenceKind::GpuTimestampSpan, EvidenceKind::EqualOutputControlledExperiment};
    static constexpr std::array<EvidenceKind, 3U> kArithmetic{EvidenceKind::ArithmeticUtilizationCounter,
                                                              EvidenceKind::CompilerResourceReport,
                                                              EvidenceKind::EqualOutputControlledExperiment};
    static constexpr std::array<EvidenceKind, 2U> kTexture{EvidenceKind::TextureCacheCounter,
                                                           EvidenceKind::EqualOutputControlledExperiment};
    static constexpr std::array<EvidenceKind, 2U> kMemory{EvidenceKind::MemoryBandwidthCounter,
                                                          EvidenceKind::EqualOutputControlledExperiment};
    static constexpr std::array<EvidenceKind, 3U> kOccupancy{EvidenceKind::OccupancyCounter,
                                                             EvidenceKind::CompilerResourceReport,
                                                             EvidenceKind::EqualOutputControlledExperiment};
    static constexpr std::array<EvidenceKind, 2U> kFixedFunction{EvidenceKind::FixedFunctionCounter,
                                                                 EvidenceKind::EqualOutputControlledExperiment};
    static constexpr std::array<EvidenceKind, 3U> kInsufficientWork{
        EvidenceKind::WorkAmount, EvidenceKind::GpuTimestampSpan, EvidenceKind::EqualOutputControlledExperiment};
    switch (hypothesis)
    {
    case BottleneckHypothesis::CpuSubmission:
        return kCpuSubmission;
    case BottleneckHypothesis::SynchronizationSerialization:
        return kSynchronization;
    case BottleneckHypothesis::ArithmeticThroughput:
        return kArithmetic;
    case BottleneckHypothesis::TextureCache:
        return kTexture;
    case BottleneckHypothesis::MemoryBandwidthLatency:
        return kMemory;
    case BottleneckHypothesis::OccupancyLatencyHiding:
        return kOccupancy;
    case BottleneckHypothesis::FixedFunction:
        return kFixedFunction;
    case BottleneckHypothesis::InsufficientWork:
        return kInsufficientWork;
    }
    // Fail closed: an out-of-range enum yields no required set, which ClassifyHypothesis treats as unknown.
    return {};
}

std::expected<HypothesisAssessment, ContractError> ClassifyHypothesis(BottleneckHypothesis const hypothesis,
                                                                      std::span<EvidenceRecord const> evidence)
{
    std::span<EvidenceKind const> const required = RequiredEvidence(hypothesis);
    if (required.empty())
    {
        return std::unexpected(ContractError::UnknownHypothesis);
    }

    for (std::size_t outer = 0U; outer < evidence.size(); ++outer)
    {
        for (std::size_t inner = outer + 1U; inner < evidence.size(); ++inner)
        {
            if (evidence[outer].kind == evidence[inner].kind)
            {
                return std::unexpected(ContractError::DuplicateEvidenceKind);
            }
        }
    }

    HypothesisAssessment assessment{};
    assessment.hypothesis = hypothesis;

    bool allSupport = true;
    for (EvidenceKind const kind : required)
    {
        EvidenceRecord const *found = nullptr;
        for (EvidenceRecord const &record : evidence)
        {
            if (record.kind == kind)
            {
                found = &record;
                break;
            }
        }
        if (found == nullptr)
        {
            assessment.missingEvidence.push_back(kind);
            allSupport = false;
            continue;
        }
        switch (found->availability)
        {
        case EvidenceAvailability::CounterUnavailable:
            assessment.unavailableCounters.push_back(kind);
            allSupport = false;
            break;
        case EvidenceAvailability::NotCollected:
            assessment.missingEvidence.push_back(kind);
            allSupport = false;
            break;
        case EvidenceAvailability::Measured:
            if (!found->supportsHypothesis)
            {
                allSupport = false;
            }
            break;
        }
    }

    if (!assessment.unavailableCounters.empty())
    {
        assessment.conclusion = HypothesisConclusion::CounterUnavailable;
    }
    else if (!assessment.missingEvidence.empty())
    {
        assessment.conclusion = HypothesisConclusion::NeedsEvidence;
    }
    else if (allSupport)
    {
        assessment.conclusion = HypothesisConclusion::Supported;
    }
    else
    {
        assessment.conclusion = HypothesisConclusion::NotSupported;
    }
    return assessment;
}

std::vector<NextExperiment> SuggestNextExperiments(HypothesisAssessment const &assessment)
{
    std::vector<NextExperiment> experiments;
    experiments.reserve(assessment.missingEvidence.size() + assessment.unavailableCounters.size());
    std::uint32_t priority = 0U;
    for (EvidenceKind const kind : assessment.missingEvidence)
    {
        experiments.push_back(
            {.evidence = kind, .reason = NextExperimentReason::CollectMissingEvidence, .priority = priority});
        ++priority;
    }
    for (EvidenceKind const kind : assessment.unavailableCounters)
    {
        experiments.push_back({.evidence = kind,
                               .reason = NextExperimentReason::FindAlternativeForUnavailableCounter,
                               .priority = priority});
        ++priority;
    }
    return experiments;
}

std::expected<void, ContractError> ValidatePixEventName(std::string_view const name) noexcept
{
    if (name.empty())
    {
        return std::unexpected(ContractError::EmptyPixEventName);
    }
    if (name.size() > kMaximumPixEventNameLength)
    {
        return std::unexpected(ContractError::PixEventNameTooLong);
    }
    for (char const character : name)
    {
        auto const value = static_cast<unsigned char>(character);
        if (value < 0x20U || value >= 0x7fU)
        {
            return std::unexpected(ContractError::InvalidPixEventCharacter);
        }
    }
    return {};
}

std::expected<std::uint32_t, ContractError> ValidatePixScopes(std::span<PixScopeMarker const> markers)
{
    // Real PIX scopes are positional: `PIXBeginEvent` carries the name and `PIXEndEvent` simply closes the
    // innermost open scope. We validate Begin names, but End markers are matched by nesting alone.
    std::uint32_t openCount = 0U;
    std::uint32_t maximumDepth = 0U;
    for (PixScopeMarker const &marker : markers)
    {
        if (marker.kind == PixScopeMarkerKind::Begin)
        {
            if (auto const validName = ValidatePixEventName(marker.name); !validName)
            {
                return std::unexpected(validName.error());
            }
            ++openCount;
            maximumDepth = std::max(maximumDepth, openCount);
        }
        else
        {
            if (openCount == 0U)
            {
                return std::unexpected(ContractError::StrayPixScopeEnd);
            }
            --openCount;
        }
    }
    if (openCount != 0U)
    {
        return std::unexpected(ContractError::UnterminatedPixScope);
    }
    return maximumDepth;
}

std::expected<void, ContractError> ValidatePixEventRegion(PixEventRegion const &region) noexcept
{
    if (auto const validName = ValidatePixEventName(region.name); !validName)
    {
        return std::unexpected(validName.error());
    }
    if (region.endTick < region.beginTick)
    {
        return std::unexpected(ContractError::PixRegionEndBeforeBegin);
    }
    return {};
}

} // namespace ch19::gpu_profiling
