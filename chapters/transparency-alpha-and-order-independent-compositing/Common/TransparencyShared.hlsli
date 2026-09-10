// Chapter 32 shared GPU lab support.
//
// This file owns the parts of the lab that are not the lesson: the constant-buffer ABI, the resource declarations,
// the record layouts the C++ side mirrors, the deterministic analytic scene, the declared cluster slicing, the
// reproducible sample hash, and the display path. It deliberately owns no premultiplication, no source-over, no
// sort, no coverage decision, no weighted accumulation, no resolve, no refraction sample, no fog fold and no
// reactive mask. Those live in each variant's own TransparencyLab.hlsl, because a shared helper that quietly
// composited would leave the learner with a chapter about calling a function.
//
// Three conventions hold everywhere below:
//
//   * Colour is scene-linear Rec.709 radiance, already multiplied by the frame pre-exposure. This chapter never
//     applies or removes a scene pre-exposure; the one display exposure it does apply happens once, after the
//     composite, and every pixel counts the application so a second one is visible in the record.
//   * Depth is view-space depth in metres, strictly positive, increasing away from the camera. Back to front means
//     descending view depth.
//   * Every authored colour, alpha and rectangle in the scene is an integer in units of 1 / FixedOne or of the
//     normalized 256-unit coordinate system, so the float the shader forms and the double the CPU contract forms
//     are the same dyadic rational and a test can demand exact agreement wherever the arithmetic downstream stays
//     inside a float's mantissa.

#ifndef LGP_CH32_TRANSPARENCY_SHARED_HLSLI
#define LGP_CH32_TRANSPARENCY_SHARED_HLSLI

// Mirrors ch32::transparency::gpu::LabStage.
static const uint StageClear = 0u;
static const uint StageOpaque = 1u;
static const uint StageRefractionSource = 2u;
static const uint StageFragments = 3u;
static const uint StageForwardComposite = 4u;
static const uint StageOitAccumulate = 5u;
static const uint StageOitResolve = 6u;
static const uint StageCompose = 7u;

// Mirrors ch32::transparency::gpu status bits.
static const uint StatusOpaque = 1u << 0u;
static const uint StatusRefractionSource = 1u << 1u;
static const uint StatusFragments = 1u << 2u;
static const uint StatusPerPixelSorted = 1u << 3u;
static const uint StatusObjectOrdered = 1u << 4u;
static const uint StatusForwardComposite = 1u << 5u;
static const uint StatusOitAccumulated = 1u << 6u;
static const uint StatusOitResolved = 1u << 7u;
static const uint StatusFogPerFragment = 1u << 8u;
static const uint StatusFogScreenSpace = 1u << 9u;
static const uint StatusReactiveMask = 1u << 10u;
static const uint StatusExposure = 1u << 11u;
static const uint StatusToneMap = 1u << 12u;
static const uint StatusDisplayEncode = 1u << 13u;
static const uint StatusBlendOnly = 1u << 14u;
static const uint StatusCoverageDecided = 1u << 15u;
static const uint StatusCapacityOverflow = 1u << 16u;
static const uint StatusRefractionApplied = 1u << 17u;
static const uint StatusRefractionFallback = 1u << 18u;
static const uint StatusClusterConsumed = 1u << 19u;
static const uint StatusDepthRejected = 1u << 20u;

// Mirrors ch32::transparency::gpu overflow flags.
static const uint OverflowFragmentCapacity = 1u << 0u;
static const uint OverflowWeightedAccumulation = 1u << 1u;
static const uint OverflowCandidateCount = 1u << 2u;

// Mirrors ch32::transparency::gpu fragment flags.
static const uint FragmentStored = 1u << 0u;
static const uint FragmentDepthRejected = 1u << 1u;
static const uint FragmentCoverageRejected = 1u << 2u;
static const uint FragmentDropped = 1u << 3u;
static const uint FragmentRefracted = 1u << 4u;
static const uint FragmentRefractionFallback = 1u << 5u;
static const uint FragmentFogged = 1u << 6u;
static const uint FragmentWritesDepth = 1u << 7u;
static const uint FragmentTiedWithPrevious = 1u << 8u;
static const uint FragmentAlphaTested = 1u << 9u;
static const uint FragmentStochastic = 1u << 10u;

// Mirrors ch32::transparency::gpu pane flags.
static const uint PaneRefracts = 1u << 0u;
static const uint PaneFogged = 1u << 1u;

// Mirrors ch32::transparency::gpu configuration flag bits.
static const uint FlagAnimateStochastic = 1u << 0u;
static const uint FlagEnableRefraction = 1u << 1u;
static const uint FlagEnableReactiveMask = 1u << 2u;
static const uint FlagRejectForegroundOccluders = 1u << 3u;
static const uint FlagTestAgainstOpaqueDepth = 1u << 4u;
static const uint FlagWroteTransparentMotionVector = 1u << 5u;

// Mirrors ch32::transparency::CoverageMode.
static const uint CoverageAlphaBlend = 0u;
static const uint CoverageAlphaTest = 1u;
static const uint CoverageStochastic = 2u;

