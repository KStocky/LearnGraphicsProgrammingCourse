// Chapter 32 Solution: transparency written out as a sequence of stages, each of which publishes the evidence that
// distinguishes it from the stage beside it.
//
// The stage graph, in submission order, is:
//
//   Clear             every buffer and counter the frame will accumulate into.
//   Opaque            the opaque G-buffer and its lighting, which is where the depth every transparent fragment is
//                     tested against comes from, and which folds fog at its own depth when the policy asks for it.
//   RefractionSource  a copy of the lit opaque scene. Screen-space refraction reads this copy and nothing else,
//                     which is the single sentence that contains every limitation the technique has.
//   Fragments         deterministic per-pixel fragment generation: the coverage decisions, the cluster the fragment
//                     itself lands in, the bounded screen-space refraction, the per-fragment fog fold, and a bounded
//                     store with a declared keep-and-drop rule and a counted overflow.
//   ForwardComposite  source-over twice over the same fragments: once in the declared per-pixel back-to-front total
//                     order, which is the reference, and once in the order the CPU sorted the draws into, which is
//                     what a real renderer can afford. The difference between them is published per pixel.
//   OitAccumulate     weighted blended order-independent accumulation, visited in whichever declared order the
//                     configuration asks for, so that order independence is measured rather than asserted.
//   OitResolve        the two accumulators resolved against the opaque background, with the epsilon guard, the
//                     revealage, and the weighted average that carries no ordering information at all.
//   Compose           the composite the frame publishes, screen-space fog when the policy asked for that placement,
//                     the temporal reactive mask, the single display exposure, the tone curve and the encoding.
//
// Nothing here claims a winner. The sorted reference is exact for the order it is given and that order is a fiction
// wherever two surfaces interpenetrate; the per-draw order is cheaper and publishes how wrong it is; weighted
// blended OIT needs no order and publishes how wrong it is everywhere; and the bounded store is exact until it
// overflows and then says what it dropped.

#include "../Common/TransparencyShared.hlsli"

// The compact per-pane decision the fragment stage takes once and then ranks. Keeping it small matters: a coverage
// decision costs a hash per sample, and re-taking it inside a ranking loop would multiply that cost by the number of
// panes twice over.
static const uint PaneStateAbsent = 0u;
static const uint PaneStateDepthRejected = 1u;
static const uint PaneStateCoverageRejected = 2u;
static const uint PaneStateCandidate = 3u;

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
// The lesson: coverage decisions. Neither of these blends anything. Both answer "is this sample present at all",
// which is why both may write depth and neither needs an order.
// ---------------------------------------------------------------------------------------------------------------

// A hard threshold on fixed-point alpha. It is stable in every frame and it destroys every gradation the alpha
// carried; no threshold recovers the partial coverage it threw away.
bool AlphaTestPasses(uint alphaFixed)
{
    return alphaFixed >= AlphaTestThresholdFixed;
}

