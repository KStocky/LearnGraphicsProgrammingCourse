#include "GpuLabSupport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace ch15::visibility_buffer::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

struct MipImage final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba8{};
};

[[nodiscard]] lgp::framework::Error MakeContractError(char const *operation, std::uint32_t error)
{
    return lgp::framework::MakeError(operation, "A Chapter 15 GPU-facing contract failed with code " +
                                                    std::to_string(error) + ".");
}

[[nodiscard]] MipImage MakeBaseMaterial()
{
    MipImage image{
        .width = kMaterialTextureSize,
        .height = kMaterialTextureSize,
        .rgba8 = std::vector<std::uint8_t>(kMaterialTextureSize * kMaterialTextureSize * 4U),
    };
    for (std::uint32_t y = 0U; y < image.height; ++y)
    {
        for (std::uint32_t x = 0U; x < image.width; ++x)
        {
            float const u = static_cast<float>(x) / static_cast<float>(image.width - 1U);
            float const v = static_cast<float>(y) / static_cast<float>(image.height - 1U);
            bool const checker = (((x / 8U) + (y / 8U)) & 1U) != 0U;
            float const detail = checker ? 0.09F : -0.09F;
            std::size_t const offset = (static_cast<std::size_t>(y) * image.width + x) * 4U;
            image.rgba8[offset + 0U] =
                static_cast<std::uint8_t>(std::clamp((0.24F + (0.58F * u) + detail) * 255.0F, 0.0F, 255.0F));
            image.rgba8[offset + 1U] =
                static_cast<std::uint8_t>(std::clamp((0.22F + (0.52F * v) - detail) * 255.0F, 0.0F, 255.0F));
            image.rgba8[offset + 2U] = static_cast<std::uint8_t>(
                std::clamp((0.30F + (0.26F * u) + (0.20F * v) + (0.5F * detail)) * 255.0F, 0.0F, 255.0F));
            image.rgba8[offset + 3U] = 255U;
        }
    }
    return image;
}

[[nodiscard]] std::vector<MipImage> BuildMaterialMips()
{
    std::vector<MipImage> mips{};
    mips.reserve(kMaterialMipCount);
    mips.push_back(MakeBaseMaterial());
    while (mips.size() < kMaterialMipCount)
    {
        MipImage const &previous = mips.back();
        MipImage next{
            .width = std::max(1U, previous.width / 2U),
            .height = std::max(1U, previous.height / 2U),
        };
        next.rgba8.resize(static_cast<std::size_t>(next.width) * next.height * 4U);
        for (std::uint32_t y = 0U; y < next.height; ++y)
        {
            for (std::uint32_t x = 0U; x < next.width; ++x)
            {
                std::array<std::uint32_t, 4U> sum{};
                for (std::uint32_t offsetY = 0U; offsetY < 2U; ++offsetY)
                {
                    for (std::uint32_t offsetX = 0U; offsetX < 2U; ++offsetX)
                    {
                        std::uint32_t const sourceX = std::min(previous.width - 1U, (x * 2U) + offsetX);
                        std::uint32_t const sourceY = std::min(previous.height - 1U, (y * 2U) + offsetY);
                        std::size_t const source = (static_cast<std::size_t>(sourceY) * previous.width + sourceX) * 4U;
                        for (std::size_t channel = 0U; channel < sum.size(); ++channel)
                        {
                            sum[channel] += previous.rgba8[source + channel];
                        }
                    }
                }
                std::size_t const destination = (static_cast<std::size_t>(y) * next.width + x) * 4U;
                for (std::size_t channel = 0U; channel < sum.size(); ++channel)
                {
                    next.rgba8[destination + channel] = static_cast<std::uint8_t>(sum[channel] / 4U);
                }
            }
        }
        mips.push_back(std::move(next));
    }
    return mips;
}

[[nodiscard]] std::expected<void, lgp::framework::Error> NameObject(ID3D12Object &object, std::wstring_view name)
{
    if (name.empty())
    {
        return {};
    }
    std::wstring const ownedName{name};
    HRESULT const result = object.SetName(ownedName.c_str());
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", result,
                                                                "Failed to name a Chapter 15 GPU resource."));
    }
    return {};
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

