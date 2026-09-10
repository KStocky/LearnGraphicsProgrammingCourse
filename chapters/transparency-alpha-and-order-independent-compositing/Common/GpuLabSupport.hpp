#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TransparencyContracts.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Chapter 32 paired GPU lab: transparency written out as an explicit sequence of stages, each of which publishes the
// evidence that distinguishes it from the stage the chapter is comparing it against.
//
// What the two variants do, and why they are structured as a pair:
//
//   * The Starter renders the same analytic scene and composites its transparent surfaces the way almost every real
//     renderer first does: the CPU sorts the transparent draws back to front by a representative depth, uploads that
//     order, and the GPU walks it once per pixel with premultiplied source-over, testing every fragment against the
//     opaque depth. That is an honest, runnable transparency pass, and it is wrong exactly where a per-draw order is
//     wrong. It runs no weighted blended accumulation, takes no coverage decision, reads no refraction source, and
//     publishes no reactive mask, and it does not call a shared helper that quietly does one: the evidence it
//     publishes - an empty accumulator, a zero coverage-rejected count, an absent per-pixel sorted reference and a
//     stage list without the order-independent stages - is what makes that claim checkable rather than a promise.
//   * The Solution adds the rest of the chapter as separate, individually observable stages: a deterministic
//     per-pixel fragment generation with a bounded, counted store; the declared per-pixel back-to-front total order
//     and its source-over reference; the same fragments composited in the Starter's per-draw order so that the cost
//     of the cheaper order is measured rather than asserted; alpha testing and stochastic coverage as coverage
//     decisions rather than blends; a weighted blended order-independent accumulation and resolve with its error
//     against the reference; a clustered light-list consumption seam; a bounded screen-space refraction with a
//     declared fallback; fog in the placement the policy asked for; and a temporal reactive mask.
//
// This header owns the ABI, device setup, configuration validation, the analytic scene table, and the order the
// passes are submitted in. It deliberately owns none of the algorithms: sorting, source-over, the coverage
// decisions, the weighted accumulation and its resolve, the refraction sample, the fog fold and the reactive mask
// all live in the learner-owned HLSL of each variant, because they are the lesson.
//
// Nothing here claims that any technique is correct. The sorted reference is exact for the order it is given, the
// per-draw order is a different order that the frame publishes the error of, the weighted blended path is
// independent of order and wrong by an amount the frame measures, and the bounded store is exact until it overflows
// and then reports what it dropped.

