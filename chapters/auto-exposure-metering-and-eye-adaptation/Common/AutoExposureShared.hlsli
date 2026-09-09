// Chapter 31 shared GPU lab support.
//
// This file owns the parts of the lab that are not the lesson: the constant-buffer ABI, the resource declarations,
// the record layouts the C++ side mirrors, the deterministic analytic scene, the artist mask, and the display path
// that Chapter 4 already taught. It deliberately owns no histogram, no reduction, no metering policy, no target
// exposure, and no adaptation step. Those live in each variant's own AutoExposureLab.hlsl, because a shared helper
// that quietly built the histogram would leave the learner with a chapter about calling a function.
//
// Two conventions hold everywhere below:
//
//   * A stored scene-linear sample is pre-exposed: it is the radiance the renderer meant, multiplied by the frame
//     pre-exposure. The pre-exposure is removed exactly once, by the learner-owned code that needs an absolute
//     luminance, and nothing here divides by it.
//   * stops = log2(scale). Positive brightens. This file never uses the term EV100 for any of its quantities.

#ifndef LGP_CH31_AUTO_EXPOSURE_SHARED_HLSLI
#define LGP_CH31_AUTO_EXPOSURE_SHARED_HLSLI

// Mirrors ch31::auto_exposure::gpu::LabStage.
static const uint StageClear = 0u;
static const uint StageScene = 1u;
static const uint StageHistogram = 2u;
static const uint StageReduce = 3u;
static const uint StageMeter = 4u;
static const uint StageDeclaredExposure = 5u;
static const uint StageAdapt = 6u;
static const uint StageCompose = 7u;

// Mirrors ch31::auto_exposure::gpu configuration flag bits.
static const uint FlagUseCentreWeighting = 1u << 0u;
static const uint FlagUseMask = 1u << 1u;
static const uint FlagCameraCut = 1u << 2u;
static const uint FlagUiEnabled = 1u << 3u;
static const uint FlagResetHistory = 1u << 4u;

// Mirrors ch31::auto_exposure::gpu status bits.
static const uint StatusScene = 1u << 0u;
static const uint StatusWeighted = 1u << 1u;
static const uint StatusClassified = 1u << 2u;
static const uint StatusPreExposureRemoved = 1u << 3u;
static const uint StatusExposure = 1u << 4u;
static const uint StatusToneMap = 1u << 5u;
static const uint StatusDisplayEncode = 1u << 6u;
static const uint StatusUi = 1u << 7u;
static const uint StatusExposureRefused = 1u << 8u;

static const uint MeterEvaluated = 1u << 0u;
static const uint MeterNoAcceptedSamples = 1u << 1u;
static const uint MeterNoPositiveLuminance = 1u << 2u;
static const uint MeterDegenerateWindow = 1u << 3u;
static const uint MeterUsedExactStatistic = 1u << 4u;
static const uint MeterBinCentreBounded = 1u << 5u;

static const uint ExposureEvaluated = 1u << 0u;
static const uint ExposureReusedHistory = 1u << 1u;
static const uint ExposureHeldOnRefusedMeasurement = 1u << 2u;
static const uint ExposureSeededFromTarget = 1u << 3u;
static const uint ExposureSeededFromDeclaredStops = 1u << 4u;
static const uint ExposureManualMode = 1u << 5u;

static const uint OverflowTileWeight = 1u << 0u;
static const uint OverflowBinWeight = 1u << 1u;
static const uint OverflowTotalWeight = 1u << 2u;

// Mirrors ch31::auto_exposure::AdaptationReset.
static const uint ResetNoHistory = 1u << 0u;
static const uint ResetFirstFrame = 1u << 1u;
static const uint ResetCameraCut = 1u << 2u;
static const uint ResetNonSequentialProducerFrame = 1u << 3u;
static const uint ResetConfigurationChanged = 1u << 4u;
static const uint ResetExposureModeChanged = 1u << 5u;
static const uint ResetInvalidHistoryValue = 1u << 6u;

