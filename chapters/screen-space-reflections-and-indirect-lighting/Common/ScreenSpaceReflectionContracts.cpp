#include "ScreenSpaceReflectionContracts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>

namespace ch29::screen_space_reflections
{
namespace
{

[[nodiscard]] bool IsFinite(double value) noexcept
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

[[nodiscard]] bool IsFinite(Rgb value) noexcept
{
    return IsFinite(value.r) && IsFinite(value.g) && IsFinite(value.b);
}

[[nodiscard]] double Dot(Float3 left, Float3 right) noexcept
{
    return (left.x * right.x) + (left.y * right.y) + (left.z * right.z);
}

[[nodiscard]] double Length(Float3 value) noexcept
{
    return std::hypot(value.x, value.y, value.z);
}

[[nodiscard]] Float3 Scale(Float3 value, double factor) noexcept
{
    return {.x = value.x * factor, .y = value.y * factor, .z = value.z * factor};
}

[[nodiscard]] Float3 Add(Float3 left, Float3 right) noexcept
{
    return {.x = left.x + right.x, .y = left.y + right.y, .z = left.z + right.z};
}

[[nodiscard]] Float3 Subtract(Float3 left, Float3 right) noexcept
{
    return {.x = left.x - right.x, .y = left.y - right.y, .z = left.z - right.z};
}

[[nodiscard]] double Lerp(double from, double to, double parameter) noexcept
{
    return ((1.0 - parameter) * from) + (parameter * to);
}

[[nodiscard]] double Saturate(double value) noexcept
{
    return std::clamp(value, 0.0, 1.0);
}

// Linear fade from one at start to zero at end. The caller guarantees end > start.
[[nodiscard]] double FadeOut(double value, double start, double end) noexcept
{
    return 1.0 - Saturate((value - start) / (end - start));
}

[[nodiscard]] std::expected<void, ContractError> ValidateUnitVector(Float3 value) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (std::abs(Length(value) - 1.0) > kUnitLengthTolerance)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateUnitNormal(Float3 value) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (std::abs(Length(value) - 1.0) > kUnitLengthTolerance)
    {
        return std::unexpected(ContractError::InvalidNormal);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateExtent(Extent2D extent) noexcept
{
    if (extent.width == 0U || extent.height == 0U || extent.width > kMaximumDimension ||
        extent.height > kMaximumDimension)
    {
        return std::unexpected(ContractError::InvalidExtent);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateUnitUv(Float2 uv) noexcept
{
    if (!IsFinite(uv))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    {
        return std::unexpected(ContractError::InvalidUv);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateRadiance(Rgb value) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(ContractError::InvalidRadiance);
    }
    if (value.r < 0.0 || value.g < 0.0 || value.b < 0.0)
    {
        return std::unexpected(ContractError::NegativeRadiance);
    }
    if (value.r > kMaximumSceneLinearValue || value.g > kMaximumSceneLinearValue || value.b > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidRadiance);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateReflectance(Rgb value) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(ContractError::InvalidAlbedo);
    }
    if (value.r < 0.0 || value.g < 0.0 || value.b < 0.0 || value.r > 1.0 || value.g > 1.0 || value.b > 1.0)
    {
        return std::unexpected(ContractError::InvalidAlbedo);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateUnitInterval(double value, ContractError error) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (value < 0.0 || value > 1.0)
    {
        return std::unexpected(error);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateRayConstructionSettings(
    RayConstructionSettings const &settings) noexcept
{
    if (!IsFinite(settings.constantNormalBias) || !IsFinite(settings.depthProportionalNormalBias) ||
        !IsFinite(settings.minimumFacingCosine) || !IsFinite(settings.maximumRayDistance))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.constantNormalBias < 0.0 || settings.depthProportionalNormalBias < 0.0 ||
        settings.minimumFacingCosine <= 0.0 || settings.minimumFacingCosine > 1.0 ||
        settings.maximumRayDistance <= 0.0 || settings.maximumRayDistance > kMaximumViewDistance)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateTraceSettings(TraceSettings const &settings) noexcept
{
    if (!IsFinite(settings.stepLengthTexels) || !IsFinite(settings.startOffsetFraction) ||
        !IsFinite(settings.constantThickness) || !IsFinite(settings.depthProportionalThickness))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.stepLengthTexels <= 0.0 || settings.maximumStepCount == 0U ||
        settings.maximumStepCount > kMaximumTraversalStepCount || settings.startOffsetFraction < 0.0 ||
        settings.startOffsetFraction >= 1.0 || settings.constantThickness < 0.0 ||
        settings.depthProportionalThickness < 0.0 || settings.refinementStepCount > kMaximumRefinementStepCount)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    if (settings.constantThickness == 0.0 && settings.depthProportionalThickness == 0.0)
    {
        // A zero-width thickness interval can only accept an exact floating-point coincidence, which is never a
        // teachable outcome. Requiring a positive interval forces the assumption to be stated.
        return std::unexpected(ContractError::InvalidSettings);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateConfidenceSettings(ConfidenceSettings const &settings) noexcept
{
    if (!IsFinite(settings.screenEdgeFadeUv) || !IsFinite(settings.distanceFadeStartFraction) ||
        !IsFinite(settings.thicknessFadeFraction) || !IsFinite(settings.roughnessFadeStart) ||
        !IsFinite(settings.roughnessFadeEnd) || !IsFinite(settings.towardCameraFadeStart) ||
        !IsFinite(settings.towardCameraFadeEnd))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.screenEdgeFadeUv <= 0.0 || settings.screenEdgeFadeUv > 0.5 ||
        settings.distanceFadeStartFraction < 0.0 || settings.distanceFadeStartFraction >= 1.0 ||
        settings.thicknessFadeFraction <= 0.0 || settings.thicknessFadeFraction > 1.0 ||
        settings.roughnessFadeStart < 0.0 || settings.roughnessFadeEnd > 1.0 ||
        settings.roughnessFadeEnd <= settings.roughnessFadeStart || settings.towardCameraFadeStart < -1.0 ||
        settings.towardCameraFadeEnd > 1.0 || settings.towardCameraFadeEnd <= settings.towardCameraFadeStart)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    return {};
}

struct FrustumPlane final
{
    Float3 normal{};
    double offset{};
    ClipLimit limit{ClipLimit::NearPlane};
};

[[nodiscard]] std::array<FrustumPlane, 6U> BuildFrustumPlanes(PerspectiveProjection projection,
                                                              FrustumTangents tangents) noexcept
{
    return {
        FrustumPlane{
            .normal = {.x = 0.0, .y = 0.0, .z = 1.0}, .offset = -projection.nearPlane, .limit = ClipLimit::NearPlane},
        FrustumPlane{
            .normal = {.x = 0.0, .y = 0.0, .z = -1.0}, .offset = projection.farPlane, .limit = ClipLimit::FarPlane},
        FrustumPlane{
            .normal = {.x = 1.0, .y = 0.0, .z = tangents.horizontal}, .offset = 0.0, .limit = ClipLimit::LeftPlane},
        FrustumPlane{
            .normal = {.x = -1.0, .y = 0.0, .z = tangents.horizontal}, .offset = 0.0, .limit = ClipLimit::RightPlane},
        FrustumPlane{
            .normal = {.x = 0.0, .y = 1.0, .z = tangents.vertical}, .offset = 0.0, .limit = ClipLimit::BottomPlane},
        FrustumPlane{
            .normal = {.x = 0.0, .y = -1.0, .z = tangents.vertical}, .offset = 0.0, .limit = ClipLimit::TopPlane},
    };
}

[[nodiscard]] TraceResult MakeMiss(MissReason reason) noexcept
{
    return TraceResult{.hit = false, .missReason = reason};
}

} // namespace

std::expected<void, ContractError> ValidateProjection(PerspectiveProjection projection) noexcept
{
    if (!IsFinite(projection.verticalFieldOfViewRadians) || !IsFinite(projection.aspectRatio) ||
        !IsFinite(projection.nearPlane) || !IsFinite(projection.farPlane))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (projection.verticalFieldOfViewRadians <= 0.0 || projection.verticalFieldOfViewRadians >= std::numbers::pi ||
        projection.aspectRatio <= 0.0 || projection.nearPlane <= 0.0 || projection.farPlane <= projection.nearPlane ||
        projection.farPlane > kMaximumViewDistance)
    {
        return std::unexpected(ContractError::InvalidProjection);
    }
    if (projection.depthConvention != DepthConvention::Forward &&
        projection.depthConvention != DepthConvention::Reversed)
    {
        return std::unexpected(ContractError::InvalidProjection);
    }
    return {};
}

std::expected<DeviceDepthCoefficients, ContractError> MakeDeviceDepthCoefficients(
    PerspectiveProjection projection) noexcept
{
    auto const valid = ValidateProjection(projection);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }

    double const range = projection.farPlane - projection.nearPlane;
    if (projection.depthConvention == DepthConvention::Forward)
    {
        return DeviceDepthCoefficients{
            .additive = projection.farPlane / range,
            .reciprocal = -(projection.nearPlane * projection.farPlane) / range,
        };
    }
    return DeviceDepthCoefficients{
        .additive = -projection.nearPlane / range,
        .reciprocal = (projection.nearPlane * projection.farPlane) / range,
    };
}

std::expected<FrustumTangents, ContractError> ViewFrustumTangents(PerspectiveProjection projection) noexcept
{
    auto const valid = ValidateProjection(projection);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }

    double const vertical = std::tan(projection.verticalFieldOfViewRadians * 0.5);
    double const horizontal = vertical * projection.aspectRatio;
    if (!IsFinite(vertical) || !IsFinite(horizontal) || vertical <= 0.0 || horizontal <= 0.0)
    {
        return std::unexpected(ContractError::InvalidProjection);
    }
    return FrustumTangents{.horizontal = horizontal, .vertical = vertical};
}

std::expected<double, ContractError> DeviceDepthFromViewDepth(double viewDepth,
                                                              PerspectiveProjection projection) noexcept
{
    auto const valid = ValidateProjection(projection);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (!IsFinite(viewDepth))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (viewDepth < projection.nearPlane * (1.0 - kDepthDomainTolerance) ||
        viewDepth > projection.farPlane * (1.0 + kDepthDomainTolerance))
    {
        return std::unexpected(ContractError::InvalidDepth);
    }

    double const clampedViewDepth = std::clamp(viewDepth, projection.nearPlane, projection.farPlane);
    double const range = projection.farPlane - projection.nearPlane;
    // Both conventions are one ratio with the denominator z * (f - n). Keeping the plane difference inside the
    // numerator makes the near and far planes encode exactly: the numerator is exactly zero at one plane, and at the
    // other it is the same rounded product as the denominator, so the quotient is exactly one. The algebraic
    // additive-plus-reciprocal form in MakeDeviceDepthCoefficients cancels two same-magnitude terms instead and can
    // land several units in the last place outside [0, 1] for ordinary near and far pairs.
    double const denominator = clampedViewDepth * range;
    double const numerator = projection.depthConvention == DepthConvention::Forward
                                 ? projection.farPlane * (clampedViewDepth - projection.nearPlane)
                                 : projection.nearPlane * (projection.farPlane - clampedViewDepth);
    if (!IsFinite(denominator) || denominator <= 0.0)
    {
        return std::unexpected(ContractError::InvalidProjection);
    }

    double const deviceDepth = numerator / denominator;
    if (!IsFinite(deviceDepth))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return std::clamp(deviceDepth, 0.0, 1.0);
}

std::expected<double, ContractError> ViewDepthFromDeviceDepth(double deviceDepth,
                                                              PerspectiveProjection projection) noexcept
{
    auto const valid = ValidateProjection(projection);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (!IsFinite(deviceDepth))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (deviceDepth < -kDepthDomainTolerance || deviceDepth > 1.0 + kDepthDomainTolerance)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }

    double const clampedDeviceDepth = std::clamp(deviceDepth, 0.0, 1.0);
    bool const forward = projection.depthConvention == DepthConvention::Forward;
    // The two plane distances are definitional, so the encodings that mean "on the near plane" and "on the far
    // plane" decode back to them exactly instead of being reconstructed through a division that is only accurate to
    // a few units in the last place.
    if (clampedDeviceDepth == 0.0)
    {
        return forward ? projection.nearPlane : projection.farPlane;
    }
    if (clampedDeviceDepth == 1.0)
    {
        return forward ? projection.farPlane : projection.nearPlane;
    }

    // z = n * f / lerp(planes). Writing the denominator as a convex combination of the planes keeps it bounded
    // between them, so no cancellation can amplify the result outside [n, f].
    double const complement = 1.0 - clampedDeviceDepth;
    double const denominator = forward
                                   ? (projection.farPlane * complement) + (projection.nearPlane * clampedDeviceDepth)
                                   : (projection.nearPlane * complement) + (projection.farPlane * clampedDeviceDepth);
    if (!IsFinite(denominator) || denominator <= 0.0)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }

    double const viewDepth = (projection.nearPlane * projection.farPlane) / denominator;
    if (!IsFinite(viewDepth))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return std::clamp(viewDepth, projection.nearPlane, projection.farPlane);
}

std::expected<Float2, ContractError> PixelCenterUv(PixelCoordinate pixel, Extent2D extent) noexcept
{
    auto const valid = ValidateExtent(extent);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (pixel.x >= extent.width || pixel.y >= extent.height)
    {
        return std::unexpected(ContractError::InvalidPixel);
    }
    return Float2{
        .x = (static_cast<double>(pixel.x) + 0.5) / static_cast<double>(extent.width),
        .y = (static_cast<double>(pixel.y) + 0.5) / static_cast<double>(extent.height),
    };
}

std::expected<Float2, ContractError> NdcFromUv(Float2 uv) noexcept
{
    if (!IsFinite(uv))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return Float2{.x = (2.0 * uv.x) - 1.0, .y = 1.0 - (2.0 * uv.y)};
}

std::expected<Float2, ContractError> UvFromNdc(Float2 ndc) noexcept
{
    if (!IsFinite(ndc))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return Float2{.x = (ndc.x + 1.0) * 0.5, .y = (1.0 - ndc.y) * 0.5};
}

std::expected<Float3, ContractError> ViewPositionFromUv(Float2 uv, double viewDepth,
                                                        PerspectiveProjection projection) noexcept
{
    auto const tangents = ViewFrustumTangents(projection);
    if (!tangents)
    {
        return std::unexpected(tangents.error());
    }
    if (!IsFinite(viewDepth))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (viewDepth <= 0.0)
    {
        return std::unexpected(ContractError::NonPositiveViewDepth);
    }
    if (viewDepth > kMaximumViewDistance)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }
    auto const ndc = NdcFromUv(uv);
    if (!ndc)
    {
        return std::unexpected(ndc.error());
    }

