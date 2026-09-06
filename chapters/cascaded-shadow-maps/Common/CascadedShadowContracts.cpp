#include "CascadedShadowContracts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ch24::cascaded_shadows
{
namespace
{

inline constexpr double kParallelThreshold = 0.999;
inline constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] bool IsFinite(float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool IsFinite(Float2 value) noexcept
{
    return IsFinite(value.x) && IsFinite(value.y);
}

[[nodiscard]] bool IsFinite(Float3 value) noexcept
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

[[nodiscard]] double Dot(Float3 lhs, Float3 rhs) noexcept
{
    return (static_cast<double>(lhs.x) * static_cast<double>(rhs.x)) +
           (static_cast<double>(lhs.y) * static_cast<double>(rhs.y)) +
           (static_cast<double>(lhs.z) * static_cast<double>(rhs.z));
}

[[nodiscard]] Float3 Cross(Float3 first, Float3 second) noexcept
{
    return {
        static_cast<float>((static_cast<double>(first.y) * static_cast<double>(second.z)) -
                           (static_cast<double>(first.z) * static_cast<double>(second.y))),
        static_cast<float>((static_cast<double>(first.z) * static_cast<double>(second.x)) -
                           (static_cast<double>(first.x) * static_cast<double>(second.z))),
        static_cast<float>((static_cast<double>(first.x) * static_cast<double>(second.y)) -
                           (static_cast<double>(first.y) * static_cast<double>(second.x))),
    };
}

[[nodiscard]] Float3 Add(Float3 lhs, Float3 rhs) noexcept
{
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

[[nodiscard]] Float3 Scale(Float3 value, double factor) noexcept
{
    return {
        static_cast<float>(static_cast<double>(value.x) * factor),
        static_cast<float>(static_cast<double>(value.y) * factor),
        static_cast<float>(static_cast<double>(value.z) * factor),
    };
}

[[nodiscard]] Float3 Negate(Float3 value) noexcept
{
    return {-value.x, -value.y, -value.z};
}

[[nodiscard]] std::expected<float, CascadeError> CheckedFloat(double value) noexcept
{
    if (!std::isfinite(value) || value > static_cast<double>((std::numeric_limits<float>::max)()) ||
        value < -static_cast<double>((std::numeric_limits<float>::max)()))
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }
    return static_cast<float>(value);
}

[[nodiscard]] std::expected<Float3, CascadeError> Normalize(Float3 value, CascadeError zeroError) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(CascadeError::NonFiniteValue);
    }
    double const lengthSquared = Dot(value, value);
    if (!std::isfinite(lengthSquared))
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }
    if (lengthSquared <= 0.0)
    {
        return std::unexpected(zeroError);
    }
    double const inverseLength = 1.0 / std::sqrt(lengthSquared);
    Float3 const result{
        static_cast<float>(static_cast<double>(value.x) * inverseLength),
        static_cast<float>(static_cast<double>(value.y) * inverseLength),
        static_cast<float>(static_cast<double>(value.z) * inverseLength),
    };
    if (!IsFinite(result))
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }
    return result;
}

[[nodiscard]] std::expected<OrthonormalBasis, CascadeError> BuildRightHandedFromForward(Float3 forwardInput,
                                                                                        Float3 upHint) noexcept
{
    std::expected<Float3, CascadeError> const forward = Normalize(forwardInput, CascadeError::DegenerateBasis);
    if (!forward)
    {
        return std::unexpected(forward.error());
    }
    std::expected<Float3, CascadeError> normalizedUp = Normalize(upHint, CascadeError::DegenerateBasis);
    if (!normalizedUp)
    {
        return std::unexpected(normalizedUp.error());
    }

    bool const needsFallback = std::fabs(Dot(*normalizedUp, *forward)) >= kParallelThreshold;
    if (needsFallback)
    {
        std::array<Float3, 3U> const axes{Float3{1.0F, 0.0F, 0.0F}, Float3{0.0F, 1.0F, 0.0F}, Float3{0.0F, 0.0F, 1.0F}};
        normalizedUp =
            *std::ranges::min_element(axes, [forward](Float3 first, Float3 second)
                                      { return std::fabs(Dot(first, *forward)) < std::fabs(Dot(second, *forward)); });
    }

    std::expected<Float3, CascadeError> const right =
        Normalize(Cross(*normalizedUp, *forward), CascadeError::DegenerateBasis);
    if (!right)
    {
        return std::unexpected(right.error());
    }
    std::expected<Float3, CascadeError> const up = Normalize(Cross(*forward, *right), CascadeError::DegenerateBasis);
    if (!up)
    {
        return std::unexpected(up.error());
    }
    return OrthonormalBasis{*right, *up, *forward, needsFallback};
}

[[nodiscard]] bool ValidPositiveFinite(float value) noexcept
{
    return IsFinite(value) && value > 0.0F;
}

[[nodiscard]] bool ValidFieldOfView(float value) noexcept
{
    return IsFinite(value) && value > 0.0F && static_cast<double>(value) < kPi;
}

[[nodiscard]] bool ValidExtents(OrthographicExtents const &extents) noexcept
{
    return IsFinite(extents.left) && IsFinite(extents.right) && IsFinite(extents.bottom) && IsFinite(extents.top) &&
           extents.left < extents.right && extents.bottom < extents.top;
}

[[nodiscard]] bool ValidDepthRange(DepthRange const &range) noexcept
{
    return IsFinite(range.nearPlane) && IsFinite(range.farPlane) && range.nearPlane < range.farPlane;
}

[[nodiscard]] std::expected<Float3, CascadeError> TransformToFloat3(Float3 point, Matrix4 const &matrix) noexcept
{
    std::expected<Float4, CascadeError> const transformed = TransformPoint(point, matrix);
    if (!transformed)
    {
        return std::unexpected(transformed.error());
    }
    if (transformed->w != 1.0F)
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }
    return Float3{transformed->x, transformed->y, transformed->z};
}