std::expected<void, lgp::framework::Error> ValidateExtent(lgp::framework::Extent2D size)
{
    if (size.empty())
    {
        return std::unexpected(lgp::framework::MakeError("ValidateExtent", "Chapter 15 requires a non-empty extent."));
    }
    if (size.width > kMaximumRenderWidth || size.height > kMaximumRenderHeight)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateExtent", "The Chapter 15 extent exceeds the explicit 320x180 lab bound."));
    }
    return {};
}

SceneData BuildScene(ScenePreset preset)
{
    if (preset == ScenePreset::Empty)
    {
        return {};
    }

    SceneData scene{};
    scene.vertices = {
        {{-3.4F, 2.2F, 5.0F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, -1.0F}, {0.0F, 0.0F}},
        {{1.2F, 2.0F, 7.5F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, -1.0F}, {7.0F, 0.0F}},
        {{-3.0F, -2.0F, 4.5F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, -1.0F}, {0.0F, 6.0F}},
        {{1.4F, -1.8F, 7.0F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, -1.0F}, {7.0F, 6.0F}},
        {{-0.8F, 1.3F, 3.4F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, -1.0F}, {0.0F, 0.0F}},
        {{2.6F, 1.0F, 4.2F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, -1.0F}, {5.0F, 0.0F}},
        {{-0.4F, -1.5F, 3.1F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, -1.0F}, {0.0F, 5.0F}},
        {{2.7F, -1.3F, 4.0F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, -1.0F}, {5.0F, 5.0F}},
        {{1.1F, 2.5F, 8.5F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, -1.0F}, {0.0F, 0.0F}},
        {{3.8F, 0.2F, 7.2F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, -1.0F}, {4.0F, 1.0F}},
        {{1.6F, -2.5F, 6.4F}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F, -1.0F}, {1.0F, 5.0F}},
    };
    scene.indices = {
        0U, 1U, 2U, 1U, 3U, 2U, 0U, 1U, 2U, 1U, 3U, 2U, 0U, 1U, 2U,
    };
    scene.draws = {
        {
            .range = {.vertexOffset = 0U, .vertexCount = 4U, .indexOffset = 0U, .indexCount = 6U},
            .baseTintAndRoughness = {0.95F, 0.58F, 0.34F, 0.72F},
            .materialParameters = {0.05F, 0.0F, 0.0F, 0.0F},
        },
        {
            .range = {.vertexOffset = 4U, .vertexCount = 4U, .indexOffset = 6U, .indexCount = 6U},
            .baseTintAndRoughness = {0.36F, 0.72F, 1.0F, 0.31F},
            .materialParameters = {0.28F, 0.0F, 0.0F, 0.0F},
        },
        {
            .range = {.vertexOffset = 8U, .vertexCount = 3U, .indexOffset = 12U, .indexCount = 3U},
            .baseTintAndRoughness = {0.52F, 1.0F, 0.46F, 0.48F},
            .materialParameters = {0.62F, 0.0F, 0.0F, 0.0F},
        },
    };
    return scene;
}