    Float3 const viewPosition{
        .x = ndc->x * viewDepth * tangents->horizontal,
        .y = ndc->y * viewDepth * tangents->vertical,
        .z = viewDepth,
    };
    if (!IsFinite(viewPosition))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return viewPosition;
}

std::expected<Float3, ContractError> ReconstructViewPosition(Float2 uv, double deviceDepth,
                                                             PerspectiveProjection projection) noexcept
{
    auto const viewDepth = ViewDepthFromDeviceDepth(deviceDepth, projection);
    if (!viewDepth)
    {
        return std::unexpected(viewDepth.error());
    }
    return ViewPositionFromUv(uv, *viewDepth, projection);
}

std::expected<ScreenProjection, ContractError> ProjectViewPosition(Float3 viewPosition,
                                                                   PerspectiveProjection projection) noexcept
{
    auto const tangents = ViewFrustumTangents(projection);
    if (!tangents)
    {
        return std::unexpected(tangents.error());
    }
    if (!IsFinite(viewPosition))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (viewPosition.z <= 0.0)
    {
        return std::unexpected(ContractError::NonPositiveViewDepth);
    }

    Float2 const ndc{
        .x = viewPosition.x / (viewPosition.z * tangents->horizontal),
        .y = viewPosition.y / (viewPosition.z * tangents->vertical),
    };
    auto const uv = UvFromNdc(ndc);
    if (!uv)
    {
        return std::unexpected(uv.error());
    }
    return ScreenProjection{.ndc = ndc, .uv = *uv, .viewDepth = viewPosition.z};
}

std::expected<ReflectionRay, ContractError> BuildReflectionRay(SurfaceSample const &surface,
                                                               RayConstructionSettings const &settings) noexcept
{
    auto const validSettings = ValidateRayConstructionSettings(settings);
    if (!validSettings)
    {
        return std::unexpected(validSettings.error());
    }
    if (!IsFinite(surface.viewPosition))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    auto const validNormal = ValidateUnitNormal(surface.viewNormal);
    if (!validNormal)
    {
        return std::unexpected(validNormal.error());
    }
    auto const validRoughness = ValidateUnitInterval(surface.roughness, ContractError::InvalidRoughness);
    if (!validRoughness)
    {
        return std::unexpected(validRoughness.error());
    }
    if (surface.viewPosition.z <= 0.0)
    {
        return std::unexpected(ContractError::NonPositiveViewDepth);
    }

    double const distanceToEye = Length(surface.viewPosition);
    if (!IsFinite(distanceToEye) || distanceToEye <= 0.0)
    {
        return std::unexpected(ContractError::NonPositiveViewDepth);
    }
    Float3 const viewDirection = Scale(surface.viewPosition, -1.0 / distanceToEye);
    double const nDotV = Dot(surface.viewNormal, viewDirection);
    if (nDotV <= settings.minimumFacingCosine)
    {
        return std::unexpected(ContractError::BackFacingSurface);
    }

    Float3 const mirrored = Subtract(Scale(surface.viewNormal, 2.0 * nDotV), viewDirection);
    double const mirroredLength = Length(mirrored);
    if (!IsFinite(mirroredLength) || mirroredLength <= 0.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    Float3 const direction = Scale(mirrored, 1.0 / mirroredLength);

    double const appliedNormalBias =
        settings.constantNormalBias + (settings.depthProportionalNormalBias * surface.viewPosition.z);
    Float3 const origin = Add(surface.viewPosition, Scale(surface.viewNormal, appliedNormalBias));
    if (!IsFinite(origin) || !IsFinite(direction))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    return ReflectionRay{
        .origin = origin,
        .direction = direction,
        .viewDirection = viewDirection,
        .nDotV = nDotV,
        .appliedNormalBias = appliedNormalBias,
        .maximumDistance = settings.maximumRayDistance,
        .towardCameraCosine = Dot(direction, viewDirection),
    };
}

std::expected<ClippedRaySegment, ContractError> ClipRayToFrustum(ReflectionRay const &ray,
                                                                 PerspectiveProjection projection) noexcept
{
    auto const tangents = ViewFrustumTangents(projection);
    if (!tangents)
    {
        return std::unexpected(tangents.error());
    }
    if (!IsFinite(ray.origin) || !IsFinite(ray.maximumDistance))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    auto const validDirection = ValidateUnitVector(ray.direction);
    if (!validDirection)
    {
        return std::unexpected(validDirection.error());
    }
    if (ray.maximumDistance <= 0.0 || ray.maximumDistance > kMaximumViewDistance)
    {
        return std::unexpected(ContractError::InvalidDistance);
    }

    double enterDistance = 0.0;
    double exitDistance = ray.maximumDistance;
    ClipLimit enterLimit = ClipLimit::RayStart;
    ClipLimit exitLimit = ClipLimit::RayEnd;

    for (FrustumPlane const &plane : BuildFrustumPlanes(projection, *tangents))
    {
        double const distanceAtOrigin = Dot(plane.normal, ray.origin) + plane.offset;
        double const rate = Dot(plane.normal, ray.direction);
        if (rate == 0.0)
        {
            if (distanceAtOrigin < 0.0)
            {
                return ClippedRaySegment{
                    .intersectsFrustum = false,
                    .enterLimit = enterLimit,
                    .exitLimit = exitLimit,
                    .rejectionLimit = plane.limit,
                };
            }
            continue;
        }

        double const crossing = -distanceAtOrigin / rate;
        if (rate > 0.0)
        {
            if (crossing > enterDistance)
            {
                enterDistance = crossing;
                enterLimit = plane.limit;
            }
        }
        else if (crossing < exitDistance)
        {
            exitDistance = crossing;
            exitLimit = plane.limit;
        }

        if (enterDistance > exitDistance)
        {
            return ClippedRaySegment{
                .intersectsFrustum = false,
                .enterLimit = enterLimit,
                .exitLimit = exitLimit,
                .rejectionLimit = plane.limit,
            };
        }
    }

    Float3 const enterPosition = Add(ray.origin, Scale(ray.direction, enterDistance));
    Float3 const exitPosition = Add(ray.origin, Scale(ray.direction, exitDistance));
    if (!IsFinite(enterPosition) || !IsFinite(exitPosition))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return ClippedRaySegment{
        .intersectsFrustum = true,
        .enterDistance = enterDistance,
        .exitDistance = exitDistance,
        .enterPosition = enterPosition,
        .exitPosition = exitPosition,
        .enterLimit = enterLimit,
        .exitLimit = exitLimit,
        .rejectionLimit = ClipLimit::RayStart,
    };
}

std::expected<ScreenRaySegment, ContractError> ProjectRaySegment(ClippedRaySegment const &segment,
                                                                 PerspectiveProjection projection,
                                                                 Extent2D extent) noexcept
{
    auto const validProjection = ValidateProjection(projection);
    if (!validProjection)
    {
        return std::unexpected(validProjection.error());
    }
    auto const validExtent = ValidateExtent(extent);
    if (!validExtent)
    {
        return std::unexpected(validExtent.error());
    }
    if (!segment.intersectsFrustum)
    {
        return std::unexpected(ContractError::EmptyRaySegment);
    }
    if (!IsFinite(segment.enterDistance) || !IsFinite(segment.exitDistance) ||
        segment.exitDistance < segment.enterDistance)
    {
        return std::unexpected(ContractError::InvalidDistance);
    }

    auto const start = ProjectViewPosition(segment.enterPosition, projection);
    if (!start)
    {
        return std::unexpected(start.error());
    }
    auto const end = ProjectViewPosition(segment.exitPosition, projection);
    if (!end)
    {
        return std::unexpected(end.error());
    }

    // Frustum clipping already guarantees both endpoints lie in the closed unit square. Saturating removes the last
    // rounding bit at the boundary so traversal never samples outside the depth image.
    Float2 const startUv{.x = Saturate(start->uv.x), .y = Saturate(start->uv.y)};
    Float2 const endUv{.x = Saturate(end->uv.x), .y = Saturate(end->uv.y)};
    double const texelsX = (endUv.x - startUv.x) * static_cast<double>(extent.width);
    double const texelsY = (endUv.y - startUv.y) * static_cast<double>(extent.height);

    return ScreenRaySegment{
        .startUv = startUv,
        .endUv = endUv,
        .startViewDepth = start->viewDepth,
        .endViewDepth = end->viewDepth,
        .startReciprocalViewDepth = 1.0 / start->viewDepth,
        .endReciprocalViewDepth = 1.0 / end->viewDepth,
        .startDistance = segment.enterDistance,
        .endDistance = segment.exitDistance,
        .screenLengthTexels = std::hypot(texelsX, texelsY),
    };
}

std::expected<TraversalSample, ContractError> SampleScreenRay(ScreenRaySegment const &segment, ReflectionRay const &ray,
                                                              PerspectiveProjection projection,
                                                              double parameter) noexcept
{
    if (!IsFinite(parameter))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (parameter < 0.0 || parameter > 1.0)
    {
        return std::unexpected(ContractError::InvalidParameter);
    }
    if (!IsFinite(segment.startReciprocalViewDepth) || !IsFinite(segment.endReciprocalViewDepth) ||
        segment.startReciprocalViewDepth <= 0.0 || segment.endReciprocalViewDepth <= 0.0)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }
    auto const validStartUv = ValidateUnitUv(segment.startUv);
    if (!validStartUv)
    {
        return std::unexpected(validStartUv.error());
    }
    auto const validEndUv = ValidateUnitUv(segment.endUv);
    if (!validEndUv)
    {
        return std::unexpected(validEndUv.error());
    }
    if (!IsFinite(ray.origin))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    auto const validDirection = ValidateUnitVector(ray.direction);
    if (!validDirection)
    {
        return std::unexpected(validDirection.error());
    }

    Float2 const uv{
        .x = Lerp(segment.startUv.x, segment.endUv.x, parameter),
        .y = Lerp(segment.startUv.y, segment.endUv.y, parameter),
    };
    double const reciprocalViewDepth =
        Lerp(segment.startReciprocalViewDepth, segment.endReciprocalViewDepth, parameter);
    if (reciprocalViewDepth <= 0.0)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }
    double const viewDepth = 1.0 / reciprocalViewDepth;
    auto const viewPosition = ViewPositionFromUv(uv, viewDepth, projection);
    if (!viewPosition)
    {
        return std::unexpected(viewPosition.error());
    }

