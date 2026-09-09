// Shared Chapter 30 GPU contract for post-processing camera effects and final compositing.
//
// Every quantity here is either scene-linear radiance, a signed display-pixel displacement, a linear view depth in
// metres, or a lens quantity in millimetres. The one place a value leaves the linear domain is EncodeDisplay, and
// the one place camera exposure is applied is FinishFrame. The formulas mirror ch30::post_processing exactly so a
// test can replay the CPU reference against the recorded GPU evidence instead of re-deriving it.

// Pass selectors.
static const uint PassScene = 0u;
static const uint PassDepthOfField = 1u;
static const uint PassMotionBlur = 2u;
static const uint PassBloomExtract = 3u;
static const uint PassBloomDownsample = 4u;
static const uint PassBloomUpsample = 5u;
static const uint PassCompose = 6u;

// Constant flags.
static const uint FlagShutterMidpoint = 1u << 0u;
static const uint FlagClampShutterToBudget = 1u << 1u;
static const uint FlagBloomEnabled = 1u << 2u;
static const uint FlagDepthOfFieldEnabled = 1u << 3u;
static const uint FlagMotionBlurEnabled = 1u << 4u;
static const uint FlagUiEnabled = 1u << 5u;
static const uint FlagUiBeforeEncode = 1u << 6u;
static const uint FlagCreativeCrossfade = 1u << 7u;
static const uint FlagBloomTopLevel = 1u << 8u;

// Per-pixel status bits. A stage only sets its bit after it has published finite, in-domain evidence, so the
// display pass can reject a partially written pixel instead of drawing a plausible colour.
static const uint StatusScene = 1u << 0u;
static const uint StatusDepthOfField = 1u << 1u;
static const uint StatusMotionBlur = 1u << 2u;
static const uint StatusBloom = 1u << 3u;
static const uint StatusComposition = 1u << 4u;
static const uint StatusExposure = 1u << 5u;
static const uint StatusToneMap = 1u << 6u;
static const uint StatusDisplayEncode = 1u << 7u;
static const uint StatusUi = 1u << 8u;

// Depth-of-field record flags.
static const uint DofInsufficientFarCoverage = 1u << 0u;
static const uint DofClampedByBudget = 1u << 1u;
static const uint DofEvaluated = 1u << 2u;

// Motion-blur record flags.
static const uint MotionExceedsBudget = 1u << 0u;
static const uint MotionClampedByBudget = 1u << 1u;
static const uint MotionIncludesCenterSample = 1u << 2u;
static const uint MotionFellBackToCenter = 1u << 3u;
static const uint MotionEvaluated = 1u << 4u;

// Bloom record flags.
static const uint BloomBelowKnee = 1u << 0u;
static const uint BloomAboveKnee = 1u << 1u;
static const uint BloomWeightClamped = 1u << 2u;
static const uint BloomEvaluated = 1u << 3u;

// Focus regions, matching ch30::post_processing::FocusRegion.
static const uint FocusRegionInFocus = 0u;
static const uint FocusRegionNear = 1u;
static const uint FocusRegionFar = 2u;

// Transfer functions, matching ch30::post_processing::TransferFunction.
static const uint TransferLinear = 0u;
static const uint TransferSrgb = 1u;
static const uint TransferGamma22 = 2u;

// Scene variants.
static const uint SceneCameraLab = 0u;
static const uint SceneConstantField = 1u;
static const uint SceneSplitField = 2u;

static const uint MaximumApertureSamples = 32u;
static const uint MaximumMotionBlurSamples = 32u;
// A tap whose circle of confusion is smaller than this deposits as a one-pixel point, which is what keeps the
// scatter-as-gather weight 1 / radius^2 finite for a perfectly focused tap.
static const float MinimumScatterRadiusPixels = 0.5f;
// The converged limit of the Chapter 28 temporal resolve: the analytic scene integrated over the jitter sequence.
// This lab consumes a resolved image; it does not re-implement temporal reconstruction.
static const uint ResolveSampleCount = 4u;
static const uint AbiMarker = 0x50503330u;