namespace ch32::transparency::gpu
{

// Display bounds. They are small enough that a WARP frame with a per-pixel fragment store is quick, and large
// enough that the analytic panes intersect across many pixels, that a 32x32 cluster tile grid has several tiles in
// each direction, and that a refraction offset of a few texels crosses the opaque scene's checker boundaries.
inline constexpr std::uint32_t kMaximumWidth = 192U;
inline constexpr std::uint32_t kMaximumHeight = 120U;
inline constexpr std::uint32_t kGroupWidth = 8U;
inline constexpr std::uint32_t kGroupHeight = 8U;

// The analytic scene is described in a normalized integer coordinate system rather than in pixels, so a pane covers
// the same part of the picture at every extent and every rectangle test is an integer comparison that the CPU and
// the shader cannot disagree about.
inline constexpr std::uint32_t kNormalizedExtent = 256U;

// Fixed-point unit for every authored colour and alpha in the scene table. Authored values are integers in these
// units, so the float the shader forms and the double the contract forms are the same dyadic rational, and a test
// can demand exact agreement wherever the arithmetic downstream stays inside a float's mantissa.
inline constexpr std::uint32_t kFixedOne = 1024U;

inline constexpr std::uint32_t kLabPaneCount = 13U;
// The per-pixel store the lab owns. It is the contract's own bound, because the point of modelling a k-buffer is
// that overflow is a counted decision rather than whichever fragments arrived first.
inline constexpr std::uint32_t kLabFragmentCapacity = kMaximumStoredFragmentCount;
static_assert(kLabPaneCount <= kMaximumFragmentsPerPixel);

inline constexpr std::uint32_t kClusterTileSize = 32U;
inline constexpr std::uint32_t kClusterSliceCount = 8U;
// Slice s covers view depths [s * kClusterSliceDepthMetres, (s + 1) * kClusterSliceDepthMetres), with everything
// beyond the last boundary landing in the last slice. Chapter 14 owns the slicing policy; this chapter only has to
// use the *fragment's* slice rather than the opaque surface's, which is the bug the seam exists to catch.
inline constexpr double kClusterSliceDepthMetres = 4.0;
inline constexpr std::uint32_t kMaximumClusterTilesX = ((kMaximumWidth + kClusterTileSize - 1U) / kClusterTileSize);
inline constexpr std::uint32_t kMaximumClusterTilesY = ((kMaximumHeight + kClusterTileSize - 1U) / kClusterTileSize);
inline constexpr std::uint32_t kMaximumLabClusterCount =
    kMaximumClusterTilesX * kMaximumClusterTilesY * kClusterSliceCount;
inline constexpr std::uint32_t kLabSceneLightCount = 8U;
// Cluster c declares 1 + (c % 3) lights, so the bound is exact rather than generous.
inline constexpr std::uint32_t kMaximumLabLightIndexCount = kMaximumLabClusterCount * 3U;

inline constexpr std::uint32_t kMaximumStochasticSamples = 32U;
static_assert(kMaximumStochasticSamples <= kMaximumStochasticSampleCount);

inline constexpr std::uint32_t kAbiMarker = 0x5452'3332U;

enum class LabVariant : std::uint8_t
{
    Starter,
    Solution,
};

// Which analytic picture the scene publishes. Every variant exists because some claim about transparency is only
// checkable in it: an exactly reproducible composite, an order failure a per-draw sort cannot avoid, a case the
// weighted blended path is exact in, a case it is wrong in by a computable amount, or a limit.
enum class SceneVariant : std::uint32_t
{
    // Six panes: two interpenetrating sheets of glass, an alpha-tested leaf card, a stochastic foliage card, a
    // refracting pane, and a far tint that the opaque surface rejects over half the frame. The picture the chapter
    // teaches with.
    Showcase = 0U,
    // The two interpenetrating sheets alone. Their depths cross at the middle of the frame, so no single per-draw
    // order is correct at every pixel and the published order error is nonzero on one side and zero on the other.
    IntersectingPair = 1U,
    // One blended pane. Weighted blended OIT is exact for a single fragment, so the resolve must reproduce the
    // sorted reference bit for bit.
    SinglePane = 2U,
    // Three panes that share a colour and differ in alpha and depth. The weighted average of one colour is that
    // colour, so the technique is exact here too, and for a completely different reason.
    SharedColorStack = 3U,
    // A saturated red pane in front of a saturated blue one, both at alpha one half. The two techniques agree on the
    // layer alpha exactly and disagree on the colour by exactly one eighth in two channels, which is a number a test
    // can state rather than measure.
    KnownApproximation = 4U,
    // One pane with alpha exactly one. Revealage is exactly zero, the background contributes nothing, and both
    // composites must reproduce the pane's own colour.
    AllTransparentOpaque = 5U,
    // A stochastic card and an alpha-tested card carrying the same requested alpha at almost the same depth. The
    // alpha test resolves the same way in every frame and destroys the gradation; the stochastic card keeps the
    // expected coverage and pays for it in noise.
    CoverageContrast = 6U,
    // The refracting pane alone, over an opaque scene that contains a near block beside it and a frame edge beyond
    // it, so both declared fallbacks are reachable.
    RefractionField = 7U,
    // Every pane at once, so a bounded store with a small capacity has more fragments than it can keep.
    OverflowStack = 8U,
    // No transparent surfaces at all. The composite must be the opaque radiance bit for bit and revealage exactly
    // one.
    Empty = 9U,
};

inline constexpr std::uint32_t kSceneVariantCount = static_cast<std::uint32_t>(SceneVariant::Empty) + 1U;

// Which composite the frame publishes as its result. All three are always computed, because the chapter is about
// their differences; this only selects which one reaches the image.
enum class CompositeMode : std::uint32_t
{
    // The declared per-pixel back-to-front order, composited with source-over. Exact for the order it is given.
    SortedReference = 0U,
    // The same fragments composited in the order the CPU sorted the draws into. It is what a real renderer can
    // afford and it is wrong wherever the per-draw order is wrong.
    ObjectSorted = 1U,
    // Weighted blended order-independent compositing. Independent of order and wrong by a bounded amount.
    WeightedOit = 2U,
};

// The order the accumulation stage visits a pixel's stored fragments in. Weighted blended accumulation is a sum and
// a product, so every one of these must produce the same counters and the same revealage exactly, and the same
// weighted sums to within floating-point rounding. Changing this knob is how the technique's order independence is
// measured instead of asserted.
enum class OitTraversal : std::uint32_t
{
    Stored = 0U,
    Reversed = 1U,
    NearToFar = 2U,
    EvenThenOdd = 3U,
    // Two independent partial accumulators merged, which is what independent GPU threads writing the same pixel
    // through additive and multiplicative blending actually do.
    SplitMerge = 4U,
};

enum class TransferFunction : std::uint32_t
{
    Linear = 0U,
    Srgb = 1U,
};

enum class DebugView : std::uint32_t
{
    Final = 0U,
    OpaqueRadiance = 1U,
    OpaqueDepth = 2U,
    RefractionSource = 3U,
    FragmentCount = 4U,
    SortedComposite = 5U,
    ObjectSortedComposite = 6U,
    OrderError = 7U,
    WeightedOitComposite = 8U,
    OitApproximationError = 9U,
    Revealage = 10U,
    TransparentLayerAlpha = 11U,
    DepthRejection = 12U,
    CoverageDecision = 13U,
    StochasticMask = 14U,
    CapacityOverflow = 15U,
    ClusterIndex = 16U,
    LightCount = 17U,
    Refraction = 18U,
    Fog = 19U,
    ReactiveMask = 20U,
    StageOrder = 21U,
    Status = 22U,
};

inline constexpr std::uint32_t kDebugViewCount = static_cast<std::uint32_t>(DebugView::Status) + 1U;

// The passes the renderer submits, in submission order. The word the shader stamps into every pixel is built from
// this list, so a test reads the order that actually ran rather than the order this comment claims.
enum class LabStage : std::uint32_t
{
    Clear = 0U,
    // The opaque G-buffer and its lighting, which is where the depth every transparent fragment is tested against
    // comes from.
    Opaque = 1U,
    // The copy of the lit opaque scene that screen-space refraction reads. It exists as its own stage because the
    // whole limitation of the technique is that this copy is taken before any transparency was composited.
    RefractionSource = 2U,
    // Deterministic per-pixel transparent fragment generation, the coverage decisions, the cluster seam, the
    // refraction sample, the per-fragment fog fold, and the bounded store.
    Fragments = 3U,
    // Source-over over the stored fragments: the declared per-pixel order in the Solution, the CPU's per-draw order
    // in both.
    ForwardComposite = 4U,
    OitAccumulate = 5U,
    OitResolve = 6U,
    // The composite the frame publishes, screen-space fog if the policy asked for it, the reactive mask, the single
    // display exposure, the tone curve and the display encoding.
    Compose = 7U,
};

// Eight stages at four bits each is exactly one 32-bit word, which is why the stage list is capped here rather than
// left to grow.
inline constexpr std::uint32_t kMaximumLabStageCount = 8U;

// Per-pixel status bits, mirrored by TransparencyShared.hlsli.
inline constexpr std::uint32_t kStatusOpaque = 1U << 0U;
inline constexpr std::uint32_t kStatusRefractionSource = 1U << 1U;
inline constexpr std::uint32_t kStatusFragments = 1U << 2U;
// The pixel's fragments were placed in the declared back-to-front total order. The Starter never sets it, because
// the only order it has is the one the CPU handed it.
inline constexpr std::uint32_t kStatusPerPixelSorted = 1U << 3U;
inline constexpr std::uint32_t kStatusObjectOrdered = 1U << 4U;
inline constexpr std::uint32_t kStatusForwardComposite = 1U << 5U;
inline constexpr std::uint32_t kStatusOitAccumulated = 1U << 6U;
inline constexpr std::uint32_t kStatusOitResolved = 1U << 7U;
inline constexpr std::uint32_t kStatusFogPerFragment = 1U << 8U;
inline constexpr std::uint32_t kStatusFogScreenSpace = 1U << 9U;
inline constexpr std::uint32_t kStatusReactiveMask = 1U << 10U;
inline constexpr std::uint32_t kStatusExposure = 1U << 11U;
inline constexpr std::uint32_t kStatusToneMap = 1U << 12U;
inline constexpr std::uint32_t kStatusDisplayEncode = 1U << 13U;
// The variant declares that every fragment it produces is a blend, so it takes no coverage decision at all. It is
// published rather than inferred, so a Starter that quietly grew an alpha test would stop matching its own claim.
inline constexpr std::uint32_t kStatusBlendOnly = 1U << 14U;
// Per-pixel facts rather than frame-wide ones. They are excluded from ExpectedStatus for that reason.
inline constexpr std::uint32_t kStatusCoverageDecided = 1U << 15U;
inline constexpr std::uint32_t kStatusCapacityOverflow = 1U << 16U;
inline constexpr std::uint32_t kStatusRefractionApplied = 1U << 17U;
inline constexpr std::uint32_t kStatusRefractionFallback = 1U << 18U;
inline constexpr std::uint32_t kStatusClusterConsumed = 1U << 19U;
inline constexpr std::uint32_t kStatusDepthRejected = 1U << 20U;

// The bits every pixel of a frame must carry. The rest are per-pixel facts.
inline constexpr std::uint32_t kFrameWideStatusMask =
    kStatusOpaque | kStatusRefractionSource | kStatusFragments | kStatusPerPixelSorted | kStatusObjectOrdered |
    kStatusForwardComposite | kStatusOitAccumulated | kStatusOitResolved | kStatusFogPerFragment |
    kStatusFogScreenSpace | kStatusReactiveMask | kStatusExposure | kStatusToneMap | kStatusDisplayEncode |
    kStatusBlendOnly;

// Frame-level overflow flags. Nothing is clamped: a frame that exceeded a bounded accumulator says so, and the
// readback is refused rather than quietly describing a composite no dispatch produced.
inline constexpr std::uint32_t kOverflowFragmentCapacity = 1U << 0U;
inline constexpr std::uint32_t kOverflowWeightedAccumulation = 1U << 1U;
inline constexpr std::uint32_t kOverflowCandidateCount = 1U << 2U;

// Per-fragment flag bits.
inline constexpr std::uint32_t kFragmentStored = 1U << 0U;
inline constexpr std::uint32_t kFragmentDepthRejected = 1U << 1U;
inline constexpr std::uint32_t kFragmentCoverageRejected = 1U << 2U;
inline constexpr std::uint32_t kFragmentDropped = 1U << 3U;
inline constexpr std::uint32_t kFragmentRefracted = 1U << 4U;
inline constexpr std::uint32_t kFragmentRefractionFallback = 1U << 5U;
inline constexpr std::uint32_t kFragmentFogged = 1U << 6U;
inline constexpr std::uint32_t kFragmentWritesDepth = 1U << 7U;
inline constexpr std::uint32_t kFragmentTiedWithPrevious = 1U << 8U;
inline constexpr std::uint32_t kFragmentAlphaTested = 1U << 9U;
inline constexpr std::uint32_t kFragmentStochastic = 1U << 10U;

// Pane flag bits, authored in the scene table.
inline constexpr std::uint32_t kPaneRefracts = 1U << 0U;
inline constexpr std::uint32_t kPaneFogged = 1U << 1U;

// How a pane's authored alpha varies over its own rectangle. Both variants are integer functions of the normalized
// coordinate, so the modulated alpha is still an exact multiple of 1 / kFixedOne and the stochastic and alpha-test
// decisions taken on it stay exactly comparable between the CPU and the shader.
enum class AlphaModulation : std::uint32_t
{
    Constant = 0U,
    // A sixteen-unit checker that drops the alpha to a quarter, which is what makes an alpha test's hard silhouette
    // visible.
    Checker = 1U,
    HorizontalGradient = 2U,
};

struct LabConfiguration final
{
    DebugView debugView{DebugView::Final};
    SceneVariant sceneVariant{SceneVariant::Showcase};
    CompositeMode compositeMode{CompositeMode::SortedReference};
    OitTraversal oitTraversal{OitTraversal::Stored};
    OitWeightFunction weightFunction{OitWeightFunction::InverseDepthPolynomial};
    FragmentOverflowPolicy overflowPolicy{FragmentOverflowPolicy::KeepNearest};
    FogApplication fogApplication{FogApplication::PerFragmentBeforeComposite};
    DepthWritePolicy depthWrite{DepthWritePolicy::CoverageDecided};
    TransferFunction transferFunction{TransferFunction::Srgb};