    return TraversalSample{
        .parameter = parameter,
        .uv = uv,
        .reciprocalViewDepth = reciprocalViewDepth,
        .viewDepth = viewDepth,
        .viewPosition = *viewPosition,
        .rayDistance = Dot(Subtract(*viewPosition, ray.origin), ray.direction),
    };
}

std::expected<TraversalStepSchedule, ContractError> BuildTraversalSchedule(ScreenRaySegment const &segment,
                                                                           TraceSettings const &settings) noexcept
{
    auto const validSettings = ValidateTraceSettings(settings);
    if (!validSettings)
    {
        return std::unexpected(validSettings.error());
    }
    if (!IsFinite(segment.screenLengthTexels))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (segment.screenLengthTexels < 0.0)
    {
        return std::unexpected(ContractError::InvalidDistance);
    }

    // A segment with no screen extent still needs one sample at its far end, otherwise a ray pointing straight along
    // the pixel's own view vector would silently report nothing.
    double const parameterPerStep =
        segment.screenLengthTexels > 0.0 ? settings.stepLengthTexels / segment.screenLengthTexels : 1.0;
    double const saturationLimit = static_cast<double>(kMaximumTraversalStepCount) + 1.0;
    double const required =
        segment.screenLengthTexels > 0.0 ? std::ceil(segment.screenLengthTexels / settings.stepLengthTexels) : 1.0;
    std::uint32_t const requiredStepCount =
        static_cast<std::uint32_t>(std::max(1.0, std::min(required, saturationLimit)));

    return TraversalStepSchedule{
        .requiredStepCount = requiredStepCount,
        .stepCount = std::min(requiredStepCount, settings.maximumStepCount),
        .stepBudgetExhausted = requiredStepCount > settings.maximumStepCount,
        .parameterPerStep = parameterPerStep,
    };
}