[[nodiscard]] double SliceRadialFactor(double verticalFov, double aspect) noexcept
{
    double const tangent = std::tan(verticalFov * 0.5);
    return tangent * std::sqrt(1.0 + (aspect * aspect));
}

} // namespace

std::expected<CascadeConfig, CascadeError> ValidateCascadeConfig(CascadeConfig const &config) noexcept
{
    if (!ValidPositiveFinite(config.nearPlane))
    {
        return std::unexpected(CascadeError::InvalidNearPlane);
    }
    if (!IsFinite(config.farPlane) || config.farPlane <= config.nearPlane)
    {
        return std::unexpected(CascadeError::InvalidDepthRange);
    }
    if (!ValidFieldOfView(config.verticalFovRadians))
    {
        return std::unexpected(CascadeError::InvalidFieldOfView);
    }
    if (!ValidPositiveFinite(config.aspectRatio))
    {
        return std::unexpected(CascadeError::InvalidAspectRatio);
    }
    if (!IsFinite(config.directionToLight) || Dot(config.directionToLight, config.directionToLight) <= 0.0)
    {
        return std::unexpected(CascadeError::ZeroLengthLightDirection);
    }
    if (config.cascadeCount < kMinCascadeCount || config.cascadeCount > kMaxCascadeCount)
    {
        return std::unexpected(CascadeError::InvalidCascadeCount);
    }
    if (config.shadowResolution == 0U)
    {
        return std::unexpected(CascadeError::InvalidShadowResolution);
    }
    if (!IsFinite(config.splitLambda) || config.splitLambda < 0.0F || config.splitLambda > 1.0F)
    {
        return std::unexpected(CascadeError::InvalidSplitLambda);
    }
    if (!IsFinite(config.blendFraction) || config.blendFraction < 0.0F || config.blendFraction > 0.5F)
    {
        return std::unexpected(CascadeError::InvalidBlendFraction);
    }
    if (!IsFinite(config.casterDepthPadding) || config.casterDepthPadding < 0.0F ||
        !IsFinite(config.receiverDepthPadding) || config.receiverDepthPadding < 0.0F)
    {
        return std::unexpected(CascadeError::InvalidPadding);
    }
    if (config.stabilization != StabilizationMode::None &&
        config.stabilization != StabilizationMode::TexelSnappedSphere &&
        config.stabilization != StabilizationMode::TightAabbTexelSnapped)
    {
        return std::unexpected(CascadeError::InvalidStabilizationMode);
    }
    return config;
}

CascadeConfig NormalizeCascadeConfigForUi(CascadeConfig const &config) noexcept
{
    CascadeConfig normalized = config;

    normalized.nearPlane = (IsFinite(config.nearPlane) && config.nearPlane > 0.0F) ? config.nearPlane : 0.1F;
    normalized.nearPlane = std::max(normalized.nearPlane, 1.0e-4F);

    float const minimumFar = normalized.nearPlane * 1.0009999F + 1.0e-4F;
    normalized.farPlane = (IsFinite(config.farPlane) && config.farPlane > normalized.nearPlane)
                              ? config.farPlane
                              : (normalized.nearPlane * 4.0F);
    normalized.farPlane = std::max(normalized.farPlane, minimumFar);

    float const maximumFov = static_cast<float>(kPi) - 0.01F;
    normalized.verticalFovRadians =
        IsFinite(config.verticalFovRadians) ? std::clamp(config.verticalFovRadians, 0.01F, maximumFov) : 1.0471975512F;

    normalized.aspectRatio =
        (IsFinite(config.aspectRatio) && config.aspectRatio > 0.0F) ? std::max(config.aspectRatio, 1.0e-3F) : 1.0F;

    if (!IsFinite(config.directionToLight) || Dot(config.directionToLight, config.directionToLight) <= 0.0)
    {
        normalized.directionToLight = Float3{0.0F, 1.0F, 0.0F};
    }

    normalized.cascadeCount = std::clamp(config.cascadeCount, kMinCascadeCount, kMaxCascadeCount);
    normalized.shadowResolution = std::max(config.shadowResolution, 1U);
    normalized.splitLambda = IsFinite(config.splitLambda) ? std::clamp(config.splitLambda, 0.0F, 1.0F) : 0.5F;
    normalized.blendFraction = IsFinite(config.blendFraction) ? std::clamp(config.blendFraction, 0.0F, 0.5F) : 0.0F;
    normalized.casterDepthPadding =
        (IsFinite(config.casterDepthPadding) && config.casterDepthPadding > 0.0F) ? config.casterDepthPadding : 0.0F;
    normalized.receiverDepthPadding = (IsFinite(config.receiverDepthPadding) && config.receiverDepthPadding > 0.0F)
                                          ? config.receiverDepthPadding
                                          : 0.0F;

    if (normalized.stabilization != StabilizationMode::None &&
        normalized.stabilization != StabilizationMode::TexelSnappedSphere &&
        normalized.stabilization != StabilizationMode::TightAabbTexelSnapped)
    {
        normalized.stabilization = StabilizationMode::TexelSnappedSphere;
    }
    return normalized;
}

