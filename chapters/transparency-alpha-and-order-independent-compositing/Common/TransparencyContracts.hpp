#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

// Chapter 32 teaching contracts for transparency, alpha, and order-independent compositing.
//
// Scope and honesty rules for this phase:
//
//   * Everything here is a deterministic CPU reference model of the decisions a transparency pass normally hides
//     inside a blend state, a sort, and one screen-space texture read: which quantity the word "alpha" refers to,
//     how a straight colour becomes a premultiplied one, what source-over actually computes, how a coverage
//     decision differs from a blend, what a sorted back-to-front pass guarantees and what it cannot, what a
//     weighted blended order-independent pass approximates, and where fog, refraction, and the temporal reactive
//     mask sit relative to the composite.
//   * There is no universally correct transparency technique. Sorted forward compositing is exact for the fragment
//     order it is given and wrong whenever that order is wrong; weighted blended OIT is independent of order and
//     wrong by a bounded, measurable amount everywhere; a bounded per-pixel fragment list is exact until it
//     overflows and then silently loses fragments unless it counts them. The chapter implements all three, makes
//     each publish the evidence that distinguishes it from the others, and refuses to nominate a winner.
//   * Coverage, opacity, transmission, and blend alpha are four different quantities that the literature routinely
//     spells "alpha". They are four different C++ types here, none of which converts to another, so a stack cannot
//     quietly multiply a coverage by a transmission and call the product an opacity.
//   * Straight and premultiplied colour are likewise different types. Only premultiplied colours compose, and the
//     only way to obtain one is Premultiply, which multiplies by alpha exactly once. Nothing downstream multiplies
//     by alpha again, which is the whole content of the alpha-squared bug that makes edges look bitten out.
//   * Alpha testing and stochastic coverage are coverage decisions, not blending. Both answer "is this sample
//     present at all"; neither carries the partial transmission that blending carries. Saying that a stochastic
//     alpha test "does transparency" is a category error, and the results here keep the two vocabularies apart.
//   * Approximations declare themselves. Weighted blended OIT publishes the error against the sorted reference for
//     the same fragments, a bounded fragment list publishes what it dropped, refraction publishes when it fell
//     back, and screen-space fog after the composite publishes that it fogged the transparent layer with the
//     opaque scene's depth.
//
// Units, spaces, and sign conventions:
//
//   * Colour is scene-linear Rec.709 RGB in the renderer's radiometric unit, already multiplied by the frame
//     pre-exposure. This chapter never applies or removes an exposure; Chapter 30 owns that, and Chapter 31 owns
//     the value it applies. The channel domain mirrors Chapter 30's compositing seam.
//   * Depth is view-space depth in metres, strictly positive, increasing away from the camera. Back-to-front means
//     descending view depth: the farthest fragment is composited first. Device depth and the forward/reversed
//     conventions that produce it belong to Chapter 12; expressing every ordering and rejection decision here in
//     one monotonically increasing quantity is what stops the sort and the depth test from disagreeing about which
//     direction "in front" points.
//   * Alpha is coverage times opacity, in [0, 1], and is the fraction of the light arriving from behind that the
//     fragment replaces. Transmission is 1 - alpha. Revealage is the product of the transmissions of every
//     composited fragment, so it is how much of the background survives, and 1 - revealage is how much of the
//     pixel the transparent layer owns.
//   * Screen-space UVs are in [0, 1] with (0, 0) at the top-left texel edge, matching the D3D12 convention the lab
//     will use.
//
// Deliberately out of scope, and not approximated here: BRDF evaluation and light accumulation, which Chapters 5
// and 14 own; clustered light-list construction, which Chapter 14 owns and which this chapter only consumes;
// per-primitive or per-object CPU sorting policy, which is a scene-management problem rather than a compositing
// one; depth peeling and per-pixel linked-list construction on the GPU, which the lab phase owns if it earns the
// bandwidth; physically based transmission through a participating medium, dispersion, total internal reflection,
// and Fresnel-coupled refraction, none of which a screen-space offset can express; and the temporal resolve that
// consumes the reactive mask, which Chapter 28 owns.