// Mirrors ch32::transparency::FragmentOutcome, plus the two outcomes the lab adds for decisions the contract's
// single-pixel reference never sees because they happen before a fragment reaches it.
static const uint OutcomeComposited = 0u;
static const uint OutcomeRejectedByOpaqueDepth = 1u;
static const uint OutcomeRejectedAsFullyTransparent = 2u;
static const uint OutcomeRejectedByCoverage = 3u;
static const uint OutcomeDroppedByCapacity = 4u;

// Mirrors ch32::transparency::FragmentOverflowPolicy.
static const uint OverflowKeepNearest = 0u;
static const uint OverflowKeepFirstSubmitted = 1u;

// Mirrors ch32::transparency::gpu::CompositeMode.
static const uint CompositeSortedReference = 0u;
static const uint CompositeObjectSorted = 1u;
static const uint CompositeWeightedOit = 2u;

// Mirrors ch32::transparency::gpu::OitTraversal.
static const uint TraversalStored = 0u;
static const uint TraversalReversed = 1u;
static const uint TraversalNearToFar = 2u;
static const uint TraversalEvenThenOdd = 3u;
static const uint TraversalSplitMerge = 4u;

// Mirrors ch32::transparency::OitWeightFunction.
static const uint WeightUniform = 0u;
static const uint WeightInverseDepthPolynomial = 1u;

// Mirrors ch32::transparency::FogApplication.
static const uint FogPerFragmentBeforeComposite = 0u;
static const uint FogScreenSpaceAfterComposite = 1u;

// Mirrors ch32::transparency::DepthWritePolicy.
static const uint DepthWriteNever = 0u;
static const uint DepthWriteCoverageDecided = 1u;

// Mirrors ch32::transparency::RefractionFallback.
static const uint RefractionFallbackNone = 0u;
static const uint RefractionFallbackOutOfBounds = 1u;
static const uint RefractionFallbackForegroundOccluder = 2u;

// Mirrors ch32::transparency::gpu::AlphaModulation.
static const uint AlphaConstant = 0u;
static const uint AlphaChecker = 1u;
static const uint AlphaHorizontalGradient = 2u;

// Mirrors ch32::transparency::gpu::TransferFunction.
static const uint TransferLinear = 0u;
static const uint TransferSrgb = 1u;

// Mirrors ch32::transparency::gpu::DebugView.
static const uint ViewFinal = 0u;
static const uint ViewOpaqueRadiance = 1u;
static const uint ViewOpaqueDepth = 2u;
static const uint ViewRefractionSource = 3u;
static const uint ViewFragmentCount = 4u;
static const uint ViewSortedComposite = 5u;
static const uint ViewObjectSortedComposite = 6u;
static const uint ViewOrderError = 7u;
static const uint ViewWeightedOitComposite = 8u;
static const uint ViewOitApproximationError = 9u;
static const uint ViewRevealage = 10u;
static const uint ViewTransparentLayerAlpha = 11u;
static const uint ViewDepthRejection = 12u;
static const uint ViewCoverageDecision = 13u;
static const uint ViewStochasticMask = 14u;
static const uint ViewCapacityOverflow = 15u;
static const uint ViewClusterIndex = 16u;
static const uint ViewLightCount = 17u;
static const uint ViewRefraction = 18u;
static const uint ViewFog = 19u;
static const uint ViewReactiveMask = 20u;
static const uint ViewStageOrder = 21u;
static const uint ViewStatus = 22u;

// Mirrors the contract's units and bounds.
static const uint FixedOne = 1024u;
static const uint NormalizedExtent = 256u;
static const uint LabFragmentCapacity = 8u;
static const uint LabPaneCount = 13u;
static const uint AbiMarker = 0x54523332u;
static const float MaximumSceneLinearValue = 1.0e6f;
static const float MaximumWeightedAccumulation = 1.0e12f;
static const float OitDepthWeightNumerator = 10.0f;
static const float OitDepthWeightBias = 1.0e-5f;

// The analytic opaque scene. Every constant is a negative power of two or an integer, so the depth and the radiance
// the transparent pass is tested and composited against are exact at every pixel.
static const float OpaqueBaseDepthMetres = 24.0f;
static const float OpaqueDepthSlopeV = 0.03125f; // 1 / 32 metres per normalized unit, so the wall runs 24 m to 16 m.
static const float NearBlockDepthMetres = 4.0f;
// The near block sits inside the refracting pane's rectangle rather than beside it, so that a screen-space offset
// taken from a neighbouring pane pixel can land on a surface that is in front of the glass. Without a reachable
// foreground occluder the fallback the contract names would be unreachable, and an unreachable branch is not
// evidence of anything.
static const uint NearBlockMinU = 160u;
static const uint NearBlockMaxU = 192u;
static const uint NearBlockMinV = 32u;
static const uint NearBlockMaxV = 96u;
// A sky band with no opaque surface at all, so the depth test has a case with nothing to reject against and the
// cluster seam has a case with no opaque cluster to be confused with.
static const uint SkyBandMaxV = 24u;
static const uint SkyFixedR = 256u;
static const uint SkyFixedG = 320u;
static const uint SkyFixedB = 448u;