// Mirrors ch31::auto_exposure::BinClass.
static const uint BinClassInterior = 0u;
static const uint BinClassBelowRange = 1u;
static const uint BinClassAboveRange = 2u;
static const uint BinClassZero = 3u;

// Mirrors ch31::auto_exposure::SampleRejection, index for index.
static const uint RejectNone = 0u;
static const uint RejectNonFiniteChannel = 1u;
static const uint RejectNegativeChannel = 2u;
static const uint RejectLuminanceOutOfDomain = 3u;
static const uint RejectZeroWeight = 4u;
static const uint RejectZeroLuminance = 5u;
static const uint RejectBelowRange = 6u;
static const uint RejectAboveRange = 7u;

// Mirrors ch31::auto_exposure::BlackSamplePolicy and RangeSamplePolicy.
static const uint BlackCountInLowestBin = 0u;
static const uint BlackReject = 1u;
static const uint RangeCountInEndBin = 0u;
static const uint RangeReject = 1u;

// Mirrors ch31::auto_exposure::MeteringPolicy.
static const uint PolicyArithmeticMean = 0u;
static const uint PolicyLogAverage = 1u;
static const uint PolicyPercentileWindow = 2u;

// Mirrors ch31::auto_exposure::ExposureMode, PreExposureMode and AdaptationDirection.
static const uint ModeAutomatic = 0u;
static const uint ModeManual = 1u;
static const uint PreExposureMatchCommitted = 0u;
static const uint PreExposureFixed = 1u;
static const uint DirectionSteady = 0u;
static const uint DirectionExposureIncreasing = 1u;
static const uint DirectionExposureDecreasing = 2u;

static const uint SceneCameraLab = 0u;
static const uint SceneConstantField = 1u;
static const uint SceneSplitField = 2u;
static const uint SceneCentreEdgeField = 3u;
static const uint SceneMaskedField = 4u;
static const uint SceneLadderField = 5u;
static const uint SceneBlackField = 6u;
static const uint SceneInvalidProbeField = 7u;

static const uint MaskFullFrame = 0u;
static const uint MaskCentreRectangle = 1u;
static const uint MaskHalfLeft = 2u;

static const uint TransferLinear = 0u;
static const uint TransferSrgb = 1u;
static const uint TransferGamma22 = 2u;

// Mirrors the contract's fixed-point units and bounds.
static const uint WeightOne = 1024u;
static const uint PercentileOne = 1000000u;
static const uint MaximumBinCount = 256u;
static const uint HistogramGroupThreads = 64u;
static const uint MaximumTileWeight = HistogramGroupThreads * WeightOne;
static const float MaximumSceneLinearValue = 1.0e6f;
// The contract's limit on |stops|, which is log2 of the exposure-scale limit and nothing else. It is a different
// quantity from the authored MinimumExposureStops and MaximumExposureStops bounds in the constant buffer below.
static const float ExposureStopsLimit = 19.931568f;
static const uint AbiMarker = 0x41453331u;

cbuffer LabConstants : register(b0)
{
    uint DisplayWidth;
    uint DisplayHeight;
    uint Flags;
    uint SceneVariantId;
    uint MaskVariantId;
    uint BinCount;
    uint BlackSamplePolicyId;
    uint BelowRangePolicyId;
    uint AboveRangePolicyId;
    uint MeteringPolicyId;
    uint ExposureModeId;
    uint PreExposureModeId;
    uint DisplayTransferFunction;
    uint FrameIndex;
    uint AnimationFrame;
    uint OutlierColumnCount;
    uint LowerPercentileFixed;
    uint UpperPercentileFixed;
    uint ConfigurationIdentityLow;
    uint ConfigurationIdentityHigh;
    uint HistoryReadSlot;
    uint HistoryWriteSlot;
    uint TileCount;
    uint TileCountX;
    uint StageOrderWord;
    uint StageCount;
    float MinimumLog2Luminance;
    float MaximumLog2Luminance;
    float CentreWeightValue;
    float EdgeWeightValue;
    float CentreFalloffPower;
    float MiddleGreyLuminance;
    float ExposureCompensationStops;
    float MinimumExposureStops;
    float MaximumExposureStops;
    float ExposureIncreaseSpeed;
    float ExposureDecreaseSpeed;
    float ManualExposureStops;
    float ResetSeedStops;
    float InitialPreExposure;
    float FixedPreExposure;
    float MinimumPreExposure;
    float MaximumPreExposure;
    float FrameDeltaSeconds;
    float BaseLuminance;
    float OutlierLuminance;
    float CentreLuminance;
    float EdgeLuminance;
    float MaskedLuminance;
    float BackgroundLuminance;
    float LadderBinOffset;
    float UiAlphaValue;
    float UiColorR;
    float UiColorG;
    float UiColorB;
};

