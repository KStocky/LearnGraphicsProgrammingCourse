// Chapter 30 Solution: an explicit, bounded post chain over a scene-linear resolved baseline.
//
// The chain is a sequence of separate dispatches whose order the renderer submits from a validated pipeline plan:
// the camera effects run in the declared order, bloom extracts from whichever image the declared policy names, the
// pyramid is built and progressively recombined on exact area footprints, and the output path composes, exposes
// once, tone maps, encodes, and composites the UI. Every stage reads the previous chain half and writes the other,
// so no pass ever aliases its own input.
#include "../Common/PostProcessingShared.hlsli"

[numthreads(8, 8, 1)] void SceneCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    WriteSceneRecord(dispatchThreadId.xy);
}

// Scatter as gather. A tap deposits its radiance uniformly over its own circle of confusion, so it reaches only
// pixels inside that radius and delivers 1 / radius^2 per unit area. The near field is separated from the far
// field because it is unbounded, spreads over whatever is behind it, and needs a coverage fraction rather than a
// colour; every other tap, including a defocused surface nearer than the centre but still behind the focus plane,
// belongs to the far field and is judged only by whether its own circle reaches this pixel.
void EvaluateDepthOfFieldPass(uint2 pixel)
{
    uint2 extent = uint2(DisplayWidth, DisplayHeight);
    uint centerIndex = TexelIndex(extent, pixel);
    float3 centerColor = Chain[SourceOffset + centerIndex].rgb;
    float centerDepth = Scene[centerIndex].viewDepthMetres;
    CircleOfConfusion centerCoc = ComputeCircleOfConfusion(centerDepth, extent);

    float farGatherRadius = abs(centerCoc.clampedSignedRadiusPixels);
    float nearGatherRadius = min(NearFieldSearchRadiusPixels, MaximumCocRadiusPixels);
    float depthTolerance = DofDepthCompareAbsoluteMetres + (DofDepthCompareRelative * centerDepth);

    float3 farAccumulated = float3(0.0f, 0.0f, 0.0f);
    float3 nearAccumulated = float3(0.0f, 0.0f, 0.0f);
    float farWeightSum = 0.0f;
    float nearWeightSum = 0.0f;
    uint farAcceptedCount = 0u;
    uint nearAcceptedCount = 0u;
    uint rejectedOutOfBounds = 0u;
    uint rejectedOutsideCoc = 0u;
    uint rejectedOwnedByNearField = 0u;
    float2 firstDisk = float2(0.0f, 0.0f);
    float2 lastDisk = float2(0.0f, 0.0f);

    for (uint sample = 0u; sample < ApertureSampleCount; ++sample)
    {
        float2 disk = ApertureDiskSample(sample, ApertureSampleCount);
        if (sample == 0u)
        {
            firstDisk = disk;
        }
        if ((sample + 1u) == ApertureSampleCount)
        {
            lastDisk = disk;
        }

        float2 farOffset = disk * farGatherRadius;
        uint2 farTexel;
        if (!OffsetTexel(extent, pixel, farOffset, farTexel))
        {
            ++rejectedOutOfBounds;
        }
        else
        {
            uint tapIndex = TexelIndex(extent, farTexel);
            float tapDepth = Scene[tapIndex].viewDepthMetres;
            CircleOfConfusion tapCoc = ComputeCircleOfConfusion(tapDepth, extent);
            float distance = length(farOffset);
            float scatterRadius = max(abs(tapCoc.clampedSignedRadiusPixels), MinimumScatterRadiusPixels);
            float weight = 0.0f;
            if (distance == 0.0f)
            {
                weight = 1.0f / (scatterRadius * scatterRadius);
            }
            else if (tapCoc.region == FocusRegionNear && tapDepth < centerDepth - depthTolerance)
            {
                ++rejectedOwnedByNearField;
            }
            else if (abs(tapCoc.clampedSignedRadiusPixels) < distance)
            {
                ++rejectedOutsideCoc;
            }
            else
            {
                weight = 1.0f / (scatterRadius * scatterRadius);
            }
            if (weight > 0.0f)
            {
                ++farAcceptedCount;
                farWeightSum += weight;
                farAccumulated += Chain[SourceOffset + tapIndex].rgb * weight;
            }
        }

        float2 nearOffset = disk * nearGatherRadius;
        uint2 nearTexel;
        if (OffsetTexel(extent, pixel, nearOffset, nearTexel))
        {
            uint tapIndex = TexelIndex(extent, nearTexel);
            float tapDepth = Scene[tapIndex].viewDepthMetres;
            CircleOfConfusion tapCoc = ComputeCircleOfConfusion(tapDepth, extent);
            float distance = length(nearOffset);
            float scatterRadius = max(abs(tapCoc.clampedSignedRadiusPixels), MinimumScatterRadiusPixels);
            bool isNearField = tapCoc.region == FocusRegionNear;
            bool isInFront = tapDepth < centerDepth - depthTolerance;
            if (isNearField && isInFront && abs(tapCoc.clampedSignedRadiusPixels) >= distance)
            {
                float weight = 1.0f / (scatterRadius * scatterRadius);
                ++nearAcceptedCount;
                nearWeightSum += weight;
                nearAccumulated += Chain[SourceOffset + tapIndex].rgb * weight;
            }
        }
    }

    float farCoverage = float(farAcceptedCount) / float(ApertureSampleCount);
    float3 farField = farWeightSum > 0.0f ? farAccumulated / farWeightSum : centerColor;
    uint flags = DofEvaluated;
    if (centerCoc.clampedByBudget)
    {
        flags |= DofClampedByBudget;
    }
    if (farCoverage < MinimumFarCoverage || farWeightSum <= 0.0f)
    {
        // Below this coverage the gather has no honest evidence, so it reports the centre colour instead of
        // inventing an average from a handful of taps.
        flags |= DofInsufficientFarCoverage;
        farField = centerColor;
    }
    float nearCoverage = float(nearAcceptedCount) / float(ApertureSampleCount);
    float3 nearField = nearWeightSum > 0.0f ? nearAccumulated / nearWeightSum : float3(0.0f, 0.0f, 0.0f);
    float3 output = (nearField * nearCoverage) + (farField * (1.0f - nearCoverage));

    Chain[DestinationOffset + centerIndex] = float4(output, 0.0f);

    PixelRecord record = Records[centerIndex];
    record.cocSignedRadiusPixels = centerCoc.signedRadiusPixels;
    record.cocClampedRadiusPixels = centerCoc.clampedSignedRadiusPixels;
    record.cocDiameterMillimetres = centerCoc.diameterMillimetres;
    record.cocApertureDiameterMillimetres = centerCoc.apertureDiameterMillimetres;
    record.cocPixelsPerMillimetre = centerCoc.pixelsPerMillimetre;
    record.cocRegion = centerCoc.region;
    record.farGatherRadiusPixels = farGatherRadius;
    record.nearGatherRadiusPixels = nearGatherRadius;
    record.nearCoverage = nearCoverage;
    record.farCoverage = farCoverage;
    record.farWeightSum = farWeightSum;
    record.nearWeightSum = nearWeightSum;
    record.dofFarR = farField.r;
    record.dofFarG = farField.g;
    record.dofFarB = farField.b;
    record.dofNearR = nearField.r;
    record.dofNearG = nearField.g;
    record.dofNearB = nearField.b;
    record.dofOutR = output.r;
    record.dofOutG = output.g;
    record.dofOutB = output.b;
    record.apertureFirstX = firstDisk.x;
    record.apertureFirstY = firstDisk.y;
    record.apertureLastX = lastDisk.x;
    record.apertureLastY = lastDisk.y;
    record.dofSampleCount = ApertureSampleCount;
    record.dofFarAcceptedCount = farAcceptedCount;
    record.dofNearAcceptedCount = nearAcceptedCount;
    record.dofRejectedOutOfBounds = rejectedOutOfBounds;
    record.dofRejectedOutsideCoc = rejectedOutsideCoc;
    record.dofRejectedOwnedByNearField = rejectedOwnedByNearField;
    record.dofFlags = flags;
    record.status |= StatusDepthOfField;
    Records[centerIndex] = record;
}

