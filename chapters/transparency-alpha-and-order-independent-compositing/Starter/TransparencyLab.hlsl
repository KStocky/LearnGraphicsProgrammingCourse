// Chapter 32 Starter: the transparency pass almost every renderer writes first, written honestly.
//
// What this file owns is a complete, runnable, believable transparent surface pass:
//
//   * The opaque scene is shaded and its view depth is stored, because a transparent fragment that is behind an
//     opaque surface must not be composited at all.
//   * The CPU sorts the transparent draws back to front by a representative depth and uploads them in that order.
//     This file walks that order, once per pixel, and composites with premultiplied source-over.
//   * Every fragment is premultiplied exactly once, and source-over never multiplies by alpha a second time.
//   * Every fragment consumes the light list of the cluster its own view depth lands in, not the cluster the opaque
//     surface behind it lands in.
//
// What this file deliberately does not own, and does not secretly do anywhere:
//
//   * There is no per-pixel sort. The only order this pass has is the one the CPU handed it, which is a total order
//     over the *draws* and is not a per-pixel order. Where two surfaces interpenetrate it is wrong, and this file
//     has no way to know that, which is exactly the problem the chapter exists to make measurable.
//   * There is no weighted blended accumulation, no revealage target and no resolve. The accumulator fields the
//     record carries stay zero and the stage list does not contain the stages that would fill them.
//   * There is no coverage decision. Every fragment is a blend, the frame says so through StatusBlendOnly, and the
//     alpha-test and stochastic counters stay at zero.
//   * There is no refraction source copy and no refraction sample, no fog fold, no bounded store with a keep-and-drop
//     rule, and no reactive mask. The configuration validation refuses to switch any of them on.
//
// The Solution adds those stages one at a time. Nothing in the structure of this file has to change for it to do so,
// which is the shape the chapter's patches follow.

#include "../Common/TransparencyShared.hlsli"

// ---------------------------------------------------------------------------------------------------------------
// The lesson: the one multiply and the one blend equation.
// ---------------------------------------------------------------------------------------------------------------

// The straight colour multiplied by alpha, exactly once. Every equation below consumes the result and none of them
// multiplies by alpha a second time; doing so is the alpha-squared bug that darkens every partially covered
// fragment by its own alpha, most visibly at every silhouette in the frame.
float3 Premultiply(float3 straightColor, float alpha)
{
    return straightColor * alpha;
}

// Source over destination, in premultiplied form. The destination weight is exactly 1 - source.alpha, so a source
// with alpha zero leaves the destination bit for bit unchanged and a source with alpha one replaces it bit for bit.
void SourceOver(float3 sourceColor, float sourceAlpha, inout float3 destinationColor, inout float destinationAlpha)
{
    float destinationWeight = 1.0f - sourceAlpha;
    destinationColor = sourceColor + (destinationWeight * destinationColor);
    destinationAlpha = sourceAlpha + (destinationWeight * destinationAlpha);
}

float3 CompositeOverOpaqueBackground(float3 layerColor, float layerAlpha, float3 background)
{
    return layerColor + ((1.0f - layerAlpha) * background);
}

// ---------------------------------------------------------------------------------------------------------------
// Stage: Clear.
// ---------------------------------------------------------------------------------------------------------------