std::expected<float, CascadeError> ComputeSplitDistance(SplitScheme scheme, float nearPlane, float farPlane,
                                                        std::uint32_t index, std::uint32_t cascadeCount,
                                                        float lambda) noexcept
{
    if (!ValidPositiveFinite(nearPlane))
    {
        return std::unexpected(CascadeError::InvalidNearPlane);
    }
    if (!IsFinite(farPlane) || farPlane <= nearPlane)
    {
        return std::unexpected(CascadeError::InvalidDepthRange);
    }
    if (cascadeCount < kMinCascadeCount || cascadeCount > kMaxCascadeCount)
    {
        return std::unexpected(CascadeError::InvalidCascadeCount);
    }
    if (index > cascadeCount)
    {
        return std::unexpected(CascadeError::InvalidCascadeIndex);
    }
    if (scheme == SplitScheme::PracticalBlend && (!IsFinite(lambda) || lambda < 0.0F || lambda > 1.0F))
    {
        return std::unexpected(CascadeError::InvalidSplitLambda);
    }
    if (scheme != SplitScheme::Uniform && scheme != SplitScheme::Logarithmic && scheme != SplitScheme::PracticalBlend)
    {
        return std::unexpected(CascadeError::InvalidSplitScheme);
    }

    if (index == 0U)
    {
        return nearPlane;
    }
    if (index == cascadeCount)
    {
        return farPlane;
    }

    double const near = static_cast<double>(nearPlane);
    double const far = static_cast<double>(farPlane);
    double const fraction = static_cast<double>(index) / static_cast<double>(cascadeCount);
    double const uniform = near + ((far - near) * fraction);
    double const logarithmic = near * std::pow(far / near, fraction);

    double result = uniform;
    if (scheme == SplitScheme::Logarithmic)
    {
        result = logarithmic;
    }
    else if (scheme == SplitScheme::PracticalBlend)
    {
        double const blend = static_cast<double>(lambda);
        result = (blend * logarithmic) + ((1.0 - blend) * uniform);
    }
    return CheckedFloat(result);
}

std::expected<CascadeSplits, CascadeError> ComputeCascadeSplits(CascadeConfig const &config,
                                                                SplitScheme scheme) noexcept
{
    if (!ValidPositiveFinite(config.nearPlane))
    {
        return std::unexpected(CascadeError::InvalidNearPlane);
    }
    if (!IsFinite(config.farPlane) || config.farPlane <= config.nearPlane)
    {
        return std::unexpected(CascadeError::InvalidDepthRange);
    }
    if (config.cascadeCount < kMinCascadeCount || config.cascadeCount > kMaxCascadeCount)
    {
        return std::unexpected(CascadeError::InvalidCascadeCount);
    }

    CascadeSplits splits{};
    splits.cascadeCount = config.cascadeCount;
    for (std::uint32_t index = 0U; index <= config.cascadeCount; ++index)
    {
        std::expected<float, CascadeError> const boundary = ComputeSplitDistance(
            scheme, config.nearPlane, config.farPlane, index, config.cascadeCount, config.splitLambda);
        if (!boundary)
        {
            return std::unexpected(boundary.error());
        }
        splits.boundaries.at(index) = *boundary;
    }

    for (std::uint32_t index = 0U; index < config.cascadeCount; ++index)
    {
        if (!(splits.boundaries.at(index) < splits.boundaries.at(static_cast<std::size_t>(index) + 1U)))
        {
            return std::unexpected(CascadeError::InvalidDepthRange);
        }
    }
    return splits;
}

std::expected<CascadeInterval, CascadeError> CascadeIntervalAt(CascadeSplits const &splits,
                                                               std::uint32_t index) noexcept
{
    if (splits.cascadeCount < kMinCascadeCount || splits.cascadeCount > kMaxCascadeCount)
    {
        return std::unexpected(CascadeError::InvalidCascadeCount);
    }
    if (index >= splits.cascadeCount)
    {
        return std::unexpected(CascadeError::InvalidCascadeIndex);
    }
    float const nearDistance = splits.boundaries.at(index);
    float const farDistance = splits.boundaries.at(static_cast<std::size_t>(index) + 1U);
    if (!IsFinite(nearDistance) || !IsFinite(farDistance) || farDistance <= nearDistance)
    {
        return std::unexpected(CascadeError::InvalidDepthRange);
    }
    bool const closedFar = (index + 1U) == splits.cascadeCount;
    return CascadeInterval{nearDistance, farDistance, closedFar};
}

std::expected<OrthonormalBasis, CascadeError> BuildCameraBasis(Float3 forward, Float3 up) noexcept
{
    return BuildRightHandedFromForward(forward, up);
}

std::expected<std::array<Float3, kSliceCornerCount>, CascadeError> ComputeSliceCorners(CameraSlice const &camera,
                                                                                       float nearDistance,
                                                                                       float farDistance) noexcept
{
    if (!IsFinite(camera.position))
    {
        return std::unexpected(CascadeError::NonFiniteValue);
    }
    if (!ValidFieldOfView(camera.verticalFovRadians))
    {
        return std::unexpected(CascadeError::InvalidFieldOfView);
    }
    if (!ValidPositiveFinite(camera.aspectRatio))
    {
        return std::unexpected(CascadeError::InvalidAspectRatio);
    }
    if (!ValidPositiveFinite(nearDistance))
    {
        return std::unexpected(CascadeError::InvalidNearPlane);
    }
    if (!IsFinite(farDistance) || farDistance <= nearDistance)
    {
        return std::unexpected(CascadeError::InvalidDepthRange);
    }

    std::expected<OrthonormalBasis, CascadeError> const basis = BuildCameraBasis(camera.forward, camera.up);
    if (!basis)
    {
        return std::unexpected(basis.error());
    }

    double const tangent = std::tan(static_cast<double>(camera.verticalFovRadians) * 0.5);
    std::array<Float3, kSliceCornerCount> corners{};
    std::size_t cornerIndex = 0U;
    for (double const distance : {static_cast<double>(nearDistance), static_cast<double>(farDistance)})
    {
        double const halfHeight = distance * tangent;
        double const halfWidth = halfHeight * static_cast<double>(camera.aspectRatio);
        Float3 const center = Add(camera.position, Scale(basis->forward, distance));
        for (double const verticalSign : {-1.0, 1.0})
        {
            for (double const horizontalSign : {-1.0, 1.0})
            {
                Float3 const offset =
                    Add(Scale(basis->right, horizontalSign * halfWidth), Scale(basis->up, verticalSign * halfHeight));
                Float3 const corner = Add(center, offset);
                if (!IsFinite(corner))
                {
                    return std::unexpected(CascadeError::ArithmeticOverflow);
                }
                corners.at(cornerIndex) = corner;
                ++cornerIndex;
            }
        }
    }
    return corners;
}