std::expected<double, ContractError> TraversalStepParameter(ScreenRaySegment const &segment,
                                                            TraceSettings const &settings,
                                                            std::uint32_t stepIndex) noexcept
{
    auto const schedule = BuildTraversalSchedule(segment, settings);
    if (!schedule)
    {
        return std::unexpected(schedule.error());
    }
    if (stepIndex == 0U || stepIndex > kMaximumTraversalStepCount)
    {
        return std::unexpected(ContractError::InvalidParameter);
    }

    double const parameter =
        (static_cast<double>(stepIndex) + settings.startOffsetFraction) * schedule->parameterPerStep;
    return std::min(1.0, parameter);
}

std::expected<void, ContractError> ValidateDepthImage(DepthImageView image) noexcept
{
    auto const valid = ValidateExtent(image.extent);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    std::size_t const expected = static_cast<std::size_t>(image.extent.width) * image.extent.height;
    if (image.deviceDepth.size() != expected)
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    for (double const storedDepth : image.deviceDepth)
    {
        if (!IsFinite(storedDepth))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (storedDepth < 0.0 || storedDepth > 1.0)
        {
            return std::unexpected(ContractError::InvalidDepth);
        }
    }
    return {};
}

std::expected<PixelCoordinate, ContractError> TexelFromUv(Float2 uv, Extent2D extent) noexcept
{
    auto const validExtent = ValidateExtent(extent);
    if (!validExtent)
    {
        return std::unexpected(validExtent.error());
    }
    auto const validUv = ValidateUnitUv(uv);
    if (!validUv)
    {
        return std::unexpected(validUv.error());
    }

    double const texelX = std::floor(uv.x * static_cast<double>(extent.width));
    double const texelY = std::floor(uv.y * static_cast<double>(extent.height));
    // uv exactly one lands on the first texel outside the image; the last texel owns that boundary.
    return PixelCoordinate{
        .x = static_cast<std::uint32_t>(std::min(texelX, static_cast<double>(extent.width - 1U))),
        .y = static_cast<std::uint32_t>(std::min(texelY, static_cast<double>(extent.height - 1U))),
    };
}

std::expected<double, ContractError> SampleDeviceDepthNearest(DepthImageView image, Float2 uv) noexcept
{
    auto const validImage = ValidateDepthImage(image);
    if (!validImage)
    {
        return std::unexpected(validImage.error());
    }
    auto const texel = TexelFromUv(uv, image.extent);
    if (!texel)
    {
        return std::unexpected(texel.error());
    }

    std::size_t const index = (static_cast<std::size_t>(texel->y) * image.extent.width) + texel->x;
    // ValidateDepthImage has already established that every texel is a finite device depth in [0, 1].
    return image.deviceDepth[index];
}

namespace
{

struct ScenePoint final
{
    bool hasGeometry{};
    double sceneViewDepth{};
    double depthDelta{};
    double thicknessInterval{};
};

[[nodiscard]] std::expected<ScenePoint, ContractError> EvaluateScenePoint(TraversalSample const &sample,
                                                                          TraceContext const &context) noexcept
{
    auto const deviceDepth = SampleDeviceDepthNearest(context.depth, sample.uv);
    if (!deviceDepth)
    {
        return std::unexpected(deviceDepth.error());
    }
    if (IsBackgroundDeviceDepth(*deviceDepth, context.projection.depthConvention))
    {
        return ScenePoint{};
    }
    auto const sceneViewDepth = ViewDepthFromDeviceDepth(*deviceDepth, context.projection);
    if (!sceneViewDepth)
    {
        return std::unexpected(sceneViewDepth.error());
    }

    return ScenePoint{
        .hasGeometry = true,
        .sceneViewDepth = *sceneViewDepth,
        .depthDelta = sample.viewDepth - *sceneViewDepth,
        .thicknessInterval =
            context.traversal.constantThickness + (context.traversal.depthProportionalThickness * *sceneViewDepth),
    };
}

} // namespace