    std::uint32_t fragmentCapacity{kLabFragmentCapacity};
    std::uint32_t frameIndex{};
    std::uint32_t stochasticSampleCount{4U};
    std::uint32_t stochasticSeed{0x9E37'79B9U};
    std::uint32_t sceneLightCount{kLabSceneLightCount};

    float alphaTestThreshold{0.5F};
    float uniformOitWeight{1.0F};
    float oitNearScaleMetres{5.0F};
    float oitFarScaleMetres{200.0F};
    // The declared clamp on the evaluated weight. A caller may narrow the technique's published range and never
    // widen it, so the defaults are exact powers of two safely inside kMinimumOitWeight and kMaximumOitWeight rather
    // than float copies of those bounds, which would round to the wrong side of them.
    float oitMinimumWeight{0.015625F};
    float oitMaximumWeight{2048.0F};
    float oitAlphaEpsilon{1.0e-5F};

    float fogDensityPerMetre{0.0F};
    float fogInscatterR{0.35F};
    float fogInscatterG{0.40F};
    float fogInscatterB{0.50F};

    float refractionMaximumOffsetUv{0.05F};
    // The screen-space displacement the refracting pane asks for, declared in whole texels so that the sampled UV
    // always lands on a texel centre and the texel a test asserts is the texel the sample actually read.
    std::int32_t refractionOffsetTexelsX{6};
    std::int32_t refractionOffsetTexelsY{-3};

    float reactiveAlphaWeight{1.0F};
    float reactiveRefractionWeight{1.0F};
    float reactiveStochasticWeight{1.0F};
    float reactiveReferenceOffsetUv{0.01F};
    float reactiveMaximumMask{static_cast<float>(kFullyReactiveMask)};

    // The exposure the display applies, exactly once, after the composite. This chapter owns neither its value nor
    // its derivation; it only owns the invariant that the transparent layer is exposed identically to the scene
    // behind it, which is why the scale is a declared constant here.
    float displayExposureScale{1.0F};

    bool animateStochasticWithFrameIndex{true};
    bool enableRefraction{true};
    bool enableReactiveMask{true};
    bool rejectForegroundOccluders{true};
    bool testAgainstOpaqueDepth{true};
    bool wroteTransparentMotionVector{false};

    [[nodiscard]] bool operator==(LabConfiguration const &) const noexcept = default;
};

// One authored transparent surface. The table is CPU-owned and uploaded every frame already sorted into the
// declared per-draw back-to-front order, so the Starter can walk the buffer in index order and perform no sort of
// any kind, and the Solution's per-draw baseline walks the same order without having to reconstruct it.
struct PaneRecord final
{
    float baseDepthMetres{};
    // Metres of view depth per normalized unit. Every authored slope is a negative power of two, so the depth is an
    // exact dyadic rational at every pixel and the sorted composite of a small stack is exact in a float.
    float depthSlopeU{};
    float depthSlopeV{};
    float paddingDepth{};

