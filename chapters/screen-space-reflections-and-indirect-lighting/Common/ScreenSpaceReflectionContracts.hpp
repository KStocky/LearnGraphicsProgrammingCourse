#pragma once

#include <cstdint>
#include <expected>
#include <span>

// Chapter 29 teaching contracts for screen-space reflections (SSR) and one-bounce screen-space diffuse indirect
// lighting (SSGI).
//
// Scope and honesty rules for this phase:
//
//   * Every quantity here is a deterministic CPU reference model. It exists so the learner can inspect, in exact
//     arithmetic, the parts of SSR that are usually hidden inside a shader: reconstruction, ray construction, frustum
//     clipping, projected traversal, thickness classification, confidence, and the Monte Carlo estimator.
//   * Screen-space transport can only see what the camera already rasterized. Off-screen geometry, geometry behind
//     the camera, geometry hidden behind a visible surface, and the back sides of visible surfaces do not exist in
//     the depth buffer. The contracts therefore return typed miss reasons and decomposed confidence factors instead
//     of silently producing a plausible colour.
//   * There is no multi-bounce transport here. A screen hit contributes the radiance that already left that surface
//     in the current frame; nothing is traced from it. These contracts do not replace irradiance probes, voxel or
//     surfel transport, or DXR. They are a bounded, cheap, and openly incomplete estimator.
//
// Coordinate conventions (DirectX):
//
//   * View space is left-handed with +X right, +Y up, and +Z forward. The eye is at the origin, so a visible point
//     has viewPosition.z > 0 and viewPosition.z is its view depth.
//   * Clip space maps the frustum to NDC with x and y in [-1, 1], +Y upward, and z in [0, 1].
//   * Texture UV has its origin at the top-left corner, so v = (1 - ndcY) / 2. The UV-Y flip is applied in exactly
//     one place per direction (NdcFromUv and UvFromNdc) and everything else is expressed through those.
//   * Pixel (x, y) samples at UV ((x + 0.5) / width, (y + 0.5) / height).
//
// Depth convention: the repository rasterizes with either forward depth (near maps to 0, far to 1, LESS_EQUAL) or
// reversed depth (near maps to 1, far to 0, GREATER_EQUAL), exactly as Chapter 12 teaches. Both are supported here
// through DepthConvention, and every depth-comparison contract states which one it was given. Device depth is
// non-linear in both cases, so scene comparisons are performed on linear view depth, never on raw device depth.

namespace ch29::screen_space_reflections
{

inline constexpr double kPi = 3.141592653589793238462643383279502884;
inline constexpr std::uint32_t kMaximumDimension = 16'384U;
inline constexpr std::uint32_t kMaximumTraversalStepCount = 256U;
inline constexpr std::uint32_t kMaximumRefinementStepCount = 16U;
inline constexpr std::uint32_t kMaximumIndirectSampleCount = 4'096U;
inline constexpr std::uint32_t kMaximumPyramidLevelCount = 16U;
inline constexpr double kMaximumSceneLinearValue = 1.0e6;
inline constexpr double kMaximumViewDistance = 1.0e6;
inline constexpr double kUnitLengthTolerance = 1.0e-6;

// Boundary slack for the two depth conversions. They are exact at the near and far planes and accurate to a few
// units in the last place in between, so a value that lands outside a stated domain can only have come from another
// rounding step upstream: a shader evaluating the algebraic coefficient form, a reprojection, or a lower-precision
// buffer. Inputs within this slack of the closed domain are folded onto it and anything further out is reported as
// a real domain error. The slack is smaller than one 32-bit float unit in the last place at either boundary, which
// is about 6e-8 for device depth near one, so it can never admit a materially different surface.
inline constexpr double kDepthDomainTolerance = 1.0e-9;

enum class ContractError : std::uint8_t
{
    NonFinite,
    InvalidProjection,
    InvalidExtent,
    InvalidPixel,
    InvalidUv,
    InvalidDepth,
    NonPositiveViewDepth,
    InvalidNormal,
    InvalidDirection,
    InvalidRoughness,
    InvalidSettings,
    InvalidDistance,
    InvalidParameter,
    InvalidConfidence,
    InvalidRadiance,
    NegativeRadiance,
    InvalidAlbedo,
    InvalidDensity,
    InvalidUnitSample,
    InvalidSampleCount,
    DegenerateSample,
    EmptySampleSet,
    BackFacingSurface,
    EmptyRaySegment,
    InvalidPyramidLevel,
    SizeMismatch,
};

struct Float2 final
{
    double x{};
    double y{};