std::expected<TraceResult, ContractError> TraceScreenSpaceRay(TraceInput const &input,
                                                              TraceContext const &context) noexcept
{
    auto const validProjection = ValidateProjection(context.projection);
    if (!validProjection)
    {
        return std::unexpected(validProjection.error());
    }
    auto const validImage = ValidateDepthImage(context.depth);
    if (!validImage)
    {
        return std::unexpected(validImage.error());
    }
    auto const validRaySettings = ValidateRayConstructionSettings(context.ray);
    if (!validRaySettings)
    {
        return std::unexpected(validRaySettings.error());
    }
    auto const validTraceSettings = ValidateTraceSettings(context.traversal);
    if (!validTraceSettings)
    {
        return std::unexpected(validTraceSettings.error());
    }

    if (input.surfaceIsBackground)
    {
        return MakeMiss(MissReason::InvalidInput);
    }
    if (input.surface.viewPosition.z < context.projection.nearPlane)
    {
        return MakeMiss(MissReason::BehindCamera);
    }

    auto const ray = BuildReflectionRay(input.surface, context.ray);
    if (!ray)
    {
        switch (ray.error())
        {
        case ContractError::BackFacingSurface:
            return MakeMiss(MissReason::BackFacing);
        case ContractError::NonPositiveViewDepth:
            return MakeMiss(MissReason::BehindCamera);
        case ContractError::NonFinite:
        case ContractError::InvalidNormal:
        case ContractError::InvalidDirection:
        case ContractError::InvalidRoughness:
            return MakeMiss(MissReason::InvalidInput);
        default:
            return std::unexpected(ray.error());
        }
    }

    auto const clipped = ClipRayToFrustum(*ray, context.projection);
    if (!clipped)
    {
        return std::unexpected(clipped.error());
    }
    if (!clipped->intersectsFrustum)
    {
        return MakeMiss(clipped->rejectionLimit == ClipLimit::NearPlane ? MissReason::BehindCamera
                                                                        : MissReason::OffScreen);
    }

    auto const segment = ProjectRaySegment(*clipped, context.projection, context.depth.extent);
    if (!segment)
    {
        return std::unexpected(segment.error());
    }
    auto const schedule = BuildTraversalSchedule(*segment, context.traversal);
    if (!schedule)
    {
        return std::unexpected(schedule.error());
    }

    TraceResult result{
        .missReason = MissReason::None,
        .stepBudgetExhausted = schedule->stepBudgetExhausted,
        .segmentExitLimit = clipped->exitLimit,
        .segmentExitDistance = clipped->exitDistance,
    };

    double lowParameter = 0.0;
    bool previousBehind = false;
    bool reachedSegmentEnd = false;
    for (std::uint32_t stepIndex = 1U; stepIndex <= schedule->stepCount; ++stepIndex)
    {
        auto const parameter = TraversalStepParameter(*segment, context.traversal, stepIndex);
        if (!parameter)
        {
            return std::unexpected(parameter.error());
        }
        auto const sample = SampleScreenRay(*segment, *ray, context.projection, *parameter);
        if (!sample)
        {
            return std::unexpected(sample.error());
        }
        auto const scene = EvaluateScenePoint(*sample, context);
        if (!scene)
        {
            return std::unexpected(scene.error());
        }
        ++result.stepCount;

        if (!scene->hasGeometry)
        {
            lowParameter = *parameter;
            previousBehind = false;
            if (*parameter >= 1.0)
            {
                reachedSegmentEnd = true;
                break;
            }
            continue;
        }
        ++result.geometrySampleCount;

        bool const behind = scene->depthDelta >= 0.0;
        if (behind && !previousBehind)
        {
            double refinedLow = lowParameter;
            double refinedHigh = *parameter;
            for (std::uint32_t refinement = 0U; refinement < context.traversal.refinementStepCount; ++refinement)
            {
                double const middle = 0.5 * (refinedLow + refinedHigh);
                auto const middleSample = SampleScreenRay(*segment, *ray, context.projection, middle);
                if (!middleSample)
                {
                    return std::unexpected(middleSample.error());
                }
                auto const middleScene = EvaluateScenePoint(*middleSample, context);
                if (!middleScene)
                {
                    return std::unexpected(middleScene.error());
                }
                ++result.refinementCount;
                if (middleScene->hasGeometry && middleScene->depthDelta >= 0.0)
                {
                    refinedHigh = middle;
                }
                else
                {
                    refinedLow = middle;
                }
            }

            auto const hitSample = SampleScreenRay(*segment, *ray, context.projection, refinedHigh);
            if (!hitSample)
            {
                return std::unexpected(hitSample.error());
            }
            auto const hitScene = EvaluateScenePoint(*hitSample, context);
            if (!hitScene)
            {
                return std::unexpected(hitScene.error());
            }

            if (hitScene->hasGeometry && hitScene->depthDelta >= 0.0 &&
                hitScene->depthDelta <= hitScene->thicknessInterval)
            {
                result.hit = true;
                result.missReason = MissReason::None;
                result.hitUv = hitSample->uv;
                result.hitViewPosition = hitSample->viewPosition;
                result.hitRayViewDepth = hitSample->viewDepth;
                result.hitSceneViewDepth = hitScene->sceneViewDepth;
                result.hitDepthDelta = hitScene->depthDelta;
                result.hitThicknessInterval = hitScene->thicknessInterval;
                result.hitRayDistance = hitSample->rayDistance;
                result.hitParameter = hitSample->parameter;
                return result;
            }
            ++result.thicknessRejectionCount;
        }

        lowParameter = *parameter;
        previousBehind = behind;
        if (*parameter >= 1.0)
        {
            reachedSegmentEnd = true;
            break;
        }
    }

    if (result.thicknessRejectionCount > 0U)
    {
        result.missReason = MissReason::ThicknessExceeded;
        return result;
    }
    if (schedule->stepBudgetExhausted || !reachedSegmentEnd)
    {
        result.stepBudgetExhausted = true;
        result.missReason = MissReason::MaximumSteps;
        return result;
    }
    if (result.geometrySampleCount == 0U)
    {
        result.missReason = MissReason::NoCrossing;
        return result;
    }
    switch (clipped->exitLimit)
    {
    case ClipLimit::RayEnd:
        result.missReason = MissReason::MaximumDistance;
        break;
    case ClipLimit::NearPlane:
        result.missReason = MissReason::BehindCamera;
        break;
    default:
        result.missReason = MissReason::OffScreen;
        break;
    }
    return result;
}