cbuffer LabConstants : register(b0)
{
    uint DisplayWidth;
    uint DisplayHeight;
    uint VariantId;
    uint SceneVariantId;
    uint ActivePaneCount;
    uint FragmentCapacity;
    uint OverflowPolicyId;
    uint CompositeModeId;
    uint OitTraversalId;
    uint OitWeightFunctionId;
    uint FogApplicationId;
    uint DepthWritePolicyId;
    uint TransferFunctionId;
    uint DebugViewId;
    uint FrameIndex;
    uint StochasticSampleCount;
    uint StochasticSeed;
    uint SceneLightCount;
    uint LightIndexCount;
    uint ClusterTileCountX;
    uint ClusterTileCountY;
    uint ClusterSliceCount;
    uint ClusterCount;
    uint StageOrderWord;
    uint StageCount;
    uint ExpectedStatus;
    uint Flags;
    uint AlphaTestThresholdFixed;
    int RefractionOffsetTexelsX;
    int RefractionOffsetTexelsY;
    float AlphaTestThreshold;
    float UniformOitWeight;
    float OitNearScaleMetres;
    float OitFarScaleMetres;
    float OitMinimumWeight;
    float OitMaximumWeight;
    float OitAlphaEpsilon;
    float FogDensityPerMetre;
    float FogInscatterR;
    float FogInscatterG;
    float FogInscatterB;
    float RefractionMaximumOffsetUv;
    float ReactiveAlphaWeight;
    float ReactiveRefractionWeight;
    float ReactiveStochasticWeight;
    float ReactiveReferenceOffsetUv;
    float ReactiveMaximumMask;
    float DisplayExposureScale;
    float ClusterSliceDepthMetres;
};

struct PaneRecord
{
    float baseDepthMetres;
    float depthSlopeU;
    float depthSlopeV;
    float paddingDepth;
    uint minU;
    uint maxU;
    uint minV;
    uint maxV;
    uint colorFixedR;
    uint colorFixedG;
    uint colorFixedB;
    uint alphaFixed;
    uint alphaModulation;
    uint coverageMode;
    uint drawOrder;
    uint primitiveId;
    uint paneIndex;
    uint flags;
    uint paddingA;
    uint paddingB;
};

struct LightRecord
{
    float radianceR;
    float radianceG;
    float radianceB;
    float padding;
};

struct ClusterRange
{
    uint lightOffset;
    uint lightCount;
};

struct FragmentRecord
{
    float straightR;
    float straightG;
    float straightB;
    float premultipliedR;
    float premultipliedG;
    float premultipliedB;
    float alpha;
    float requestedAlpha;
    float viewDepthMetres;
    float oitWeight;
    float fogTransmittance;
    float refractionOffsetLengthUv;
    float revealageBefore;
    float revealageAfter;
    float transmittanceInFront;
    float lightScale;
    uint paneIndex;
    uint drawOrder;
    uint primitiveId;
    uint coverageMode;
    uint outcome;
    uint sortedPosition;
    uint objectPosition;
    uint flags;
    uint clusterIndex;
    uint sliceIndex;
    uint lightOffset;
    uint lightCount;
    uint acceptedSampleMaskLow;
    uint acceptedSampleMaskHigh;
    uint acceptedSampleCount;
    uint sampleCount;
};

struct PixelRecord
{
    float opaqueR;
    float opaqueG;
    float opaqueB;
    float opaqueViewDepthMetres;
    float refractionSourceR;
    float refractionSourceG;
    float refractionSourceB;
    float paddingSource;

    float sortedLayerR;
    float sortedLayerG;
    float sortedLayerB;
    float sortedLayerAlpha;
    float sortedOverR;
    float sortedOverG;
    float sortedOverB;
    float sortedRevealage;

    float objectLayerR;
    float objectLayerG;
    float objectLayerB;
    float objectLayerAlpha;
    float objectOverR;
    float objectOverG;
    float objectOverB;
    float objectRevealage;

    float weightedColorSumR;
    float weightedColorSumG;
    float weightedColorSumB;
    float weightedAlphaSum;
    float oitAverageR;
    float oitAverageG;
    float oitAverageB;
    float oitRevealage;
    float oitLayerR;
    float oitLayerG;
    float oitLayerB;
    float oitLayerAlpha;
    float oitOverR;
    float oitOverG;
    float oitOverB;
    float paddingOit;

    float oitChannelError;
    float oitLuminanceError;
    float oitAlphaError;
    float orderChannelError;

    float compositeR;
    float compositeG;
    float compositeB;
    float fogTransmittance;
    float foggedR;
    float foggedG;
    float foggedB;
    float paddingFog;

    float refractionSurfaceU;
    float refractionSurfaceV;
    float refractionRequestedU;
    float refractionRequestedV;
    float refractionAppliedU;
    float refractionAppliedV;
    float refractionSampledU;
    float refractionSampledV;
    float refractionRequestedLengthUv;
    float refractionAppliedLengthUv;
    float writtenViewDepthMetres;
    float paddingRefraction;

    float reactiveMask;
    float reactiveAlphaTerm;
    float reactiveRefractionTerm;
    float reactiveStochasticTerm;

    float exposedR;
    float exposedG;
    float exposedB;
    // exposed[c] / fogged[c] on the deterministically chosen channel c, or zero where the pre-exposure radiance was
    // zero in every channel and no ratio exists.
    float netExposureScale;

