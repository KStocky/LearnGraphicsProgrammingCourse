#include "GpuLabSupport.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace ch14::clustered_lighting::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

[[nodiscard]] lgp::framework::Error MakeContractError(char const *operation, ContractError error)
{
    return lgp::framework::MakeError(operation, "Chapter 14 clustered-lighting contract failed with code " +
                                                    std::to_string(static_cast<std::uint32_t>(error)) + ".");
}

[[nodiscard]] std::expected<float, lgp::framework::Error> ReadDepth(TextureReadback const &depth, std::uint32_t x,
                                                                    std::uint32_t y)
{
    if (depth.format != kDepthResourceFormat && depth.format != kDepthSrvFormat)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadDepth", "The Chapter 14 depth readback has an unexpected format."));
    }
    if (x >= depth.size.width || y >= depth.size.height)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadDepth", "The Chapter 14 depth coordinate is out of range."));
    }

    std::size_t const offset =
        static_cast<std::size_t>(y) * depth.rowPitch + static_cast<std::size_t>(x) * sizeof(float);
    if (offset + sizeof(float) > depth.bytes.size())
    {
        return std::unexpected(lgp::framework::MakeError("ReadDepth", "The Chapter 14 depth readback is truncated."));
    }

    float value{};
    std::memcpy(&value, depth.bytes.data() + offset, sizeof(value));
    return value;
}

} // namespace

BufferResource::BufferResource(BufferResource &&other) noexcept
{
    *this = std::move(other);
}

BufferResource &BufferResource::operator=(BufferResource &&other) noexcept
{
    if (this != &other)
    {
        Reset();
        resource_ = std::move(other.resource_);
        sizeInBytes_ = std::exchange(other.sizeInBytes_, 0U);
        mappedData_ = std::exchange(other.mappedData_, nullptr);
    }
    return *this;
}

BufferResource::~BufferResource()
{
    Reset();
}

void BufferResource::Reset() noexcept
{
    if (resource_ != nullptr && mappedData_ != nullptr)
    {
        D3D12_RANGE const writtenRange{0U, 0U};
        resource_->Unmap(0U, &writtenRange);
    }
    resource_.Reset();
    sizeInBytes_ = 0U;
    mappedData_ = nullptr;
}

std::expected<std::uint32_t, lgp::framework::Error> CellCountForMode(LightingMode mode, lgp::framework::Extent2D size)
{
    auto const tiles = MakeTileGrid(size.width, size.height, kTileWidth, kTileHeight);
    if (!tiles)
    {
        return std::unexpected(MakeContractError("CellCountForMode", tiles.error()));
    }
    if (mode != LightingMode::Clustered)
    {
        return tiles->tileCount;
    }
    auto const clusters = MakeClusterGrid(*tiles, kClusterSliceCount);
    if (!clusters)
    {
        return std::unexpected(MakeContractError("CellCountForMode", clusters.error()));
    }
    return clusters->clusterCount;
}

PerspectiveProjection MakeProjection(lgp::framework::Extent2D size, DepthConvention depthConvention) noexcept
{
    float const aspectRatio =
        size.height == 0U ? 1.0F : static_cast<float>(size.width) / static_cast<float>(size.height);
    return {
        .verticalFieldOfViewRadians = kVerticalFieldOfView,
        .aspectRatio = aspectRatio,
        .nearPlane = kNearPlane,
        .farPlane = kFarPlane,
        .depthConvention = depthConvention,
    };
}

LogDepthSlicing MakeSlicing() noexcept
{
    return {.nearDepth = kNearPlane, .farDepth = kFarPlane, .sliceCount = kClusterSliceCount};
}

std::array<Vertex, 4U> BuildUnitQuadVertices() noexcept
{
    return {
        Vertex{{-0.5F, 0.5F, 0.0F}, {0.0F, 0.0F, -1.0F}},
        Vertex{{0.5F, 0.5F, 0.0F}, {0.0F, 0.0F, -1.0F}},
        Vertex{{-0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, -1.0F}},
        Vertex{{0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, -1.0F}},
    };
}

