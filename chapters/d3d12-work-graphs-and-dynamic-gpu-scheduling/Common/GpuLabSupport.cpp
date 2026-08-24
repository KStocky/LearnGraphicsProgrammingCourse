#include "GpuLabSupport.hpp"

#include <lgp/framework/pix.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace ch20::work_graphs::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorsPerSlot = 13U;
inline constexpr wchar_t kWorkGraphProgramName[] = L"Ch20WorkGraph";
inline constexpr wchar_t kWorkGraphEntryName[] = L"ClassifyNode";
inline constexpr std::uint64_t kQueueIdentity = 1U;

enum DescriptorIndex : UINT
{
    InputSrv = 0U,
    ClassifiedSrv = 1U,
    TransformedSrv = 2U,
    CanonicalSrv = 3U,
    ClassifiedUav = 4U,
    TransformedUav = 5U,
    CanonicalUav = 6U,
    CountersUav = 7U,
    BucketAggregatesUav = 8U,
    TransformArgumentsUav = 9U,
    TransformCountUav = 10U,
    FinalizeArgumentsUav = 11U,
    FinalizeCountUav = 12U,
};

enum ComputeRootParameter : UINT
{
    ComputeConstants = 0U,
    ComputeSrvTable = 1U,
    ComputeUavTable = 2U,
};

enum GraphicsRootParameter : UINT
{
    GraphicsConstants = 0U,
    GraphicsSrvTable = 1U,
};

struct DispatchConstants final
{
    std::uint32_t inputCount{};
    std::uint32_t outputCapacity{};
    std::uint32_t bucketCount{kBucketCount};
    std::uint32_t fixtureSeed{};
    std::uint32_t maximumInputRecords{kMaximumInputRecords};
    std::uint32_t threadGroupSize{kThreadGroupSize};
    std::uint32_t executionPath{};
    std::uint32_t reserved{};
};

struct DisplayConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t inputCount{};
    std::uint32_t diagnosticView{};
    std::uint32_t executionPath{};
    std::uint32_t outcome{};
    std::uint32_t activeCount{};
    std::uint32_t finalCount{};
    std::uint32_t overflowCount{};
    std::uint32_t checksum{};
    std::uint32_t tier{};
    std::uint32_t supportStatus{};
    std::uint32_t initializeCount{};
    std::uint32_t reuseCount{};
    std::uint32_t backingMinimum{};
    std::uint32_t backingMaximum{};
};

static_assert(sizeof(DispatchConstants) == 32U);
static_assert(sizeof(DisplayConstants) == 64U);

[[nodiscard]] bool IsValidExecutionPath(ExecutionPath const path) noexcept
{
    return static_cast<std::uint32_t>(path) <= static_cast<std::uint32_t>(ExecutionPath::WorkGraph);
}

[[nodiscard]] bool IsValidFixture(Fixture const fixture) noexcept
{
    return static_cast<std::uint32_t>(fixture) <= static_cast<std::uint32_t>(Fixture::CapacityBoundary);
}

[[nodiscard]] bool IsValidDiagnosticView(DiagnosticView const view) noexcept
{
    return static_cast<std::uint32_t>(view) <= static_cast<std::uint32_t>(DiagnosticView::CapabilityEvidence);
}

[[nodiscard]] std::uint32_t HashWord(std::uint32_t hash, std::uint32_t const value) noexcept
{
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
    {
        hash = (hash ^ ((value >> shift) & 0xffU)) * 16'777'619U;
    }
    return hash;
}

[[nodiscard]] std::uint32_t RecordChecksum(std::uint32_t const stableId, std::uint32_t const bucketIndex,
                                           std::int32_t const sourceValue, std::int32_t const transformedValue,
                                           std::uint32_t const seed) noexcept
{
    std::uint32_t hash = 2'166'136'261U;
    hash = HashWord(hash, stableId);
    hash = HashWord(hash, bucketIndex);
    hash = HashWord(hash, static_cast<std::uint32_t>(sourceValue));
    hash = HashWord(hash, static_cast<std::uint32_t>(transformedValue));
    return HashWord(hash, seed);
}

[[nodiscard]] bool IsActive(WorkGraphEntryRecord const &input, std::uint32_t const seed) noexcept
{
    std::uint32_t const mixed = input.stableId * 1'664'525U + input.bucketSeed * 1'013'904'223U + seed;
    return (mixed & 3U) != 0U;
}

[[nodiscard]] std::uint32_t FixtureInputCount(Fixture const fixture) noexcept
{
    switch (fixture)
    {
    case Fixture::Normal:
        return 24U;
    case Fixture::ZeroWork:
        return 0U;
    case Fixture::CapacityBoundary:
        return kMaximumInputRecords;
    default:
        return 0U;
    }
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] lgp::framework::Error LabContractFailure(char const *operation, LabContractError const error)
{
    return lgp::framework::MakeError(operation, "The Chapter 20 bounded GPU-lab contract rejected the configuration (" +
                                                    std::to_string(static_cast<std::uint32_t>(error)) + ").");
}

[[nodiscard]] lgp::framework::Status ValidateExtent(lgp::framework::Extent2D const size)
{
    if (size.width == 0U || size.height == 0U || size.width > kMaximumWidth || size.height > kMaximumHeight)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateExtent", "Chapter 20 requires a non-empty extent up to 640x360."));
    }
    return {};
}

[[nodiscard]] lgp::framework::Status CompileShader(lgp::framework::ShaderCompiler &compiler,
                                                   lgp::framework::ShaderCompileOptions &options,
                                                   wchar_t const *entryPoint, wchar_t const *targetProfile,
                                                   lgp::framework::CompiledShader &shader)
{
    options.entryPoint = entryPoint;
    options.targetProfile = targetProfile;
    options.defines.clear();
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto result = compiler.Compile(options);
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    shader = std::move(*result);
    return {};
}

[[nodiscard]] lgp::framework::Status CompileLibrary(lgp::framework::ShaderCompiler &compiler,
                                                    lgp::framework::ShaderCompileOptions &options,
                                                    lgp::framework::CompiledShader &shader)
{
    options.entryPoint.clear();
    options.targetProfile = L"lib_6_8";
    options.defines = {{L"LGP_ENABLE_WORK_GRAPH_NODES", L"1"}};
    options.additionalArguments = {L"-T", options.targetProfile};
    auto result = compiler.Compile(options);
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    shader = std::move(*result);
    return {};
}

[[nodiscard]] lgp::framework::Status CreateComputePipeline(ID3D12Device10 &device, ID3D12RootSignature &rootSignature,
                                                           lgp::framework::CompiledShader const &shader,
                                                           char const *label, ComPtr<ID3D12PipelineState> &pipeline)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &rootSignature;
    description.CS = shader.Bytecode();
    HRESULT const result =
        device.CreateComputePipelineState(&description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState", result,
                                             std::string{"Failed to create the Chapter 20 "} + label + " pipeline."));
    }
    return {};
}

[[nodiscard]] bool ProgramIdentifierIsValid(D3D12_PROGRAM_IDENTIFIER const &identifier) noexcept
{
    return std::any_of(std::begin(identifier.OpaqueData), std::end(identifier.OpaqueData),
                       [](std::uint64_t const value) { return value != 0U; });
}

[[nodiscard]] WorkGraphsTier ConvertTier(D3D12_WORK_GRAPHS_TIER const tier) noexcept
{
    return tier >= D3D12_WORK_GRAPHS_TIER_1_0 ? WorkGraphsTier::Tier1_0 : WorkGraphsTier::NotSupported;
}

[[nodiscard]] std::uint32_t GroupCount(std::size_t const inputCount) noexcept
{
    return static_cast<std::uint32_t>((inputCount + kThreadGroupSize - 1U) / kThreadGroupSize);
}

[[nodiscard]] std::uint32_t ClampToUint32(std::uint64_t const value) noexcept
{
    return value > std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max()
                                                             : static_cast<std::uint32_t>(value);
}

} // namespace

std::expected<void, LabContractError> ValidateLabConfiguration(LabConfiguration const &configuration,
                                                               LabEdition const edition) noexcept
{
    if (!IsValidExecutionPath(configuration.path))
    {
        return std::unexpected(LabContractError::InvalidExecutionPath);
    }
    if (edition == LabEdition::Starter && configuration.path != ExecutionPath::FixedDispatch)
    {
        return std::unexpected(LabContractError::InvalidExecutionPath);
    }
    if (!IsValidFixture(configuration.fixture))
    {
        return std::unexpected(LabContractError::InvalidFixture);
    }
    if (!IsValidDiagnosticView(configuration.diagnosticView))
    {
        return std::unexpected(LabContractError::InvalidDiagnosticView);
    }
    if (configuration.outputCapacity > kMaximumInputRecords)
    {
        return std::unexpected(LabContractError::OutputCapacityExceeded);
    }
    if (FixtureInputCount(configuration.fixture) > kMaximumInputRecords)
    {
        return std::unexpected(LabContractError::InputCapacityExceeded);
    }
    return {};
}

