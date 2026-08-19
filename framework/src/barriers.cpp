#include <lgp/framework/barriers.hpp>

#include <cstdint>

namespace lgp::framework
{

void TransitionTexture(ID3D12GraphicsCommandList7 &commandList, ID3D12Resource &resource,
                       TextureBarrierState const before, TextureBarrierState const after,
                       D3D12_TEXTURE_BARRIER_FLAGS const flags) noexcept
{
    if (before == after && flags == D3D12_TEXTURE_BARRIER_FLAG_NONE)
    {
        return;
    }

    D3D12_TEXTURE_BARRIER barrier{};
    barrier.SyncBefore = before.sync;
    barrier.SyncAfter = after.sync;
    barrier.AccessBefore = before.access;
    barrier.AccessAfter = after.access;
    barrier.LayoutBefore = before.layout;
    barrier.LayoutAfter = after.layout;
    barrier.pResource = &resource;
    barrier.Subresources.IndexOrFirstMipLevel = UINT32_MAX;
    barrier.Flags = flags;

    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1U;
    group.pTextureBarriers = &barrier;
    commandList.Barrier(1U, &group);
}

} // namespace lgp::framework