std::array<std::uint32_t, 6U> BuildUnitQuadIndices() noexcept
{
    return {0U, 1U, 2U, 1U, 3U, 2U};
}

std::vector<SceneObject> BuildScene(ScenePreset scene)
{
    if (scene == ScenePreset::Empty)
    {
        return {};
    }
    return {
        {{{-2.2F, 0.9F, 5.0F}, 0.72F, {4.8F, 3.4F, 1.0F}, 0.05F, {0.56F, 0.18F, 0.10F}, 0.0F}},
        {{{2.0F, 1.25F, 10.0F}, 0.36F, {5.0F, 4.0F, 1.0F}, 0.25F, {0.12F, 0.38F, 0.76F}, 0.0F}},
        {{{-0.4F, -2.25F, 18.0F}, 0.58F, {10.0F, 2.8F, 1.0F}, 0.7F, {0.62F, 0.58F, 0.18F}, 0.0F}},
        {{{0.0F, 2.9F, 28.0F}, 0.22F, {13.0F, 2.0F, 1.0F}, 0.1F, {0.18F, 0.64F, 0.42F}, 0.0F}},
    };
}

std::vector<PointLightData> BuildLights(LightPreset preset, std::uint32_t lightCount)
{
    lightCount = NormalizeLightCount(lightCount);
    std::vector<PointLightData> lights{};
    lights.reserve(lightCount);
    for (std::uint32_t index = 0U; index < lightCount; ++index)
    {
        std::uint32_t const column = index % 8U;
        std::uint32_t const row = index / 8U;
        if (preset == LightPreset::Offscreen)
        {
            lights.push_back({
                .position = {80.0F + static_cast<float>(column), 70.0F + static_cast<float>(row), 8.0F},
                .radius = 2.0F,
                .color = {0.8F, 0.7F, 0.6F},
                .intensity = 6.0F,
            });
            continue;
        }

        float const red = 0.35F + (0.09F * static_cast<float>((index * 3U) % 7U));
        float const green = 0.30F + (0.08F * static_cast<float>((index * 5U) % 8U));
        float const blue = 0.38F + (0.075F * static_cast<float>((index * 7U) % 7U));
        lights.push_back({
            .position =
                {
                    -5.6F + (1.6F * static_cast<float>(column)),
                    -2.8F + (0.82F * static_cast<float>(row)),
                    3.0F + (3.7F * static_cast<float>((index + row) % 7U)),
                },
            .radius = 3.0F + (0.45F * static_cast<float>(index % 4U)),
            .color = {red, green, blue},
            .intensity = 5.0F + (0.6F * static_cast<float>(index % 5U)),
        });
    }
    return lights;
}

lgp::framework::TextureBarrierState CommonState() noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_COMMON};
}

lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
}

lgp::framework::TextureBarrierState DepthWriteState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_DEPTH_STENCIL,
        D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
        D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
    };
}

lgp::framework::TextureBarrierState ComputeShaderResourceState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
        D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE,
    };
}

lgp::framework::TextureBarrierState DepthReadState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_DEPTH_STENCIL,
        D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ,
        D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ,
    };
}

lgp::framework::TextureBarrierState CopySourceTextureState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_SOURCE};
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

BufferBarrierState NoAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS};
}

BufferBarrierState ComputeUnorderedAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
}

BufferBarrierState PixelShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
}

BufferBarrierState CopySourceBufferState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE};
}

D3D12_RESOURCE_DESC1 MakeTextureDescription(lgp::framework::Extent2D size, DXGI_FORMAT format,
                                            D3D12_RESOURCE_FLAGS flags) noexcept
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

D3D12_TEXTURE_BARRIER MakeTextureBarrier(ID3D12Resource &resource, lgp::framework::TextureBarrierState before,
                                         lgp::framework::TextureBarrierState after,
                                         D3D12_TEXTURE_BARRIER_FLAGS flags) noexcept
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

