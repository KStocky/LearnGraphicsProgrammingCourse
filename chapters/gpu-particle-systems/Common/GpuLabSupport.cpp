#include "GpuLabSupport.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <string>
#include <utility>

namespace ch34::particles::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

[[nodiscard]] std::uint64_t HashBytes(std::uint64_t hash, std::uint32_t value) noexcept
{
    constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
    {
        hash ^= static_cast<std::uint64_t>((value >> shift) & 0xFFU);
        hash *= kPrime;
    }
    return hash;
}

[[nodiscard]] std::uint32_t FloatBits(float value) noexcept
{
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] GpuParticle ParticleToGpu(Particle const &particle) noexcept
{
    if (!particle.alive)
    {
        return GpuParticle{};
    }
    GpuParticle gpu{};
    gpu.identity = particle.identity;
    gpu.alive = 1U;
    gpu.px = static_cast<float>(particle.motion.positionMetres.x);
    gpu.py = static_cast<float>(particle.motion.positionMetres.y);
    gpu.pz = static_cast<float>(particle.motion.positionMetres.z);
    gpu.vx = static_cast<float>(particle.motion.velocityMetresPerSecond.x);
    gpu.vy = static_cast<float>(particle.motion.velocityMetresPerSecond.y);
    gpu.vz = static_cast<float>(particle.motion.velocityMetresPerSecond.z);
    gpu.age = static_cast<float>(particle.ageSeconds);
    gpu.lifetime = static_cast<float>(particle.lifetimeSeconds);
    return gpu;
}

} // namespace

std::uint32_t ResolveDrawCapacity(LabConfiguration const &configuration) noexcept
{
    return configuration.drawCapacity == 0U ? configuration.capacity : configuration.drawCapacity;
}

LabConfiguration DefaultLabConfiguration() noexcept
{
    LabConfiguration configuration{};
    configuration.capacity = 64U;
    configuration.seed = 1U;
    configuration.frame.step = {.fixedTimestepSeconds = 1.0 / 128.0,
                                .maximumSubstepCount = 8U,
                                .maximumFrameDeltaSeconds = 0.5,
                                .overflowPolicy = SubstepOverflowPolicy::DropExcessTime};
    configuration.frame.emission = {.ratePerSecond = 256.0,
                                    .lifetimeSeconds = 2.0,
                                    .originMetres = {0.0, 0.5, 0.0},
                                    .positionJitterMetres = 0.25,
                                    .baseVelocityMetresPerSecond = {0.0, 0.0, 0.0},
                                    .velocityJitterMetresPerSecond = 0.5,
                                    .deadSlotPolicy = DeadSlotPolicy::AscendingIndex};
    configuration.frame.field = {.gravityMetresPerSecondSquared = {0.0, -8.0, 0.0}, .linearDragPerSecond = 0.0};
    configuration.frame.integrator = Integrator::SemiImplicitEuler;
    configuration.frame.ground = {
        .enabled = false, .heightMetres = 0.0, .restitution = 0.0, .friction = 0.0, .restingSpeedMetresPerSecond = 0.0};
    configuration.frameDeltaSeconds = 1.0 / 60.0;
    configuration.frameCount = 4U;
    return configuration;
}

std::uint32_t ResolveOutputCapacity(LabConfiguration const &configuration) noexcept
{
    return configuration.outputCapacity == 0U ? configuration.capacity : configuration.outputCapacity;
}

std::uint64_t ChecksumGpuState(std::span<GpuParticle const> state) noexcept
{
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (GpuParticle const &particle : state)
    {
        hash = HashBytes(hash, particle.identity);
        hash = HashBytes(hash, particle.alive != 0U ? 1U : 0U);
        if (particle.alive == 0U)
        {
            continue;
        }
        hash = HashBytes(hash, FloatBits(particle.px));
        hash = HashBytes(hash, FloatBits(particle.py));
        hash = HashBytes(hash, FloatBits(particle.pz));
        hash = HashBytes(hash, FloatBits(particle.vx));
        hash = HashBytes(hash, FloatBits(particle.vy));
        hash = HashBytes(hash, FloatBits(particle.vz));
        hash = HashBytes(hash, FloatBits(particle.age));
        hash = HashBytes(hash, FloatBits(particle.lifetime));
    }
    return hash;
}