namespace ch32::transparency
{

// ---------------------------------------------------------------------------------------------------------------
// Bounds. Every limit exists so that a malformed input is rejected instead of producing a plausible pixel.
// ---------------------------------------------------------------------------------------------------------------

// The integration seam. Chapter 30 rejects any scene-linear channel above 1e6, so a wider domain here would let
// this chapter publish a colour the compositing stage refuses. The value is mirrored rather than included, because
// the two chapters are separate teaching units with no build dependency between them, and it is pinned by a test so
// that the two cannot drift apart silently.
inline constexpr double kMaximumSceneLinearValue = 1.0e6;

inline constexpr double kMaximumViewDepthMetres = 1.0e6;
inline constexpr std::uint32_t kMaximumRenderDimension = 16'384U;

// One pixel's worth of transparent fragments in the reference path. The limit is small because the reference is a
// teaching model of one pixel, not a scene-wide allocator, and because every diagnostic array below is sized by it.
inline constexpr std::uint32_t kMaximumFragmentsPerPixel = 32U;

// The capacity a bounded per-pixel fragment store may declare. A real k-buffer or per-pixel linked list is bounded
// by memory in exactly this way; the point of modelling it is that overflow is a counted, deterministic decision
// rather than whichever fragments happened to arrive first.
inline constexpr std::uint32_t kMaximumStoredFragmentCount = 8U;

inline constexpr std::uint32_t kMaximumStochasticSampleCount = 64U;

// Weighted blended OIT weight limits. They are McGuire and Bavoil's published bounds, kept because the weight
// function is only meaningful as a bounded function: an unbounded weight makes one fragment win the average
// outright and turns the technique into an expensive, order-independent way of drawing the nearest fragment. Both
// bounds are floors and ceilings on the *settings* as well as on the evaluated weight, so a caller cannot widen
// the technique's declared range by declaring a smaller minimum or a larger maximum of its own.
inline constexpr double kMinimumOitWeight = 1.0e-2;
inline constexpr double kMaximumOitWeight = 3.0e3;
inline constexpr double kMaximumOitDepthScaleMetres = 1.0e5;
// The largest value any weighted accumulator may reach. kMaximumFragmentsPerPixel fragments at the largest legal
// channel and the largest legal weight is 32 * 1e6 * 3e3 = 9.6e10, so the limit leaves an order of magnitude of
// headroom and still sits far below the point where a double loses integer resolution.
inline constexpr double kMaximumWeightedAccumulation = 1.0e12;
inline constexpr double kMinimumOitAlphaEpsilon = 1.0e-12;
inline constexpr double kMaximumOitAlphaEpsilon = 1.0e-1;
// The guard on the accumulated-alpha divide. It is a floor on the divisor, not a clamp on the result: when the
// accumulated alpha is smaller than this the transparent layer is empty enough that its colour cannot matter, and
// the revealage that multiplies it is one to within the same epsilon.
inline constexpr double kDefaultOitAlphaEpsilon = 1.0e-5;

inline constexpr double kMaximumFogDensityPerMetre = 1.0;
inline constexpr double kMaximumRefractionOffsetUv = 0.25;

inline constexpr std::uint32_t kMaximumStageCount = 8U;
inline constexpr std::uint32_t kMaximumClusterCount = 1'048'576U;
inline constexpr std::uint32_t kMaximumLightIndexCount = 4'194'304U;
inline constexpr std::uint32_t kMaximumSceneLightCount = 65'536U;

// A reactive mask of one is the value Chapter 28's default history validation refuses history at, and zero is the
// value that leaves the temporal resolve entirely alone. Nothing here decides what a resolve does with the number
// in between; this chapter only measures the risk.
inline constexpr double kFullyReactiveMask = 1.0;

// A premultiplied colour is valid when every channel lies in [0, alpha * kMaximumSceneLinearValue]. The bound is
// evaluated with a small relative slack because source-over, fog, and the weighted resolve all round, and a channel
// that is one ulp over the exact product is a rounding artefact rather than a broken colour. The slack is far
// smaller than any error a real bug produces: premultiplying twice, the classic failure, is wrong by a factor of
// alpha.
inline constexpr double kPremultipliedBoundSlack = 1.0 + 1.0e-12;

// The relative tolerance below which two composites are called equal. It is a rounding budget, not a quality
// target: the sorted reference and the weighted blended resolve accumulate the same quantities in different orders,
// and a difference of this size means the two agree as exactly as the format allows. Any real approximation error
// is orders of magnitude larger.
inline constexpr double kCompositeExactnessTolerance = 1.0e-12;

enum class ContractError : std::uint8_t
{
    NonFinite = 0U,
    NegativeRadiance,
    RadianceTooLarge,
    CoverageOutOfRange,
    OpacityOutOfRange,
    AlphaOutOfRange,
    TransmissionOutOfRange,
    ReactiveMaskOutOfRange,
    // A premultiplied channel exceeds alpha times the legal straight-colour domain, so the colour cannot be the
    // product of a legal straight colour and its own alpha. Premultiplying an already premultiplied colour is the
    // usual cause, and so is an additive fragment: a colour with zero alpha and non-zero radiance is a different
    // blend equation, not a source-over colour, and it is refused here rather than silently composited.
    NotPremultiplied,
    InvalidThreshold,
    InvalidSampleCount,
    InvalidSampleIndex,
    InvalidPixelCoordinate,
    InvalidViewDepth,
    InvalidFragmentCount,
    // A fragment declared a coverage decision, AlphaTest or StochasticCoverage, and still carries a fractional
    // premultiplied alpha. The two are contradictory: a coverage decision resolves to present or absent, so its
    // fragment is either the source-over identity or a fully opaque sample, and nothing in between ever reaches a
    // blend unit. Accepting the contradiction is what would let a half-transparent fragment blend *and* write
    // depth, which is neither a blend nor a coverage decision.
    FractionalCoverageAlpha,
    InvalidCapacity,
    CounterOverflow,
    InvalidWeightSettings,
    AccumulationOverflow,
    InvalidAlphaEpsilon,
    InvalidUv,
    InvalidExtent,
    InvalidOffsetLimit,
    InvalidFogSettings,
    InvalidStage,
    TooManyStages,
    InvalidClusterGrid,
    InvalidTileCoordinate,
    InvalidSliceIndex,
    // The declared cluster index is the one the *opaque* surface at this pixel would use, and the transparent
    // fragment is in a different slice. Reusing the opaque cluster is the classic transparent-lighting bug: it
    // lights a window with the light list of whatever is behind it.
    ClusterFromOpaqueDepth,
    ClusterIndexMismatch,
    MalformedLightRange,
    InvalidLightIndex,
    InvalidLightCount,
    InvalidReactiveSettings,
};

struct Rgb final
{
    double r{};
    double g{};
    double b{};

    [[nodiscard]] bool operator==(Rgb const &) const noexcept = default;
};

struct Float2 final
{
    double x{};
    double y{};

    [[nodiscard]] bool operator==(Float2 const &) const noexcept = default;
};

struct Extent2D final
{
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] bool operator==(Extent2D const &) const noexcept = default;
};

struct PixelCoordinate final
{
    std::uint32_t x{};
    std::uint32_t y{};

    [[nodiscard]] bool operator==(PixelCoordinate const &) const noexcept = default;
};

// ---------------------------------------------------------------------------------------------------------------
// A. The four quantities the word "alpha" is used for
// ---------------------------------------------------------------------------------------------------------------

// The fraction of the pixel's area the fragment geometrically covers. A leaf cut out of a quad has a coverage of
// about a half at its silhouette and a coverage of one in its interior, whatever the leaf's material does to light.
struct Coverage final
{
    double value{};

    [[nodiscard]] bool operator==(Coverage const &) const noexcept = default;
};

// The fraction of the light arriving from behind that the material stops *where it is covered*. Clear glass has an
// opacity near zero over its whole coverage; the leaf has an opacity of one over its partial coverage.
struct Opacity final
{
    double value{};

    [[nodiscard]] bool operator==(Opacity const &) const noexcept = default;
};

// The fraction of the light arriving from behind that survives the fragment: 1 - alpha. It is the quantity that
// multiplies, which is why revealage is a product of transmissions and not a sum of alphas.
struct Transmission final
{
    double value{};

    [[nodiscard]] bool operator==(Transmission const &) const noexcept = default;
};

// Coverage times opacity: the single number a blend unit consumes. Two fragments with the same blend alpha are
// indistinguishable to source-over even when one is a half-covered opaque leaf and the other is fully covered
// half-transparent glass. That collapse is the approximation every alpha-blended renderer makes, and naming it
// separately from its two factors is what makes the approximation visible.
struct BlendAlpha final
{
    double value{};

    [[nodiscard]] bool operator==(BlendAlpha const &) const noexcept = default;
};

// The risk, in [0, 1], that a temporal resolve's history is invalid at this pixel because of what transparency did
// to it. It is an output of this chapter and an input to Chapter 28; it is not a decision about feedback.
struct ReactiveMask final
{
    double value{};

    [[nodiscard]] bool operator==(ReactiveMask const &) const noexcept = default;
};

[[nodiscard]] std::expected<Coverage, ContractError> MakeCoverage(double value) noexcept;
[[nodiscard]] std::expected<Opacity, ContractError> MakeOpacity(double value) noexcept;
[[nodiscard]] std::expected<Transmission, ContractError> MakeTransmission(double value) noexcept;
[[nodiscard]] std::expected<BlendAlpha, ContractError> MakeBlendAlpha(double value) noexcept;
[[nodiscard]] std::expected<ReactiveMask, ContractError> MakeReactiveMask(double value) noexcept;

// alpha = coverage * opacity. This is the only sanctioned way to build a blend alpha from the two factors, and the
// product is exact for the dyadic values a test uses.
[[nodiscard]] std::expected<BlendAlpha, ContractError> BlendAlphaFromCoverage(Coverage coverage,
                                                                              Opacity opacity) noexcept;
[[nodiscard]] std::expected<Transmission, ContractError> TransmissionFromAlpha(BlendAlpha alpha) noexcept;
[[nodiscard]] std::expected<BlendAlpha, ContractError> AlphaFromTransmission(Transmission transmission) noexcept;

// Rec.709 luminance of a scene-linear colour, in the colour's own radiometric unit. It exists so that error
// evidence can be reported as one number without pretending that one number is the whole error.
[[nodiscard]] std::expected<double, ContractError> SceneLinearLuminance(Rgb color) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// B. Straight and premultiplied colour
// ---------------------------------------------------------------------------------------------------------------

