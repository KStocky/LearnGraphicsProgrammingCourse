#pragma once

#include <cstdint>
#include <expected>

namespace ch11::reprojection
{

struct Float2 final
{
    float x{};
    float y{};

    [[nodiscard]] constexpr bool operator==(Float2 const &) const noexcept = default;
};

struct ClipPosition final
{
    float x{};
    float y{};
    float z{};
    float w{};
};

enum class ProjectionError : std::uint8_t
{
    NonFinite = 0U,
    NonPositiveW,
};

struct ProjectedSurface final
{
    Float2 unjitteredUv{};
    float deviceDepth{};
};

using ProjectionResult = std::expected<ProjectedSurface, ProjectionError>;

[[nodiscard]] ProjectionResult ProjectUnjittered(ClipPosition clipPosition) noexcept;

[[nodiscard]] std::expected<Float2, ProjectionError> ComputeUnjitteredMotion(
    ClipPosition currentUnjitteredClip, ClipPosition previousUnjitteredClip) noexcept;

[[nodiscard]] Float2 ReprojectToHistoryUv(Float2 currentRasterUv, Float2 unjitteredMotion, Float2 currentJitterUv,
                                          Float2 previousJitterUv) noexcept;

enum class HistoryRejectReason : std::uint32_t
{
    None = 0U,
    NoHistory = 1U << 0U,
    Reset = 1U << 1U,
    NonFinite = 1U << 2U,
    PreviousW = 1U << 3U,
    UvOutOfBounds = 1U << 4U,
    DepthMismatch = 1U << 5U,
    IdentityMismatch = 1U << 6U,
    Exposure = 1U << 7U,
};

[[nodiscard]] constexpr HistoryRejectReason operator|(HistoryRejectReason left, HistoryRejectReason right) noexcept
{
    return static_cast<HistoryRejectReason>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr HistoryRejectReason &operator|=(HistoryRejectReason &left, HistoryRejectReason right) noexcept
{
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool HasReason(HistoryRejectReason reasons, HistoryRejectReason reason) noexcept
{
    return (static_cast<std::uint32_t>(reasons) & static_cast<std::uint32_t>(reason)) != 0U;
}

struct DepthValidationSettings final
{
    float absoluteFloor{0.001F};
    float relativeScale{0.005F};
    float gradientScale{1.0F};
};

struct HistoryValidationSettings final
{
    std::uint32_t renderWidth{};
    std::uint32_t renderHeight{};
    DepthValidationSettings depth{};
    float maximumExposureRatio{4.0F};
};

struct HistoryValidationInput final
{
    bool hasHistory{};
    bool resetRequested{};
    Float2 previousHistoryUv{};
    float previousClipW{};
    float expectedPreviousViewDepth{};
    float sampledPreviousViewDepth{};
    float localPreviousDepthGradient{};
    std::uint32_t currentIdentity{};
    std::uint32_t sampledPreviousIdentity{};
    float currentPreExposure{1.0F};
    float previousPreExposure{1.0F};
};

struct HistoryValidationResult final
{
    HistoryRejectReason reasons{HistoryRejectReason::None};
    float depthTolerance{};
    float historyToCurrentExposureScale{1.0F};

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return reasons == HistoryRejectReason::None;
    }
};

[[nodiscard]] HistoryValidationResult ValidateHistory(HistoryValidationInput const &input,
                                                      HistoryValidationSettings const &settings) noexcept;

} // namespace ch11::reprojection
