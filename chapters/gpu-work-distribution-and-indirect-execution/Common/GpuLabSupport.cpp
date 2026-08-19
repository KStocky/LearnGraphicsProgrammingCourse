#include "GpuLabSupport.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace ch13::work_distribution::gpu
{
namespace
{

[[nodiscard]] lgp::framework::Error MakeContractError(char const *operation, ContractError error)
{
    std::string message = "Work-distribution contract failed: ";
    switch (error)
    {
    case ContractError::TooManyCandidates:
        message += "candidate count exceeds the Chapter 13 GPU lab limit.";
        break;
    case ContractError::NonBinaryFlag:
        message += "visibility flags must be binary.";
        break;
    case ContractError::InvalidArrivalOrder:
        message += "the simulated atomic arrival order is invalid.";
        break;
    case ContractError::DuplicateArrival:
        message += "the simulated atomic arrival order contains duplicates.";
        break;
    case ContractError::CapacityExceeded:
        message += "the emitted command list exceeds the bounded capacity.";
        break;
    case ContractError::InvalidCandidateIndex:
        message += "an indirect command references an invalid candidate index.";
        break;
    case ContractError::InvalidVertexCount:
        message += "the quad vertex count must be greater than zero.";
        break;
    case ContractError::InvalidStride:
        message += "the indirect command stride is invalid.";
        break;
    case ContractError::MisalignedOffset:
        message += "the indirect buffer offset is not DWORD aligned.";
        break;
    case ContractError::BufferRangeExceeded:
        message += "the indirect execution range exceeds the buffer size.";
        break;
    case ContractError::ArithmeticOverflow:
        message += "the indirect execution layout overflows 64-bit arithmetic.";
        break;
    }
    return lgp::framework::MakeError(operation, message);
}

[[nodiscard]] Float4 CandidateColor(std::uint32_t row, std::uint32_t column) noexcept
{
    return {
        0.18F + (0.08F * static_cast<float>(column)),
        0.16F + (0.07F * static_cast<float>(row)),
        0.28F + (0.05F * static_cast<float>((row + column) % 4U)),
        1.0F,
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

lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
}

BufferBarrierState NoAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS};
}

BufferBarrierState ComputeShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
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

BufferBarrierState CopyDestState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST};
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
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 13 buffers must be non-empty."));
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = heapType;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1U;
    heapProperties.VisibleNodeMask = 1U;

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
        &heapProperties, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0U,
        nullptr, IID_PPV_ARGS(buffer.resource_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                                "Failed to create a Chapter 13 buffer."));
    }

    if (!name.empty())
    {
        std::wstring const resourceName{name};
        HRESULT const nameResult = buffer.resource_->SetName(resourceName.c_str());
        if (FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name a Chapter 13 buffer."));
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
                                                                    "Failed to map a Chapter 13 buffer."));
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
            lgp::framework::MakeError("WriteBuffer", "The Chapter 13 buffer is not persistently mapped."));
    }
    if (destinationOffset > buffer.size_in_bytes() || bytes.size_bytes() > (buffer.size_in_bytes() - destinationOffset))
    {
        return std::unexpected(
            lgp::framework::MakeError("WriteBuffer", "The Chapter 13 buffer write is out of range."));
    }

    std::memcpy(buffer.mapped_data() + destinationOffset, bytes.data(), bytes.size_bytes());
    return {};
}

std::vector<CandidateData> BuildCandidates(ScenePreset scene)
{
    std::vector<CandidateData> candidates{};
    candidates.reserve(kCandidateCount);
    for (std::uint32_t row = 0U; row < kCandidateGridHeight; ++row)
    {
        for (std::uint32_t column = 0U; column < kCandidateGridWidth; ++column)
        {
            std::uint32_t const candidateIndex = (row * kCandidateGridWidth) + column;
            bool visible = false;
            switch (scene)
            {
            case ScenePreset::Default:
                visible = ((candidateIndex % 3U) != 1U) && ((row + column) != 5U);
                break;
            case ScenePreset::Empty:
                visible = false;
                break;
            case ScenePreset::Overflow:
                visible = true;
                break;
            }

            candidates.push_back({
                .center =
                    {
                        -0.875F + (0.25F * static_cast<float>(column)),
                        0.875F - (0.25F * static_cast<float>(row)),
                    },
                .halfExtent = {0.095F, 0.095F},
                .color = CandidateColor(row, column),
                .visible = visible ? 1U : 0U,
                .padding = {0U, 0U, 0U},
            });
        }
    }
    return candidates;
}

std::vector<std::uint32_t> ExtractVisibilityFlags(std::span<CandidateData const> candidates)
{
    std::vector<std::uint32_t> flags{};
    flags.reserve(candidates.size());
    for (CandidateData const &candidate : candidates)
    {
        flags.push_back(candidate.visible);
    }
    return flags;
}

std::expected<CpuReference, lgp::framework::Error> BuildCpuReference(LabConfiguration configuration)
{
    configuration.capacity = NormalizeCapacity(configuration.capacity);

    CpuReference reference{};
    reference.mode = configuration.mode;
    reference.candidates = BuildCandidates(configuration.scene);
    reference.visibilityFlags = ExtractVisibilityFlags(reference.candidates);

    DistributionResult distribution{};
    if (configuration.mode == ExecutionMode::Stable)
    {
        auto compacted = StableCompactVisible(reference.visibilityFlags, configuration.capacity);
        if (!compacted)
        {
            return std::unexpected(MakeContractError("BuildCpuReference", compacted.error()));
        }
        distribution = std::move(*compacted);
    }
    else
    {
        std::array<std::uint32_t, kCandidateCount> reverseOrder{};
        for (std::uint32_t index = 0U; index < kCandidateCount; ++index)
        {
            reverseOrder[index] = (kCandidateCount - 1U) - index;
        }

        auto appended = SimulateBoundedAtomicAppend(reference.visibilityFlags, reverseOrder, configuration.capacity);
        if (!appended)
        {
            return std::unexpected(MakeContractError("BuildCpuReference", appended.error()));
        }
        distribution = std::move(*appended);
    }

    auto commands = BuildIndirectDrawCommands(distribution.candidateIndices, kCandidateCount, configuration.capacity,
                                              kVertexCountPerQuad);
    if (!commands)
    {
        return std::unexpected(MakeContractError("BuildCpuReference", commands.error()));
    }

    reference.statistics = distribution.statistics;
    reference.emittedCandidateIndices = std::move(distribution.candidateIndices);
    reference.indirectCommands = std::move(*commands);
    reference.gpuCount = reference.statistics.visibleCount;
    reference.executionCount = ResolveExecutionCount(configuration.capacity, reference.gpuCount);
    return reference;
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

} // namespace ch13::work_distribution::gpu