struct PixelRecord
{
    float sceneR;
    float sceneG;
    float sceneB;
    float storedLuminance;
    float absoluteLuminance;
    float log2Luminance;
    float preExposure;
    float maskValue;
    float absoluteR;
    float absoluteG;
    float absoluteB;
    float exposedR;
    float exposedG;
    float exposedB;
    float toneMappedR;
    float toneMappedG;
    float toneMappedB;
    float encodedR;
    float encodedG;
    float encodedB;
    float uiR;
    float uiG;
    float uiB;
    float uiAlpha;
    float finalR;
    float finalG;
    float finalB;
    float displayedExposureScale;
    float displayedExposureStops;
    float netScaleFromStored;
    float centreWeightUnit;
    float sampleWeightUnit;
    uint centreWeight;
    uint maskWeight;
    uint sampleWeight;
    uint bin;
    uint classification;
    uint rejection;
    uint accepted;
    uint hasLog2Luminance;
    uint exposureApplicationCount;
    uint stageOrderWord;
    uint status;
    uint abiMarker;
};

struct SceneRecord
{
    float radianceR;
    float radianceG;
    float radianceB;
    float preExposure;
    uint sampleWeight;
    uint centreWeight;
    uint maskWeight;
    uint padding;
};

struct HistogramBin
{
    uint weight;
    uint sampleCount;
};

struct TilePartial
{
    float weightedLuminanceSum;
    float weightedLog2LuminanceSum;
    float minimumObservedLuminance;
    float maximumObservedLuminance;
    uint hasAcceptedSample;
    uint log2AccumulatedWeight;
    uint acceptedWeight;
    uint acceptedSampleCount;
};

struct HistogramStatisticsRecord
{
    uint acceptedSampleCount;
    uint acceptedWeight;
    uint rejectedSampleCount;
    uint rejectedWeight;
    uint belowRangeSampleCount;
    uint belowRangeWeight;
    uint aboveRangeSampleCount;
    uint aboveRangeWeight;
    uint zeroLuminanceSampleCount;
    uint zeroLuminanceWeight;
    uint log2AccumulatedWeight;
    uint hasAcceptedSample;
    uint rejectedNone;
    uint rejectedNonFiniteChannel;
    uint rejectedNegativeChannel;
    uint rejectedLuminanceOutOfDomain;
    uint rejectedZeroWeight;
    uint rejectedZeroLuminance;
    uint rejectedBelowRange;
    uint rejectedAboveRange;
    uint tileCount;
    uint overflowFlags;
    uint abiMarker;
    uint padding;
    float weightedLuminanceSum;
    float weightedLog2LuminanceSum;
    float minimumObservedLuminance;
    float maximumObservedLuminance;
};

