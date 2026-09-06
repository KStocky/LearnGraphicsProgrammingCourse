#pragma once

#include <array>
#include <cstdint>
#include <expected>

namespace ch24::cascaded_shadows
{

// ===========================================================================
// Coordinate, matrix, and interval conventions
// ---------------------------------------------------------------------------
// - Matrices are row-major and points are row vectors: p' = p * M. A composed
//   transform (A * B) applies A first, then B: p * (A * B) = (p * A) * B.
// - World space, the perspective camera view space, and the directional light
//   view space are all left-handed. The camera and the light both look along
//   +Z in their own view space. This matches Chapter 1's
//   XMMatrixPerspectiveFovLH camera and Chapter 7's directional light view.
// - Clip/NDC is Direct3D style: x and y in [-1, 1], z in [0, 1] (this core does
//   not assume reverse-Z). Shadow-map UV flips Y: u = x*0.5 + 0.5,
//   v = 0.5 - y*0.5.
// - View-space depth is the positive distance measured along +Z (camera
//   forward). Cascade splits partition the positive interval [near, far].
// - A cascade's view-depth interval is half-open [split_i, split_{i+1}) for
//   every cascade except the last, whose far end is closed so that a receiver
//   exactly on the far plane still belongs to the final cascade.
// - Light view space places the shadow map in the XY plane; +Z is the light
//   forward (into the scene), so occluders nearer the light have smaller Z.
// ===========================================================================

inline constexpr std::uint32_t kMinCascadeCount = 1U;
inline constexpr std::uint32_t kMaxCascadeCount = 4U;
inline constexpr std::size_t kSliceCornerCount = 8U;
inline constexpr std::size_t kMaxSplitBoundaryCount = static_cast<std::size_t>(kMaxCascadeCount) + 1U;

struct Float2 final
{
    float x{};
    float y{};

    [[nodiscard]] constexpr bool operator==(Float2 const &) const noexcept = default;
};

struct Float3 final
{
    float x{};
    float y{};
    float z{};

    [[nodiscard]] constexpr bool operator==(Float3 const &) const noexcept = default;
};

struct Float4 final
{
    float x{};
    float y{};
    float z{};
    float w{};

    [[nodiscard]] constexpr bool operator==(Float4 const &) const noexcept = default;
};

struct Matrix4 final
{
    std::array<std::array<float, 4U>, 4U> elements{};

