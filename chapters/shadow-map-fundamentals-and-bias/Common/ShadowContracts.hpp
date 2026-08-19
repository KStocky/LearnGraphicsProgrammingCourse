#pragma once

#include <array>
#include <cstdint>
#include <expected>

namespace ch07::shadows
{

// Matrices are row-major and points are row vectors: p' = p * M. Light view
// space is left-handed (+Z points from the light toward the scene). D3D NDC is
// x/y in [-1, 1], z in [0, 1]; shadow UV has its Y axis flipped.
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

struct AxisAlignedBounds final
{
    Float3 minimum{};
    Float3 maximum{};
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

struct ShadowMapDimensions final
{
    std::uint32_t width{};
    std::uint32_t height{};
};

struct DirectionalLightViewInput final
{
    Float3 position{};
    Float3 directionToLight{};
    Float3 upHint{0.0F, 1.0F, 0.0F};
};

struct DirectionalLightBasis final
{
    Float3 right{};
    Float3 up{};
    Float3 forward{};
    bool usedFallbackUp{};
};

struct LightViewResult final
{
    DirectionalLightBasis basis{};
    Matrix4 view{};
};

enum class FrustumBoundary : std::uint8_t
{
    None = 0U,
    Left = 1U << 0U,
    Right = 1U << 1U,
    Bottom = 1U << 2U,
    Top = 1U << 3U,
    Near = 1U << 4U,
    Far = 1U << 5U,
};

enum class FrustumClassification : std::uint8_t
{
    Inside = 0,
    OnBoundary,
    Outside,
};

struct ProjectionResult final
{
    Float4 clip{};
    Float3 ndc{};
    Float2 uv{};
    float depth{};
    FrustumClassification classification{FrustumClassification::Outside};
    FrustumBoundary boundaryMask{FrustumBoundary::None};
    FrustumBoundary outsideMask{FrustumBoundary::None};
};

struct ReceiverInput final
{
    Float3 worldPosition{};
    Float3 worldNormal{};
};

struct RasterBiasParameters final
{
    float constantDepthBias{};
    float slopeScaledDepthBias{};
    float depthBiasClamp{};
};

struct ReceiverBiasParameters final
{
    float receiverDepthBias{};
    float normalOffsetWorld{};
};

struct SlopeParameters final
{
    float minimumAbsCosine{1.0e-4F};
    float maximumSlope{1.0e4F};
};

struct SlopeResult final
{
    float absCosine{};
    float unclampedSlope{};
    float slope{};
    bool grazingClamped{};
};

struct RasterBiasResult final
{
    float constantContribution{};
    float slopeContribution{};
    float unclampedBias{};
    float appliedBias{};
    bool clampApplied{};
};

struct ReceiverBiasResult final
{
    Float3 normalOffset{};
    Float3 offsetWorldPosition{};
    float originalDepth{};
    float normalOffsetDepth{};
    float normalOffsetDepthContribution{};
    float receiverDepthContribution{};
    float comparisonDepth{};
};

struct ShadowComparisonResult final
{
    bool lit{};
    bool compared{};
    float receiverComparisonDepth{};
    float storedDepth{};
};

enum class ShadowError : std::uint8_t
{
    NonFiniteValue = 0,
    ZeroLengthDirection,
    InvalidBounds,
    InvalidOrthographicExtents,
    InvalidDepthRange,
    InvalidMapDimensions,
    DegenerateBasis,
    InvalidHomogeneousCoordinate,
    InvalidSlopeParameters,
    InvalidBiasParameters,
    ArithmeticOverflow,
};

[[nodiscard]] constexpr FrustumBoundary operator|(FrustumBoundary first, FrustumBoundary second) noexcept
{
    return static_cast<FrustumBoundary>(static_cast<std::uint8_t>(first) | static_cast<std::uint8_t>(second));
}

[[nodiscard]] constexpr bool HasBoundary(FrustumBoundary mask, FrustumBoundary boundary) noexcept
{
    return (static_cast<std::uint8_t>(mask) & static_cast<std::uint8_t>(boundary)) != 0U;
}

[[nodiscard]] std::expected<DirectionalLightBasis, ShadowError> BuildDirectionalLightBasis(
    Float3 directionToLight, Float3 upHint = {0.0F, 1.0F, 0.0F}) noexcept;
[[nodiscard]] std::expected<LightViewResult, ShadowError> BuildDirectionalLightView(
    DirectionalLightViewInput const &input) noexcept;
[[nodiscard]] std::expected<std::array<Float3, 8U>, ShadowError> BoundsCorners(
    AxisAlignedBounds const &bounds) noexcept;
[[nodiscard]] std::expected<Float4, ShadowError> TransformPoint(Float3 point, Matrix4 const &matrix) noexcept;
[[nodiscard]] std::expected<AxisAlignedBounds, ShadowError> TransformBounds(AxisAlignedBounds const &bounds,
                                                                            Matrix4 const &matrix) noexcept;
[[nodiscard]] std::expected<Matrix4, ShadowError> BuildD3DOrthographicProjection(OrthographicExtents const &extents,
                                                                                 DepthRange const &depthRange) noexcept;
[[nodiscard]] std::expected<Matrix4, ShadowError> Multiply(Matrix4 const &first, Matrix4 const &second) noexcept;
[[nodiscard]] std::expected<ProjectionResult, ShadowError> ProjectWorldToShadow(
    Float3 worldPoint, Matrix4 const &lightViewProjection, float boundaryTolerance = 1.0e-6F) noexcept;
[[nodiscard]] std::expected<Float2, ShadowError> WorldTexelSize(OrthographicExtents const &extents,
                                                                ShadowMapDimensions dimensions) noexcept;
[[nodiscard]] std::expected<SlopeResult, ShadowError> ComputeSlopeMetric(
    Float3 worldNormal, Float3 directionToLight, SlopeParameters const &parameters = {}) noexcept;
[[nodiscard]] std::expected<RasterBiasResult, ShadowError> ComputeRasterBias(RasterBiasParameters const &parameters,
                                                                             float slope) noexcept;
[[nodiscard]] std::expected<ReceiverBiasResult, ShadowError> ComputeReceiverBias(
    ReceiverInput const &receiver, ReceiverBiasParameters const &parameters,
    Matrix4 const &lightViewProjection) noexcept;
[[nodiscard]] std::expected<ShadowComparisonResult, ShadowError> CompareShadowDepthLessEqual(
    ProjectionResult const &receiverProjection, float storedDepth, float receiverDepthBias = 0.0F) noexcept;

} // namespace ch07::shadows