struct MeterRecord
{
    uint policy;
    uint status;
    uint firstWindowBin;
    uint lastWindowBin;
    uint windowWeight;
    uint lowerTargetWeight;
    uint upperTargetWeight;
    uint logAverageWeight;
    uint occupiedBinCount;
    uint lowestOccupiedBin;
    uint highestOccupiedBin;
    uint hasOccupiedBin;
    uint clippedIntoLowestBin;
    uint clippedIntoHighestBin;
    uint excludedZeroLuminanceFromLogAverage;
    uint abiMarker;
    float meteredLuminance;
    float meteredLog2Luminance;
    float binCentreLog2Luminance;
    float binQuantizationLog2Bound;
    float windowLowerLog2Luminance;
    float windowUpperLog2Luminance;
    float minimumObservedLuminance;
    float maximumObservedLuminance;
};

struct ExposureRecord
{
    uint frameIndex;
    uint producerFrameIndex;
    uint resets;
    uint direction;
    uint status;
    uint mode;
    uint historyValid;
    uint reusedHistory;
    uint clampedToMinimum;
    uint clampedToMaximum;
    uint targetClampedToMinimum;
    uint targetClampedToMaximum;
    uint preExposureClampedToMinimum;
    uint preExposureClampedToMaximum;
    uint configurationIdentityLow;
    uint configurationIdentityHigh;
    uint historyIdentityLow;
    uint historyIdentityHigh;
    uint abiMarker;
    uint padding;
    float displayedStops;
    float displayedScale;
    float targetStops;
    float targetUnclampedStops;
    float targetScale;
    float exposedMeteredLuminance;
    float committedStops;
    float committedScale;
    float currentStops;
    float alpha;
    float meteredLuminance;
    float preExposure;
    float nextPreExposure;
    float previousToNextScale;
    float netScaleFromStored;
    float padding2;
};

struct ExposureHistorySlot
{
    uint valid;
    uint producerFrameIndex;
    uint configurationIdentityLow;
    uint configurationIdentityHigh;
    uint mode;
    uint preExposureValid;
    float committedStops;
    float nextPreExposure;
};

RWStructuredBuffer<PixelRecord> Records : register(u0);
RWStructuredBuffer<SceneRecord> Scene : register(u1);
RWStructuredBuffer<HistogramBin> Histogram : register(u2);
// One entry per histogram tile, so the two real-valued sums are reduced in a declared order instead of by a
// floating-point atomic whose result would depend on which group finished first.
RWStructuredBuffer<TilePartial> Partials : register(u3);
RWStructuredBuffer<HistogramStatisticsRecord> Statistics : register(u4);
RWStructuredBuffer<MeterRecord> Meter : register(u5);
RWStructuredBuffer<ExposureRecord> Exposure : register(u6);
// Sequence-owned, not frame-slot-owned: slot HistoryWriteSlot receives this frame's committed exposure and slot
// HistoryReadSlot holds what the previous frame committed.
RWStructuredBuffer<ExposureHistorySlot> History : register(u7);

float SceneLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

uint TexelIndex(uint2 extent, uint2 pixel)
{
    return (pixel.y * extent.x) + pixel.x;
}

bool IsFiniteColor(float3 color)
{
    return isfinite(color.r) && isfinite(color.g) && isfinite(color.b);
}

// ---------------------------------------------------------------------------------------------------------------
// The artist mask. It is authored data, not an algorithm: every variant is defined on integer pixel comparisons and
// takes values that are exact multiples of 1 / WeightOne, so the CPU that declares the mask identity and the
// dispatch that measures under it can never disagree about a quantized weight.
// ---------------------------------------------------------------------------------------------------------------

bool InsideCentreRectangle(uint2 extent, uint2 pixel)
{
    return (4u * pixel.x) >= extent.x && (4u * pixel.x) < (3u * extent.x) && (4u * pixel.y) >= extent.y &&
           (4u * pixel.y) < (3u * extent.y);
}

float MaskValue(uint2 extent, uint2 pixel)
{
    if ((Flags & FlagUseMask) == 0u)
    {
        return 1.0f;
    }
    if (MaskVariantId == MaskCentreRectangle)
    {
        return InsideCentreRectangle(extent, pixel) ? 1.0f : 0.0f;
    }
    if (MaskVariantId == MaskHalfLeft)
    {
        return (2u * pixel.x) < extent.x ? 0.5f : 1.0f;
    }
    return 1.0f;
}