std::expected<FramePlanResult, ContractError> PlanFrameEmission(FrameSettings const &settings,
                                                                double accumulatorSeconds, double emissionCarry,
                                                                double frameDeltaSeconds)
{
    auto const plan = PlanFixedSteps(settings.step, accumulatorSeconds, frameDeltaSeconds);
    if (!plan)
    {
        return std::unexpected{plan.error()};
    }

    FramePlanResult result{};
    result.plan = *plan;
    result.nextAccumulatorSeconds = plan->carrySeconds;
    result.requestedPerSubstep.reserve(static_cast<std::size_t>(plan->substepCount));

    double carry = emissionCarry;
    for (std::uint32_t substep = 0U; substep < plan->substepCount; ++substep)
    {
        EmissionSchedule const schedule{.ratePerSecond = settings.emission.ratePerSecond, .carry = carry};
        auto const tick = AccumulateEmission(schedule, settings.step.fixedTimestepSeconds);
        if (!tick)
        {
            return std::unexpected{tick.error()};
        }
        result.requestedPerSubstep.push_back(tick->spawnCount);
        carry = tick->carry;
    }
    result.nextEmissionCarry = carry;
    return result;
}

std::expected<FrameReadback, lgp::framework::Error> BuildReferenceReadback(LabConfiguration const &configuration)
{
    if (configuration.capacity == 0U || configuration.capacity > kMaximumLabCapacity)
    {
        return std::unexpected(
            lgp::framework::MakeError("BuildReferenceReadback", "Chapter 34 lab capacity is out of range."));
    }
    if (configuration.frameCount == 0U)
    {
        return std::unexpected(
            lgp::framework::MakeError("BuildReferenceReadback", "Chapter 34 lab frame count must be positive."));
    }

    auto state = ResetParticleSystem(configuration.capacity, configuration.seed);
    if (!state)
    {
        return std::unexpected(lgp::framework::MakeError(
            "BuildReferenceReadback", std::string{"Reset failed: "} + std::string{ContractErrorName(state.error())}));
    }

    FrameReport lastReport{};
    std::uint64_t totalSubsteps = 0U;
    for (std::uint32_t frame = 0U; frame < configuration.frameCount; ++frame)
    {
        auto report = SimulateFrame(*state, configuration.frame, configuration.frameDeltaSeconds);
        if (!report)
        {
            return std::unexpected(lgp::framework::MakeError("BuildReferenceReadback",
                                                             std::string{"SimulateFrame failed: "} +
                                                                 std::string{ContractErrorName(report.error())}));
        }
        totalSubsteps += report->plan.substepCount;
        lastReport = std::move(*report);
    }

    std::uint32_t const outputCapacity = ResolveOutputCapacity(configuration);
    std::uint32_t const drawCapacity = ResolveDrawCapacity(configuration);

    auto compaction = CompactParticleSystem(*state, outputCapacity);
    if (!compaction)
    {
        return std::unexpected(lgp::framework::MakeError("BuildReferenceReadback",
                                                         std::string{"Compaction failed: "} +
                                                             std::string{ContractErrorName(compaction.error())}));
    }

    IndirectDrawRequest const drawRequest{
        .liveCount = compaction->aliveCount,
        .drawCapacity = std::min(drawCapacity, compaction->emittedCount),
        .vertexCountPerParticle = kVertexCountPerParticle,
        .startVertexLocation = 0U,
        .startInstanceLocation = 0U,
    };
    auto arguments = BuildIndirectDrawArguments(drawRequest);
    if (!arguments)
    {
        return std::unexpected(
            lgp::framework::MakeError("BuildReferenceReadback", std::string{"Indirect arguments failed: "} +
                                                                    std::string{ContractErrorName(arguments.error())}));
    }

    auto checksum = ChecksumParticleState(*state);
    if (!checksum)
    {
        return std::unexpected(
            lgp::framework::MakeError("BuildReferenceReadback", std::string{"Checksum failed: "} +
                                                                    std::string{ContractErrorName(checksum.error())}));
    }

    FrameReadback readback{};
    readback.lastPlan = lastReport.plan;
    readback.requested = static_cast<std::uint32_t>(state->counters.requestedCount);
    readback.spawned = static_cast<std::uint32_t>(state->counters.spawnedCount);
    readback.dropped = static_cast<std::uint32_t>(state->counters.droppedByCapacityCount);
    readback.died = static_cast<std::uint32_t>(state->counters.diedCount);
    readback.contact = static_cast<std::uint32_t>(state->counters.contactCount);
    readback.liveCount = compaction->aliveCount;
    readback.emittedCount = compaction->emittedCount;
    readback.compactedSlots = compaction->compactedSlots;
    readback.compactedIdentities.assign(compaction->compactedIdentities.begin(), compaction->compactedIdentities.end());
    readback.indirectArguments = arguments->arguments;
    readback.executedCommandCount = arguments->arguments.instanceCount > 0U ? 1U : 0U;
    readback.checksum = *checksum;

    readback.state.reserve(state->slots.size());
    for (Particle const &particle : state->slots)
    {
        readback.state.push_back(ParticleToGpu(particle));
    }
    readback.readbackChecksum = ChecksumGpuState(readback.state);
    readback.slotsUsed = lastReport.slotsUsed;
    readback.resultSlot = lastReport.resultSlot;
    readback.stagePartition = BuildStagePartition(totalSubsteps > 0U, readback.executedCommandCount > 0U, true);
    readback.guardsIntact = true;
    readback.barriersModelled = true;
    return readback;
}