[numthreads(64, 1, 1)] void ClearCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint pixelCount = DisplayWidth * DisplayHeight;
    uint index = dispatchThreadId.x;
    if (index == 0u)
    {
        FrameRecord frame = (FrameRecord)0;
        frame.abiMarker = AbiMarker;
        frame.variant = VariantId;
        frame.stageOrderWord = StageOrderWord;
        frame.stageCount = StageCount;
        frame.activePaneCount = ActivePaneCount;
        frame.clusterTileCountX = ClusterTileCountX;
        frame.clusterTileCountY = ClusterTileCountY;
        frame.clusterSliceCount = ClusterSliceCount;
        frame.clusterCount = ClusterCount;
        frame.sceneLightCount = SceneLightCount;
        frame.lightIndexCount = LightIndexCount;
        frame.compositeSource = CompositeModeId;
        Frame[0] = frame;
    }
    if (index >= pixelCount)
    {
        return;
    }
    PixelRecord record = (PixelRecord)0;
    record.stageOrderWord = StageOrderWord;
    record.abiMarker = AbiMarker;
    record.sortedRevealage = 1.0f;
    record.objectRevealage = 1.0f;
    record.oitRevealage = 1.0f;
    // No exposure has been applied yet, so there is no ratio to publish. Clearing this to one would have the cleared
    // record claim a measured identity exposure.
    record.netExposureScale = 0.0f;
    record.netExposureScaleDerived = 0u;
    record.netExposureScaleChannel = 0u;
    record.fogTransmittance = 1.0f;
    Records[index] = record;
    // The refraction source is cleared and never written again: this variant copies no opaque scene and samples
    // nothing from one, and a zero buffer beside a zero status bit is what makes that checkable.
    RefractionSource[index] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (uint slot = 0u; slot < LabFragmentCapacity; ++slot)
    {
        Fragments[(index * LabFragmentCapacity) + slot] = (FragmentRecord)0;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Stage: Opaque. The G-buffer and its lighting, and the depth every transparent fragment will be tested against.
// ---------------------------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)] void OpaqueCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    uint2 pixel = dispatchThreadId.xy;
    uint index = TexelIndex(pixel);
    uint2 normalized = NormalizedCoordinate(pixel);
    PixelRecord record = Records[index];

    bool hasSurface = HasOpaqueSurfaceAt(normalized);
    float depth = OpaqueViewDepthAt(normalized);
    record.hasOpaqueSurface = hasSurface ? 1u : 0u;
    record.opaqueViewDepthMetres = depth;
    record.tileX = pixel.x / 32u;
    record.tileY = pixel.y / 32u;
    record.opaqueSliceIndex = hasSurface ? SliceIndexForDepth(depth) : 0u;
    record.opaqueClusterIndex = hasSurface ? ClusterIndexFor(record.tileX, record.tileY, record.opaqueSliceIndex) : 0u;

    float3 radiance;
    if (hasSurface)
    {
        uint opaqueLightOffset;
        uint opaqueLightCount;
        float3 lightScale = ClusterLightScale(record.opaqueClusterIndex, opaqueLightOffset, opaqueLightCount);
        uint3 albedoFixed = OpaqueAlbedoFixed(normalized);
        radiance = (float3(albedoFixed) / float(FixedOne)) * lightScale;
    }
    else
    {
        radiance = float3(float(SkyFixedR), float(SkyFixedG), float(SkyFixedB)) / float(FixedOne);
    }

    record.opaqueR = radiance.r;
    record.opaqueG = radiance.g;
    record.opaqueB = radiance.b;
    record.status |= StatusOpaque;
    Records[index] = record;
}