// ---------------------------------------------------------------------------------------------------------------
// The analytic scene. Every variant is a controlled luminance distribution, and every one of them is a neutral grey
// so that its Rec.709 luminance is the number the variant's name promises.
// ---------------------------------------------------------------------------------------------------------------

// A deterministic two-stop walk with period five. It is a step function rather than a sinusoid so that a test can
// name the exact luminance of frame n without reproducing a trigonometric identity.
float LabAnimationScale(uint frame)
{
    return exp2(0.5f * float(frame % 5u));
}

float3 Grey(float luminance)
{
    return float3(luminance, luminance, luminance);
}

float3 EvaluateCameraLab(uint2 extent, uint2 pixel)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(float(extent.x), float(extent.y));
    float scale = LabAnimationScale(AnimationFrame);
    // A bright sky band, a dim ground, a lit wall, one lamp far above any plausible middle grey, and a patch of
    // true black so the black-sample policy has something to decide about in the picture the chapter teaches with.
    float luminance = 0.35f;
    if (uv.y < 0.30f)
    {
        luminance = 6.0f;
    }
    else if (uv.y > 0.78f)
    {
        luminance = 0.05f;
    }
    if (uv.x > 0.80f && uv.y > 0.30f && uv.y < 0.78f)
    {
        luminance = 900.0f;
    }
    if (uv.x < 0.10f && uv.y > 0.55f)
    {
        luminance = 0.0f;
    }
    return Grey(luminance * scale);
}

float3 EvaluateScene(uint2 extent, uint2 pixel)
{
    if (SceneVariantId == SceneConstantField)
    {
        return Grey(BaseLuminance);
    }
    if (SceneVariantId == SceneSplitField)
    {
        bool outlier = pixel.x + OutlierColumnCount >= extent.x;
        return Grey(outlier ? OutlierLuminance : BaseLuminance);
    }
    if (SceneVariantId == SceneCentreEdgeField)
    {
        return Grey(InsideCentreRectangle(extent, pixel) ? CentreLuminance : EdgeLuminance);
    }
    if (SceneVariantId == SceneMaskedField)
    {
        return Grey(InsideCentreRectangle(extent, pixel) ? MaskedLuminance : BackgroundLuminance);
    }
    if (SceneVariantId == SceneLadderField)
    {
        // Column x lands in bin x % BinCount, at LadderBinOffset of a bin above that bin's lower edge. Nothing sits
        // on a bin centre, which is exactly what makes a bin-centre reconstruction distinguishable from the exact
        // sufficient statistic the reduction accumulates.
        uint bin = pixel.x % BinCount;
        float span = MaximumLog2Luminance - MinimumLog2Luminance;
        float width = span / float(BinCount);
        float log2Luminance = MinimumLog2Luminance + (span * float(bin)) / float(BinCount) + (LadderBinOffset * width);
        return Grey(exp2(log2Luminance));
    }
    if (SceneVariantId == SceneBlackField)
    {
        return Grey(0.0f);
    }
    if (SceneVariantId == SceneInvalidProbeField)
    {
        if (pixel.y == 0u)
        {
            // Declared probes, one per failure the contract distinguishes. They are authored as data because a
            // frame buffer with a NaN in it is a rendering bug, and metering is the cheapest place to notice one.
            if (pixel.x == 0u)
            {
                return Grey(asfloat(0x7FC00000u));
            }
            if (pixel.x == 1u)
            {
                return Grey(-1.0f);
            }
            if (pixel.x == 2u)
            {
                return Grey(asfloat(0x7F800000u));
            }
            if (pixel.x == 3u)
            {
                return Grey(2.0f * MaximumSceneLinearValue);
            }
            if (pixel.x == 4u)
            {
                return Grey(0.0f);
            }
            if (pixel.x == 5u)
            {
                return Grey(exp2(MinimumLog2Luminance - 4.0f));
            }
            if (pixel.x == 6u)
            {
                return Grey(exp2(MaximumLog2Luminance + 2.0f));
            }
        }
        return Grey(BaseLuminance);
    }
    return EvaluateCameraLab(extent, pixel);
}