D3D12_BUFFER_BARRIER MakeBufferBarrier(ID3D12Resource &resource, BufferBarrierState before, BufferBarrierState after,
                                       std::uint64_t offset, std::uint64_t size) noexcept
{
    D3D12_BUFFER_BARRIER barrier{};
    barrier.SyncBefore = before.sync;
    barrier.SyncAfter = after.sync;
    barrier.AccessBefore = before.access;
    barrier.AccessAfter = after.access;
    barrier.pResource = &resource;
    barrier.Offset = offset;
    barrier.Size = size;
    return barrier;
}

void SubmitBufferBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_BUFFER_BARRIER> &barriers)
{
    if (barriers.empty())
    {
        return;
    }
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_BUFFER;
    group.NumBarriers = static_cast<UINT>(barriers.size());
    group.pBufferBarriers = barriers.data();
    commandList.Barrier(1U, &group);
}

std::expected<BufferResource, lgp::framework::Error> CreateBuffer(ID3D12Device10 &device, std::uint64_t sizeInBytes,
                                                                  D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
                                                                  std::wstring_view name, bool mapPersistently)
{
    if (sizeInBytes == 0U)
    {
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 14 buffers must be non-empty."));
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    heap.CreationNodeMask = 1U;
    heap.VisibleNodeMask = 1U;

    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = sizeInBytes;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;

    BufferResource buffer{};
    HRESULT const result = device.CreateCommittedResource3(
        &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0U, nullptr,
        IID_PPV_ARGS(buffer.resource_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                                "Failed to create a Chapter 14 buffer."));
    }

    if (!name.empty())
    {
        std::wstring const resourceName{name};
        HRESULT const nameResult = buffer.resource_->SetName(resourceName.c_str());
        if (FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name a Chapter 14 buffer."));
        }
    }

    if (mapPersistently)
    {
        D3D12_RANGE const readRange{0U, 0U};
        void *mapped = nullptr;
        HRESULT const mapResult = buffer.resource_->Map(0U, &readRange, &mapped);
        if (FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map a Chapter 14 buffer."));
        }
        buffer.mappedData_ = static_cast<std::byte *>(mapped);
    }

    buffer.sizeInBytes_ = sizeInBytes;
    return buffer;
}

lgp::framework::Status WriteBuffer(BufferResource &buffer, std::span<std::byte const> bytes,
                                   std::uint64_t destinationOffset)
{
    if (bytes.empty())
    {
        return {};
    }
    if (buffer.mapped_data() == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("WriteBuffer", "The Chapter 14 buffer is not persistently mapped."));
    }
    if (destinationOffset > buffer.size_in_bytes() || bytes.size_bytes() > buffer.size_in_bytes() - destinationOffset)
    {
        return std::unexpected(
            lgp::framework::MakeError("WriteBuffer", "The Chapter 14 buffer write is out of range."));
    }
    std::memcpy(buffer.mapped_data() + destinationOffset, bytes.data(), bytes.size_bytes());
    return {};
}