std::expected<LabReference, LabContractError> BuildLabReference(LabConfiguration const &configuration)
{
    if (auto const validation = ValidateLabConfiguration(configuration, LabEdition::Solution); !validation)
    {
        return std::unexpected(validation.error());
    }

    std::uint32_t const inputCount = FixtureInputCount(configuration.fixture);
    LabReference reference{};
    reference.inputs.reserve(inputCount);
    reference.canonicalRecords.resize(inputCount);
    reference.externalUavBucketAggregates.resize(kBucketCount);
    std::set<std::uint32_t> stableIds{};

    for (std::uint32_t index = 0U; index < inputCount; ++index)
    {
        std::uint32_t const sourceBits = index * 37U + configuration.seed;
        WorkGraphEntryRecord const input{
            .dispatchGrid = 1U,
            .stableId = index,
            .bucketSeed = (index * 11U + (configuration.seed >> 5U)) & 0xffU,
            .sourceValue = static_cast<std::int32_t>(sourceBits % 201U) - 100,
        };
        if (!stableIds.insert(input.stableId).second)
        {
            return std::unexpected(LabContractError::DuplicateStableIdentity);
        }
        reference.inputs.push_back(input);
    }

    reference.counters.inputCount = inputCount;
    reference.counters.zeroWork = inputCount == 0U ? 1U : 0U;
    reference.counters.stageMask = inputCount == 0U ? 0U : kAllStageBits;

    for (WorkGraphEntryRecord const &input : reference.inputs)
    {
        bool const active = IsActive(input, configuration.seed);
        if (!active)
        {
            continue;
        }
        ++reference.counters.activeCount;
        if (input.stableId >= configuration.outputCapacity)
        {
            ++reference.counters.overflowCount;
            continue;
        }

        std::uint32_t const bucketIndex = (input.bucketSeed + input.stableId + configuration.seed) % kBucketCount;
        std::int64_t const transformed64 = static_cast<std::int64_t>(input.sourceValue) * 3 +
                                           static_cast<std::int64_t>(bucketIndex) * 17 -
                                           static_cast<std::int64_t>((configuration.seed >> 3U) & 31U);
        if (transformed64 < std::numeric_limits<std::int32_t>::min() ||
            transformed64 > std::numeric_limits<std::int32_t>::max())
        {
            return std::unexpected(LabContractError::ArithmeticOverflow);
        }
        std::int32_t const transformed = static_cast<std::int32_t>(transformed64);
        std::uint32_t const checksum =
            RecordChecksum(input.stableId, bucketIndex, input.sourceValue, transformed, configuration.seed);
        reference.canonicalRecords[input.stableId] = {
            .stableId = input.stableId,
            .bucketIndex = bucketIndex,
            .sourceValue = input.sourceValue,
            .transformedValue = transformed,
            .seed = configuration.seed,
            .recordChecksum = checksum,
            .valid = kCanonicalRecordValid,
            .reserved = 0U,
        };

        BucketAggregate &aggregate = reference.externalUavBucketAggregates[bucketIndex];
        if ((transformed > 0 && aggregate.transformedSum > std::numeric_limits<std::int32_t>::max() - transformed) ||
            (transformed < 0 && aggregate.transformedSum < std::numeric_limits<std::int32_t>::min() - transformed))
        {
            return std::unexpected(LabContractError::ArithmeticOverflow);
        }
        ++aggregate.recordCount;
        aggregate.transformedSum += transformed;
        aggregate.checksum ^= checksum;
        ++reference.counters.finalCount;
        reference.counters.checksum ^= checksum;
        ++reference.counters.externalUavAggregateCount;
    }

    reference.canonicalBytes.resize(reference.canonicalRecords.size() * sizeof(CanonicalRecord));
    if (!reference.canonicalBytes.empty())
    {
        std::memcpy(reference.canonicalBytes.data(), reference.canonicalRecords.data(),
                    reference.canonicalBytes.size());
    }
    return reference;
}

WorkGraphCapability EvaluateWorkGraphCapability(WorkGraphFeatureQuery const &query) noexcept
{
    WorkGraphCapability capability{};
    capability.tier = query.reportedTier;
    capability.commandList10Available = query.commandList10Available;
    if (!query.querySucceeded)
    {
        capability.status = WorkGraphSupportStatus::FeatureQueryFailed;
        return capability;
    }
    if (query.reportedTier == WorkGraphsTier::NotSupported)
    {
        capability.status = WorkGraphSupportStatus::TierUnsupported;
        return capability;
    }
    if (!query.commandList10Available)
    {
        capability.status = WorkGraphSupportStatus::CommandList10Unavailable;
        return capability;
    }
    capability.status = WorkGraphSupportStatus::Supported;
    capability.tier10Executable = true;
    return capability;
}

std::string_view WorkGraphSupportDiagnostic(WorkGraphSupportStatus const status) noexcept
{
    switch (status)
    {
    case WorkGraphSupportStatus::Supported:
        return "D3D12 Work Graph Tier 1.0 execution is available.";
    case WorkGraphSupportStatus::FeatureQueryFailed:
        return "D3D12_FEATURE_D3D12_OPTIONS21 could not be queried.";
    case WorkGraphSupportStatus::TierUnsupported:
        return "The selected adapter reports D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED.";
    case WorkGraphSupportStatus::CommandList10Unavailable:
        return "ID3D12GraphicsCommandList10 is unavailable.";
    case WorkGraphSupportStatus::StateObjectUnavailable:
        return "The Work Graph executable state object could not be created.";
    case WorkGraphSupportStatus::EntrypointAbiMismatch:
        return "The Work Graph entrypoint record ABI does not match the CPU record.";
    case WorkGraphSupportStatus::BackingMemoryInvalid:
        return "The Work Graph backing-memory requirements failed validation.";
    default:
        return "Unknown Work Graph support status.";
    }
}

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

std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
    ID3D12Device10 &device, std::uint64_t const sizeInBytes, D3D12_HEAP_TYPE const heapType,
    D3D12_RESOURCE_FLAGS const flags, std::wstring_view const name, bool const mapPersistently)
{
    if (sizeInBytes == 0U)
    {
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 20 buffers must be non-empty."));
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = heapType;
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
                                                                "Failed to create a Chapter 20 buffer."));
    }
    if (!name.empty())
    {
        std::wstring const objectName{name};
        HRESULT const nameResult = buffer.resource_->SetName(objectName.c_str());
        if (FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name a Chapter 20 buffer."));
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
                                                                    "Failed to map a Chapter 20 buffer."));
        }
        buffer.mappedData_ = static_cast<std::byte *>(mapped);
    }
    buffer.sizeInBytes_ = sizeInBytes;
    return buffer;
}

lgp::framework::Status WriteBuffer(BufferResource &buffer, std::span<std::byte const> const bytes,
                                   std::uint64_t const destinationOffset)
{
    if (bytes.empty())
    {
        return {};
    }
    if (buffer.mapped_data() == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("WriteBuffer", "The Chapter 20 buffer is not persistently mapped."));
    }
    if (destinationOffset > buffer.size_in_bytes() || bytes.size_bytes() > buffer.size_in_bytes() - destinationOffset)
    {
        return std::unexpected(
            lgp::framework::MakeError("WriteBuffer", "The Chapter 20 buffer write is out of range."));
    }
    std::memcpy(buffer.mapped_data() + destinationOffset, bytes.data(), bytes.size_bytes());
    return {};
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

BufferBarrierState PixelShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
}

BufferBarrierState ExecuteIndirectState() noexcept
{
    return {D3D12_BARRIER_SYNC_EXECUTE_INDIRECT, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT};
}

BufferBarrierState CopySourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE};
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

D3D12_BUFFER_BARRIER MakeBufferBarrier(ID3D12Resource &resource, BufferBarrierState const before,
                                       BufferBarrierState const after) noexcept
{
    D3D12_BUFFER_BARRIER barrier{};
    barrier.SyncBefore = before.sync;
    barrier.SyncAfter = after.sync;
    barrier.AccessBefore = before.access;
    barrier.AccessAfter = after.access;
    barrier.pResource = &resource;
    barrier.Offset = 0U;
    barrier.Size = UINT64_MAX;
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

RendererCore::RendererCore(std::filesystem::path shaderPath, LabEdition const edition)
    : shaderPath_{std::move(shaderPath)}, edition_{edition}
{
}

lgp::framework::Status RendererCore::QueryWorkGraphCapability()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 options{};
    HRESULT const result =
        deviceResources_->device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &options, sizeof(options));
    workGraphCapability_ = EvaluateWorkGraphCapability({
        .querySucceeded = SUCCEEDED(result),
        .reportedTier = SUCCEEDED(result) ? ConvertTier(options.WorkGraphsTier) : WorkGraphsTier::NotSupported,
        .commandList10Available = true,
    });
    return {};
}