    float finalR;
    float finalG;
    float finalB;
    float paddingFinal;

    uint hasOpaqueSurface;
    uint candidateFragmentCount;
    uint storedFragmentCount;
    uint droppedFragmentCount;
    uint compositedCount;
    uint depthRejectedCount;
    uint coverageRejectedCount;
    uint fullyTransparentCount;
    uint depthTieCount;
    uint depthWriteCount;
    uint alphaTestPassCount;
    uint alphaTestFailCount;
    uint stochasticSampleCount;
    uint stochasticAcceptedCount;
    uint stochasticMaskLow;
    uint stochasticMaskHigh;
    uint stochasticPixelAccepted;
    uint hasStochasticFragment;
    uint overflowed;
    uint refractionUsed;
    uint refractionFallback;
    uint refractionClamped;
    uint refractionSampledTexelX;
    uint refractionSampledTexelY;
    uint refractionCandidateTexelX;
    uint refractionCandidateTexelY;
    uint refractionCandidateInBounds;
    uint tileX;
    uint tileY;
    uint fragmentSliceIndex;
    uint opaqueSliceIndex;
    uint clusterIndex;
    uint opaqueClusterIndex;
    uint clusterAmbiguous;
    uint lightOffset;
    uint lightCount;
    uint hasClusterBinding;
    uint oitFragmentCount;
    uint oitUsedAlphaEpsilon;
    uint oitFullyRevealed;
    uint oitFullyOccluded;
    uint depthWritten;
    uint exposureApplicationCount;
    uint netExposureScaleDerived;
    uint netExposureScaleChannel;
    uint compositeSource;
    uint stageOrderWord;
    uint status;
    uint abiMarker;
};

struct FrameRecord
{
    uint abiMarker;
    uint variant;
    uint stageOrderWord;
    uint stageCount;
    uint activePaneCount;
    uint clusterTileCountX;
    uint clusterTileCountY;
    uint clusterSliceCount;
    uint clusterCount;
    uint sceneLightCount;
    uint lightIndexCount;
    uint overflowFlags;
    uint candidateFragmentCount;
    uint storedFragmentCount;
    uint droppedFragmentCount;
    uint compositedFragmentCount;
    uint depthRejectedFragmentCount;
    uint coverageRejectedFragmentCount;
    uint fullyTransparentFragmentCount;
    uint depthTieCount;
    uint depthWriteCount;
    uint alphaTestPassCount;
    uint alphaTestFailCount;
    uint stochasticAcceptedPixelCount;
    uint stochasticPixelCount;
    uint overflowPixelCount;
    uint refractionPixelCount;
    uint refractionFallbackPixelCount;
    uint clusterPixelCount;
    uint ambiguousClusterPixelCount;
    uint maximumStoredFragmentCount;
    uint compositeSource;
    uint maximumOitChannelErrorBits;
    uint maximumOitAlphaErrorBits;
    uint maximumOrderChannelErrorBits;
    uint maximumReactiveMaskBits;
    uint maximumWeightedAlphaSumBits;
    uint paddingA;
    uint paddingB;
    uint paddingC;
};

RWStructuredBuffer<PixelRecord> Records : register(u0);
// LabFragmentCapacity entries per pixel, written in stored order by the fragment stage and read by every stage
// after it. It is the lab's k-buffer, and it is bounded on purpose.
RWStructuredBuffer<FragmentRecord> Fragments : register(u1);
RWStructuredBuffer<FrameRecord> Frame : register(u2);
// The copy of the lit opaque scene that screen-space refraction reads. It is a separate resource rather than a
// re-evaluation of the opaque pass precisely because the technique's whole limitation is that it reads a copy taken
// before any transparency was composited.
RWStructuredBuffer<float4> RefractionSource : register(u3);

// Authored scene data, uploaded already sorted into the declared per-draw back-to-front order.
StructuredBuffer<PaneRecord> Panes : register(t0);
StructuredBuffer<LightRecord> Lights : register(t1);
// Chapter 14 owns the construction of these two. This chapter only consumes them.
StructuredBuffer<uint> LightIndices : register(t2);
StructuredBuffer<ClusterRange> Clusters : register(t3);

// The display pass reads the same two buffers the compute stages wrote, through their own read-only views, so the
// draw that produces the image is a genuine consumer of the dispatches rather than a second evaluation of them.
StructuredBuffer<PixelRecord> DisplayRecords : register(t4);
StructuredBuffer<FrameRecord> DisplayFrame : register(t5);

// ---------------------------------------------------------------------------------------------------------------
// Small shared utilities. None of these is a compositing decision.
// ---------------------------------------------------------------------------------------------------------------

uint TexelIndex(uint2 pixel)
{
    return (pixel.y * DisplayWidth) + pixel.x;
}

float SceneLuminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

bool IsFiniteColor(float3 color)
{
    return isfinite(color.r) && isfinite(color.g) && isfinite(color.b);
}

float MaximumChannel(float3 color)
{
    return max(max(color.r, color.g), color.b);
}

