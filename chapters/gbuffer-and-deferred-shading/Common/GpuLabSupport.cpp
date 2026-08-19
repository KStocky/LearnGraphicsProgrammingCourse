#include "GpuLabSupport.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace ch12::gbuffer::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

[[nodiscard]] std::uint64_t BytesPerPixel(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32_UINT:
        return 4U;
    case DXGI_FORMAT_R8_UNORM:
        return 1U;
    default:
        return 0U;
    }
}

[[nodiscard]] lgp::framework::Error MakeUnsupportedReadbackFormatError(DXGI_FORMAT format)
{
    return lgp::framework::MakeError("ReadBackTexture",
                                     "Unsupported Chapter 12 readback format " + std::to_string(format) + ".");
}

} // namespace

lgp::framework::TextureBarrierState CommonState() noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_COMMON};
}

lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_RENDER_TARGET,
        D3D12_BARRIER_ACCESS_RENDER_TARGET,
        D3D12_BARRIER_LAYOUT_RENDER_TARGET,
    };
}

lgp::framework::TextureBarrierState DepthWriteState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_DEPTH_STENCIL,
        D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
        D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
    };
}

lgp::framework::TextureBarrierState UnorderedAccessState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,
        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
        D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
    };
}

lgp::framework::TextureBarrierState DirectQueueShaderResourceState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_PIXEL_SHADING,
        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
        D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
    };
}

lgp::framework::TextureBarrierState CopySourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_SOURCE};
}

lgp::framework::TextureBarrierState CopyDestState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_DEST};
}

lgp::framework::TextureBarrierState FrameStartState(lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frameContext.renderTargetInitialLayout};
}

lgp::framework::TextureBarrierState FrameEndState(lgp::framework::FrameContext const &frameContext) noexcept
{
    return {
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT,
    };
}

D3D12_RESOURCE_DESC1 MakeTextureDescription(lgp::framework::Extent2D const size, DXGI_FORMAT const format,
                                            D3D12_RESOURCE_FLAGS const flags) noexcept
{
    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = size.width;
    description.Height = size.height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = format;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;
    return description;
}

D3D12_TEXTURE_BARRIER MakeTextureBarrier(ID3D12Resource &resource, lgp::framework::TextureBarrierState const before,
                                         lgp::framework::TextureBarrierState const after,
                                         D3D12_TEXTURE_BARRIER_FLAGS const flags) noexcept
{
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
    return barrier;
}

void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers)
{
    if (barriers.empty())
    {
        return;
    }

    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = static_cast<UINT>(barriers.size());
    group.pTextureBarriers = barriers.data();
    commandList.Barrier(1U, &group);
}