// ---------------------------------------------------------------------------------------------------------------
// Stage: Fragments. One pass over the panes in the order the CPU sorted them into, keeping the ones the opaque
// depth test admits. There is no sort here and no coverage decision: the pane buffer arrives in the declared
// per-draw back-to-front order and this loop walks it in index order, which is precisely what "the CPU sorted the
// draws" means.
// ---------------------------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)] void FragmentsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    uint2 pixel = dispatchThreadId.xy;
    uint index = TexelIndex(pixel);
    uint2 normalized = NormalizedCoordinate(pixel);
    PixelRecord record = Records[index];

    bool hasOpaque = record.hasOpaqueSurface != 0u;
    float opaqueDepth = record.opaqueViewDepthMetres;
    bool testDepth = (Flags & FlagTestAgainstOpaqueDepth) != 0u;
    uint fragmentBase = index * LabFragmentCapacity;

    uint candidateCount = 0u;
    uint storedCount = 0u;
    uint depthRejected = 0u;
    uint clusterIndex = 0u;
    uint clusterSlice = 0u;
    uint clusterLightOffset = 0u;
    uint clusterLightCount = 0u;
    uint hasClusterBinding = 0u;
    float nearestDepth = 0.0f;

    for (uint slot = 0u; slot < ActivePaneCount; ++slot)
    {
        PaneRecord pane = Panes[slot];
        if (!PaneCovers(pane, normalized))
        {
            continue;
        }
        float depth = PaneViewDepth(pane, normalized);

        // Equal depths are rejected, matching a LESS depth test: a transparent fragment coplanar with the opaque
        // surface it is drawn against is z-fighting, and accepting it would make the image depend on precision.
        if (testDepth && hasOpaque && !(depth < opaqueDepth))
        {
            ++depthRejected;
            continue;
        }
        ++candidateCount;
        if (storedCount >= LabFragmentCapacity)
        {
            continue;
        }

        // The pane's authored alpha, used as a blend alpha and nothing else. This variant takes no coverage
        // decision, so an alpha-tested or stochastic pane is blended here exactly like every other pane, and the
        // frame declares that through StatusBlendOnly rather than leaving a reader to assume it.
        uint alphaFixed = PaneAlphaFixed(pane, normalized);
        float alpha = float(alphaFixed) / float(FixedOne);

        // The clustered light-list seam. The slice is derived from this fragment's own depth; deriving it from the
        // opaque surface's depth would light a window with the light list of whatever is behind it.
        uint sliceIndex = SliceIndexForDepth(depth);
        uint fragmentCluster = ClusterIndexFor(record.tileX, record.tileY, sliceIndex);
        uint lightOffset;
        uint lightCount;
        float3 lightScale = ClusterLightScale(fragmentCluster, lightOffset, lightCount);
        float3 straight =
            (float3(float(pane.colorFixedR), float(pane.colorFixedG), float(pane.colorFixedB)) / float(FixedOne)) *
            lightScale;
        float3 premultiplied = Premultiply(straight, alpha);

        FragmentRecord fragment = (FragmentRecord)0;
        fragment.straightR = straight.r;
        fragment.straightG = straight.g;
        fragment.straightB = straight.b;
        fragment.premultipliedR = premultiplied.r;
        fragment.premultipliedG = premultiplied.g;
        fragment.premultipliedB = premultiplied.b;
        fragment.alpha = alpha;
        fragment.requestedAlpha = alpha;
        fragment.viewDepthMetres = depth;
        fragment.oitWeight = 0.0f;
        fragment.fogTransmittance = 1.0f;
        fragment.refractionOffsetLengthUv = 0.0f;
        fragment.revealageBefore = 1.0f;
        fragment.revealageAfter = 1.0f;
        fragment.transmittanceInFront = 1.0f;
        fragment.lightScale = lightScale.r;
        fragment.paneIndex = pane.paneIndex;
        fragment.drawOrder = pane.drawOrder;
        fragment.primitiveId = pane.primitiveId;
        // Every fragment this variant produces is a blend, whatever the pane authored, and it says so.
        fragment.coverageMode = CoverageAlphaBlend;
        fragment.outcome = OutcomeComposited;
        // The only order this pass has is the CPU's, so the two positions are the same number. That is not a
        // per-pixel sort quietly agreeing with a per-draw one; it is the absence of a per-pixel sort.
        fragment.sortedPosition = storedCount;
        fragment.objectPosition = storedCount;
        fragment.flags = FragmentStored;
        fragment.clusterIndex = fragmentCluster;
        fragment.sliceIndex = sliceIndex;
        fragment.lightOffset = lightOffset;
        fragment.lightCount = lightCount;
        fragment.sampleCount = 0u;
        Fragments[fragmentBase + storedCount] = fragment;
        ++storedCount;

        if (hasClusterBinding == 0u || depth < nearestDepth)
        {
            nearestDepth = depth;
            clusterIndex = fragmentCluster;
            clusterSlice = sliceIndex;
            clusterLightOffset = lightOffset;
            clusterLightCount = lightCount;
            hasClusterBinding = 1u;
        }
    }

    record.candidateFragmentCount = candidateCount;
    record.storedFragmentCount = storedCount;
    record.droppedFragmentCount = candidateCount - storedCount;
    record.depthRejectedCount = depthRejected;
    record.coverageRejectedCount = 0u;
    record.alphaTestPassCount = 0u;
    record.alphaTestFailCount = 0u;
    record.stochasticSampleCount = 0u;
    record.stochasticAcceptedCount = 0u;
    record.stochasticMaskLow = 0u;
    record.stochasticMaskHigh = 0u;
    record.stochasticPixelAccepted = 0u;
    record.hasStochasticFragment = 0u;
    record.overflowed = record.droppedFragmentCount > 0u ? 1u : 0u;
    record.refractionUsed = 0u;
    record.refractionFallback = RefractionFallbackNone;
    record.refractionClamped = 0u;
    record.fragmentSliceIndex = clusterSlice;
    record.clusterIndex = clusterIndex;
    record.lightOffset = clusterLightOffset;
    record.lightCount = clusterLightCount;
    record.hasClusterBinding = hasClusterBinding;
    record.clusterAmbiguous =
        (hasClusterBinding != 0u && record.hasOpaqueSurface != 0u && clusterSlice == record.opaqueSliceIndex) ? 1u : 0u;
    record.status |= StatusFragments;
    if (depthRejected > 0u)
    {
        record.status |= StatusDepthRejected;
    }
    if (record.overflowed != 0u)
    {
        record.status |= StatusCapacityOverflow;
    }
    Records[index] = record;
}