lgp::framework::Status RendererCore::CreateShaders()
{
    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    lgp::framework::ShaderCompiler compiler = std::move(*compilerResult);
    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = shaderPath_;
    options.includeDirectories = {shaderPath_.parent_path(), shaderPath_.parent_path().parent_path()};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif

    struct ShaderRequest final
    {
        wchar_t const *entryPoint;
        wchar_t const *profile;
        lgp::framework::CompiledShader *shader;
    };
    std::array<ShaderRequest, 6U> const requests{{
        {L"ResetCS", L"cs_6_8", &resetShader_},
        {L"ClassifyCS", L"cs_6_8", &classifyShader_},
        {L"TransformCS", L"cs_6_8", &transformShader_},
        {L"FinalizeCS", L"cs_6_8", &finalizeShader_},
        {L"FullscreenVS", L"vs_6_8", &vertexShader_},
        {L"DisplayPS", L"ps_6_8", &pixelShader_},
    }};
    for (ShaderRequest const &request : requests)
    {
        if (auto status = CompileShader(compiler, options, request.entryPoint, request.profile, *request.shader);
            !status)
        {
            return status;
        }
        shaderArtifacts_.push_back({
            .entryPoint = request.entryPoint,
            .targetProfile = request.profile,
            .bytecodeSize = request.shader->bytecode.size(),
        });
    }

    if (edition_ == LabEdition::Solution)
    {
        if (auto status = CompileLibrary(compiler, options, workGraphLibrary_); !status)
        {
            return status;
        }
        shaderArtifacts_.push_back({
            .entryPoint = {},
            .targetProfile = L"lib_6_8",
            .bytecodeSize = workGraphLibrary_.bytecode.size(),
        });
    }
    return {};
}