// A centred gather along this pixel's own shutter trajectory. Rejected taps are removed from the average and the
// remaining weights are renormalised, so a partially rejected trajectory darkens nothing.
void EvaluateMotionBlurPass(uint2 pixel)
{
    uint2 extent = uint2(DisplayWidth, DisplayHeight);
    uint centerIndex = TexelIndex(extent, pixel);
    SceneRecord centerScene = Scene[centerIndex];
    float3 centerColor = Chain[SourceOffset + centerIndex].rgb;
    float centerDepth = centerScene.viewDepthMetres;
    ShutterSchedule schedule = BuildShutterSchedule(float2(centerScene.motionX, centerScene.motionY), extent);
    float depthTolerance = MotionDepthCompareAbsoluteMetres + (MotionDepthCompareRelative * centerDepth);

    float3 accumulated = float3(0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;
    uint acceptedCount = 0u;
    uint rejectedOutOfBounds = 0u;
    uint rejectedForeground = 0u;
    uint rejectedBackground = 0u;
    uint flags = schedule.flags | MotionEvaluated;
    float firstParameter = 0.0f;
    float lastParameter = 0.0f;
    float parameterSum = 0.0f;

    for (uint sample = 0u; sample < MotionSampleCount; ++sample)
    {
        float parameter = ShutterParameter(sample, MotionSampleCount);
        parameterSum += parameter;
        if (sample == 0u)
        {
            firstParameter = parameter;
        }
        if ((sample + 1u) == MotionSampleCount)
        {
            lastParameter = parameter;
        }
        if (parameter == 0.0f)
        {
            flags |= MotionIncludesCenterSample;
        }

        float2 offset = schedule.shutterDisplacementPixels * parameter;
        uint2 texel;
        if (!OffsetTexel(extent, pixel, offset, texel))
        {
            ++rejectedOutOfBounds;
            continue;
        }
        uint tapIndex = TexelIndex(extent, texel);
        SceneRecord tapScene = Scene[tapIndex];
        bool accepted = false;
        if (parameter == 0.0f)
        {
            accepted = true;
        }
        else if (abs(tapScene.viewDepthMetres - centerDepth) <= depthTolerance)
        {
            accepted = true;
        }
        else if (tapScene.viewDepthMetres < centerDepth)
        {
            // The tap is in front of the centre, so it may only contribute if its own shutter trajectory was long
            // enough to have covered this pixel while the shutter was open.
            float2 tapDisplacement = MotionToDisplayPixels(float2(tapScene.motionX, tapScene.motionY), extent);
            float tapShutterLength = length(tapDisplacement) * ExposureTimeFraction;
            if ((Flags & FlagClampShutterToBudget) != 0u)
            {
                tapShutterLength = min(tapShutterLength, MaximumShutterDisplacementPixels);
            }
            if ((0.5f * tapShutterLength) >= length(offset))
            {
                accepted = true;
            }
            else
            {
                ++rejectedForeground;
            }
        }
        else
        {
            // The background it exposes was never in front of the moving object, so accepting it would invent a
            // trail across the disocclusion.
            ++rejectedBackground;
        }

        if (accepted)
        {
            ++acceptedCount;
            weightSum += 1.0f;
            accumulated += Chain[SourceOffset + tapIndex].rgb;
        }
    }

    float3 output;
    if (weightSum > 0.0f)
    {
        output = accumulated / weightSum;
    }
    else
    {
        output = centerColor;
        flags |= MotionFellBackToCenter;
    }
    Chain[DestinationOffset + centerIndex] = float4(output, 0.0f);

    PixelRecord record = Records[centerIndex];
    record.frameDisplacementX = schedule.frameDisplacementPixels.x;
    record.frameDisplacementY = schedule.frameDisplacementPixels.y;
    record.shutterDisplacementX = schedule.shutterDisplacementPixels.x;
    record.shutterDisplacementY = schedule.shutterDisplacementPixels.y;
    record.shutterAppliedScale = schedule.appliedScale;
    record.shutterGatherRadiusPixels = schedule.gatherRadiusPixels;
    record.shutterCentroidParameter = parameterSum / float(MotionSampleCount);
    record.shutterFirstParameter = firstParameter;
    record.shutterLastParameter = lastParameter;
    record.motionWeightSum = weightSum;
    record.motionBlurR = output.r;
    record.motionBlurG = output.g;
    record.motionBlurB = output.b;
    record.motionSampleCount = MotionSampleCount;
    record.motionAcceptedCount = acceptedCount;
    record.motionRejectedOutOfBounds = rejectedOutOfBounds;
    record.motionRejectedForeground = rejectedForeground;
    record.motionRejectedBackground = rejectedBackground;
    record.motionFlags = flags;
    record.status |= StatusMotionBlur;
    Records[centerIndex] = record;
}

[numthreads(8, 8, 1)] void CameraEffectCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    if (PassKind == PassDepthOfField)
    {
        EvaluateDepthOfFieldPass(dispatchThreadId.xy);
    }
    else
    {
        EvaluateMotionBlurPass(dispatchThreadId.xy);
    }
}