std::vector<PointLightData> BuildLights(std::uint32_t lightCount)
{
    constexpr std::array<PointLightData, kMaximumLightCount> lights{
        PointLightData{{-2.5F, 1.8F, 2.1F}, 4.8F, {1.0F, 0.42F, 0.26F}, 6.0F},
        PointLightData{{0.2F, 2.4F, 2.4F}, 4.6F, {0.36F, 0.72F, 1.0F}, 5.2F},
        PointLightData{{2.7F, 1.0F, 3.0F}, 4.2F, {0.45F, 1.0F, 0.54F}, 5.6F},
        PointLightData{{-2.0F, -1.7F, 3.2F}, 4.9F, {0.95F, 0.82F, 0.36F}, 5.1F},
        PointLightData{{0.9F, -2.0F, 2.0F}, 4.3F, {0.82F, 0.38F, 1.0F}, 5.5F},
        PointLightData{{3.2F, -1.4F, 4.8F}, 4.0F, {0.34F, 0.86F, 0.86F}, 5.8F},
        PointLightData{{-0.6F, 0.1F, 5.6F}, 3.2F, {1.0F, 0.30F, 0.48F}, 4.7F},
        PointLightData{{2.0F, 2.8F, 6.0F}, 3.5F, {0.72F, 0.76F, 1.0F}, 4.9F},
    };
    lightCount = NormalizeLightCount(lightCount);
    return {lights.begin(), lights.begin() + lightCount};
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

std::expected<DirectX::XMFLOAT4X4, lgp::framework::Error> MakeProjectionMatrix(PerspectiveProjection projection)
{
    auto const coefficients = ch12::gbuffer::MakeDeviceDepthCoefficients(projection);
    if (!coefficients)
    {
        return std::unexpected(
            MakeContractError("MakeProjectionMatrix", static_cast<std::uint32_t>(coefficients.error())));
    }
    float const tangentHalfFov = std::tan(projection.verticalFieldOfViewRadians * 0.5F);
    float const xScale = 1.0F / (tangentHalfFov * projection.aspectRatio);
    float const yScale = 1.0F / tangentHalfFov;
    DirectX::XMFLOAT4X4 matrix{};
    DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMATRIX(xScale, 0.0F, 0.0F, 0.0F, 0.0F, yScale, 0.0F, 0.0F, 0.0F, 0.0F,
                                                        coefficients->additive, 1.0F, 0.0F, 0.0F,
                                                        coefficients->reciprocal, 0.0F));
    return matrix;
}

std::expected<std::vector<GeometryVertex>, lgp::framework::Error> BuildContractVertices(SceneData const &scene,
                                                                                        lgp::framework::Extent2D size,
                                                                                        DepthConvention depthConvention)
{
    if (auto const extent = ValidateExtent(size); !extent)
    {
        return std::unexpected(extent.error());
    }
    PerspectiveProjection const projection = MakeProjection(size, depthConvention);
    auto const coefficients = ch12::gbuffer::MakeDeviceDepthCoefficients(projection);
    if (!coefficients)
    {
        return std::unexpected(
            MakeContractError("BuildContractVertices", static_cast<std::uint32_t>(coefficients.error())));
    }
    float const tangentHalfFov = std::tan(projection.verticalFieldOfViewRadians * 0.5F);
    float const xScale = 1.0F / (tangentHalfFov * projection.aspectRatio);
    float const yScale = 1.0F / tangentHalfFov;

    std::vector<GeometryVertex> result{};
    result.reserve(scene.vertices.size());
    for (Vertex const &vertex : scene.vertices)
    {
        result.push_back({
            .clipPosition =
                {
                    vertex.position.x * xScale,
                    vertex.position.y * yScale,
                    (vertex.position.z * coefficients->additive) + coefficients->reciprocal,
                    vertex.position.z,
                },
            .surface =
                {
                    .position = vertex.position,
                    .normal = vertex.normal,
                    .textureCoordinates = vertex.textureCoordinates,
                    .tangent = vertex.tangent,
                },
        });
    }
    return result;
}

std::vector<IndexedDrawRange> BuildDrawRanges(SceneData const &scene)
{
    std::vector<IndexedDrawRange> ranges{};
    ranges.reserve(scene.draws.size());
    for (DrawData const &draw : scene.draws)
    {
        ranges.push_back(draw.range);
    }
    return ranges;
}

