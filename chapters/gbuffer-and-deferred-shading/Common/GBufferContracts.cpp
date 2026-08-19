#include "GBufferContracts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace ch12::gbuffer
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

[[nodiscard]] bool IsFinite(Float3 value) noexcept
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

[[nodiscard]] float Dot(Float3 first, Float3 second) noexcept
{
    return (first.x * second.x) + (first.y * second.y) + (first.z * second.z);
}

[[nodiscard]] float SignNotZero(float value) noexcept
{
    return value >= 0.0F ? 1.0F : -1.0F;
}

[[nodiscard]] bool CheckedMultiply(std::uint64_t left, std::uint64_t right, std::uint64_t &result) noexcept
{
    if (right != 0U && left > std::numeric_limits<std::uint64_t>::max() / right)
    {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool CheckedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t &result) noexcept
{
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
    {
        return false;
    }
    result = left + right;
    return true;
}

} // namespace

std::expected<DeviceDepthCoefficients, ContractError> MakeDeviceDepthCoefficients(
    PerspectiveProjection projection) noexcept
{
    if (!IsFinite(projection.verticalFieldOfViewRadians) || !IsFinite(projection.aspectRatio) ||
        !IsFinite(projection.nearPlane) || !IsFinite(projection.farPlane))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (projection.verticalFieldOfViewRadians <= 0.0F ||
        projection.verticalFieldOfViewRadians >= std::numbers::pi_v<float> || projection.aspectRatio <= 0.0F ||
        projection.nearPlane <= 0.0F || projection.farPlane <= projection.nearPlane)
    {
        return std::unexpected(ContractError::InvalidProjection);
    }

    float const range = projection.farPlane - projection.nearPlane;
    if (projection.depthConvention == DepthConvention::Forward)
    {
        return DeviceDepthCoefficients{
            .additive = projection.farPlane / range,
            .reciprocal = -(projection.nearPlane * projection.farPlane) / range,
        };
    }
    if (projection.depthConvention == DepthConvention::Reversed)
    {
        return DeviceDepthCoefficients{
            .additive = -projection.nearPlane / range,
            .reciprocal = (projection.nearPlane * projection.farPlane) / range,
        };
    }
    return std::unexpected(ContractError::InvalidProjection);
}

std::expected<float, ContractError> DeviceDepthFromViewDepth(float viewDepth, PerspectiveProjection projection) noexcept
{
    if (!IsFinite(viewDepth))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (viewDepth < projection.nearPlane || viewDepth > projection.farPlane)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }
    auto const coefficients = MakeDeviceDepthCoefficients(projection);
    if (!coefficients)
    {
        return std::unexpected(coefficients.error());
    }

    float const deviceDepth = coefficients->additive + (coefficients->reciprocal / viewDepth);
    if (!IsFinite(deviceDepth))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return deviceDepth;
}

std::expected<float, ContractError> ViewDepthFromDeviceDepth(float deviceDepth,
                                                             PerspectiveProjection projection) noexcept
{
    if (!IsFinite(deviceDepth))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (deviceDepth < 0.0F || deviceDepth > 1.0F)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }
    auto const coefficients = MakeDeviceDepthCoefficients(projection);
    if (!coefficients)
    {
        return std::unexpected(coefficients.error());
    }

    float const denominator = deviceDepth - coefficients->additive;
    if (denominator == 0.0F)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }
    float const viewDepth = coefficients->reciprocal / denominator;
    if (!IsFinite(viewDepth) || viewDepth < projection.nearPlane || viewDepth > projection.farPlane)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }
    return viewDepth;
}

std::expected<Float3, ContractError> ReconstructViewPosition(std::uint32_t pixelX, std::uint32_t pixelY,
                                                             std::uint32_t renderWidth, std::uint32_t renderHeight,
                                                             float deviceDepth,
                                                             PerspectiveProjection projection) noexcept
{
    if (renderWidth == 0U || renderHeight == 0U || pixelX >= renderWidth || pixelY >= renderHeight)
    {
        return std::unexpected(ContractError::InvalidExtent);
    }
    auto const viewDepth = ViewDepthFromDeviceDepth(deviceDepth, projection);
    if (!viewDepth)
    {
        return std::unexpected(viewDepth.error());
    }

    float const uvX = (static_cast<float>(pixelX) + 0.5F) / static_cast<float>(renderWidth);
    float const uvY = (static_cast<float>(pixelY) + 0.5F) / static_cast<float>(renderHeight);
    float const ndcX = (2.0F * uvX) - 1.0F;
    float const ndcY = 1.0F - (2.0F * uvY);
    float const tangentHalfFov = std::tan(projection.verticalFieldOfViewRadians * 0.5F);
    Float3 const viewPosition{
        .x = ndcX * *viewDepth * tangentHalfFov * projection.aspectRatio,
        .y = ndcY * *viewDepth * tangentHalfFov,
        .z = *viewDepth,
    };
    if (!IsFinite(viewPosition))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return viewPosition;
}

