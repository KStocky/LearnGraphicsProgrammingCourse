#include "ShadowContracts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ch07::shadows
{
namespace
{

inline constexpr double kParallelThreshold = 0.999;

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

[[nodiscard]] bool IsFinite(Matrix4 const &value) noexcept
{
    for (auto const &row : value.elements)
    {
        for (float const element : row)
        {
            if (!IsFinite(element))
            {
                return false;
            }
        }
    }
    return true;
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

[[nodiscard]] std::expected<Float3, ShadowError> Normalize(Float3 value, ShadowError zeroError) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(ShadowError::NonFiniteValue);
    }
    double const lengthSquared = Dot(value, value);
    if (!std::isfinite(lengthSquared))
    {
        return std::unexpected(ShadowError::ArithmeticOverflow);
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
        return std::unexpected(ShadowError::ArithmeticOverflow);
    }
    return result;
}

[[nodiscard]] std::expected<float, ShadowError> CheckedFloat(double value) noexcept
{
    if (!std::isfinite(value) || value > static_cast<double>((std::numeric_limits<float>::max)()) ||
        value < -static_cast<double>((std::numeric_limits<float>::max)()))
    {
        return std::unexpected(ShadowError::ArithmeticOverflow);
    }
    return static_cast<float>(value);
}

[[nodiscard]] bool ValidBounds(AxisAlignedBounds const &bounds) noexcept
{
    return IsFinite(bounds.minimum) && IsFinite(bounds.maximum) && bounds.minimum.x < bounds.maximum.x &&
           bounds.minimum.y < bounds.maximum.y && bounds.minimum.z < bounds.maximum.z;
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

[[nodiscard]] Float3 Negate(Float3 value) noexcept
{
    return {-value.x, -value.y, -value.z};
}

[[nodiscard]] FrustumBoundary ClassifyAxis(float value, float minimum, float maximum, float tolerance,
                                           FrustumBoundary minimumBoundary, FrustumBoundary maximumBoundary,
                                           FrustumBoundary &outside) noexcept
{
    FrustumBoundary boundary = FrustumBoundary::None;
    if (value < minimum)
    {
        outside = outside | minimumBoundary;
    }
    else if ((value - minimum) <= tolerance)
    {
        boundary = boundary | minimumBoundary;
    }

    if (value > maximum)
    {
        outside = outside | maximumBoundary;
    }
    else if ((maximum - value) <= tolerance)
    {
        boundary = boundary | maximumBoundary;
    }
    return boundary;
}

[[nodiscard]] bool HasWellConditionedHomogeneousW(Float3 point, Matrix4 const &matrix, float w) noexcept
{
    std::array<double, 4U> const input{
        static_cast<double>(point.x),
        static_cast<double>(point.y),
        static_cast<double>(point.z),
        1.0,
    };
    double magnitude = 0.0;
    for (std::size_t row = 0U; row < 4U; ++row)
    {
        magnitude += std::fabs(input[row] * static_cast<double>(matrix.elements[row][3]));
    }
    return std::fabs(static_cast<double>(w)) > static_cast<double>((std::numeric_limits<float>::epsilon)()) * magnitude;
}

} // namespace

std::expected<DirectionalLightBasis, ShadowError> BuildDirectionalLightBasis(Float3 directionToLight,
                                                                             Float3 upHint) noexcept
{
    std::expected<Float3, ShadowError> const direction = Normalize(directionToLight, ShadowError::ZeroLengthDirection);
    if (!direction)
    {
        return std::unexpected(direction.error());
    }
    std::expected<Float3, ShadowError> normalizedUp = Normalize(upHint, ShadowError::DegenerateBasis);
    if (!normalizedUp)
    {
        return std::unexpected(normalizedUp.error());
    }

    Float3 const forward = Negate(*direction);
    bool const needsFallback = std::fabs(Dot(*normalizedUp, forward)) >= kParallelThreshold;
    if (needsFallback)
    {
        std::array<Float3, 3U> const axes{Float3{1.0F, 0.0F, 0.0F}, Float3{0.0F, 1.0F, 0.0F}, Float3{0.0F, 0.0F, 1.0F}};
        normalizedUp =
            *std::ranges::min_element(axes, [forward](Float3 first, Float3 second)
                                      { return std::fabs(Dot(first, forward)) < std::fabs(Dot(second, forward)); });
    }

    std::expected<Float3, ShadowError> const right =
        Normalize(Cross(*normalizedUp, forward), ShadowError::DegenerateBasis);
    if (!right)
    {
        return std::unexpected(right.error());
    }
    std::expected<Float3, ShadowError> const up = Normalize(Cross(forward, *right), ShadowError::DegenerateBasis);
    if (!up)
    {
        return std::unexpected(up.error());
    }

    return DirectionalLightBasis{*right, *up, forward, needsFallback};
}

std::expected<LightViewResult, ShadowError> BuildDirectionalLightView(DirectionalLightViewInput const &input) noexcept
{
    if (!IsFinite(input.position))
    {
        return std::unexpected(ShadowError::NonFiniteValue);
    }
    std::expected<DirectionalLightBasis, ShadowError> const basis =
        BuildDirectionalLightBasis(input.directionToLight, input.upHint);
    if (!basis)
    {
        return std::unexpected(basis.error());
    }

    std::expected<float, ShadowError> const translateRight = CheckedFloat(-Dot(input.position, basis->right));
    std::expected<float, ShadowError> const translateUp = CheckedFloat(-Dot(input.position, basis->up));
    std::expected<float, ShadowError> const translateForward = CheckedFloat(-Dot(input.position, basis->forward));
    if (!translateRight || !translateUp || !translateForward)
    {
        return std::unexpected(ShadowError::ArithmeticOverflow);
    }

    Matrix4 const view{{{
        {basis->right.x, basis->up.x, basis->forward.x, 0.0F},
        {basis->right.y, basis->up.y, basis->forward.y, 0.0F},
        {basis->right.z, basis->up.z, basis->forward.z, 0.0F},
        {*translateRight, *translateUp, *translateForward, 1.0F},
    }}};
    return LightViewResult{*basis, view};
}

std::expected<std::array<Float3, 8U>, ShadowError> BoundsCorners(AxisAlignedBounds const &bounds) noexcept
{
    if (!IsFinite(bounds.minimum) || !IsFinite(bounds.maximum))
    {
        return std::unexpected(ShadowError::NonFiniteValue);
    }
    if (!ValidBounds(bounds))
    {
        return std::unexpected(ShadowError::InvalidBounds);
    }
    return std::array<Float3, 8U>{
        Float3{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z},
        Float3{bounds.maximum.x, bounds.minimum.y, bounds.minimum.z},
        Float3{bounds.minimum.x, bounds.maximum.y, bounds.minimum.z},
        Float3{bounds.maximum.x, bounds.maximum.y, bounds.minimum.z},
        Float3{bounds.minimum.x, bounds.minimum.y, bounds.maximum.z},
        Float3{bounds.maximum.x, bounds.minimum.y, bounds.maximum.z},
        Float3{bounds.minimum.x, bounds.maximum.y, bounds.maximum.z},
        Float3{bounds.maximum.x, bounds.maximum.y, bounds.maximum.z},
    };
}

std::expected<Float4, ShadowError> TransformPoint(Float3 point, Matrix4 const &matrix) noexcept
{
    if (!IsFinite(point) || !IsFinite(matrix))
    {
        return std::unexpected(ShadowError::NonFiniteValue);
    }
    std::array<double, 4U> const input{point.x, point.y, point.z, 1.0};
    Float4 output{};
    std::array<float *, 4U> const components{&output.x, &output.y, &output.z, &output.w};
    for (std::size_t column = 0U; column < 4U; ++column)
    {
        double value = 0.0;
        for (std::size_t row = 0U; row < 4U; ++row)
        {
            value += input[row] * static_cast<double>(matrix.elements[row][column]);
        }
        std::expected<float, ShadowError> const checked = CheckedFloat(value);
        if (!checked)
        {
            return std::unexpected(checked.error());
        }
        *components[column] = *checked;
    }
    return output;
}

std::expected<AxisAlignedBounds, ShadowError> TransformBounds(AxisAlignedBounds const &bounds,
                                                              Matrix4 const &matrix) noexcept
{
    std::expected<std::array<Float3, 8U>, ShadowError> const corners = BoundsCorners(bounds);
    if (!corners)
    {
        return std::unexpected(corners.error());
    }
    if (!IsFinite(matrix))
    {
        return std::unexpected(ShadowError::NonFiniteValue);
    }

    AxisAlignedBounds transformed{
        {(std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)()},
        {(std::numeric_limits<float>::lowest)(), (std::numeric_limits<float>::lowest)(),
         (std::numeric_limits<float>::lowest)()},
    };
    for (Float3 const corner : *corners)
    {
        std::expected<Float4, ShadowError> const point = TransformPoint(corner, matrix);
        if (!point)
        {
            return std::unexpected(point.error());
        }
        if (point->w != 1.0F)
        {
            return std::unexpected(ShadowError::InvalidHomogeneousCoordinate);
        }
        transformed.minimum.x = std::min(transformed.minimum.x, point->x);
        transformed.minimum.y = std::min(transformed.minimum.y, point->y);
        transformed.minimum.z = std::min(transformed.minimum.z, point->z);
        transformed.maximum.x = std::max(transformed.maximum.x, point->x);
        transformed.maximum.y = std::max(transformed.maximum.y, point->y);
        transformed.maximum.z = std::max(transformed.maximum.z, point->z);
    }
    return transformed;
}

std::expected<Matrix4, ShadowError> BuildD3DOrthographicProjection(OrthographicExtents const &extents,
                                                                   DepthRange const &depthRange) noexcept
{
    if (!IsFinite(extents.left) || !IsFinite(extents.right) || !IsFinite(extents.bottom) || !IsFinite(extents.top))
    {
        return std::unexpected(ShadowError::NonFiniteValue);
    }
    if (!ValidExtents(extents))
    {
        return std::unexpected(ShadowError::InvalidOrthographicExtents);
    }
    if (!IsFinite(depthRange.nearPlane) || !IsFinite(depthRange.farPlane))
    {
        return std::unexpected(ShadowError::NonFiniteValue);
    }
    if (!ValidDepthRange(depthRange))
    {
        return std::unexpected(ShadowError::InvalidDepthRange);
    }

    double const width = static_cast<double>(extents.right) - static_cast<double>(extents.left);
    double const height = static_cast<double>(extents.top) - static_cast<double>(extents.bottom);
    double const depth = static_cast<double>(depthRange.farPlane) - static_cast<double>(depthRange.nearPlane);
    std::array<std::expected<float, ShadowError>, 6U> const values{
        CheckedFloat(2.0 / width),
        CheckedFloat(2.0 / height),
        CheckedFloat(1.0 / depth),
        CheckedFloat(-(static_cast<double>(extents.left) + static_cast<double>(extents.right)) / width),
        CheckedFloat(-(static_cast<double>(extents.bottom) + static_cast<double>(extents.top)) / height),
        CheckedFloat(-static_cast<double>(depthRange.nearPlane) / depth),
    };
    if (std::ranges::any_of(values, [](auto const &value) { return !value.has_value(); }))
    {
        return std::unexpected(ShadowError::ArithmeticOverflow);
    }

    return Matrix4{{{
        {*values[0], 0.0F, 0.0F, 0.0F},
        {0.0F, *values[1], 0.0F, 0.0F},
        {0.0F, 0.0F, *values[2], 0.0F},
        {*values[3], *values[4], *values[5], 1.0F},
    }}};
}

std::expected<Matrix4, ShadowError> Multiply(Matrix4 const &first, Matrix4 const &second) noexcept
{
    if (!IsFinite(first) || !IsFinite(second))
    {
        return std::unexpected(ShadowError::NonFiniteValue);
    }
    Matrix4 result{};
    for (std::size_t row = 0U; row < 4U; ++row)
    {
        for (std::size_t column = 0U; column < 4U; ++column)
        {
            double value = 0.0;
            for (std::size_t inner = 0U; inner < 4U; ++inner)
            {
                value += static_cast<double>(first.elements[row][inner]) *
                         static_cast<double>(second.elements[inner][column]);
            }
            std::expected<float, ShadowError> const checked = CheckedFloat(value);
            if (!checked)
            {
                return std::unexpected(checked.error());
            }
            result.elements[row][column] = *checked;
        }
    }
    return result;
}

std::expected<ProjectionResult, ShadowError> ProjectWorldToShadow(Float3 worldPoint, Matrix4 const &lightViewProjection,
                                                                  float boundaryTolerance) noexcept
{
    if (!IsFinite(boundaryTolerance) || boundaryTolerance < 0.0F)
    {
        return std::unexpected(ShadowError::NonFiniteValue);
    }
    std::expected<Float4, ShadowError> const clip = TransformPoint(worldPoint, lightViewProjection);
    if (!clip)
    {
        return std::unexpected(clip.error());
    }
    if (!HasWellConditionedHomogeneousW(worldPoint, lightViewProjection, clip->w))
    {
        return std::unexpected(ShadowError::InvalidHomogeneousCoordinate);
    }

    double const reciprocalW = 1.0 / static_cast<double>(clip->w);
    std::expected<float, ShadowError> const ndcX = CheckedFloat(static_cast<double>(clip->x) * reciprocalW);
    std::expected<float, ShadowError> const ndcY = CheckedFloat(static_cast<double>(clip->y) * reciprocalW);
    std::expected<float, ShadowError> const ndcZ = CheckedFloat(static_cast<double>(clip->z) * reciprocalW);
    if (!ndcX || !ndcY || !ndcZ)
    {
        return std::unexpected(ShadowError::ArithmeticOverflow);
    }

    ProjectionResult result{};
    result.clip = *clip;
    result.ndc = {*ndcX, *ndcY, *ndcZ};
    result.uv = {(*ndcX * 0.5F) + 0.5F, 0.5F - (*ndcY * 0.5F)};
    result.depth = *ndcZ;
    result.boundaryMask = ClassifyAxis(*ndcX, -1.0F, 1.0F, boundaryTolerance, FrustumBoundary::Left,
                                       FrustumBoundary::Right, result.outsideMask) |
                          ClassifyAxis(*ndcY, -1.0F, 1.0F, boundaryTolerance, FrustumBoundary::Bottom,
                                       FrustumBoundary::Top, result.outsideMask) |
                          ClassifyAxis(*ndcZ, 0.0F, 1.0F, boundaryTolerance, FrustumBoundary::Near,
                                       FrustumBoundary::Far, result.outsideMask);
    if (result.outsideMask != FrustumBoundary::None)
    {
        result.classification = FrustumClassification::Outside;
    }
    else if (result.boundaryMask != FrustumBoundary::None)
    {
        result.classification = FrustumClassification::OnBoundary;
    }
    else
    {
        result.classification = FrustumClassification::Inside;
    }
    if (!IsFinite(result.uv))
    {
        return std::unexpected(ShadowError::ArithmeticOverflow);
    }
    return result;
}

std::expected<Float2, ShadowError> WorldTexelSize(OrthographicExtents const &extents,
                                                  ShadowMapDimensions dimensions) noexcept
{
    if (!IsFinite(extents.left) || !IsFinite(extents.right) || !IsFinite(extents.bottom) || !IsFinite(extents.top))
    {
        return std::unexpected(ShadowError::NonFiniteValue);
    }
    if (!ValidExtents(extents))
    {
        return std::unexpected(ShadowError::InvalidOrthographicExtents);
    }
    if (dimensions.width == 0U || dimensions.height == 0U)
    {
        return std::unexpected(ShadowError::InvalidMapDimensions);
    }
    std::expected<float, ShadowError> const x =
        CheckedFloat((static_cast<double>(extents.right) - static_cast<double>(extents.left)) /
                     static_cast<double>(dimensions.width));
    std::expected<float, ShadowError> const y =
        CheckedFloat((static_cast<double>(extents.top) - static_cast<double>(extents.bottom)) /
                     static_cast<double>(dimensions.height));
    if (!x || !y)
    {
        return std::unexpected(ShadowError::ArithmeticOverflow);
    }
    if (*x <= 0.0F || *y <= 0.0F)
    {
        return std::unexpected(ShadowError::ArithmeticOverflow);
    }
    return Float2{*x, *y};
}

std::expected<SlopeResult, ShadowError> ComputeSlopeMetric(Float3 worldNormal, Float3 directionToLight,
                                                           SlopeParameters const &parameters) noexcept
{
    if (!IsFinite(parameters.minimumAbsCosine) || !IsFinite(parameters.maximumSlope) ||
        parameters.minimumAbsCosine <= 0.0F || parameters.minimumAbsCosine > 1.0F || parameters.maximumSlope < 0.0F)
    {
        return std::unexpected(ShadowError::InvalidSlopeParameters);
    }
    std::expected<Float3, ShadowError> const normal = Normalize(worldNormal, ShadowError::ZeroLengthDirection);
    std::expected<Float3, ShadowError> const light = Normalize(directionToLight, ShadowError::ZeroLengthDirection);
    if (!normal)
    {
        return std::unexpected(normal.error());
    }
    if (!light)
    {
        return std::unexpected(light.error());
    }

    double const absCosine = std::clamp(std::fabs(Dot(*normal, *light)), 0.0, 1.0);
    double const sine = std::sqrt(std::max(0.0, 1.0 - (absCosine * absCosine)));
    double const denominator = std::max(absCosine, static_cast<double>(parameters.minimumAbsCosine));
    double const unclamped = sine / denominator;
    std::expected<float, ShadowError> const checkedCosine = CheckedFloat(absCosine);
    std::expected<float, ShadowError> const checkedSlope = CheckedFloat(unclamped);
    if (!checkedCosine || !checkedSlope)
    {
        return std::unexpected(ShadowError::ArithmeticOverflow);
    }
    float const slope = std::min(*checkedSlope, parameters.maximumSlope);
    return SlopeResult{*checkedCosine, *checkedSlope, slope, slope < *checkedSlope};
}

std::expected<RasterBiasResult, ShadowError> ComputeRasterBias(RasterBiasParameters const &parameters,
                                                               float slope) noexcept
{
    if (!IsFinite(parameters.constantDepthBias) || !IsFinite(parameters.slopeScaledDepthBias) ||
        !IsFinite(parameters.depthBiasClamp) || !IsFinite(slope) || slope < 0.0F)
    {
        return std::unexpected(ShadowError::InvalidBiasParameters);
    }
    std::expected<float, ShadowError> const slopeContribution =
        CheckedFloat(static_cast<double>(parameters.slopeScaledDepthBias) * static_cast<double>(slope));
    if (!slopeContribution)
    {
        return std::unexpected(slopeContribution.error());
    }
    std::expected<float, ShadowError> const unclamped =
        CheckedFloat(static_cast<double>(parameters.constantDepthBias) + static_cast<double>(*slopeContribution));
    if (!unclamped)
    {
        return std::unexpected(unclamped.error());
    }

    float applied = *unclamped;
    if (parameters.depthBiasClamp > 0.0F)
    {
        applied = std::min(applied, parameters.depthBiasClamp);
    }
    else if (parameters.depthBiasClamp < 0.0F)
    {
        applied = std::max(applied, parameters.depthBiasClamp);
    }
    return RasterBiasResult{parameters.constantDepthBias, *slopeContribution, *unclamped, applied,
                            applied != *unclamped};
}

std::expected<ReceiverBiasResult, ShadowError> ComputeReceiverBias(ReceiverInput const &receiver,
                                                                   ReceiverBiasParameters const &parameters,
                                                                   Matrix4 const &lightViewProjection) noexcept
{
    if (!IsFinite(receiver.worldPosition) || !IsFinite(parameters.receiverDepthBias) ||
        !IsFinite(parameters.normalOffsetWorld))
    {
        return std::unexpected(ShadowError::NonFiniteValue);
    }
    std::expected<Float3, ShadowError> const normal = Normalize(receiver.worldNormal, ShadowError::ZeroLengthDirection);
    if (!normal)
    {
        return std::unexpected(normal.error());
    }

    Float3 const offset{
        normal->x * parameters.normalOffsetWorld,
        normal->y * parameters.normalOffsetWorld,
        normal->z * parameters.normalOffsetWorld,
    };
    Float3 const offsetPosition{
        receiver.worldPosition.x + offset.x,
        receiver.worldPosition.y + offset.y,
        receiver.worldPosition.z + offset.z,
    };
    if (!IsFinite(offset) || !IsFinite(offsetPosition))
    {
        return std::unexpected(ShadowError::ArithmeticOverflow);
    }

    std::expected<ProjectionResult, ShadowError> const original =
        ProjectWorldToShadow(receiver.worldPosition, lightViewProjection);
    std::expected<ProjectionResult, ShadowError> const displaced =
        ProjectWorldToShadow(offsetPosition, lightViewProjection);
    if (!original)
    {
        return std::unexpected(original.error());
    }
    if (!displaced)
    {
        return std::unexpected(displaced.error());
    }
    std::expected<float, ShadowError> const comparisonDepth =
        CheckedFloat(static_cast<double>(displaced->depth) - static_cast<double>(parameters.receiverDepthBias));
    if (!comparisonDepth)
    {
        return std::unexpected(comparisonDepth.error());
    }
    std::expected<float, ShadowError> const normalOffsetDepthContribution =
        CheckedFloat(static_cast<double>(displaced->depth) - static_cast<double>(original->depth));
    if (!normalOffsetDepthContribution)
    {
        return std::unexpected(normalOffsetDepthContribution.error());
    }
    return ReceiverBiasResult{
        offset,
        offsetPosition,
        original->depth,
        displaced->depth,
        *normalOffsetDepthContribution,
        -parameters.receiverDepthBias,
        *comparisonDepth,
    };
}

std::expected<ShadowComparisonResult, ShadowError> CompareShadowDepthLessEqual(
    ProjectionResult const &receiverProjection, float storedDepth, float receiverDepthBias) noexcept
{
    if (!IsFinite(receiverProjection.depth) || !IsFinite(storedDepth) || !IsFinite(receiverDepthBias))
    {
        return std::unexpected(ShadowError::NonFiniteValue);
    }
    if (receiverProjection.classification == FrustumClassification::Outside)
    {
        return ShadowComparisonResult{true, false, receiverProjection.depth, storedDepth};
    }
    std::expected<float, ShadowError> const comparisonDepth =
        CheckedFloat(static_cast<double>(receiverProjection.depth) - static_cast<double>(receiverDepthBias));
    if (!comparisonDepth)
    {
        return std::unexpected(comparisonDepth.error());
    }
    return ShadowComparisonResult{*comparisonDepth <= storedDepth, true, *comparisonDepth, storedDepth};
}

} // namespace ch07::shadows