// ---------------------------------------------------------------------------------------------------------------
// Display path. Chapter 4 owns tone-curve design and Chapter 30 owns compositing order; this lab needs one curve
// whose output lands in the unit range so the display encoding has a legal domain, and says which one it uses.
// ---------------------------------------------------------------------------------------------------------------

float3 ToneMap(float3 exposedSceneLinear)
{
    return saturate(exposedSceneLinear / (1.0f + exposedSceneLinear));
}

float EncodeChannel(float value)
{
    if (DisplayTransferFunction == TransferSrgb)
    {
        return value <= 0.0031308f ? 12.92f * value : (1.055f * pow(value, 1.0f / 2.4f)) - 0.055f;
    }
    if (DisplayTransferFunction == TransferGamma22)
    {
        return pow(value, 1.0f / 2.2f);
    }
    return value;
}

float3 EncodeDisplay(float3 displayLinear)
{
    return float3(EncodeChannel(displayLinear.r), EncodeChannel(displayLinear.g), EncodeChannel(displayLinear.b));
}

void EvaluateUi(uint2 extent, uint2 pixel, out float3 color, out float alpha)
{
    color = float3(UiColorR, UiColorG, UiColorB);
    float2 uv = (float2(pixel) + 0.5f) / float2(float(extent.x), float(extent.y));
    bool inside = uv.x < 0.30f && uv.y < 0.12f;
    alpha = ((Flags & FlagUiEnabled) != 0u && inside) ? UiAlphaValue : 0.0f;
}

float3 CompositeUi(float3 destination, float3 uiColor, float uiAlpha)
{
    return (uiColor * uiAlpha) + (destination * (1.0f - uiAlpha));
}

// ---------------------------------------------------------------------------------------------------------------
// Shared pass bodies that carry no algorithm: zeroing the frame's accumulators, and publishing the scene.
// ---------------------------------------------------------------------------------------------------------------

// Every accumulator the frame will add into starts at a known value, so a readback describes this frame and not a
// residue of the previous one. The history slots are cleared only when the frame declares a reset, because clearing
// them unconditionally would destroy the very state the one-frame delay depends on.
void ClearFrameState(uint index)
{
    if (index < MaximumBinCount)
    {
        HistogramBin bin;
        bin.weight = 0u;
        bin.sampleCount = 0u;
        Histogram[index] = bin;
    }
    if (index < TileCount)
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
        Partials[index] = partial;
    }
    if (index == 0u)
    {
        HistogramStatisticsRecord statistics = (HistogramStatisticsRecord)0;
        statistics.tileCount = TileCount;
        statistics.abiMarker = AbiMarker;
        Statistics[0] = statistics;

        MeterRecord meter = (MeterRecord)0;
        meter.policy = MeteringPolicyId;
        meter.abiMarker = AbiMarker;
        Meter[0] = meter;

        ExposureRecord exposure = (ExposureRecord)0;
        exposure.frameIndex = FrameIndex;
        exposure.abiMarker = AbiMarker;
        Exposure[0] = exposure;
    }
    if ((Flags & FlagResetHistory) != 0u && index < 2u)
    {
        ExposureHistorySlot slot = (ExposureHistorySlot)0;
        History[index] = slot;
    }
}

// The pre-exposure this frame stores its radiance under. It was chosen by the previous frame from the exposure that
// frame committed, which is why a correctly exposed pixel is stored near middle grey whatever the scene's absolute
// scale is. A reset frame has nothing to inherit and uses the declared seed instead.
float FramePreExposure()
{
    if (PreExposureModeId == PreExposureFixed)
    {
        return clamp(FixedPreExposure, MinimumPreExposure, MaximumPreExposure);
    }
    ExposureHistorySlot slot = History[HistoryReadSlot];
    if ((Flags & FlagResetHistory) != 0u || slot.preExposureValid == 0u || !isfinite(slot.nextPreExposure) ||
        slot.nextPreExposure < MinimumPreExposure || slot.nextPreExposure > MaximumPreExposure)
    {
        return clamp(InitialPreExposure, MinimumPreExposure, MaximumPreExposure);
    }
    return slot.nextPreExposure;
}

