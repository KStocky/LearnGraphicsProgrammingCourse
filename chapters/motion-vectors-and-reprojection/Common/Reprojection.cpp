#include "Reprojection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ch11::reprojection
{
namespace
{

[[nodiscard]] bool IsFinite(float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool IsFinite(Float2 value) noexcept
{
    return IsFinite(value.x) && IsFinite(value.y);
}

[[nodiscard]] bool IsFinite(ClipPosition value) noexcept
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z) && IsFinite(value.w);
}

} // namespace

ProjectionResult ProjectUnjittered(ClipPosition clipPosition) noexcept
{
    if (!IsFinite(clipPosition))
    {
        return std::unexpected(ProjectionError::NonFinite);
    }
    if (clipPosition.w <= 0.0F)
    {
        return std::unexpected(ProjectionError::NonPositiveW);
    }

    float const inverseW = 1.0F / clipPosition.w;
    float const ndcX = clipPosition.x * inverseW;
    float const ndcY = clipPosition.y * inverseW;
    float const deviceDepth = clipPosition.z * inverseW;
    ProjectedSurface const projected{
        .unjitteredUv = {.x = ndcX * 0.5F + 0.5F, .y = -ndcY * 0.5F + 0.5F},
        .deviceDepth = deviceDepth,
    };

    if (!IsFinite(projected.unjitteredUv) || !IsFinite(projected.deviceDepth))
    {
        return std::unexpected(ProjectionError::NonFinite);
    }
    return projected;
}

std::expected<Float2, ProjectionError> ComputeUnjitteredMotion(ClipPosition currentUnjitteredClip,
                                                               ClipPosition previousUnjitteredClip) noexcept
{
    auto const current = ProjectUnjittered(currentUnjitteredClip);
    if (!current)
    {
        return std::unexpected(current.error());
    }
    auto const previous = ProjectUnjittered(previousUnjitteredClip);
    if (!previous)
    {
        return std::unexpected(previous.error());
    }

    return Float2{
        .x = previous->unjitteredUv.x - current->unjitteredUv.x,
        .y = previous->unjitteredUv.y - current->unjitteredUv.y,
    };
}

Float2 ReprojectToHistoryUv(Float2 currentRasterUv, Float2 unjitteredMotion, Float2 currentJitterUv,
                            Float2 previousJitterUv) noexcept
{
    return {
        .x = currentRasterUv.x + unjitteredMotion.x + previousJitterUv.x - currentJitterUv.x,
        .y = currentRasterUv.y + unjitteredMotion.y + previousJitterUv.y - currentJitterUv.y,
    };
}

HistoryValidationResult ValidateHistory(HistoryValidationInput const &input,
                                        HistoryValidationSettings const &settings) noexcept
{
    HistoryValidationResult result{};
    if (!input.hasHistory)
    {
        result.reasons |= HistoryRejectReason::NoHistory;
    }
    if (input.resetRequested)
    {
        result.reasons |= HistoryRejectReason::Reset;
    }

    bool const finiteProjection = IsFinite(input.previousHistoryUv) && IsFinite(input.previousClipW) &&
                                  IsFinite(input.expectedPreviousViewDepth) &&
                                  IsFinite(input.sampledPreviousViewDepth) &&
                                  IsFinite(input.localPreviousDepthGradient);
    if (!finiteProjection)
    {
        result.reasons |= HistoryRejectReason::NonFinite;
    }
    if (IsFinite(input.previousClipW) && input.previousClipW <= 0.0F)
    {
        result.reasons |= HistoryRejectReason::PreviousW;
    }

    if (settings.renderWidth == 0U || settings.renderHeight == 0U)
    {
        result.reasons |= HistoryRejectReason::UvOutOfBounds;
    }
    else if (IsFinite(input.previousHistoryUv))
    {
        float const halfTexelX = 0.5F / static_cast<float>(settings.renderWidth);
        float const halfTexelY = 0.5F / static_cast<float>(settings.renderHeight);
        if (input.previousHistoryUv.x < halfTexelX || input.previousHistoryUv.x > 1.0F - halfTexelX ||
            input.previousHistoryUv.y < halfTexelY || input.previousHistoryUv.y > 1.0F - halfTexelY)
        {
            result.reasons |= HistoryRejectReason::UvOutOfBounds;
        }
    }

    if (finiteProjection)
    {
        float const depthScale =
            std::max(std::abs(input.expectedPreviousViewDepth), std::abs(input.sampledPreviousViewDepth));
        result.depthTolerance = std::max({settings.depth.absoluteFloor, settings.depth.relativeScale * depthScale,
                                          settings.depth.gradientScale * std::abs(input.localPreviousDepthGradient)});
        if (std::abs(input.sampledPreviousViewDepth - input.expectedPreviousViewDepth) > result.depthTolerance)
        {
            result.reasons |= HistoryRejectReason::DepthMismatch;
        }
    }

    if (input.currentIdentity != input.sampledPreviousIdentity)
    {
        result.reasons |= HistoryRejectReason::IdentityMismatch;
    }

    bool const validExposure = IsFinite(input.currentPreExposure) && IsFinite(input.previousPreExposure) &&
                               input.currentPreExposure > 0.0F && input.previousPreExposure > 0.0F &&
                               IsFinite(settings.maximumExposureRatio) && settings.maximumExposureRatio >= 1.0F;
    if (validExposure)
    {
        result.historyToCurrentExposureScale = input.currentPreExposure / input.previousPreExposure;
        float const symmetricRatio =
            std::max(result.historyToCurrentExposureScale, 1.0F / result.historyToCurrentExposureScale);
        if (!IsFinite(symmetricRatio) || symmetricRatio > settings.maximumExposureRatio)
        {
            result.reasons |= HistoryRejectReason::Exposure;
        }
    }
    else
    {
        result.reasons |= HistoryRejectReason::Exposure;
        result.historyToCurrentExposureScale = std::numeric_limits<float>::quiet_NaN();
    }

    return result;
}

} // namespace ch11::reprojection