// A set of present-or-absent opaque samples whose expected fraction is the requested alpha. It composites nothing,
// it needs no ordering, and it produces noise instead of the smooth gradient a blend would.
void EvaluateStochasticCoverage(uint alphaFixed, uint2 pixel, out uint maskLow, out uint maskHigh,
                                out uint acceptedCount)
{
    maskLow = 0u;
    maskHigh = 0u;
    acceptedCount = 0u;
    for (uint sampleIndex = 0u; sampleIndex < StochasticSampleCount; ++sampleIndex)
    {
        if (!StochasticSampleAccepted(alphaFixed, pixel, sampleIndex, FrameIndex))
        {
            continue;
        }
        ++acceptedCount;
        if (sampleIndex < 32u)
        {
            maskLow |= 1u << sampleIndex;
        }
        else
        {
            maskHigh |= 1u << (sampleIndex - 32u);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The lesson: the weighted blended weight function. It is a confidence in a fragment, not a second opacity, which
// is why it is a function of depth alone and why alpha enters the accumulation separately and exactly once. An
// unbounded weight would make one fragment win the average outright and turn the technique into an expensive,
// order-independent way of drawing the nearest fragment, which is why the clamp is not optional.
// ---------------------------------------------------------------------------------------------------------------

float OitFragmentWeight(float viewDepthMetres)
{
    if (OitWeightFunctionId == WeightUniform)
    {
        return UniformOitWeight;
    }
    float nearRatio = viewDepthMetres / OitNearScaleMetres;
    float farRatio = viewDepthMetres / OitFarScaleMetres;
    float farSquared = farRatio * farRatio;
    float denominator = OitDepthWeightBias + (nearRatio * nearRatio) + (farSquared * farSquared * farSquared);
    return clamp(OitDepthWeightNumerator / denominator, OitMinimumWeight, OitMaximumWeight);
}

// ---------------------------------------------------------------------------------------------------------------
// The lesson: fog placement. Folding fog into a fragment at the fragment's own depth is the only placement that can
// be correct, because a pane at five metres and the wall behind it need different amounts of fog and only a
// per-fragment application knows two depths. The in-scattering is multiplied by the fragment's alpha so that the
// result is still premultiplied: fog added to the covered part of the pixel only. Adding unpremultiplied
// in-scattering here is what makes transparent surfaces glow against a foggy background.
// ---------------------------------------------------------------------------------------------------------------

float FogTransmittance(float viewDepthMetres)
{
    return exp(-FogDensityPerMetre * viewDepthMetres);
}

float3 ApplyFogToRadiance(float3 radiance, float viewDepthMetres)
{
    float transmittance = FogTransmittance(viewDepthMetres);
    return (radiance * transmittance) + (float3(FogInscatterR, FogInscatterG, FogInscatterB) * (1.0f - transmittance));
}

float3 ApplyFogToFragment(float3 premultiplied, float alpha, float viewDepthMetres, out float transmittance)
{
    transmittance = FogTransmittance(viewDepthMetres);
    return (premultiplied * transmittance) +
           (float3(FogInscatterR, FogInscatterG, FogInscatterB) * ((1.0f - transmittance) * alpha));
}

// The temporal reactive mask reports risk, not policy: this pixel's history is likely to describe something else.
// What a resolve should do about it belongs to Chapter 28.
float ProbabilisticOr(float left, float right)
{
    return (left + right) - (left * right);
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
    RefractionSource[index] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (uint slot = 0u; slot < LabFragmentCapacity; ++slot)
    {
        Fragments[(index * LabFragmentCapacity) + slot] = (FragmentRecord)0;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Stage: Opaque. The G-buffer and its lighting, and the depth every transparent fragment will be tested against.
// The opaque surface consumes the cluster of its own depth, which is the correct thing for it to do and is exactly
// the cluster a transparent fragment must not reuse.
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

    // Per-fragment fog is folded into every shaded fragment, and an opaque surface is a shaded fragment. A sky pixel
    // has no depth of its own, so the frame declares the far wall's depth for it rather than inventing one.
    if (FogApplicationId == FogPerFragmentBeforeComposite && FogDensityPerMetre > 0.0f)
    {
        radiance = ApplyFogToRadiance(radiance, hasSurface ? depth : OpaqueBaseDepthMetres);
    }

    record.opaqueR = radiance.r;
    record.opaqueG = radiance.g;
    record.opaqueB = radiance.b;
    record.status |= StatusOpaque;
    Records[index] = record;
}

// ---------------------------------------------------------------------------------------------------------------
// Stage: RefractionSource. One copy of the lit opaque scene, taken before any transparency was composited. Whatever
// is not in this copy cannot be refracted, which is why a second pane of glass behind the first is invisible
// through it, and why the copy has to be its own stage rather than a re-evaluation of the opaque pass.
// ---------------------------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)] void RefractionSourceCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    uint index = TexelIndex(dispatchThreadId.xy);
    PixelRecord record = Records[index];
    float3 source = float3(record.opaqueR, record.opaqueG, record.opaqueB);
    RefractionSource[index] = float4(source, 1.0f);
    record.refractionSourceR = source.r;
    record.refractionSourceG = source.g;
    record.refractionSourceB = source.b;
    record.status |= StatusRefractionSource;
    Records[index] = record;
}

// ---------------------------------------------------------------------------------------------------------------
// Stage: Fragments.
// ---------------------------------------------------------------------------------------------------------------

// The declared total order, back to front: descending view depth, then ascending draw order, then ascending buffer
// index. The last key is what makes the order total, so the composite never depends on how the fragments happened
// to be gathered.
bool PrecedesInDeclaredOrder(float leftDepth, uint leftDrawOrder, uint leftSlot, float rightDepth,
                             uint rightDrawOrder, uint rightSlot)
{
    if (leftDepth != rightDepth)
    {
        return leftDepth > rightDepth;
    }
    if (leftDrawOrder != rightDrawOrder)
    {
        return leftDrawOrder < rightDrawOrder;
    }
    return leftSlot < rightSlot;
}

bool EarlierSubmitted(uint leftDrawOrder, uint leftSlot, uint rightDrawOrder, uint rightSlot)
{
    if (leftDrawOrder != rightDrawOrder)
    {
        return leftDrawOrder < rightDrawOrder;
    }
    return leftSlot < rightSlot;
}

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

    float paneDepth[LabPaneCount];
    uint paneState[LabPaneCount];
    uint paneRequestedAlpha[LabPaneCount];
    uint paneResolvedAlpha[LabPaneCount];
    uint paneMaskLow[LabPaneCount];
    uint paneMaskHigh[LabPaneCount];
    uint paneAccepted[LabPaneCount];

    uint candidateCount = 0u;
    uint depthRejected = 0u;
    uint coverageRejected = 0u;
    uint alphaTestPass = 0u;
    uint alphaTestFail = 0u;
    uint stochasticAccepted = 0u;
    uint stochasticMaskLow = 0u;
    uint stochasticMaskHigh = 0u;
    uint hasStochastic = 0u;
    uint stochasticPixelAccepted = 0u;
    uint coverageDecided = 0u;

    uint slot = 0u;
    for (slot = 0u; slot < LabPaneCount; ++slot)
    {
        paneDepth[slot] = 0.0f;
        paneState[slot] = PaneStateAbsent;
        paneRequestedAlpha[slot] = 0u;
        paneResolvedAlpha[slot] = 0u;
        paneMaskLow[slot] = 0u;
        paneMaskHigh[slot] = 0u;
        paneAccepted[slot] = 0u;
    }

    for (slot = 0u; slot < ActivePaneCount; ++slot)
    {
        PaneRecord pane = Panes[slot];
        if (!PaneCovers(pane, normalized))
        {
            continue;
        }
        float depth = PaneViewDepth(pane, normalized);
        paneDepth[slot] = depth;
        uint requestedAlpha = PaneAlphaFixed(pane, normalized);
        paneRequestedAlpha[slot] = requestedAlpha;

        // The depth test comes first because a fragment behind the opaque surface never reaches a blend unit or a
        // coverage decision at all. Equal depths are rejected, matching a LESS depth test: a transparent fragment
        // coplanar with the opaque surface it is drawn against is z-fighting.
        if (testDepth && hasOpaque && !(depth < opaqueDepth))
        {
            paneState[slot] = PaneStateDepthRejected;
            ++depthRejected;
            continue;
        }

        uint resolvedAlpha = requestedAlpha;
        if (pane.coverageMode == CoverageAlphaTest)
        {
            coverageDecided = 1u;
            bool passed = AlphaTestPasses(requestedAlpha);
            resolvedAlpha = passed ? FixedOne : 0u;
            if (passed)
            {
                ++alphaTestPass;
            }
            else
            {
                ++alphaTestFail;
            }
        }
        else if (pane.coverageMode == CoverageStochastic)
        {
            coverageDecided = 1u;
            hasStochastic = 1u;
            uint maskLow;
            uint maskHigh;
            uint accepted;
            EvaluateStochasticCoverage(requestedAlpha, pixel, maskLow, maskHigh, accepted);
            paneMaskLow[slot] = maskLow;
            paneMaskHigh[slot] = maskHigh;
            paneAccepted[slot] = accepted;
            stochasticMaskLow = maskLow;
            stochasticMaskHigh = maskHigh;
            stochasticAccepted = accepted;
            // The lab rasterizes one sample per pixel, so sample zero is the decision that decides presence. The
            // whole mask is published beside it because the fraction of samples an N-sample resolve would have kept
            // is the quantity the technique is actually about.
            bool present = (maskLow & 1u) != 0u;
            stochasticPixelAccepted = present ? 1u : 0u;
            resolvedAlpha = present ? FixedOne : 0u;
        }

        if (pane.coverageMode != CoverageAlphaBlend && resolvedAlpha == 0u)
        {
            paneState[slot] = PaneStateCoverageRejected;
            ++coverageRejected;
            continue;
        }

        paneResolvedAlpha[slot] = resolvedAlpha;
        paneState[slot] = PaneStateCandidate;
        ++candidateCount;
    }

    uint capacity = min(FragmentCapacity, LabFragmentCapacity);
    uint storedCount = 0u;
    uint droppedCount = 0u;

    // The bounded store's keep-and-drop rule, evaluated without ever materialising a list. A candidate is kept when
    // fewer than the capacity of other candidates rank ahead of it under the declared policy, so the decision is a
    // function of the fragment set rather than of the order the panes happened to be visited in. Neither policy is
    // safe: KeepNearest loses the far fragments a thick stack of glass needs, and KeepFirstSubmitted is not a
    // function of the geometry at all. Both count what they dropped, which is the only property that makes an
    // overflowing store debuggable.
    uint keptMask = 0u;
    for (slot = 0u; slot < ActivePaneCount; ++slot)
    {
        if (paneState[slot] != PaneStateCandidate)
        {
            continue;
        }
        uint ahead = 0u;
        for (uint other = 0u; other < ActivePaneCount; ++other)
        {
            if (other == slot || paneState[other] != PaneStateCandidate)
            {
                continue;
            }
            bool ranksAhead;
            if (OverflowPolicyId == OverflowKeepFirstSubmitted)
            {
                ranksAhead = EarlierSubmitted(Panes[other].drawOrder, other, Panes[slot].drawOrder, slot);
            }
            else
            {
                // KeepNearest keeps the tail of the back-to-front order, so the candidates that rank ahead of this
                // one are the ones nearer than it.
                ranksAhead = PrecedesInDeclaredOrder(paneDepth[slot], Panes[slot].drawOrder, slot, paneDepth[other],
                                                     Panes[other].drawOrder, other);
            }
            if (ranksAhead)
            {
                ++ahead;
            }
        }
        if (ahead < capacity)
        {
            keptMask |= 1u << slot;
            ++storedCount;
        }
        else
        {
            ++droppedCount;
        }
    }

    uint2 sourceExtent = uint2(DisplayWidth, DisplayHeight);
    float2 surfaceUv = (float2(pixel) + 0.5f) / float2(sourceExtent);
    uint fragmentBase = index * LabFragmentCapacity;
    uint refractionUsed = 0u;
    uint refractionFallback = RefractionFallbackNone;
    uint refractionClamped = 0u;
    uint clusterIndex = 0u;
    uint clusterSlice = 0u;
    uint clusterLightOffset = 0u;
    uint clusterLightCount = 0u;
    uint hasClusterBinding = 0u;
    float nearestDepth = 0.0f;

    for (slot = 0u; slot < ActivePaneCount; ++slot)
    {
        if ((keptMask & (1u << slot)) == 0u)
        {
            continue;
        }
        PaneRecord pane = Panes[slot];
        float depth = paneDepth[slot];
        uint resolvedAlphaFixed = paneResolvedAlpha[slot];
        float alpha = float(resolvedAlphaFixed) / float(FixedOne);

        // The declared back-to-front position among the kept fragments, and the position the CPU's per-draw order
        // would have put this fragment in. Publishing both is what lets the two composites be compared without
        // either of them re-deriving the other's order.
        uint sortedPosition = 0u;
        uint objectPosition = 0u;
        for (uint other = 0u; other < ActivePaneCount; ++other)
        {
            if (other == slot || (keptMask & (1u << other)) == 0u)
            {
                continue;
            }
            if (PrecedesInDeclaredOrder(paneDepth[other], Panes[other].drawOrder, other, depth, pane.drawOrder, slot))
            {
                ++sortedPosition;
            }
            if (other < slot)
            {
                ++objectPosition;
            }
        }

        // The clustered light-list seam. The slice is derived from this fragment's own depth; deriving it from the
        // opaque surface's depth would light a window with the light list of whatever is behind it.
        uint sliceIndex = SliceIndexForDepth(depth);
        uint fragmentCluster = ClusterIndexFor(record.tileX, record.tileY, sliceIndex);
        uint lightOffset;
        uint lightCount;
        float3 lightScale = ClusterLightScale(fragmentCluster, lightOffset, lightCount);
        float3 tint =
            (float3(float(pane.colorFixedR), float(pane.colorFixedG), float(pane.colorFixedB)) / float(FixedOne)) *
            lightScale;

        float2 requestedOffset = float2(0.0f, 0.0f);
        float2 appliedOffset = float2(0.0f, 0.0f);
        float2 sampledUv = surfaceUv;
        uint2 candidateTexel = pixel;
        uint fallback = RefractionFallbackNone;
        float requestedLength = 0.0f;
        float3 straight = tint;
        bool refracts = (pane.flags & PaneRefracts) != 0u && (Flags & FlagEnableRefraction) != 0u;
        if (refracts)
        {
            // The sign of the offset flips on a coarse checker so that one configuration reaches a sample that stays
            // inside the frame, a sample that leaves it, and a sample that lands on the near block in front of the
            // glass. The offset is declared in whole texels, so an unclamped sample always lands on a texel centre.
            int signX = ((pixel.x >> 4u) & 1u) != 0u ? 1 : -1;
            int signY = ((pixel.y >> 4u) & 1u) != 0u ? 1 : -1;
            float2 offsetTexels =
                float2(float(RefractionOffsetTexelsX * signX), float(RefractionOffsetTexelsY * signY));
            requestedOffset = offsetTexels / float2(sourceExtent);
            appliedOffset = requestedOffset;
            requestedLength = length(requestedOffset);
            if (requestedLength > RefractionMaximumOffsetUv)
            {
                appliedOffset = requestedOffset * (RefractionMaximumOffsetUv / requestedLength);
                refractionClamped = 1u;
            }
            float2 candidateUv = surfaceUv + appliedOffset;
            bool outOfBounds =
                candidateUv.x < 0.0f || candidateUv.x > 1.0f || candidateUv.y < 0.0f || candidateUv.y > 1.0f;
            if (!outOfBounds)
            {
                candidateTexel = uint2(min((uint)floor(candidateUv.x * float(sourceExtent.x)), sourceExtent.x - 1u),
                                       min((uint)floor(candidateUv.y * float(sourceExtent.y)), sourceExtent.y - 1u));
            }
            uint candidateIndex = (candidateTexel.y * DisplayWidth) + candidateTexel.x;
            PixelRecord candidateRecord = Records[candidateIndex];
            // The opaque surface at the offset location is in front of the refracting surface, so it is not behind
            // the glass at all. Sampling it would drag a foreground object into the refraction and smear it across
            // the glass, which is the artefact every screen-space refraction implementation is judged on.
            bool foregroundOccluder = (Flags & FlagRejectForegroundOccluders) != 0u &&
                                      candidateRecord.hasOpaqueSurface != 0u &&
                                      candidateRecord.opaqueViewDepthMetres < depth;
            if (outOfBounds)
            {
                fallback = RefractionFallbackOutOfBounds;
            }
            else if (foregroundOccluder)
            {
                fallback = RefractionFallbackForegroundOccluder;
            }
            if (fallback != RefractionFallbackNone)
            {
                // The fallback is always the surface's own UV, so a refraction that fails degrades to clear glass
                // rather than to a wrong part of the scene.
                appliedOffset = float2(0.0f, 0.0f);
                sampledUv = surfaceUv;
            }
            else
            {
                sampledUv = candidateUv;
            }
            uint2 sampledTexel = uint2(min((uint)floor(sampledUv.x * float(sourceExtent.x)), sourceExtent.x - 1u),
                                       min((uint)floor(sampledUv.y * float(sourceExtent.y)), sourceExtent.y - 1u));
            uint sampledIndex = (sampledTexel.y * DisplayWidth) + sampledTexel.x;
            float3 refracted = RefractionSource[sampledIndex].rgb;
            // The glass carries its own shaded radiance plus the background radiance it transmits from the refracted
            // direction. The transmitted term is a plausible screen-space distortion of a copy of the opaque scene,
            // not transported light, and nothing here models dispersion, total internal reflection or absorption.
            straight = tint + refracted;
            refractionUsed = 1u;
            refractionFallback = fallback;
            record.refractionSurfaceU = surfaceUv.x;
            record.refractionSurfaceV = surfaceUv.y;
            record.refractionRequestedU = requestedOffset.x;
            record.refractionRequestedV = requestedOffset.y;
            record.refractionAppliedU = appliedOffset.x;
            record.refractionAppliedV = appliedOffset.y;
            record.refractionSampledU = sampledUv.x;
            record.refractionSampledV = sampledUv.y;
            record.refractionRequestedLengthUv = requestedLength;
            record.refractionAppliedLengthUv = length(appliedOffset);
            record.refractionSampledTexelX = sampledTexel.x;
            record.refractionSampledTexelY = sampledTexel.y;
            record.refractionCandidateTexelX = candidateTexel.x;
            record.refractionCandidateTexelY = candidateTexel.y;
            record.refractionCandidateInBounds = outOfBounds ? 0u : 1u;
        }

        float3 premultiplied = Premultiply(straight, alpha);
        float fogTransmittance = 1.0f;
        uint fragmentFlags = FragmentStored;
        if (FogApplicationId == FogPerFragmentBeforeComposite && FogDensityPerMetre > 0.0f)
        {
            premultiplied = ApplyFogToFragment(premultiplied, alpha, depth, fogTransmittance);
            fragmentFlags |= FragmentFogged;
        }
        if (refracts)
        {
            fragmentFlags |= FragmentRefracted;
            if (fallback != RefractionFallbackNone)
            {
                fragmentFlags |= FragmentRefractionFallback;
            }
        }
        if (pane.coverageMode == CoverageAlphaTest)
        {
            fragmentFlags |= FragmentAlphaTested;
        }
        if (pane.coverageMode == CoverageStochastic)
        {
            fragmentFlags |= FragmentStochastic;
        }

        FragmentRecord fragment = (FragmentRecord)0;
        fragment.straightR = straight.r;
        fragment.straightG = straight.g;
        fragment.straightB = straight.b;
        fragment.premultipliedR = premultiplied.r;
        fragment.premultipliedG = premultiplied.g;
        fragment.premultipliedB = premultiplied.b;
        fragment.alpha = alpha;
        fragment.requestedAlpha = float(paneRequestedAlpha[slot]) / float(FixedOne);
        fragment.viewDepthMetres = depth;
        fragment.oitWeight = OitFragmentWeight(depth);
        fragment.fogTransmittance = fogTransmittance;
        fragment.refractionOffsetLengthUv = length(appliedOffset);
        fragment.revealageBefore = 1.0f;
        fragment.revealageAfter = 1.0f;
        fragment.transmittanceInFront = 1.0f;
        fragment.lightScale = lightScale.r;
        fragment.paneIndex = pane.paneIndex;
        fragment.drawOrder = pane.drawOrder;
        fragment.primitiveId = pane.primitiveId;
        fragment.coverageMode = pane.coverageMode;
        fragment.outcome = OutcomeComposited;
        fragment.sortedPosition = sortedPosition;
        fragment.objectPosition = objectPosition;
        fragment.flags = fragmentFlags;
        fragment.clusterIndex = fragmentCluster;
        fragment.sliceIndex = sliceIndex;
        fragment.lightOffset = lightOffset;
        fragment.lightCount = lightCount;
        fragment.acceptedSampleMaskLow = paneMaskLow[slot];
        fragment.acceptedSampleMaskHigh = paneMaskHigh[slot];
        fragment.acceptedSampleCount = paneAccepted[slot];
        fragment.sampleCount = pane.coverageMode == CoverageStochastic ? StochasticSampleCount : 0u;
        Fragments[fragmentBase + sortedPosition] = fragment;

        // The cluster evidence the pixel publishes is the nearest stored fragment's, which is the fragment a viewer
        // is looking at through everything else.
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
    record.droppedFragmentCount = droppedCount;
    record.depthRejectedCount = depthRejected;
    record.coverageRejectedCount = coverageRejected;
    record.alphaTestPassCount = alphaTestPass;
    record.alphaTestFailCount = alphaTestFail;
    record.stochasticSampleCount = hasStochastic != 0u ? StochasticSampleCount : 0u;
    record.stochasticAcceptedCount = stochasticAccepted;
    record.stochasticMaskLow = stochasticMaskLow;
    record.stochasticMaskHigh = stochasticMaskHigh;
    record.stochasticPixelAccepted = stochasticPixelAccepted;
    record.hasStochasticFragment = hasStochastic;
    record.overflowed = droppedCount > 0u ? 1u : 0u;
    record.refractionUsed = refractionUsed;
    record.refractionFallback = refractionFallback;
    record.refractionClamped = refractionClamped;
    record.fragmentSliceIndex = clusterSlice;
    record.clusterIndex = clusterIndex;
    record.lightOffset = clusterLightOffset;
    record.lightCount = clusterLightCount;
    record.hasClusterBinding = hasClusterBinding;
    record.clusterAmbiguous =
        (hasClusterBinding != 0u && record.hasOpaqueSurface != 0u && clusterSlice == record.opaqueSliceIndex) ? 1u : 0u;
    record.status |= StatusFragments | StatusPerPixelSorted;
    if (coverageDecided != 0u)
    {
        record.status |= StatusCoverageDecided;
    }
    if (droppedCount > 0u)
    {
        record.status |= StatusCapacityOverflow;
    }
    if (depthRejected > 0u)
    {
        record.status |= StatusDepthRejected;
    }
    if (refractionUsed != 0u)
    {
        record.status |= StatusRefractionApplied;
    }
    if (refractionFallback != RefractionFallbackNone)
    {
        record.status |= StatusRefractionFallback;
    }
    Records[index] = record;
}

// ---------------------------------------------------------------------------------------------------------------
// Stage: ForwardComposite. The same fragments composited twice, in two different orders, so the cost of the cheaper
// order is measured rather than asserted.
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

    float3 sortedColor = float3(0.0f, 0.0f, 0.0f);
    float sortedAlpha = 0.0f;
    float revealage = 1.0f;
    uint composited = 0u;
    uint fullyTransparent = 0u;
    uint depthTies = 0u;
    uint depthWrites = 0u;
    uint depthWritten = 0u;
    float writtenDepth = 0.0f;

    uint position = 0u;
    for (position = 0u; position < count; ++position)
    {
        FragmentRecord fragment = Fragments[base + position];
        fragment.revealageBefore = revealage;
        if (position > 0u && Fragments[base + position - 1u].viewDepthMetres == fragment.viewDepthMetres)
        {
            // A nonzero tie count means the image depends on the declared tie-break rather than on geometry.
            fragment.flags |= FragmentTiedWithPrevious;
            ++depthTies;
        }
        if (fragment.alpha == 0.0f)
        {
            // Alpha zero with a black premultiplied colour is the source-over identity. It is recorded as seen and
            // excluded from the composited count rather than counted as a contribution that did nothing.
            fragment.outcome = OutcomeRejectedAsFullyTransparent;
            fragment.revealageAfter = revealage;
            ++fullyTransparent;
            Fragments[base + position] = fragment;
            continue;
        }
        float3 source = float3(fragment.premultipliedR, fragment.premultipliedG, fragment.premultipliedB);
        SourceOver(source, fragment.alpha, sortedColor, sortedAlpha);
        revealage *= 1.0f - fragment.alpha;
        fragment.revealageAfter = revealage;
        fragment.outcome = OutcomeComposited;
        ++composited;
        if (DepthWritePolicyId == DepthWriteCoverageDecided && fragment.coverageMode != CoverageAlphaBlend)
        {
            // The walk runs far to near, so the last writer is the nearest surviving coverage-decided fragment, and
            // that fragment was composited with an alpha of exactly one. The depth write and the composite can
            // therefore never disagree about whether the surface is opaque.
            fragment.flags |= FragmentWritesDepth;
            ++depthWrites;
            depthWritten = 1u;
            writtenDepth = fragment.viewDepthMetres;
        }
        Fragments[base + position] = fragment;
    }

    // Survival is a fact about the fragments in front, so it is only known after the walk finishes. A fragment with
    // a large alpha and a tiny survival fraction is invisible, and that is exactly the fragment a bounded store
    // should have dropped.
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

    // The same fragments in the order the CPU sorted the draws into. It is a total order over the draws and it is
    // not a per-pixel order, which is exactly why the two results differ wherever two surfaces interpenetrate.
    float3 objectColor = float3(0.0f, 0.0f, 0.0f);
    float objectAlpha = 0.0f;
    float objectRevealage = 1.0f;
    for (position = 0u; position < count; ++position)
    {
        for (uint candidate = 0u; candidate < count; ++candidate)
        {
            FragmentRecord fragment = Fragments[base + candidate];
            if (fragment.objectPosition != position || fragment.alpha == 0.0f)
            {
                continue;
            }
            float3 source = float3(fragment.premultipliedR, fragment.premultipliedG, fragment.premultipliedB);
            SourceOver(source, fragment.alpha, objectColor, objectAlpha);
            objectRevealage *= 1.0f - fragment.alpha;
        }
    }

    record.sortedLayerR = sortedColor.r;
    record.sortedLayerG = sortedColor.g;
    record.sortedLayerB = sortedColor.b;
    record.sortedLayerAlpha = sortedAlpha;
    record.sortedRevealage = revealage;
    float3 sortedOver = CompositeOverOpaqueBackground(sortedColor, sortedAlpha, background);
    record.sortedOverR = sortedOver.r;
    record.sortedOverG = sortedOver.g;
    record.sortedOverB = sortedOver.b;

    record.objectLayerR = objectColor.r;
    record.objectLayerG = objectColor.g;
    record.objectLayerB = objectColor.b;
    record.objectLayerAlpha = objectAlpha;
    record.objectRevealage = objectRevealage;
    float3 objectOver = CompositeOverOpaqueBackground(objectColor, objectAlpha, background);
    record.objectOverR = objectOver.r;
    record.objectOverG = objectOver.g;
    record.objectOverB = objectOver.b;

    record.compositedCount = composited;
    record.fullyTransparentCount = fullyTransparent;
    record.depthTieCount = depthTies;
    record.depthWriteCount = depthWrites;
    record.depthWritten = depthWritten;
    record.writtenViewDepthMetres = writtenDepth;
    record.status |= StatusForwardComposite | StatusObjectOrdered;
    Records[index] = record;
}

// ---------------------------------------------------------------------------------------------------------------
// Stage: OitAccumulate. A sum and a product, both commutative and associative in exact arithmetic, which is the
// whole basis of the technique's order independence. The traversal order is a configuration knob precisely so that
// the independence can be measured instead of promised.
// ---------------------------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)] void OitAccumulateCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    uint index = TexelIndex(dispatchThreadId.xy);
    PixelRecord record = Records[index];
    uint base = index * LabFragmentCapacity;
    uint count = min(record.storedFragmentCount, LabFragmentCapacity);

    float3 colorSumA = float3(0.0f, 0.0f, 0.0f);
    float alphaSumA = 0.0f;
    float revealageA = 1.0f;
    float3 colorSumB = float3(0.0f, 0.0f, 0.0f);
    float alphaSumB = 0.0f;
    float revealageB = 1.0f;
    uint accumulated = 0u;

    for (uint step = 0u; step < count; ++step)
    {
        uint position = step;
        bool secondAccumulator = false;
        if (OitTraversalId == TraversalReversed || OitTraversalId == TraversalNearToFar)
        {
            // The stored order is far to near, so visiting it backwards is near to far.
            position = count - 1u - step;
        }
        else if (OitTraversalId == TraversalEvenThenOdd)
        {
            uint evenCount = (count + 1u) / 2u;
            position = step < evenCount ? (step * 2u) : (((step - evenCount) * 2u) + 1u);
        }
        else if (OitTraversalId == TraversalSplitMerge)
        {
            secondAccumulator = step >= (count / 2u);
        }

        FragmentRecord fragment = Fragments[base + position];
        float weight = fragment.oitWeight;
        float3 weighted = float3(fragment.premultipliedR, fragment.premultipliedG, fragment.premultipliedB) * weight;
        if (secondAccumulator)
        {
            colorSumB += weighted;
            alphaSumB += fragment.alpha * weight;
            revealageB *= 1.0f - fragment.alpha;
        }
        else
        {
            colorSumA += weighted;
            alphaSumA += fragment.alpha * weight;
            revealageA *= 1.0f - fragment.alpha;
        }
        ++accumulated;
    }

    // Merging two partial accumulators is exactly what independent GPU threads writing the same pixel through
    // additive and multiplicative blending do, so the merge is written out rather than hidden inside the loop.
    float3 colorSum = colorSumA + colorSumB;
    float alphaSum = alphaSumA + alphaSumB;
    float revealage = revealageA * revealageB;

    record.weightedColorSumR = colorSum.r;
    record.weightedColorSumG = colorSum.g;
    record.weightedColorSumB = colorSum.b;
    record.weightedAlphaSum = alphaSum;
    record.oitRevealage = revealage;
    record.oitFragmentCount = accumulated;
    record.status |= StatusOitAccumulated;
    Records[index] = record;
}

// ---------------------------------------------------------------------------------------------------------------
// Stage: OitResolve. What the technique gets exactly right is the coverage: revealage is the same product of
// transmissions the sorted reference accumulates. What it approximates is the colour: every fragment is replaced by
// the weighted average of all of them, so a red pane in front of a blue one and a blue pane in front of a red one
// resolve identically. It is exact in two declared cases and only two: a single fragment, and any number of
// fragments that share a colour.
// ---------------------------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)] void OitResolveCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    uint index = TexelIndex(dispatchThreadId.xy);
    PixelRecord record = Records[index];
    float3 background = float3(record.opaqueR, record.opaqueG, record.opaqueB);
    float3 colorSum = float3(record.weightedColorSumR, record.weightedColorSumG, record.weightedColorSumB);
    float alphaSum = record.weightedAlphaSum;
    float revealage = record.oitRevealage;

    // The guard is a floor on the divisor, not a clamp on the result: when the accumulated alpha is smaller than
    // this, the transparent layer is empty enough that its colour cannot matter.
    bool guarded = alphaSum < OitAlphaEpsilon;
    float divisor = max(alphaSum, OitAlphaEpsilon);
    float3 average = colorSum / divisor;
    float layerAlpha = 1.0f - revealage;
    float3 layer = average * layerAlpha;

    record.oitAverageR = average.r;
    record.oitAverageG = average.g;
    record.oitAverageB = average.b;
    record.oitLayerR = layer.r;
    record.oitLayerG = layer.g;
    record.oitLayerB = layer.b;
    record.oitLayerAlpha = layerAlpha;
    float3 over = CompositeOverOpaqueBackground(layer, layerAlpha, background);
    record.oitOverR = over.r;
    record.oitOverG = over.g;
    record.oitOverB = over.b;
    record.oitUsedAlphaEpsilon = guarded ? 1u : 0u;
    record.oitFullyRevealed = revealage == 1.0f ? 1u : 0u;
    record.oitFullyOccluded = revealage == 0.0f ? 1u : 0u;
    record.status |= StatusOitResolved;
    Records[index] = record;
}