[numthreads(8, 8, 1)] void BloomExtractCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    uint2 pixel = dispatchThreadId.xy;
    uint index = TexelIndex(uint2(DisplayWidth, DisplayHeight), pixel);
    float3 preExposed = Chain[SourceOffset + index].rgb;
    BloomExtraction extraction =
        ExtractBloomHighlights(preExposed, PreExposure, BloomThresholdLuminance, BloomSoftKneeLuminance);
    Bloom[DestinationOffset + index] = float4(extraction.extractedPreExposed, 0.0f);

    PixelRecord record = Records[index];
    record.bloomPreExposedLuminance = extraction.preExposedLuminance;
    record.bloomAbsoluteLuminance = extraction.absoluteLuminance;
    record.bloomExcessLuminance = extraction.excessLuminance;
    record.bloomThresholdWeight = extraction.thresholdWeight;
    record.bloomExtractedR = extraction.extractedPreExposed.r;
    record.bloomExtractedG = extraction.extractedPreExposed.g;
    record.bloomExtractedB = extraction.extractedPreExposed.b;
    record.bloomFlags = extraction.flags;
    record.bloomLevelCount = BloomLevelCount;
    record.status |= StatusBloom;
    Records[index] = record;
}

// Each axis halves with a ceiling, so a five-texel axis becomes three and then two and no column is ever dropped.
// The area footprint keeps the weights summing to one, so a constant field survives the whole chain unchanged.
[numthreads(8, 8, 1)] void BloomDownsampleCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DestinationWidth || dispatchThreadId.y >= DestinationHeight)
    {
        return;
    }
    uint2 pixel = dispatchThreadId.xy;
    DownsampleFootprint1D horizontal = BloomDownsampleFootprint(pixel.x, SourceWidth, DestinationWidth);
    DownsampleFootprint1D vertical = BloomDownsampleFootprint(pixel.y, SourceHeight, DestinationHeight);

    float3 accumulated = float3(0.0f, 0.0f, 0.0f);
    for (uint row = 0u; row < vertical.tapCount; ++row)
    {
        for (uint column = 0u; column < horizontal.tapCount; ++column)
        {
            uint sourceIndex = (vertical.indices[row] * SourceWidth) + horizontal.indices[column];
            accumulated += Bloom[SourceOffset + sourceIndex].rgb * (horizontal.weights[column] * vertical.weights[row]);
        }
    }
    Bloom[DestinationOffset + (pixel.y * DestinationWidth) + pixel.x] = float4(accumulated, 0.0f);
}