std::expected<BoundingSphere, CascadeError> ComputeSliceBoundingSphere(CameraSlice const &camera, float nearDistance,
                                                                       float farDistance) noexcept
{
    if (!IsFinite(camera.position))
    {
        return std::unexpected(CascadeError::NonFiniteValue);
    }
    if (!ValidFieldOfView(camera.verticalFovRadians))
    {
        return std::unexpected(CascadeError::InvalidFieldOfView);
    }
    if (!ValidPositiveFinite(camera.aspectRatio))
    {
        return std::unexpected(CascadeError::InvalidAspectRatio);
    }
    if (!ValidPositiveFinite(nearDistance))
    {
        return std::unexpected(CascadeError::InvalidNearPlane);
    }
    if (!IsFinite(farDistance) || farDistance <= nearDistance)
    {
        return std::unexpected(CascadeError::InvalidDepthRange);
    }

    std::expected<OrthonormalBasis, CascadeError> const basis = BuildCameraBasis(camera.forward, camera.up);
    if (!basis)
    {
        return std::unexpected(basis.error());
    }

    double const near = static_cast<double>(nearDistance);
    double const far = static_cast<double>(farDistance);
    double const radialFactor =
        SliceRadialFactor(static_cast<double>(camera.verticalFovRadians), static_cast<double>(camera.aspectRatio));
    double const radialSquared = radialFactor * radialFactor;

    // The equidistant point minimizes the maximum distance to the near and far
    // corner rings unless it lies beyond the far ring. In that case the far
    // ring's own center is the minimum. Both depend only on scalar slice
    // geometry, preserving rotation and translation invariance.
    double const equidistantCenter = ((far + near) * (1.0 + radialSquared)) * 0.5;
    double const axialCenter = std::min(equidistantCenter, far);
    double const nearDistanceSquared = ((near - axialCenter) * (near - axialCenter)) + (near * near * radialSquared);
    double const farDistanceSquared = ((far - axialCenter) * (far - axialCenter)) + (far * far * radialSquared);
    double const radius = std::sqrt(std::max(nearDistanceSquared, farDistanceSquared));

    std::expected<float, CascadeError> const checkedRadius = CheckedFloat(radius);
    if (!checkedRadius)
    {
        return std::unexpected(checkedRadius.error());
    }
    Float3 const center = Add(camera.position, Scale(basis->forward, axialCenter));
    if (!IsFinite(center))
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }
    return BoundingSphere{center, *checkedRadius};
}

std::expected<OrthonormalBasis, CascadeError> BuildDirectionalLightBasis(Float3 directionToLight,
                                                                         Float3 upHint) noexcept
{
    std::expected<Float3, CascadeError> const direction =
        Normalize(directionToLight, CascadeError::ZeroLengthLightDirection);
    if (!direction)
    {
        return std::unexpected(direction.error());
    }
    return BuildRightHandedFromForward(Negate(*direction), upHint);
}

std::expected<LightView, CascadeError> BuildDirectionalLightView(Float3 directionToLight, Float3 upHint) noexcept
{
    std::expected<OrthonormalBasis, CascadeError> const basis = BuildDirectionalLightBasis(directionToLight, upHint);
    if (!basis)
    {
        return std::unexpected(basis.error());
    }
    Matrix4 const view{{{
        {basis->right.x, basis->up.x, basis->forward.x, 0.0F},
        {basis->right.y, basis->up.y, basis->forward.y, 0.0F},
        {basis->right.z, basis->up.z, basis->forward.z, 0.0F},
        {0.0F, 0.0F, 0.0F, 1.0F},
    }}};
    return LightView{*basis, view};
}

std::expected<Float4, CascadeError> TransformPoint(Float3 point, Matrix4 const &matrix) noexcept
{
    if (!IsFinite(point))
    {
        return std::unexpected(CascadeError::NonFiniteValue);
    }
    for (auto const &row : matrix.elements)
    {
        for (float const element : row)
        {
            if (!IsFinite(element))
            {
                return std::unexpected(CascadeError::NonFiniteValue);
            }
        }
    }
    std::array<double, 4U> const input{point.x, point.y, point.z, 1.0};
    Float4 output{};
    std::array<float *, 4U> const components{&output.x, &output.y, &output.z, &output.w};
    for (std::size_t column = 0U; column < 4U; ++column)
    {
        double value = 0.0;
        for (std::size_t row = 0U; row < 4U; ++row)
        {
            value += input.at(row) * static_cast<double>(matrix.elements.at(row).at(column));
        }
        std::expected<float, CascadeError> const checked = CheckedFloat(value);
        if (!checked)
        {
            return std::unexpected(checked.error());
        }
        *components.at(column) = *checked;
    }
    return output;
}

std::expected<Matrix4, CascadeError> Multiply(Matrix4 const &first, Matrix4 const &second) noexcept
{
    Matrix4 result{};
    for (std::size_t row = 0U; row < 4U; ++row)
    {
        for (std::size_t column = 0U; column < 4U; ++column)
        {
            double value = 0.0;
            for (std::size_t inner = 0U; inner < 4U; ++inner)
            {
                value += static_cast<double>(first.elements.at(row).at(inner)) *
                         static_cast<double>(second.elements.at(inner).at(column));
            }
            std::expected<float, CascadeError> const checked = CheckedFloat(value);
            if (!checked)
            {
                return std::unexpected(checked.error());
            }
            result.elements.at(row).at(column) = *checked;
        }
    }
    return result;
}