// Publishes the only screen data the rest of the frame is allowed to see: pre-exposed scene-linear radiance and the
// authored mask value, together with the pre-exposure the samples were stored under.
void WriteSceneRecord(uint2 pixel, float preExposure)
{
    uint2 extent = uint2(DisplayWidth, DisplayHeight);
    uint index = TexelIndex(extent, pixel);
    float3 absolute = EvaluateScene(extent, pixel);
    float3 stored = absolute * preExposure;
    float maskValue = MaskValue(extent, pixel);

    SceneRecord scene;
    scene.radianceR = stored.r;
    scene.radianceG = stored.g;
    scene.radianceB = stored.b;
    scene.preExposure = preExposure;
    scene.sampleWeight = 0u;
    scene.centreWeight = 0u;
    scene.maskWeight = 0u;
    scene.padding = 0u;
    Scene[index] = scene;

    PixelRecord record = (PixelRecord)0;
    record.sceneR = stored.r;
    record.sceneG = stored.g;
    record.sceneB = stored.b;
    record.storedLuminance = SceneLuminance(stored);
    record.preExposure = preExposure;
    record.maskValue = maskValue;
    record.stageOrderWord = StageOrderWord;
    record.abiMarker = AbiMarker;
    record.status = StatusScene;
    Records[index] = record;
}

// ---------------------------------------------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------------------------------------------

struct FullscreenVertex
{
    float4 position : SV_Position;
};

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenVertex output;
    output.position = float4(vertexId == 2u ? 3.0f : -1.0f, vertexId == 1u ? 3.0f : -1.0f, 0.0f, 1.0f);
    return output;
}

cbuffer DisplayConstants : register(b0)
{
    uint DisplayViewWidth;
    uint DisplayViewHeight;
    uint SelectedDebugView;
    uint ExpectedStatus;
    uint DisplayBinCount;
    float DisplayMinimumLog2Luminance;
    float DisplayMaximumLog2Luminance;
    float DisplayExposureStopRange;
};

StructuredBuffer<PixelRecord> DisplayRecords : register(t0);
StructuredBuffer<HistogramBin> DisplayHistogram : register(t1);
StructuredBuffer<MeterRecord> DisplayMeter : register(t2);
StructuredBuffer<ExposureRecord> DisplayExposure : register(t3);

float NormalizeStops(float stops)
{
    return saturate(0.5f + (stops / (2.0f * max(DisplayExposureStopRange, 1.0e-3f))));
}