std::expected<TextureReadback, lgp::framework::Error> ReadBackTexture(lgp::framework::DeviceResources &deviceResources,
                                                                      ID3D12Resource &resource,
                                                                      lgp::framework::TextureBarrierState currentState)
{
    auto const idle = deviceResources.WaitForGpuIdle();
    if (!idle)
    {
        return std::unexpected(idle.error());
    }

    D3D12_RESOURCE_DESC const sourceDescription = resource.GetDesc();
    if (BytesPerPixel(sourceDescription.Format) == 0U)
    {
        return std::unexpected(MakeUnsupportedReadbackFormatError(sourceDescription.Format));
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0U;
    UINT64 rowSize = 0U;
    UINT64 totalBytes = 0U;
    deviceResources.device()->GetCopyableFootprints(&sourceDescription, 0U, 1U, 0U, &footprint, &rowCount, &rowSize,
                                                    &totalBytes);

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readbackDescription{};
    readbackDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDescription.Width = totalBytes;
    readbackDescription.Height = 1U;
    readbackDescription.DepthOrArraySize = 1U;
    readbackDescription.MipLevels = 1U;
    readbackDescription.SampleDesc.Count = 1U;
    readbackDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readbackBuffer{};
    HRESULT const readbackResult = deviceResources.device()->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(readbackBuffer.ReleaseAndGetAddressOf()));
    if (FAILED(readbackResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommittedResource", readbackResult,
                                                                "Failed to create a Chapter 12 readback buffer."));
    }

    ComPtr<ID3D12CommandAllocator> allocator{};
    HRESULT const allocatorResult = deviceResources.device()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf()));
    if (FAILED(allocatorResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandAllocator", allocatorResult,
                                                                "Failed to create a Chapter 12 readback allocator."));
    }

    ComPtr<ID3D12GraphicsCommandList7> commandList{};
    HRESULT const listResult =
        deviceResources.device()->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                    IID_PPV_ARGS(commandList.ReleaseAndGetAddressOf()));
    if (FAILED(listResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandList", listResult,
                                                                "Failed to create a Chapter 12 readback list."));
    }

    std::vector<D3D12_TEXTURE_BARRIER> barriers{
        MakeTextureBarrier(resource, currentState, CopySourceState()),
    };
    SubmitTextureBarriers(*commandList.Get(), barriers);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readbackBuffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = &resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0U;
    commandList->CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);

    barriers = {
        MakeTextureBarrier(resource, CopySourceState(), currentState),
    };
    SubmitTextureBarriers(*commandList.Get(), barriers);

    HRESULT const closeResult = commandList->Close();
    if (FAILED(closeResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12GraphicsCommandList::Close", closeResult,
                                                                "Failed to close a Chapter 12 readback list."));
    }

    ID3D12CommandList *const lists[]{commandList.Get()};
    deviceResources.graphics_queue()->ExecuteCommandLists(1U, lists);

    ComPtr<ID3D12Fence> fence{};
    HRESULT const fenceResult =
        deviceResources.device()->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.ReleaseAndGetAddressOf()));
    if (FAILED(fenceResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateFence", fenceResult,
                                                                "Failed to create a Chapter 12 readback fence."));
    }

    HANDLE const eventHandle = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (eventHandle == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeLastError("CreateEventW", "Failed to create a Chapter 12 readback event."));
    }

    constexpr std::uint64_t kFenceValue = 1U;
    HRESULT const signalResult = deviceResources.graphics_queue()->Signal(fence.Get(), kFenceValue);
    if (FAILED(signalResult))
    {
        ::CloseHandle(eventHandle);
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12CommandQueue::Signal", signalResult,
                                                                "Failed to signal a Chapter 12 readback fence."));
    }

    HRESULT const setEventResult = fence->SetEventOnCompletion(kFenceValue, eventHandle);
    if (FAILED(setEventResult))
    {
        ::CloseHandle(eventHandle);
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Fence::SetEventOnCompletion", setEventResult,
                                             "Failed to register a Chapter 12 readback completion event."));
    }

    DWORD const waitResult = ::WaitForSingleObject(eventHandle, INFINITE);
    ::CloseHandle(eventHandle);
    if (waitResult != WAIT_OBJECT_0)
    {
        return std::unexpected(
            lgp::framework::MakeLastError("WaitForSingleObject", "Failed while waiting for Chapter 12 readback."));
    }

    TextureReadback readback{};
    readback.size = {
        static_cast<std::uint32_t>(sourceDescription.Width),
        sourceDescription.Height,
    };
    readback.format = sourceDescription.Format;
    readback.rowPitch = footprint.Footprint.RowPitch;
    readback.bytes.resize(static_cast<std::size_t>(totalBytes));

    void *mappedData = nullptr;
    D3D12_RANGE const readRange{0U, static_cast<SIZE_T>(totalBytes)};
    HRESULT const mapResult = readbackBuffer->Map(0U, &readRange, &mappedData);
    if (FAILED(mapResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                "Failed to map a Chapter 12 readback buffer."));
    }

    std::memcpy(readback.bytes.data(), mappedData, static_cast<std::size_t>(totalBytes));
    D3D12_RANGE const writtenRange{0U, 0U};
    readbackBuffer->Unmap(0U, &writtenRange);
    return readback;
}

float SrgbEncode(float const linear) noexcept
{
    float const clamped = std::clamp(linear, 0.0F, 1.0F);
    return clamped <= 0.0031308F ? clamped * 12.92F : (1.055F * std::pow(clamped, 1.0F / 2.4F)) - 0.055F;
}

float SrgbDecode(float const encoded) noexcept
{
    float const clamped = std::clamp(encoded, 0.0F, 1.0F);
    return clamped <= 0.04045F ? clamped / 12.92F : std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

std::array<std::uint8_t, 4U> ColorToUnorm8(ch12::gbuffer::Float3 const linearColor, float const alpha) noexcept
{
    auto const quantize = [](float value) noexcept
    { return static_cast<std::uint8_t>(std::clamp(std::lround(value * 255.0F), 0L, 255L)); };

    return {
        quantize(SrgbEncode(linearColor.x)),
        quantize(SrgbEncode(linearColor.y)),
        quantize(SrgbEncode(linearColor.z)),
        quantize(std::clamp(alpha, 0.0F, 1.0F)),
    };
}

} // namespace ch12::gbuffer::gpu