lgp::framework::Status RendererCore::CreateRootSignatures()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 4U;
    srvRange.BaseShaderRegister = 0U;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 9U;
    uavRange.BaseShaderRegister = 0U;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER computeParameters[3]{};
    computeParameters[ComputeConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[ComputeConstants].Constants.ShaderRegister = 0U;
    computeParameters[ComputeConstants].Constants.Num32BitValues = sizeof(DispatchConstants) / sizeof(std::uint32_t);
    computeParameters[ComputeConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[ComputeSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeSrvTable].DescriptorTable.NumDescriptorRanges = 1U;
    computeParameters[ComputeSrvTable].DescriptorTable.pDescriptorRanges = &srvRange;
    computeParameters[ComputeSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[ComputeUavTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeUavTable].DescriptorTable.NumDescriptorRanges = 1U;
    computeParameters[ComputeUavTable].DescriptorTable.pDescriptorRanges = &uavRange;
    computeParameters[ComputeUavTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC computeDescription{};
    computeDescription.NumParameters = static_cast<UINT>(std::size(computeParameters));
    computeDescription.pParameters = computeParameters;
    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};
    HRESULT const computeSerialize =
        D3D12SerializeRootSignature(&computeDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                    serialized.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (FAILED(computeSerialize))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", computeSerialize, BlobText(errors.Get())));
    }
    HRESULT const computeCreate =
        deviceResources_->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(computeRootSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(computeCreate))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device::CreateRootSignature", computeCreate,
            "Failed to create the Chapter 20 compute and Work Graph global root signature."));
    }

    D3D12_DESCRIPTOR_RANGE graphicsSrvRange{};
    graphicsSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    graphicsSrvRange.NumDescriptors = 1U;
    graphicsSrvRange.BaseShaderRegister = 3U;
    graphicsSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER graphicsParameters[2]{};
    graphicsParameters[GraphicsConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[GraphicsConstants].Constants.ShaderRegister = 1U;
    graphicsParameters[GraphicsConstants].Constants.Num32BitValues = sizeof(DisplayConstants) / sizeof(std::uint32_t);
    graphicsParameters[GraphicsConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    graphicsParameters[GraphicsSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParameters[GraphicsSrvTable].DescriptorTable.NumDescriptorRanges = 1U;
    graphicsParameters[GraphicsSrvTable].DescriptorTable.pDescriptorRanges = &graphicsSrvRange;
    graphicsParameters[GraphicsSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC graphicsDescription{};
    graphicsDescription.NumParameters = static_cast<UINT>(std::size(graphicsParameters));
    graphicsDescription.pParameters = graphicsParameters;
    graphicsDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    serialized.Reset();
    errors.Reset();
    HRESULT const graphicsSerialize =
        D3D12SerializeRootSignature(&graphicsDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                    serialized.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (FAILED(graphicsSerialize))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", graphicsSerialize, BlobText(errors.Get())));
    }
    HRESULT const graphicsCreate =
        deviceResources_->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(graphicsRootSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(graphicsCreate))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", graphicsCreate,
                                             "Failed to create the Chapter 20 graphics root signature."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreatePipelines()
{
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), resetShader_,
                                            "reset", resetPipeline_);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), classifyShader_,
                                            "classify", classifyPipeline_);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), transformShader_,
                                            "transform", transformPipeline_);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), finalizeShader_,
                                            "finalize", finalizePipeline_);
        !status)
    {
        return status;
    }

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = graphicsRootSignature_.Get();
    description.VS = vertexShader_.Bytecode();
    description.PS = pixelShader_.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = deviceResources_->back_buffer_format();
    description.SampleDesc.Count = 1U;
    HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(graphicsPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                             "Failed to create the Chapter 20 diagnostic graphics pipeline."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreateCommandSignature()
{
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC description{};
    description.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    description.NumArgumentDescs = 1U;
    description.pArgumentDescs = &argument;
    HRESULT const result = deviceResources_->device()->CreateCommandSignature(
        &description, nullptr, IID_PPV_ARGS(dispatchCommandSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateCommandSignature", result,
                                             "Failed to create the Chapter 20 bounded dispatch command signature."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreateWorkGraphStateObject()
{
    if (edition_ != LabEdition::Solution || !workGraphCapability_.tier10Executable)
    {
        return {};
    }

    GraphDescription graph{};
    NodeDescription classify{
        .nodeId = 0U,
        .name = "ClassifyNode",
        .launchMode = LaunchMode::Broadcasting,
        .maximumDispatchGrid = DispatchGrid{1U, 1U, 1U},
        .entryPointInputSizeBytes = sizeof(WorkGraphEntryRecord),
        .outputs = {OutputDeclaration{
            .name = "transformNode",
            .targetNodeName = "TransformNode",
            .maxRecords = 1U,
            .maxOutputSizeBytes = sizeof(ClassifiedRecord),
        }},
    };
    NodeDescription transform{
        .nodeId = 1U,
        .name = "TransformNode",
        .launchMode = LaunchMode::Coalescing,
        .nodeInput = NodeInputDescription{.sizeBytes = sizeof(ClassifiedRecord)},
        .outputs = {OutputDeclaration{
            .name = "finalizeNode",
            .targetNodeName = "FinalizeNode",
            .maxRecords = kThreadGroupSize,
            .maxOutputSizeBytes = sizeof(TransformedRecord),
        }},
    };
    NodeDescription finalize{
        .nodeId = 2U,
        .name = "FinalizeNode",
        .launchMode = LaunchMode::Thread,
        .nodeInput = NodeInputDescription{.sizeBytes = sizeof(TransformedRecord)},
    };
    graph.nodes = {std::move(classify), std::move(transform), std::move(finalize)};
    if (auto const validation = ValidateGraphDescription(graph); !validation)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateGraphDescription", "The Chapter 20 Work Graph topology exceeds a typed construction contract."));
    }

    D3D12_DXIL_LIBRARY_DESC library{};
    library.DXILLibrary = workGraphLibrary_.Bytecode();
    D3D12_GLOBAL_ROOT_SIGNATURE globalRoot{};
    globalRoot.pGlobalRootSignature = computeRootSignature_.Get();
    D3D12_WORK_GRAPH_DESC workGraph{};
    workGraph.ProgramName = kWorkGraphProgramName;
    workGraph.Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
    std::array<D3D12_STATE_SUBOBJECT, 3U> subobjects{{
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRoot},
        {D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH, &workGraph},
    }};
    D3D12_STATE_OBJECT_DESC description{};
    description.Type = D3D12_STATE_OBJECT_TYPE_EXECUTABLE;
    description.NumSubobjects = static_cast<UINT>(subobjects.size());
    description.pSubobjects = subobjects.data();
    HRESULT const result = deviceResources_->device()->CreateStateObject(
        &description, IID_PPV_ARGS(workGraphStateObject_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        workGraphCapability_.status = WorkGraphSupportStatus::StateObjectUnavailable;
        workGraphCapability_.tier10Executable = false;
        return {};
    }

    ComPtr<ID3D12StateObjectProperties1> stateProperties{};
    ComPtr<ID3D12WorkGraphProperties> graphProperties{};
    if (FAILED(workGraphStateObject_.As(&stateProperties)) || FAILED(workGraphStateObject_.As(&graphProperties)))
    {
        workGraphCapability_.status = WorkGraphSupportStatus::StateObjectUnavailable;
        workGraphCapability_.tier10Executable = false;
        workGraphStateObject_.Reset();
        return {};
    }
    programIdentifier_ = stateProperties->GetProgramIdentifier(kWorkGraphProgramName);
    if (!ProgramIdentifierIsValid(programIdentifier_))
    {
        workGraphCapability_.status = WorkGraphSupportStatus::StateObjectUnavailable;
        workGraphCapability_.tier10Executable = false;
        workGraphStateObject_.Reset();
        return {};
    }

    workGraphRuntimeProperties_.workGraphIndex = graphProperties->GetWorkGraphIndex(kWorkGraphProgramName);
    if (workGraphRuntimeProperties_.workGraphIndex == UINT_MAX)
    {
        workGraphCapability_.status = WorkGraphSupportStatus::StateObjectUnavailable;
        workGraphCapability_.tier10Executable = false;
        workGraphStateObject_.Reset();
        return {};
    }
    workGraphRuntimeProperties_.nodeCount = graphProperties->GetNumNodes(workGraphRuntimeProperties_.workGraphIndex);
    D3D12_NODE_ID const entryId{kWorkGraphEntryName, 0U};
    workGraphRuntimeProperties_.entrypointIndex =
        graphProperties->GetEntrypointIndex(workGraphRuntimeProperties_.workGraphIndex, entryId);
    if (workGraphRuntimeProperties_.entrypointIndex == UINT_MAX)
    {
        workGraphCapability_.status = WorkGraphSupportStatus::StateObjectUnavailable;
        workGraphCapability_.tier10Executable = false;
        workGraphStateObject_.Reset();
        return {};
    }
    workGraphRuntimeProperties_.entrypointRecordSizeBytes = graphProperties->GetEntrypointRecordSizeInBytes(
        workGraphRuntimeProperties_.workGraphIndex, workGraphRuntimeProperties_.entrypointIndex);
    workGraphRuntimeProperties_.entrypointRecordAlignmentBytes = graphProperties->GetEntrypointRecordAlignmentInBytes(
        workGraphRuntimeProperties_.workGraphIndex, workGraphRuntimeProperties_.entrypointIndex);
    if (workGraphRuntimeProperties_.entrypointRecordSizeBytes != sizeof(WorkGraphEntryRecord) ||
        workGraphRuntimeProperties_.entrypointRecordAlignmentBytes != alignof(WorkGraphEntryRecord))
    {
        workGraphCapability_.status = WorkGraphSupportStatus::EntrypointAbiMismatch;
        workGraphCapability_.tier10Executable = false;
        return std::unexpected(lgp::framework::MakeError(
            "ID3D12WorkGraphProperties::GetEntrypointRecordSizeInBytes",
            "Chapter 20 Work Graph entrypoint record size or alignment differs from the CPU ABI."));
    }

    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS memory{};
    graphProperties->GetWorkGraphMemoryRequirements(workGraphRuntimeProperties_.workGraphIndex, &memory);
    workGraphRuntimeProperties_.backingRequirements = {
        .minimumSizeBytes = memory.MinSizeInBytes,
        .maximumSizeBytes = memory.MaxSizeInBytes,
        .sizeGranularityBytes = memory.SizeGranularityInBytes,
    };
    BackingMemoryAllocation const probeAllocation = memory.MinSizeInBytes == 0U
                                                        ? BackingMemoryAllocation{}
                                                        : BackingMemoryAllocation{
                                                              .gpuAddress = kBackingMemoryAlignmentBytes,
                                                              .sizeBytes = memory.MinSizeInBytes,
                                                          };
    if (auto const validation =
            ValidateBackingMemoryRequest(workGraphRuntimeProperties_.backingRequirements, probeAllocation);
        !validation)
    {
        workGraphCapability_.status = WorkGraphSupportStatus::BackingMemoryInvalid;
        workGraphCapability_.tier10Executable = false;
        return std::unexpected(lgp::framework::MakeError(
            "ValidateBackingMemoryRequest",
            "Chapter 20 rejected Work Graph backing memory (min=" + std::to_string(memory.MinSizeInBytes) +
                ", max=" + std::to_string(memory.MaxSizeInBytes) +
                ", granularity=" + std::to_string(memory.SizeGranularityInBytes) +
                ", contract=" + std::to_string(static_cast<std::uint32_t>(validation.error())) + ")."));
    }
    programToken_ = {
        .stateObjectLifetime = 1U,
        .programIdentifierLifetime = 1U,
        .graphIdentity = static_cast<std::uint64_t>(workGraphRuntimeProperties_.workGraphIndex) + 1U,
    };
    return {};
}

lgp::framework::Status RendererCore::CreateFrameSlotDescriptors(FrameSlotResources &slot)
{
    auto const makeStructuredSrv =
        [this, &slot](BufferResource &buffer, UINT const index, UINT const elementCount, UINT const stride)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC description{};
        description.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.Buffer.NumElements = elementCount;
        description.Buffer.StructureByteStride = stride;
        deviceResources_->device()->CreateShaderResourceView(buffer.Get(), &description,
                                                             slot.descriptors.CpuHandle(index));
    };
    auto const makeStructuredUav =
        [this, &slot](BufferResource &buffer, UINT const index, UINT const elementCount, UINT const stride)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
        description.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.Buffer.NumElements = elementCount;
        description.Buffer.StructureByteStride = stride;
        deviceResources_->device()->CreateUnorderedAccessView(buffer.Get(), nullptr, &description,
                                                              slot.descriptors.CpuHandle(index));
    };
    auto const makeRawUav = [this, &slot](BufferResource &buffer, UINT const index)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
        description.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        description.Format = DXGI_FORMAT_R32_TYPELESS;
        description.Buffer.NumElements = static_cast<UINT>(buffer.size_in_bytes() / sizeof(std::uint32_t));
        description.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        deviceResources_->device()->CreateUnorderedAccessView(buffer.Get(), nullptr, &description,
                                                              slot.descriptors.CpuHandle(index));
    };

    makeStructuredSrv(slot.input, InputSrv, kMaximumInputRecords, sizeof(WorkGraphEntryRecord));
    makeStructuredSrv(slot.classified, ClassifiedSrv, kMaximumInputRecords, sizeof(ClassifiedRecord));
    makeStructuredSrv(slot.transformed, TransformedSrv, kMaximumInputRecords, sizeof(TransformedRecord));
    makeStructuredSrv(slot.canonical, CanonicalSrv, kMaximumInputRecords, sizeof(CanonicalRecord));
    makeStructuredUav(slot.classified, ClassifiedUav, kMaximumInputRecords, sizeof(ClassifiedRecord));
    makeStructuredUav(slot.transformed, TransformedUav, kMaximumInputRecords, sizeof(TransformedRecord));
    makeStructuredUav(slot.canonical, CanonicalUav, kMaximumInputRecords, sizeof(CanonicalRecord));
    makeStructuredUav(slot.counters, CountersUav, 8U, sizeof(std::uint32_t));
    makeStructuredUav(slot.bucketAggregates, BucketAggregatesUav, kBucketCount, sizeof(BucketAggregate));
    makeRawUav(slot.transformArguments, TransformArgumentsUav);
    makeStructuredUav(slot.transformCount, TransformCountUav, 1U, sizeof(std::uint32_t));
    makeRawUav(slot.finalizeArguments, FinalizeArgumentsUav);
    makeStructuredUav(slot.finalizeCount, FinalizeCountUav, 1U, sizeof(std::uint32_t));
    return {};
}

lgp::framework::Status RendererCore::ValidateAndCreateBackingState(FrameSlotResources &slot,
                                                                   std::uint32_t const frameSlot)
{
    if (edition_ != LabEdition::Solution || !workGraphCapability_.tier10Executable || workGraphStateObject_ == nullptr)
    {
        return {};
    }
    BackingMemoryAllocation const allocation{
        .gpuAddress = slot.backingMemory.gpu_virtual_address(),
        .sizeBytes = slot.backingMemory.size_in_bytes(),
    };
    auto state =
        WorkGraphBackingState::Create(workGraphRuntimeProperties_.backingRequirements, allocation, programToken_);
    if (!state)
    {
        return std::unexpected(lgp::framework::MakeError("WorkGraphBackingState::Create",
                                                         "Chapter 20 frame slot " + std::to_string(frameSlot) +
                                                             " has invalid Work Graph backing memory."));
    }
    slot.backingState = std::move(*state);
    return {};
}

