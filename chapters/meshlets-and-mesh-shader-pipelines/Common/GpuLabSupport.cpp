#include "GpuLabSupport.hpp"

#include <array>
#include <cstring>
#include <string>
#include <utility>

namespace ch22::meshlets::gpu
{
namespace
{

[[nodiscard]] lgp::framework::Error MeshFailure(char const *operation, MeshError error)
{
    return lgp::framework::MakeError(operation, "Chapter 22 meshlet contract failed with code " +
                                                    std::to_string(static_cast<unsigned int>(error)) + ".");
}

[[nodiscard]] std::uint32_t GridVertexIndex(std::uint32_t column, std::uint32_t row) noexcept
{
    return row * (kGridColumns + 1U) + column;
}

[[nodiscard]] std::vector<Float3> BuildGridPositions()
{
    std::vector<Float3> positions{};
    positions.reserve((kGridColumns + 1U) * (kGridRows + 1U));
    for (std::uint32_t row = 0U; row <= kGridRows; ++row)
    {
        for (std::uint32_t column = 0U; column <= kGridColumns; ++column)
        {
            positions.push_back(
                {-0.75F + 0.5F * static_cast<float>(column), -0.5F + 0.5F * static_cast<float>(row), 0.25F});
        }
    }
    return positions;
}

[[nodiscard]] std::vector<GlobalIndex> BuildGridIndices()
{
    std::vector<GlobalIndex> indices{};
    indices.reserve(static_cast<std::size_t>(kGridColumns) * kGridRows * 6U);
    for (std::uint32_t row = 0U; row < kGridRows; ++row)
    {
        for (std::uint32_t column = 0U; column < kGridColumns; ++column)
        {
            GlobalIndex const v00 = GridVertexIndex(column, row);
            GlobalIndex const v10 = GridVertexIndex(column + 1U, row);
            GlobalIndex const v11 = GridVertexIndex(column + 1U, row + 1U);
            GlobalIndex const v01 = GridVertexIndex(column, row + 1U);
            indices.push_back(v00);
            indices.push_back(v10);
            indices.push_back(v11);
            indices.push_back(v00);
            indices.push_back(v11);
            indices.push_back(v01);
        }
    }
    return indices;
}

[[nodiscard]] std::uint32_t PackPrimitive(PrimitiveTriangle const &primitive) noexcept
{
    return static_cast<std::uint32_t>(primitive.a) | (static_cast<std::uint32_t>(primitive.b) << 10U) |
           (static_cast<std::uint32_t>(primitive.c) << 20U);
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

ID3D12Resource *BufferResource::Get() const noexcept
{
    return resource_.Get();
}

std::uint64_t BufferResource::size_in_bytes() const noexcept
{
    return sizeInBytes_;
}

std::byte *BufferResource::mapped_data() noexcept
{
    return mappedData_;
}

std::byte const *BufferResource::mapped_data() const noexcept
{
    return mappedData_;
}

void BufferResource::Reset() noexcept
{
    if (resource_ != nullptr && mappedData_ != nullptr)
    {
        D3D12_RANGE const range{0U, 0U};
        resource_->Unmap(0U, &range);
    }
    resource_.Reset();
    sizeInBytes_ = 0U;
    mappedData_ = nullptr;
}

std::expected<GpuScene, lgp::framework::Error> BuildGpuScene()
{
    GpuScene scene{};
    scene.positions = BuildGridPositions();
    scene.sourceIndices = BuildGridIndices();
    scene.limits = {.maxVertices = kSceneMaxVertices, .maxPrimitives = kSceneMaxPrimitives};

    auto build = BuildMeshlets(scene.positions, scene.sourceIndices, scene.limits);
    if (!build)
    {
        return std::unexpected(MeshFailure("BuildGpuScene", build.error()));
    }
    scene.build = std::move(*build);

    if (auto const validation = ValidateMeshletBuild(scene.build, scene.positions, scene.sourceIndices, scene.limits);
        !validation)
    {
        return std::unexpected(MeshFailure("BuildGpuScene", validation.error()));
    }

    scene.meshletCount = scene.build.statistics.meshletCount;
    scene.emittedVertexReferences = scene.build.statistics.vertexReferenceCount;
    scene.emittedPrimitives = scene.build.statistics.primitiveCount;

    scene.meshPositions.reserve(scene.positions.size());
    for (Float3 const &position : scene.positions)
    {
        scene.meshPositions.push_back({position.x, position.y, position.z, 1.0F});
    }

    std::uint32_t meshletId = 0U;
    std::uint32_t vertexBase = 0U;
    for (Meshlet const &meshlet : scene.build.meshlets)
    {
        MeshletDescriptor descriptor{};
        descriptor.vertexOffset = static_cast<std::uint32_t>(scene.meshletVertices.size());
        descriptor.vertexCount = static_cast<std::uint32_t>(meshlet.vertexRemap.size());
        descriptor.primitiveOffset = static_cast<std::uint32_t>(scene.meshletPrimitives.size());
        descriptor.primitiveCount = static_cast<std::uint32_t>(meshlet.primitives.size());
        scene.meshletDescriptors.push_back(descriptor);

        for (GlobalIndex const global : meshlet.vertexRemap)
        {
            Float3 const &position = scene.positions[global];
            scene.classicVertices.push_back({position.x, position.y, position.z, meshletId});
            scene.meshletVertices.push_back(global);
        }
        for (PrimitiveTriangle const &primitive : meshlet.primitives)
        {
            scene.classicIndices.push_back(vertexBase + primitive.a);
            scene.classicIndices.push_back(vertexBase + primitive.b);
            scene.classicIndices.push_back(vertexBase + primitive.c);
            scene.meshletPrimitives.push_back(PackPrimitive(primitive));
        }
        vertexBase += descriptor.vertexCount;
        ++meshletId;
    }

    return scene;
}

MeshShaderCapabilities QueryMeshShaderCapabilities(ID3D12Device10 &device)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options{};
    if (FAILED(device.CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options))))
    {
        return {};
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_5};
    bool const shaderModel65Supported =
        SUCCEEDED(device.CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) &&
        shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
    return {
        .supported = options.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1 && shaderModel65Supported,
        .shaderModel65Supported = shaderModel65Supported,
        .tier = options.MeshShaderTier,
    };
}