// The normalized integer coordinate the scene is authored in. It is an integer division, so the CPU table and the
// shader address the same rectangle at every extent.
uint2 NormalizedCoordinate(uint2 pixel)
{
    return uint2((pixel.x * NormalizedExtent) / DisplayWidth, (pixel.y * NormalizedExtent) / DisplayHeight);
}

// ---------------------------------------------------------------------------------------------------------------
// The declared cluster slicing. Chapter 14 owns the policy; the only thing this chapter has to get right is that a
// transparent fragment reads the cluster of its *own* depth, which is why the slice is a function of a depth
// argument rather than of anything the pixel already holds.
// ---------------------------------------------------------------------------------------------------------------

uint SliceIndexForDepth(float viewDepthMetres)
{
    if (!(viewDepthMetres > 0.0f))
    {
        return 0u;
    }
    uint slice = (uint)floor(viewDepthMetres / ClusterSliceDepthMetres);
    return min(slice, ClusterSliceCount - 1u);
}

uint ClusterIndexFor(uint tileX, uint tileY, uint sliceIndex)
{
    return tileX + (ClusterTileCountX * (tileY + (ClusterTileCountY * sliceIndex)));
}

// The light list the cluster declares, reduced to the scale a fragment in it is lit by. Chapter 5 owns what a light
// actually does to a surface; this chapter only has to prove that the fragment consumed its own cluster's list.
float3 ClusterLightScale(uint clusterIndex, out uint lightOffset, out uint lightCount)
{
    lightOffset = 0u;
    lightCount = 0u;
    float3 scale = float3(0.0f, 0.0f, 0.0f);
    if (clusterIndex >= ClusterCount)
    {
        return scale;
    }
    ClusterRange range = Clusters[clusterIndex];
    lightOffset = range.lightOffset;
    lightCount = range.lightCount;
    for (uint slot = 0u; slot < range.lightCount; ++slot)
    {
        uint lightIndex = LightIndices[range.lightOffset + slot];
        if (lightIndex >= SceneLightCount)
        {
            continue;
        }
        LightRecord light = Lights[lightIndex];
        scale += float3(light.radianceR, light.radianceG, light.radianceB);
    }
    return scale;
}

// ---------------------------------------------------------------------------------------------------------------
// The analytic opaque scene. It is authored data, not an algorithm: a wall whose depth is an exact dyadic function
// of the normalized coordinate, a near block beside the refracting pane so that a screen-space offset can land on a
// foreground occluder, and a sky band with no opaque surface at all.
// ---------------------------------------------------------------------------------------------------------------

bool InsideNearBlock(uint2 normalized)
{
    return normalized.x >= NearBlockMinU && normalized.x < NearBlockMaxU && normalized.y >= NearBlockMinV &&
           normalized.y < NearBlockMaxV;
}

bool HasOpaqueSurfaceAt(uint2 normalized)
{
    return normalized.y >= SkyBandMaxV;
}

float OpaqueViewDepthAt(uint2 normalized)
{
    if (!HasOpaqueSurfaceAt(normalized))
    {
        return 0.0f;
    }
    if (InsideNearBlock(normalized))
    {
        return NearBlockDepthMetres;
    }
    return OpaqueBaseDepthMetres - (float(normalized.y) * OpaqueDepthSlopeV);
}

uint3 OpaqueAlbedoFixed(uint2 normalized)
{
    uint cu = (normalized.x >> 5u) & 1u;
    uint cv = (normalized.y >> 5u) & 1u;
    uint cd = ((normalized.x >> 5u) + (normalized.y >> 5u)) & 1u;
    return uint3(64u + (128u * cu), 64u + (128u * cv), 64u + (128u * cd));
}

// ---------------------------------------------------------------------------------------------------------------
// The authored transparent panes. The buffer arrives already in the declared per-draw back-to-front order, so a
// variant that walks it in index order is performing exactly the CPU's per-object sort and nothing else.
// ---------------------------------------------------------------------------------------------------------------

bool PaneCovers(PaneRecord pane, uint2 normalized)
{
    return normalized.x >= pane.minU && normalized.x < pane.maxU && normalized.y >= pane.minV &&
           normalized.y < pane.maxV;
}

float PaneViewDepth(PaneRecord pane, uint2 normalized)
{
    return pane.baseDepthMetres + (pane.depthSlopeU * float(normalized.x)) + (pane.depthSlopeV * float(normalized.y));
}

// The pane's authored alpha at this pixel, in units of 1 / FixedOne. Every branch is integer arithmetic, so the
// alpha a coverage decision is taken on is the same integer on the CPU and on the GPU.
uint PaneAlphaFixed(PaneRecord pane, uint2 normalized)
{
    if (pane.alphaModulation == AlphaChecker)
    {
        bool onCell = (((normalized.x >> 4u) + (normalized.y >> 4u)) & 1u) != 0u;
        return onCell ? pane.alphaFixed : (pane.alphaFixed / 4u);
    }
    if (pane.alphaModulation == AlphaHorizontalGradient)
    {
        return (pane.alphaFixed * (normalized.x + 1u)) / NormalizedExtent;
    }
    return pane.alphaFixed;
}