lgp::framework::Status RendererCore::CreateFrameSlotResources(lgp::framework::Extent2D const size)
{
    if (auto status = ValidateExtent(size); !status)
    {
        return status;
    }
    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (std::uint32_t frameSlot = 0U; frameSlot < frameSlots_.size(); ++frameSlot)
    {
        FrameSlotResources &slot = frameSlots_[frameSlot];
        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorsPerSlot);
        if (!descriptors)
        {
            return std::unexpected(std::move(descriptors.error()));
        }
        slot.descriptors = *descriptors;

        auto input = CreateBuffer(*deviceResources_->device(),
                                  static_cast<std::uint64_t>(kMaximumInputRecords) * sizeof(WorkGraphEntryRecord),
                                  D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch20 Input Records", true);
        auto classified = CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(kMaximumInputRecords) * sizeof(ClassifiedRecord),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch20 Classified Records");
        auto transformed = CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(kMaximumInputRecords) * sizeof(TransformedRecord),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch20 Transformed Records");
        auto canonical = CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(kMaximumInputRecords) * sizeof(CanonicalRecord),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch20 Canonical Records");
        auto counters = CreateBuffer(*deviceResources_->device(), sizeof(GpuCounters), D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch20 Counters");
        auto buckets =
            CreateBuffer(*deviceResources_->device(),
                         static_cast<std::uint64_t>(kBucketCount) * sizeof(BucketAggregate), D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch20 External UAV Bucket Aggregates");
        auto transformArguments =
            CreateBuffer(*deviceResources_->device(), sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch20 Transform Indirect Arguments");
        auto transformCount =
            CreateBuffer(*deviceResources_->device(), sizeof(std::uint32_t), D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch20 Transform Indirect Count");
        auto finalizeArguments =
            CreateBuffer(*deviceResources_->device(), sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch20 Finalize Indirect Arguments");
        auto finalizeCount = CreateBuffer(*deviceResources_->device(), sizeof(std::uint32_t), D3D12_HEAP_TYPE_DEFAULT,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch20 Finalize Indirect Count");
        auto canonicalReadback = CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(kMaximumInputRecords) * sizeof(CanonicalRecord),
            D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, L"Ch20 Canonical Readback", true);
        auto countersReadback = CreateBuffer(*deviceResources_->device(), sizeof(GpuCounters), D3D12_HEAP_TYPE_READBACK,
                                             D3D12_RESOURCE_FLAG_NONE, L"Ch20 Counters Readback", true);
        auto bucketsReadback = CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(kBucketCount) * sizeof(BucketAggregate),
            D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, L"Ch20 Bucket Aggregate Readback", true);

        if (!input)
        {
            return std::unexpected(std::move(input.error()));
        }
        if (!classified)
        {
            return std::unexpected(std::move(classified.error()));
        }
        if (!transformed)
        {
            return std::unexpected(std::move(transformed.error()));
        }
        if (!canonical)
        {
            return std::unexpected(std::move(canonical.error()));
        }
        if (!counters)
        {
            return std::unexpected(std::move(counters.error()));
        }
        if (!buckets)
        {
            return std::unexpected(std::move(buckets.error()));
        }
        if (!transformArguments)
        {
            return std::unexpected(std::move(transformArguments.error()));
        }
        if (!transformCount)
        {
            return std::unexpected(std::move(transformCount.error()));
        }
        if (!finalizeArguments)
        {
            return std::unexpected(std::move(finalizeArguments.error()));
        }
        if (!finalizeCount)
        {
            return std::unexpected(std::move(finalizeCount.error()));
        }
        if (!canonicalReadback)
        {
            return std::unexpected(std::move(canonicalReadback.error()));
        }
        if (!countersReadback)
        {
            return std::unexpected(std::move(countersReadback.error()));
        }
        if (!bucketsReadback)
        {
            return std::unexpected(std::move(bucketsReadback.error()));
        }

        slot.input = std::move(*input);
        slot.classified = std::move(*classified);
        slot.transformed = std::move(*transformed);
        slot.canonical = std::move(*canonical);
        slot.counters = std::move(*counters);
        slot.bucketAggregates = std::move(*buckets);
        slot.transformArguments = std::move(*transformArguments);
        slot.transformCount = std::move(*transformCount);
        slot.finalizeArguments = std::move(*finalizeArguments);
        slot.finalizeCount = std::move(*finalizeCount);
        slot.canonicalReadback = std::move(*canonicalReadback);
        slot.countersReadback = std::move(*countersReadback);
        slot.bucketAggregatesReadback = std::move(*bucketsReadback);

        if (edition_ == LabEdition::Solution && workGraphCapability_.tier10Executable &&
            workGraphStateObject_ != nullptr && workGraphRuntimeProperties_.backingRequirements.minimumSizeBytes != 0U)
        {
            auto backing =
                CreateBuffer(*deviceResources_->device(),
                             workGraphRuntimeProperties_.backingRequirements.minimumSizeBytes, D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch20 Per-Frame Work Graph Backing Memory");
            if (!backing)
            {
                return std::unexpected(std::move(backing.error()));
            }
            slot.backingMemory = std::move(*backing);
        }
        if (auto status = ValidateAndCreateBackingState(slot, frameSlot); !status)
        {
            return status;
        }
        if (auto status = CreateFrameSlotDescriptors(slot); !status)
        {
            return status;
        }
    }
    size_ = size;
    hasRendered_ = false;
    return {};
}

LabConfiguration RendererCore::ActiveConfiguration() const noexcept
{
    return headless_ ? headlessConfiguration_ : interactiveConfiguration_;
}

lgp::framework::Status RendererCore::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    interactiveConfiguration_.path = ExecutionPath::FixedDispatch;
    if (auto status = QueryWorkGraphCapability(); !status)
    {
        return status;
    }
    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreateRootSignatures(); !status)
    {
        return status;
    }
    if (auto status = CreatePipelines(); !status)
    {
        return status;
    }
    if (auto status = CreateCommandSignature(); !status)
    {
        return status;
    }
    return CreateWorkGraphStateObject();
}

lgp::framework::Status RendererCore::OnResize(lgp::framework::DeviceResources &deviceResources,
                                              lgp::framework::Extent2D const drawableSize)
{
    DestroyFrameSlotResources(deviceResources);
    return CreateFrameSlotResources(drawableSize);
}

lgp::framework::Status RendererCore::Update(lgp::framework::UpdateContext const &context)
{
    if (!headless_)
    {
        if (edition_ == LabEdition::Solution)
        {
            if (context.input.WasKeyPressed('1'))
            {
                interactiveConfiguration_.path = ExecutionPath::FixedDispatch;
            }
            if (context.input.WasKeyPressed('2'))
            {
                interactiveConfiguration_.path = ExecutionPath::ExecuteIndirect;
            }
            if (context.input.WasKeyPressed('3'))
            {
                interactiveConfiguration_.path = ExecutionPath::WorkGraph;
            }
        }
        if (context.input.WasKeyPressed('Q'))
        {
            interactiveConfiguration_.fixture = Fixture::Normal;
        }
        if (context.input.WasKeyPressed('W'))
        {
            interactiveConfiguration_.fixture = Fixture::ZeroWork;
        }
        if (context.input.WasKeyPressed('E'))
        {
            interactiveConfiguration_.fixture = Fixture::CapacityBoundary;
        }
        if (context.input.WasKeyPressed('A'))
        {
            --interactiveConfiguration_.seed;
        }
        if (context.input.WasKeyPressed('D'))
        {
            ++interactiveConfiguration_.seed;
        }
        if (context.input.WasKeyPressed('Z'))
        {
            interactiveConfiguration_.diagnosticView = DiagnosticView::CanonicalOutput;
        }
        if (context.input.WasKeyPressed('X'))
        {
            interactiveConfiguration_.diagnosticView = DiagnosticView::StructuralEvidence;
        }
        if (context.input.WasKeyPressed('C'))
        {
            interactiveConfiguration_.diagnosticView = DiagnosticView::FunctionalEvidence;
        }
        if (context.input.WasKeyPressed('V'))
        {
            interactiveConfiguration_.diagnosticView = DiagnosticView::CapabilityEvidence;
        }
    }

    LabConfiguration const configuration = ActiveConfiguration();
    if (auto const validation = ValidateLabConfiguration(configuration, edition_); !validation)
    {
        return std::unexpected(LabContractFailure("ValidateLabConfiguration", validation.error()));
    }
    auto reference = BuildLabReference(configuration);
    if (!reference)
    {
        return std::unexpected(LabContractFailure("BuildLabReference", reference.error()));
    }
    currentReference_ = std::move(*reference);
    return {};
}

lgp::framework::Status RendererCore::RecordReset(ID3D12GraphicsCommandList7 &commandList, FrameSlotResources &slot,
                                                 LabReference const &reference, LabConfiguration const &configuration)
{
    if (auto status = WriteBuffer(slot.input, std::span<WorkGraphEntryRecord const>{reference.inputs}); !status)
    {
        return status;
    }

    BufferBarrierState const classifiedBefore =
        slot.normalizedResourceStateInitialized ? ComputeShaderResourceState() : NoAccessState();
    BufferBarrierState const transformedBefore =
        slot.normalizedResourceStateInitialized ? ComputeShaderResourceState() : NoAccessState();
    BufferBarrierState const canonicalBefore =
        slot.normalizedResourceStateInitialized ? CopySourceState() : NoAccessState();
    BufferBarrierState const evidenceBefore =
        slot.normalizedResourceStateInitialized ? CopySourceState() : NoAccessState();
    BufferBarrierState const indirectBefore =
        slot.normalizedResourceStateInitialized ? ComputeUnorderedAccessState() : NoAccessState();
    std::vector<D3D12_BUFFER_BARRIER> barriers{
        MakeBufferBarrier(*slot.classified.Get(), classifiedBefore, ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.transformed.Get(), transformedBefore, ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.canonical.Get(), canonicalBefore, ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.counters.Get(), evidenceBefore, ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.bucketAggregates.Get(), evidenceBefore, ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.transformArguments.Get(), indirectBefore, ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.transformCount.Get(), indirectBefore, ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.finalizeArguments.Get(), indirectBefore, ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.finalizeCount.Get(), indirectBefore, ComputeUnorderedAccessState()),
    };
    SubmitBufferBarriers(commandList, barriers);

    DispatchConstants const constants{
        .inputCount = static_cast<std::uint32_t>(reference.inputs.size()),
        .outputCapacity = configuration.outputCapacity,
        .bucketCount = kBucketCount,
        .fixtureSeed = configuration.seed,
        .executionPath = static_cast<std::uint32_t>(configuration.path),
    };
    commandList.SetComputeRootSignature(computeRootSignature_.Get());
    commandList.SetComputeRoot32BitConstants(ComputeConstants, sizeof(constants) / sizeof(std::uint32_t), &constants,
                                             0U);
    commandList.SetComputeRootDescriptorTable(ComputeSrvTable, slot.descriptors.GpuHandle(InputSrv));
    commandList.SetComputeRootDescriptorTable(ComputeUavTable, slot.descriptors.GpuHandle(ClassifiedUav));
    commandList.SetPipelineState(resetPipeline_.Get());
    lgp::framework::PixEventScope const resetEvent{commandList, lgp::framework::PixColor(70U, 70U, 70U),
                                                   L"Ch20 Reset Per-Frame App-Owned State"};
    commandList.Dispatch(GroupCount(kMaximumInputRecords), 1U, 1U);
    barriers = {
        MakeBufferBarrier(*slot.classified.Get(), ComputeUnorderedAccessState(), ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.transformed.Get(), ComputeUnorderedAccessState(), ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.canonical.Get(), ComputeUnorderedAccessState(), ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.counters.Get(), ComputeUnorderedAccessState(), ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.bucketAggregates.Get(), ComputeUnorderedAccessState(), ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.transformArguments.Get(), ComputeUnorderedAccessState(), ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.transformCount.Get(), ComputeUnorderedAccessState(), ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.finalizeArguments.Get(), ComputeUnorderedAccessState(), ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.finalizeCount.Get(), ComputeUnorderedAccessState(), ComputeUnorderedAccessState()),
    };
    SubmitBufferBarriers(commandList, barriers);
    slot.normalizedResourceStateInitialized = false;
    return {};
}

void RendererCore::RecordFixedPath(ID3D12GraphicsCommandList7 &commandList, FrameSlotResources &slot,
                                   LabReference const &reference, LabConfiguration const &configuration,
                                   StructuralEvidence &evidence)
{
    if (reference.inputs.empty())
    {
        return;
    }
    std::uint32_t const groupCount = GroupCount(reference.inputs.size());
    lgp::framework::PixEventScope const pathEvent{commandList, lgp::framework::PixColor(30U, 120U, 220U),
                                                  L"Ch20 Fixed Dispatch Path"};
    {
        lgp::framework::PixEventScope const stage{commandList, lgp::framework::PixColor(45U, 140U, 230U),
                                                  L"Ch20 Fixed Classify"};
        commandList.SetPipelineState(classifyPipeline_.Get());
        commandList.Dispatch(groupCount, 1U, 1U);
    }
    std::vector<D3D12_BUFFER_BARRIER> barriers{
        MakeBufferBarrier(*slot.classified.Get(), ComputeUnorderedAccessState(), ComputeShaderResourceState()),
    };
    SubmitBufferBarriers(commandList, barriers);
    {
        lgp::framework::PixEventScope const stage{commandList, lgp::framework::PixColor(45U, 185U, 190U),
                                                  L"Ch20 Fixed Transform and External UAV Aggregate"};
        commandList.SetPipelineState(transformPipeline_.Get());
        commandList.Dispatch(groupCount, 1U, 1U);
    }
    barriers = {
        MakeBufferBarrier(*slot.transformed.Get(), ComputeUnorderedAccessState(), ComputeShaderResourceState()),
    };
    SubmitBufferBarriers(commandList, barriers);
    {
        lgp::framework::PixEventScope const stage{commandList, lgp::framework::PixColor(80U, 210U, 120U),
                                                  L"Ch20 Fixed Finalize"};
        commandList.SetPipelineState(finalizePipeline_.Get());
        commandList.Dispatch(groupCount, 1U, 1U);
    }
    evidence.fixedDispatchCallCount = 3U;
    evidence.applicationOwnedIntermediateBufferCount = 9U;
    evidence.graphInputOwnership = DispatchInputOwnership::CpuCopiedAtCommandRecording;
    (void)configuration;
}

void RendererCore::RecordExecuteIndirectPath(ID3D12GraphicsCommandList7 &commandList, FrameSlotResources &slot,
                                             LabReference const &reference, LabConfiguration const &configuration,
                                             StructuralEvidence &evidence)
{
    if (reference.inputs.empty())
    {
        return;
    }
    lgp::framework::PixEventScope const pathEvent{commandList, lgp::framework::PixColor(225U, 135U, 35U),
                                                  L"Ch20 GPU-Authored ExecuteIndirect Path"};
    {
        lgp::framework::PixEventScope const stage{commandList, lgp::framework::PixColor(230U, 155U, 45U),
                                                  L"Ch20 Indirect Classify and Author Transform Dispatch"};
        commandList.SetPipelineState(classifyPipeline_.Get());
        commandList.Dispatch(GroupCount(reference.inputs.size()), 1U, 1U);
    }
    std::vector<D3D12_BUFFER_BARRIER> barriers{
        MakeBufferBarrier(*slot.classified.Get(), ComputeUnorderedAccessState(), ComputeShaderResourceState()),
        MakeBufferBarrier(*slot.transformArguments.Get(), ComputeUnorderedAccessState(), ExecuteIndirectState()),
        MakeBufferBarrier(*slot.transformCount.Get(), ComputeUnorderedAccessState(), ExecuteIndirectState()),
    };
    SubmitBufferBarriers(commandList, barriers);
    {
        lgp::framework::PixEventScope const stage{commandList, lgp::framework::PixColor(235U, 175U, 55U),
                                                  L"Ch20 ExecuteIndirect Transform"};
        commandList.SetPipelineState(transformPipeline_.Get());
        commandList.ExecuteIndirect(dispatchCommandSignature_.Get(), 1U, slot.transformArguments.Get(), 0U,
                                    slot.transformCount.Get(), 0U);
    }
    barriers = {
        MakeBufferBarrier(*slot.transformed.Get(), ComputeUnorderedAccessState(), ComputeShaderResourceState()),
        MakeBufferBarrier(*slot.transformArguments.Get(), ExecuteIndirectState(), ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.transformCount.Get(), ExecuteIndirectState(), ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.finalizeArguments.Get(), ComputeUnorderedAccessState(), ExecuteIndirectState()),
        MakeBufferBarrier(*slot.finalizeCount.Get(), ComputeUnorderedAccessState(), ExecuteIndirectState()),
    };
    SubmitBufferBarriers(commandList, barriers);
    {
        lgp::framework::PixEventScope const stage{commandList, lgp::framework::PixColor(240U, 205U, 65U),
                                                  L"Ch20 ExecuteIndirect Finalize"};
        commandList.SetPipelineState(finalizePipeline_.Get());
        commandList.ExecuteIndirect(dispatchCommandSignature_.Get(), 1U, slot.finalizeArguments.Get(), 0U,
                                    slot.finalizeCount.Get(), 0U);
    }
    barriers = {
        MakeBufferBarrier(*slot.finalizeArguments.Get(), ExecuteIndirectState(), ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.finalizeCount.Get(), ExecuteIndirectState(), ComputeUnorderedAccessState()),
    };
    SubmitBufferBarriers(commandList, barriers);
    evidence.fixedDispatchCallCount = 1U;
    evidence.executeIndirectCallCount = 2U;
    evidence.applicationOwnedIntermediateBufferCount = 9U;
    evidence.graphInputOwnership = DispatchInputOwnership::CpuCopiedAtCommandRecording;
    (void)configuration;
}

void RendererCore::CompletePendingBackingUse(FrameSlotResources &slot) noexcept
{
    if (slot.backingUsePending && slot.backingState)
    {
        (void)slot.backingState->EndUse(kQueueIdentity);
        slot.backingUsePending = false;
    }
}

lgp::framework::Status RendererCore::RecordWorkGraphPath(ID3D12GraphicsCommandList7 &commandList,
                                                         FrameSlotResources &slot, LabReference const &reference,
                                                         LabConfiguration const &configuration,
                                                         StructuralEvidence &evidence, ExecutionOutcome &outcome)
{
    if (!workGraphCapability_.tier10Executable || workGraphStateObject_ == nullptr)
    {
        outcome = ExecutionOutcome::Unsupported;
        return {};
    }

    ComPtr<ID3D12GraphicsCommandList10> commandList10{};
    HRESULT const interfaceResult = commandList.QueryInterface(IID_PPV_ARGS(commandList10.ReleaseAndGetAddressOf()));
    if (FAILED(interfaceResult))
    {
        workGraphCapability_.status = WorkGraphSupportStatus::CommandList10Unavailable;
        workGraphCapability_.commandList10Available = false;
        workGraphCapability_.tier10Executable = false;
        outcome = ExecutionOutcome::Unsupported;
        return {};
    }

    CompletePendingBackingUse(slot);
    bool const initialize = slot.backingState && !slot.backingState->IsInitialized();
    if (!slot.backingState)
    {
        return std::unexpected(
            lgp::framework::MakeError("RecordWorkGraphPath", "The Chapter 20 Work Graph backing state is missing."));
    }
    ProgramLifetime const lifetime{
        .token = programToken_,
        .stateObjectAlive = workGraphStateObject_ != nullptr,
        .programIdentifierAlive = ProgramIdentifierIsValid(programIdentifier_),
    };
    if (auto const begin = slot.backingState->BeginUse(lifetime, kQueueIdentity, initialize); !begin)
    {
        return std::unexpected(lgp::framework::MakeError(
            "WorkGraphBackingState::BeginUse", "The Chapter 20 Work Graph backing-state lifetime check failed."));
    }
    slot.backingUsePending = true;
    if (initialize)
    {
        ++slot.backingInitializeCount;
    }
    else
    {
        ++slot.backingReuseCount;
    }

    if (slot.backingMemory.Get() != nullptr)
    {
        BufferBarrierState const before =
            slot.backingAccessInitialized ? ComputeUnorderedAccessState() : NoAccessState();
        std::vector<D3D12_BUFFER_BARRIER> barriers{
            MakeBufferBarrier(*slot.backingMemory.Get(), before, ComputeUnorderedAccessState()),
        };
        SubmitBufferBarriers(commandList, barriers);
        slot.backingAccessInitialized = true;
    }

    D3D12_SET_PROGRAM_DESC program{};
    program.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
    program.WorkGraph.ProgramIdentifier = programIdentifier_;
    program.WorkGraph.Flags = initialize ? D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE : D3D12_SET_WORK_GRAPH_FLAG_NONE;
    program.WorkGraph.BackingMemory = {
        .StartAddress = slot.backingMemory.gpu_virtual_address(),
        .SizeInBytes = slot.backingMemory.size_in_bytes(),
    };

    DispatchRequest const request{
        .mode = DispatchMode::NodeCpuInput,
        .commandListType = CommandListType::Direct,
        .recordCount = reference.inputs.size(),
        .workCount = reference.inputs.size(),
        .nodeInputCount = reference.inputs.empty() ? 0U : 1U,
        .cpuInputAvailable = !reference.inputs.empty(),
    };
    auto const dispatchValidation = ValidateDispatchRequest(request);
    if (!dispatchValidation)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateDispatchRequest", "The Chapter 20 CPU-input DispatchGraph request failed validation."));
    }

    D3D12_DISPATCH_GRAPH_DESC dispatch{};
    dispatch.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
    dispatch.NodeCPUInput.EntrypointIndex = workGraphRuntimeProperties_.entrypointIndex;
    dispatch.NodeCPUInput.NumRecords = static_cast<UINT>(reference.inputs.size());
    dispatch.NodeCPUInput.pRecords = reference.inputs.empty() ? nullptr : reference.inputs.data();
    dispatch.NodeCPUInput.RecordStrideInBytes = sizeof(WorkGraphEntryRecord);

    commandList.SetComputeRootSignature(computeRootSignature_.Get());
    DispatchConstants const constants{
        .inputCount = static_cast<std::uint32_t>(reference.inputs.size()),
        .outputCapacity = configuration.outputCapacity,
        .bucketCount = kBucketCount,
        .fixtureSeed = configuration.seed,
        .executionPath = static_cast<std::uint32_t>(configuration.path),
    };
    commandList.SetComputeRoot32BitConstants(ComputeConstants, sizeof(constants) / sizeof(std::uint32_t), &constants,
                                             0U);
    commandList.SetComputeRootDescriptorTable(ComputeSrvTable, slot.descriptors.GpuHandle(InputSrv));
    commandList.SetComputeRootDescriptorTable(ComputeUavTable, slot.descriptors.GpuHandle(ClassifiedUav));

    lgp::framework::PixEventScope const pathEvent{commandList, lgp::framework::PixColor(155U, 65U, 225U),
                                                  L"Ch20 Work Graph: Broadcasting -> Coalescing -> Thread"};
    commandList10->SetProgram(&program);
    commandList10->DispatchGraph(&dispatch);

    evidence.dispatchGraphCallCount = 1U;
    evidence.applicationOwnedIntermediateBufferCount = 9U;
    evidence.opaqueBackingAllocationCount = slot.backingMemory.Get() == nullptr ? 0U : 1U;
    evidence.graphBroadcastingNodeCount = 1U;
    evidence.graphCoalescingNodeCount = 1U;
    evidence.graphThreadNodeCount = 1U;
    evidence.graphInputOwnership = dispatchValidation->inputOwnership;
    outcome = ExecutionOutcome::Executed;
    return {};
}

void RendererCore::RecordDisplayAndReadback(lgp::framework::FrameContext const &frameContext, FrameSlotResources &slot,
                                            LabConfiguration const &configuration, StructuralEvidence const &evidence,
                                            ExecutionOutcome const outcome)
{
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    bool const fixedStagesRan = !currentReference_.inputs.empty() && outcome == ExecutionOutcome::Executed &&
                                configuration.path != ExecutionPath::WorkGraph;
    BufferBarrierState const classifiedBefore =
        fixedStagesRan ? ComputeShaderResourceState() : ComputeUnorderedAccessState();
    BufferBarrierState const transformedBefore =
        fixedStagesRan ? ComputeShaderResourceState() : ComputeUnorderedAccessState();
    std::vector<D3D12_BUFFER_BARRIER> barriers{
        MakeBufferBarrier(*slot.classified.Get(), classifiedBefore, ComputeShaderResourceState()),
        MakeBufferBarrier(*slot.transformed.Get(), transformedBefore, ComputeShaderResourceState()),
        MakeBufferBarrier(*slot.canonical.Get(), ComputeUnorderedAccessState(), PixelShaderResourceState()),
        MakeBufferBarrier(*slot.counters.Get(), ComputeUnorderedAccessState(), CopySourceState()),
        MakeBufferBarrier(*slot.bucketAggregates.Get(), ComputeUnorderedAccessState(), CopySourceState()),
    };
    SubmitBufferBarriers(commandList, barriers);

    std::vector<D3D12_TEXTURE_BARRIER> textureBarriers{
        MakeTextureBarrier(*frameContext.renderTarget, FrameStartState(frameContext), RenderTargetState()),
    };
    SubmitTextureBarriers(commandList, textureBarriers);
    float const clearColor[]{0.01F, 0.01F, 0.018F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    std::uint32_t initializeCount{};
    std::uint32_t reuseCount{};
    for (FrameSlotResources const &frameSlot : frameSlots_)
    {
        initializeCount += frameSlot.backingInitializeCount;
        reuseCount += frameSlot.backingReuseCount;
    }
    DisplayConstants const display{
        .width = size_.width,
        .height = size_.height,
        .inputCount = static_cast<std::uint32_t>(currentReference_.inputs.size()),
        .diagnosticView = static_cast<std::uint32_t>(configuration.diagnosticView),
        .executionPath = static_cast<std::uint32_t>(configuration.path),
        .outcome = static_cast<std::uint32_t>(outcome),
        .activeCount = currentReference_.counters.activeCount,
        .finalCount = currentReference_.counters.finalCount,
        .overflowCount = currentReference_.counters.overflowCount,
        .checksum = currentReference_.counters.checksum,
        .tier = static_cast<std::uint32_t>(workGraphCapability_.tier),
        .supportStatus = static_cast<std::uint32_t>(workGraphCapability_.status),
        .initializeCount = initializeCount,
        .reuseCount = reuseCount,
        .backingMinimum = ClampToUint32(workGraphRuntimeProperties_.backingRequirements.minimumSizeBytes),
        .backingMaximum = ClampToUint32(workGraphRuntimeProperties_.backingRequirements.maximumSizeBytes),
    };
    commandList.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    commandList.SetGraphicsRoot32BitConstants(GraphicsConstants, sizeof(display) / sizeof(std::uint32_t), &display, 0U);
    commandList.SetGraphicsRootDescriptorTable(GraphicsSrvTable, slot.descriptors.GpuHandle(CanonicalSrv));
    commandList.SetPipelineState(graphicsPipeline_.Get());
    commandList.DrawInstanced(3U, 1U, 0U, 0U);

    barriers = {
        MakeBufferBarrier(*slot.canonical.Get(), PixelShaderResourceState(), CopySourceState()),
    };
    SubmitBufferBarriers(commandList, barriers);
    commandList.CopyBufferRegion(slot.canonicalReadback.Get(), 0U, slot.canonical.Get(), 0U,
                                 slot.canonical.size_in_bytes());
    commandList.CopyBufferRegion(slot.countersReadback.Get(), 0U, slot.counters.Get(), 0U,
                                 slot.counters.size_in_bytes());
    commandList.CopyBufferRegion(slot.bucketAggregatesReadback.Get(), 0U, slot.bucketAggregates.Get(), 0U,
                                 slot.bucketAggregates.size_in_bytes());
    textureBarriers = {
        MakeTextureBarrier(*frameContext.renderTarget, RenderTargetState(), FrameEndState(frameContext)),
    };
    SubmitTextureBarriers(commandList, textureBarriers);
    slot.normalizedResourceStateInitialized = true;
    (void)evidence;
}

lgp::framework::Status RendererCore::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 20 frame slot is out of range."));
    }
    LabConfiguration const configuration = ActiveConfiguration();
    if (auto const validation = ValidateLabConfiguration(configuration, edition_); !validation)
    {
        lastExecutionOutcome_ = ExecutionOutcome::RejectedBeforeSubmission;
        return std::unexpected(LabContractFailure("ValidateLabConfiguration", validation.error()));
    }

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    if (auto status = RecordReset(commandList, slot, currentReference_, configuration); !status)
    {
        return status;
    }

    StructuralEvidence evidence{};
    ExecutionOutcome outcome = ExecutionOutcome::Executed;
    switch (configuration.path)
    {
    case ExecutionPath::FixedDispatch:
        RecordFixedPath(commandList, slot, currentReference_, configuration, evidence);
        break;
    case ExecutionPath::ExecuteIndirect:
        RecordExecuteIndirectPath(commandList, slot, currentReference_, configuration, evidence);
        break;
    case ExecutionPath::WorkGraph:
        if (auto status = RecordWorkGraphPath(commandList, slot, currentReference_, configuration, evidence, outcome);
            !status)
        {
            return status;
        }
        break;
    default:
        outcome = ExecutionOutcome::RejectedBeforeSubmission;
        return std::unexpected(
            lgp::framework::MakeError("Render", "The Chapter 20 execution path is outside the bounded enum."));
    }
    RecordDisplayAndReadback(frameContext, slot, configuration, evidence, outcome);
    lastRenderedConfiguration_ = configuration;
    lastStructuralEvidence_ = evidence;
    lastExecutionOutcome_ = outcome;
    lastRenderedFrameSlot_ = frameContext.frameSlot;
    hasRendered_ = true;
    return {};
}

void RendererCore::DestroyFrameSlotResources(lgp::framework::DeviceResources &deviceResources) noexcept
{
    for (FrameSlotResources &slot : frameSlots_)
    {
        CompletePendingBackingUse(slot);
        if (slot.descriptors)
        {
            deviceResources.shader_visible_cbv_srv_uav_heap().Free(slot.descriptors);
            slot.descriptors = {};
        }
    }
    frameSlots_.clear();
    size_ = {};
    hasRendered_ = false;
}

void RendererCore::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    DestroyFrameSlotResources(deviceResources);
    workGraphStateObject_.Reset();
    dispatchCommandSignature_.Reset();
    graphicsPipeline_.Reset();
    finalizePipeline_.Reset();
    transformPipeline_.Reset();
    classifyPipeline_.Reset();
    resetPipeline_.Reset();
    graphicsRootSignature_.Reset();
    computeRootSignature_.Reset();
    resetShader_ = {};
    classifyShader_ = {};
    transformShader_ = {};
    finalizeShader_ = {};
    vertexShader_ = {};
    pixelShader_ = {};
    workGraphLibrary_ = {};
    programIdentifier_ = {};
    deviceResources_ = nullptr;
}