// ---------------------------------------------------------------------------------------------------------------
// Stage: ForwardComposite. One source-over walk, in the order the CPU handed this pass. The per-pixel sorted
// reference the Solution publishes has no counterpart here, so those fields stay at the values the clear wrote and
// StatusPerPixelSorted is never set.
// ---------------------------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)] void ForwardCompositeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    uint index = TexelIndex(dispatchThreadId.xy);
    PixelRecord record = Records[index];
    uint base = index * LabFragmentCapacity;
    uint count = min(record.storedFragmentCount, LabFragmentCapacity);
    float3 background = float3(record.opaqueR, record.opaqueG, record.opaqueB);

    float3 objectColor = float3(0.0f, 0.0f, 0.0f);
    float objectAlpha = 0.0f;
    float revealage = 1.0f;
    uint composited = 0u;
    uint fullyTransparent = 0u;
    uint depthTies = 0u;

    uint position = 0u;
    for (position = 0u; position < count; ++position)
    {
        FragmentRecord fragment = Fragments[base + position];
        fragment.revealageBefore = revealage;
        if (position > 0u && Fragments[base + position - 1u].viewDepthMetres == fragment.viewDepthMetres)
        {
            fragment.flags |= FragmentTiedWithPrevious;
            ++depthTies;
        }
        if (fragment.alpha == 0.0f)
        {
            // Alpha zero with a black premultiplied colour is the source-over identity: recorded as seen and
            // excluded from the composited count rather than counted as a contribution that did nothing.
            fragment.outcome = OutcomeRejectedAsFullyTransparent;
            fragment.revealageAfter = revealage;
            ++fullyTransparent;
            Fragments[base + position] = fragment;
            continue;
        }
        float3 source = float3(fragment.premultipliedR, fragment.premultipliedG, fragment.premultipliedB);
        SourceOver(source, fragment.alpha, objectColor, objectAlpha);
        revealage *= 1.0f - fragment.alpha;
        fragment.revealageAfter = revealage;
        fragment.outcome = OutcomeComposited;
        ++composited;
        Fragments[base + position] = fragment;
    }

    // Survival is a fact about the fragments in front, so it is only known after the walk finishes.
    float transmittance = 1.0f;
    for (position = count; position > 0u; --position)
    {
        uint slotIndex = base + position - 1u;
        FragmentRecord fragment = Fragments[slotIndex];
        fragment.transmittanceInFront = transmittance;
        if (fragment.outcome == OutcomeComposited)
        {
            transmittance *= 1.0f - fragment.alpha;
        }
        Fragments[slotIndex] = fragment;
    }

    record.objectLayerR = objectColor.r;
    record.objectLayerG = objectColor.g;
    record.objectLayerB = objectColor.b;
    record.objectLayerAlpha = objectAlpha;
    record.objectRevealage = revealage;
    float3 objectOver = CompositeOverOpaqueBackground(objectColor, objectAlpha, background);
    record.objectOverR = objectOver.r;
    record.objectOverG = objectOver.g;
    record.objectOverB = objectOver.b;

    record.compositedCount = composited;
    record.fullyTransparentCount = fullyTransparent;
    record.depthTieCount = depthTies;
    record.depthWriteCount = 0u;
    record.depthWritten = 0u;
    record.writtenViewDepthMetres = 0.0f;
    record.status |= StatusForwardComposite | StatusObjectOrdered;
    Records[index] = record;
}