StagePartition BuildStagePartition(bool emissionRan, bool drawRan, bool readbackRan) noexcept
{
    StagePartition partition{};
    auto record = [&partition](FrameStage stage, bool submitted)
    {
        if (submitted)
        {
            partition.submittedMask |= StageBit(stage);
        }
        else
        {
            partition.skippedMask |= StageBit(stage);
        }
    };

    record(FrameStage::Reset, true);
    record(FrameStage::Emission, emissionRan);
    record(FrameStage::Simulation, emissionRan);
    record(FrameStage::Compaction, true);
    record(FrameStage::IndirectArguments, true);
    record(FrameStage::Render, drawRan);
    record(FrameStage::Readback, readbackRan);

    partition.recordedMask = partition.submittedMask | partition.skippedMask | partition.unavailableMask;
    return partition;
}

// ---------------------------------------------------------------------------------------------------------------
// Buffer and barrier helpers.
// ---------------------------------------------------------------------------------------------------------------

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

BufferBarrierState VertexShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_VERTEX_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
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
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 34 buffers must be non-empty."));
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
                                                                "Failed to create a Chapter 34 buffer."));
    }

    if (!name.empty())
    {
        std::wstring const resourceName{name};
        HRESULT const nameResult = buffer.resource_->SetName(resourceName.c_str());
        if (FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name a Chapter 34 buffer."));
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
                                                                    "Failed to map a Chapter 34 buffer."));
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
            lgp::framework::MakeError("WriteBuffer", "The Chapter 34 buffer is not persistently mapped."));
    }
    if (destinationOffset > buffer.size_in_bytes() || bytes.size_bytes() > (buffer.size_in_bytes() - destinationOffset))
    {
        return std::unexpected(
            lgp::framework::MakeError("WriteBuffer", "The Chapter 34 buffer write is out of range."));
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

} // namespace ch34::particles::gpu
