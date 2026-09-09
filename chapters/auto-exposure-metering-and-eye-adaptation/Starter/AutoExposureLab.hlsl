// Chapter 31 Starter: an honest, declared exposure and nothing that pretends to measure one.
//
// The whole point of this file is that it contains no auto exposure at all. There is no histogram, no reduction, no
// metering policy, no target, and no adaptation, and there is no shared helper hiding one: the exposure this frame
// applies is the number the configuration declared, and the evidence the frame publishes is enough to prove that.
//
// What the Starter does own is the part an auto-exposure system must not get wrong before it is worth measuring
// anything:
//
//   * The scene is stored pre-exposed. Radiance is multiplied by a declared, constant pre-exposure so a 16-bit
//     target would keep its mantissa, and that pre-exposure is published per pixel.
//   * The pre-exposure is removed exactly once, in ComposeCS, by dividing by the value the scene pass recorded.
//   * The display exposure is applied exactly once, immediately after that division, and the pixel counts the
//     application so a later stage cannot apply it again without the count saying so.
//   * The tone curve, the display encoding, and the UI composite follow, in that order.
//
// The Solution replaces ExposureCS with a real feedback loop and adds the four stages before it. Nothing else in
// this file has to change, which is the shape the chapter's patches follow.

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

// One thread publishes the exposure the frame will display. It reads no histogram, no statistics, and no history,
// because a declared exposure has nothing to read: it is the authored value, clamped to the authored bounds.
//
// The record still carries a committed exposure and a next pre-exposure, and both are the same declared value. That
// is not a feedback loop pretending to be idle; it is the loop's vocabulary filled in by a stage that measured
// nothing, and the status word says so.
[numthreads(1, 1, 1)] void ExposureCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x != 0u)
    {
        return;
    }

    float declaredStops = clamp(ManualExposureStops, MinimumExposureStops, MaximumExposureStops);
    float declaredScale = exp2(declaredStops);
    float preExposure = clamp(FixedPreExposure, MinimumPreExposure, MaximumPreExposure);

    ExposureRecord record = (ExposureRecord)0;
    record.frameIndex = FrameIndex;
    record.producerFrameIndex = FrameIndex;
    record.resets = 0u;
    record.direction = DirectionSteady;
    record.status = ExposureEvaluated | ExposureManualMode | ExposureSeededFromDeclaredStops;
    record.mode = ModeManual;
    record.historyValid = 0u;
    record.reusedHistory = 0u;
    record.clampedToMinimum = ManualExposureStops < MinimumExposureStops ? 1u : 0u;
    record.clampedToMaximum = ManualExposureStops > MaximumExposureStops ? 1u : 0u;
    record.targetClampedToMinimum = record.clampedToMinimum;
    record.targetClampedToMaximum = record.clampedToMaximum;
    record.preExposureClampedToMinimum = FixedPreExposure < MinimumPreExposure ? 1u : 0u;
    record.preExposureClampedToMaximum = FixedPreExposure > MaximumPreExposure ? 1u : 0u;
    record.configurationIdentityLow = ConfigurationIdentityLow;
    record.configurationIdentityHigh = ConfigurationIdentityHigh;
    record.historyIdentityLow = 0u;
    record.historyIdentityHigh = 0u;
    record.abiMarker = AbiMarker;
    record.displayedStops = declaredStops;
    record.displayedScale = declaredScale;
    record.targetStops = declaredStops;
    record.targetUnclampedStops = ManualExposureStops;
    record.targetScale = declaredScale;
    record.exposedMeteredLuminance = 0.0f;
    record.committedStops = declaredStops;
    record.committedScale = declaredScale;
    record.currentStops = declaredStops;
    record.alpha = 0.0f;
    // Nothing was measured. Publishing a zero here rather than a plausible luminance is what makes the Starter's
    // honesty checkable: a metered luminance of zero is not a measurement, and no test can mistake it for one.
    record.meteredLuminance = 0.0f;
    record.preExposure = preExposure;
    record.nextPreExposure = preExposure;
    record.previousToNextScale = 1.0f;
    record.netScaleFromStored = declaredScale / preExposure;
    Exposure[0] = record;

    // The declared exposure is also what the next frame will store its radiance under, so the history slot records
    // it. The producer frame index is written too, because a later chapter step turns this stage into the real loop
    // and the resource it writes must already be the resource that loop reads.
    ExposureHistorySlot slot;
    slot.valid = 1u;
    slot.producerFrameIndex = FrameIndex;
    slot.configurationIdentityLow = ConfigurationIdentityLow;
    slot.configurationIdentityHigh = ConfigurationIdentityHigh;
    slot.mode = ModeManual;
    slot.preExposureValid = 1u;
    slot.committedStops = declaredStops;
    slot.nextPreExposure = preExposure;
    History[HistoryWriteSlot] = slot;
}

// Removes the storage scale, applies the display exposure once, tone maps, encodes, and composites the UI.
//
// The order matters and is visible: the division by the pre-exposure recovers the radiance the renderer meant, the
// multiply by the exposure is the camera, and the tone curve only ever sees exposed radiance. Doing the two scales
// as one combined multiply would be faster and would also make it impossible to tell, from the published record,
// whether the exposure had been applied twice.
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

    // The same domain check the contract's ApplyDisplayExposure performs. A sample it would refuse is reported as a
    // refusal and painted with the magenta marker, not turned into a NaN the display would silently swallow.
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
    record.absoluteLuminance = SceneLuminance(absolute);
    record.log2Luminance = record.absoluteLuminance > 0.0f ? log2(record.absoluteLuminance) : 0.0f;
    record.hasLog2Luminance = record.absoluteLuminance > 0.0f ? 1u : 0u;
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