    std::uint32_t minU{};
    std::uint32_t maxU{};
    std::uint32_t minV{};
    std::uint32_t maxV{};

    std::uint32_t colorFixedR{};
    std::uint32_t colorFixedG{};
    std::uint32_t colorFixedB{};
    std::uint32_t alphaFixed{};

    std::uint32_t alphaModulation{};
    std::uint32_t coverageMode{};
    std::uint32_t drawOrder{};
    std::uint32_t primitiveId{};

    std::uint32_t paneIndex{};
    std::uint32_t flags{};
    std::uint32_t paddingA{};
    std::uint32_t paddingB{};

    [[nodiscard]] bool operator==(PaneRecord const &) const noexcept = default;
};
static_assert(sizeof(PaneRecord) == 80U);
static_assert(alignof(PaneRecord) == 4U);

struct LightRecord final
{
    float radianceR{};
    float radianceG{};
    float radianceB{};
    float padding{};

    [[nodiscard]] bool operator==(LightRecord const &) const noexcept = default;
};
static_assert(sizeof(LightRecord) == 16U);

// One cluster's slice of the light index buffer. Chapter 14 builds these; this chapter only consumes them, and the
// only thing it has to get right is that a transparent fragment reads the cluster of its own depth slice.
struct ClusterRange final
{
    std::uint32_t lightOffset{};
    std::uint32_t lightCount{};