// ---------------------------------------------------------------------------------------------------------------
// Stage: Compose. The composite the frame publishes, then one display exposure, the tone curve and the encoding.
//
// The exposure is applied once, after the composite. Applying it before would expose the transparent layer
// differently from the scene behind it; applying it twice would be invisible in the image and obvious in the count.
// ---------------------------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)] void ComposeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    uint index = TexelIndex(dispatchThreadId.xy);
    PixelRecord record = Records[index];

    // The only composite this variant has. The approximation and order-error fields the Solution publishes stay at
    // zero, because there is no second composite here to measure anything against; publishing a plausible number
    // for them would be the one thing an honest baseline must not do.
    float3 composite = float3(record.objectOverR, record.objectOverG, record.objectOverB);
    record.compositeR = composite.r;
    record.compositeG = composite.g;
    record.compositeB = composite.b;
    record.compositeSource = CompositeObjectSorted;
    record.fogTransmittance = 1.0f;
    record.foggedR = composite.r;
    record.foggedG = composite.g;
    record.foggedB = composite.b;
    record.status |= StatusFogPerFragment;

    // The pre-exposure radiance of this variant is the composite itself: no fog is folded in here, and the record
    // says so by publishing a transmittance of one and a fogged colour equal to the composite. Exposing that once
    // and dividing the exposure back out of the result is what makes the single multiply checkable.
    float3 exposed = composite * DisplayExposureScale;
    record.exposedR = exposed.r;
    record.exposedG = exposed.g;
    record.exposedB = exposed.b;
    PublishNetExposureScale(composite, exposed, record);
    record.exposureApplicationCount += 1u;
    record.status |= StatusExposure;

    float3 displayLinear = ToneMap(exposed);
    record.status |= StatusToneMap;
    float3 encoded = EncodeDisplay(displayLinear);
    record.finalR = encoded.r;
    record.finalG = encoded.g;
    record.finalB = encoded.b;
    record.status |= StatusDisplayEncode | StatusBlendOnly;
    Records[index] = record;

    // Frame-level evidence. Every counter is an integer, so the totals are a property of the dispatch rather than of
    // the order its groups happened to finish in.
    InterlockedAdd(Frame[0].candidateFragmentCount, record.candidateFragmentCount);
    InterlockedAdd(Frame[0].storedFragmentCount, record.storedFragmentCount);
    InterlockedAdd(Frame[0].droppedFragmentCount, record.droppedFragmentCount);
    InterlockedAdd(Frame[0].compositedFragmentCount, record.compositedCount);
    InterlockedAdd(Frame[0].depthRejectedFragmentCount, record.depthRejectedCount);
    InterlockedAdd(Frame[0].fullyTransparentFragmentCount, record.fullyTransparentCount);
    InterlockedAdd(Frame[0].depthTieCount, record.depthTieCount);
    if (record.overflowed != 0u)
    {
        InterlockedAdd(Frame[0].overflowPixelCount, 1u);
        InterlockedOr(Frame[0].overflowFlags, OverflowFragmentCapacity);
    }
    if (record.hasClusterBinding != 0u)
    {
        InterlockedAdd(Frame[0].clusterPixelCount, 1u);
        if (record.clusterAmbiguous != 0u)
        {
            InterlockedAdd(Frame[0].ambiguousClusterPixelCount, 1u);
        }
    }
    InterlockedMax(Frame[0].maximumStoredFragmentCount, record.storedFragmentCount);
}