    [[nodiscard]] bool operator==(Float2 const &) const noexcept = default;
};

struct Float3 final
{
    double x{};
    double y{};
    double z{};

    [[nodiscard]] bool operator==(Float3 const &) const noexcept = default;
};

// Scene-linear RGB radiance. Nothing in this chapter blends gamma-coded values.
struct Rgb final
{
    double r{};
    double g{};
    double b{};

    [[nodiscard]] bool operator==(Rgb const &) const noexcept = default;
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

enum class DepthConvention : std::uint8_t
{
    Forward = 0U,
    Reversed,
};

struct PerspectiveProjection final
{
    double verticalFieldOfViewRadians{};
    double aspectRatio{};
    double nearPlane{};
    double farPlane{};
    DepthConvention depthConvention{DepthConvention::Forward};
};

// deviceDepth = additive + reciprocal / viewDepth for both conventions.
struct DeviceDepthCoefficients final
{
    double additive{};
    double reciprocal{};

    [[nodiscard]] bool operator==(DeviceDepthCoefficients const &) const noexcept = default;
};

// Half-extents of the projection window divided by the view depth: x = ndcX * viewDepth * horizontal.
struct FrustumTangents final
{
    double horizontal{};
    double vertical{};

    [[nodiscard]] bool operator==(FrustumTangents const &) const noexcept = default;
};

[[nodiscard]] constexpr double DepthClearValue(DepthConvention convention) noexcept
{
    return convention == DepthConvention::Forward ? 1.0 : 0.0;
}

// The depth clear value is the farthest representable device depth in both conventions, so a texel still holding it
// is background. Background texels carry no surface and can never be intersected.
[[nodiscard]] constexpr bool IsBackgroundDeviceDepth(double deviceDepth, DepthConvention convention) noexcept
{
    return deviceDepth == DepthClearValue(convention);
}

[[nodiscard]] std::expected<void, ContractError> ValidateProjection(PerspectiveProjection projection) noexcept;
// The algebraic shader-side constants, kept because they are what a reconstruction shader uploads. They are exposed
// for teaching, not used by the conversions below: evaluating additive + reciprocal / z subtracts two terms of
// almost equal magnitude at the planes, so it is the arrangement that loses the boundary.
[[nodiscard]] std::expected<DeviceDepthCoefficients, ContractError> MakeDeviceDepthCoefficients(
    PerspectiveProjection projection) noexcept;
[[nodiscard]] std::expected<FrustumTangents, ContractError> ViewFrustumTangents(
    PerspectiveProjection projection) noexcept;

// The two conversions are exact inverses at the planes for every legal projection: the near plane encodes to
// exactly 0 under forward depth and exactly 1 under reversed depth, the far plane the other way round, and each
// encoding decodes back to the plane distance itself. Inputs are admitted within kDepthDomainTolerance of their
// closed domain and folded onto it; results are always returned inside the exact closed domain, so a device depth
// is never a few units in the last place outside [0, 1] and a view depth is never outside [nearPlane, farPlane].
// Values further out than the slack are ContractError::InvalidDepth.
[[nodiscard]] std::expected<double, ContractError> DeviceDepthFromViewDepth(double viewDepth,
                                                                            PerspectiveProjection projection) noexcept;
[[nodiscard]] std::expected<double, ContractError> ViewDepthFromDeviceDepth(double deviceDepth,
                                                                            PerspectiveProjection projection) noexcept;

[[nodiscard]] std::expected<Float2, ContractError> PixelCenterUv(PixelCoordinate pixel, Extent2D extent) noexcept;
// Pure conversions that apply the UV-Y flip exactly once each. They accept any finite value, because a point outside
// the screen has a real NDC outside [-1, 1] and a real UV outside [0, 1]; range rules belong to the sampling
// contracts that actually read a texel.
[[nodiscard]] std::expected<Float2, ContractError> NdcFromUv(Float2 uv) noexcept;
[[nodiscard]] std::expected<Float2, ContractError> UvFromNdc(Float2 ndc) noexcept;

// Reconstruction from UV plus linear view depth. This is the only place that turns a screen sample into a view-space
// point, so a sign or UV-Y error can only be introduced once.
[[nodiscard]] std::expected<Float3, ContractError> ViewPositionFromUv(Float2 uv, double viewDepth,
                                                                      PerspectiveProjection projection) noexcept;
// Reconstruction from UV plus raster device depth, honouring the supplied depth convention.
[[nodiscard]] std::expected<Float3, ContractError> ReconstructViewPosition(Float2 uv, double deviceDepth,
                                                                           PerspectiveProjection projection) noexcept;

struct ScreenProjection final
{
    Float2 ndc{};
    Float2 uv{};
    double viewDepth{};