std::expected<BoundedLightLists, lgp::framework::Error> BuildClusteredLightLists(lgp::framework::Extent2D size,
                                                                                 PerspectiveProjection projection,
                                                                                 std::span<PointLightData const> lights)
{
    if (auto const extent = ValidateExtent(size); !extent)
    {
        return std::unexpected(extent.error());
    }
    if (lights.size() > kMaximumLightCount)
    {
        return std::unexpected(
            lgp::framework::MakeError("BuildClusteredLightLists", "The Chapter 15 light count exceeds its lab bound."));
    }

    auto const tiles = ch14::clustered_lighting::MakeTileGrid(size.width, size.height, kTileWidth, kTileHeight);
    if (!tiles)
    {
        return std::unexpected(
            MakeContractError("BuildClusteredLightLists", static_cast<std::uint32_t>(tiles.error())));
    }
    auto const clusters = ch14::clustered_lighting::MakeClusterGrid(*tiles, kClusterSliceCount);
    if (!clusters)
    {
        return std::unexpected(
            MakeContractError("BuildClusteredLightLists", static_cast<std::uint32_t>(clusters.error())));
    }
    if (clusters->clusterCount > kMaximumClusterCount)
    {
        return std::unexpected(lgp::framework::MakeError("BuildClusteredLightLists",
                                                         "The Chapter 15 cluster count exceeds its allocation."));
    }

    ch14::clustered_lighting::LogDepthSlicing const slicing{
        .nearDepth = kNearPlane,
        .farDepth = kFarPlane,
        .sliceCount = kClusterSliceCount,
    };
    std::vector<std::uint8_t> overlaps(static_cast<std::size_t>(clusters->clusterCount) * lights.size());
    for (std::uint32_t clusterIndex = 0U; clusterIndex < clusters->clusterCount; ++clusterIndex)
    {
        auto const coordinate = ch14::clustered_lighting::CheckedClusterCoordinate(*clusters, clusterIndex);
        if (!coordinate)
        {
            return std::unexpected(
                MakeContractError("BuildClusteredLightLists", static_cast<std::uint32_t>(coordinate.error())));
        }
        for (std::uint32_t lightIndex = 0U; lightIndex < lights.size(); ++lightIndex)
        {
            PointLightData const &light = lights[lightIndex];
            auto const overlap = ch14::clustered_lighting::SphereOverlapsCluster(
                {{light.position.x, light.position.y, light.position.z}, light.radius}, tiles.value(), coordinate->x,
                coordinate->y, coordinate->z, slicing, projection);
            if (!overlap)
            {
                return std::unexpected(
                    MakeContractError("BuildClusteredLightLists", static_cast<std::uint32_t>(overlap.error())));
            }
            overlaps[static_cast<std::size_t>(clusterIndex) * lights.size() + lightIndex] =
                *overlap ? std::uint8_t{1U} : std::uint8_t{0U};
        }
    }

    auto lists = ch14::clustered_lighting::BuildBoundedLightLists(
        clusters->clusterCount, static_cast<std::uint32_t>(lights.size()), overlaps, kMaximumLightIndexCount);
    if (!lists)
    {
        return std::unexpected(
            MakeContractError("BuildClusteredLightLists", static_cast<std::uint32_t>(lists.error())));
    }
    auto const validation =
        ch14::clustered_lighting::ValidateBoundedLightLists(*lists, static_cast<std::uint32_t>(lights.size()));
    if (!validation)
    {
        return std::unexpected(
            MakeContractError("BuildClusteredLightLists", static_cast<std::uint32_t>(validation.error())));
    }
    return *lists;
}

std::expected<void, lgp::framework::Error> ValidateScene(SceneData const &scene)
{
    if (scene.vertices.size() > kMaximumVertexCount || scene.indices.size() > kMaximumIndexCount ||
        scene.draws.size() > kMaximumDrawCount)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateScene", "The Chapter 15 scene exceeds an explicit lab bound."));
    }
    auto const vertices =
        BuildContractVertices(scene, {kMaximumRenderWidth, kMaximumRenderHeight}, DepthConvention::Forward);
    if (!vertices)
    {
        return std::unexpected(vertices.error());
    }
    std::vector<IndexedDrawRange> const ranges = BuildDrawRanges(scene);
    for (std::uint32_t drawIndex = 0U; drawIndex < scene.draws.size(); ++drawIndex)
    {
        DrawData const &draw = scene.draws[drawIndex];
        std::uint32_t const primitiveCount = draw.range.indexCount / 3U;
        if (primitiveCount > kMaximumPrimitiveCount)
        {
            return std::unexpected(
                lgp::framework::MakeError("ValidateScene", "A Chapter 15 draw exceeds the primitive bound."));
        }
        for (std::uint32_t primitiveIndex = 0U; primitiveIndex < primitiveCount; ++primitiveIndex)
        {
            auto const triangle =
                CheckedIndexedTriangle(drawIndex + 1U, primitiveIndex, ranges, scene.indices, vertices.value());
            if (!triangle)
            {
                return std::unexpected(
                    MakeContractError("ValidateScene", static_cast<std::uint32_t>(triangle.error())));
            }
        }
    }
    return {};
}

