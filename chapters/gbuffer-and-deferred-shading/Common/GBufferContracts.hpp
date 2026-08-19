#pragma once

#include <cstdint>
#include <expected>
#include <span>

namespace ch12::gbuffer
{

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

enum class ContractError : std::uint8_t
{
    NonFinite = 0U,
    ValueOutOfRange,
    DegenerateNormal,
    InvalidExtent,
    InvalidProjection,
    InvalidDepth,
    InvalidBytesPerPixel,
    DuplicateAttachment,
    ArithmeticOverflow,
};

enum class DepthConvention : std::uint8_t
{
    Forward = 0U,
    Reversed,
};

struct PerspectiveProjection final
{
    float verticalFieldOfViewRadians{};
    float aspectRatio{};
    float nearPlane{};
    float farPlane{};
    DepthConvention depthConvention{DepthConvention::Forward};
};

struct DeviceDepthCoefficients final
{
    float additive{};
    float reciprocal{};
};

[[nodiscard]] std::expected<DeviceDepthCoefficients, ContractError> MakeDeviceDepthCoefficients(
    PerspectiveProjection projection) noexcept;
[[nodiscard]] std::expected<float, ContractError> DeviceDepthFromViewDepth(float viewDepth,
                                                                           PerspectiveProjection projection) noexcept;
[[nodiscard]] std::expected<float, ContractError> ViewDepthFromDeviceDepth(float deviceDepth,
                                                                           PerspectiveProjection projection) noexcept;
[[nodiscard]] std::expected<Float3, ContractError> ReconstructViewPosition(std::uint32_t pixelX, std::uint32_t pixelY,
                                                                           std::uint32_t renderWidth,
                                                                           std::uint32_t renderHeight,
                                                                           float deviceDepth,
                                                                           PerspectiveProjection projection) noexcept;

[[nodiscard]] std::expected<Float2, ContractError> EncodeOctahedralNormal(Float3 unitNormal) noexcept;
[[nodiscard]] std::expected<Float3, ContractError> DecodeOctahedralNormal(Float2 encoded) noexcept;
[[nodiscard]] std::expected<float, ContractError> QuantizeUnorm(float value, std::uint8_t bitCount) noexcept;

enum class AttachmentSemantic : std::uint8_t
{
    BaseColorMetalness = 0U,
    OctahedralNormal,
    Roughness,
    DeviceDepth,
    Motion,
    Identity,
};

enum class SamplingRule : std::uint8_t
{
    ExactPixel = 0U,
    BilinearSignal,
};

[[nodiscard]] constexpr SamplingRule DeferredSamplingRule(AttachmentSemantic) noexcept
{
    return SamplingRule::ExactPixel;
}

[[nodiscard]] constexpr bool IsCategorical(AttachmentSemantic semantic) noexcept
{
    return semantic == AttachmentSemantic::Identity;
}

[[nodiscard]] constexpr float DepthClearValue(DepthConvention convention) noexcept
{
    return convention == DepthConvention::Forward ? 1.0F : 0.0F;
}

[[nodiscard]] constexpr bool IsBackgroundDepth(float deviceDepth, DepthConvention convention) noexcept
{
    return deviceDepth == DepthClearValue(convention);
}

[[nodiscard]] constexpr bool IsBackgroundIdentity(std::uint32_t identity) noexcept
{
    return identity == 0U;
}

struct AttachmentStorage final
{
    AttachmentSemantic semantic{};
    std::uint32_t bytesPerPixel{};
};

struct LogicalGBufferTraffic final
{
    std::uint64_t residentBytes{};
    std::uint64_t rasterWriteBytes{};
    std::uint64_t deferredReadBytes{};
    std::uint64_t totalPayloadBytes{};
};

[[nodiscard]] std::expected<LogicalGBufferTraffic, ContractError> ComputeLogicalTraffic(
    std::uint32_t renderWidth, std::uint32_t renderHeight, std::span<AttachmentStorage const> attachments) noexcept;

} // namespace ch12::gbuffer