// The colour the material shades to, independent of how much of the pixel it covers. Straight colours are what a
// texture stores and what an artist authors; they do not compose, because a weighted average of straight colours
// weights the invisible parts of a texel as heavily as the visible parts.
struct StraightColor final
{
    Rgb rgb{};
    BlendAlpha alpha{};

    [[nodiscard]] bool operator==(StraightColor const &) const noexcept = default;
};

// The colour the fragment actually contributes: straight colour multiplied by alpha, exactly once. Premultiplied
// colours compose, filter, and add without any further reference to alpha, which is why every blend equation below
// consumes them and why nothing below ever multiplies by alpha a second time.
struct PremultipliedColor final
{
    Rgb rgb{};
    BlendAlpha alpha{};

    [[nodiscard]] bool operator==(PremultipliedColor const &) const noexcept = default;
};

[[nodiscard]] constexpr PremultipliedColor TransparentBlack() noexcept
{
    return {.rgb = {}, .alpha = {}};
}

[[nodiscard]] std::expected<void, ContractError> ValidateStraightColor(StraightColor const &color) noexcept;

// Rejects a premultiplied colour that no legal straight colour could have produced. The error precedence is
// NonFinite, then NegativeRadiance, then AlphaOutOfRange, then RadianceTooLarge for a channel outside the absolute
// domain, then NotPremultiplied for a channel that is inside the absolute domain but above alpha times its limit.
[[nodiscard]] std::expected<void, ContractError> ValidatePremultipliedColor(PremultipliedColor const &color) noexcept;

// The one multiplication by alpha in the whole chapter.
[[nodiscard]] std::expected<PremultipliedColor, ContractError> Premultiply(StraightColor const &color) noexcept;

// The inverse, for the cases that genuinely need a straight colour again, such as writing an authored texture back
// out. Alpha of exactly zero has no straight colour to recover, and the only premultiplied colour with zero alpha
// that this chapter accepts is transparent black, which comes back as a straight black; anything else was already
// refused as NotPremultiplied, so there is no division that can produce an infinity here.
[[nodiscard]] std::expected<StraightColor, ContractError> Unpremultiply(PremultipliedColor const &color) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// C. Porter-Duff source-over
// ---------------------------------------------------------------------------------------------------------------

// Source over destination, in premultiplied form:
//
//     rgb   = source.rgb   + (1 - source.alpha) * destination.rgb
//     alpha = source.alpha + (1 - source.alpha) * destination.alpha
//
// Three properties follow from the equations alone, and all three are exact in floating point rather than
// approximate:
//
//   * Transparent identity. A source with alpha zero and a black premultiplied colour leaves the destination bit
//     for bit unchanged, because the source term is zero and the destination weight is exactly one.
//   * Opaque replacement. A source with alpha one reproduces the source bit for bit, because the destination
//     weight is exactly zero.
//   * No second alpha multiply. The source contributes source.rgb, not source.alpha * source.rgb. Writing the
//     latter is the alpha-squared bug: it darkens every partially covered fragment by its own alpha and is most
//     visible exactly where coverage is fractional, which is every silhouette in the frame.
//
// Associativity holds in exact arithmetic: (a over b) over c and a over (b over c) are the same composite, which is
// what makes an incremental back-to-front loop equal to a single expression over the whole ordered list. In
// floating point the two differ by rounding, and the tests pin the exact case with dyadic values rather than
// claiming an equality the format does not provide.
//
// Commutativity does *not* hold. a over b is not b over a unless one of them is transparent or the pair happens to
// be degenerate, and that asymmetry is the entire reason transparency needs an order at all.
[[nodiscard]] std::expected<PremultipliedColor, ContractError> SourceOver(
    PremultipliedColor const &source, PremultipliedColor const &destination) noexcept;

// Composites a premultiplied layer over opaque background radiance. The background carries no alpha because it is
// opaque by construction, so the result is radiance rather than a premultiplied colour.
[[nodiscard]] std::expected<Rgb, ContractError> CompositeOverOpaqueBackground(PremultipliedColor const &layer,
                                                                              Rgb background) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// D. Coverage decisions: alpha testing and stochastic coverage
// ---------------------------------------------------------------------------------------------------------------

// How a fragment's alpha is turned into presence. AlphaBlend keeps the partial transmission and needs an order;
// AlphaTest and StochasticCoverage throw the partial transmission away and produce present-or-absent samples that
// need no order at all, which is why both can be drawn in the opaque pass with depth writes on.
//
// The mode is a claim about the fragment's alpha, not a label attached to it afterwards. A fragment that declares
// AlphaTest or StochasticCoverage must already have had its coverage decision resolved, so its premultiplied alpha
// is exactly zero, meaning the sample is absent, or exactly one, meaning the sample is present and opaque. Any
// value in between is refused as FractionalCoverageAlpha wherever a TransparentFragment is validated, because a
// fragment that both blends partially and occludes what is behind it describes no blend equation any hardware
// implements.
enum class CoverageMode : std::uint8_t
{
    AlphaBlend = 0U,
    AlphaTest,
    StochasticCoverage,
};

struct AlphaTestSettings final
{
    // A fragment is kept when alpha >= threshold. The comparison is inclusive so that a threshold of zero keeps
    // every fragment including a fully transparent one, and a threshold of one keeps only fully opaque fragments.
    double threshold{0.5};

    [[nodiscard]] bool operator==(AlphaTestSettings const &) const noexcept = default;
};

struct AlphaTestResult final
{
    bool passed{};
    double alpha{};
    double threshold{};
    // One or zero. An alpha test does not blend, so a surviving fragment is fully present and a rejected one is
    // absent; the fragment's own alpha never reaches a blend unit.
    BlendAlpha resolvedAlpha{};

    [[nodiscard]] bool operator==(AlphaTestResult const &) const noexcept = default;
};

// Alpha testing is a hard threshold, so it is stable frame to frame and free of order dependence, and it destroys
// every gradation the texture's alpha channel contained. A leaf card looks identical under any camera motion and
// has a hard, aliased silhouette; no amount of threshold tuning recovers the missing partial coverage.
[[nodiscard]] std::expected<AlphaTestResult, ContractError> EvaluateAlphaTest(
    BlendAlpha alpha, AlphaTestSettings const &settings) noexcept;

// The identity of one coverage sample. Every stochastic decision in this chapter is a pure function of this
// identity, the seed, and the sample count, so a CPU reference, a WARP dispatch, and a real GPU wave agree exactly
// without sharing any state.
struct SampleIdentity final
{
    std::uint32_t pixelX{};
    std::uint32_t pixelY{};
    std::uint32_t sampleIndex{};
    std::uint32_t frameIndex{};

    [[nodiscard]] bool operator==(SampleIdentity const &) const noexcept = default;
};