D3D12_COMPARISON_FUNC DepthFunction(DepthConvention convention) noexcept
{
    return convention == DepthConvention::Forward ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                                  : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
}

std::uint32_t DepthModeFlag(DepthConvention convention) noexcept
{
    return convention == DepthConvention::Forward ? 0U : 1U;
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

lgp::framework::TextureBarrierState UnorderedAccessState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
        D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
    };
}

lgp::framework::TextureBarrierState CopySourceTextureState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_SOURCE};
}

lgp::framework::TextureBarrierState CopyDestTextureState() noexcept
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

BufferBarrierState NoAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS};
}

BufferBarrierState ComputeUnorderedAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
}

BufferBarrierState CopySourceBufferState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE};
}

D3D12_RESOURCE_DESC1 MakeTextureDescription(lgp::framework::Extent2D size, DXGI_FORMAT format,
                                            D3D12_RESOURCE_FLAGS flags, std::uint16_t mipLevels) noexcept
{
    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = size.width;
    description.Height = size.height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = mipLevels;
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
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 15 buffers must be non-empty."));
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
                                                                "Failed to create a Chapter 15 buffer."));
    }
    if (auto const named = NameObject(*buffer.resource_.Get(), name); !named)
    {
        return std::unexpected(named.error());
    }

    if (mapPersistently)
    {
        D3D12_RANGE const readRange{0U, heapType == D3D12_HEAP_TYPE_READBACK ? static_cast<SIZE_T>(sizeInBytes) : 0U};
        void *mapped = nullptr;
        HRESULT const mapResult = buffer.resource_->Map(0U, &readRange, &mapped);
        if (FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map a Chapter 15 buffer."));
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
            lgp::framework::MakeError("WriteBuffer", "The Chapter 15 destination buffer is not mapped."));
    }
    if (destinationOffset > buffer.size_in_bytes() || bytes.size_bytes() > buffer.size_in_bytes() - destinationOffset)
    {
        return std::unexpected(
            lgp::framework::MakeError("WriteBuffer", "The Chapter 15 buffer write exceeds its allocation."));
    }
    std::memcpy(buffer.mapped_data() + destinationOffset, bytes.data(), bytes.size_bytes());
    return {};
}