    [[nodiscard]] bool operator==(ScreenProjection const &) const noexcept = default;
};

// Forward projection of a view-space point. Points at or behind the eye have no screen position and are rejected
// instead of being wrapped around by the perspective divide.
[[nodiscard]] std::expected<ScreenProjection, ContractError> ProjectViewPosition(
    Float3 viewPosition, PerspectiveProjection projection) noexcept;

struct SurfaceSample final
{
    Float3 viewPosition{};
    Float3 viewNormal{};
    double roughness{};
};

struct RayConstructionSettings final
{
    // The origin is displaced along the surface normal by constantNormalBias + depthProportionalNormalBias * viewDepth.
    // The depth-proportional term keeps the displacement roughly constant in screen space as the surface recedes.
    double constantNormalBias{0.005};
    double depthProportionalNormalBias{0.001};
    // Surfaces seen edge-on or from behind cannot produce a stable reflection direction.
    double minimumFacingCosine{1.0e-3};
    double maximumRayDistance{50.0};
};

struct ReflectionRay final
{
    Float3 origin{};
    Float3 direction{};
    // Unit vector from the surface toward the eye. In view space that is normalize(-viewPosition).
    Float3 viewDirection{};
    double nDotV{};
    double appliedNormalBias{};
    double maximumDistance{};
    // dot(direction, viewDirection). Positive values mean the reflected ray travels back toward the camera, which is
    // the configuration screen-space tracing serves worst.
    double towardCameraCosine{};

    [[nodiscard]] bool operator==(ReflectionRay const &) const noexcept = default;
};

// R = 2 * dot(N, V) * N - V, renormalised. The result always satisfies dot(R, N) = dot(V, N) > 0, so a mirror ray
// never starts by pointing into the surface it left.
[[nodiscard]] std::expected<ReflectionRay, ContractError> BuildReflectionRay(
    SurfaceSample const &surface, RayConstructionSettings const &settings) noexcept;

enum class ClipLimit : std::uint8_t
{
    RayStart = 0U,
    RayEnd,
    NearPlane,
    FarPlane,
    LeftPlane,
    RightPlane,
    BottomPlane,
    TopPlane,
};

struct ClippedRaySegment final
{
    bool intersectsFrustum{};
    double enterDistance{};
    double exitDistance{};
    Float3 enterPosition{};
    Float3 exitPosition{};
    ClipLimit enterLimit{ClipLimit::RayStart};
    ClipLimit exitLimit{ClipLimit::RayEnd};
    // Only meaningful when intersectsFrustum is false: the plane that closed the parameter interval.
    ClipLimit rejectionLimit{ClipLimit::RayStart};

    [[nodiscard]] bool operator==(ClippedRaySegment const &) const noexcept = default;
};

// Clips the finite segment [0, maximumDistance] against the six view-frustum half-spaces in the fixed order near,
// far, left, right, bottom, top, and reports which plane bounded each end. The screen domain and the frustum are the
// same region: a point inside the frustum has UV inside [0, 1]^2 and a representable device depth.
[[nodiscard]] std::expected<ClippedRaySegment, ContractError> ClipRayToFrustum(
    ReflectionRay const &ray, PerspectiveProjection projection) noexcept;

// A straight view-space segment is not linear in screen space, but 1/viewDepth is. The segment therefore stores both
// endpoints in UV and in reciprocal view depth, and traversal interpolates both linearly in the same parameter. That
// is exact perspective-correct interpolation, not an approximation.
struct ScreenRaySegment final
{
    Float2 startUv{};
    Float2 endUv{};
    double startViewDepth{};
    double endViewDepth{};
    double startReciprocalViewDepth{};
    double endReciprocalViewDepth{};
    double startDistance{};
    double endDistance{};
    double screenLengthTexels{};