struct StochasticCoverageSettings final
{
    // Samples per pixel. The decision is quantized to 1 / sampleCount, so four samples can only ever express
    // coverages of 0, 0.25, 0.5, 0.75, and 1, and the difference between the requested alpha and the achieved
    // fraction is reported rather than hidden.
    std::uint32_t sampleCount{4U};
    std::uint32_t seed{0x9E37'79B9U};
    // Whether the frame index participates in the hash. Animating the pattern decorrelates the error over time so
    // that a temporal resolve can average it away, at the cost of making every frame differ; freezing it produces a
    // stable but permanently patterned image and is the right choice when there is no temporal resolve to feed.
    bool animateWithFrameIndex{true};

    [[nodiscard]] bool operator==(StochasticCoverageSettings const &) const noexcept = default;
};

struct StochasticCoverageResult final
{
    std::uint32_t sampleCount{};
    std::uint32_t acceptedSampleCount{};
    // Bit i is set when sample i was accepted. It is published so that a test can assert *which* samples were
    // chosen rather than only how many, which is the difference between a stable hash and a plausible one.
    std::uint64_t acceptedSampleMask{};
    double requestedAlpha{};
    // acceptedSampleCount / sampleCount. This is a coverage, not an alpha: every accepted sample is fully opaque
    // and the fraction is what an MSAA resolve will average, so calling this "the fragment's alpha" would confuse
    // a resolve-time average with a blend-time transmission.
    Coverage achievedCoverage{};
    double quantizationStep{};
    // achievedCoverage - requestedAlpha. Its sign matters: a positive value means this pixel is more covered than
    // its alpha asked for, and neighbouring pixels with the opposite sign are what make the average come out right.
    double coverageError{};
    bool fullyAccepted{};
    bool fullyRejected{};

    [[nodiscard]] bool operator==(StochasticCoverageResult const &) const noexcept = default;
};

// A stable per-sample hash. Any 64-bit avalanche mix would do; what matters is that it is a pure function of the
// identity, that it is reproducible in HLSL, and that it does not depend on the order samples are visited in.
[[nodiscard]] std::expected<std::uint64_t, ContractError> StochasticSampleHash(
    SampleIdentity identity, StochasticCoverageSettings const &settings) noexcept;

// The comparison threshold for one sample, in [0, 1). The half-open range is deliberate: a fragment with alpha
// exactly one must accept every sample, and a fragment with alpha exactly zero must accept none, so the rule is
// accept when alpha > threshold.
[[nodiscard]] std::expected<double, ContractError> StochasticSampleThreshold(
    SampleIdentity identity, StochasticCoverageSettings const &settings) noexcept;

// Stochastic coverage is not alpha blending. It converts a transmission into a set of present-or-absent opaque
// samples whose *expected* fraction equals the requested alpha; it does not composite anything, it needs no
// ordering, and it produces noise instead of the smooth gradient a blend would. Two stochastic surfaces behind one
// another resolve correctly through the depth buffer, which is the property sorted blending cannot offer, and they
// pay for it with variance that only a temporal or spatial filter can remove.
[[nodiscard]] std::expected<StochasticCoverageResult, ContractError> EvaluateStochasticCoverage(
    BlendAlpha alpha, PixelCoordinate pixel, std::uint32_t frameIndex,
    StochasticCoverageSettings const &settings) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// E. Fragments, ordering, and the opaque depth test
// ---------------------------------------------------------------------------------------------------------------

// The opaque scene's depth at one pixel, as the transparent pass reads it. The pass tests against this value and,
// under the policies below, may or may not write to it.
struct OpaqueDepthState final
{
    bool hasSurface{};
    double viewDepthMetres{};

    [[nodiscard]] bool operator==(OpaqueDepthState const &) const noexcept = default;
};

// Whether a transparent pass writes depth. Never is the ordinary choice for blended transparency: writing depth
// would let one transparent surface reject another, which is not what "transparent" means. CoverageDecided is the
// choice for alpha-tested and stochastic fragments, which are opaque at every surviving sample and therefore both
// may and should occlude what is behind them.
enum class DepthWritePolicy : std::uint8_t
{
    Never = 0U,
    CoverageDecided,
};

struct TransparentFragment final
{
    // Shaded, premultiplied, scene-linear, pre-exposed radiance for this fragment at this pixel.
    PremultipliedColor color{};
    double viewDepthMetres{};
    // Submission index, used as the declared tie-break when two fragments share a depth. Two coplanar fragments
    // have no geometric order, so the order must come from somewhere; taking it from submission makes the result
    // reproducible instead of dependent on rasterization order.
    std::uint32_t drawOrder{};
    std::uint32_t primitiveId{};
    // How the fragment's alpha became presence. Under AlphaTest and StochasticCoverage the premultiplied alpha
    // must be exactly zero or exactly one; see CoverageMode for why, and FractionalCoverageAlpha for what happens
    // when it is not.
    CoverageMode mode{CoverageMode::AlphaBlend};

    [[nodiscard]] bool operator==(TransparentFragment const &) const noexcept = default;
};

// The declared total order for one pixel's fragments, back to front: descending view depth, then ascending draw
// order, then ascending input index. The last key guarantees a total order even when two fragments agree on both of
// the first two, so the result never depends on the sort algorithm's stability.
struct FragmentOrder final
{
    std::array<std::uint32_t, kMaximumFragmentsPerPixel> indices{};
    std::uint32_t count{};
    // Adjacent pairs in the sorted order that share a view depth exactly. A nonzero count means the image depends
    // on the declared tie-break rather than on geometry.
    std::uint32_t depthTieCount{};
    // True when every fragment has a distinct depth, so the order is a geometric fact rather than a convention.
    bool strictlyOrdered{};

    [[nodiscard]] bool operator==(FragmentOrder const &) const noexcept = default;
};

// Sorting is per pixel and per fragment, which is the strongest thing a sort can be, and it is still not enough in
// general:
//
//   * Interpenetrating surfaces produce one fragment each at a pixel, and the fragment depths there are whatever
//     the rasterizer sampled. The correct composite would need the two surfaces split at their intersection; a
//     total order over the samples cannot express that the front surface is behind the back one over part of the
//     pixel's area.
//   * Sorting per object or per draw, which is what a real renderer can afford, is not a total order at all. Three
//     objects can overlap cyclically so that no single draw order is correct at every pixel, and even two objects
//     can require opposite orders at two different pixels. The reference below composites a *declared* per-pixel
//     order so that the cost of getting that order wrong is measurable rather than theoretical.
[[nodiscard]] std::expected<FragmentOrder, ContractError> SortFragmentsBackToFront(
    std::span<TransparentFragment const> fragments) noexcept;

// A fragment is in front of the opaque surface when its view depth is strictly smaller. Equal depths are rejected,
// matching a LESS depth test: a transparent fragment coplanar with the opaque surface it is drawn against is
// z-fighting, and accepting it would make the image depend on rasterizer precision.
[[nodiscard]] std::expected<bool, ContractError> PassesOpaqueDepthTest(double fragmentViewDepthMetres,
                                                                       OpaqueDepthState opaque) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// F. Bounded per-pixel fragment storage
// ---------------------------------------------------------------------------------------------------------------

// Which fragments a full store keeps. Neither policy is safe: KeepNearest keeps the fragments that contribute most
// and loses the far ones that a thick stack of glass needs, and KeepFirstSubmitted keeps the fragments the scene
// submitted first, by ascending drawOrder with the input index as the tie-break, and is not a function of the
// geometry at all. Both are deterministic, and both count what they dropped, which is the only property that makes
// an overflowing store debuggable.
enum class FragmentOverflowPolicy : std::uint8_t
{
    KeepNearest = 0U,
    KeepFirstSubmitted,
};

struct BoundedFragmentSettings final
{
    std::uint32_t capacity{kMaximumStoredFragmentCount};
    FragmentOverflowPolicy policy{FragmentOverflowPolicy::KeepNearest};