std::expected<Matrix4, CascadeError> BuildD3DOrthographicProjection(OrthographicExtents const &extents,
                                                                    DepthRange const &depthRange) noexcept
{
    if (!IsFinite(extents.left) || !IsFinite(extents.right) || !IsFinite(extents.bottom) || !IsFinite(extents.top))
    {
        return std::unexpected(CascadeError::NonFiniteValue);
    }
    if (!ValidExtents(extents))
    {
        return std::unexpected(CascadeError::InvalidExtents);
    }
    if (!IsFinite(depthRange.nearPlane) || !IsFinite(depthRange.farPlane))
    {
        return std::unexpected(CascadeError::NonFiniteValue);
    }
    if (!ValidDepthRange(depthRange))
    {
        return std::unexpected(CascadeError::InvalidDepthRange);
    }

    double const width = static_cast<double>(extents.right) - static_cast<double>(extents.left);
    double const height = static_cast<double>(extents.top) - static_cast<double>(extents.bottom);
    double const depth = static_cast<double>(depthRange.farPlane) - static_cast<double>(depthRange.nearPlane);
    std::array<std::expected<float, CascadeError>, 6U> const values{
        CheckedFloat(2.0 / width),
        CheckedFloat(2.0 / height),
        CheckedFloat(1.0 / depth),
        CheckedFloat(-(static_cast<double>(extents.left) + static_cast<double>(extents.right)) / width),
        CheckedFloat(-(static_cast<double>(extents.bottom) + static_cast<double>(extents.top)) / height),
        CheckedFloat(-static_cast<double>(depthRange.nearPlane) / depth),
    };
    if (std::ranges::any_of(values, [](std::expected<float, CascadeError> const &value) { return !value.has_value(); }))
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }

    return Matrix4{{{
        {*values.at(0), 0.0F, 0.0F, 0.0F},
        {0.0F, *values.at(1), 0.0F, 0.0F},
        {0.0F, 0.0F, *values.at(2), 0.0F},
        {*values.at(3), *values.at(4), *values.at(5), 1.0F},
    }}};
}

std::expected<TexelSnap, CascadeError> SnapToTexelGrid(Float2 center, float worldUnitsPerTexel) noexcept
{
    if (!IsFinite(center) || !IsFinite(worldUnitsPerTexel))
    {
        return std::unexpected(CascadeError::NonFiniteValue);
    }
    if (worldUnitsPerTexel <= 0.0F)
    {
        return std::unexpected(CascadeError::InvalidShadowResolution);
    }

    double const texel = static_cast<double>(worldUnitsPerTexel);
    double const snappedX = std::floor(static_cast<double>(center.x) / texel) * texel;
    double const snappedY = std::floor(static_cast<double>(center.y) / texel) * texel;
    std::expected<float, CascadeError> const outX = CheckedFloat(snappedX);
    std::expected<float, CascadeError> const outY = CheckedFloat(snappedY);
    std::expected<float, CascadeError> const offsetX = CheckedFloat(snappedX - static_cast<double>(center.x));
    std::expected<float, CascadeError> const offsetY = CheckedFloat(snappedY - static_cast<double>(center.y));
    if (!outX || !outY || !offsetX || !offsetY)
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }
    return TexelSnap{{*outX, *outY}, {*offsetX, *offsetY}};
}