    [[nodiscard]] bool operator==(ScreenRaySegment const &) const noexcept = default;
};

// Clipping guarantees both endpoints lie in the closed unit square, so the projected UVs are saturated to remove the
// last rounding bit at the boundary. Nothing else in traversal needs a range guard afterwards.
[[nodiscard]] std::expected<ScreenRaySegment, ContractError> ProjectRaySegment(ClippedRaySegment const &segment,
                                                                               PerspectiveProjection projection,
                                                                               Extent2D extent) noexcept;

struct TraversalSample final
{
    double parameter{};
    Float2 uv{};
    double reciprocalViewDepth{};
    double viewDepth{};
    Float3 viewPosition{};
    double rayDistance{};

    [[nodiscard]] bool operator==(TraversalSample const &) const noexcept = default;
};

[[nodiscard]] std::expected<TraversalSample, ContractError> SampleScreenRay(ScreenRaySegment const &segment,
                                                                            ReflectionRay const &ray,
                                                                            PerspectiveProjection projection,
                                                                            double parameter) noexcept;

// Deterministic step schedule. Sample i (1-based) uses parameter min(1, (i + startOffsetFraction) * stepLengthTexels
// / screenLengthTexels), so steps are uniform in screen space and the final sample always lands exactly on the
// segment end. startOffsetFraction in [0, 1) is the per-pixel dither that trades banding for noise.
// requiredStepCount saturates at kMaximumTraversalStepCount + 1, which is enough to decide whether the configured
// budget is exhausted without overflowing on an extremely short step length.
struct TraversalStepSchedule final
{
    std::uint32_t requiredStepCount{};
    std::uint32_t stepCount{};
    bool stepBudgetExhausted{};
    double parameterPerStep{};

    [[nodiscard]] bool operator==(TraversalStepSchedule const &) const noexcept = default;
};

struct TraceSettings final
{
    double stepLengthTexels{4.0};
    std::uint32_t maximumStepCount{64U};
    double startOffsetFraction{0.0};
    // Depth buffers store one surface, not a solid. A hit is only accepted when the ray passes behind the sampled
    // surface by no more than constantThickness + depthProportionalThickness * sceneViewDepth. Everything deeper is
    // information the screen simply does not have.
    double constantThickness{0.05};
    double depthProportionalThickness{0.0};
    std::uint32_t refinementStepCount{4U};
};

[[nodiscard]] std::expected<TraversalStepSchedule, ContractError> BuildTraversalSchedule(
    ScreenRaySegment const &segment, TraceSettings const &settings) noexcept;
[[nodiscard]] std::expected<double, ContractError> TraversalStepParameter(ScreenRaySegment const &segment,
                                                                          TraceSettings const &settings,
                                                                          std::uint32_t stepIndex) noexcept;

// Non-owning row-major device-depth image with the UV origin at the top-left texel. Every texel must be a finite
// device depth in [0, 1]; that is a property of the resource, not of any one ray.
struct DepthImageView final
{
    Extent2D extent{};
    std::span<double const> deviceDepth{};
};

// Checks the extent, the storage size, and every stored texel. The whole-image scan is deliberate: it makes a
// malformed depth resource a deterministic context error instead of an error that only appears when some ray
// happens to sample the bad texel. A shader validates its resources once when they are created; this contract makes
// the same obligation explicit and eager at the cost of one linear pass.
[[nodiscard]] std::expected<void, ContractError> ValidateDepthImage(DepthImageView image) noexcept;
[[nodiscard]] std::expected<PixelCoordinate, ContractError> TexelFromUv(Float2 uv, Extent2D extent) noexcept;
// Depth is never filtered: a bilinear blend of two surfaces is a depth that belongs to neither of them.
[[nodiscard]] std::expected<double, ContractError> SampleDeviceDepthNearest(DepthImageView image, Float2 uv) noexcept;

// Every way a screen-space ray can fail to produce usable evidence. The reasons are mutually exclusive and are
// decided in exactly this order, so a result never has to be interpreted alongside a second explanation:
//   1. InvalidInput      - the shaded pixel itself is unusable: background, non-finite, degenerate normal, or a
//                          roughness outside [0, 1].
//   2. BackFacing        - dot(N, V) is at or below the facing minimum, so no stable reflection exists.
//   3. BehindCamera      - the shaded surface is nearer than the near plane, or the clipped segment is empty
//                          because the near plane closed it.
//   4. OffScreen         - the clipped segment is empty because a side or far plane closed it, so the ray never
//                          enters the region the depth buffer represents.
//   5. ThicknessExceeded - crossings were found, but every one was deeper behind the sampled surface than the
//                          thickness interval allows. The screen cannot say what is behind that surface.
//   6. MaximumSteps      - the step budget ran out before the segment was fully searched.
//   7. NoCrossing        - the segment was fully searched and no traversal sample contained any visible geometry.
//   8. Otherwise the segment was fully searched, geometry was seen, and nothing was crossed. The reason is then
//      where the search ran out of screen: MaximumDistance for the ray's own maximum distance, BehindCamera for
//      the near plane, and OffScreen for the far and side planes. segmentExitLimit records the exact boundary.
enum class MissReason : std::uint8_t
{
    None = 0U,
    InvalidInput,
    BackFacing,
    BehindCamera,
    OffScreen,
    MaximumSteps,
    ThicknessExceeded,
    NoCrossing,
    MaximumDistance,
};

struct TraceInput final
{
    SurfaceSample surface{};
    bool surfaceIsBackground{};
};

struct TraceContext final
{
    PerspectiveProjection projection{};
    DepthImageView depth{};
    RayConstructionSettings ray{};
    TraceSettings traversal{};
};

struct TraceResult final
{
    bool hit{};
    MissReason missReason{MissReason::InvalidInput};
    Float2 hitUv{};
    Float3 hitViewPosition{};
    double hitRayViewDepth{};
    double hitSceneViewDepth{};
    double hitDepthDelta{};
    double hitThicknessInterval{};
    double hitRayDistance{};
    double hitParameter{};
    std::uint32_t stepCount{};
    std::uint32_t refinementCount{};
    std::uint32_t geometrySampleCount{};
    std::uint32_t thicknessRejectionCount{};
    bool stepBudgetExhausted{};
    ClipLimit segmentExitLimit{ClipLimit::RayEnd};
    double segmentExitDistance{};