    [[nodiscard]] bool operator==(BoundedFragmentSettings const &) const noexcept = default;
};

// The kept fragments, already in the declared back-to-front order so that the list composites directly. Under
// KeepFirstSubmitted the kept *set* is the fragments with the smallest drawOrder, ties broken by ascending input
// index, and the kept set is then sorted, because a store that chose or returned its fragments in array order
// would make the result depend on the order the caller happened to gather them in rather than on the submission
// identity drawOrder declares.
struct BoundedFragmentList final
{
    std::array<TransparentFragment, kMaximumStoredFragmentCount> fragments{};
    std::uint32_t count{};
    std::uint32_t capacity{};
    std::uint32_t attemptedCount{};
    std::uint32_t droppedCount{};
    bool overflowed{};

    [[nodiscard]] bool operator==(BoundedFragmentList const &) const noexcept = default;
};

[[nodiscard]] std::expected<BoundedFragmentList, ContractError> CollectBoundedFragments(
    std::span<TransparentFragment const> fragments, BoundedFragmentSettings const &settings) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// G. Sorted forward reference composition
// ---------------------------------------------------------------------------------------------------------------

enum class FragmentOutcome : std::uint8_t
{
    Composited = 0U,
    RejectedByOpaqueDepth,
    // Alpha zero and a black premultiplied colour: the fragment is the source-over identity, so it is recorded as
    // seen and excluded from the composited count rather than counted as a contribution that did nothing.
    RejectedAsFullyTransparent,
};

struct FragmentEvidence final
{
    std::uint32_t inputIndex{};
    std::uint32_t sortedPosition{};
    FragmentOutcome outcome{FragmentOutcome::Composited};
    double viewDepthMetres{};
    double alpha{};
    // Revealage before and after this fragment was composited. Equal values mean the fragment changed nothing.
    double revealageBefore{};
    double revealageAfter{};
    // The product of the transmissions of every composited fragment nearer than this one, which is the fraction of
    // this fragment's contribution that survives to the final pixel. A fragment with a large alpha and a tiny
    // survival fraction is invisible, and that is exactly the fragment a bounded store should have dropped.
    double transmittanceInFront{};
    bool writesDepth{};
    bool tiedWithPrevious{};

    [[nodiscard]] bool operator==(FragmentEvidence const &) const noexcept = default;
};

struct CompositionStatistics final
{
    std::uint64_t inputFragmentCount{};
    std::uint64_t compositedFragmentCount{};
    std::uint64_t depthRejectedFragmentCount{};
    std::uint64_t fullyTransparentFragmentCount{};
    std::uint64_t droppedFragmentCount{};
    std::uint64_t depthTieCount{};
    std::uint64_t depthWriteCount{};

    [[nodiscard]] bool operator==(CompositionStatistics const &) const noexcept = default;
};

// Adds two pixels' or two tiles' statistics. Every field is checked for wraparound rather than allowed to carry,
// because a wrapped counter reports plausible progress and is indistinguishable from a correct one.
[[nodiscard]] std::expected<CompositionStatistics, ContractError> AccumulateCompositionStatistics(
    CompositionStatistics const &left, CompositionStatistics const &right) noexcept;

struct SortedCompositionSettings final
{
    DepthWritePolicy depthWrite{DepthWritePolicy::Never};
    bool testAgainstOpaqueDepth{true};

    [[nodiscard]] bool operator==(SortedCompositionSettings const &) const noexcept = default;
};

struct SortedCompositionInput final
{
    std::span<TransparentFragment const> fragments{};
    OpaqueDepthState opaque{};
    // The lit opaque radiance already at this pixel, scene-linear and pre-exposed.
    Rgb backgroundRadiance{};
    // Fragments a bounded store refused before this call. The composite cannot see them, so it publishes the count
    // and reports itself incomplete rather than presenting a partial result as a reference.
    std::uint32_t droppedFragmentCount{};
};

struct SortedCompositionResult final
{
    // The transparent layer composited over transparent black, so it can be handed to a later composite unchanged.
    PremultipliedColor layer{};
    Rgb overBackground{};
    // The product of the transmissions of the composited fragments. It equals 1 - layer.alpha in exact arithmetic;
    // both are published because their difference is a direct measure of the accumulated rounding, and because the
    // weighted blended path computes revealage the same way and can be compared against this one field for field.
    double revealage{};
    FragmentOrder order{};
    std::array<FragmentEvidence, kMaximumFragmentsPerPixel> fragments{};
    std::uint32_t fragmentCount{};
    CompositionStatistics statistics{};
    bool depthWritten{};
    double writtenViewDepthMetres{};
    // False when a bounded store dropped fragments, so no consumer can mistake a truncated composite for the
    // reference.
    bool complete{};

    [[nodiscard]] bool operator==(SortedCompositionResult const &) const noexcept = default;
};

// The reference composite. Fragments are sorted into the declared back-to-front order, tested against the opaque
// depth, and composited with source-over in that order; every fragment gets a record whether it was composited or
// not, so a diagnostic can say why a surface is missing rather than only that it is.
//
// Depth writes are applied after the composite and describe what a *later* pass sees, not what this composite sees.
// Under CoverageDecided the nearest surviving alpha-tested or stochastic fragment supplies the new depth, because
// those fragments are opaque wherever they survive; blended fragments never write, so the pass leaves the depth
// buffer exactly as it found it. A coverage-decided fragment can only be composited with an alpha of exactly one,
// so the fragment that writes depth is always the one that also replaced everything behind it, and the depth write
// and the composite can never disagree about whether the surface is opaque.
//
// The error precedence is fixed and observable: fragment-count limits, then per-fragment validity in input order,
// and within one fragment its premultiplied colour, then its view depth, then FractionalCoverageAlpha when a
// coverage-decided fragment carries an alpha that is neither exactly zero nor exactly one; then the opaque depth
// state, then the background radiance.
[[nodiscard]] std::expected<SortedCompositionResult, ContractError> CompositeSortedFragments(
    SortedCompositionInput const &input, SortedCompositionSettings const &settings) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// H. Weighted blended order-independent compositing
// ---------------------------------------------------------------------------------------------------------------

// The declared weight function. Uniform gives every fragment the same say and is the honest baseline: it shows
// that the technique's order independence comes from the accumulation, not from the weighting. InverseDepthPolynomial
// is McGuire and Bavoil's bounded depth weight, which biases the average toward near fragments so that a near
// surface is not washed out by the far ones behind it.
//
//     base(z) = 10 / (1e-5 + (z / nearScale)^2 + (z / farScale)^6)
//     w(z)    = clamp(base(z), minimumWeight, maximumWeight)
//
// The weight is a function of depth only. Alpha enters the accumulation explicitly, once, through the premultiplied
// colour and the accumulated alpha, so a reader can see that the weight is a *confidence*, not a second opacity.
enum class OitWeightFunction : std::uint8_t
{
    Uniform = 0U,
    InverseDepthPolynomial,
};

struct OitWeightSettings final
{
    OitWeightFunction function{OitWeightFunction::InverseDepthPolynomial};
    // The weight every fragment receives under Uniform. It is bounded by kMinimumOitWeight and kMaximumOitWeight
    // whichever function is selected, so a settings value can never describe a weight the technique refuses, and by
    // the declared minimum and maximum below when it is the function actually in use.
    double uniformWeight{1.0};
    double nearScaleMetres{5.0};
    double farScaleMetres{200.0};
    // The clamp applied to the evaluated weight. minimumWeight may not sit below kMinimumOitWeight and
    // maximumWeight may not sit above kMaximumOitWeight: a caller may narrow the published range, never widen it.
    double minimumWeight{kMinimumOitWeight};
    double maximumWeight{kMaximumOitWeight};

