// Chapter 31 Solution: the auto-exposure feedback loop, written out as five explicit stages.
//
// The loop this file implements is deliberately not hidden behind a helper, because every one of its steps is a
// place where a plausible-looking shortcut produces a plausible-looking wrong number:
//
//   1. HistogramCS turns each stored sample into an absolute luminance, weights it in integer fixed point, and adds
//      it into a bounded log2 histogram. Every accumulator is an integer, so the answer does not depend on which
//      wave got there first. Alongside the bins it accumulates the two real-valued sufficient statistics that the
//      bins cannot reproduce, in a declared order, in group-shared memory, never through a floating-point atomic.
//   2. ReduceCS sums the per-tile partials in tile-index order. One declared order, one answer.
//   3. MeterCS reduces the distribution to a single luminance under the requested policy, and publishes both the
//      exact statistic it used and the bin-centre reconstruction it did *not* use, so the two can be told apart.
//   4. ExposureCS turns that luminance into a target exposure, decides whether the stored adaptation state may be
//      reused, smooths in the log exposure domain, and commits a value for the *next* frame.
//   5. ComposeCS applies the exposure the *previous* frame committed, exactly once.
//
// The one-frame delay is therefore a property of which resource each stage reads, not of a comment. ComposeCS never
// reads the meter; it reads the displayed exposure, and the displayed exposure is the history value ExposureCS
// found in the slot the previous frame wrote. A reset is the single exception, and it is always published.

#include "../Common/AutoExposureShared.hlsli"

[numthreads(64, 1, 1)] void ClearCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ClearFrameState(dispatchThreadId.x);
}

[numthreads(8, 8, 1)] void SceneCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    WriteSceneRecord(dispatchThreadId.xy, FramePreExposure());
}

// ---------------------------------------------------------------------------------------------------------------
// Sample weights. Integer fixed point in [0, WeightOne], rounded half away from zero, which is the one rule that
// does not depend on the current floating-point rounding mode.
// ---------------------------------------------------------------------------------------------------------------

uint QuantizeUnitWeight(float weight)
{
    return uint(floor(saturate(weight) * float(WeightOne) + 0.5f));
}

// Fixed-point product, rounded half up. Both operands are already bounded by WeightOne, so the result is too, and
// the divide is a shift rather than a divide.
uint CombineWeights(uint left, uint right)
{
    return ((left * right) + (WeightOne / 2u)) / WeightOne;
}

// A radially symmetric centre weighting. The normalized radius is the distance from the frame centre divided by the
// distance to the *corner pixel centre*, not to the image corner: normalizing by the image corner instead would
// make the corner pixel's weight depend on the resolution, which is the kind of silent resolution dependence a
// metering rule must not have.
uint CentreWeightQuantized(uint2 extent, uint2 pixel)
{
    float halfWidth = 0.5f * float(extent.x);
    float halfHeight = 0.5f * float(extent.y);
    float dx = (float(pixel.x) + 0.5f) - halfWidth;
    float dy = (float(pixel.y) + 0.5f) - halfHeight;
    float cornerX = halfWidth - 0.5f;
    float cornerY = halfHeight - 0.5f;
    float cornerDistance = sqrt((cornerX * cornerX) + (cornerY * cornerY));
    float radius = cornerDistance > 0.0f ? saturate(sqrt((dx * dx) + (dy * dy)) / cornerDistance) : 0.0f;
    float profile = pow(1.0f - radius, CentreFalloffPower);
    return QuantizeUnitWeight(EdgeWeightValue + ((CentreWeightValue - EdgeWeightValue) * profile));
}

// ---------------------------------------------------------------------------------------------------------------
// Histogram layout. Edge b is minimum + span * b / binCount, computed from b rather than by repeatedly adding a bin
// width, so edge 0 is exactly the declared minimum, edge binCount is exactly the declared maximum, and nothing
// drifts along the array.
// ---------------------------------------------------------------------------------------------------------------

float Log2LuminanceSpan()
{
    return MaximumLog2Luminance - MinimumLog2Luminance;
}

float BinLowerLog2Luminance(uint binEdge)
{
    return MinimumLog2Luminance + ((Log2LuminanceSpan() * float(binEdge)) / float(BinCount));
}

float BinCentreLog2Luminance(uint bin)
{
    float numerator = Log2LuminanceSpan() * ((2.0f * float(bin)) + 1.0f);
    return MinimumLog2Luminance + (numerator / (2.0f * float(BinCount)));
}

struct SampleClassification
{
    uint accepted;
    uint rejection;
    uint classification;
    uint bin;
    float absoluteLuminance;
    float log2Luminance;
    uint hasLog2Luminance;
};