    [[nodiscard]] bool operator==(TraceResult const &) const noexcept = default;
};

// Returns an error only for malformed context data, which is a programming mistake. Every data-dependent outcome,
// including all the ways screen-space information is missing, is reported as a typed miss.
//
// That claim is enforced rather than asserted. The context is fully validated on entry, including every depth texel,
// so a malformed depth resource fails the same way no matter which ray was traced. After that entry check no legal
// depth value can fail during traversal: every stored device depth in [0, 1] decodes to a view depth inside
// [nearPlane, farPlane], including the exact near-plane and far-plane encodings, and every traversal UV is inside
// the unit square by construction.
//
// Traversal detail the result exposes rather than hides:
//   * A texel holding the far-plane encoding is indistinguishable from a cleared texel, because the depth clear
//     value is the far-plane encoding. Both are treated as background and can never be hit.
//   * refinementCount counts bisection samples only; the accepted or rejected crossing is evaluated once more
//     afterwards, so a hit costs refinementStepCount + 1 extra depth samples.
//   * A crossing found at the very first step means the biased origin already started behind the sampled surface.
//     Refinement then converges toward the segment start, and only the thickness interval separates a real contact
//     from self-intersection. Increase the origin bias rather than the thickness when that happens.
//   * Nothing here knows the orientation of the surface the ray crosses. A ray that passes behind a visible surface
//     and hits its back side is accepted exactly like a front-facing contact, because the depth buffer stores no
//     normal. That is a real limitation of screen-space tracing, not an omission in this contract.
[[nodiscard]] std::expected<TraceResult, ContractError> TraceScreenSpaceRay(TraceInput const &input,
                                                                            TraceContext const &context) noexcept;

// Confidence is kept decomposed so a learner can see which assumption failed, and each factor is independently
// bounded to [0, 1]. The combined value is their product; it is a fade weight, not a probability.
struct ConfidenceSettings final
{
    // Width of the border band, in UV, over which a hit fades out as it approaches the edge of the screen.
    double screenEdgeFadeUv{0.1};
    // Fraction of the maximum ray distance after which a hit begins to fade.
    double distanceFadeStartFraction{0.5};
    // A hit exactly on the sampled surface is fully trusted; one at the far end of the thickness interval is not.
    double thicknessFadeFraction{1.0};
    // A single mirror ray stops representing the specular lobe as roughness grows.
    double roughnessFadeStart{0.3};
    double roughnessFadeEnd{0.8};
    // Rays travelling back toward the camera are the classic screen-space failure and fade out over this cosine band.
    double towardCameraFadeStart{0.2};
    double towardCameraFadeEnd{0.9};
};

struct ConfidenceInput final
{
    bool hit{};
    Float2 hitUv{};
    double hitRayDistance{};
    double maximumRayDistance{};
    double hitDepthDelta{};
    double hitThicknessInterval{};
    double roughness{};
    double towardCameraCosine{};
};

struct ConfidenceFactors final
{
    double validity{};
    double screenEdge{};
    double rayDistance{};
    double thickness{};
    double roughness{};
    double towardCamera{};
    double combined{};