struct PixelRecord
{
    float sceneR, sceneG, sceneB, sceneLuminance;
    float viewDepthMetres, motionX, motionY, coverageAlpha;
    float cocSignedRadiusPixels, cocClampedRadiusPixels, cocDiameterMillimetres, cocApertureDiameterMillimetres;
    float cocPixelsPerMillimetre, farGatherRadiusPixels, nearGatherRadiusPixels, nearCoverage;
    float farCoverage, farWeightSum, nearWeightSum, dofFarR;
    float dofFarG, dofFarB, dofNearR, dofNearG;
    float dofNearB, dofOutR, dofOutG, dofOutB;
    float apertureFirstX, apertureFirstY, apertureLastX, apertureLastY;
    float frameDisplacementX, frameDisplacementY, shutterDisplacementX, shutterDisplacementY;
    float shutterAppliedScale, shutterGatherRadiusPixels, shutterCentroidParameter, shutterFirstParameter;
    float shutterLastParameter, motionWeightSum, motionBlurR, motionBlurG;
    float motionBlurB, bloomPreExposedLuminance, bloomAbsoluteLuminance, bloomExcessLuminance;
    float bloomThresholdWeight, bloomExtractedR, bloomExtractedG, bloomExtractedB;
    float bloomContributionR, bloomContributionG, bloomContributionB, baseR;
    float baseG, baseB, composedR, composedG;
    float composedB, composedBaseFraction, composedBloomFraction, composedDiscardedBaseLuminance;
    float composedOutputLuminance, absoluteR, absoluteG, absoluteB;
    float exposedR, exposedG, exposedB, exposureAppliedScale;
    float toneMappedR, toneMappedG, toneMappedB, encodedR;
    float encodedG, encodedB, uiR, uiG;
    float uiB, uiAlpha, finalR, finalG;
    float finalB;
    uint cocRegion, dofSampleCount, dofFarAcceptedCount, dofNearAcceptedCount;
    uint dofRejectedOutOfBounds, dofRejectedOutsideCoc, dofRejectedOwnedByNearField, dofFlags;
    uint motionSampleCount, motionAcceptedCount, motionRejectedOutOfBounds, motionRejectedForeground;
    uint motionRejectedBackground, motionFlags, bloomFlags, bloomLevelCount;
    uint exposureApplicationCount, stageOrderWord, stageCount, status;
    uint abiMarker;
};

// The only screen data the post chain is allowed to see. Depth and motion are produced by the raster passes and are
// never filtered: only radiance is.
struct SceneRecord
{
    float radianceR, radianceG, radianceB, viewDepthMetres;
    float motionX, motionY, coverageAlpha, padding;
};

cbuffer PostConstants : register(b0)
{
    uint DisplayWidth;
    uint DisplayHeight;
    uint SourceOffset;
    uint SourceWidth;
    uint SourceHeight;
    uint DestinationOffset;
    uint DestinationWidth;
    uint DestinationHeight;
    uint PassKind;
    uint Flags;
    uint AnimationFrame;
    uint SceneVariant;
    uint ApertureSampleCount;
    uint MotionSampleCount;
    uint BloomLevelCount;
    uint BloomLevelOffset;
    uint StageOrderWord;
    uint StageCount;
    uint DisplayTransferFunction;
    float PreExposure;
    float ExposureScale;
    float ExposureTimeFraction;
    float NearPlaneMetres;
    float FarPlaneMetres;
    float BloomThresholdLuminance;
    float BloomSoftKneeLuminance;
    float BloomLevelWeight;
    float BloomGain;
    float BloomIntensity;
    float FocalLengthMillimetres;
    float FNumber;
    float FocusDistanceMetres;
    float SensorHeightMillimetres;
    float MaximumCocRadiusPixels;
    float InFocusRadiusPixels;
    float NearFieldSearchRadiusPixels;
    float MinimumFarCoverage;
    float DofDepthCompareAbsoluteMetres;
    float DofDepthCompareRelative;
    float MaximumShutterDisplacementPixels;
    float MotionDepthCompareAbsoluteMetres;
    float MotionDepthCompareRelative;
    float UiAlpha;
    float UiColorR;
    float UiColorG;
    float UiColorB;
    float ConstantRadianceR;
    float ConstantRadianceG;
    float ConstantRadianceB;
    float ConstantViewDepthMetres;
    float ConstantMotionU;
    float ConstantMotionV;
    float SplitNearDepthMetres;
    float SplitRadianceScale;
    float SplitMotionScale;
};

RWStructuredBuffer<PixelRecord> Records : register(u0);
RWStructuredBuffer<SceneRecord> Scene : register(u1);
// Two display-sized halves selected by SourceOffset and DestinationOffset, so a pass never aliases its own input.
RWStructuredBuffer<float4> Chain : register(u2);
// The extracted pyramid followed by the progressive-combine accumulator, both addressed by explicit level offsets.
RWStructuredBuffer<float4> Bloom : register(u3);

float SceneLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

uint TexelIndex(uint2 extent, uint2 pixel)
{
    return (pixel.y * extent.x) + pixel.x;
}

// A tap lands in the texel that contains the offset position, which keeps a zero offset on the centre texel
// exactly. Matches OffsetTexel in the CPU reference.
bool OffsetTexel(uint2 extent, uint2 center, float2 offsetPixels, out uint2 texel)
{
    texel = uint2(0u, 0u);
    float x = float(center.x) + 0.5f + offsetPixels.x;
    float y = float(center.y) + 0.5f + offsetPixels.y;
    if (!isfinite(x) || !isfinite(y))
    {
        return false;
    }
    if (x < 0.0f || y < 0.0f)
    {
        return false;
    }
    float flooredX = floor(x);
    float flooredY = floor(y);
    if (flooredX >= float(extent.x) || flooredY >= float(extent.y))
    {
        return false;
    }
    texel = uint2(uint(flooredX), uint(flooredY));
    return true;
}

float RadicalInverse(uint index, uint base)
{
    float result = 0.0f;
    float inverseBase = 1.0f / float(base);
    float fraction = inverseBase;
    uint value = index;
    while (value > 0u)
    {
        result += float(value % base) * fraction;
        value /= base;
        fraction *= inverseBase;
    }
    return result;
}

// Centroid-corrected Halton, exactly as Chapter 28 generates its jitter: the mean offset over the sequence is
// removed so a finite resolve is not biased away from the pixel centre.
float2 CentroidCorrectedHaltonOffset(uint sample, uint sampleCount)
{
    float2 mean = float2(0.0f, 0.0f);
    for (uint phase = 0u; phase < sampleCount; ++phase)
    {
        mean += float2(RadicalInverse(phase + 1u, 2u), RadicalInverse(phase + 1u, 3u)) - 0.5f;
    }
    mean /= float(sampleCount);
    return float2(RadicalInverse(sample + 1u, 2u), RadicalInverse(sample + 1u, 3u)) - 0.5f - mean;
}