lgp::framework::TextureBarrierState FrameStartState(lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frameContext.renderTargetInitialLayout};
}

lgp::framework::TextureBarrierState FrameEndState(lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
            frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT};
}

lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
}

BufferBarrierState NoAccessState() noexcept
{
    return {};
}

BufferBarrierState CopyDestState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST};
}

BufferBarrierState MeshUnorderedAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_DRAW, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
}

BufferBarrierState CopySourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE};
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
    if (!barriers.empty())
    {
        D3D12_BARRIER_GROUP group{};
        group.Type = D3D12_BARRIER_TYPE_TEXTURE;
        group.NumBarriers = static_cast<UINT>(barriers.size());
        group.pTextureBarriers = barriers.data();
        commandList.Barrier(1U, &group);
    }
}

D3D12_BUFFER_BARRIER MakeBufferBarrier(ID3D12Resource &resource, BufferBarrierState before, BufferBarrierState after,
                                       std::uint64_t offset, std::uint64_t size) noexcept
{
    return {before.sync, after.sync, before.access, after.access, &resource, offset, size};
}

void SubmitBufferBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_BUFFER_BARRIER> &barriers)
{
    if (!barriers.empty())
    {
        D3D12_BARRIER_GROUP group{};
        group.Type = D3D12_BARRIER_TYPE_BUFFER;
        group.NumBarriers = static_cast<UINT>(barriers.size());
        group.pBufferBarriers = barriers.data();
        commandList.Barrier(1U, &group);
    }
}

std::expected<BufferResource, lgp::framework::Error> CreateBuffer(ID3D12Device10 &device, std::uint64_t sizeInBytes,
                                                                  D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
                                                                  std::wstring_view name, bool mapPersistently)
{
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
                                                                "Failed to create a Chapter 22 buffer."));
    }
    std::wstring const resourceName{name};
    if (!resourceName.empty())
    {
        (void)buffer.resource_->SetName(resourceName.c_str());
    }
    if (mapPersistently)
    {
        D3D12_RANGE const range{0U, heapType == D3D12_HEAP_TYPE_READBACK ? static_cast<SIZE_T>(sizeInBytes) : 0U};
        void *mapped{};
        HRESULT const mapResult = buffer.resource_->Map(0U, &range, &mapped);
        if (FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map a Chapter 22 buffer."));
        }
        buffer.mappedData_ = static_cast<std::byte *>(mapped);
    }
    buffer.sizeInBytes_ = sizeInBytes;
    return buffer;
}

lgp::framework::Status WriteBuffer(BufferResource &buffer, std::span<std::byte const> bytes,
                                   std::uint64_t destinationOffset)
{
    if (buffer.mapped_data() == nullptr || destinationOffset > buffer.size_in_bytes() ||
        bytes.size_bytes() > buffer.size_in_bytes() - destinationOffset)
    {
        return std::unexpected(lgp::framework::MakeError("WriteBuffer", "Chapter 22 upload write is invalid."));
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
    options.additionalArguments = {L"-E", entryPoint, L"-T", targetProfile};
    auto result = compiler.Compile(options);
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    shader = std::move(*result);
    return {};
}

} // namespace ch22::meshlets::gpu
