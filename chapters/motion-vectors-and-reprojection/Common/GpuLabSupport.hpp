#pragma once

#include "Reprojection.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/device_resources.hpp>
#include <lgp/framework/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace ch11::reprojection::gpu
{

inline constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
inline constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_R32_FLOAT;
inline constexpr DXGI_FORMAT kIdentityFormat = DXGI_FORMAT_R32_UINT;
inline constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
inline constexpr DXGI_FORMAT kUvFormat = DXGI_FORMAT_R32G32_FLOAT;
inline constexpr DXGI_FORMAT kReasonFormat = DXGI_FORMAT_R32_UINT;
inline constexpr DXGI_FORMAT kScalarFormat = DXGI_FORMAT_R32_FLOAT;
inline constexpr std::uint32_t kThreadsX = 8U;
inline constexpr std::uint32_t kThreadsY = 8U;
inline constexpr std::uint32_t kMaxSurfaces = 4U;

enum class Scenario : std::uint32_t
{
    Static = 0U,
    JitterA,
    JitterB,
    MotionPrevious,
    MotionCurrent,
    IdentityDepthPrevious,
    IdentityDepthCurrent,
    ExposurePrevious,
    ExposureValidCurrent,
    ExposureExcessiveCurrent,
    PreviousWCurrent,
};

struct Float4 final
{
    float x{};
    float y{};
    float z{};
    float w{};

    [[nodiscard]] constexpr bool operator==(Float4 const &) const noexcept = default;
};

struct Uint4 final
{
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
    std::uint32_t w{};
};

struct ClipTransformRows final
{
    std::array<Float4, 4U> rows{};
};

struct SurfaceState final
{
    Float4 currentRectUv{};
    Float4 currentColor{};
    std::uint32_t currentIdentity{};
    float currentLinearDepth{};
    float expectedPreviousLinearDepth{};
    std::uint32_t reserved0{};
    std::uint32_t reserved1{};
    ClipTransformRows currentClipFromLocal{};
    ClipTransformRows previousClipFromLocal{};
};

struct ScenarioState final
{
    Scenario scenario{Scenario::Static};
    Float2 currentJitterUv{};
    float currentPreExposure{1.0F};
    std::uint32_t surfaceCount{};
    std::array<SurfaceState, kMaxSurfaces> surfaces{};
};

struct HistoryBuffers final
{
    bool hasHistory{};
    Float2 jitterUv{};
    float preExposure{1.0F};
    std::vector<Float4> color{};
    std::vector<float> linearDepth{};
    std::vector<std::uint32_t> identity{};
};

struct TextureReadback final
{
    lgp::framework::Extent2D size{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    std::uint32_t rowPitch{};
    std::vector<std::byte> bytes{};
};

struct ReferenceOutputs final
{
    std::vector<Float4> currentColor{};
    std::vector<float> currentLinearDepth{};
    std::vector<std::uint32_t> currentIdentity{};
    std::vector<Float4> motionClipDepth{};
    std::vector<Float2> previousHistoryUv{};
    std::vector<Float4> reprojectedHistoryColor{};
    std::vector<std::uint32_t> rejectionReasons{};
    std::vector<float> exposureScale{};
    std::vector<std::array<std::uint8_t, 4U>> composite{};
    HistoryBuffers capturedHistory{};
};

[[nodiscard]] lgp::framework::TextureBarrierState UndefinedState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState ComputeUnorderedAccessState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState ComputeShaderResourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState PixelShaderResourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState CopySourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState CopyDestState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(bool headless) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(bool headless) noexcept;

[[nodiscard]] D3D12_RESOURCE_DESC1 MakeTextureDescription(lgp::framework::Extent2D size, DXGI_FORMAT format,
                                                          D3D12_RESOURCE_FLAGS flags) noexcept;
[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(
    ID3D12Resource &resource, lgp::framework::TextureBarrierState before, lgp::framework::TextureBarrierState after,
    D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) noexcept;
void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers);

[[nodiscard]] ScenarioState MakeScenarioState(Scenario scenario) noexcept;
[[nodiscard]] std::string_view ScenarioLabel(Scenario scenario) noexcept;
[[nodiscard]] HistoryBuffers MakeEmptyHistory(lgp::framework::Extent2D size);
[[nodiscard]] ReferenceOutputs SimulateFrame(ScenarioState const &scenario, HistoryBuffers const &previousHistory,
                                             lgp::framework::Extent2D size, bool resetRequested);
[[nodiscard]] std::expected<TextureReadback, lgp::framework::Error> ReadBackTexture(
    lgp::framework::DeviceResources &deviceResources, ID3D12Resource &resource,
    lgp::framework::TextureBarrierState currentState);

} // namespace ch11::reprojection::gpu