float RadicalInverseBase2(uint index)
{
    uint bits = index;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

// ---------------------------------------------------------------------------------------------------------------
// Analytic scene. The lab owns one deterministic screen picture and never consults it again after the scene pass.
// ---------------------------------------------------------------------------------------------------------------

struct SceneSample
{
    float3 radiance;
    float viewDepthMetres;
    float2 motionPreviousMinusCurrentUv;
    float coverageAlpha;
};

float2 MovingBarCenter(uint frame)
{
    float t = float(frame) * 0.21f;
    return float2(0.5f + 0.18f * sin(t), 0.55f);
}

SceneSample EvaluateCameraLab(float2 uv, uint frame)
{
    SceneSample sample;
    sample.radiance = float3(0.05f, 0.06f, 0.09f);
    sample.viewDepthMetres = 24.0f;
    sample.motionPreviousMinusCurrentUv = float2(0.0f, 0.0f);
    sample.coverageAlpha = 1.0f;

    if (uv.x > 0.85f)
    {
        // A distant wall, far behind any plausible focus plane.
        sample.radiance = float3(0.30f, 0.30f, 0.35f);
        sample.viewDepthMetres = 40.0f;
    }
    if (uv.x < 0.16f && uv.x > 0.08f)
    {
        // A near post, in front of any plausible focus plane, so the near field has something to spread.
        sample.radiance = float3(0.15f, 0.50f, 0.25f);
        sample.viewDepthMetres = 0.8f;
    }
    float2 highlightLocal = uv - float2(0.72f, 0.28f);
    if (dot(highlightLocal, highlightLocal) < 0.05f * 0.05f)
    {
        // The only radiance in the picture well above a unit threshold, so bloom has honest highlights to extract.
        sample.radiance = float3(30.0f, 26.0f, 18.0f);
        sample.viewDepthMetres = 6.0f;
    }
    float2 barCenter = MovingBarCenter(frame);
    float2 barLocal = uv - barCenter;
    if (abs(barLocal.x) < 0.05f && abs(barLocal.y) < 0.20f)
    {
        sample.radiance = float3(0.90f, 0.35f, 0.20f);
        sample.viewDepthMetres = 3.0f;
        float2 previousCenter = MovingBarCenter(frame > 0u ? frame - 1u : 0u);
        sample.motionPreviousMinusCurrentUv = previousCenter - barCenter;
    }
    if (uv.x < 0.06f && uv.y < 0.06f)
    {
        // A hand-checkable constant patch inside the rich scene.
        sample.radiance = float3(ConstantRadianceR, ConstantRadianceG, ConstantRadianceB);
        sample.viewDepthMetres = ConstantViewDepthMetres;
        sample.motionPreviousMinusCurrentUv = float2(0.0f, 0.0f);
    }
    return sample;
}

SceneSample EvaluateScene(float2 uv, uint frame)
{
    if (SceneVariant == SceneConstantField)
    {
        SceneSample sample;
        sample.radiance = float3(ConstantRadianceR, ConstantRadianceG, ConstantRadianceB);
        sample.viewDepthMetres = ConstantViewDepthMetres;
        sample.motionPreviousMinusCurrentUv = float2(ConstantMotionU, ConstantMotionV);
        sample.coverageAlpha = 1.0f;
        return sample;
    }
    if (SceneVariant == SceneSplitField)
    {
        SceneSample sample;
        sample.coverageAlpha = 1.0f;
        bool nearHalf = uv.x < 0.5f;
        sample.radiance = float3(ConstantRadianceR, ConstantRadianceG, ConstantRadianceB);
        sample.viewDepthMetres = nearHalf ? SplitNearDepthMetres : ConstantViewDepthMetres;
        if (!nearHalf)
        {
            sample.radiance *= SplitRadianceScale;
        }
        // The two slabs move at different speeds, so a gather that crosses the split meets both a foreground whose
        // own trajectory may or may not cover this pixel and a background that never passed in front of it. Which
        // slab moves faster is configurable, because both cases occur and they fail differently.
        float slabScale = nearHalf ? 1.0f : SplitMotionScale;
        sample.motionPreviousMinusCurrentUv = float2(ConstantMotionU, ConstantMotionV) * slabScale;
        return sample;
    }
    return EvaluateCameraLab(uv, frame);
}

// The UI layer is authored at display resolution and is never filtered by anything that follows it.
void EvaluateUi(uint2 pixel, out float3 color, out float alpha)
{
    color = float3(UiColorR, UiColorG, UiColorB);
    float2 uv = (float2(pixel) + 0.5f) / float2(float(DisplayWidth), float(DisplayHeight));
    bool inside = uv.x < 0.30f && uv.y < 0.12f;
    alpha = ((Flags & FlagUiEnabled) != 0u && inside) ? UiAlpha : 0.0f;
}

// ---------------------------------------------------------------------------------------------------------------
// Bloom
// ---------------------------------------------------------------------------------------------------------------

struct BloomExtraction
{
    float preExposedLuminance;
    float absoluteLuminance;
    float excessLuminance;
    float thresholdWeight;
    float3 extractedPreExposed;
    uint flags;
};

// Matches ExtractBloomHighlights: the threshold is compared against the pre-exposure-free luminance, so moving the
// pre-exposure never changes which highlights bloom.
BloomExtraction ExtractBloomHighlights(float3 preExposedSceneLinear, float preExposure, float threshold, float knee)
{
    BloomExtraction extraction;
    extraction.preExposedLuminance = SceneLuminance(preExposedSceneLinear);
    extraction.absoluteLuminance = extraction.preExposedLuminance / preExposure;
    extraction.flags = BloomEvaluated;

    float luminance = extraction.absoluteLuminance;
    bool belowKnee = luminance <= threshold - knee;
    bool aboveKnee = luminance >= threshold + knee;
    if (belowKnee)
    {
        extraction.excessLuminance = 0.0f;
        extraction.flags |= BloomBelowKnee;
    }
    else if (aboveKnee)
    {
        extraction.excessLuminance = luminance - threshold;
        extraction.flags |= BloomAboveKnee;
    }
    else
    {
        // Only reachable for a positive knee width: a zero knee makes the two branches above exhaustive.
        float kneeInput = luminance - threshold + knee;
        extraction.excessLuminance = (kneeInput * kneeInput) / (4.0f * knee);
    }

    float rawWeight = luminance > 0.0f ? extraction.excessLuminance / luminance : 0.0f;
    extraction.thresholdWeight = saturate(rawWeight);
    if (extraction.thresholdWeight != rawWeight)
    {
        extraction.flags |= BloomWeightClamped;
    }
    extraction.extractedPreExposed = preExposedSceneLinear * extraction.thresholdWeight;
    return extraction;
}

struct DownsampleFootprint1D
{
    uint tapCount;
    uint indices[3];
    float weights[3];
};

// Exact area resampling in integer arithmetic: destination texel j owns [j * source, (j + 1) * source) in units of
// one destination texel. The weights sum to one for every legal extent pair, including odd ones.
DownsampleFootprint1D BloomDownsampleFootprint(uint destinationIndex, uint sourceExtent, uint destinationExtent)
{
    DownsampleFootprint1D footprint;
    footprint.tapCount = 0u;
    footprint.indices[0] = 0u;
    footprint.indices[1] = 0u;
    footprint.indices[2] = 0u;
    footprint.weights[0] = 0.0f;
    footprint.weights[1] = 0.0f;
    footprint.weights[2] = 0.0f;

    uint start = destinationIndex * sourceExtent;
    uint end = start + sourceExtent;
    uint index = start / destinationExtent;
    [loop] while (index < sourceExtent && (index * destinationExtent) < end && footprint.tapCount < 3u)
    {
        uint texelStart = index * destinationExtent;
        uint texelEnd = texelStart + destinationExtent;
        uint overlap = min(end, texelEnd) - max(start, texelStart);
        if (overlap > 0u)
        {
            footprint.indices[footprint.tapCount] = index;
            footprint.weights[footprint.tapCount] = float(overlap) / float(sourceExtent);
            ++footprint.tapCount;
        }
        ++index;
    }
    return footprint;
}

struct UpsampleFootprint1D
{
    uint lowIndex;
    uint highIndex;
    float lowWeight;
    float highWeight;
};

// Bilinear reconstruction at destination texel centres with clamp-to-edge borders. The two weights sum to one, so
// upsampling cannot change the level of a constant.
UpsampleFootprint1D BloomUpsampleFootprint(uint destinationIndex, uint sourceExtent, uint destinationExtent)
{
    float scale = float(sourceExtent) / float(destinationExtent);
    float coordinate = ((float(destinationIndex) + 0.5f) * scale) - 0.5f;
    float flooredCoordinate = floor(coordinate);
    float fraction = coordinate - flooredCoordinate;
    float highestIndex = float(sourceExtent) - 1.0f;

    UpsampleFootprint1D footprint;
    footprint.lowIndex = uint(clamp(flooredCoordinate, 0.0f, highestIndex));
    footprint.highIndex = uint(clamp(flooredCoordinate + 1.0f, 0.0f, highestIndex));
    footprint.lowWeight = 1.0f - fraction;
    footprint.highWeight = fraction;
    return footprint;
}

// ---------------------------------------------------------------------------------------------------------------
// Motion blur
// ---------------------------------------------------------------------------------------------------------------

// The single place the previousUV - currentUV convention becomes a forward, signed, display-pixel displacement.
float2 MotionToDisplayPixels(float2 motionPreviousMinusCurrentUv, uint2 displayExtent)
{
    return -motionPreviousMinusCurrentUv * float2(float(displayExtent.x), float(displayExtent.y));
}

struct ShutterSchedule
{
    float2 frameDisplacementPixels;
    float2 shutterDisplacementPixels;
    float appliedScale;
    float gatherRadiusPixels;
    uint flags;
};

ShutterSchedule BuildShutterSchedule(float2 motionPreviousMinusCurrentUv, uint2 displayExtent)
{
    ShutterSchedule schedule;
    schedule.flags = 0u;
    schedule.frameDisplacementPixels = MotionToDisplayPixels(motionPreviousMinusCurrentUv, displayExtent);
    float2 shutterDisplacement = schedule.frameDisplacementPixels * ExposureTimeFraction;
    float shutterLength = length(shutterDisplacement);
    schedule.appliedScale = 1.0f;
    if (shutterLength > MaximumShutterDisplacementPixels)
    {
        schedule.flags |= MotionExceedsBudget;
        if ((Flags & FlagClampShutterToBudget) != 0u)
        {
            schedule.appliedScale = MaximumShutterDisplacementPixels / shutterLength;
            shutterDisplacement *= schedule.appliedScale;
            schedule.flags |= MotionClampedByBudget;
        }
    }
    schedule.shutterDisplacementPixels = shutterDisplacement;
    schedule.gatherRadiusPixels = 0.5f * length(shutterDisplacement);
    return schedule;
}

// Both schedules are centred on the current frame time: the mean parameter is exactly zero, so a symmetric shutter
// cannot shift the image.
float ShutterParameter(uint sample, uint sampleCount)
{
    if ((Flags & FlagShutterMidpoint) != 0u)
    {
        return ((float(sample) + 0.5f) / float(sampleCount)) - 0.5f;
    }
    if (sampleCount == 1u)
    {
        return 0.0f;
    }
    return -0.5f + (float(sample) / float(sampleCount - 1u));
}

// ---------------------------------------------------------------------------------------------------------------
// Depth of field
// ---------------------------------------------------------------------------------------------------------------

struct CircleOfConfusion
{
    float apertureDiameterMillimetres;
    float diameterMillimetres;
    float pixelsPerMillimetre;
    float signedRadiusPixels;
    float clampedSignedRadiusPixels;
    uint region;
    bool clampedByBudget;
};

// c = A * f * |z - S| / (z * (S - f)) millimetres on the sensor, with A = f / N. Scene distances arrive in metres
// and are converted exactly once. Negative in the near field, positive in the far field.
CircleOfConfusion ComputeCircleOfConfusion(float viewDepthMetres, uint2 displayExtent)
{
    float focalLength = FocalLengthMillimetres;
    float apertureDiameter = focalLength / FNumber;
    float focusDistance = FocusDistanceMetres * 1000.0f;
    float subjectDistance = viewDepthMetres * 1000.0f;
    float diameter = (apertureDiameter * focalLength * abs(subjectDistance - focusDistance)) /
                     (subjectDistance * (focusDistance - focalLength));
    float pixelsPerMillimetre = float(displayExtent.y) / SensorHeightMillimetres;
    float sign = subjectDistance >= focusDistance ? 1.0f : -1.0f;
    float signedRadius = sign * 0.5f * diameter * pixelsPerMillimetre;

    CircleOfConfusion coc;
    coc.apertureDiameterMillimetres = apertureDiameter;
    coc.diameterMillimetres = diameter;
    coc.pixelsPerMillimetre = pixelsPerMillimetre;
    coc.signedRadiusPixels = signedRadius;
    coc.clampedSignedRadiusPixels = clamp(signedRadius, -MaximumCocRadiusPixels, MaximumCocRadiusPixels);
    coc.clampedByBudget = coc.clampedSignedRadiusPixels != signedRadius;
    if (abs(signedRadius) <= InFocusRadiusPixels)
    {
        coc.region = FocusRegionInFocus;
    }
    else
    {
        coc.region = signedRadius < 0.0f ? FocusRegionNear : FocusRegionFar;
    }
    return coc;
}

// Shirley and Chiu's concentric mapping: area preserving and continuous, so equal-area regions of the square carry
// equal aperture area. A naive polar mapping would bunch samples at the centre and bias every bokeh disc.
float2 MapConcentricDisk(float2 unitSquareSample)
{
    float a = (2.0f * unitSquareSample.x) - 1.0f;
    float b = (2.0f * unitSquareSample.y) - 1.0f;
    if (a == 0.0f && b == 0.0f)
    {
        return float2(0.0f, 0.0f);
    }
    const float quarterPi = 0.78539816339744831f;
    float radius;
    float angle;
    if ((a * a) > (b * b))
    {
        radius = a;
        angle = quarterPi * (b / a);
    }
    else
    {
        radius = b;
        angle = (2.0f * quarterPi) - (quarterPi * (a / b));
    }
    return float2(radius * cos(angle), radius * sin(angle));
}

// Deterministic Hammersley points offset to cell centres, so no sample lands on the degenerate square centre.
float2 ApertureDiskSample(uint index, uint sampleCount)
{
    float2 unitSquare = float2((float(index) + 0.5f) / float(sampleCount), RadicalInverseBase2(index));
    return MapConcentricDisk(unitSquare);
}

// ---------------------------------------------------------------------------------------------------------------
// Exposure, tone mapping, display encoding, and UI
// ---------------------------------------------------------------------------------------------------------------

// Chapter 4 owns tone-curve design. This lab needs one curve whose output is guaranteed to land in [0, 1] so the
// display encoding has a legal domain, so it uses the plain Reinhard compression and says so.
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

// Straight-alpha source-over composition, in whichever domain the policy chose.
float3 CompositeUi(float3 destination, float3 uiColor, float uiAlpha)
{
    return (uiColor * uiAlpha) + (destination * (1.0f - uiAlpha));
}

// ---------------------------------------------------------------------------------------------------------------
// Shared stage bodies
// ---------------------------------------------------------------------------------------------------------------

// The converged temporal resolve this chapter consumes. Radiance is filtered over the jitter sequence; depth and
// motion are point evidence and are read at the unjittered pixel centre.
void WriteSceneRecord(uint2 pixel)
{
    uint2 extent = uint2(DisplayWidth, DisplayHeight);
    uint index = TexelIndex(extent, pixel);
    float2 inverseExtent = 1.0f / float2(float(DisplayWidth), float(DisplayHeight));

    float3 resolved = float3(0.0f, 0.0f, 0.0f);
    [unroll] for (uint sample = 0u; sample < ResolveSampleCount; ++sample)
    {
        float2 jitter = CentroidCorrectedHaltonOffset(sample, ResolveSampleCount);
        float2 uv = (float2(pixel) + 0.5f + jitter) * inverseExtent;
        resolved += EvaluateScene(uv, AnimationFrame).radiance;
    }
    resolved /= float(ResolveSampleCount);

    float2 centerUv = (float2(pixel) + 0.5f) * inverseExtent;
    SceneSample centerSample = EvaluateScene(centerUv, AnimationFrame);
    float3 preExposed = resolved * PreExposure;

    SceneRecord scene;
    scene.radianceR = preExposed.r;
    scene.radianceG = preExposed.g;
    scene.radianceB = preExposed.b;
    scene.viewDepthMetres = centerSample.viewDepthMetres;
    scene.motionX = centerSample.motionPreviousMinusCurrentUv.x;
    scene.motionY = centerSample.motionPreviousMinusCurrentUv.y;
    scene.coverageAlpha = centerSample.coverageAlpha;
    scene.padding = 0.0f;
    Scene[index] = scene;
    Chain[DestinationOffset + index] = float4(preExposed, 0.0f);

    PixelRecord record = (PixelRecord)0;
    record.sceneR = preExposed.r;
    record.sceneG = preExposed.g;
    record.sceneB = preExposed.b;
    record.sceneLuminance = SceneLuminance(preExposed);
    record.viewDepthMetres = scene.viewDepthMetres;
    record.motionX = scene.motionX;
    record.motionY = scene.motionY;
    record.coverageAlpha = scene.coverageAlpha;
    record.stageOrderWord = StageOrderWord;
    record.stageCount = StageCount;
    record.abiMarker = AbiMarker;
    record.status = StatusScene;
    Records[index] = record;
}

// Composition, exposure, tone mapping, display encoding, and UI composition. Exposure is applied here and nowhere
// else, and the counter records that fact per pixel rather than asserting it.
void FinishFrame(uint2 pixel, float3 baseSceneLinear, float3 bloomContribution)
{
    uint index = TexelIndex(uint2(DisplayWidth, DisplayHeight), pixel);
    PixelRecord record = Records[index];

    record.baseR = baseSceneLinear.r;
    record.baseG = baseSceneLinear.g;
    record.baseB = baseSceneLinear.b;
    record.bloomContributionR = bloomContribution.r;
    record.bloomContributionG = bloomContribution.g;
    record.bloomContributionB = bloomContribution.b;

    float3 scaledBloom = bloomContribution * BloomIntensity;
    float baseFraction = (Flags & FlagCreativeCrossfade) != 0u ? 1.0f - BloomIntensity : 1.0f;
    float3 composed = (baseSceneLinear * baseFraction) + scaledBloom;
    record.composedR = composed.r;
    record.composedG = composed.g;
    record.composedB = composed.b;
    record.composedBaseFraction = baseFraction;
    record.composedBloomFraction = BloomIntensity;
    record.composedDiscardedBaseLuminance = SceneLuminance(baseSceneLinear) * (1.0f - baseFraction);
    record.composedOutputLuminance = SceneLuminance(composed);
    record.status |= StatusComposition;

    float3 absolute = composed / PreExposure;
    float3 exposed = absolute * ExposureScale;
    record.absoluteR = absolute.r;
    record.absoluteG = absolute.g;
    record.absoluteB = absolute.b;
    record.exposedR = exposed.r;
    record.exposedG = exposed.g;
    record.exposedB = exposed.b;
    record.exposureAppliedScale = ExposureScale / PreExposure;
    record.exposureApplicationCount += 1u;
    record.status |= StatusExposure;

    float3 displayLinear = ToneMap(exposed);
    record.toneMappedR = displayLinear.r;
    record.toneMappedG = displayLinear.g;
    record.toneMappedB = displayLinear.b;
    record.status |= StatusToneMap;

    float3 uiColor;
    float uiAlpha;
    EvaluateUi(pixel, uiColor, uiAlpha);
    record.uiR = uiColor.r;
    record.uiG = uiColor.g;
    record.uiB = uiColor.b;
    record.uiAlpha = uiAlpha;

    // The UI is authored in whichever domain the policy composites it in, so nothing re-encodes it on the way in.
    bool uiEnabled = (Flags & FlagUiEnabled) != 0u;
    bool uiBeforeEncode = uiEnabled && (Flags & FlagUiBeforeEncode) != 0u;
    if (uiBeforeEncode)
    {
        displayLinear = CompositeUi(displayLinear, uiColor, uiAlpha);
        record.status |= StatusUi;
    }
    float3 encoded = EncodeDisplay(displayLinear);
    record.encodedR = encoded.r;
    record.encodedG = encoded.g;
    record.encodedB = encoded.b;
    record.status |= StatusDisplayEncode;

    float3 finalColor = encoded;
    if (uiEnabled && !uiBeforeEncode)
    {
        finalColor = CompositeUi(encoded, uiColor, uiAlpha);
        record.status |= StatusUi;
    }
    record.finalR = finalColor.r;
    record.finalG = finalColor.g;
    record.finalB = finalColor.b;
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
    float DisplayMaximumCocRadiusPixels;
    float DisplayFarPlaneMetres;
};

StructuredBuffer<PixelRecord> DisplayRecords : register(t0);

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
    uint2 pixel = min(uint2(position.xy), uint2(DisplayViewWidth - 1u, DisplayViewHeight - 1u));
    PixelRecord record = DisplayRecords[(pixel.y * DisplayViewWidth) + pixel.x];
    if ((record.status & ExpectedStatus) != ExpectedStatus || record.abiMarker != AbiMarker)
    {
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    }

    // The final view is already display-encoded. Every other view is a diagnostic that maps its own quantity into
    // the unit range explicitly, so nothing here pretends to be a second tone curve.
    if (SelectedDebugView == 0u)
    {
        return float4(record.finalR, record.finalG, record.finalB, 1.0f);
    }

    float3 result = float3(0.0f, 0.0f, 0.0f);
    if (SelectedDebugView == 1u)
    {
        float3 scene = float3(record.sceneR, record.sceneG, record.sceneB);
        result = scene / (1.0f + scene);
    }
    else if (SelectedDebugView == 2u)
    {
        result = saturate(record.viewDepthMetres / DisplayFarPlaneMetres).xxx;
    }
    else if (SelectedDebugView == 3u)
    {
        result = float3(0.5f + record.motionX * 8.0f, 0.5f + record.motionY * 8.0f, 0.5f);
    }
    else if (SelectedDebugView == 4u)
    {
        float normalized = record.cocSignedRadiusPixels / max(DisplayMaximumCocRadiusPixels, 1.0e-4f);
        result = float3(saturate(-normalized), saturate(normalized), record.cocRegion == FocusRegionInFocus ? 1.0f : 0.0f);
    }
    else if (SelectedDebugView == 5u)
    {
        float3 dof = float3(record.dofOutR, record.dofOutG, record.dofOutB);
        result = dof / (1.0f + dof);
    }
    else if (SelectedDebugView == 6u)
    {
        float3 blurred = float3(record.motionBlurR, record.motionBlurG, record.motionBlurB);
        result = blurred / (1.0f + blurred);
    }
    else if (SelectedDebugView == 7u)
    {
        float3 extracted = float3(record.bloomExtractedR, record.bloomExtractedG, record.bloomExtractedB);
        result = extracted / (1.0f + extracted);
    }
    else if (SelectedDebugView == 8u)
    {
        float3 bloom = float3(record.bloomContributionR, record.bloomContributionG, record.bloomContributionB);
        result = bloom / (1.0f + bloom);
    }
    else if (SelectedDebugView == 9u)
    {
        float3 composed = float3(record.composedR, record.composedG, record.composedB);
        result = composed / (1.0f + composed);
    }
    else if (SelectedDebugView == 10u)
    {
        // The exposed radiance clipped rather than tone mapped, so the view shows exactly which pixels the tone
        // curve still has to bring into range instead of showing the tone curve twice.
        result = saturate(float3(record.exposedR, record.exposedG, record.exposedB));
    }
    else if (SelectedDebugView == 11u)
    {
        result = float3(record.toneMappedR, record.toneMappedG, record.toneMappedB);
    }
    else
    {
        result = float3(record.encodedR, record.encodedG, record.encodedB);
    }
    return float4(saturate(result), 1.0f);
}