// ---------------------------------------------------------------------------------------------------------------
// The reproducible sample hash, mirrored from ch32::transparency::StochasticSampleHash. It is authored data in the
// same sense the artist mask of Chapter 31 was: a declared, reproducible function, not a design decision, and the
// lab keeps it here so that the learner-owned code owns the coverage *decision* rather than the bit mixing.
//
// The 64-bit arithmetic is written out in pairs of 32-bit words rather than in uint64_t so that the lab does not
// depend on a device supporting 64-bit shader integers, and so that a reader can see that the mix is the contract's
// mix rather than a plausible relative of it.
// ---------------------------------------------------------------------------------------------------------------

uint2 Mul32To64(uint a, uint b)
{
    uint a0 = a & 0xFFFFu;
    uint a1 = a >> 16u;
    uint b0 = b & 0xFFFFu;
    uint b1 = b >> 16u;
    uint p00 = a0 * b0;
    uint p01 = a0 * b1;
    uint p10 = a1 * b0;
    uint p11 = a1 * b1;
    uint mid = (p00 >> 16u) + (p01 & 0xFFFFu) + (p10 & 0xFFFFu);
    uint low = (p00 & 0xFFFFu) | (mid << 16u);
    uint high = p11 + (p01 >> 16u) + (p10 >> 16u) + (mid >> 16u);
    return uint2(low, high);
}

uint2 Mul64(uint2 a, uint2 b)
{
    uint2 low = Mul32To64(a.x, b.x);
    return uint2(low.x, low.y + (a.x * b.y) + (a.y * b.x));
}

uint2 Add64(uint2 a, uint2 b)
{
    uint low = a.x + b.x;
    uint carry = low < a.x ? 1u : 0u;
    return uint2(low, a.y + b.y + carry);
}

uint2 Xor64(uint2 a, uint2 b)
{
    return uint2(a.x ^ b.x, a.y ^ b.y);
}

// Defined for 1 <= shift <= 63 only, which is every shift the mix and the threshold use.
uint2 Shr64(uint2 value, uint shift)
{
    if (shift >= 32u)
    {
        return uint2(value.y >> (shift - 32u), 0u);
    }
    return uint2((value.x >> shift) | (value.y << (32u - shift)), value.y >> shift);
}

bool Greater64(uint2 a, uint2 b)
{
    return a.y != b.y ? a.y > b.y : a.x > b.x;
}

uint2 Mix64(uint2 value)
{
    uint2 mixed = Add64(value, uint2(0x7F4A7C15u, 0x9E3779B9u));
    mixed = Mul64(Xor64(mixed, Shr64(mixed, 30u)), uint2(0x1CE4E5B9u, 0xBF58476Du));
    mixed = Mul64(Xor64(mixed, Shr64(mixed, 27u)), uint2(0x133111EBu, 0x94D049BBu));
    return Xor64(mixed, Shr64(mixed, 31u));
}

uint2 StochasticSampleHash(uint2 pixel, uint sampleIndex, uint frameIndex)
{
    uint2 hash = Mix64(uint2(pixel.x, 0u));
    hash = Mix64(Xor64(hash, Mul64(uint2(pixel.y, 0u), uint2(0x4B3C8A11u, 0xD1195B27u))));
    hash = Mix64(Xor64(hash, Mul64(uint2(sampleIndex, 0u), uint2(0x27D4EB4Fu, 0xC2B2AE3Du))));
    if ((Flags & FlagAnimateStochastic) != 0u)
    {
        hash = Mix64(Xor64(hash, Mul64(uint2(frameIndex, 0u), uint2(0x9E3779F9u, 0x165667B1u))));
    }
    return Mix64(Xor64(hash, uint2(StochasticSeed, 0u)));
}

// The contract accepts a sample when alpha > hash / 2^53, with alpha a double. Every alpha in this lab is an
// integer number of 1 / FixedOne, so alpha * 2^53 is the exact 64-bit integer alphaFixed << 43 and the comparison
// below is provably the same decision as the contract's rather than a float approximation of it. Comparing floats
// here would be a decision that could differ from the reference in the last bit, which is exactly the kind of
// silent CPU/GPU disagreement the chapter is meant to rule out.
bool StochasticSampleAccepted(uint alphaFixed, uint2 pixel, uint sampleIndex, uint frameIndex)
{
    uint2 threshold = Shr64(StochasticSampleHash(pixel, sampleIndex, frameIndex), 11u);
    uint2 scaledAlpha = uint2(0u, alphaFixed << 11u);
    return Greater64(scaledAlpha, threshold);
}

// The float threshold the same hash describes. It is published as evidence and is never the value a decision is
// taken on.
float StochasticSampleThresholdValue(uint2 pixel, uint sampleIndex, uint frameIndex)
{
    uint2 bits = Shr64(StochasticSampleHash(pixel, sampleIndex, frameIndex), 11u);
    return ((float(bits.y) * 4294967296.0f) + float(bits.x)) / 9007199254740992.0f;
}

// ---------------------------------------------------------------------------------------------------------------
// The display path. Chapter 4 taught the encoding and Chapter 30 owns the post chain; what happens here is one
// exposure, one tone curve, one encode, and a set of diagnostic views over the evidence the frame published.
// ---------------------------------------------------------------------------------------------------------------

float3 ToneMap(float3 exposed)
{
    return exposed / (1.0f + exposed);
}