// ---------------------------------------------------------------------------------------------------------------
// Stage: Compose.
// ---------------------------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)] void ComposeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    uint index = TexelIndex(dispatchThreadId.xy);
    PixelRecord record = Records[index];

    float3 sortedOver = float3(record.sortedOverR, record.sortedOverG, record.sortedOverB);
    float3 objectOver = float3(record.objectOverR, record.objectOverG, record.objectOverB);
    float3 oitOver = float3(record.oitOverR, record.oitOverG, record.oitOverB);

    // The approximation evidence. Nothing here claims the weighted blended path is exact; these are the numbers that
    // say by how much it is not, and the order error beside them says the same about the cheaper per-draw order.
    record.oitChannelError = MaximumChannel(abs(oitOver - sortedOver));
    record.oitLuminanceError = abs(SceneLuminance(oitOver) - SceneLuminance(sortedOver));
    record.oitAlphaError = abs(record.oitLayerAlpha - record.sortedLayerAlpha);
    record.orderChannelError = MaximumChannel(abs(objectOver - sortedOver));

    float3 composite = sortedOver;
    if (CompositeModeId == CompositeObjectSorted)
    {
        composite = objectOver;
    }
    else if (CompositeModeId == CompositeWeightedOit)
    {
        composite = oitOver;
    }
    record.compositeR = composite.r;
    record.compositeG = composite.g;
    record.compositeB = composite.b;
    record.compositeSource = CompositeModeId;

    // Screen-space fog after the composite is cheaper, it is what a deferred fog pass naturally does, and it is
    // wrong for every transparent pixel because it fogs the near glass at the depth of whatever is behind it. The
    // frame reports it as the approximation the policy chose rather than as a mistake it can fix.
    float3 fogged = composite;
    float transmittance = 1.0f;
    if (FogApplicationId == FogScreenSpaceAfterComposite)
    {
        float fogDepth = record.hasOpaqueSurface != 0u ? record.opaqueViewDepthMetres : OpaqueBaseDepthMetres;
        transmittance = FogTransmittance(fogDepth);
        fogged = ApplyFogToRadiance(composite, fogDepth);
        record.status |= StatusFogScreenSpace;
    }
    else
    {
        record.status |= StatusFogPerFragment;
    }
    record.fogTransmittance = transmittance;
    record.foggedR = fogged.r;
    record.foggedG = fogged.g;
    record.foggedB = fogged.b;

    // The reactive mask reports risk, not policy. The three terms are combined as a probabilistic union so that the
    // result is monotonic in every term, never leaves the unit interval, and never double counts a pixel that is
    // both refractive and stochastic.
    float alphaTerm = 0.0f;
    float refractionTerm = 0.0f;
    float stochasticTerm = 0.0f;
    if ((Flags & FlagEnableReactiveMask) != 0u)
    {
        float transparentAlpha = 1.0f - record.sortedRevealage;
        if ((Flags & FlagWroteTransparentMotionVector) == 0u)
        {
            alphaTerm = ReactiveAlphaWeight * transparentAlpha;
        }
        if (record.refractionUsed != 0u)
        {
            // Refraction contributes even when the surface wrote a motion vector, because the refracted background
            // moves with the background rather than with the glass. A fallback counts as fully reactive: the sample
            // position jumps between the refracted and the unrefracted texel as the surface moves.
            float normalized = record.refractionFallback != RefractionFallbackNone
                                   ? 1.0f
                                   : min(1.0f, record.refractionAppliedLengthUv / ReactiveReferenceOffsetUv);
            refractionTerm = ReactiveRefractionWeight * normalized;
        }
        if (record.hasStochasticFragment != 0u && (Flags & FlagAnimateStochastic) != 0u)
        {
            stochasticTerm = ReactiveStochasticWeight;
        }
        record.status |= StatusReactiveMask;
    }
    float combined = ProbabilisticOr(ProbabilisticOr(alphaTerm, refractionTerm), stochasticTerm);
    record.reactiveAlphaTerm = alphaTerm;
    record.reactiveRefractionTerm = refractionTerm;
    record.reactiveStochasticTerm = stochasticTerm;
    record.reactiveMask = min(combined, ReactiveMaximumMask);

    // The one display exposure, applied once, after the composite. Applying it before would expose the transparent
    // layer differently from the scene behind it; applying it twice would be invisible in the image and obvious in
    // the published ratio, which is divided back out of this pixel's own radiance rather than copied from the
    // constant that was uploaded.
    float3 exposed = fogged * DisplayExposureScale;
    record.exposedR = exposed.r;
    record.exposedG = exposed.g;
    record.exposedB = exposed.b;
    PublishNetExposureScale(fogged, exposed, record);
    record.exposureApplicationCount += 1u;
    record.status |= StatusExposure;

    float3 displayLinear = ToneMap(exposed);
    record.status |= StatusToneMap;
    float3 encoded = EncodeDisplay(displayLinear);
    record.finalR = encoded.r;
    record.finalG = encoded.g;
    record.finalB = encoded.b;
    record.status |= StatusDisplayEncode;
    Records[index] = record;

    // Frame-level evidence. Every counter is an integer, so the totals are a property of the dispatch rather than of
    // the order its groups happened to finish in, and every maximum is accumulated as the bit pattern of a
    // non-negative float, which orders identically to the float itself.
    InterlockedAdd(Frame[0].candidateFragmentCount, record.candidateFragmentCount);
    InterlockedAdd(Frame[0].storedFragmentCount, record.storedFragmentCount);
    InterlockedAdd(Frame[0].droppedFragmentCount, record.droppedFragmentCount);
    InterlockedAdd(Frame[0].compositedFragmentCount, record.compositedCount);
    InterlockedAdd(Frame[0].depthRejectedFragmentCount, record.depthRejectedCount);
    InterlockedAdd(Frame[0].coverageRejectedFragmentCount, record.coverageRejectedCount);
    InterlockedAdd(Frame[0].fullyTransparentFragmentCount, record.fullyTransparentCount);
    InterlockedAdd(Frame[0].depthTieCount, record.depthTieCount);
    InterlockedAdd(Frame[0].depthWriteCount, record.depthWriteCount);
    InterlockedAdd(Frame[0].alphaTestPassCount, record.alphaTestPassCount);
    InterlockedAdd(Frame[0].alphaTestFailCount, record.alphaTestFailCount);
    if (record.hasStochasticFragment != 0u)
    {
        InterlockedAdd(Frame[0].stochasticPixelCount, 1u);
        InterlockedAdd(Frame[0].stochasticAcceptedPixelCount, record.stochasticPixelAccepted);
    }
    if (record.overflowed != 0u)
    {
        InterlockedAdd(Frame[0].overflowPixelCount, 1u);
        InterlockedOr(Frame[0].overflowFlags, OverflowFragmentCapacity);
    }
    if (record.candidateFragmentCount > LabPaneCount)
    {
        InterlockedOr(Frame[0].overflowFlags, OverflowCandidateCount);
    }
    if (record.weightedAlphaSum > MaximumWeightedAccumulation)
    {
        InterlockedOr(Frame[0].overflowFlags, OverflowWeightedAccumulation);
    }
    if (record.refractionUsed != 0u)
    {
        InterlockedAdd(Frame[0].refractionPixelCount, 1u);
        if (record.refractionFallback != RefractionFallbackNone)
        {
            InterlockedAdd(Frame[0].refractionFallbackPixelCount, 1u);
        }
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
    InterlockedMax(Frame[0].maximumOitChannelErrorBits, asuint(record.oitChannelError));
    InterlockedMax(Frame[0].maximumOitAlphaErrorBits, asuint(record.oitAlphaError));
    InterlockedMax(Frame[0].maximumOrderChannelErrorBits, asuint(record.orderChannelError));
    InterlockedMax(Frame[0].maximumReactiveMaskBits, asuint(record.reactiveMask));
    InterlockedMax(Frame[0].maximumWeightedAlphaSumBits, asuint(record.weightedAlphaSum));
}