    [[nodiscard]] bool operator==(ConfidenceFactors const &) const noexcept = default;
};

[[nodiscard]] std::expected<ConfidenceFactors, ContractError> EvaluateConfidence(
    ConfidenceInput const &input, ConfidenceSettings const &settings) noexcept;

struct ReflectionCompositionInput final
{
    Rgb screenRadiance{};
    Rgb environmentRadiance{};
    double confidence{};
};

// The screen and environment weights are confidence and 1 - confidence. They sum to exactly one, so the incoming
// radiance is a partition of the same quantity and no energy is counted twice when the screen partially answers.
struct ReflectionComposition final
{
    double screenWeight{};
    double environmentWeight{};
    Rgb screenContribution{};
    Rgb environmentContribution{};
    Rgb incomingRadiance{};

    [[nodiscard]] bool operator==(ReflectionComposition const &) const noexcept = default;
};

[[nodiscard]] std::expected<ReflectionComposition, ContractError> ComposeReflection(
    ReflectionCompositionInput const &input) noexcept;

// The split-sum scale and bias come from Chapter 27. The specular BRDF weight F0 * A + B is applied exactly once,
// after the screen and environment radiance have been combined, so a partial screen answer cannot be shaded twice.
[[nodiscard]] std::expected<Rgb, ContractError> ApplySplitSumSpecular(Rgb incomingRadiance,
                                                                      Rgb normalIncidenceReflectance, double scaleA,
                                                                      double biasB) noexcept;

struct HemisphereSample final
{
    Float3 direction{};
    double pdf{};
    double cosine{};

    [[nodiscard]] bool operator==(HemisphereSample const &) const noexcept = default;
};

// Malley's method in the local frame whose +Z is the surface normal: r = sqrt(u1), phi = 2*pi*u2, z = sqrt(1 - u1).
// The density is cos(theta) / pi with respect to solid angle.
[[nodiscard]] std::expected<HemisphereSample, ContractError> MapCosineHemisphere(Float2 unitSample) noexcept;
[[nodiscard]] std::expected<double, ContractError> CosineHemispherePdf(double cosine) noexcept;

struct TangentFrame final
{
    Float3 tangent{};
    Float3 bitangent{};
    Float3 normal{};