    [[nodiscard]] bool operator==(OitWeightSettings const &) const noexcept = default;
};

[[nodiscard]] std::expected<double, ContractError> OitFragmentWeight(double viewDepthMetres,
                                                                     OitWeightSettings const &settings) noexcept;

// The two render targets a weighted blended pass owns, plus the counters that make a merge checkable. Accumulation
// is a sum and revealage is a product, and both are commutative and associative in exact arithmetic, which is the
// whole basis of the technique's order independence.
struct WeightedOitAccumulator final
{
    Rgb weightedColorSum{};
    double weightedAlphaSum{};
    double revealage{1.0};
    std::uint32_t fragmentCount{};

    [[nodiscard]] bool operator==(WeightedOitAccumulator const &) const noexcept = default;
};

[[nodiscard]] std::expected<WeightedOitAccumulator, ContractError> AccumulateWeightedOitFragment(
    WeightedOitAccumulator const &accumulator, TransparentFragment const &fragment,
    OitWeightSettings const &settings) noexcept;

[[nodiscard]] std::expected<WeightedOitAccumulator, ContractError> AccumulateWeightedOit(
    std::span<TransparentFragment const> fragments, OitWeightSettings const &settings) noexcept;

// Merging two partial accumulators is exactly what independent GPU threads writing to the same pixel through
// additive and multiplicative blending do. It is offered so that order independence can be demonstrated
// structurally rather than only by shuffling an array.
[[nodiscard]] std::expected<WeightedOitAccumulator, ContractError> MergeWeightedOitAccumulators(
    WeightedOitAccumulator const &left, WeightedOitAccumulator const &right) noexcept;

struct WeightedOitResolveSettings final
{
    double alphaEpsilon{kDefaultOitAlphaEpsilon};

    [[nodiscard]] bool operator==(WeightedOitResolveSettings const &) const noexcept = default;
};

struct WeightedOitResult final
{
    PremultipliedColor layer{};
    Rgb overBackground{};
    // The weighted average colour of the transparent fragments: sum(w * a * c) / sum(w * a). It is a convex
    // combination of the fragment colours, so it can never leave their range, and it carries no ordering
    // information at all.
    Rgb averageColor{};
    double revealage{};
    double weightedAlphaSum{};
    // True when the accumulated alpha fell below the declared epsilon and the epsilon was used as the divisor. The
    // layer is empty to within that epsilon when it happens, so the guard changes the colour of nothing visible.
    bool usedAlphaEpsilon{};
    // Revealage of exactly one: no fragment contributed, and the result must be the background bit for bit.
    bool fullyRevealed{};
    // Revealage of exactly zero: the transparent layer is opaque, the background contributes nothing, and the
    // result is the weighted average colour alone.
    bool fullyOccluded{};
    std::uint32_t fragmentCount{};

    [[nodiscard]] bool operator==(WeightedOitResult const &) const noexcept = default;
};

// Resolves the two targets against the opaque background:
//
//     average = weightedColorSum / max(weightedAlphaSum, epsilon)
//     result  = average * (1 - revealage) + background * revealage
//
// What the technique gets exactly right is the *coverage*: revealage is the same product of transmissions the
// sorted reference accumulates, so the transparent layer's alpha is order independent and correct. What it
// approximates is the *colour*: every fragment is replaced by the weighted average of all of them, so a red pane
// in front of a blue one and a blue pane in front of a red one resolve identically. The technique is exact in two
// declared cases and only two: a single fragment, and any number of fragments that share a colour. Everywhere else
// it is wrong by an amount CompareWeightedOitToSorted measures.
[[nodiscard]] std::expected<WeightedOitResult, ContractError> ResolveWeightedOit(
    WeightedOitAccumulator const &accumulator, Rgb background, WeightedOitResolveSettings const &settings) noexcept;

struct OitComparison final
{
    // Largest absolute channel difference between the two composites over the same background.
    double maximumChannelError{};
    double luminanceError{};
    // Difference in the transparent layer's alpha. It is bounded by accumulated rounding rather than by the
    // approximation, because both techniques compute the same product of transmissions.
    double alphaError{};
    double sortedLuminance{};
    double weightedLuminance{};
    bool colorExact{};
    bool alphaExact{};

    [[nodiscard]] bool operator==(OitComparison const &) const noexcept = default;
};

// Both results must have been produced from the same fragments over the same background for the comparison to mean
// anything; nothing here can check that, which is why the sorted reference is the argument rather than an internal
// recomputation.
[[nodiscard]] std::expected<OitComparison, ContractError> CompareWeightedOitToSorted(
    WeightedOitResult const &weighted, SortedCompositionResult const &sorted) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// I. Transparent lighting: consuming the opaque depth and the declared cluster light list
// ---------------------------------------------------------------------------------------------------------------

// Chapter 14 builds the cluster grid and the light lists. This chapter only consumes them, and the only thing it
// has to get right is which cluster a *transparent* fragment belongs to. The grid is described rather than
// constructed here, and the cluster index is row-major in x, then y, then slice, which is the layout Chapter 14
// publishes.
struct ClusterGridDescription final
{
    std::uint32_t tileCountX{};
    std::uint32_t tileCountY{};
    std::uint32_t sliceCount{};
    std::uint32_t clusterCount{};

    [[nodiscard]] bool operator==(ClusterGridDescription const &) const noexcept = default;
};

[[nodiscard]] std::expected<void, ContractError> ValidateClusterGrid(ClusterGridDescription const &grid) noexcept;
[[nodiscard]] std::expected<std::uint32_t, ContractError> ClusterIndex(ClusterGridDescription const &grid,
                                                                       std::uint32_t tileX, std::uint32_t tileY,
                                                                       std::uint32_t sliceIndex) noexcept;

// What a transparent shading invocation claims about itself. The slice indices are supplied because Chapter 14 owns
// the depth slicing; what is checked here is that the fragment used *its own* slice and a light range that actually
// exists.
struct TransparentLightBinding final
{
    std::uint32_t tileX{};
    std::uint32_t tileY{};
    std::uint32_t fragmentSliceIndex{};
    double fragmentViewDepthMetres{};
    OpaqueDepthState opaque{};
    std::uint32_t opaqueSliceIndex{};
    std::uint32_t declaredClusterIndex{};
    std::uint32_t lightOffset{};
    std::uint32_t lightCount{};

    [[nodiscard]] bool operator==(TransparentLightBinding const &) const noexcept = default;
};

struct TransparentLightBindingEvidence final
{
    std::uint32_t expectedClusterIndex{};
    std::uint32_t opaqueClusterIndex{};
    bool hasOpaqueCluster{};
    // True when the fragment's own slice and the opaque surface's slice coincide, so the two cluster indices agree
    // and the binding cannot distinguish a correct implementation from the bug. It is published so that a test
    // designed to catch the bug can prove it used a case where the bug is detectable.
    bool clusterAmbiguous{};
    std::uint32_t lightCount{};
    std::uint32_t lightOffset{};
    // True when the binding declared an opaque surface, which is the depth the fragment must be tested against.
    bool consumedOpaqueDepth{};
    bool depthRejected{};
    // Every light index in the declared range, checked against the scene's light count.
    bool lightIndicesValid{};