// Progressive combine from the smallest level down to level zero. The accumulator holds the partial sum at the
// level currently being processed, so nothing is resampled twice and the result carries the bloom contribution
// only: the base image is never duplicated into it.
[numthreads(8, 8, 1)] void BloomUpsampleCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DestinationWidth || dispatchThreadId.y >= DestinationHeight)
    {
        return;
    }
    uint2 pixel = dispatchThreadId.xy;
    uint destinationIndex = (pixel.y * DestinationWidth) + pixel.x;
    float3 weighted = Bloom[BloomLevelOffset + destinationIndex].rgb * BloomLevelWeight;
    if ((Flags & FlagBloomTopLevel) != 0u)
    {
        Bloom[DestinationOffset + destinationIndex] = float4(weighted, 0.0f);
        return;
    }

    UpsampleFootprint1D horizontal = BloomUpsampleFootprint(pixel.x, SourceWidth, DestinationWidth);
    UpsampleFootprint1D vertical = BloomUpsampleFootprint(pixel.y, SourceHeight, DestinationHeight);
    float3 accumulated = float3(0.0f, 0.0f, 0.0f);
    accumulated += Bloom[SourceOffset + (vertical.lowIndex * SourceWidth) + horizontal.lowIndex].rgb *
                   (horizontal.lowWeight * vertical.lowWeight);
    accumulated += Bloom[SourceOffset + (vertical.lowIndex * SourceWidth) + horizontal.highIndex].rgb *
                   (horizontal.highWeight * vertical.lowWeight);
    accumulated += Bloom[SourceOffset + (vertical.highIndex * SourceWidth) + horizontal.lowIndex].rgb *
                   (horizontal.lowWeight * vertical.highWeight);
    accumulated += Bloom[SourceOffset + (vertical.highIndex * SourceWidth) + horizontal.highIndex].rgb *
                   (horizontal.highWeight * vertical.highWeight);
    Bloom[DestinationOffset + destinationIndex] = float4(accumulated + weighted, 0.0f);
}

[numthreads(8, 8, 1)] void ComposeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    uint2 pixel = dispatchThreadId.xy;
    uint index = TexelIndex(uint2(DisplayWidth, DisplayHeight), pixel);
    float3 base = Chain[SourceOffset + index].rgb;
    // The bloom accumulator holds the extracted excess only, so composing it adds the light the lens scattered out
    // of the highlights without counting the scene's radiance a second time.
    float3 bloom = (Flags & FlagBloomEnabled) != 0u ? Bloom[BloomLevelOffset + index].rgb * BloomGain
                                                    : float3(0.0f, 0.0f, 0.0f);
    FinishFrame(pixel, base, bloom);
}
