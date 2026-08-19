#pragma once

#include <d3d12.h>

namespace lgp::framework
{

struct TextureBarrierState final
{
    D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};
    D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};
    D3D12_BARRIER_LAYOUT layout{D3D12_BARRIER_LAYOUT_COMMON};

    constexpr bool operator==(TextureBarrierState const &) const noexcept = default;
};

void TransitionTexture(ID3D12GraphicsCommandList7 &commandList, ID3D12Resource &resource, TextureBarrierState before,
                       TextureBarrierState after,
                       D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) noexcept;

} // namespace lgp::framework