    [[nodiscard]] bool operator==(TransparentLightBindingEvidence const &) const noexcept = default;
};

// Validates that a transparent fragment consumed the opaque depth and the declared cluster's light list correctly.
// The error precedence is fixed, and it runs cheapest and most structural first so that a binding with several
// faults always reports the same one: NonFinite for the fragment depth or, when a surface is declared, the opaque
// depth; then the structural checks, which are grid validity, tile bounds, the fragment slice, the opaque slice
// when a surface is declared, the scene light count, the light index span and the declared range inside it, and
// the individual light indices; then the semantic checks, which are fragment and opaque depth validity,
// ClusterFromOpaqueDepth when the declared index is the opaque surface's and the two slices differ, and
// ClusterIndexMismatch for any other wrong index.
[[nodiscard]] std::expected<TransparentLightBindingEvidence, ContractError> ValidateTransparentLightBinding(
    TransparentLightBinding const &binding, ClusterGridDescription const &grid,
    std::span<std::uint32_t const> lightIndices, std::uint32_t sceneLightCount) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// J. Screen-space refraction input
// ---------------------------------------------------------------------------------------------------------------

// What the refraction sample fell back to, if anything. The fallback is always the surface's own UV, which samples
// the opaque scene directly behind the surface: it is the answer an unrefracted surface would give, so a fallback
// degrades to "clear glass" rather than to a wrong part of the scene.
enum class RefractionFallback : std::uint8_t
{
    None = 0U,
    OutOfBounds,
    // The opaque surface at the offset location is in front of the refracting surface, so it is not behind the
    // glass at all. Sampling it would drag a foreground object into the refraction and smear it across the glass,
    // which is the artefact every screen-space refraction implementation is judged on.
    ForegroundOccluder,
};

struct RefractionInput final
{
    Float2 surfaceUv{};
    // The screen-space offset the material asked for, before any clamping. Producing it from a normal, an index of
    // refraction, and a thickness is the lab's job; bounding it is this contract's.
    Float2 requestedOffsetUv{};
    Extent2D sourceExtent{};
    double surfaceViewDepthMetres{};
    OpaqueDepthState offsetOpaque{};

    [[nodiscard]] bool operator==(RefractionInput const &) const noexcept = default;
};

struct RefractionSettings final
{
    double maximumOffsetUv{0.05};
    bool rejectForegroundOccluders{true};

    [[nodiscard]] bool operator==(RefractionSettings const &) const noexcept = default;
};

struct RefractionSample final
{
    Float2 sampledUv{};
    Float2 appliedOffsetUv{};
    double requestedOffsetLength{};
    double appliedOffsetLength{};
    bool clampedOffset{};
    RefractionFallback fallback{RefractionFallback::None};
    bool usedFallback{};
    // The texel the sampled UV lands in, so that a test can assert the offset moved the sample by the number of
    // texels it claimed to.
    PixelCoordinate sampledTexel{};

    [[nodiscard]] bool operator==(RefractionSample const &) const noexcept = default;
};

// Screen-space refraction reads a copy of the *opaque* scene, taken before any transparency was composited. That
// single sentence contains every limitation the technique has: whatever is not in the opaque scene cannot be
// refracted, so a second pane of glass behind the first is invisible through it; whatever left the screen cannot be
// refracted, so an offset that leaves the frame has nothing to read; and the sampled colour is the radiance that
// travelled toward the camera, not the radiance that travelled toward the refracting surface, so the result is a
// plausible distortion rather than transported light.
//
// Nothing here models wavelength-dependent dispersion, total internal reflection, absorption along the path
// through the medium, or the Fresnel split between the reflected and transmitted lobes. The offset is a bounded
// screen-space displacement and is presented as one.
//
// The error precedence is fixed, and it runs cheapest and most structural first so that an input with several
// faults always reports the same one: NonFinite for any non-finite surface UV, requested offset, surface depth,
// declared opaque depth, or offset limit; then the structural checks, which are the source extent, a surface UV
// outside [0, 1], and the declared offset limit against its own domain; then the semantic checks, which are the
// surface's view depth and the opaque surface's view depth.
[[nodiscard]] std::expected<RefractionSample, ContractError> ComputeRefractionSample(
    RefractionInput const &input, RefractionSettings const &settings) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// K. Fog and frame ordering
// ---------------------------------------------------------------------------------------------------------------

// Where fog is applied relative to the transparent composite.
//
// PerFragmentBeforeComposite folds fog into each fragment at the fragment's own depth, which is the only placement
// that can be correct: a transparent surface at 5 metres and the wall at 500 metres behind it need different
// amounts of fog, and only a per-fragment application knows two depths.
//
// ScreenSpaceAfterComposite runs one full-screen pass using the opaque depth buffer. It is cheaper, it is what a
// deferred fog pass naturally does, and it is wrong for every transparent pixel: it fogs the near glass as though
// it were at the depth of whatever is behind it. The plan below reports that as an approximation the policy chose,
// not as a mistake it can fix.
//
// The policy only describes a placement it can actually have. A screen-space fog pass submitted *before* the
// transparent composite fogs the opaque scene and then has scene-linear transparent radiance added on top of it,
// so the transparent layer is never fogged at all and the approximation the policy declared is not the one the
// frame performs. That is a FogPolicyMismatch rather than a quieter approximation, because a plan that reported it
// as clean would let a frame claim an approximation it did not make.
enum class FogApplication : std::uint8_t
{
    PerFragmentBeforeComposite = 0U,
    ScreenSpaceAfterComposite,
};

struct FogSettings final
{
    double densityPerMetre{};
    // Scene-linear radiance the fog adds along the path. It is a constant here because the point is the ordering,
    // not the fog model; a height-dependent or scattering-phase-dependent model changes the transmittance and
    // in-scattering values but not where the pass belongs.
    Rgb inscatteringRadiance{};

    [[nodiscard]] bool operator==(FogSettings const &) const noexcept = default;
};

// exp(-density * depth), the fraction of the radiance from that depth that survives the fog.
[[nodiscard]] std::expected<double, ContractError> FogTransmittance(double viewDepthMetres,
                                                                    FogSettings const &settings) noexcept;

// Fogs opaque radiance at one depth. This is what a screen-space fog pass does to every pixel it touches.
[[nodiscard]] std::expected<Rgb, ContractError> ApplyFogToRadiance(Rgb radiance, double viewDepthMetres,
                                                                   FogSettings const &settings) noexcept;

// Fogs one premultiplied fragment at the fragment's own depth. The in-scattered radiance is multiplied by the
// fragment's alpha so that the result is still a premultiplied colour: fog added to the covered part of the pixel
// only. Adding unpremultiplied in-scattering here is the bug that makes transparent surfaces glow against a foggy
// background, and it is exactly what happens when a straight-colour fog helper is reused on premultiplied data.
[[nodiscard]] std::expected<PremultipliedColor, ContractError> ApplyFogToFragment(PremultipliedColor const &color,
                                                                                  double viewDepthMetres,
                                                                                  FogSettings const &settings) noexcept;

enum class FrameStage : std::uint8_t
{
    OpaqueGBuffer = 0U,
    OpaqueLighting,
    // The copy of the lit opaque scene that screen-space refraction reads.
    RefractionSourceCopy,
    ScreenSpaceFog,
    TransparentComposite,
    TemporalResolve,
    Exposure,
    ToneMap,
};

struct TransparencyPipelinePolicy final
{
    FogApplication fog{FogApplication::PerFragmentBeforeComposite};
    bool requiresRefractionSource{true};
    bool requiresTemporalResolve{true};