lgp::framework::Status CompileShader(lgp::framework::ShaderCompiler &compiler,
                                     lgp::framework::ShaderCompileOptions &options, wchar_t const *entryPoint,
                                     wchar_t const *targetProfile, lgp::framework::CompiledShader &shader)
{
    options.entryPoint = entryPoint;
    options.targetProfile = targetProfile;
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto result = compiler.Compile(options);
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    shader = std::move(*result);
    return {};
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
    if (sourceDescription.Format != kDepthResourceFormat && sourceDescription.Format != kDepthSrvFormat)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackTexture", "Unsupported Chapter 14 readback texture format."));
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0U;
    UINT64 rowSize = 0U;
    UINT64 totalBytes = 0U;
    deviceResources.device()->GetCopyableFootprints(&sourceDescription, 0U, 1U, 0U, &footprint, &rowCount, &rowSize,
                                                    &totalBytes);

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC1 readbackDescription{};
    readbackDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDescription.Width = totalBytes;
    readbackDescription.Height = 1U;
    readbackDescription.DepthOrArraySize = 1U;
    readbackDescription.MipLevels = 1U;
    readbackDescription.SampleDesc.Count = 1U;
    readbackDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readbackBuffer{};
    HRESULT const readbackResult = deviceResources.device()->CreateCommittedResource3(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0U,
        nullptr, IID_PPV_ARGS(readbackBuffer.ReleaseAndGetAddressOf()));
    if (FAILED(readbackResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3",
                                                                readbackResult,
                                                                "Failed to create a Chapter 14 texture readback."));
    }

    ComPtr<ID3D12CommandAllocator> allocator{};
    HRESULT const allocatorResult = deviceResources.device()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf()));
    if (FAILED(allocatorResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandAllocator", allocatorResult,
                                                                "Failed to create a Chapter 14 readback allocator."));
    }

    ComPtr<ID3D12GraphicsCommandList7> commandList{};
    HRESULT const listResult =
        deviceResources.device()->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                    IID_PPV_ARGS(commandList.ReleaseAndGetAddressOf()));
    if (FAILED(listResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandList", listResult,
                                                                "Failed to create a Chapter 14 readback list."));
    }

    std::vector<D3D12_TEXTURE_BARRIER> barriers{
        MakeTextureBarrier(resource, currentState, CopySourceTextureState()),
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

    barriers = {MakeTextureBarrier(resource, CopySourceTextureState(), currentState)};
    SubmitTextureBarriers(*commandList.Get(), barriers);
    HRESULT const closeResult = commandList->Close();
    if (FAILED(closeResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12GraphicsCommandList::Close", closeResult,
                                                                "Failed to close a Chapter 14 readback list."));
    }

    ID3D12CommandList *const lists[]{commandList.Get()};
    deviceResources.graphics_queue()->ExecuteCommandLists(1U, lists);
    auto const copyIdle = deviceResources.WaitForGpuIdle();
    if (!copyIdle)
    {
        return std::unexpected(copyIdle.error());
    }

    D3D12_RANGE const readRange{0U, static_cast<SIZE_T>(totalBytes)};
    void *mapped = nullptr;
    HRESULT const mapResult = readbackBuffer->Map(0U, &readRange, &mapped);
    if (FAILED(mapResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                "Failed to map a Chapter 14 texture readback."));
    }

    TextureReadback output{
        .size = {static_cast<std::uint32_t>(sourceDescription.Width), sourceDescription.Height},
        .format = sourceDescription.Format,
        .rowPitch = footprint.Footprint.RowPitch,
        .bytes = std::vector<std::byte>(static_cast<std::size_t>(totalBytes)),
    };
    std::memcpy(output.bytes.data(), mapped, output.bytes.size());
    D3D12_RANGE const writtenRange{0U, 0U};
    readbackBuffer->Unmap(0U, &writtenRange);
    return output;
}