// The per-sample decision, which is exactly what one thread does. A malformed configuration is refused by the CPU
// before the dispatch; a malformed *sample* is data, and comes back as a rejection reason with accepted false.
//
// The sample's values are inspected before its weight, so a NaN inside a masked-out region is still reported. A
// frame buffer with NaNs in it is a rendering bug, and metering is the cheapest place in the frame to notice one.
SampleClassification ClassifySample(float3 storedSceneLinear, float preExposure, uint weight)
{
    SampleClassification sample;
    sample.accepted = 0u;
    sample.rejection = RejectNone;
    sample.classification = BinClassZero;
    sample.bin = 0u;
    sample.absoluteLuminance = 0.0f;
    sample.log2Luminance = 0.0f;
    sample.hasLog2Luminance = 0u;

    if (!IsFiniteColor(storedSceneLinear))
    {
        sample.rejection = RejectNonFiniteChannel;
        return sample;
    }
    if (storedSceneLinear.r < 0.0f || storedSceneLinear.g < 0.0f || storedSceneLinear.b < 0.0f)
    {
        // A negative radiance is a bug in whatever produced the sample. Raising it to zero would hide a broken
        // light, a bad filter kernel, or a decoded NaN.
        sample.rejection = RejectNegativeChannel;
        return sample;
    }
    if (storedSceneLinear.r > MaximumSceneLinearValue || storedSceneLinear.g > MaximumSceneLinearValue ||
        storedSceneLinear.b > MaximumSceneLinearValue)
    {
        sample.rejection = RejectLuminanceOutOfDomain;
        return sample;
    }

    // The pre-exposure is removed exactly once, here, and nothing downstream divides again. That is what makes the
    // same scene metered under two different pre-exposures reach the same exposure.
    float absolute = SceneLuminance(storedSceneLinear) / preExposure;
    if (!isfinite(absolute))
    {
        sample.rejection = RejectLuminanceOutOfDomain;
        return sample;
    }
    sample.absoluteLuminance = absolute;

    if (absolute == 0.0f)
    {
        // log2(0) is never evaluated. Black is classified structurally, which is the only way to keep a black sky
        // out of the floating-point domain while still letting a policy decide what to do with it.
        sample.classification = BinClassZero;
        sample.bin = 0u;
    }
    else
    {
        float log2Luminance = log2(absolute);
        sample.log2Luminance = log2Luminance;
        sample.hasLog2Luminance = 1u;
        if (log2Luminance < MinimumLog2Luminance)
        {
            sample.classification = BinClassBelowRange;
            sample.bin = 0u;
        }
        else if (log2Luminance >= MaximumLog2Luminance)
        {
            sample.classification = BinClassAboveRange;
            sample.bin = BinCount - 1u;
        }
        else
        {
            float scaled = (log2Luminance - MinimumLog2Luminance) * (float(BinCount) / Log2LuminanceSpan());
            float floored = floor(scaled);
            uint bin = BinCount - 1u;
            if (floored >= 0.0f && floored < float(BinCount))
            {
                bin = uint(floored);
            }
            // The published edges, not the scaled position, decide which bin owns a value: rounding in the multiply
            // above can put a value that sits exactly on an edge one bin to either side. The edges are strictly
            // increasing, so walking the candidate back onto its own half-open interval terminates.
            uint guard = 0u;
            while (bin > 0u && log2Luminance < BinLowerLog2Luminance(bin) && guard < MaximumBinCount)
            {
                --bin;
                ++guard;
            }
            guard = 0u;
            while ((bin + 1u) < BinCount && log2Luminance >= BinLowerLog2Luminance(bin + 1u) && guard < MaximumBinCount)
            {
                ++bin;
                ++guard;
            }
            sample.classification = BinClassInterior;
            sample.bin = bin;
        }
    }

    if (weight == 0u)
    {
        // A masked-out sample is not an error; it is a sample the shot said not to meter. It is counted so that an
        // accidentally empty mask is visible instead of silently halving the measurement.
        sample.rejection = RejectZeroWeight;
        return sample;
    }
    if (sample.classification == BinClassZero && BlackSamplePolicyId == BlackReject)
    {
        sample.rejection = RejectZeroLuminance;
        return sample;
    }
    if (sample.classification == BinClassBelowRange && BelowRangePolicyId == RangeReject)
    {
        sample.rejection = RejectBelowRange;
        return sample;
    }
    if (sample.classification == BinClassAboveRange && AboveRangePolicyId == RangeReject)
    {
        sample.rejection = RejectAboveRange;
        return sample;
    }

    sample.accepted = 1u;
    sample.rejection = RejectNone;
    return sample;
}

// ---------------------------------------------------------------------------------------------------------------
// Histogram construction
// ---------------------------------------------------------------------------------------------------------------

groupshared uint gsBinWeight[MaximumBinCount];
groupshared uint gsBinSampleCount[MaximumBinCount];
// Per-thread slots rather than a group-shared accumulator, because these two sums are real-valued and a
// group-shared float atomic does not exist for a reason: its result would depend on arrival order.
groupshared float gsWeightedLuminance[HistogramGroupThreads];
groupshared float gsWeightedLog2Luminance[HistogramGroupThreads];
groupshared float gsMinimumLuminance[HistogramGroupThreads];
groupshared float gsMaximumLuminance[HistogramGroupThreads];
groupshared uint gsHasAccepted[HistogramGroupThreads];
groupshared uint gsLog2Weight[HistogramGroupThreads];
groupshared uint gsAcceptedWeight[HistogramGroupThreads];
groupshared uint gsAcceptedCount[HistogramGroupThreads];
// Integer tallies. Addition is associative and commutative, so a group-shared atomic is exactly reproducible here
// no matter which thread arrives first.
groupshared uint gsRejectedByReason[8];
groupshared uint gsRejectedCount;
groupshared uint gsRejectedWeight;
groupshared uint gsBelowRangeCount;
groupshared uint gsBelowRangeWeight;
groupshared uint gsAboveRangeCount;
groupshared uint gsAboveRangeWeight;
groupshared uint gsZeroLuminanceCount;
groupshared uint gsZeroLuminanceWeight;
groupshared uint gsOverflow;