    [[nodiscard]] bool operator==(TangentFrame const &) const noexcept = default;
};

// Branchless orthonormal basis (Duff et al.). Only the normal is meaningful to shading; the tangent choice is
// arbitrary but deterministic and right-handed.
[[nodiscard]] std::expected<TangentFrame, ContractError> BuildTangentFrame(Float3 normal) noexcept;
[[nodiscard]] std::expected<Float3, ContractError> TransformLocalToView(TangentFrame const &frame,
                                                                        Float3 localDirection) noexcept;

// The Lambertian BRDF is albedo / pi with units 1/sr. It is deliberately separate from the irradiance estimate so the
// pi in the BRDF and the pi that appears in the cosine-weighted estimator cannot be confused or applied twice.
[[nodiscard]] std::expected<Rgb, ContractError> EvaluateLambertianBrdf(Rgb albedo) noexcept;

enum class IndirectMissPolicy : std::uint8_t
{
    EnvironmentFallback = 0U,
    ZeroRadiance,
};

struct IndirectSample final
{
    Float3 direction{};
    double pdf{};
    bool hit{};
    Rgb hitRadiance{};
    Rgb environmentRadiance{};
};

struct IndirectSettings final
{
    // One bounce only: hitRadiance is the radiance that already left the hit surface this frame. Clamping bounds the
    // variance of a single sample and is reported rather than hidden.
    double maximumRadiance{100.0};
    IndirectMissPolicy missPolicy{IndirectMissPolicy::EnvironmentFallback};
};

// irradiance = (1/N) * sum L_i * cos_i / pdf_i, in watts per square metre. outgoingRadiance = (albedo / pi) *
// irradiance, in watts per square metre per steradian. With the cosine density the cos / pdf ratio is exactly pi, so
// a constant radiance L over the hemisphere gives irradiance = pi * L and outgoing radiance = albedo * L.
struct IndirectEstimate final
{
    Rgb irradiance{};
    Rgb outgoingRadiance{};
    std::uint32_t sampleCount{};
    std::uint32_t hitCount{};
    std::uint32_t missCount{};
    std::uint32_t clampedSampleCount{};
    double averageCosineOverPdf{};

    [[nodiscard]] bool operator==(IndirectEstimate const &) const noexcept = default;
};

[[nodiscard]] std::expected<IndirectEstimate, ContractError> EstimateDiffuseIndirect(
    Float3 viewNormal, Rgb albedo, std::span<IndirectSample const> samples, IndirectSettings const &settings) noexcept;

// Chapter 28 stores motion as previousUV - currentUV and validates history in scene-linear units. This contract
// reuses those two conventions and nothing else; neighbourhood statistics, variance clipping, and the full temporal
// resolve remain that chapter's material.
//
// A history sample is only reused after it has been shown to describe the same surface. Two independent pieces of
// evidence decide that, and both are reported:
//
//   * Material identity. The identifier is whatever the G-buffer stores; the contract only compares it and never
//     interprets it.
//   * View-depth agreement between expectedPreviousViewDepth and sampledHistoryViewDepth. The expected value is the
//     depth of the *current* surface at its previous-frame position, not its current depth, so a surface that moved
//     toward or away from the eye is still recognised as itself. Comparing the current depth instead would reject a
//     correctly reprojected moving surface every frame.
//
// Depth is compared in linear view units, so the device-depth convention that produced it is irrelevant: forward
// and reversed encodings of the same distance decode to the same view depth and reach the same decision.
enum class HistoryRejection : std::uint32_t
{
    None = 0U,
    // The caller has no valid history resource for this frame, so nothing was sampled.
    NoHistory = 1U << 0U,
    // previousUV left the unit square. Off-screen history is dropped, never clamped: clamping would reuse an
    // unrelated edge texel.
    OffScreen = 1U << 1U,
    // The sampled texel has never accumulated anything.
    NoSamples = 1U << 2U,
    MaterialMismatch = 1U << 3U,
    DepthMismatch = 1U << 4U,
};

[[nodiscard]] constexpr std::uint32_t operator|(HistoryRejection left, HistoryRejection right) noexcept
{
    return static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right);
}

struct TemporalReuseInput final
{
    Float2 currentUv{};
    Float2 motionPreviousMinusCurrentUv{};
    // True when a history resource exists for this frame. The history fields below are only read, and only
    // validated, when a sample was actually taken: hasHistory and an on-screen previousUV together. A caller that
    // cannot sample leaves them at whatever it likes, and that is never an error.
    bool hasHistory{};
    Rgb currentSceneLinear{};
    Rgb historySceneLinear{};
    double currentConfidence{};
    double historyConfidence{};
    std::uint32_t previousSampleCount{};
    // Linear view depth of the current surface where it stood in the previous frame.
    double expectedPreviousViewDepth{};
    double sampledHistoryViewDepth{};
    std::uint32_t currentMaterialId{};
    std::uint32_t sampledHistoryMaterialId{};
};

struct TemporalReuseSettings final
{
    std::uint32_t maximumSampleCount{16U};
    // A usable history sample may never remove the current frame entirely. Without this ceiling a pixel whose
    // current confidence is zero would take the history at weight one, so the colour and the confidence would both
    // become fixed points and stale lighting could survive indefinitely. The bound is strictly below one, so the
    // current frame always keeps at least 1 - maximumHistoryWeight of the result and the history decays
    // geometrically once the screen stops confirming it.
    double maximumHistoryWeight{0.95};
    // Depth agreement tolerance: |expected - sampled| <= absolute + relative * max(expected, sampled). The absolute
    // term covers quantisation near the eye and the relative term covers it far away.
    double absoluteDepthTolerance{0.05};
    double relativeDepthTolerance{0.02};
    bool requireMaterialIdentity{true};
};

struct TemporalReuseResult final
{
    Float2 previousUv{};
    bool historyUsable{};
    // Zero exactly when historyUsable is true; otherwise a bitwise or of HistoryRejection values.
    std::uint32_t rejectionReasons{};
    double currentWeight{};
    double historyWeight{};
    double depthDifference{};
    double depthTolerance{};
    Rgb outputSceneLinear{};
    double outputConfidence{};
    std::uint32_t nextSampleCount{};