std::expected<CascadeProjection, CascadeError> FitCascadeProjection(
    CameraSlice const &camera, CascadeInterval const &interval, Float3 directionToLight, std::uint32_t shadowResolution,
    float casterDepthPadding, float receiverDepthPadding, StabilizationMode mode, Float3 lightUpHint) noexcept
{
    if (shadowResolution == 0U)
    {
        return std::unexpected(CascadeError::InvalidShadowResolution);
    }
    if (!IsFinite(casterDepthPadding) || casterDepthPadding < 0.0F || !IsFinite(receiverDepthPadding) ||
        receiverDepthPadding < 0.0F)
    {
        return std::unexpected(CascadeError::InvalidPadding);
    }
    if (mode != StabilizationMode::None && mode != StabilizationMode::TexelSnappedSphere &&
        mode != StabilizationMode::TightAabbTexelSnapped)
    {
        return std::unexpected(CascadeError::InvalidStabilizationMode);
    }
    if (!ValidPositiveFinite(interval.nearDistance) || !IsFinite(interval.farDistance) ||
        interval.farDistance <= interval.nearDistance)
    {
        return std::unexpected(CascadeError::InvalidDepthRange);
    }

    std::expected<std::array<Float3, kSliceCornerCount>, CascadeError> const worldCorners =
        ComputeSliceCorners(camera, interval.nearDistance, interval.farDistance);
    if (!worldCorners)
    {
        return std::unexpected(worldCorners.error());
    }
    std::expected<LightView, CascadeError> const lightView = BuildDirectionalLightView(directionToLight, lightUpHint);
    if (!lightView)
    {
        return std::unexpected(lightView.error());
    }

    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double minZ = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    double maxZ = -std::numeric_limits<double>::infinity();
    for (Float3 const corner : *worldCorners)
    {
        std::expected<Float3, CascadeError> const lightSpace = TransformToFloat3(corner, lightView->view);
        if (!lightSpace)
        {
            return std::unexpected(lightSpace.error());
        }
        minX = std::min(minX, static_cast<double>(lightSpace->x));
        minY = std::min(minY, static_cast<double>(lightSpace->y));
        minZ = std::min(minZ, static_cast<double>(lightSpace->z));
        maxX = std::max(maxX, static_cast<double>(lightSpace->x));
        maxY = std::max(maxY, static_cast<double>(lightSpace->y));
        maxZ = std::max(maxZ, static_cast<double>(lightSpace->z));
    }

    std::expected<BoundingSphere, CascadeError> const sphere =
        ComputeSliceBoundingSphere(camera, interval.nearDistance, interval.farDistance);
    if (!sphere)
    {
        return std::unexpected(sphere.error());
    }
    std::expected<Float3, CascadeError> const sphereCenterLight = TransformToFloat3(sphere->center, lightView->view);
    if (!sphereCenterLight)
    {
        return std::unexpected(sphereCenterLight.error());
    }

    // Choose the light-space XY footprint. TexelSnappedSphere uses the
    // rotation-invariant bounding-sphere diameter (constant under camera
    // rotation); the other modes use the tight transformed-corner AABB.
    double widthX = 0.0;
    double widthY = 0.0;
    double minCornerX = 0.0;
    double minCornerY = 0.0;
    if (mode == StabilizationMode::TexelSnappedSphere)
    {
        widthX = 2.0 * static_cast<double>(sphere->radius);
        widthY = widthX;
        minCornerX = static_cast<double>(sphereCenterLight->x) - static_cast<double>(sphere->radius);
        minCornerY = static_cast<double>(sphereCenterLight->y) - static_cast<double>(sphere->radius);
    }
    else
    {
        widthX = maxX - minX;
        widthY = maxY - minY;
        minCornerX = minX;
        minCornerY = minY;
    }
    double const texelX = widthX / static_cast<double>(shadowResolution);
    double const texelY = widthY / static_cast<double>(shadowResolution);
    if (!(texelX > 0.0) || !(texelY > 0.0))
    {
        return std::unexpected(CascadeError::InvalidExtents);
    }

    // Snap the minimum corner (not the center) to the texel grid while keeping
    // the footprint width fixed. This keeps the shadow texels world-aligned, so
    // sub-texel camera translation reuses the identical projection.
    Float2 stabilizationOffset{0.0F, 0.0F};
    if (mode != StabilizationMode::None)
    {
        double const snappedX = std::floor(minCornerX / texelX) * texelX;
        double const snappedY = std::floor(minCornerY / texelY) * texelY;
        std::expected<float, CascadeError> const offsetX = CheckedFloat(snappedX - minCornerX);
        std::expected<float, CascadeError> const offsetY = CheckedFloat(snappedY - minCornerY);
        if (!offsetX || !offsetY)
        {
            return std::unexpected(CascadeError::ArithmeticOverflow);
        }
        stabilizationOffset = {*offsetX, *offsetY};
        minCornerX = snappedX;
        minCornerY = snappedY;
    }

    double const nearZ = minZ - static_cast<double>(casterDepthPadding);
    double const farZ = maxZ + static_cast<double>(receiverDepthPadding);
    std::array<std::expected<float, CascadeError>, 6U> const scalars{
        CheckedFloat(minCornerX), CheckedFloat(minCornerX + widthX),
        CheckedFloat(minCornerY), CheckedFloat(minCornerY + widthY),
        CheckedFloat(nearZ),      CheckedFloat(farZ),
    };
    if (std::ranges::any_of(scalars,
                            [](std::expected<float, CascadeError> const &value) { return !value.has_value(); }))
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }
    std::expected<float, CascadeError> const centerXChecked = CheckedFloat(minCornerX + (widthX * 0.5));
    std::expected<float, CascadeError> const centerYChecked = CheckedFloat(minCornerY + (widthY * 0.5));
    std::expected<float, CascadeError> const centerZChecked = CheckedFloat((minZ + maxZ) * 0.5);
    std::expected<float, CascadeError> const radiusChecked = CheckedFloat(static_cast<double>(sphere->radius));
    std::expected<float, CascadeError> const texelXChecked = CheckedFloat(texelX);
    std::expected<float, CascadeError> const texelYChecked = CheckedFloat(texelY);
    if (!centerXChecked || !centerYChecked || !centerZChecked || !radiusChecked || !texelXChecked || !texelYChecked)
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }

    OrthographicExtents const extents{*scalars.at(0), *scalars.at(1), *scalars.at(2), *scalars.at(3)};
    DepthRange const depthRange{*scalars.at(4), *scalars.at(5)};
    std::expected<Matrix4, CascadeError> const projection = BuildD3DOrthographicProjection(extents, depthRange);
    if (!projection)
    {
        return std::unexpected(projection.error());
    }
    std::expected<Matrix4, CascadeError> const lightViewProjection = Multiply(lightView->view, *projection);
    if (!lightViewProjection)
    {
        return std::unexpected(lightViewProjection.error());
    }

    CascadeProjection result{};
    result.extents = extents;
    result.depthRange = depthRange;
    result.lightView = lightView->view;
    result.lightViewProjection = *lightViewProjection;
    result.lightSpaceCenter = {*centerXChecked, *centerYChecked};
    result.lightSpaceCenterZ = *centerZChecked;
    result.boundingRadius = *radiusChecked;
    result.worldUnitsPerTexel = {*texelXChecked, *texelYChecked};
    result.stabilizationOffset = stabilizationOffset;
    result.mode = mode;
    result.usedFallbackUp = lightView->basis.usedFallbackUp;
    return result;
}