// The channel the net exposure ratio is measured on: the largest-magnitude channel of the radiance the frame held
// immediately before the display exposure, with ties broken towards the lowest index so that every run of the same
// frame measures the same channel. Returns 3 when every channel is zero, which is the one case in which the frame
// carries no evidence of what the exposure did to it.
uint NetExposureChannel(float3 preExposure)
{
    float magnitude = abs(preExposure.r);
    uint channel = 0u;
    if (abs(preExposure.g) > magnitude)
    {
        magnitude = abs(preExposure.g);
        channel = 1u;
    }
    if (abs(preExposure.b) > magnitude)
    {
        magnitude = abs(preExposure.b);
        channel = 2u;
    }
    return magnitude > 0.0f ? channel : 3u;
}

// Publishes the exposure the frame actually applied, by dividing it back out of the frame's own values rather than
// restating the constant that was uploaded. Restating the constant would agree with a pass that exposed twice, with
// one that exposed before the composite, and with one that never exposed at all; a ratio taken from the pixel's own
// pre-exposure and post-exposure radiance disagrees with all three. Where the pre-exposure radiance is zero in
// every channel no ratio exists, and the record says so instead of publishing a number it did not measure.
void PublishNetExposureScale(float3 preExposure, float3 exposed, inout PixelRecord record)
{
    uint channel = NetExposureChannel(preExposure);
    bool derived = channel < 3u;
    uint measured = derived ? channel : 0u;
    float denominator = derived ? preExposure[measured] : 1.0f;
    record.netExposureScaleChannel = measured;
    record.netExposureScaleDerived = derived ? 1u : 0u;
    record.netExposureScale = derived ? (exposed[measured] / denominator) : 0.0f;
}

float EncodeSrgbChannel(float value)
{
    float clamped = saturate(value);
    return clamped <= 0.0031308f ? (clamped * 12.92f) : ((1.055f * pow(clamped, 1.0f / 2.4f)) - 0.055f);
}

float3 EncodeDisplay(float3 displayLinear)
{
    if (TransferFunctionId == TransferSrgb)
    {
        return float3(EncodeSrgbChannel(displayLinear.r), EncodeSrgbChannel(displayLinear.g),
                      EncodeSrgbChannel(displayLinear.b));
    }
    return saturate(displayLinear);
}

// A saturating ramp for scalar diagnostics. It is monotonic and reaches both ends, so two different quantities
// cannot collapse onto the same picture unless they were equal.
float3 Ramp(float value)
{
    float clamped = saturate(value);
    return float3(clamped, clamped * clamped, 1.0f - clamped);
}

float3 CountRamp(uint count, uint maximum)
{
    return Ramp(maximum == 0u ? 0.0f : (float(count) / float(maximum)));
}

// Three bit fields as three channels, so an integer diagnostic is legible and so two different integers cannot
// produce the same pixel unless their low nine bits agree.
float3 BitsToColor(uint value)
{
    return float3(float(value & 7u) / 7.0f, float((value >> 3u) & 7u) / 7.0f, float((value >> 6u) & 7u) / 7.0f);
}

