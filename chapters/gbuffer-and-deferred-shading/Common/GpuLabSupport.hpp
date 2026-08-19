#pragma once

#include "GBufferContracts.hpp"

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
#include <vector>

namespace ch12::gbuffer::gpu
{

inline constexpr DXGI_FORMAT kBaseColorMetalnessResourceFormat = DXGI_FORMAT_R8G8B8A8_TYPELESS;
inline constexpr DXGI_FORMAT kBaseColorMetalnessViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
inline constexpr DXGI_FORMAT kOctahedralNormalFormat = DXGI_FORMAT_R16G16_UNORM;
inline constexpr DXGI_FORMAT kRoughnessFormat = DXGI_FORMAT_R8_UNORM;
inline constexpr DXGI_FORMAT kDepthResourceFormat = DXGI_FORMAT_R32_TYPELESS;
inline constexpr DXGI_FORMAT kDepthDsvFormat = DXGI_FORMAT_D32_FLOAT;
inline constexpr DXGI_FORMAT kDepthSrvFormat = DXGI_FORMAT_R32_FLOAT;
inline constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
inline constexpr DXGI_FORMAT kIdentityFormat = DXGI_FORMAT_R32_UINT;

enum class SceneMode : std::uint32_t
{
    Full = 0U,
    FrontQuad,
    ClearOnly,
    SharedEdgeQuad,
};

enum class DebugView : std::uint32_t
{
    Final = 0U,
    BaseColor,
    Metalness,
    Normal,
    Roughness,
    Depth,
    Motion,
    Identity,
};

struct TextureReadback final
{
    lgp::framework::Extent2D size{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    std::uint32_t rowPitch{};
    std::vector<std::byte> bytes{};
};

[[nodiscard]] lgp::framework::TextureBarrierState CommonState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState DepthWriteState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState UnorderedAccessState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState DirectQueueShaderResourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState CopySourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState CopyDestState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept;

[[nodiscard]] D3D12_RESOURCE_DESC1 MakeTextureDescription(lgp::framework::Extent2D size, DXGI_FORMAT format,
                                                          D3D12_RESOURCE_FLAGS flags) noexcept;
[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(
    ID3D12Resource &resource, lgp::framework::TextureBarrierState before, lgp::framework::TextureBarrierState after,
    D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) noexcept;
void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers);

[[nodiscard]] std::expected<TextureReadback, lgp::framework::Error> ReadBackTexture(
    lgp::framework::DeviceResources &deviceResources, ID3D12Resource &resource,
    lgp::framework::TextureBarrierState currentState);

[[nodiscard]] float SrgbEncode(float linear) noexcept;
[[nodiscard]] float SrgbDecode(float encoded) noexcept;
[[nodiscard]] std::array<std::uint8_t, 4U> ColorToUnorm8(ch12::gbuffer::Float3 linearColor,
                                                         float alpha = 1.0F) noexcept;

} // namespace ch12::gbuffer::gpu