std::expected<CascadeSelection, CascadeError> SelectCascade(CascadeSplits const &splits, float viewDepth,
                                                            float blendFraction) noexcept
{
    if (splits.cascadeCount < kMinCascadeCount || splits.cascadeCount > kMaxCascadeCount)
    {
        return std::unexpected(CascadeError::InvalidCascadeCount);
    }
    if (!IsFinite(blendFraction) || blendFraction < 0.0F || blendFraction > 0.5F)
    {
        return std::unexpected(CascadeError::InvalidBlendFraction);
    }
    if (!IsFinite(viewDepth))
    {
        return std::unexpected(CascadeError::NonFiniteValue);
    }
    if (viewDepth <= 0.0F)
    {
        return std::unexpected(CascadeError::NonPositiveViewDepth);
    }
    std::uint32_t const count = splits.cascadeCount;
    for (std::uint32_t index = 0U; index <= count; ++index)
    {
        if (!IsFinite(splits.boundaries.at(index)))
        {
            return std::unexpected(CascadeError::NonFiniteValue);
        }
        if (index < count &&
            !(splits.boundaries.at(index) < splits.boundaries.at(static_cast<std::size_t>(index) + 1U)))
        {
            return std::unexpected(CascadeError::InvalidDepthRange);
        }
    }

    float const nearBound = splits.boundaries.at(0);
    float const farBound = splits.boundaries.at(count);

    CascadeSelection selection{};
    if (viewDepth < nearBound)
    {
        selection.primaryIndex = 0U;
        selection.secondaryIndex = 0U;
        selection.belowRange = true;
        return selection;
    }
    if (viewDepth > farBound)
    {
        selection.primaryIndex = count - 1U;
        selection.secondaryIndex = count - 1U;
        selection.aboveRange = true;
        return selection;
    }

    selection.inRange = true;
    std::uint32_t primary = count - 1U;
    for (std::uint32_t index = 0U; index + 1U < count; ++index)
    {
        if (viewDepth < splits.boundaries.at(static_cast<std::size_t>(index) + 1U))
        {
            primary = index;
            break;
        }
    }
    selection.primaryIndex = primary;
    selection.secondaryIndex = primary;

    bool const hasNext = (primary + 1U) < count;
    if (hasNext && blendFraction > 0.0F)
    {
        double const lower = static_cast<double>(splits.boundaries.at(primary));
        double const upper = static_cast<double>(splits.boundaries.at(static_cast<std::size_t>(primary) + 1U));
        double const width = (upper - lower) * static_cast<double>(blendFraction);
        double const bandStart = upper - width;
        std::expected<float, CascadeError> const widthChecked = CheckedFloat(width);
        if (!widthChecked)
        {
            return std::unexpected(widthChecked.error());
        }
        selection.transitionWidth = *widthChecked;
        if (static_cast<double>(viewDepth) >= bandStart)
        {
            double const weight = (static_cast<double>(viewDepth) - bandStart) / width;
            std::expected<float, CascadeError> const weightChecked = CheckedFloat(std::clamp(weight, 0.0, 1.0));
            if (!weightChecked)
            {
                return std::unexpected(weightChecked.error());
            }
            selection.secondaryIndex = primary + 1U;
            selection.blendWeight = *weightChecked;
        }
    }
    return selection;
}

std::expected<CascadeArraySlice, CascadeError> CascadeArraySliceForIndex(std::uint32_t index,
                                                                         std::uint32_t cascadeCount,
                                                                         std::uint32_t shadowResolution,
                                                                         std::uint32_t borderTexels) noexcept
{
    if (cascadeCount < kMinCascadeCount || cascadeCount > kMaxCascadeCount)
    {
        return std::unexpected(CascadeError::InvalidCascadeCount);
    }
    if (index >= cascadeCount)
    {
        return std::unexpected(CascadeError::InvalidCascadeIndex);
    }
    if (shadowResolution == 0U)
    {
        return std::unexpected(CascadeError::InvalidShadowResolution);
    }
    if ((static_cast<std::uint64_t>(borderTexels) * 2U) >= static_cast<std::uint64_t>(shadowResolution))
    {
        return std::unexpected(CascadeError::InvalidExtents);
    }

    double const resolution = static_cast<double>(shadowResolution);
    double const inset = static_cast<double>(borderTexels) / resolution;
    std::expected<float, CascadeError> const minValue = CheckedFloat(inset);
    std::expected<float, CascadeError> const maxValue = CheckedFloat(1.0 - inset);
    if (!minValue || !maxValue)
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }
    return CascadeArraySlice{index, {*minValue, *minValue}, {*maxValue, *maxValue}};
}

std::expected<Float2, CascadeError> CascadeTexelWorldSize(OrthographicExtents const &extents,
                                                          std::uint32_t shadowResolution) noexcept
{
    if (!IsFinite(extents.left) || !IsFinite(extents.right) || !IsFinite(extents.bottom) || !IsFinite(extents.top))
    {
        return std::unexpected(CascadeError::NonFiniteValue);
    }
    if (!ValidExtents(extents))
    {
        return std::unexpected(CascadeError::InvalidExtents);
    }
    if (shadowResolution == 0U)
    {
        return std::unexpected(CascadeError::InvalidShadowResolution);
    }
    std::expected<float, CascadeError> const width =
        CheckedFloat((static_cast<double>(extents.right) - static_cast<double>(extents.left)) /
                     static_cast<double>(shadowResolution));
    std::expected<float, CascadeError> const height =
        CheckedFloat((static_cast<double>(extents.top) - static_cast<double>(extents.bottom)) /
                     static_cast<double>(shadowResolution));
    if (!width || !height)
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }
    if (*width <= 0.0F || *height <= 0.0F)
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }
    return Float2{*width, *height};
}

std::expected<CascadeBiasScale, CascadeError> ScaleBiasForCascade(OrthographicExtents const &extents,
                                                                  std::uint32_t shadowResolution,
                                                                  float baseReceiverDepthBias,
                                                                  float baseNormalOffsetWorld,
                                                                  float referenceWorldUnitsPerTexel) noexcept
{
    if (!IsFinite(baseReceiverDepthBias) || !IsFinite(baseNormalOffsetWorld))
    {
        return std::unexpected(CascadeError::InvalidBiasParameters);
    }
    if (!IsFinite(referenceWorldUnitsPerTexel) || referenceWorldUnitsPerTexel <= 0.0F)
    {
        return std::unexpected(CascadeError::InvalidBiasParameters);
    }
    std::expected<Float2, CascadeError> const worldUnitsPerTexel = CascadeTexelWorldSize(extents, shadowResolution);
    if (!worldUnitsPerTexel)
    {
        return std::unexpected(worldUnitsPerTexel.error());
    }

    double const representative =
        std::max(static_cast<double>(worldUnitsPerTexel->x), static_cast<double>(worldUnitsPerTexel->y));
    double const scale = representative / static_cast<double>(referenceWorldUnitsPerTexel);
    std::expected<float, CascadeError> const scaleChecked = CheckedFloat(scale);
    std::expected<float, CascadeError> const receiverChecked =
        CheckedFloat(static_cast<double>(baseReceiverDepthBias) * scale);
    std::expected<float, CascadeError> const normalChecked =
        CheckedFloat(static_cast<double>(baseNormalOffsetWorld) * scale);
    if (!scaleChecked || !receiverChecked || !normalChecked)
    {
        return std::unexpected(CascadeError::ArithmeticOverflow);
    }
    return CascadeBiasScale{*worldUnitsPerTexel, *scaleChecked, *receiverChecked, *normalChecked};
}