void ClearMappedBuffer(BufferResource &buffer) noexcept
{
    if (buffer.mapped_data() != nullptr)
    {
        std::memset(buffer.mapped_data(), 0, static_cast<std::size_t>(buffer.size_in_bytes()));
    }
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

std::expected<ComPtr<ID3D12Resource>, lgp::framework::Error> CreateDiagnosticMaterialTexture(
    lgp::framework::DeviceResources &deviceResources)
{
    ID3D12Device10 &device = *deviceResources.device();
    std::vector<MipImage> const mips = BuildMaterialMips();
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CreationNodeMask = 1U;
    defaultHeap.VisibleNodeMask = 1U;
    D3D12_RESOURCE_DESC1 const textureDescription =
        MakeTextureDescription({kMaterialTextureSize, kMaterialTextureSize}, kMaterialFormat, D3D12_RESOURCE_FLAG_NONE,
                               static_cast<std::uint16_t>(mips.size()));

    ComPtr<ID3D12Resource> texture{};
    HRESULT const textureResult = device.CreateCommittedResource3(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDescription, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr, 0U,
        nullptr, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf()));
    if (FAILED(textureResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", textureResult,
                                             "Failed to create the Chapter 15 diagnostic material texture."));
    }
    if (auto const named = NameObject(*texture.Get(), L"Ch15 diagnostic mipmapped material"); !named)
    {
        return std::unexpected(named.error());
    }

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mips.size());
    std::vector<UINT> rowCounts(mips.size());
    std::vector<UINT64> rowSizes(mips.size());
    UINT64 uploadBytes = 0U;
    device.GetCopyableFootprints1(&textureDescription, 0U, static_cast<UINT>(mips.size()), 0U, footprints.data(),
                                  rowCounts.data(), rowSizes.data(), &uploadBytes);
    auto upload = CreateBuffer(device, uploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                               L"Ch15 material texture upload", true);
    if (!upload)
    {
        return std::unexpected(upload.error());
    }
    for (std::size_t mipIndex = 0U; mipIndex < mips.size(); ++mipIndex)
    {
        MipImage const &mip = mips[mipIndex];
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT const &footprint = footprints[mipIndex];
        std::size_t const sourceRowBytes = static_cast<std::size_t>(mip.width) * 4U;
        for (std::uint32_t row = 0U; row < mip.height; ++row)
        {
            std::byte *const destination = upload->mapped_data() + footprint.Offset +
                                           (static_cast<std::size_t>(row) * footprint.Footprint.RowPitch);
            std::uint8_t const *const source = mip.rgba8.data() + (static_cast<std::size_t>(row) * sourceRowBytes);
            std::memcpy(destination, source, sourceRowBytes);
        }
    }

    ComPtr<ID3D12CommandAllocator> allocator{};
    HRESULT const allocatorResult =
        device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf()));
    if (FAILED(allocatorResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateCommandAllocator", allocatorResult,
                                             "Failed to create the Chapter 15 material upload allocator."));
    }
    ComPtr<ID3D12GraphicsCommandList7> commandList{};
    HRESULT const listResult = device.CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                        IID_PPV_ARGS(commandList.ReleaseAndGetAddressOf()));
    if (FAILED(listResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateCommandList", listResult,
                                             "Failed to create the Chapter 15 material upload command list."));
    }

    for (std::size_t mipIndex = 0U; mipIndex < mips.size(); ++mipIndex)
    {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = static_cast<UINT>(mipIndex);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload->Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprints[mipIndex];
        commandList->CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);
    }
    std::vector<D3D12_TEXTURE_BARRIER> barriers{
        MakeTextureBarrier(*texture.Get(),
                           {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_DEST},
                           {D3D12_BARRIER_SYNC_PIXEL_SHADING | D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE}),
    };
    SubmitTextureBarriers(*commandList.Get(), barriers);
    HRESULT const closeResult = commandList->Close();
    if (FAILED(closeResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12GraphicsCommandList::Close", closeResult,
                                             "Failed to close the Chapter 15 material upload command list."));
    }
    ID3D12CommandList *const commandLists[]{commandList.Get()};
    deviceResources.graphics_queue()->ExecuteCommandLists(1U, commandLists);
    auto const idle = deviceResources.WaitForGpuIdle();
    if (!idle)
    {
        return std::unexpected(idle.error());
    }
    return texture;
}

std::expected<TextureReadbackBuffer, lgp::framework::Error> CreateTextureReadbackBuffer(
    ID3D12Device10 &device, D3D12_RESOURCE_DESC const &sourceDescription, std::wstring_view name)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0U;
    UINT64 rowSize = 0U;
    UINT64 totalBytes = 0U;
    device.GetCopyableFootprints(&sourceDescription, 0U, 1U, 0U, &footprint, &rowCount, &rowSize, &totalBytes);
    auto buffer = CreateBuffer(device, totalBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, name, true);
    if (!buffer)
    {
        return std::unexpected(buffer.error());
    }
    return TextureReadbackBuffer{
        .buffer = std::move(*buffer),
        .footprint = footprint,
        .totalBytes = totalBytes,
    };
}