std::expected<Float2, ContractError> EncodeOctahedralNormal(Float3 unitNormal) noexcept
{
    if (!IsFinite(unitNormal))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    float const squaredLength = Dot(unitNormal, unitNormal);
    if (squaredLength < 0.999F || squaredLength > 1.001F)
    {
        return std::unexpected(ContractError::DegenerateNormal);
    }

    float const inverseL1 = 1.0F / (std::abs(unitNormal.x) + std::abs(unitNormal.y) + std::abs(unitNormal.z));
    Float2 projected{unitNormal.x * inverseL1, unitNormal.y * inverseL1};
    if (unitNormal.z < 0.0F)
    {
        Float2 const folded{
            (1.0F - std::abs(projected.y)) * SignNotZero(projected.x),
            (1.0F - std::abs(projected.x)) * SignNotZero(projected.y),
        };
        projected = folded;
    }
    return Float2{projected.x * 0.5F + 0.5F, projected.y * 0.5F + 0.5F};
}

std::expected<Float3, ContractError> DecodeOctahedralNormal(Float2 encoded) noexcept
{
    if (!IsFinite(encoded))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (encoded.x < 0.0F || encoded.x > 1.0F || encoded.y < 0.0F || encoded.y > 1.0F)
    {
        return std::unexpected(ContractError::ValueOutOfRange);
    }

    Float3 normal{
        .x = encoded.x * 2.0F - 1.0F,
        .y = encoded.y * 2.0F - 1.0F,
        .z = 1.0F - std::abs(encoded.x * 2.0F - 1.0F) - std::abs(encoded.y * 2.0F - 1.0F),
    };
    float const fold = std::clamp(-normal.z, 0.0F, 1.0F);
    normal.x += normal.x >= 0.0F ? -fold : fold;
    normal.y += normal.y >= 0.0F ? -fold : fold;

    float const squaredLength = Dot(normal, normal);
    if (squaredLength <= std::numeric_limits<float>::min())
    {
        return std::unexpected(ContractError::DegenerateNormal);
    }
    float const inverseLength = 1.0F / std::sqrt(squaredLength);
    normal = {normal.x * inverseLength, normal.y * inverseLength, normal.z * inverseLength};
    return normal;
}

std::expected<float, ContractError> QuantizeUnorm(float value, std::uint8_t bitCount) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (value < 0.0F || value > 1.0F || bitCount == 0U || bitCount > 16U)
    {
        return std::unexpected(ContractError::ValueOutOfRange);
    }

    std::uint32_t const maximum = (1U << bitCount) - 1U;
    return std::round(value * static_cast<float>(maximum)) / static_cast<float>(maximum);
}

std::expected<LogicalGBufferTraffic, ContractError> ComputeLogicalTraffic(
    std::uint32_t renderWidth, std::uint32_t renderHeight, std::span<AttachmentStorage const> attachments) noexcept
{
    if (renderWidth == 0U || renderHeight == 0U || attachments.empty())
    {
        return std::unexpected(ContractError::InvalidExtent);
    }

    std::array<bool, 6U> seen{};
    std::uint64_t bytesPerPixel = 0U;
    for (AttachmentStorage const attachment : attachments)
    {
        std::size_t const semanticIndex = static_cast<std::size_t>(attachment.semantic);
        if (semanticIndex >= seen.size())
        {
            return std::unexpected(ContractError::ValueOutOfRange);
        }
        if (seen[semanticIndex])
        {
            return std::unexpected(ContractError::DuplicateAttachment);
        }
        if (attachment.bytesPerPixel == 0U)
        {
            return std::unexpected(ContractError::InvalidBytesPerPixel);
        }
        seen[semanticIndex] = true;
        if (!CheckedAdd(bytesPerPixel, attachment.bytesPerPixel, bytesPerPixel))
        {
            return std::unexpected(ContractError::ArithmeticOverflow);
        }
    }

    std::uint64_t pixelCount{};
    std::uint64_t residentBytes{};
    std::uint64_t totalPayloadBytes{};
    if (!CheckedMultiply(renderWidth, renderHeight, pixelCount) ||
        !CheckedMultiply(pixelCount, bytesPerPixel, residentBytes) ||
        !CheckedMultiply(residentBytes, 2U, totalPayloadBytes))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return LogicalGBufferTraffic{
        .residentBytes = residentBytes,
        .rasterWriteBytes = residentBytes,
        .deferredReadBytes = residentBytes,
        .totalPayloadBytes = totalPayloadBytes,
    };
}

} // namespace ch12::gbuffer