std::expected<ConfidenceFactors, ContractError> EvaluateConfidence(ConfidenceInput const &input,
                                                                   ConfidenceSettings const &settings) noexcept
{
    auto const validSettings = ValidateConfidenceSettings(settings);
    if (!validSettings)
    {
        return std::unexpected(validSettings.error());
    }
    if (!IsFinite(input.hitRayDistance) || !IsFinite(input.maximumRayDistance) || !IsFinite(input.hitDepthDelta) ||
        !IsFinite(input.hitThicknessInterval) || !IsFinite(input.roughness) || !IsFinite(input.towardCameraCosine))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    auto const validRoughness = ValidateUnitInterval(input.roughness, ContractError::InvalidRoughness);
    if (!validRoughness)
    {
        return std::unexpected(validRoughness.error());
    }
    if (input.towardCameraCosine < -1.0 || input.towardCameraCosine > 1.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    if (input.maximumRayDistance <= 0.0 || input.maximumRayDistance > kMaximumViewDistance)
    {
        return std::unexpected(ContractError::InvalidDistance);
    }

    // Roughness and view-facing depend only on the shaded surface and the ray, so they are reported for a miss too.
    double const roughnessFactor = FadeOut(input.roughness, settings.roughnessFadeStart, settings.roughnessFadeEnd);
    double const towardCameraFactor =
        FadeOut(input.towardCameraCosine, settings.towardCameraFadeStart, settings.towardCameraFadeEnd);

    ConfidenceFactors factors{
        .validity = 0.0,
        .screenEdge = 0.0,
        .rayDistance = 0.0,
        .thickness = 0.0,
        .roughness = roughnessFactor,
        .towardCamera = towardCameraFactor,
        .combined = 0.0,
    };
    if (!input.hit)
    {
        return factors;
    }

    auto const validUv = ValidateUnitUv(input.hitUv);
    if (!validUv)
    {
        return std::unexpected(validUv.error());
    }
    if (input.hitRayDistance < 0.0 || input.hitDepthDelta < 0.0 || input.hitThicknessInterval <= 0.0)
    {
        return std::unexpected(ContractError::InvalidDistance);
    }

    double const borderDistance = std::min({input.hitUv.x, 1.0 - input.hitUv.x, input.hitUv.y, 1.0 - input.hitUv.y});
    double const travelledFraction = Saturate(input.hitRayDistance / input.maximumRayDistance);
    double const thicknessLimit = input.hitThicknessInterval * settings.thicknessFadeFraction;

    factors.validity = 1.0;
    factors.screenEdge = Saturate(borderDistance / settings.screenEdgeFadeUv);
    factors.rayDistance = FadeOut(travelledFraction, settings.distanceFadeStartFraction, 1.0);
    factors.thickness = FadeOut(input.hitDepthDelta, 0.0, thicknessLimit);
    factors.combined = factors.validity * factors.screenEdge * factors.rayDistance * factors.thickness *
                       factors.roughness * factors.towardCamera;
    return factors;
}

std::expected<ReflectionComposition, ContractError> ComposeReflection(ReflectionCompositionInput const &input) noexcept
{
    auto const validScreen = ValidateRadiance(input.screenRadiance);
    if (!validScreen)
    {
        return std::unexpected(validScreen.error());
    }
    auto const validEnvironment = ValidateRadiance(input.environmentRadiance);
    if (!validEnvironment)
    {
        return std::unexpected(validEnvironment.error());
    }
    auto const validConfidence = ValidateUnitInterval(input.confidence, ContractError::InvalidConfidence);
    if (!validConfidence)
    {
        return std::unexpected(validConfidence.error());
    }

    double const screenWeight = input.confidence;
    double const environmentWeight = 1.0 - input.confidence;
    Rgb const screenContribution{
        .r = input.screenRadiance.r * screenWeight,
        .g = input.screenRadiance.g * screenWeight,
        .b = input.screenRadiance.b * screenWeight,
    };
    Rgb const environmentContribution{
        .r = input.environmentRadiance.r * environmentWeight,
        .g = input.environmentRadiance.g * environmentWeight,
        .b = input.environmentRadiance.b * environmentWeight,
    };

    return ReflectionComposition{
        .screenWeight = screenWeight,
        .environmentWeight = environmentWeight,
        .screenContribution = screenContribution,
        .environmentContribution = environmentContribution,
        .incomingRadiance =
            {
                .r = screenContribution.r + environmentContribution.r,
                .g = screenContribution.g + environmentContribution.g,
                .b = screenContribution.b + environmentContribution.b,
            },
    };
}

std::expected<Rgb, ContractError> ApplySplitSumSpecular(Rgb incomingRadiance, Rgb normalIncidenceReflectance,
                                                        double scaleA, double biasB) noexcept
{
    auto const validRadiance = ValidateRadiance(incomingRadiance);
    if (!validRadiance)
    {
        return std::unexpected(validRadiance.error());
    }
    auto const validReflectance = ValidateReflectance(normalIncidenceReflectance);
    if (!validReflectance)
    {
        return std::unexpected(validReflectance.error());
    }
    auto const validScale = ValidateUnitInterval(scaleA, ContractError::InvalidSettings);
    if (!validScale)
    {
        return std::unexpected(validScale.error());
    }
    auto const validBias = ValidateUnitInterval(biasB, ContractError::InvalidSettings);
    if (!validBias)
    {
        return std::unexpected(validBias.error());
    }

    Rgb const result{
        .r = incomingRadiance.r * ((normalIncidenceReflectance.r * scaleA) + biasB),
        .g = incomingRadiance.g * ((normalIncidenceReflectance.g * scaleA) + biasB),
        .b = incomingRadiance.b * ((normalIncidenceReflectance.b * scaleA) + biasB),
    };
    auto const validResult = ValidateRadiance(result);
    if (!validResult)
    {
        return std::unexpected(validResult.error());
    }
    return result;
}

std::expected<HemisphereSample, ContractError> MapCosineHemisphere(Float2 unitSample) noexcept
{
    if (!IsFinite(unitSample))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (unitSample.x < 0.0 || unitSample.x > 1.0 || unitSample.y < 0.0 || unitSample.y > 1.0)
    {
        return std::unexpected(ContractError::InvalidUnitSample);
    }

    double const radius = std::sqrt(unitSample.x);
    double const azimuth = 2.0 * kPi * unitSample.y;
    double const cosine = std::sqrt(1.0 - unitSample.x);
    if (cosine <= 0.0)
    {
        return std::unexpected(ContractError::DegenerateSample);
    }

    return HemisphereSample{
        .direction =
            {
                .x = radius * std::cos(azimuth),
                .y = radius * std::sin(azimuth),
                .z = cosine,
            },
        .pdf = cosine / kPi,
        .cosine = cosine,
    };
}

std::expected<double, ContractError> CosineHemispherePdf(double cosine) noexcept
{
    if (!IsFinite(cosine))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (cosine <= 0.0 || cosine > 1.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    return cosine / kPi;
}

std::expected<TangentFrame, ContractError> BuildTangentFrame(Float3 normal) noexcept
{
    auto const validNormal = ValidateUnitNormal(normal);
    if (!validNormal)
    {
        return std::unexpected(validNormal.error());
    }

    double const sign = std::copysign(1.0, normal.z);
    double const a = -1.0 / (sign + normal.z);
    double const b = normal.x * normal.y * a;
    Float3 const tangent{
        .x = 1.0 + (sign * normal.x * normal.x * a),
        .y = sign * b,
        .z = -sign * normal.x,
    };
    Float3 const bitangent{
        .x = b,
        .y = sign + (normal.y * normal.y * a),
        .z = -normal.y,
    };
    if (!IsFinite(tangent) || !IsFinite(bitangent))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return TangentFrame{.tangent = tangent, .bitangent = bitangent, .normal = normal};
}

std::expected<Float3, ContractError> TransformLocalToView(TangentFrame const &frame, Float3 localDirection) noexcept
{
    auto const validNormal = ValidateUnitNormal(frame.normal);
    if (!validNormal)
    {
        return std::unexpected(validNormal.error());
    }
    auto const validTangent = ValidateUnitVector(frame.tangent);
    if (!validTangent)
    {
        return std::unexpected(validTangent.error());
    }
    auto const validBitangent = ValidateUnitVector(frame.bitangent);
    if (!validBitangent)
    {
        return std::unexpected(validBitangent.error());
    }
    auto const validLocal = ValidateUnitVector(localDirection);
    if (!validLocal)
    {
        return std::unexpected(validLocal.error());
    }

    Float3 const result = Add(Add(Scale(frame.tangent, localDirection.x), Scale(frame.bitangent, localDirection.y)),
                              Scale(frame.normal, localDirection.z));
    if (!IsFinite(result))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return result;
}

std::expected<Rgb, ContractError> EvaluateLambertianBrdf(Rgb albedo) noexcept
{
    auto const validAlbedo = ValidateReflectance(albedo);
    if (!validAlbedo)
    {
        return std::unexpected(validAlbedo.error());
    }
    return Rgb{.r = albedo.r / kPi, .g = albedo.g / kPi, .b = albedo.b / kPi};
}

std::expected<IndirectEstimate, ContractError> EstimateDiffuseIndirect(Float3 viewNormal, Rgb albedo,
                                                                       std::span<IndirectSample const> samples,
                                                                       IndirectSettings const &settings) noexcept
{
    auto const validNormal = ValidateUnitNormal(viewNormal);
    if (!validNormal)
    {
        return std::unexpected(validNormal.error());
    }
    auto const brdf = EvaluateLambertianBrdf(albedo);
    if (!brdf)
    {
        return std::unexpected(brdf.error());
    }
    if (samples.empty())
    {
        return std::unexpected(ContractError::EmptySampleSet);
    }
    if (samples.size() > kMaximumIndirectSampleCount)
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }
    if (!IsFinite(settings.maximumRadiance) || settings.maximumRadiance <= 0.0 ||
        settings.maximumRadiance > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    if (settings.missPolicy != IndirectMissPolicy::EnvironmentFallback &&
        settings.missPolicy != IndirectMissPolicy::ZeroRadiance)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }

    IndirectEstimate estimate{};
    Rgb accumulated{};
    double accumulatedWeight = 0.0;
    for (IndirectSample const &sample : samples)
    {
        auto const validDirection = ValidateUnitVector(sample.direction);
        if (!validDirection)
        {
            return std::unexpected(validDirection.error());
        }
        double const cosine = Dot(viewNormal, sample.direction);
        if (cosine <= 0.0)
        {
            return std::unexpected(ContractError::InvalidDirection);
        }
        if (!IsFinite(sample.pdf) || sample.pdf <= 0.0)
        {
            return std::unexpected(ContractError::InvalidDensity);
        }
        auto const validHitRadiance = ValidateRadiance(sample.hitRadiance);
        if (!validHitRadiance)
        {
            return std::unexpected(validHitRadiance.error());
        }
        auto const validEnvironmentRadiance = ValidateRadiance(sample.environmentRadiance);
        if (!validEnvironmentRadiance)
        {
            return std::unexpected(validEnvironmentRadiance.error());
        }

        Rgb incoming{};
        if (sample.hit)
        {
            incoming = sample.hitRadiance;
            ++estimate.hitCount;
        }
        else
        {
            ++estimate.missCount;
            if (settings.missPolicy == IndirectMissPolicy::EnvironmentFallback)
            {
                incoming = sample.environmentRadiance;
            }
        }

        Rgb const clamped{
            .r = std::min(incoming.r, settings.maximumRadiance),
            .g = std::min(incoming.g, settings.maximumRadiance),
            .b = std::min(incoming.b, settings.maximumRadiance),
        };
        if (clamped != incoming)
        {
            ++estimate.clampedSampleCount;
        }

        double const weight = cosine / sample.pdf;
        if (!IsFinite(weight))
        {
            return std::unexpected(ContractError::InvalidDensity);
        }
        accumulatedWeight += weight;
        accumulated.r += clamped.r * weight;
        accumulated.g += clamped.g * weight;
        accumulated.b += clamped.b * weight;
    }

    double const sampleCount = static_cast<double>(samples.size());
    estimate.sampleCount = static_cast<std::uint32_t>(samples.size());
    estimate.irradiance = {
        .r = accumulated.r / sampleCount,
        .g = accumulated.g / sampleCount,
        .b = accumulated.b / sampleCount,
    };
    estimate.averageCosineOverPdf = accumulatedWeight / sampleCount;
    estimate.outgoingRadiance = {
        .r = brdf->r * estimate.irradiance.r,
        .g = brdf->g * estimate.irradiance.g,
        .b = brdf->b * estimate.irradiance.b,
    };
    if (!IsFinite(estimate.irradiance) || !IsFinite(estimate.outgoingRadiance))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return estimate;
}

std::expected<TemporalReuseResult, ContractError> ReuseTemporalSamples(TemporalReuseInput const &input,
                                                                       TemporalReuseSettings const &settings) noexcept
{
    if (settings.maximumSampleCount == 0U || settings.maximumSampleCount > 1'024U)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    if (!IsFinite(settings.maximumHistoryWeight) || settings.maximumHistoryWeight < 0.0 ||
        settings.maximumHistoryWeight >= 1.0)
    {
        // A ceiling of exactly one would let a usable history sample erase the current frame, which is the fixed
        // point this bound exists to prevent.
        return std::unexpected(ContractError::InvalidSettings);
    }
    if (!IsFinite(settings.absoluteDepthTolerance) || !IsFinite(settings.relativeDepthTolerance) ||
        settings.absoluteDepthTolerance < 0.0 || settings.relativeDepthTolerance < 0.0)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    auto const validUv = ValidateUnitUv(input.currentUv);
    if (!validUv)
    {
        return std::unexpected(validUv.error());
    }
    if (!IsFinite(input.motionPreviousMinusCurrentUv))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    auto const validCurrent = ValidateRadiance(input.currentSceneLinear);
    if (!validCurrent)
    {
        return std::unexpected(validCurrent.error());
    }
    auto const validCurrentConfidence = ValidateUnitInterval(input.currentConfidence, ContractError::InvalidConfidence);
    if (!validCurrentConfidence)
    {
        return std::unexpected(validCurrentConfidence.error());
    }

    Float2 const previousUv{
        .x = input.currentUv.x + input.motionPreviousMinusCurrentUv.x,
        .y = input.currentUv.y + input.motionPreviousMinusCurrentUv.y,
    };
    if (!IsFinite(previousUv))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    bool const inBounds = previousUv.x >= 0.0 && previousUv.x <= 1.0 && previousUv.y >= 0.0 && previousUv.y <= 1.0;
    // The history fields describe a texel that was actually read. When nothing was read they carry no meaning, so
    // they are neither validated nor consulted; that keeps a caller that could not sample from having to invent
    // well-formed values.
    bool const sampled = input.hasHistory && inBounds;

    std::uint32_t rejectionReasons = 0U;
    rejectionReasons |= input.hasHistory ? 0U : static_cast<std::uint32_t>(HistoryRejection::NoHistory);
    rejectionReasons |= inBounds ? 0U : static_cast<std::uint32_t>(HistoryRejection::OffScreen);

    double depthDifference = 0.0;
    double depthTolerance = 0.0;
    if (sampled)
    {
        auto const validHistory = ValidateRadiance(input.historySceneLinear);
        if (!validHistory)
        {
            return std::unexpected(validHistory.error());
        }
        auto const validHistoryConfidence =
            ValidateUnitInterval(input.historyConfidence, ContractError::InvalidConfidence);
        if (!validHistoryConfidence)
        {
            return std::unexpected(validHistoryConfidence.error());
        }
        if (!IsFinite(input.expectedPreviousViewDepth) || !IsFinite(input.sampledHistoryViewDepth))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (input.expectedPreviousViewDepth <= 0.0 || input.sampledHistoryViewDepth <= 0.0)
        {
            return std::unexpected(ContractError::NonPositiveViewDepth);
        }
        if (input.expectedPreviousViewDepth > kMaximumViewDistance ||
            input.sampledHistoryViewDepth > kMaximumViewDistance)
        {
            return std::unexpected(ContractError::InvalidDepth);
        }

        depthDifference = std::abs(input.expectedPreviousViewDepth - input.sampledHistoryViewDepth);
        depthTolerance = settings.absoluteDepthTolerance +
                         (settings.relativeDepthTolerance *
                          std::max(input.expectedPreviousViewDepth, input.sampledHistoryViewDepth));

        rejectionReasons |=
            input.previousSampleCount == 0U ? static_cast<std::uint32_t>(HistoryRejection::NoSamples) : 0U;
        rejectionReasons |=
            settings.requireMaterialIdentity && input.currentMaterialId != input.sampledHistoryMaterialId
                ? static_cast<std::uint32_t>(HistoryRejection::MaterialMismatch)
                : 0U;
        rejectionReasons |=
            depthDifference > depthTolerance ? static_cast<std::uint32_t>(HistoryRejection::DepthMismatch) : 0U;
    }

    bool const historyUsable = rejectionReasons == 0U;
    std::uint32_t const boundedPreviousCount = std::min(input.previousSampleCount, settings.maximumSampleCount);
    double const historyScore =
        historyUsable ? input.historyConfidence * static_cast<double>(boundedPreviousCount) : 0.0;
    double const currentScore = input.currentConfidence;
    double const total = historyScore + currentScore;

    double historyWeight = 0.0;
    if (total > 0.0)
    {
        historyWeight = historyScore / total;
    }
    // The confidence-and-age split decides how the two samples share the result; the ceiling decides how much of
    // the result a history sample is ever allowed to own. Applying it here keeps the split below the bound intact.
    historyWeight = std::min(historyWeight, settings.maximumHistoryWeight);
    double const currentWeight = 1.0 - historyWeight;

    Rgb const historyRadiance = historyUsable ? input.historySceneLinear : Rgb{};
    double const historyConfidence = historyUsable ? input.historyConfidence : 0.0;
    return TemporalReuseResult{
        .previousUv = previousUv,
        .historyUsable = historyUsable,
        .rejectionReasons = rejectionReasons,
        .currentWeight = currentWeight,
        .historyWeight = historyWeight,
        .depthDifference = depthDifference,
        .depthTolerance = depthTolerance,
        .outputSceneLinear =
            {
                .r = (currentWeight * input.currentSceneLinear.r) + (historyWeight * historyRadiance.r),
                .g = (currentWeight * input.currentSceneLinear.g) + (historyWeight * historyRadiance.g),
                .b = (currentWeight * input.currentSceneLinear.b) + (historyWeight * historyRadiance.b),
            },
        .outputConfidence = (currentWeight * input.currentConfidence) + (historyWeight * historyConfidence),
        .nextSampleCount = historyUsable ? std::min(boundedPreviousCount + 1U, settings.maximumSampleCount) : 1U,
    };
}

std::expected<Extent2D, ContractError> NextPyramidExtent(Extent2D extent) noexcept
{
    auto const valid = ValidateExtent(extent);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    return Extent2D{
        .width = std::max(1U, (extent.width + 1U) / 2U),
        .height = std::max(1U, (extent.height + 1U) / 2U),
    };
}

std::expected<std::uint32_t, ContractError> PyramidLevelCount(Extent2D baseExtent) noexcept
{
    auto const valid = ValidateExtent(baseExtent);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }

    std::uint32_t levels = 1U;
    Extent2D current = baseExtent;
    while (current.width > 1U || current.height > 1U)
    {
        auto const next = NextPyramidExtent(current);
        if (!next)
        {
            return std::unexpected(next.error());
        }
        current = *next;
        ++levels;
        if (levels > kMaximumPyramidLevelCount)
        {
            return std::unexpected(ContractError::InvalidPyramidLevel);
        }
    }
    return levels;
}