    [[nodiscard]] bool operator==(ClusterRange const &) const noexcept = default;
};
static_assert(sizeof(ClusterRange) == 8U);

// One stored transparent fragment, as the dispatch published it. Every float precedes every integer so a finiteness
// sweep can read the whole float block at once.
struct FragmentRecord final
{
    float straightR{};
    float straightG{};
    float straightB{};
    // Premultiplied: the straight colour multiplied by the resolved alpha, exactly once. Publishing both is what
    // lets a test prove the single multiply rather than take it on trust.
    float premultipliedR{};
    float premultipliedG{};
    float premultipliedB{};
    float alpha{};
    // The alpha the material asked for, before any coverage decision resolved it to present or absent.
    float requestedAlpha{};
    float viewDepthMetres{};
    float oitWeight{};
    float fogTransmittance{};
    float refractionOffsetLengthUv{};
    float revealageBefore{};
    float revealageAfter{};
    float transmittanceInFront{};
    float lightScale{};

    std::uint32_t paneIndex{};
    std::uint32_t drawOrder{};
    std::uint32_t primitiveId{};
    std::uint32_t coverageMode{};
    std::uint32_t outcome{};
    std::uint32_t sortedPosition{};
    std::uint32_t objectPosition{};
    std::uint32_t flags{};
    std::uint32_t clusterIndex{};
    std::uint32_t sliceIndex{};
    std::uint32_t lightOffset{};
    std::uint32_t lightCount{};
    std::uint32_t acceptedSampleMaskLow{};
    std::uint32_t acceptedSampleMaskHigh{};
    std::uint32_t acceptedSampleCount{};
    std::uint32_t sampleCount{};

    [[nodiscard]] bool operator==(FragmentRecord const &) const noexcept = default;
};
static_assert(sizeof(FragmentRecord) == 128U);
static_assert(alignof(FragmentRecord) == 4U);
static_assert(offsetof(FragmentRecord, paneIndex) == 64U);

// One display pixel of byte-inspectable evidence.
struct PixelRecord final
{
    float opaqueR{};
    float opaqueG{};
    float opaqueB{};
    float opaqueViewDepthMetres{};
    float refractionSourceR{};
    float refractionSourceG{};
    float refractionSourceB{};
    float paddingSource{};

    float sortedLayerR{};
    float sortedLayerG{};
    float sortedLayerB{};
    float sortedLayerAlpha{};
    float sortedOverR{};
    float sortedOverG{};
    float sortedOverB{};
    float sortedRevealage{};

    float objectLayerR{};
    float objectLayerG{};
    float objectLayerB{};
    float objectLayerAlpha{};
    float objectOverR{};
    float objectOverG{};
    float objectOverB{};
    float objectRevealage{};

    float weightedColorSumR{};
    float weightedColorSumG{};
    float weightedColorSumB{};
    float weightedAlphaSum{};
    float oitAverageR{};
    float oitAverageG{};
    float oitAverageB{};
    float oitRevealage{};
    float oitLayerR{};
    float oitLayerG{};
    float oitLayerB{};
    float oitLayerAlpha{};
    float oitOverR{};
    float oitOverG{};
    float oitOverB{};
    float paddingOit{};

    // The published approximation evidence. Nothing here claims the weighted blended path is exact; these are the
    // numbers that say by how much it is not.
    float oitChannelError{};
    float oitLuminanceError{};
    float oitAlphaError{};
    float orderChannelError{};

    float compositeR{};
    float compositeG{};
    float compositeB{};
    float fogTransmittance{};
    float foggedR{};
    float foggedG{};
    float foggedB{};
    float paddingFog{};

    float refractionSurfaceU{};
    float refractionSurfaceV{};
    float refractionRequestedU{};
    float refractionRequestedV{};
    float refractionAppliedU{};
    float refractionAppliedV{};
    float refractionSampledU{};
    float refractionSampledV{};
    float refractionRequestedLengthUv{};
    float refractionAppliedLengthUv{};
    float writtenViewDepthMetres{};
    float paddingRefraction{};

    float reactiveMask{};
    float reactiveAlphaTerm{};
    float reactiveRefractionTerm{};
    float reactiveStochasticTerm{};

    float exposedR{};
    float exposedG{};
    float exposedB{};
    // The measured net multiply from the radiance this pixel actually held immediately before the display exposure
    // - the fogged composite, whichever fog placement produced it - to the exposed one, taken on a single colour
    // channel: exposed[c] / fogged[c]. It is divided out of the frame's own values rather than copied from the
    // constant that was uploaded, so a second exposure anywhere between the fog fold and the tone curve changes it.
    // The channel is chosen deterministically as the largest-magnitude channel of the pre-exposure radiance, with
    // ties broken towards the lowest index, and is published in netExposureScaleChannel. A pixel whose
    // pre-exposure radiance is zero in every channel has no derivable ratio: there the field is zero and
    // netExposureScaleDerived is zero, because reporting the configured scale there would be an assumption rather
    // than a measurement.
    float netExposureScale{};

    float finalR{};
    float finalG{};
    float finalB{};
    float paddingFinal{};