[numthreads(8, 8, 1)] void HistogramCS(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupId : SV_GroupID,
                                       uint groupIndex : SV_GroupIndex)
{
    uint2 extent = uint2(DisplayWidth, DisplayHeight);

    for (uint clearBin = groupIndex; clearBin < MaximumBinCount; clearBin += HistogramGroupThreads)
    {
        gsBinWeight[clearBin] = 0u;
        gsBinSampleCount[clearBin] = 0u;
    }
    gsWeightedLuminance[groupIndex] = 0.0f;
    gsWeightedLog2Luminance[groupIndex] = 0.0f;
    gsMinimumLuminance[groupIndex] = 0.0f;
    gsMaximumLuminance[groupIndex] = 0.0f;
    gsHasAccepted[groupIndex] = 0u;
    gsLog2Weight[groupIndex] = 0u;
    gsAcceptedWeight[groupIndex] = 0u;
    gsAcceptedCount[groupIndex] = 0u;
    if (groupIndex < 8u)
    {
        gsRejectedByReason[groupIndex] = 0u;
    }
    if (groupIndex == 0u)
    {
        gsRejectedCount = 0u;
        gsRejectedWeight = 0u;
        gsBelowRangeCount = 0u;
        gsBelowRangeWeight = 0u;
        gsAboveRangeCount = 0u;
        gsAboveRangeWeight = 0u;
        gsZeroLuminanceCount = 0u;
        gsZeroLuminanceWeight = 0u;
        gsOverflow = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    bool inside = dispatchThreadId.x < DisplayWidth && dispatchThreadId.y < DisplayHeight;
    if (inside)
    {
        uint2 pixel = dispatchThreadId.xy;
        uint index = TexelIndex(extent, pixel);
        SceneRecord scene = Scene[index];
        float3 stored = float3(scene.radianceR, scene.radianceG, scene.radianceB);

        // Centre weighting and the artist mask compose in fixed point and are deliberately not normalized here.
        // Normalization belongs to the estimators, which divide by the weight they actually accumulated; dividing
        // earlier would throw away the fact that a mask covers only part of the frame.
        uint centreWeight = ((Flags & FlagUseCentreWeighting) != 0u) ? CentreWeightQuantized(extent, pixel) : WeightOne;
        float maskValue = MaskValue(extent, pixel);
        uint maskWeight = QuantizeUnitWeight(maskValue);
        uint weight = WeightOne;
        if ((Flags & FlagUseCentreWeighting) != 0u)
        {
            weight = CombineWeights(weight, centreWeight);
        }
        if ((Flags & FlagUseMask) != 0u)
        {
            weight = CombineWeights(weight, maskWeight);
        }

        SampleClassification sample = ClassifySample(stored, scene.preExposure, weight);

        scene.sampleWeight = weight;
        scene.centreWeight = centreWeight;
        scene.maskWeight = maskWeight;
        Scene[index] = scene;

        PixelRecord record = Records[index];
        record.absoluteLuminance = sample.absoluteLuminance;
        record.log2Luminance = sample.log2Luminance;
        record.hasLog2Luminance = sample.hasLog2Luminance;
        record.centreWeight = centreWeight;
        record.maskWeight = maskWeight;
        record.sampleWeight = weight;
        record.centreWeightUnit = float(centreWeight) / float(WeightOne);
        record.sampleWeightUnit = float(weight) / float(WeightOne);
        record.bin = sample.bin;
        record.classification = sample.classification;
        record.rejection = sample.rejection;
        record.accepted = sample.accepted;
        record.status |= StatusWeighted | StatusClassified;
        Records[index] = record;

        if (sample.accepted != 0u)
        {
            uint priorTileWeight;
            InterlockedAdd(gsBinWeight[sample.bin], weight, priorTileWeight);
            InterlockedAdd(gsBinSampleCount[sample.bin], 1u);
            if ((MaximumTileWeight - priorTileWeight) < weight)
            {
                // Structurally unreachable for a group of HistogramGroupThreads samples at WeightOne each, and
                // checked anyway: a wrapped tile histogram is indistinguishable from a correct one downstream.
                InterlockedOr(gsOverflow, OverflowTileWeight);
            }

            float unitWeight = float(weight) / float(WeightOne);
            gsAcceptedWeight[groupIndex] = weight;
            gsAcceptedCount[groupIndex] = 1u;
            gsWeightedLuminance[groupIndex] = unitWeight * sample.absoluteLuminance;
            gsMinimumLuminance[groupIndex] = sample.absoluteLuminance;
            gsMaximumLuminance[groupIndex] = sample.absoluteLuminance;
            gsHasAccepted[groupIndex] = 1u;
            if (sample.hasLog2Luminance != 0u)
            {
                gsWeightedLog2Luminance[groupIndex] = unitWeight * sample.log2Luminance;
                gsLog2Weight[groupIndex] = weight;
            }

            if (sample.classification == BinClassZero)
            {
                InterlockedAdd(gsZeroLuminanceCount, 1u);
                InterlockedAdd(gsZeroLuminanceWeight, weight);
            }
            else if (sample.classification == BinClassBelowRange)
            {
                InterlockedAdd(gsBelowRangeCount, 1u);
                InterlockedAdd(gsBelowRangeWeight, weight);
            }
            else if (sample.classification == BinClassAboveRange)
            {
                InterlockedAdd(gsAboveRangeCount, 1u);
                InterlockedAdd(gsAboveRangeWeight, weight);
            }
        }
        else
        {
            InterlockedAdd(gsRejectedCount, 1u);
            InterlockedAdd(gsRejectedWeight, weight);
            InterlockedAdd(gsRejectedByReason[sample.rejection], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // One thread folds the tile's real-valued contributions together in thread-index order. That order is the whole
    // guarantee: a wave-wide shuffle reduction would be faster and would give a different sum on a different wave
    // width, and a floating-point atomic would give a different sum on every run.
    if (groupIndex == 0u)
    {
        TilePartial partial;
        partial.weightedLuminanceSum = 0.0f;
        partial.weightedLog2LuminanceSum = 0.0f;
        partial.minimumObservedLuminance = 0.0f;
        partial.maximumObservedLuminance = 0.0f;
        partial.hasAcceptedSample = 0u;
        partial.log2AccumulatedWeight = 0u;
        partial.acceptedWeight = 0u;
        partial.acceptedSampleCount = 0u;
        for (uint slot = 0u; slot < HistogramGroupThreads; ++slot)
        {
            partial.weightedLuminanceSum += gsWeightedLuminance[slot];
            partial.weightedLog2LuminanceSum += gsWeightedLog2Luminance[slot];
            partial.log2AccumulatedWeight += gsLog2Weight[slot];
            partial.acceptedWeight += gsAcceptedWeight[slot];
            partial.acceptedSampleCount += gsAcceptedCount[slot];
            if (gsHasAccepted[slot] != 0u)
            {
                if (partial.hasAcceptedSample == 0u)
                {
                    partial.hasAcceptedSample = 1u;
                    partial.minimumObservedLuminance = gsMinimumLuminance[slot];
                    partial.maximumObservedLuminance = gsMaximumLuminance[slot];
                }
                else
                {
                    partial.minimumObservedLuminance = min(partial.minimumObservedLuminance, gsMinimumLuminance[slot]);
                    partial.maximumObservedLuminance = max(partial.maximumObservedLuminance, gsMaximumLuminance[slot]);
                }
            }
        }
        uint tileIndex = (groupId.y * TileCountX) + groupId.x;
        if (tileIndex < TileCount)
        {
            Partials[tileIndex] = partial;
        }

        InterlockedAdd(Statistics[0].rejectedSampleCount, gsRejectedCount);
        InterlockedAdd(Statistics[0].rejectedWeight, gsRejectedWeight);
        InterlockedAdd(Statistics[0].belowRangeSampleCount, gsBelowRangeCount);
        InterlockedAdd(Statistics[0].belowRangeWeight, gsBelowRangeWeight);
        InterlockedAdd(Statistics[0].aboveRangeSampleCount, gsAboveRangeCount);
        InterlockedAdd(Statistics[0].aboveRangeWeight, gsAboveRangeWeight);
        InterlockedAdd(Statistics[0].zeroLuminanceSampleCount, gsZeroLuminanceCount);
        InterlockedAdd(Statistics[0].zeroLuminanceWeight, gsZeroLuminanceWeight);
        InterlockedAdd(Statistics[0].rejectedNone, gsRejectedByReason[RejectNone]);
        InterlockedAdd(Statistics[0].rejectedNonFiniteChannel, gsRejectedByReason[RejectNonFiniteChannel]);
        InterlockedAdd(Statistics[0].rejectedNegativeChannel, gsRejectedByReason[RejectNegativeChannel]);
        InterlockedAdd(Statistics[0].rejectedLuminanceOutOfDomain, gsRejectedByReason[RejectLuminanceOutOfDomain]);
        InterlockedAdd(Statistics[0].rejectedZeroWeight, gsRejectedByReason[RejectZeroWeight]);
        InterlockedAdd(Statistics[0].rejectedZeroLuminance, gsRejectedByReason[RejectZeroLuminance]);
        InterlockedAdd(Statistics[0].rejectedBelowRange, gsRejectedByReason[RejectBelowRange]);
        InterlockedAdd(Statistics[0].rejectedAboveRange, gsRejectedByReason[RejectAboveRange]);
        if (gsOverflow != 0u)
        {
            InterlockedOr(Statistics[0].overflowFlags, gsOverflow);
        }
    }

    // Every thread merges part of the tile histogram into the frame histogram. These are integer atomics, so the
    // merged result is identical whatever order the tiles complete in, which is exactly why the weights are
    // integers in the first place.
    for (uint mergeBin = groupIndex; mergeBin < BinCount; mergeBin += HistogramGroupThreads)
    {
        uint tileWeight = gsBinWeight[mergeBin];
        if (tileWeight == 0u && gsBinSampleCount[mergeBin] == 0u)
        {
            continue;
        }
        uint priorWeight;
        InterlockedAdd(Histogram[mergeBin].weight, tileWeight, priorWeight);
        InterlockedAdd(Histogram[mergeBin].sampleCount, gsBinSampleCount[mergeBin]);
        if ((0xFFFFFFFFu - priorWeight) < tileWeight)
        {
            InterlockedOr(Statistics[0].overflowFlags, OverflowBinWeight);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Deterministic reduction. One thread, tiles visited in tile-index order.
// ---------------------------------------------------------------------------------------------------------------

[numthreads(1, 1, 1)] void ReduceCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x != 0u)
    {
        return;
    }
    HistogramStatisticsRecord statistics = Statistics[0];
    float weightedLuminanceSum = 0.0f;
    float weightedLog2LuminanceSum = 0.0f;
    float minimumObserved = 0.0f;
    float maximumObserved = 0.0f;
    uint hasAccepted = 0u;
    uint acceptedWeight = 0u;
    uint acceptedSampleCount = 0u;
    uint log2AccumulatedWeight = 0u;
    uint overflow = 0u;

    for (uint tile = 0u; tile < TileCount; ++tile)
    {
        TilePartial partial = Partials[tile];
        weightedLuminanceSum += partial.weightedLuminanceSum;
        weightedLog2LuminanceSum += partial.weightedLog2LuminanceSum;
        if ((0xFFFFFFFFu - acceptedWeight) < partial.acceptedWeight)
        {
            overflow |= OverflowTotalWeight;
        }
        acceptedWeight += partial.acceptedWeight;
        acceptedSampleCount += partial.acceptedSampleCount;
        log2AccumulatedWeight += partial.log2AccumulatedWeight;
        if (partial.hasAcceptedSample != 0u)
        {
            if (hasAccepted == 0u)
            {
                hasAccepted = 1u;
                minimumObserved = partial.minimumObservedLuminance;
                maximumObserved = partial.maximumObservedLuminance;
            }
            else
            {
                minimumObserved = min(minimumObserved, partial.minimumObservedLuminance);
                maximumObserved = max(maximumObserved, partial.maximumObservedLuminance);
            }
        }
    }

    statistics.weightedLuminanceSum = weightedLuminanceSum;
    statistics.weightedLog2LuminanceSum = weightedLog2LuminanceSum;
    statistics.minimumObservedLuminance = minimumObserved;
    statistics.maximumObservedLuminance = maximumObserved;
    statistics.hasAcceptedSample = hasAccepted;
    statistics.acceptedWeight = acceptedWeight;
    statistics.acceptedSampleCount = acceptedSampleCount;
    statistics.log2AccumulatedWeight = log2AccumulatedWeight;
    statistics.overflowFlags |= overflow;
    Statistics[0] = statistics;
}

// ---------------------------------------------------------------------------------------------------------------
// Metering
// ---------------------------------------------------------------------------------------------------------------

// floor(totalWeight * percentileFixed / PercentileOne), exactly, in 32-bit arithmetic.
//
// This is the one multiply in the chapter that can leave the width of its accumulator: the lab's total weight
// reaches 2^26 and a percentile reaches 10^6, so the naive product needs 46 bits. Doing it in floats instead would
// move a boundary that lands exactly on a bin edge to whichever side the last mantissa bit fell, which is precisely
// the reproducibility the integer weights were chosen to protect.
//
// W = 10^6 * q + r, so W * P / 10^6 = q * P + r * P / 10^6, and the second term is split again by writing
// P = 1000 * a + b, which keeps every intermediate below 2^30.
uint PercentilePosition(uint totalWeight, uint percentileFixed)
{
    uint q = totalWeight / PercentileOne;
    uint r = totalWeight % PercentileOne;
    uint a = percentileFixed / 1000u;
    uint b = percentileFixed % 1000u;
    uint high = (a * r) + ((b * r) / 1000u);
    return (q * percentileFixed) + (high / 1000u);
}

[numthreads(1, 1, 1)] void MeterCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x != 0u)
    {
        return;
    }
    HistogramStatisticsRecord statistics = Statistics[0];
    MeterRecord meter = (MeterRecord)0;
    meter.policy = MeteringPolicyId;
    meter.abiMarker = AbiMarker;
    meter.minimumObservedLuminance = statistics.minimumObservedLuminance;
    meter.maximumObservedLuminance = statistics.maximumObservedLuminance;
    meter.binQuantizationLog2Bound = 0.5f * (Log2LuminanceSpan() / float(BinCount));
    meter.logAverageWeight = statistics.log2AccumulatedWeight;
    meter.excludedZeroLuminanceFromLogAverage = statistics.zeroLuminanceWeight > 0u ? 1u : 0u;
    meter.clippedIntoLowestBin = statistics.belowRangeWeight > 0u ? 1u : 0u;
    meter.clippedIntoHighestBin = statistics.aboveRangeWeight > 0u ? 1u : 0u;

    uint occupied = 0u;
    uint lowest = 0u;
    uint highest = 0u;
    uint hasOccupied = 0u;
    for (uint scan = 0u; scan < BinCount; ++scan)
    {
        if (Histogram[scan].sampleCount == 0u)
        {
            continue;
        }
        if (hasOccupied == 0u)
        {
            hasOccupied = 1u;
            lowest = scan;
        }
        highest = scan;
        ++occupied;
    }
    meter.occupiedBinCount = occupied;
    meter.lowestOccupiedBin = lowest;
    meter.highestOccupiedBin = highest;
    meter.hasOccupiedBin = hasOccupied;

    if (statistics.hasAcceptedSample == 0u || statistics.acceptedWeight == 0u)
    {
        meter.status = MeterNoAcceptedSamples;
        Meter[0] = meter;
        return;
    }
    // No accepted sample carried a positive luminance, which under the CountInLowestBin policy is exactly what an
    // all-black frame looks like: accepted samples, accepted weight, and an occupied lowest bin. It is refused here,
    // before any estimator can report the lowest bin's centre as though a measurement had happened. The test is
    // exact and takes no logarithm of zero.
    if (statistics.log2AccumulatedWeight == 0u)
    {
        meter.status = MeterNoPositiveLuminance;
        Meter[0] = meter;
        return;
    }

    // Every accepted sample lies within half a bin of its recorded bin centre unless it was black or was saturated
    // into an end bin. Either fact breaks the bound, so it is published as invalid rather than quietly quoted.
    bool bounded = statistics.zeroLuminanceWeight == 0u && statistics.belowRangeWeight == 0u &&
                   statistics.aboveRangeWeight == 0u;
    meter.status = MeterEvaluated | (bounded ? MeterBinCentreBounded : 0u);

    float totalRealWeight = float(statistics.acceptedWeight) / float(WeightOne);

    if (MeteringPolicyId == PolicyPercentileWindow)
    {
        // A percentile boundary is an integer position inside the accumulated weight, so it is exactly reproducible
        // for a given histogram. It is not independent of how much weight the frame accumulated, and this is the one
        // policy that cannot be exact: the distribution is only known to bin resolution.
        uint lowerTarget = PercentilePosition(statistics.acceptedWeight, LowerPercentileFixed);
        uint upperTarget = PercentilePosition(statistics.acceptedWeight, UpperPercentileFixed);
        meter.lowerTargetWeight = lowerTarget;
        meter.upperTargetWeight = upperTarget;

        uint firstBin = 0u;
        uint lastBin = 0u;
        uint windowWeight = 0u;
        float log2Luminance = 0.0f;

        if (upperTarget == lowerTarget)
        {
            // The two boundaries quantized onto the same position. That happens when the percentiles are equal, and
            // also for two distinct percentiles separated by less than one unit of weight. Reporting the bin that
            // owns that position is how a plain median is requested, so it is answered rather than refused.
            uint position = min(lowerTarget, statistics.acceptedWeight - 1u);
            uint cumulative = 0u;
            bool found = false;
            for (uint bin = 0u; bin < BinCount; ++bin)
            {
                cumulative += Histogram[bin].weight;
                if (!found && cumulative > position)
                {
                    found = true;
                    firstBin = bin;
                    lastBin = bin;
                    windowWeight = Histogram[bin].weight;
                    log2Luminance = BinCentreLog2Luminance(bin);
                }
            }
            if (!found)
            {
                meter.status = MeterNoAcceptedSamples;
                Meter[0] = meter;
                return;
            }
            meter.status |= MeterDegenerateWindow;
        }
        else
        {
            float weightedLog2Sum = 0.0f;
            uint cumulative = 0u;
            bool hasFirst = false;
            for (uint bin = 0u; bin < BinCount; ++bin)
            {
                uint binStart = cumulative;
                cumulative += Histogram[bin].weight;
                uint overlapLow = max(binStart, lowerTarget);
                uint overlapHigh = min(cumulative, upperTarget);
                if (overlapHigh <= overlapLow)
                {
                    continue;
                }
                uint overlap = overlapHigh - overlapLow;
                if (!hasFirst)
                {
                    hasFirst = true;
                    firstBin = bin;
                }
                lastBin = bin;
                windowWeight += overlap;
                weightedLog2Sum += float(overlap) * BinCentreLog2Luminance(bin);
            }
            if (!hasFirst || windowWeight == 0u)
            {
                meter.status = MeterNoAcceptedSamples;
                Meter[0] = meter;
                return;
            }
            log2Luminance = weightedLog2Sum / float(windowWeight);
        }

        meter.firstWindowBin = firstBin;
        meter.lastWindowBin = lastBin;
        meter.windowWeight = windowWeight;
        meter.windowLowerLog2Luminance = BinLowerLog2Luminance(firstBin);
        meter.windowUpperLog2Luminance = BinLowerLog2Luminance(lastBin + 1u);
        meter.meteredLog2Luminance = log2Luminance;
        meter.meteredLuminance = exp2(log2Luminance);
        // The percentile answer *is* a bin-centre reconstruction by definition, so it is published as such rather
        // than being flagged as an exact statistic.
        meter.binCentreLog2Luminance = log2Luminance;
        Meter[0] = meter;
        return;
    }

    meter.status |= MeterUsedExactStatistic;
    if (MeteringPolicyId == PolicyArithmeticMean)
    {
        float binCentreSum = 0.0f;
        for (uint bin = 0u; bin < BinCount; ++bin)
        {
            uint binWeight = Histogram[bin].weight;
            if (binWeight == 0u)
            {
                continue;
            }
            binCentreSum += (float(binWeight) / float(WeightOne)) * exp2(BinCentreLog2Luminance(bin));
        }
        float binCentreMean = binCentreSum / totalRealWeight;
        meter.binCentreLog2Luminance = binCentreMean > 0.0f ? log2(binCentreMean) : 0.0f;

        // The exact statistic, not a reconstruction: a bin records how much weight fell inside it, not where inside
        // it, so any mean recovered from bin centres is quantized and this one is not.
        float mean = statistics.weightedLuminanceSum / totalRealWeight;
        if (!isfinite(mean) || mean <= 0.0f)
        {
            meter.status = MeterNoPositiveLuminance;
            Meter[0] = meter;
            return;
        }
        meter.meteredLuminance = mean;
        meter.meteredLog2Luminance = log2(mean);
        Meter[0] = meter;
        return;
    }

    float binCentreLog2Sum = 0.0f;
    for (uint bin = 0u; bin < BinCount; ++bin)
    {
        uint binWeight = Histogram[bin].weight;
        if (binWeight == 0u)
        {
            continue;
        }
        binCentreLog2Sum += (float(binWeight) / float(WeightOne)) * BinCentreLog2Luminance(bin);
    }
    meter.binCentreLog2Luminance = binCentreLog2Sum / totalRealWeight;

    // The log average is normalized by the weight that actually contributed a logarithm, which is smaller than the
    // accepted weight exactly when black samples were accepted into the lowest bin.
    float logWeight = float(statistics.log2AccumulatedWeight) / float(WeightOne);
    float logAverage = statistics.weightedLog2LuminanceSum / logWeight;
    if (!isfinite(logAverage))
    {
        meter.status = MeterNoPositiveLuminance;
        Meter[0] = meter;
        return;
    }
    meter.meteredLog2Luminance = logAverage;
    meter.meteredLuminance = exp2(logAverage);
    Meter[0] = meter;
}

// ---------------------------------------------------------------------------------------------------------------
// Target exposure and temporal adaptation
// ---------------------------------------------------------------------------------------------------------------

// alpha = 1 - exp(-speed * dt). Zero dt gives exactly zero, so a paused frame changes nothing; a large dt approaches
// but never reaches one. Because the residual after dt is exp(-speed * dt), splitting a step into two halves
// multiplies the residuals and lands in the same place: the smoothing is frame-rate independent by construction.
//
// The CPU reference writes this as -expm1(-x) to avoid the cancellation that eats the leading digits of 1 - exp(-x)
// for the small products a high frame rate produces. HLSL has no expm1, so this evaluates the subtraction directly
// and loses a few low bits of a value that is already only used to blend two stop values; the chapter says so here
// rather than implying the two are bit-identical.
float SmoothingAlpha(float speedPerSecond, float deltaSeconds)
{
    return saturate(1.0f - exp2(-(speedPerSecond * deltaSeconds) * 1.44269504f));
}

[numthreads(1, 1, 1)] void ExposureCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x != 0u)
    {
        return;
    }
    MeterRecord meter = Meter[0];
    ExposureHistorySlot history = History[HistoryReadSlot];
    float preExposure = FramePreExposure();

    ExposureRecord record = (ExposureRecord)0;
    record.frameIndex = FrameIndex;
    record.abiMarker = AbiMarker;
    record.mode = ExposureModeId;
    record.configurationIdentityLow = ConfigurationIdentityLow;
    record.configurationIdentityHigh = ConfigurationIdentityHigh;
    record.historyIdentityLow = history.configurationIdentityLow;
    record.historyIdentityHigh = history.configurationIdentityHigh;
    record.preExposure = preExposure;
    record.status = ExposureEvaluated;

    bool historyValid = (Flags & FlagResetHistory) == 0u && history.valid != 0u;
    record.historyValid = historyValid ? 1u : 0u;
    record.producerFrameIndex = historyValid ? history.producerFrameIndex : FrameIndex;

    bool manual = ExposureModeId == ModeManual;
    bool measurementValid = manual || (meter.status & MeterEvaluated) != 0u;
    if (manual)
    {
        record.status |= ExposureManualMode;
    }

    // What this frame's measurement asks for. It is never the exposure applied to this frame; only the blend below
    // may turn it into a committed exposure, and only the *previous* frame's committed exposure is displayed.
    float unclampedTargetStops = 0.0f;
    float meteredLuminance = 0.0f;
    if (manual)
    {
        unclampedTargetStops = ManualExposureStops;
    }
    else if (measurementValid)
    {
        meteredLuminance = meter.meteredLuminance;
        // The difference of logarithms, not the logarithm of the ratio: middleGrey / metered can underflow or
        // overflow for a legitimate metered value, and its logarithm would then be infinite for a configuration
        // that is perfectly representable in stops.
        unclampedTargetStops = (log2(MiddleGreyLuminance) - log2(meteredLuminance)) + ExposureCompensationStops;
        if (!isfinite(unclampedTargetStops))
        {
            measurementValid = false;
            unclampedTargetStops = 0.0f;
            meteredLuminance = 0.0f;
        }
    }

    uint resets = 0u;
    if (!historyValid)
    {
        resets |= ResetNoHistory;
    }
    if (FrameIndex == 0u)
    {
        resets |= ResetFirstFrame;
    }
    if ((Flags & FlagCameraCut) != 0u)
    {
        resets |= ResetCameraCut;
    }
    if (historyValid)
    {
        bool sequential = FrameIndex != 0u && (FrameIndex - 1u) == history.producerFrameIndex;
        if (!sequential)
        {
            // A stale value fed through a dt-based smoother produces an adaptation that silently depends on how long
            // the stall was, so the stored value is discarded instead of reused.
            resets |= ResetNonSequentialProducerFrame;
        }
        if (history.configurationIdentityLow != ConfigurationIdentityLow ||
            history.configurationIdentityHigh != ConfigurationIdentityHigh)
        {
            resets |= ResetConfigurationChanged;
        }
        if (history.mode != ExposureModeId)
        {
            resets |= ResetExposureModeChanged;
        }
        if (!isfinite(history.committedStops) || abs(history.committedStops) > ExposureStopsLimit)
        {
            resets |= ResetInvalidHistoryValue;
        }
    }
    bool reset = resets != 0u;
    record.resets = resets;
    record.reusedHistory = reset ? 0u : 1u;
    if (!reset)
    {
        record.status |= ExposureReusedHistory;
    }

    float targetStops = clamp(unclampedTargetStops, MinimumExposureStops, MaximumExposureStops);
    record.targetClampedToMinimum = (measurementValid && unclampedTargetStops < MinimumExposureStops) ? 1u : 0u;
    record.targetClampedToMaximum = (measurementValid && unclampedTargetStops > MaximumExposureStops) ? 1u : 0u;

    // The exposure applied to the image the viewer sees now. On a reused-history frame it is exactly what the
    // previous frame committed and owes nothing to this frame's measurement.
    float currentStops;
    if (!reset)
    {
        currentStops = history.committedStops;
    }
    else if (measurementValid)
    {
        currentStops = targetStops;
        record.status |= ExposureSeededFromTarget;
    }
    else
    {
        // A reset frame whose measurement was refused has nothing to seed from, so it adopts the declared seed
        // rather than inventing a luminance that was never observed.
        currentStops = clamp(ResetSeedStops, MinimumExposureStops, MaximumExposureStops);
        record.status |= ExposureSeededFromDeclaredStops;
    }

    float alpha;
    if (!measurementValid)
    {
        // Nothing was measured, so the loop holds. Holding is a decision, and it is published rather than disguised
        // as a measurement that happened to agree with the previous frame.
        targetStops = currentStops;
        unclampedTargetStops = currentStops;
        alpha = reset ? 1.0f : 0.0f;
        record.status |= ExposureHeldOnRefusedMeasurement;
    }
    else
    {
        alpha = 1.0f;
    }

    uint direction = DirectionSteady;
    if (targetStops > currentStops)
    {
        direction = DirectionExposureIncreasing;
    }
    else if (targetStops < currentStops)
    {
        direction = DirectionExposureDecreasing;
    }
    // Named from what the exposure does, not from what the scene does: a scene going dark makes the exposure go up.
    float speed = (direction == DirectionExposureDecreasing) ? ExposureDecreaseSpeed : ExposureIncreaseSpeed;
    if (!reset && measurementValid)
    {
        alpha = SmoothingAlpha(speed, FrameDeltaSeconds);
    }

    // Adaptation happens in stops, so moving one stop takes the same time whether the scene is a candle or a desert.
    // Smoothing the linear scale instead would make the two directions take wildly different times for the same
    // perceptual distance.
    float blended = currentStops + (alpha * (targetStops - currentStops));
    float committedStops = clamp(blended, MinimumExposureStops, MaximumExposureStops);

    record.direction = direction;
    record.alpha = alpha;
    record.currentStops = currentStops;
    record.displayedStops = currentStops;
    record.displayedScale = exp2(currentStops);
    record.targetStops = targetStops;
    record.targetUnclampedStops = unclampedTargetStops;
    record.targetScale = exp2(targetStops);
    record.meteredLuminance = meteredLuminance;
    record.exposedMeteredLuminance = meteredLuminance * exp2(targetStops);
    record.committedStops = committedStops;
    record.committedScale = exp2(committedStops);
    record.clampedToMinimum = blended < MinimumExposureStops ? 1u : 0u;
    record.clampedToMaximum = blended > MaximumExposureStops ? 1u : 0u;
    record.netScaleFromStored = record.displayedScale / preExposure;

    // The next frame's pre-exposure. Note the direction: the stored value is radiance times the exposure the frame
    // will be displayed with, not radiance divided by it. Writing the reciprocal here is the classic bug, and it
    // makes the storage scale move the wrong way in exactly the scenes where it matters.
    float requestedPreExposure = (PreExposureModeId == PreExposureFixed) ? FixedPreExposure : record.committedScale;
    float nextPreExposure = clamp(requestedPreExposure, MinimumPreExposure, MaximumPreExposure);
    record.preExposureClampedToMinimum = requestedPreExposure < MinimumPreExposure ? 1u : 0u;
    record.preExposureClampedToMaximum = requestedPreExposure > MaximumPreExposure ? 1u : 0u;
    record.nextPreExposure = nextPreExposure;
    record.previousToNextScale = nextPreExposure / preExposure;
    Exposure[0] = record;

    ExposureHistorySlot next;
    next.valid = 1u;
    // The frame that produced this state. The exposure it holds is applied to frame producerFrameIndex + 1, which is
    // the whole of the one-frame delay written down.
    next.producerFrameIndex = FrameIndex;
    next.configurationIdentityLow = ConfigurationIdentityLow;
    next.configurationIdentityHigh = ConfigurationIdentityHigh;
    next.mode = ExposureModeId;
    next.preExposureValid = 1u;
    next.committedStops = committedStops;
    next.nextPreExposure = nextPreExposure;
    History[HistoryWriteSlot] = next;
}

// ---------------------------------------------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------------------------------------------

// Identical to the Starter's compose stage, and deliberately so: adding a feedback loop must not change what
// "apply the exposure" means. The exposure it reads is the displayed exposure, which is the value the previous
// frame committed; this frame's measurement is not in this function at all.
[numthreads(8, 8, 1)] void ComposeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    uint2 extent = uint2(DisplayWidth, DisplayHeight);
    uint2 pixel = dispatchThreadId.xy;
    uint index = TexelIndex(extent, pixel);

    SceneRecord scene = Scene[index];
    ExposureRecord exposure = Exposure[0];
    PixelRecord record = Records[index];

    float3 stored = float3(scene.radianceR, scene.radianceG, scene.radianceB);
    float preExposure = scene.preExposure;
    float exposureScale = exposure.displayedScale;
    record.displayedExposureScale = exposureScale;
    record.displayedExposureStops = exposure.displayedStops;
    record.netScaleFromStored = exposureScale / preExposure;

    bool storedIsLegal = IsFiniteColor(stored) && stored.r >= 0.0f && stored.g >= 0.0f && stored.b >= 0.0f &&
                         stored.r <= MaximumSceneLinearValue && stored.g <= MaximumSceneLinearValue &&
                         stored.b <= MaximumSceneLinearValue;
    if (!storedIsLegal)
    {
        record.status |= StatusExposureRefused;
        record.finalR = 1.0f;
        record.finalG = 0.0f;
        record.finalB = 1.0f;
        Records[index] = record;
        return;
    }

    float3 absolute = stored / preExposure;
    record.absoluteR = absolute.r;
    record.absoluteG = absolute.g;
    record.absoluteB = absolute.b;
    // The absolute luminance and its logarithm are not republished here. HistogramCS already wrote the values the
    // measurement was actually taken on, and recomputing them from this pass's division would overwrite the
    // metering evidence with a differently rounded copy of it.
    record.status |= StatusPreExposureRemoved;

    float3 exposed = absolute * exposureScale;
    record.exposedR = exposed.r;
    record.exposedG = exposed.g;
    record.exposedB = exposed.b;
    record.exposureApplicationCount += 1u;
    record.status |= StatusExposure;

    float3 displayLinear = ToneMap(exposed);
    record.toneMappedR = displayLinear.r;
    record.toneMappedG = displayLinear.g;
    record.toneMappedB = displayLinear.b;
    record.status |= StatusToneMap;

    float3 encoded = EncodeDisplay(displayLinear);
    record.encodedR = encoded.r;
    record.encodedG = encoded.g;
    record.encodedB = encoded.b;
    record.status |= StatusDisplayEncode;

    float3 uiColor;
    float uiAlpha;
    EvaluateUi(extent, pixel, uiColor, uiAlpha);
    record.uiR = uiColor.r;
    record.uiG = uiColor.g;
    record.uiB = uiColor.b;
    record.uiAlpha = uiAlpha;

    float3 finalColor = encoded;
    if ((Flags & FlagUiEnabled) != 0u)
    {
        finalColor = CompositeUi(encoded, uiColor, uiAlpha);
        record.status |= StatusUi;
    }
    record.finalR = finalColor.r;
    record.finalG = finalColor.g;
    record.finalB = finalColor.b;
    Records[index] = record;
}