float3 DebugViewColor(PixelRecord record, FrameRecord frame, uint2 pixel)
{
    if (DebugViewId == ViewOpaqueRadiance)
    {
        return EncodeDisplay(ToneMap(float3(record.opaqueR, record.opaqueG, record.opaqueB)));
    }
    if (DebugViewId == ViewOpaqueDepth)
    {
        return record.hasOpaqueSurface == 0u ? float3(0.0f, 0.0f, 0.0f)
                                             : Ramp(record.opaqueViewDepthMetres / OpaqueBaseDepthMetres);
    }
    if (DebugViewId == ViewRefractionSource)
    {
        // What the refraction actually read, which is the copy at the *sampled* texel rather than the copy under the
        // pixel. Where an offset was applied the two are different texels, which is precisely the thing this view
        // exists to show and the reason it is not the same picture as the opaque radiance.
        uint sourceIndex = record.refractionUsed != 0u
                               ? ((record.refractionSampledTexelY * DisplayWidth) + record.refractionSampledTexelX)
                               : ((pixel.y * DisplayWidth) + pixel.x);
        PixelRecord sampled = DisplayRecords[sourceIndex];
        return EncodeDisplay(
            ToneMap(float3(sampled.refractionSourceR, sampled.refractionSourceG, sampled.refractionSourceB)));
    }
    if (DebugViewId == ViewFragmentCount)
    {
        return CountRamp(record.storedFragmentCount, LabFragmentCapacity);
    }
    if (DebugViewId == ViewSortedComposite)
    {
        return EncodeDisplay(ToneMap(float3(record.sortedOverR, record.sortedOverG, record.sortedOverB)));
    }
    if (DebugViewId == ViewObjectSortedComposite)
    {
        return EncodeDisplay(ToneMap(float3(record.objectOverR, record.objectOverG, record.objectOverB)));
    }
    if (DebugViewId == ViewOrderError)
    {
        return Ramp(record.orderChannelError * 8.0f);
    }
    if (DebugViewId == ViewWeightedOitComposite)
    {
        return EncodeDisplay(ToneMap(float3(record.oitOverR, record.oitOverG, record.oitOverB)));
    }
    if (DebugViewId == ViewOitApproximationError)
    {
        return Ramp(record.oitChannelError * 8.0f);
    }
    if (DebugViewId == ViewRevealage)
    {
        return Ramp(record.sortedRevealage);
    }
    if (DebugViewId == ViewTransparentLayerAlpha)
    {
        return Ramp(record.sortedLayerAlpha);
    }
    if (DebugViewId == ViewDepthRejection)
    {
        return CountRamp(record.depthRejectedCount, LabPaneCount);
    }
    if (DebugViewId == ViewCoverageDecision)
    {
        return float3(record.alphaTestPassCount > 0u ? 1.0f : 0.0f, record.hasStochasticFragment != 0u ? 1.0f : 0.0f,
                      record.coverageRejectedCount > 0u ? 1.0f : 0.0f);
    }
    if (DebugViewId == ViewStochasticMask)
    {
        return record.hasStochasticFragment == 0u
                   ? float3(0.0f, 0.0f, 0.0f)
                   : float3(record.stochasticPixelAccepted != 0u ? 1.0f : 0.0f,
                            record.stochasticSampleCount == 0u
                                ? 0.0f
                                : (float(record.stochasticAcceptedCount) / float(record.stochasticSampleCount)),
                            float(record.stochasticMaskLow & 15u) / 15.0f);
    }
    if (DebugViewId == ViewCapacityOverflow)
    {
        return float3(record.overflowed != 0u ? 1.0f : 0.0f, CountRamp(record.droppedFragmentCount, LabPaneCount).x,
                      CountRamp(record.candidateFragmentCount, LabPaneCount).x);
    }
    if (DebugViewId == ViewClusterIndex)
    {
        return record.hasClusterBinding == 0u ? float3(0.0f, 0.0f, 0.0f) : BitsToColor(record.clusterIndex + 1u);
    }
    if (DebugViewId == ViewLightCount)
    {
        return record.hasClusterBinding == 0u ? float3(0.0f, 0.0f, 0.0f)
                                              : float3(CountRamp(record.lightCount, 4u).x,
                                                       float(record.lightOffset & 15u) / 15.0f,
                                                       float(record.fragmentSliceIndex) / float(ClusterSliceCount));
    }
    if (DebugViewId == ViewRefraction)
    {
        return record.refractionUsed == 0u
                   ? float3(0.0f, 0.0f, 0.0f)
                   : float3(saturate(0.5f + (record.refractionAppliedU * 8.0f)),
                            saturate(0.5f + (record.refractionAppliedV * 8.0f)),
                            record.refractionFallback == RefractionFallbackNone
                                ? 0.0f
                                : (record.refractionFallback == RefractionFallbackOutOfBounds ? 0.5f : 1.0f));
    }
    if (DebugViewId == ViewFog)
    {
        return float3(record.fogTransmittance, EncodeDisplay(ToneMap(float3(record.foggedR, 0.0f, 0.0f))).r,
                      FogApplicationId == FogScreenSpaceAfterComposite ? 1.0f : 0.25f);
    }
    if (DebugViewId == ViewReactiveMask)
    {
        return float3(record.reactiveMask, record.reactiveRefractionTerm, record.reactiveStochasticTerm);
    }
    if (DebugViewId == ViewStageOrder)
    {
        return float3(float(record.stageOrderWord & 255u) / 255.0f, float((record.stageOrderWord >> 8u) & 255u) / 255.0f,
                      float(frame.stageCount) / float(StageCount + 1u));
    }
    if (DebugViewId == ViewStatus)
    {
        return float3(float(record.status & 255u) / 255.0f, float((record.status >> 8u) & 255u) / 255.0f,
                      float((record.status >> 16u) & 255u) / 255.0f);
    }
    return float3(record.finalR, record.finalG, record.finalB);
}

// ---------------------------------------------------------------------------------------------------------------
// The one draw in the frame. Everything above it is a dispatch, and the only way the image can exist at all is for
// this draw to have consumed what those dispatches published, which is what makes reading the back buffer proof
// that the draw ran rather than an inference from a compute record.
// ---------------------------------------------------------------------------------------------------------------

struct DisplayVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

DisplayVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    DisplayVertex output;
    output.uv = float2((vertexId << 1u) & 2u, vertexId & 2u);
    output.position = float4((output.uv * float2(2.0f, -2.0f)) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 DisplayPS(DisplayVertex input) : SV_Target
{
    uint2 pixel = uint2(min((uint)input.position.x, DisplayWidth - 1u), min((uint)input.position.y, DisplayHeight - 1u));
    PixelRecord record = DisplayRecords[TexelIndex(pixel)];
    FrameRecord frame = DisplayFrame[0];
    // A record whose marker or status does not match what the frame declared is painted magenta rather than
    // displayed, so a stage that failed to run is visible in the image instead of producing a plausible picture.
    if (record.abiMarker != AbiMarker || (record.status & ExpectedStatus) != ExpectedStatus)
    {
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    }
    return float4(saturate(DebugViewColor(record, frame, pixel)), 1.0f);
}

#endif // LGP_CH32_TRANSPARENCY_SHARED_HLSLI