    std::uint32_t hasOpaqueSurface{};
    std::uint32_t candidateFragmentCount{};
    std::uint32_t storedFragmentCount{};
    std::uint32_t droppedFragmentCount{};
    std::uint32_t compositedCount{};
    std::uint32_t depthRejectedCount{};
    std::uint32_t coverageRejectedCount{};
    std::uint32_t fullyTransparentCount{};
    std::uint32_t depthTieCount{};
    std::uint32_t depthWriteCount{};
    std::uint32_t alphaTestPassCount{};
    std::uint32_t alphaTestFailCount{};
    std::uint32_t stochasticSampleCount{};
    std::uint32_t stochasticAcceptedCount{};
    std::uint32_t stochasticMaskLow{};
    std::uint32_t stochasticMaskHigh{};
    std::uint32_t stochasticPixelAccepted{};
    std::uint32_t hasStochasticFragment{};
    std::uint32_t overflowed{};
    std::uint32_t refractionUsed{};
    std::uint32_t refractionFallback{};
    std::uint32_t refractionClamped{};
    std::uint32_t refractionSampledTexelX{};
    std::uint32_t refractionSampledTexelY{};
    // The texel the unclamped, unrejected offset pointed at. It is published beside the sampled texel precisely so
    // that a fallback can be replayed through the contract: on a fallback the sampled texel is the surface's own,
    // and the candidate is the only record of what the offset asked for.
    std::uint32_t refractionCandidateTexelX{};
    std::uint32_t refractionCandidateTexelY{};
    std::uint32_t refractionCandidateInBounds{};
    std::uint32_t tileX{};
    std::uint32_t tileY{};
    std::uint32_t fragmentSliceIndex{};
    std::uint32_t opaqueSliceIndex{};
    std::uint32_t clusterIndex{};
    std::uint32_t opaqueClusterIndex{};
    std::uint32_t clusterAmbiguous{};
    std::uint32_t lightOffset{};
    std::uint32_t lightCount{};
    std::uint32_t hasClusterBinding{};
    std::uint32_t oitFragmentCount{};
    std::uint32_t oitUsedAlphaEpsilon{};
    std::uint32_t oitFullyRevealed{};
    std::uint32_t oitFullyOccluded{};
    std::uint32_t depthWritten{};
    std::uint32_t exposureApplicationCount{};
    // One where netExposureScale is a ratio this pixel's own radiance supports, zero where the pre-exposure
    // radiance was zero in every channel and no ratio exists to publish.
    std::uint32_t netExposureScaleDerived{};
    // The channel index (0, 1, 2) the ratio was taken on.
    std::uint32_t netExposureScaleChannel{};
    std::uint32_t compositeSource{};
    std::uint32_t stageOrderWord{};
    std::uint32_t status{};
    std::uint32_t abiMarker{};

    [[nodiscard]] bool operator==(PixelRecord const &) const noexcept = default;
};
static_assert(sizeof(PixelRecord) % 4U == 0U);
static_assert(alignof(PixelRecord) == 4U);
static_assert(offsetof(PixelRecord, hasOpaqueSurface) % 4U == 0U);
inline constexpr std::size_t kPixelRecordFloatCount = offsetof(PixelRecord, hasOpaqueSurface) / sizeof(float);

// Frame-level evidence, accumulated on the GPU with integer atomics so that the totals are a property of the
// dispatch rather than of the order its groups happened to finish in.
struct FrameRecord final
{
    std::uint32_t abiMarker{};
    std::uint32_t variant{};
    std::uint32_t stageOrderWord{};
    std::uint32_t stageCount{};
    std::uint32_t activePaneCount{};
    std::uint32_t clusterTileCountX{};
    std::uint32_t clusterTileCountY{};
    std::uint32_t clusterSliceCount{};
    std::uint32_t clusterCount{};
    std::uint32_t sceneLightCount{};
    std::uint32_t lightIndexCount{};
    std::uint32_t overflowFlags{};

    std::uint32_t candidateFragmentCount{};
    std::uint32_t storedFragmentCount{};
    std::uint32_t droppedFragmentCount{};
    std::uint32_t compositedFragmentCount{};
    std::uint32_t depthRejectedFragmentCount{};
    std::uint32_t coverageRejectedFragmentCount{};
    std::uint32_t fullyTransparentFragmentCount{};
    std::uint32_t depthTieCount{};
    std::uint32_t depthWriteCount{};
    std::uint32_t alphaTestPassCount{};
    std::uint32_t alphaTestFailCount{};
    std::uint32_t stochasticAcceptedPixelCount{};
    std::uint32_t stochasticPixelCount{};
    std::uint32_t overflowPixelCount{};
    std::uint32_t refractionPixelCount{};
    std::uint32_t refractionFallbackPixelCount{};
    std::uint32_t clusterPixelCount{};
    std::uint32_t ambiguousClusterPixelCount{};
    std::uint32_t maximumStoredFragmentCount{};
    std::uint32_t compositeSource{};

    // Maxima are accumulated as the bit patterns of non-negative floats, which order identically to the floats
    // themselves, so InterlockedMax gives a deterministic answer without a floating-point atomic.
    std::uint32_t maximumOitChannelErrorBits{};
    std::uint32_t maximumOitAlphaErrorBits{};
    std::uint32_t maximumOrderChannelErrorBits{};
    std::uint32_t maximumReactiveMaskBits{};
    std::uint32_t maximumWeightedAlphaSumBits{};
    std::uint32_t paddingA{};
    std::uint32_t paddingB{};
    std::uint32_t paddingC{};

    [[nodiscard]] bool operator==(FrameRecord const &) const noexcept = default;
};
static_assert(sizeof(FrameRecord) == 160U);

struct FrameReadback final
{
    LabConfiguration configuration{};
    LabVariant variant{};
    lgp::framework::Extent2D displaySize{};
    std::vector<PixelRecord> pixels{};
    // kLabFragmentCapacity entries per pixel, in stored order.
    std::vector<FragmentRecord> fragments{};
    FrameRecord frame{};