float NormalizeLog2Luminance(float log2Luminance)
{
    float span = max(DisplayMaximumLog2Luminance - DisplayMinimumLog2Luminance, 1.0e-3f);
    return saturate((log2Luminance - DisplayMinimumLog2Luminance) / span);
}

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
    uint2 pixel = min(uint2(position.xy), uint2(DisplayViewWidth - 1u, DisplayViewHeight - 1u));
    PixelRecord record = DisplayRecords[(pixel.y * DisplayViewWidth) + pixel.x];
    MeterRecord meter = DisplayMeter[0];
    ExposureRecord exposure = DisplayExposure[0];
    if ((record.status & ExpectedStatus) != ExpectedStatus || record.abiMarker != AbiMarker)
    {
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    }

    // The final view is already display-encoded. Every other view is a diagnostic that maps its own quantity into
    // the unit range explicitly, so nothing below pretends to be a second tone curve.
    if (SelectedDebugView == 0u)
    {
        return float4(record.finalR, record.finalG, record.finalB, 1.0f);
    }

    float3 result = float3(0.0f, 0.0f, 0.0f);
    float2 uv = (float2(pixel) + 0.5f) / float2(float(DisplayViewWidth), float(DisplayViewHeight));
    if (SelectedDebugView == 1u)
    {
        float3 scene = float3(record.sceneR, record.sceneG, record.sceneB);
        result = scene / (1.0f + scene);
    }
    else if (SelectedDebugView == 2u)
    {
        result = NormalizeLog2Luminance(record.log2Luminance).xxx;
        if (record.hasLog2Luminance == 0u)
        {
            result = float3(0.0f, 0.0f, 1.0f);
        }
    }
    else if (SelectedDebugView == 3u)
    {
        result = float3(record.centreWeightUnit, record.sampleWeightUnit, record.maskValue);
    }
    else if (SelectedDebugView == 4u)
    {
        // Bin occupancy as a column chart, normalized by the busiest bin so an empty layout is still legible.
        uint bin = min(uint(uv.x * float(DisplayBinCount)), DisplayBinCount - 1u);
        uint busiest = 1u;
        for (uint search = 0u; search < DisplayBinCount; ++search)
        {
            busiest = max(busiest, DisplayHistogram[search].weight);
        }
        float height = float(DisplayHistogram[bin].weight) / float(busiest);
        result = (1.0f - uv.y) <= height ? float3(0.15f, 0.85f, 0.35f) : float3(0.05f, 0.05f, 0.08f);
    }
    else if (SelectedDebugView == 5u)
    {
        uint bin = min(uint(uv.x * float(DisplayBinCount)), DisplayBinCount - 1u);
        bool inside = meter.hasOccupiedBin != 0u && bin >= meter.firstWindowBin && bin <= meter.lastWindowBin;
        bool occupied = DisplayHistogram[bin].weight > 0u;
        result = inside ? float3(0.95f, 0.75f, 0.20f) : (occupied ? float3(0.20f, 0.35f, 0.55f) : float3(0.05f, 0.05f, 0.08f));
    }
    else if (SelectedDebugView == 6u)
    {
        float metered = NormalizeLog2Luminance(meter.meteredLog2Luminance);
        float binCentre = NormalizeLog2Luminance(meter.binCentreLog2Luminance);
        result = float3(metered, binCentre, (meter.status & MeterEvaluated) != 0u ? 0.0f : 1.0f);
    }
    else if (SelectedDebugView == 7u)
    {
        result = float3(NormalizeStops(exposure.displayedStops), NormalizeStops(exposure.targetStops),
                        NormalizeStops(exposure.committedStops));
    }
    else if (SelectedDebugView == 8u)
    {
        result = float3(exposure.direction == DirectionExposureIncreasing ? 1.0f : 0.0f,
                        exposure.direction == DirectionExposureDecreasing ? 1.0f : 0.0f, saturate(exposure.alpha));
    }
    else if (SelectedDebugView == 9u)
    {
        result = float3(meter.clippedIntoLowestBin != 0u ? 1.0f : 0.0f,
                        meter.clippedIntoHighestBin != 0u ? 1.0f : 0.0f,
                        record.classification == BinClassZero ? 1.0f : 0.0f);
    }
    else if (SelectedDebugView == 10u)
    {
        result = float3(exposure.resets != 0u ? 1.0f : 0.0f, exposure.reusedHistory != 0u ? 1.0f : 0.0f,
                        float(min(exposure.resets, 127u)) / 127.0f);
    }
    else if (SelectedDebugView == 11u)
    {
        result = float3(saturate(log2(max(record.preExposure, 1.0e-8f)) / 16.0f + 0.5f),
                        saturate(log2(max(record.netScaleFromStored, 1.0e-8f)) / 16.0f + 0.5f),
                        saturate(exposure.previousToNextScale));
    }
    else
    {
        result = float3(record.encodedR, record.encodedG, record.encodedB);
    }
    return float4(saturate(result), 1.0f);
}

#endif // LGP_CH31_AUTO_EXPOSURE_SHARED_HLSLI
