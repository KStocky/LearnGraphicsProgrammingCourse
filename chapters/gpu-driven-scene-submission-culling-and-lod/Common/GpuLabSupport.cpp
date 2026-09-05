#include "GpuLabSupport.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>

namespace ch21::gpu_driven::gpu
{
namespace
{

[[nodiscard]] lgp::framework::Error ContractFailure(char const *operation, ContractError error)
{
    return lgp::framework::MakeError(operation, "Chapter 21 CPU reference contract failed with code " +
                                                    std::to_string(static_cast<unsigned int>(error)) + ".");
}

[[nodiscard]] std::vector<DrawTemplate> BuildTemplates()
{
    return {
        {.indexCount = 6U, .startIndex = 0U, .baseVertex = 0, .materialIndex = 0U},
        {.indexCount = 3U, .startIndex = 6U, .baseVertex = 0, .materialIndex = 1U},
        {.indexCount = 3U, .startIndex = 9U, .baseVertex = 0, .materialIndex = 2U},
    };
}

[[nodiscard]] std::vector<GpuInstance> BuildGpuInstances(ScenePreset scene)
{
    std::array<Float4, 8U> const bounds{{
        {-2.0F, 0.8F, 4.0F, 0.7F},
        {1.5F, -0.7F, 7.0F, 0.9F},
        {0.0F, 1.4F, 12.0F, 1.0F},
        {-3.0F, -1.5F, 18.0F, 1.2F},
        {4.4F, 0.0F, 5.0F, 0.4F},
        {0.0F, 0.0F, 31.0F, 1.0F},
        {0.0F, 0.0F, 1.0F, 0.75F},
        {2.5F, 1.4F, 9.0F, 0.6F},
    }};
    std::array<std::uint32_t, 8U> const stableIds{{91U, 7U, 44U, 203U, 18U, 66U, 130U, 5U}};
    std::array<std::uint32_t, 8U> order{{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}};
    if (scene == ScenePreset::Permuted)
    {
        order = {{5U, 2U, 7U, 0U, 6U, 3U, 1U, 4U}};
    }

    std::uint32_t count = 8U;
    if (scene == ScenePreset::Empty)
    {
        count = 0U;
    }
    else if (scene == ScenePreset::Overflow)
    {
        count = 16U;
    }
    std::vector<GpuInstance> result{};
    result.reserve(count);
    for (std::uint32_t physicalIndex = 0U; physicalIndex < count; ++physicalIndex)
    {
        std::uint32_t const source = order[physicalIndex % order.size()];
        Float4 sphere = bounds[source];
        std::uint32_t stableId = stableIds[source];
        if (scene == ScenePreset::Overflow && physicalIndex >= order.size())
        {
            sphere.x = -2.8F + 0.8F * static_cast<float>(physicalIndex - 8U);
            sphere.y = 2.2F;
            sphere.z = 6.0F;
            sphere.w = 0.55F;
            stableId = 300U + physicalIndex;
        }
        result.push_back({
            .bounds = sphere,
            .display =
                {
                    -0.72F + 0.48F * static_cast<float>(physicalIndex % 4U),
                    0.72F - 0.48F * static_cast<float>(physicalIndex / 4U),
                    0.12F,
                    1.0F,
                },
            .stableId = stableId,
            .instanceDataIndex = physicalIndex,
            .firstDrawTemplate = 0U,
            .lodCount = kDrawTemplateCount,
            .previousLod = source % kDrawTemplateCount,
        });
    }
    return result;
}

[[nodiscard]] std::vector<Plane> Frustum()
{
    return {
        {{1.0, 0.0, 0.75}, 0.0},  {{-1.0, 0.0, 0.75}, 0.0}, {{0.0, 1.0, 0.75}, 0.0},
        {{0.0, -1.0, 0.75}, 0.0}, {{0.0, 0.0, 1.0}, -0.5},  {{0.0, 0.0, -1.0}, 30.0},
    };
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

std::expected<CpuReference, lgp::framework::Error> BuildCpuReference(LabConfiguration configuration)
{
    configuration.capacity = NormalizeCapacity(configuration.capacity);
    CpuReference reference{};
    reference.configuration = configuration;
    reference.gpuInstances = BuildGpuInstances(configuration.scene);
    reference.drawTemplates = BuildTemplates();
    reference.instances.reserve(reference.gpuInstances.size());
    reference.decisions.reserve(reference.gpuInstances.size());

    std::vector<Plane> const frustum = Frustum();
    LodPolicy const lodPolicy{{80.0, 32.0}, 4.0};
    ProjectionParameters const projection{kViewportHeight, kProjectionScale, kNearPlane};
    for (GpuInstance const &gpuInstance : reference.gpuInstances)
    {
        InstanceRecord const instance{
            .stableId = gpuInstance.stableId,
            .instanceDataIndex = gpuInstance.instanceDataIndex,
            .firstDrawTemplate = gpuInstance.firstDrawTemplate,
            .lodCount = gpuInstance.lodCount,
            .previousLod = gpuInstance.previousLod,
            .bounds = {{gpuInstance.bounds.x, gpuInstance.bounds.y, gpuInstance.bounds.z}, gpuInstance.bounds.w},
        };
        reference.instances.push_back(instance);
        auto const classification = ClassifySphereAgainstFrustum(frustum, instance.bounds);
        if (!classification)
        {
            return std::unexpected(ContractFailure("BuildCpuReference", classification.error()));
        }

        std::uint32_t lod = instance.previousLod;
        bool const visible = *classification != FrustumClassification::Outside;
        if (visible)
        {
            auto const projected =
                ProjectSphereConservatively(instance.bounds.center.z, instance.bounds.radius, projection);
            if (!projected)
            {
                return std::unexpected(ContractFailure("BuildCpuReference", projected.error()));
            }
            auto const selection = SelectLod(projected->radiusPixels, lodPolicy, instance.previousLod);
            if (!selection)
            {
                return std::unexpected(ContractFailure("BuildCpuReference", selection.error()));
            }
            lod = std::min(selection->selectedLod, instance.lodCount - 1U);
            ++reference.visibleCount;
        }
        reference.decisions.push_back({instance.stableId, instance.instanceDataIndex, lod, visible});
    }

    if (reference.instances.empty())
    {
        return reference;
    }

    auto const validation = ValidateScene(reference.instances, reference.drawTemplates, kMaximumInstanceCount, 12U);
    if (!validation)
    {
        return std::unexpected(ContractFailure("BuildCpuReference", validation.error()));
    }
    auto commands = BuildIndirectSubmission(reference.instances, reference.drawTemplates, reference.decisions,
                                            configuration.capacity);
    if (!commands)
    {
        if (commands.error() != ContractError::OutputCapacityExceeded)
        {
            return std::unexpected(ContractFailure("BuildCpuReference", commands.error()));
        }
        std::vector<VisibilityDecision> boundedDecisions{};
        std::uint32_t acceptedVisibleCount{};
        for (VisibilityDecision const &decision : reference.decisions)
        {
            if (decision.visible && acceptedVisibleCount >= configuration.capacity)
            {
                boundedDecisions.push_back({decision.stableId, decision.instanceDataIndex, decision.lod, false});
            }
            else
            {
                boundedDecisions.push_back(decision);
                acceptedVisibleCount += decision.visible ? 1U : 0U;
            }
        }
        commands = BuildIndirectSubmission(reference.instances, reference.drawTemplates, boundedDecisions,
                                           configuration.capacity);
    }
    if (!commands)
    {
        return std::unexpected(ContractFailure("BuildCpuReference", commands.error()));
    }
    reference.commands = std::move(*commands);
    return reference;
}

IndirectCommand ToContractCommand(GpuIndirectCommand const &command, std::span<InstanceRecord const> instances,
                                  std::span<DrawTemplate const> drawTemplates)
{
    auto const instance = std::ranges::find(instances, command.stableId, &InstanceRecord::stableId);
    std::uint32_t const templateIndex = instance == instances.end() ? 0U : instance->firstDrawTemplate + command.lod;
    (void)drawTemplates;
    return {
        .stableId = command.stableId,
        .lod = command.lod,
        .drawTemplateIndex = templateIndex,
        .draw =
            {
                command.indexCountPerInstance,
                command.instanceCount,
                command.startIndexLocation,
                command.baseVertexLocation,
                command.startInstanceLocation,
            },
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

BufferBarrierState ComputeUnorderedAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
}

BufferBarrierState ExecuteIndirectState() noexcept
{
    return {D3D12_BARRIER_SYNC_EXECUTE_INDIRECT, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT};
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
                                                                "Failed to create a Chapter 21 buffer."));
    }
    std::wstring const resourceName{name};
    if (!resourceName.empty())
    {
        (void)buffer.resource_->SetName(resourceName.c_str());
    }
    if (mapPersistently)
    {
        D3D12_RANGE const range{0U, 0U};
        void *mapped{};
        HRESULT const mapResult = buffer.resource_->Map(0U, &range, &mapped);
        if (FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map a Chapter 21 buffer."));
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
        return std::unexpected(lgp::framework::MakeError("WriteBuffer", "Chapter 21 upload write is invalid."));
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

} // namespace ch21::gpu_driven::gpu