std::expected<Extent2D, ContractError> PyramidLevelExtent(Extent2D baseExtent, std::uint32_t level) noexcept
{
    auto const levelCount = PyramidLevelCount(baseExtent);
    if (!levelCount)
    {
        return std::unexpected(levelCount.error());
    }
    if (level >= *levelCount)
    {
        return std::unexpected(ContractError::InvalidPyramidLevel);
    }

    Extent2D current = baseExtent;
    for (std::uint32_t index = 0U; index < level; ++index)
    {
        auto const next = NextPyramidExtent(current);
        if (!next)
        {
            return std::unexpected(next.error());
        }
        current = *next;
    }
    return current;
}

std::expected<Extent2D, ContractError> ReduceDepthLevel(DepthImageView source, DepthConvention convention,
                                                        DepthReduction reduction,
                                                        std::span<double> destination) noexcept
{
    auto const validSource = ValidateDepthImage(source);
    if (!validSource)
    {
        return std::unexpected(validSource.error());
    }
    if (convention != DepthConvention::Forward && convention != DepthConvention::Reversed)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    if (reduction != DepthReduction::NearestSurface && reduction != DepthReduction::FarthestSurface)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    auto const destinationExtent = NextPyramidExtent(source.extent);
    if (!destinationExtent)
    {
        return std::unexpected(destinationExtent.error());
    }
    std::size_t const destinationCount = static_cast<std::size_t>(destinationExtent->width) * destinationExtent->height;
    if (destination.size() != destinationCount)
    {
        return std::unexpected(ContractError::SizeMismatch);
    }

    // Under forward depth the nearest surface is the smallest device depth; under reversed depth it is the largest.
    bool const takeMinimum = (reduction == DepthReduction::NearestSurface) == (convention == DepthConvention::Forward);

    for (std::uint32_t y = 0U; y < destinationExtent->height; ++y)
    {
        for (std::uint32_t x = 0U; x < destinationExtent->width; ++x)
        {
            double reduced =
                takeMinimum ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
            for (std::uint32_t offsetY = 0U; offsetY < 2U; ++offsetY)
            {
                for (std::uint32_t offsetX = 0U; offsetX < 2U; ++offsetX)
                {
                    std::uint32_t const childX = std::min((2U * x) + offsetX, source.extent.width - 1U);
                    std::uint32_t const childY = std::min((2U * y) + offsetY, source.extent.height - 1U);
                    std::size_t const index = (static_cast<std::size_t>(childY) * source.extent.width) + childX;
                    double const value = source.deviceDepth[index];
                    reduced = takeMinimum ? std::min(reduced, value) : std::max(reduced, value);
                }
            }
            destination[(static_cast<std::size_t>(y) * destinationExtent->width) + x] = reduced;
        }
    }
    return *destinationExtent;
}