std::expected<BoundedLightLists, lgp::framework::Error> BuildCpuReference(LabConfiguration configuration,
                                                                          PerspectiveProjection projection,
                                                                          std::span<PointLightData const> lights,
                                                                          TextureReadback const &depth)
{
    auto const tiles = MakeTileGrid(depth.size.width, depth.size.height, kTileWidth, kTileHeight);
    if (!tiles)
    {
        return std::unexpected(MakeContractError("BuildCpuReference", tiles.error()));
    }
    std::uint32_t const lightCount =
        std::min(NormalizeLightCount(configuration.lightCount), static_cast<std::uint32_t>(lights.size()));
    LightingMode const listMode =
        configuration.mode == LightingMode::Clustered ? LightingMode::Clustered : LightingMode::Tiled;
    auto const cellCount = CellCountForMode(listMode, depth.size);
    if (!cellCount)
    {
        return std::unexpected(cellCount.error());
    }

    std::vector<TileDepthRange> tileDepthRanges(tiles->tileCount);
    std::vector<float> samples{};
    samples.reserve(static_cast<std::size_t>(kTileWidth) * kTileHeight);
    for (std::uint32_t tileY = 0U; tileY < tiles->tileCountY; ++tileY)
    {
        for (std::uint32_t tileX = 0U; tileX < tiles->tileCountX; ++tileX)
        {
            samples.clear();
            std::uint32_t const minimumX = tileX * kTileWidth;
            std::uint32_t const minimumY = tileY * kTileHeight;
            std::uint32_t const maximumX = std::min(minimumX + kTileWidth, depth.size.width);
            std::uint32_t const maximumY = std::min(minimumY + kTileHeight, depth.size.height);
            for (std::uint32_t y = minimumY; y < maximumY; ++y)
            {
                for (std::uint32_t x = minimumX; x < maximumX; ++x)
                {
                    auto const sample = ReadDepth(depth, x, y);
                    if (!sample)
                    {
                        return std::unexpected(sample.error());
                    }
                    samples.push_back(*sample);
                }
            }
            auto const range = ReduceTileDepthRange(samples, projection);
            if (!range)
            {
                return std::unexpected(MakeContractError("BuildCpuReference", range.error()));
            }
            std::uint32_t const tileIndex = tileY * tiles->tileCountX + tileX;
            tileDepthRanges[tileIndex] = *range;
        }
    }

    std::vector<std::uint8_t> overlaps(static_cast<std::size_t>(*cellCount) * lightCount);
    LogDepthSlicing const slicing = MakeSlicing();
    for (std::uint32_t cellIndex = 0U; cellIndex < *cellCount; ++cellIndex)
    {
        std::uint32_t const tileIndex = cellIndex % tiles->tileCount;
        std::uint32_t const tileX = tileIndex % tiles->tileCountX;
        std::uint32_t const tileY = tileIndex / tiles->tileCountX;
        TileDepthRange const &tileDepth = tileDepthRanges[tileIndex];
        bool clusterActive = !tileDepth.IsEmpty();
        std::uint32_t sliceIndex = 0U;
        if (listMode == LightingMode::Clustered)
        {
            sliceIndex = cellIndex / tiles->tileCount;
            auto const slice = SliceDepthBounds(sliceIndex, slicing);
            if (!slice)
            {
                return std::unexpected(MakeContractError("BuildCpuReference", slice.error()));
            }
            clusterActive = clusterActive && tileDepth.viewDepths->maximum >= slice->minimum &&
                            tileDepth.viewDepths->minimum <= slice->maximum;
        }

        for (std::uint32_t lightIndex = 0U; lightIndex < lightCount; ++lightIndex)
        {
            bool overlapsCell = false;
            PointLightView const light{lights[lightIndex].position, lights[lightIndex].radius};
            if (listMode == LightingMode::Clustered)
            {
                if (clusterActive)
                {
                    auto const overlap =
                        SphereOverlapsCluster(light, *tiles, tileX, tileY, sliceIndex, slicing, projection);
                    if (!overlap)
                    {
                        return std::unexpected(MakeContractError("BuildCpuReference", overlap.error()));
                    }
                    overlapsCell = *overlap;
                }
            }
            else
            {
                auto const overlap = SphereOverlapsTileDepthRange(light, *tiles, tileX, tileY, tileDepth, projection);
                if (!overlap)
                {
                    return std::unexpected(MakeContractError("BuildCpuReference", overlap.error()));
                }
                overlapsCell = *overlap;
            }
            overlaps[static_cast<std::size_t>(cellIndex) * lightCount + lightIndex] =
                overlapsCell ? std::uint8_t{1U} : std::uint8_t{0U};
        }
    }

    auto const lists =
        BuildBoundedLightLists(*cellCount, lightCount, overlaps, NormalizeCapacity(configuration.capacity));
    if (!lists)
    {
        return std::unexpected(MakeContractError("BuildCpuReference", lists.error()));
    }
    return *lists;
}

} // namespace ch14::clustered_lighting::gpu