    [[nodiscard]] constexpr bool operator==(Matrix4 const &) const noexcept = default;
};

struct OrthographicExtents final
{
    float left{};
    float right{};
    float bottom{};
    float top{};
};

struct DepthRange final
{
    float nearPlane{};
    float farPlane{};
};

struct BoundingSphere final
{
    Float3 center{};
    float radius{};
};

// Strategy chosen for stabilizing the XY footprint of each cascade. The
// recommended TexelSnappedSphere strategy is honest about its trade-off: a
// rotation-invariant bounding sphere keeps the projected XY extent constant as
// the camera yaws, at the cost of wasting resolution on the disc that
// circumscribes the tighter frustum-slice footprint. TightAabbTexelSnapped
// keeps the axis-aligned footprint tight but its extent changes as the camera
// rotates, which reintroduces shimmer even though the corner is still snapped.
//
// Honest limitations (do not overclaim): texel snapping removes the sub-texel
// crawl caused by camera translation, but it does not freeze the projection. A
// changing field of view or near/far distance rescales the bounding sphere, and
// caster/receiver padding changes the depth range; both still alter the
// projection frame to frame. Snapping the min corner also leaves up to one
// texel of slack at the far edge, which is why the array slice contract exposes
// a border inset.
enum class StabilizationMode : std::uint8_t
{
    None = 0U,
    TexelSnappedSphere,
    TightAabbTexelSnapped,
};

// Split placement scheme. PracticalBlend combines the uniform and logarithmic
// schemes: d_i = lambda * d_log + (1 - lambda) * d_uniform.
enum class SplitScheme : std::uint8_t
{
    Uniform = 0U,
    Logarithmic,
    PracticalBlend,
};

enum class CascadeError : std::uint8_t
{
    NonFiniteValue = 0U,
    InvalidNearPlane,
    InvalidDepthRange,
    InvalidFieldOfView,
    InvalidAspectRatio,
    ZeroLengthLightDirection,
    DegenerateBasis,
    InvalidCascadeCount,
    InvalidShadowResolution,
    InvalidSplitScheme,
    InvalidSplitLambda,
    InvalidBlendFraction,
    InvalidPadding,
    InvalidStabilizationMode,
    InvalidCascadeIndex,
    NonPositiveViewDepth,
    InvalidExtents,
    InvalidBiasParameters,
    ArithmeticOverflow,
};

struct CascadeConfig final
{
    float nearPlane{0.1F};
    float farPlane{200.0F};
    float verticalFovRadians{1.0471975512F};
    float aspectRatio{1.7777777778F};
    Float3 directionToLight{0.0F, 1.0F, 0.0F};
    std::uint32_t cascadeCount{4U};
    std::uint32_t shadowResolution{2048U};
    float splitLambda{0.5F};
    float blendFraction{0.1F};
    float casterDepthPadding{0.0F};
    float receiverDepthPadding{0.0F};
    StabilizationMode stabilization{StabilizationMode::TexelSnappedSphere};
};

struct CascadeSplits final
{
    std::uint32_t cascadeCount{};
    std::array<float, kMaxSplitBoundaryCount> boundaries{};
};

struct CascadeInterval final
{
    float nearDistance{};
    float farDistance{};
    bool closedFar{};
};

struct OrthonormalBasis final
{
    Float3 right{};
    Float3 up{};
    Float3 forward{};
    bool usedFallbackUp{};
};

struct CameraSlice final
{
    Float3 position{};
    Float3 forward{0.0F, 0.0F, 1.0F};
    Float3 up{0.0F, 1.0F, 0.0F};
    float verticalFovRadians{1.0471975512F};
    float aspectRatio{1.7777777778F};
};

struct LightView final
{
    OrthonormalBasis basis{};
    Matrix4 view{};
};

struct TexelSnap final
{
    Float2 snappedCenter{};
    Float2 offset{};
};

struct CascadeProjection final
{
    OrthographicExtents extents{};
    DepthRange depthRange{};
    Matrix4 lightView{};
    Matrix4 lightViewProjection{};
    Float2 lightSpaceCenter{};
    float lightSpaceCenterZ{};
    float boundingRadius{};
    Float2 worldUnitsPerTexel{};
    Float2 stabilizationOffset{};
    StabilizationMode mode{StabilizationMode::None};
    bool usedFallbackUp{};
};

struct CascadeSelection final
{
    std::uint32_t primaryIndex{};
    std::uint32_t secondaryIndex{};
    float blendWeight{};
    float transitionWidth{};
    bool inRange{};
    bool belowRange{};
    bool aboveRange{};
};

struct CascadeArraySlice final
{
    std::uint32_t sliceIndex{};
    Float2 uvMin{};
    Float2 uvMax{};
};

struct CascadeBiasScale final
{
    Float2 worldUnitsPerTexel{};
    float texelScale{};
    float scaledReceiverDepthBias{};
    float scaledNormalOffsetWorld{};
};

struct CascadeDiagnostics final
{
    float splitNear{};
    float splitFar{};
    Float2 lightSpaceCenter{};
    Float2 xyExtent{};
    float zSpan{};
    Float2 worldUnitsPerTexel{};
    float xyUtilization{};
    float wastedAreaProxy{};
    float blendWidth{};
    Float2 stabilizationOffset{};
    float texelRatioToNext{};
    bool blendDisabled{};
    bool usedFallbackUp{};
};

struct CascadeData final
{
    CascadeInterval interval{};
    CascadeProjection projection{};
    CascadeDiagnostics diagnostics{};
};

struct CascadeScene final
{
    Float3 cameraPosition{};
    Float3 cameraForward{0.0F, 0.0F, 1.0F};
    Float3 cameraUp{0.0F, 1.0F, 0.0F};
    CascadeConfig config{};
    Float3 lightUpHint{0.0F, 1.0F, 0.0F};
    SplitScheme splitScheme{SplitScheme::PracticalBlend};
};

struct CascadeSetup final
{
    std::uint32_t cascadeCount{};
    CascadeSplits splits{};
    std::array<CascadeData, kMaxCascadeCount> cascades{};
};

// --- Configuration -------------------------------------------------------

// Validates every field with typed errors. Never clamps: an invalid field is
// reported rather than silently replaced with a default.
[[nodiscard]] std::expected<CascadeConfig, CascadeError> ValidateCascadeConfig(CascadeConfig const &config) noexcept;

// Deliberate, separate UI helper that clamps a config into the supported range.
// This is for interactive editing only; the math core always validates.
[[nodiscard]] CascadeConfig NormalizeCascadeConfigForUi(CascadeConfig const &config) noexcept;

// --- Split placement -----------------------------------------------------

[[nodiscard]] std::expected<float, CascadeError> ComputeSplitDistance(SplitScheme scheme, float nearPlane,
                                                                      float farPlane, std::uint32_t index,
                                                                      std::uint32_t cascadeCount,
                                                                      float lambda) noexcept;
[[nodiscard]] std::expected<CascadeSplits, CascadeError> ComputeCascadeSplits(
    CascadeConfig const &config, SplitScheme scheme = SplitScheme::PracticalBlend) noexcept;
[[nodiscard]] std::expected<CascadeInterval, CascadeError> CascadeIntervalAt(CascadeSplits const &splits,
                                                                             std::uint32_t index) noexcept;

// --- Camera frustum slice ------------------------------------------------

[[nodiscard]] std::expected<OrthonormalBasis, CascadeError> BuildCameraBasis(Float3 forward, Float3 up) noexcept;
[[nodiscard]] std::expected<std::array<Float3, kSliceCornerCount>, CascadeError> ComputeSliceCorners(
    CameraSlice const &camera, float nearDistance, float farDistance) noexcept;

// Rotation- and translation-invariant minimal on-axis bounding sphere of a
// frustum slice. The radius depends only on near/far distance, field of view,
// and aspect, so the stabilized extent does not change as the camera moves or
// rotates.
[[nodiscard]] std::expected<BoundingSphere, CascadeError> ComputeSliceBoundingSphere(CameraSlice const &camera,
                                                                                     float nearDistance,
                                                                                     float farDistance) noexcept;

// --- Linear algebra helpers ---------------------------------------------

[[nodiscard]] std::expected<OrthonormalBasis, CascadeError> BuildDirectionalLightBasis(Float3 directionToLight,
                                                                                       Float3 upHint = {0.0F, 1.0F,
                                                                                                        0.0F}) noexcept;
[[nodiscard]] std::expected<LightView, CascadeError> BuildDirectionalLightView(Float3 directionToLight,
                                                                               Float3 upHint = {0.0F, 1.0F,
                                                                                                0.0F}) noexcept;
[[nodiscard]] std::expected<Float4, CascadeError> TransformPoint(Float3 point, Matrix4 const &matrix) noexcept;
[[nodiscard]] std::expected<Matrix4, CascadeError> Multiply(Matrix4 const &first, Matrix4 const &second) noexcept;
[[nodiscard]] std::expected<Matrix4, CascadeError> BuildD3DOrthographicProjection(
    OrthographicExtents const &extents, DepthRange const &depthRange) noexcept;

// --- Stabilization -------------------------------------------------------

// Snaps a light-space center to the texel grid. Uses floored division so the
// convention is deterministic for negative coordinates and any resolution.
[[nodiscard]] std::expected<TexelSnap, CascadeError> SnapToTexelGrid(Float2 center, float worldUnitsPerTexel) noexcept;

// Fits and stabilizes the orthographic projection for a single cascade. XY is
// the shadow footprint (stabilized); Z is the padded depth range, computed
// separately from the transformed slice corners.
[[nodiscard]] std::expected<CascadeProjection, CascadeError> FitCascadeProjection(
    CameraSlice const &camera, CascadeInterval const &interval, Float3 directionToLight, std::uint32_t shadowResolution,
    float casterDepthPadding, float receiverDepthPadding, StabilizationMode mode,
    Float3 lightUpHint = {0.0F, 1.0F, 0.0F}) noexcept;

// --- Cascade selection and transition blending ---------------------------

// Selects the cascade(s) for a positive view-space depth. Returns the primary
// and secondary cascade indices and the secondary blend weight in [0, 1). The
// transition band width equals blendFraction * (interval far - interval near)
// and is placed at the upper end of each cascade interval only, so adjacent
// cascades never blend twice and never leave a gap. The final cascade and
// out-of-range depths report a zero blend weight with secondary == primary.
//
// Honest limitation: sampling two cascades across the band hides the resolution
// seam by cross-fading, but it costs a second shadow lookup and does not fix a
// bias or filter-footprint mismatch between the two cascades; those must be
// reconciled with per-cascade bias scaling (see ScaleBiasForCascade).
[[nodiscard]] std::expected<CascadeSelection, CascadeError> SelectCascade(CascadeSplits const &splits, float viewDepth,
                                                                          float blendFraction) noexcept;

// --- Texture2DArray slice mapping ----------------------------------------

// Maps a cascade index to a Texture2DArray layer plus the sample-safe UV
// bounds. The optional border inset is a slice-local UV clamp to avoid bleeding
// across the edge of a cascade; it is deliberately not an atlas gutter and is
// unrelated to comparison-sampler border behavior.
[[nodiscard]] std::expected<CascadeArraySlice, CascadeError> CascadeArraySliceForIndex(
    std::uint32_t index, std::uint32_t cascadeCount, std::uint32_t shadowResolution,
    std::uint32_t borderTexels = 0U) noexcept;

// --- Bias scaling --------------------------------------------------------

// Cascade-aware world-units-per-texel diagnostic (one value per axis).
[[nodiscard]] std::expected<Float2, CascadeError> CascadeTexelWorldSize(OrthographicExtents const &extents,
                                                                        std::uint32_t shadowResolution) noexcept;

// Scales a receiver/normal bias by the ratio of this cascade's texel world size
// to a reference texel world size. This makes explicit that a single global
// bias is not correct across cascades with different world-units-per-texel.
[[nodiscard]] std::expected<CascadeBiasScale, CascadeError> ScaleBiasForCascade(
    OrthographicExtents const &extents, std::uint32_t shadowResolution, float baseReceiverDepthBias,
    float baseNormalOffsetWorld, float referenceWorldUnitsPerTexel) noexcept;

// --- Diagnostics and full setup ------------------------------------------

[[nodiscard]] std::expected<CascadeSetup, CascadeError> BuildCascadedShadowSetup(CascadeScene const &scene) noexcept;

} // namespace ch24::cascaded_shadows