void RendererCore::ConfigureHeadlessTest(LabConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

std::expected<FrameReadback, lgp::framework::Error> RendererCore::ReadBackOutputs()
{
    if (deviceResources_ == nullptr || frameSlots_.empty() || !hasRendered_)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "No Chapter 20 frame has completed recording."));
    }
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }
    for (FrameSlotResources &slot : frameSlots_)
    {
        CompletePendingBackingUse(slot);
    }

    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    if (slot.canonicalReadback.mapped_data() == nullptr || slot.countersReadback.mapped_data() == nullptr ||
        slot.bucketAggregatesReadback.mapped_data() == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "The Chapter 20 readback buffers are not mapped."));
    }

    FrameReadback readback{};
    readback.configuration = lastRenderedConfiguration_;
    readback.outcome = lastExecutionOutcome_;
    readback.reference = currentReference_;
    readback.canonicalRecords.resize(currentReference_.canonicalRecords.size());
    if (!readback.canonicalRecords.empty())
    {
        std::memcpy(readback.canonicalRecords.data(), slot.canonicalReadback.mapped_data(),
                    readback.canonicalRecords.size() * sizeof(CanonicalRecord));
    }
    std::memcpy(&readback.counters, slot.countersReadback.mapped_data(), sizeof(GpuCounters));
    readback.externalUavBucketAggregates.resize(kBucketCount);
    std::memcpy(readback.externalUavBucketAggregates.data(), slot.bucketAggregatesReadback.mapped_data(),
                readback.externalUavBucketAggregates.size() * sizeof(BucketAggregate));
    readback.canonicalBytes.resize(readback.canonicalRecords.size() * sizeof(CanonicalRecord));
    if (!readback.canonicalBytes.empty())
    {
        std::memcpy(readback.canonicalBytes.data(), readback.canonicalRecords.data(), readback.canonicalBytes.size());
    }

    readback.structural = lastStructuralEvidence_;
    readback.structural.gpuStageMask = readback.counters.stageMask;
    readback.functional = {
        .canonicalBytesEqual = readback.canonicalBytes == currentReference_.canonicalBytes,
        .canonicalRecordsEqual = readback.canonicalRecords == currentReference_.canonicalRecords,
        .bucketAggregatesEqual = readback.externalUavBucketAggregates == currentReference_.externalUavBucketAggregates,
        .countersEqual = readback.counters == currentReference_.counters,
        .expectedActiveCount = currentReference_.counters.activeCount,
        .expectedFinalCount = currentReference_.counters.finalCount,
        .expectedOverflowCount = currentReference_.counters.overflowCount,
        .expectedChecksum = currentReference_.counters.checksum,
    };
    readback.capability.capability = workGraphCapability_;
    readback.capability.runtimeProperties = workGraphRuntimeProperties_;
    readback.capability.backingSlots.reserve(frameSlots_.size());
    for (FrameSlotResources const &frameSlot : frameSlots_)
    {
        readback.capability.backingSlots.push_back({
            .gpuAddress = frameSlot.backingMemory.gpu_virtual_address(),
            .sizeBytes = frameSlot.backingMemory.size_in_bytes(),
            .initializeCount = frameSlot.backingInitializeCount,
            .reuseCount = frameSlot.backingReuseCount,
            .initialized = frameSlot.backingState && frameSlot.backingState->IsInitialized(),
            .stateObjectAliveThroughFence = workGraphStateObject_ != nullptr,
            .programIdentifierAliveThroughFence = ProgramIdentifierIsValid(programIdentifier_),
        });
    }
    readback.physicalProfiling = PhysicalProfilingStatus::NotCollected;
    readback.size = size_;
    readback.frameSlot = lastRenderedFrameSlot_;
    return readback;
}

WorkGraphCapability RendererCore::work_graph_capability() const noexcept
{
    return workGraphCapability_;
}

std::span<ShaderArtifact const> RendererCore::shader_artifacts() const noexcept
{
    return shaderArtifacts_;
}

} // namespace ch20::work_graphs::gpu