    // The CPU-side mirrors of the settings the dispatch received, in the contract's own types. Tests replay the
    // GPU's published fragments through these rather than transcribing the numbers a second time.
    std::vector<PaneRecord> panes{};
    std::vector<LightRecord> lights{};
    std::vector<std::uint32_t> lightIndices{};
    std::vector<ClusterRange> clusters{};
    ClusterGridDescription clusterGrid{};
    OitWeightSettings weightSettings{};
    WeightedOitResolveSettings resolveSettings{};
    AlphaTestSettings alphaTestSettings{};
    StochasticCoverageSettings stochasticSettings{};
    FogSettings fogSettings{};
    RefractionSettings refractionSettings{};
    ReactiveMaskSettings reactiveSettings{};
    BoundedFragmentSettings boundedSettings{};
    SortedCompositionSettings sortedSettings{};
    TransparencyPipelinePolicy pipelinePolicy{};
    TransparencyOrderPlan orderPlan{};
    std::array<FrameStage, kMaximumStageCount> frameStages{};
    std::uint32_t frameStageCount{};
    std::array<LabStage, kMaximumLabStageCount> executedStages{};
    std::uint32_t executedStageCount{};
    std::uint32_t stageOrderWord{};
    std::uint32_t expectedStatus{};
    std::uint32_t activePaneCount{};
    std::uint32_t frameSlot{};
    // The identity of the staging buffer this frame's scene copy read from, and of the mapping the CPU wrote the
    // scene tables through. They are published so a test can prove the upload a frame used belongs to the frame
    // slot that submitted it rather than to a resource every in-flight frame shares. Both are stable for the
    // resource's lifetime and distinct between live resources, so they identify the buffer without owning it.
    std::uint64_t sceneUploadGpuAddress{};
    std::uint64_t sceneUploadCpuAddress{};
};

struct BufferBarrierState final
{
    D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};
    D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};
};

[[nodiscard]] lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration,
                                                              LabVariant variant);
// The configuration a variant starts from. The Starter owns no order-independent path, no coverage decision, no
// refraction and no reactive mask, so its defaults switch all of them off and its validation refuses to switch any
// of them on.
[[nodiscard]] LabConfiguration DefaultConfiguration(LabVariant variant) noexcept;

// The contract-typed settings the renderer derives from a configuration. Tests reuse them so a contract replay is
// driven by the same numbers the dispatch received rather than by a second transcription of them.
[[nodiscard]] OitWeightSettings MakeOitWeightSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] WeightedOitResolveSettings MakeResolveSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] AlphaTestSettings MakeAlphaTestSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] StochasticCoverageSettings MakeStochasticSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] FogSettings MakeFogSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] RefractionSettings MakeRefractionSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] ReactiveMaskSettings MakeReactiveSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] BoundedFragmentSettings MakeBoundedSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] SortedCompositionSettings MakeSortedSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] TransparencyPipelinePolicy MakePipelinePolicy(LabConfiguration const &configuration,
                                                            LabVariant variant) noexcept;

[[nodiscard]] ClusterGridDescription MakeClusterGrid(lgp::framework::Extent2D extent) noexcept;
// The declared depth slicing. Chapter 14 owns the policy; the only thing this chapter has to do is apply it to the
// *fragment's* depth rather than to the opaque surface's.
[[nodiscard]] std::uint32_t SliceIndexForDepth(double viewDepthMetres) noexcept;
[[nodiscard]] std::vector<LightRecord> BuildLights(LabConfiguration const &configuration);
// The cluster light lists, exactly as Chapter 14 would publish them: a flat index buffer plus a range per cluster.
[[nodiscard]] std::vector<std::uint32_t> BuildLightIndices(ClusterGridDescription const &grid,
                                                           std::uint32_t sceneLightCount);
[[nodiscard]] std::vector<ClusterRange> BuildClusterRanges(ClusterGridDescription const &grid,
                                                           std::uint32_t sceneLightCount);

// Which panes a scene variant declares, as a bit per pane index.
[[nodiscard]] std::uint32_t ScenePaneMask(SceneVariant scene) noexcept;
// The authored scene table, unordered.
[[nodiscard]] std::span<PaneRecord const> PaneTable() noexcept;
// The active panes for a scene, sorted into the declared per-draw back-to-front order: descending representative
// view depth, then ascending draw order. The buffer the shader reads is this order, so the Starter walks it without
// sorting anything and the Solution's per-draw baseline is the same order rather than a reconstruction of it.
[[nodiscard]] std::vector<PaneRecord> BuildActivePanes(LabConfiguration const &configuration);

// The stage sequence the renderer submits, in submission order, with the stages the variant does not own left out.
[[nodiscard]] std::uint32_t BuildExecutedStages(LabVariant variant, std::span<LabStage> stages) noexcept;
// Four bits per submitted stage, holding the stage enumerator plus one so an unused nibble reads as zero.
[[nodiscard]] std::uint32_t EncodeStageOrder(std::span<LabStage const> stages) noexcept;
// The frame stages the lab's submitted passes map onto, in the contract's own vocabulary, so that the order the
// frame actually runs can be checked by PlanTransparencyOrder rather than described in a comment.
[[nodiscard]] std::uint32_t BuildFrameStages(LabConfiguration const &configuration, LabVariant variant,
                                             std::span<FrameStage> stages) noexcept;