std::expected<CascadeSetup, CascadeError> BuildCascadedShadowSetup(CascadeScene const &scene) noexcept
{
    std::expected<CascadeConfig, CascadeError> const validated = ValidateCascadeConfig(scene.config);
    if (!validated)
    {
        return std::unexpected(validated.error());
    }
    if (!IsFinite(scene.cameraPosition))
    {
        return std::unexpected(CascadeError::NonFiniteValue);
    }
    std::expected<CascadeSplits, CascadeError> const splits = ComputeCascadeSplits(scene.config, scene.splitScheme);
    if (!splits)
    {
        return std::unexpected(splits.error());
    }

    CameraSlice const camera{scene.cameraPosition, scene.cameraForward, scene.cameraUp, scene.config.verticalFovRadians,
                             scene.config.aspectRatio};

    CascadeSetup setup{};
    setup.cascadeCount = scene.config.cascadeCount;
    setup.splits = *splits;

    std::array<Float2, kMaxCascadeCount> worldUnitsPerTexel{};
    for (std::uint32_t index = 0U; index < scene.config.cascadeCount; ++index)
    {
        std::expected<CascadeInterval, CascadeError> const interval = CascadeIntervalAt(*splits, index);
        if (!interval)
        {
            return std::unexpected(interval.error());
        }
        std::expected<CascadeProjection, CascadeError> const projection =
            FitCascadeProjection(camera, *interval, scene.config.directionToLight, scene.config.shadowResolution,
                                 scene.config.casterDepthPadding, scene.config.receiverDepthPadding,
                                 scene.config.stabilization, scene.lightUpHint);
        if (!projection)
        {
            return std::unexpected(projection.error());
        }

        std::expected<std::array<Float3, kSliceCornerCount>, CascadeError> const corners =
            ComputeSliceCorners(camera, interval->nearDistance, interval->farDistance);
        if (!corners)
        {
            return std::unexpected(corners.error());
        }
        double minX = std::numeric_limits<double>::infinity();
        double minY = std::numeric_limits<double>::infinity();
        double maxX = -std::numeric_limits<double>::infinity();
        double maxY = -std::numeric_limits<double>::infinity();
        for (Float3 const corner : *corners)
        {
            std::expected<Float3, CascadeError> const lightSpace = TransformToFloat3(corner, projection->lightView);
            if (!lightSpace)
            {
                return std::unexpected(lightSpace.error());
            }
            minX = std::min(minX, static_cast<double>(lightSpace->x));
            minY = std::min(minY, static_cast<double>(lightSpace->y));
            maxX = std::max(maxX, static_cast<double>(lightSpace->x));
            maxY = std::max(maxY, static_cast<double>(lightSpace->y));
        }

        double const extentX =
            static_cast<double>(projection->extents.right) - static_cast<double>(projection->extents.left);
        double const extentY =
            static_cast<double>(projection->extents.top) - static_cast<double>(projection->extents.bottom);
        double const tightArea = std::max(0.0, maxX - minX) * std::max(0.0, maxY - minY);
        double const chosenArea = extentX * extentY;
        double utilization = 1.0;
        if (chosenArea > 0.0)
        {
            utilization = std::clamp(tightArea / chosenArea, 0.0, 1.0);
        }

        CascadeDiagnostics diagnostics{};
        diagnostics.splitNear = interval->nearDistance;
        diagnostics.splitFar = interval->farDistance;
        diagnostics.lightSpaceCenter = projection->lightSpaceCenter;
        diagnostics.xyExtent = {static_cast<float>(extentX), static_cast<float>(extentY)};
        diagnostics.zSpan = projection->depthRange.farPlane - projection->depthRange.nearPlane;
        diagnostics.worldUnitsPerTexel = projection->worldUnitsPerTexel;
        diagnostics.xyUtilization = static_cast<float>(utilization);
        diagnostics.wastedAreaProxy = static_cast<float>(1.0 - utilization);
        diagnostics.stabilizationOffset = projection->stabilizationOffset;
        diagnostics.usedFallbackUp = projection->usedFallbackUp;

        bool const isFinal = (index + 1U) == scene.config.cascadeCount;
        if (isFinal || scene.config.blendFraction <= 0.0F)
        {
            diagnostics.blendWidth = 0.0F;
            diagnostics.blendDisabled = true;
        }
        else
        {
            diagnostics.blendWidth = (interval->farDistance - interval->nearDistance) * scene.config.blendFraction;
            diagnostics.blendDisabled = false;
        }

        worldUnitsPerTexel.at(index) = projection->worldUnitsPerTexel;
        setup.cascades.at(index) = CascadeData{*interval, *projection, diagnostics};
    }

    for (std::uint32_t index = 0U; index < scene.config.cascadeCount; ++index)
    {
        bool const isFinal = (index + 1U) == scene.config.cascadeCount;
        if (isFinal)
        {
            setup.cascades.at(index).diagnostics.texelRatioToNext = 1.0F;
            continue;
        }
        double const current = std::max(static_cast<double>(worldUnitsPerTexel.at(index).x),
                                        static_cast<double>(worldUnitsPerTexel.at(index).y));
        double const next =
            std::max(static_cast<double>(worldUnitsPerTexel.at(static_cast<std::size_t>(index) + 1U).x),
                     static_cast<double>(worldUnitsPerTexel.at(static_cast<std::size_t>(index) + 1U).y));
        double ratio = 1.0;
        if (current > 0.0)
        {
            ratio = next / current;
        }
        std::expected<float, CascadeError> const ratioChecked = CheckedFloat(ratio);
        if (!ratioChecked)
        {
            return std::unexpected(ratioChecked.error());
        }
        setup.cascades.at(index).diagnostics.texelRatioToNext = *ratioChecked;
    }
    return setup;
}

} // namespace ch24::cascaded_shadows