    [[nodiscard]] bool operator==(TransparencyPipelinePolicy const &) const noexcept = default;
};

// Every way a submitted order can put transparency in the wrong place. They are flags rather than a first error,
// because one misplaced pass usually breaks more than one obligation and reporting only the first teaches a
// learner to fix an order one symptom at a time.
//
// The list is deliberately narrow: this chapter owns where the transparent composite, the refraction source copy,
// and fog sit. Chapter 30 owns the order of the post chain itself, and no violation here duplicates one of its
// rules.
enum class OrderViolation : std::uint32_t
{
    None = 0U,
    DuplicateStage = 1U << 0U,
    MissingOpaqueLighting = 1U << 1U,
    MissingTransparentComposite = 1U << 2U,
    MissingRefractionSource = 1U << 3U,
    MissingTemporalResolve = 1U << 4U,
    // The refraction source would contain the transparent surfaces themselves, so the glass would refract a copy of
    // itself and feed back frame over frame.
    RefractionSourceAfterTransparentComposite = 1U << 5U,
    // Copying before the opaque scene is lit refracts an unlit or partially lit image.
    RefractionSourceBeforeOpaqueLighting = 1U << 6U,
    TransparentCompositeBeforeOpaqueLighting = 1U << 7U,
    // Compositing after the temporal resolve leaves the transparent layer with no temporal filtering at all and
    // makes the reactive mask meaningless, because the resolve it describes has already run.
    TransparentCompositeAfterTemporalResolve = 1U << 8U,
    // Compositing after exposure or the tone curve adds scene-linear radiance to a signal that is no longer
    // scene-linear, so the transparent layer is exposed differently from the scene behind it.
    TransparentCompositeAfterExposure = 1U << 9U,
    TransparentCompositeAfterToneMap = 1U << 10U,
    // The declared fog policy and the submitted stages disagree: per-fragment fog with a screen-space fog pass
    // fogs the opaque scene twice, screen-space fog with no fog pass never fogs anything, and screen-space fog
    // submitted before the transparent composite never reaches the transparent layer, so the pass cannot be the
    // approximation the policy asked for.
    FogPolicyMismatch = 1U << 11U,
};

[[nodiscard]] constexpr OrderViolation operator|(OrderViolation left, OrderViolation right) noexcept
{
    return static_cast<OrderViolation>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr OrderViolation &operator|=(OrderViolation &left, OrderViolation right) noexcept
{
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool HasViolation(OrderViolation violations, OrderViolation violation) noexcept
{
    return (static_cast<std::uint32_t>(violations) & static_cast<std::uint32_t>(violation)) != 0U;
}

struct TransparencyOrderPlan final
{
    std::array<FrameStage, kMaximumStageCount> stages{};
    std::uint32_t stageCount{};
    OrderViolation violations{OrderViolation::None};
    bool valid{};
    std::uint32_t transparentCompositePosition{};
    bool hasTransparentComposite{};
    bool refractionSourcePrecedesComposite{};
    bool fogFoldedIntoFragments{};
    // True when a screen-space fog pass runs after the transparent composite, so the transparent pixels were fogged
    // with the opaque scene's depth. It is an approximation the policy asked for, not a violation, and it is
    // published so that the cost of the cheaper placement is visible instead of assumed away.
    //
    // A valid plan under FogApplication::ScreenSpaceAfterComposite always has this set, because a fog pass that is
    // missing or that precedes the composite is a FogPolicyMismatch. The approximation therefore never appears as
    // an absence: a plan is either fogged with the wrong depth and says so, or it is not this policy's plan.
    bool screenSpaceFogUsesOpaqueDepth{};

    [[nodiscard]] bool operator==(TransparencyOrderPlan const &) const noexcept = default;
};

[[nodiscard]] std::expected<TransparencyOrderPlan, ContractError> CanonicalTransparencyOrder(
    TransparencyPipelinePolicy const &policy) noexcept;
[[nodiscard]] std::expected<TransparencyOrderPlan, ContractError> PlanTransparencyOrder(
    std::span<FrameStage const> stages, TransparencyPipelinePolicy const &policy) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// L. Temporal reactive mask
// ---------------------------------------------------------------------------------------------------------------

struct ReactiveMaskInput final
{
    // How much of the pixel the transparent layer owns: 1 - revealage from either composite.
    double transparentAlpha{};
    // Whether the transparent surfaces wrote motion vectors. Transparency usually does not, because a pixel covered
    // by two surfaces has two motions and the buffer holds one; a surface that did write one is at least
    // reprojectable.
    bool wroteTransparentMotionVector{};
    bool usedRefraction{};
    double refractionOffsetLengthUv{};
    bool refractionFallback{};
    // Stochastic coverage with an animated pattern changes the surviving samples every frame on purpose, so the
    // history at that pixel describes a different set of samples.
    bool animatedStochasticCoverage{};

    [[nodiscard]] bool operator==(ReactiveMaskInput const &) const noexcept = default;
};

struct ReactiveMaskSettings final
{
    double alphaWeight{1.0};
    double refractionWeight{1.0};
    double stochasticWeight{1.0};
    // The offset length that counts as fully reactive. A refraction that moves the sample by less than this is
    // proportionally less likely to have invalidated the history.
    double referenceOffsetUv{0.01};
    double maximumMask{kFullyReactiveMask};

    [[nodiscard]] bool operator==(ReactiveMaskSettings const &) const noexcept = default;
};

struct ReactiveMaskResult final
{
    ReactiveMask mask{};
    double alphaTerm{};
    double refractionTerm{};
    double stochasticTerm{};
    bool saturated{};
    bool anyRisk{};

    [[nodiscard]] bool operator==(ReactiveMaskResult const &) const noexcept = default;
};

// The mask reports *risk*, not policy. It says that this pixel's history is likely to describe something else, and
// it deliberately does not say what a temporal resolve should do about it: rejecting history outright, lowering
// feedback, or widening a neighbourhood clamp are all defensible, they belong to Chapter 28, and hard-coding one of
// them here would smuggle a TAA policy into a compositing contract.
//
// The three risks are combined as a probabilistic union, term1 + term2 - term1 * term2, so the result is monotonic
// in every term, never leaves [0, 1], and never double counts a pixel that is both refractive and stochastic.
//
// Refraction contributes even when the surface wrote a motion vector, because the refracted background moves with
// the *background*, not with the glass, so the surface's own motion vector reprojects the wrong pixel. A refraction
// that fell back counts as fully reactive rather than as no refraction at all: the sample position jumps between
// the refracted and the unrefracted texel as the surface moves, which is exactly the discontinuity a history buffer
// cannot follow.
//
// The error precedence is fixed: the transparent alpha, then the refraction offset length against the legal UV
// displacement, then the settings.
[[nodiscard]] std::expected<ReactiveMaskResult, ContractError> ComputeReactiveMask(
    ReactiveMaskInput const &input, ReactiveMaskSettings const &settings) noexcept;

} // namespace ch32::transparency