[[nodiscard]] std::uint32_t ExpectedStatus(LabConfiguration const &configuration, LabVariant variant) noexcept;

class BufferResource final
{
  public:
    BufferResource() = default;
    BufferResource(BufferResource &&other) noexcept;
    BufferResource &operator=(BufferResource &&other) noexcept;
    BufferResource(BufferResource const &) = delete;
    BufferResource &operator=(BufferResource const &) = delete;
    ~BufferResource();

    [[nodiscard]] ID3D12Resource *Get() const noexcept
    {
        return resource_.Get();
    }
    [[nodiscard]] std::uint64_t size_in_bytes() const noexcept
    {
        return sizeInBytes_;
    }
    [[nodiscard]] std::byte const *mapped_data() const noexcept
    {
        return mappedData_;
    }
    [[nodiscard]] std::byte *mapped_data() noexcept
    {
        return mappedData_;
    }

  private:
    friend std::expected<BufferResource, lgp::framework::Error> CreateBuffer(ID3D12Device10 &, std::uint64_t,
                                                                             D3D12_HEAP_TYPE, D3D12_RESOURCE_FLAGS,
                                                                             std::wstring_view, bool);
    void Reset() noexcept;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::uint64_t sizeInBytes_{};
    std::byte *mappedData_{};
};

[[nodiscard]] std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
    ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
    std::wstring_view name, bool mapPersistently = false);

class RendererCore : public lgp::framework::IChapterRenderer
{
  public:
    RendererCore(std::filesystem::path shaderPath, LabVariant variant);
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void ConfigureHeadlessTest(LabConfiguration const &configuration) noexcept;
    [[nodiscard]] std::expected<FrameReadback, lgp::framework::Error> ReadBackOutputs();

  private:
    // Everything the frame produces is frame-slot-owned, because two frames in flight must not share a readback
    // staging buffer - and, for the same reason, must not share the upload buffer the CPU writes the scene tables
    // into. The scene tables themselves are sequence-owned and reused every frame: they describe the authored scene
    // rather than one frame's work, and every reuse passes through a named barrier so a persistent resource cannot
    // be read in one state and written in another.
    struct FrameSlotResources final
    {
        BufferResource records{};
        BufferResource fragments{};
        BufferResource frame{};
        BufferResource refractionSource{};
        BufferResource recordsReadback{};
        BufferResource fragmentsReadback{};
        BufferResource frameReadback{};
        // The staging the frame's scene copy reads. It is written by the CPU between this slot's fence wait in
        // BeginFrame and the submission of the frame that follows it, so the only GPU work that can be reading it
        // is work this slot's fence has already reported complete.
        BufferResource sceneUpload{};
        lgp::framework::DescriptorAllocation descriptors{};
        bool initialized{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateResources(lgp::framework::Extent2D size);
    void DestroyResources(lgp::framework::DeviceResources &deviceResources) noexcept;
    [[nodiscard]] LabConfiguration ActiveConfiguration() const noexcept;

    std::filesystem::path shaderPath_{};
    LabVariant variant_{};
    bool headless_{};
    bool hasRendered_{};
    lgp::framework::DeviceResources *deviceResources_{};

    lgp::framework::CompiledShader clearShader_{};
    lgp::framework::CompiledShader opaqueShader_{};
    lgp::framework::CompiledShader refractionSourceShader_{};
    lgp::framework::CompiledShader fragmentsShader_{};
    lgp::framework::CompiledShader forwardShader_{};
    lgp::framework::CompiledShader oitAccumulateShader_{};
    lgp::framework::CompiledShader oitResolveShader_{};
    lgp::framework::CompiledShader composeShader_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};

    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> clearPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> opaquePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> refractionSourcePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> fragmentsPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> forwardPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> oitAccumulatePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> oitResolvePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> composePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};

    std::vector<FrameSlotResources> frameSlots_{};

    // Sequence-owned scene tables. They are uploaded through a per-frame-slot staging copy every frame rather than
    // mapped directly, so the shader always reads a default-heap resource whose every state change is a declared
    // barrier.
    BufferResource panes_{};
    BufferResource lights_{};
    BufferResource lightIndices_{};
    BufferResource clusters_{};
    bool sceneInitialized_{};

    lgp::framework::Extent2D size_{};
    std::optional<LabConfiguration> headlessConfiguration_{};
    LabConfiguration interactiveConfiguration_{};
    LabConfiguration lastRenderedConfiguration_{};

    std::vector<PaneRecord> lastPanes_{};
    std::vector<LightRecord> lastLights_{};
    std::vector<std::uint32_t> lastLightIndices_{};
    std::vector<ClusterRange> lastClusters_{};
    ClusterGridDescription lastClusterGrid_{};
    TransparencyOrderPlan lastOrderPlan_{};
    std::array<FrameStage, kMaximumStageCount> lastFrameStages_{};
    std::uint32_t lastFrameStageCount_{};
    std::array<LabStage, kMaximumLabStageCount> lastStages_{};
    std::uint32_t lastStageCount_{};
    std::uint32_t lastStageOrderWord_{};
    std::uint32_t lastExpectedStatus_{};
    std::uint32_t lastActivePaneCount_{};
    std::uint32_t lastRenderedFrameSlot_{};
    std::uint64_t lastSceneUploadGpuAddress_{};
    std::uint64_t lastSceneUploadCpuAddress_{};
};

} // namespace ch32::transparency::gpu