    [[nodiscard]] bool operator==(TemporalReuseResult const &) const noexcept = default;
};

// currentWeight is always at least 1 - maximumHistoryWeight and the two weights always sum to exactly one.
[[nodiscard]] std::expected<TemporalReuseResult, ContractError> ReuseTemporalSamples(
    TemporalReuseInput const &input, TemporalReuseSettings const &settings) noexcept;

// Hierarchical depth pyramid. Levels halve with ceiling division so an odd extent keeps its final column or row
// instead of dropping it, and out-of-range children are clamped to the last valid texel rather than skipped. Both
// choices exist for the same reason: every base texel must be covered by its parent, or traversal can skip an
// occluder that really is there.
enum class DepthReduction : std::uint8_t
{
    // The surface closest to the eye inside the cell. That is the minimum device depth under forward depth and the
    // maximum device depth under reversed depth.
    NearestSurface = 0U,
    FarthestSurface,
};

[[nodiscard]] std::expected<Extent2D, ContractError> NextPyramidExtent(Extent2D extent) noexcept;
[[nodiscard]] std::expected<std::uint32_t, ContractError> PyramidLevelCount(Extent2D baseExtent) noexcept;
[[nodiscard]] std::expected<Extent2D, ContractError> PyramidLevelExtent(Extent2D baseExtent,
                                                                        std::uint32_t level) noexcept;
[[nodiscard]] std::expected<Extent2D, ContractError> ReduceDepthLevel(DepthImageView source, DepthConvention convention,
                                                                      DepthReduction reduction,
                                                                      std::span<double> destination) noexcept;

struct PyramidCellBounds final
{
    Float2 minimumUv{};
    Float2 maximumUv{};
    PixelCoordinate baseMinimum{};
    PixelCoordinate baseMaximumInclusive{};

    [[nodiscard]] bool operator==(PyramidCellBounds const &) const noexcept = default;
};

[[nodiscard]] std::expected<PyramidCellBounds, ContractError> PyramidCellBoundsAt(Extent2D baseExtent,
                                                                                  std::uint32_t level,
                                                                                  PixelCoordinate levelTexel) noexcept;

enum class PyramidCellDecision : std::uint8_t
{
    // The whole ray segment inside the cell is nearer than the nearest surface the cell contains, so no surface in
    // the cell can be crossed and the segment may be skipped without sampling any base texel.
    SkipInFront = 0U,
    // The segment reaches or passes the nearest surface in the cell, so a finer level must be examined. This is
    // deliberately conservative: a cell is never skipped because its contents are assumed to be behind the ray.
    Descend,
};

[[nodiscard]] std::expected<PyramidCellDecision, ContractError> ClassifyPyramidCell(
    double cellNearestViewDepth, double segmentMinimumViewDepth, double segmentMaximumViewDepth) noexcept;

// Parameter at which uv + t * uvDirection leaves the cell. minimumAdvance guarantees forward progress so a ray
// running exactly along a cell boundary cannot stall the traversal.
[[nodiscard]] std::expected<double, ContractError> CellExitParameter(Float2 uv, Float2 uvDirection,
                                                                     PyramidCellBounds const &bounds,
                                                                     double minimumAdvance) noexcept;

} // namespace ch29::screen_space_reflections