void CopyTextureToReadback(ID3D12GraphicsCommandList7 &commandList, ID3D12Resource &source,
                           TextureReadbackBuffer const &destination) noexcept
{
    D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
    destinationLocation.pResource = destination.buffer.Get();
    destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destinationLocation.PlacedFootprint = destination.footprint;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource = &source;
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sourceLocation.SubresourceIndex = 0U;
    commandList.CopyTextureRegion(&destinationLocation, 0U, 0U, 0U, &sourceLocation, nullptr);
}

std::expected<TextureReadback, lgp::framework::Error> ResolveTextureReadback(TextureReadbackBuffer const &readback,
                                                                             lgp::framework::Extent2D size,
                                                                             DXGI_FORMAT format)
{
    if (readback.buffer.mapped_data() == nullptr || readback.totalBytes > readback.buffer.size_in_bytes())
    {
        return std::unexpected(
            lgp::framework::MakeError("ResolveTextureReadback", "A Chapter 15 texture readback is not mapped."));
    }
    TextureReadback result{
        .size = size,
        .format = format,
        .rowPitch = readback.footprint.Footprint.RowPitch,
        .bytes = std::vector<std::byte>(static_cast<std::size_t>(readback.totalBytes)),
    };
    std::memcpy(result.bytes.data(), readback.buffer.mapped_data(), result.bytes.size());
    return result;
}

std::expected<std::vector<PixelDiagnostics>, lgp::framework::Error> ResolveDiagnostics(BufferResource const &readback,
                                                                                       std::uint32_t pixelCount)
{
    std::uint64_t const requiredBytes = static_cast<std::uint64_t>(pixelCount) * sizeof(PixelDiagnostics);
    if (readback.mapped_data() == nullptr || requiredBytes > readback.size_in_bytes())
    {
        return std::unexpected(
            lgp::framework::MakeError("ResolveDiagnostics", "The Chapter 15 diagnostics readback is malformed."));
    }
    std::vector<PixelDiagnostics> result(pixelCount);
    std::memcpy(result.data(), readback.mapped_data(), static_cast<std::size_t>(requiredBytes));
    return result;
}

std::expected<BoundedLightLists, lgp::framework::Error> ResolveUploadedLightLists(BufferResource const &cells,
                                                                                  BufferResource const &indices,
                                                                                  std::uint32_t cellCount,
                                                                                  std::uint32_t lightIndexCount,
                                                                                  std::uint32_t lightCount)
{
    std::uint64_t const cellBytes = static_cast<std::uint64_t>(cellCount) * sizeof(CellLightRange);
    std::uint64_t const indexBytes = static_cast<std::uint64_t>(lightIndexCount) * sizeof(std::uint32_t);
    if (cells.mapped_data() == nullptr || indices.mapped_data() == nullptr || cellBytes > cells.size_in_bytes() ||
        indexBytes > indices.size_in_bytes())
    {
        return std::unexpected(
            lgp::framework::MakeError("ResolveUploadedLightLists", "A Chapter 15 uploaded light list is malformed."));
    }

    auto const *cellData = reinterpret_cast<CellLightRange const *>(cells.mapped_data());
    auto const *indexData = reinterpret_cast<std::uint32_t const *>(indices.mapped_data());
    BoundedLightLists result{};
    result.cells.assign(cellData, cellData + cellCount);
    result.lightIndices.assign(indexData, indexData + lightIndexCount);
    for (CellLightRange const range : result.cells)
    {
        result.statistics.attemptedCount += range.attemptedCount;
        result.statistics.emittedCount += range.count;
        result.statistics.overflowCount += range.overflowCount;
    }
    auto const validation = ch14::clustered_lighting::ValidateBoundedLightLists(result, lightCount);
    if (!validation)
    {
        return std::unexpected(
            MakeContractError("ResolveUploadedLightLists", static_cast<std::uint32_t>(validation.error())));
    }
    return result;
}

std::filesystem::path SharedShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "VisibilityBufferShared.hlsli";
}

} // namespace ch15::visibility_buffer::gpu