std::expected<PyramidCellBounds, ContractError> PyramidCellBoundsAt(Extent2D baseExtent, std::uint32_t level,
                                                                    PixelCoordinate levelTexel) noexcept
{
    auto const levelExtent = PyramidLevelExtent(baseExtent, level);
    if (!levelExtent)
    {
        return std::unexpected(levelExtent.error());
    }
    if (levelTexel.x >= levelExtent->width || levelTexel.y >= levelExtent->height)
    {
        return std::unexpected(ContractError::InvalidPixel);
    }

    std::uint64_t const cellSize = std::uint64_t{1} << level;
    std::uint64_t const minimumX = std::uint64_t{levelTexel.x} * cellSize;
    std::uint64_t const minimumY = std::uint64_t{levelTexel.y} * cellSize;
    if (minimumX >= baseExtent.width || minimumY >= baseExtent.height)
    {
        return std::unexpected(ContractError::InvalidPixel);
    }
    std::uint64_t const maximumX = std::min(minimumX + cellSize - 1U, std::uint64_t{baseExtent.width} - 1U);
    std::uint64_t const maximumY = std::min(minimumY + cellSize - 1U, std::uint64_t{baseExtent.height} - 1U);

    return PyramidCellBounds{
        .minimumUv =
            {
                .x = static_cast<double>(minimumX) / static_cast<double>(baseExtent.width),
                .y = static_cast<double>(minimumY) / static_cast<double>(baseExtent.height),
            },
        .maximumUv =
            {
                .x = static_cast<double>(maximumX + 1U) / static_cast<double>(baseExtent.width),
                .y = static_cast<double>(maximumY + 1U) / static_cast<double>(baseExtent.height),
            },
        .baseMinimum =
            {
                .x = static_cast<std::uint32_t>(minimumX),
                .y = static_cast<std::uint32_t>(minimumY),
            },
        .baseMaximumInclusive =
            {
                .x = static_cast<std::uint32_t>(maximumX),
                .y = static_cast<std::uint32_t>(maximumY),
            },
    };
}

std::expected<PyramidCellDecision, ContractError> ClassifyPyramidCell(double cellNearestViewDepth,
                                                                      double segmentMinimumViewDepth,
                                                                      double segmentMaximumViewDepth) noexcept
{
    if (!IsFinite(cellNearestViewDepth) || !IsFinite(segmentMinimumViewDepth) || !IsFinite(segmentMaximumViewDepth))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (cellNearestViewDepth <= 0.0 || segmentMinimumViewDepth <= 0.0 || segmentMaximumViewDepth <= 0.0)
    {
        return std::unexpected(ContractError::NonPositiveViewDepth);
    }
    if (segmentMaximumViewDepth < segmentMinimumViewDepth)
    {
        return std::unexpected(ContractError::InvalidParameter);
    }
    return segmentMaximumViewDepth < cellNearestViewDepth ? PyramidCellDecision::SkipInFront
                                                          : PyramidCellDecision::Descend;
}

std::expected<double, ContractError> CellExitParameter(Float2 uv, Float2 uvDirection, PyramidCellBounds const &bounds,
                                                       double minimumAdvance) noexcept
{
    if (!IsFinite(uv) || !IsFinite(uvDirection) || !IsFinite(minimumAdvance) || !IsFinite(bounds.minimumUv) ||
        !IsFinite(bounds.maximumUv))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (minimumAdvance <= 0.0)
    {
        return std::unexpected(ContractError::InvalidParameter);
    }
    if (bounds.maximumUv.x <= bounds.minimumUv.x || bounds.maximumUv.y <= bounds.minimumUv.y)
    {
        return std::unexpected(ContractError::InvalidUv);
    }
    if (uv.x < bounds.minimumUv.x || uv.x > bounds.maximumUv.x || uv.y < bounds.minimumUv.y ||
        uv.y > bounds.maximumUv.y)
    {
        return std::unexpected(ContractError::InvalidUv);
    }
    if (uvDirection.x == 0.0 && uvDirection.y == 0.0)
    {
        return std::unexpected(ContractError::InvalidDirection);
    }

    double exitParameter = std::numeric_limits<double>::infinity();
    if (uvDirection.x > 0.0)
    {
        exitParameter = std::min(exitParameter, (bounds.maximumUv.x - uv.x) / uvDirection.x);
    }
    else if (uvDirection.x < 0.0)
    {
        exitParameter = std::min(exitParameter, (bounds.minimumUv.x - uv.x) / uvDirection.x);
    }
    if (uvDirection.y > 0.0)
    {
        exitParameter = std::min(exitParameter, (bounds.maximumUv.y - uv.y) / uvDirection.y);
    }
    else if (uvDirection.y < 0.0)
    {
        exitParameter = std::min(exitParameter, (bounds.minimumUv.y - uv.y) / uvDirection.y);
    }
    if (!IsFinite(exitParameter))
    {
        return std::unexpected(ContractError::InvalidDirection);
    }
    return std::max(exitParameter, minimumAdvance);
}

} // namespace ch29::screen_space_reflections
