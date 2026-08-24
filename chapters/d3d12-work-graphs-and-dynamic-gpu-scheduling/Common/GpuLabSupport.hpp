#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "WorkGraphsContracts.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ch20::work_graphs::gpu
{

inline constexpr std::uint32_t kThreadGroupSize = 32U;
inline constexpr std::uint32_t kMaximumInputRecords = 64U;
inline constexpr std::uint32_t kBucketCount = 4U;
inline constexpr std::uint32_t kDefaultOutputCapacity = kMaximumInputRecords;
inline constexpr std::uint32_t kMaximumWidth = 640U;
inline constexpr std::uint32_t kMaximumHeight = 360U;
inline constexpr std::uint32_t kCanonicalRecordValid = 1U;
inline constexpr std::uint32_t kClassifyStageBit = 1U << 0U;
inline constexpr std::uint32_t kTransformStageBit = 1U << 1U;
inline constexpr std::uint32_t kFinalizeStageBit = 1U << 2U;
inline constexpr std::uint32_t kAllStageBits = kClassifyStageBit | kTransformStageBit | kFinalizeStageBit;

enum class LabEdition : std::uint8_t
{
    Starter = 0U,
    Solution,
};

enum class ExecutionPath : std::uint32_t
{
    FixedDispatch = 0U,
    ExecuteIndirect,
    WorkGraph,
};

enum class Fixture : std::uint32_t
{
    Normal = 0U,
    ZeroWork,
    CapacityBoundary,
};

enum class DiagnosticView : std::uint32_t
{
    CanonicalOutput = 0U,
    StructuralEvidence,
    FunctionalEvidence,
    CapabilityEvidence,
};

enum class LabContractError : std::uint8_t
{
    InvalidExecutionPath = 0U,
    InvalidFixture,
    InvalidDiagnosticView,
    OutputCapacityExceeded,
    InputCapacityExceeded,
    DuplicateStableIdentity,
    ArithmeticOverflow,
};

enum class WorkGraphSupportStatus : std::uint32_t
{
    Supported = 0U,
    FeatureQueryFailed,
    TierUnsupported,
    CommandList10Unavailable,
    StateObjectUnavailable,
    EntrypointAbiMismatch,
    BackingMemoryInvalid,
};

enum class ExecutionOutcome : std::uint32_t
{
    Executed = 0U,
    Unsupported,
    RejectedBeforeSubmission,
};

enum class PhysicalProfilingStatus : std::uint32_t
{
    NotCollected = 0U,
};

struct LabConfiguration final
{
    ExecutionPath path{ExecutionPath::FixedDispatch};
    Fixture fixture{Fixture::Normal};
    DiagnosticView diagnosticView{DiagnosticView::CanonicalOutput};
    std::uint32_t seed{0x20'2026U};
    std::uint32_t outputCapacity{kDefaultOutputCapacity};

    [[nodiscard]] bool operator==(LabConfiguration const &) const noexcept = default;
};

struct WorkGraphEntryRecord final
{
    std::uint32_t dispatchGrid{1U};
    std::uint32_t stableId{};
    std::uint32_t bucketSeed{};
    std::int32_t sourceValue{};

    [[nodiscard]] bool operator==(WorkGraphEntryRecord const &) const noexcept = default;
};

struct ClassifiedRecord final
{
    std::uint32_t stableId{};
    std::uint32_t bucketIndex{};
    std::int32_t sourceValue{};
    std::uint32_t selected{};

    [[nodiscard]] bool operator==(ClassifiedRecord const &) const noexcept = default;
};

struct TransformedRecord final
{
    std::uint32_t stableId{};
    std::uint32_t bucketIndex{};
    std::int32_t sourceValue{};
    std::int32_t transformedValue{};
    std::uint32_t seed{};
    std::uint32_t recordChecksum{};
    std::uint32_t reserved0{};
    std::uint32_t reserved1{};

    [[nodiscard]] bool operator==(TransformedRecord const &) const noexcept = default;
};

struct CanonicalRecord final
{
    std::uint32_t stableId{};
    std::uint32_t bucketIndex{};
    std::int32_t sourceValue{};
    std::int32_t transformedValue{};
    std::uint32_t seed{};
    std::uint32_t recordChecksum{};
    std::uint32_t valid{};
    std::uint32_t reserved{};

    [[nodiscard]] bool operator==(CanonicalRecord const &) const noexcept = default;
};

struct BucketAggregate final
{
    std::uint32_t recordCount{};
    std::int32_t transformedSum{};
    std::uint32_t checksum{};
    std::uint32_t reserved{};

    [[nodiscard]] bool operator==(BucketAggregate const &) const noexcept = default;
};

struct GpuCounters final
{
    std::uint32_t inputCount{};
    std::uint32_t activeCount{};
    std::uint32_t finalCount{};
    std::uint32_t overflowCount{};
    std::uint32_t checksum{};
    std::uint32_t zeroWork{};
    std::uint32_t stageMask{};
    std::uint32_t externalUavAggregateCount{};

    [[nodiscard]] bool operator==(GpuCounters const &) const noexcept = default;
};

struct LabReference final
{
    std::vector<WorkGraphEntryRecord> inputs{};
    std::vector<CanonicalRecord> canonicalRecords{};
    std::vector<BucketAggregate> externalUavBucketAggregates{};
    GpuCounters counters{};
    std::vector<std::byte> canonicalBytes{};

    [[nodiscard]] bool operator==(LabReference const &) const noexcept = default;
};

struct WorkGraphFeatureQuery final
{
    bool querySucceeded{};
    WorkGraphsTier reportedTier{WorkGraphsTier::NotSupported};
    bool commandList10Available{true};
};

struct WorkGraphCapability final
{
    WorkGraphSupportStatus status{WorkGraphSupportStatus::TierUnsupported};
    WorkGraphsTier tier{WorkGraphsTier::NotSupported};
    bool tier10Executable{};
    bool commandList10Available{};

    [[nodiscard]] bool operator==(WorkGraphCapability const &) const noexcept = default;
};

struct WorkGraphRuntimeProperties final
{
    std::uint32_t workGraphIndex{UINT_MAX};
    std::uint32_t entrypointIndex{UINT_MAX};
    std::uint32_t entrypointRecordSizeBytes{};
    std::uint32_t entrypointRecordAlignmentBytes{};
    std::uint32_t nodeCount{};
    BackingMemoryRequirements backingRequirements{};

    [[nodiscard]] bool operator==(WorkGraphRuntimeProperties const &) const noexcept = default;
};

struct StructuralEvidence final
{
    std::uint32_t fixedDispatchCallCount{};
    std::uint32_t executeIndirectCallCount{};
    std::uint32_t dispatchGraphCallCount{};
    std::uint32_t applicationOwnedIntermediateBufferCount{};
    std::uint32_t opaqueBackingAllocationCount{};
    std::uint32_t graphBroadcastingNodeCount{};
    std::uint32_t graphCoalescingNodeCount{};
    std::uint32_t graphThreadNodeCount{};
    std::uint32_t gpuStageMask{};
    DispatchInputOwnership graphInputOwnership{DispatchInputOwnership::CpuCopiedAtCommandRecording};

    [[nodiscard]] bool operator==(StructuralEvidence const &) const noexcept = default;
};

struct FunctionalEvidence final
{
    bool canonicalBytesEqual{};
    bool canonicalRecordsEqual{};
    bool bucketAggregatesEqual{};
    bool countersEqual{};
    std::uint32_t expectedActiveCount{};
    std::uint32_t expectedFinalCount{};
    std::uint32_t expectedOverflowCount{};
    std::uint32_t expectedChecksum{};

    [[nodiscard]] bool operator==(FunctionalEvidence const &) const noexcept = default;
};

struct BackingSlotEvidence final
{
    std::uint64_t gpuAddress{};
    std::uint64_t sizeBytes{};
    std::uint32_t initializeCount{};
    std::uint32_t reuseCount{};
    bool initialized{};
    bool stateObjectAliveThroughFence{};
    bool programIdentifierAliveThroughFence{};

    [[nodiscard]] bool operator==(BackingSlotEvidence const &) const noexcept = default;
};

struct CapabilityEvidence final
{
    WorkGraphCapability capability{};
    WorkGraphRuntimeProperties runtimeProperties{};
    std::vector<BackingSlotEvidence> backingSlots{};

    [[nodiscard]] bool operator==(CapabilityEvidence const &) const noexcept = default;
};

struct FrameReadback final
{
    LabConfiguration configuration{};
    ExecutionOutcome outcome{ExecutionOutcome::RejectedBeforeSubmission};
    std::vector<CanonicalRecord> canonicalRecords{};
    std::vector<BucketAggregate> externalUavBucketAggregates{};
    GpuCounters counters{};
    std::vector<std::byte> canonicalBytes{};
    LabReference reference{};
    StructuralEvidence structural{};
    FunctionalEvidence functional{};
    CapabilityEvidence capability{};
    PhysicalProfilingStatus physicalProfiling{PhysicalProfilingStatus::NotCollected};
    lgp::framework::Extent2D size{};
    std::uint32_t frameSlot{};
};

struct ShaderArtifact final
{
    std::wstring entryPoint{};
    std::wstring targetProfile{};
    std::size_t bytecodeSize{};

    [[nodiscard]] bool operator==(ShaderArtifact const &) const noexcept = default;
};

struct BufferBarrierState final
{
    D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};
    D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};
};

class BufferResource final
{
  public:
    BufferResource() = default;
    BufferResource(BufferResource &&other) noexcept;
    BufferResource &operator=(BufferResource &&other) noexcept;
    BufferResource(BufferResource const &) = delete;
    BufferResource &operator=(BufferResource const &) = delete;
    ~BufferResource();

    [[nodiscard]] ID3D12Resource *Get() const noexcept
    {
        return resource_.Get();
    }
    [[nodiscard]] std::uint64_t size_in_bytes() const noexcept
    {
        return sizeInBytes_;
    }
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS gpu_virtual_address() const noexcept
    {
        return resource_ == nullptr ? 0U : resource_->GetGPUVirtualAddress();
    }
    [[nodiscard]] std::byte *mapped_data() noexcept
    {
        return mappedData_;
    }
    [[nodiscard]] std::byte const *mapped_data() const noexcept
    {
        return mappedData_;
    }

  private:
    friend std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
        ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
        std::wstring_view name, bool mapPersistently);

    void Reset() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::uint64_t sizeInBytes_{};
    std::byte *mappedData_{};
};

static_assert(sizeof(WorkGraphEntryRecord) == 16U);
static_assert(alignof(WorkGraphEntryRecord) == 4U);
static_assert(sizeof(ClassifiedRecord) == 16U);
static_assert(sizeof(TransformedRecord) == 32U);
static_assert(sizeof(CanonicalRecord) == 32U);
static_assert(sizeof(BucketAggregate) == 16U);
static_assert(sizeof(GpuCounters) == 32U);

[[nodiscard]] std::expected<void, LabContractError> ValidateLabConfiguration(LabConfiguration const &configuration,
                                                                             LabEdition edition) noexcept;
[[nodiscard]] std::expected<LabReference, LabContractError> BuildLabReference(LabConfiguration const &configuration);
[[nodiscard]] WorkGraphCapability EvaluateWorkGraphCapability(WorkGraphFeatureQuery const &query) noexcept;
[[nodiscard]] std::string_view WorkGraphSupportDiagnostic(WorkGraphSupportStatus status) noexcept;

[[nodiscard]] std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
    ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
    std::wstring_view name, bool mapPersistently = false);
[[nodiscard]] lgp::framework::Status WriteBuffer(BufferResource &buffer, std::span<std::byte const> bytes,
                                                 std::uint64_t destinationOffset = 0U);

template <typename T>
[[nodiscard]] inline lgp::framework::Status WriteBuffer(BufferResource &buffer, std::span<T const> values,
                                                        std::uint64_t destinationOffset = 0U)
{
    return WriteBuffer(buffer, std::as_bytes(values), destinationOffset);
}

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] BufferBarrierState NoAccessState() noexcept;
[[nodiscard]] BufferBarrierState ComputeShaderResourceState() noexcept;
[[nodiscard]] BufferBarrierState ComputeUnorderedAccessState() noexcept;
[[nodiscard]] BufferBarrierState PixelShaderResourceState() noexcept;
[[nodiscard]] BufferBarrierState ExecuteIndirectState() noexcept;
[[nodiscard]] BufferBarrierState CopySourceState() noexcept;
[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(
    ID3D12Resource &resource, lgp::framework::TextureBarrierState before, lgp::framework::TextureBarrierState after,
    D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) noexcept;
void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers);
[[nodiscard]] D3D12_BUFFER_BARRIER MakeBufferBarrier(ID3D12Resource &resource, BufferBarrierState before,
                                                     BufferBarrierState after) noexcept;
void SubmitBufferBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_BUFFER_BARRIER> &barriers);

class RendererCore : public lgp::framework::IChapterRenderer
{
  public:
    RendererCore(std::filesystem::path shaderPath, LabEdition edition);
    RendererCore(RendererCore &&) noexcept = default;
    RendererCore &operator=(RendererCore &&) noexcept = default;
    RendererCore(RendererCore const &) = delete;
    RendererCore &operator=(RendererCore const &) = delete;
    ~RendererCore() override = default;

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void ConfigureHeadlessTest(LabConfiguration const &configuration) noexcept;
    [[nodiscard]] std::expected<FrameReadback, lgp::framework::Error> ReadBackOutputs();
    [[nodiscard]] WorkGraphCapability work_graph_capability() const noexcept;
    [[nodiscard]] std::span<ShaderArtifact const> shader_artifacts() const noexcept;

  private:
    struct FrameSlotResources final
    {
        BufferResource input{};
        BufferResource classified{};
        BufferResource transformed{};
        BufferResource canonical{};
        BufferResource counters{};
        BufferResource bucketAggregates{};
        BufferResource transformArguments{};
        BufferResource transformCount{};
        BufferResource finalizeArguments{};
        BufferResource finalizeCount{};
        BufferResource canonicalReadback{};
        BufferResource countersReadback{};
        BufferResource bucketAggregatesReadback{};
        BufferResource backingMemory{};
        lgp::framework::DescriptorAllocation descriptors{};
        std::optional<WorkGraphBackingState> backingState{};
        bool normalizedResourceStateInitialized{};
        bool backingAccessInitialized{};
        bool backingUsePending{};
        std::uint32_t backingInitializeCount{};
        std::uint32_t backingReuseCount{};
    };

    [[nodiscard]] lgp::framework::Status QueryWorkGraphCapability();
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateCommandSignature();
    [[nodiscard]] lgp::framework::Status CreateWorkGraphStateObject();
    [[nodiscard]] lgp::framework::Status CreateFrameSlotResources(lgp::framework::Extent2D size);
    [[nodiscard]] lgp::framework::Status CreateFrameSlotDescriptors(FrameSlotResources &slot);
    [[nodiscard]] lgp::framework::Status ValidateAndCreateBackingState(FrameSlotResources &slot,
                                                                       std::uint32_t frameSlot);
    void DestroyFrameSlotResources(lgp::framework::DeviceResources &deviceResources) noexcept;
    [[nodiscard]] LabConfiguration ActiveConfiguration() const noexcept;
    [[nodiscard]] lgp::framework::Status RecordReset(ID3D12GraphicsCommandList7 &commandList, FrameSlotResources &slot,
                                                     LabReference const &reference,
                                                     LabConfiguration const &configuration);
    void RecordFixedPath(ID3D12GraphicsCommandList7 &commandList, FrameSlotResources &slot,
                         LabReference const &reference, LabConfiguration const &configuration,
                         StructuralEvidence &evidence);
    void RecordExecuteIndirectPath(ID3D12GraphicsCommandList7 &commandList, FrameSlotResources &slot,
                                   LabReference const &reference, LabConfiguration const &configuration,
                                   StructuralEvidence &evidence);
    [[nodiscard]] lgp::framework::Status RecordWorkGraphPath(ID3D12GraphicsCommandList7 &commandList,
                                                             FrameSlotResources &slot, LabReference const &reference,
                                                             LabConfiguration const &configuration,
                                                             StructuralEvidence &evidence, ExecutionOutcome &outcome);
    void RecordDisplayAndReadback(lgp::framework::FrameContext const &frameContext, FrameSlotResources &slot,
                                  LabConfiguration const &configuration, StructuralEvidence const &evidence,
                                  ExecutionOutcome outcome);
    void CompletePendingBackingUse(FrameSlotResources &slot) noexcept;

    std::filesystem::path shaderPath_{};
    LabEdition edition_{LabEdition::Starter};
    bool headless_{};
    bool hasRendered_{};
    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader resetShader_{};
    lgp::framework::CompiledShader classifyShader_{};
    lgp::framework::CompiledShader transformShader_{};
    lgp::framework::CompiledShader finalizeShader_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    lgp::framework::CompiledShader workGraphLibrary_{};
    std::vector<ShaderArtifact> shaderArtifacts_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> resetPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> classifyPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> transformPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> finalizePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatchCommandSignature_{};
    Microsoft::WRL::ComPtr<ID3D12StateObject> workGraphStateObject_{};
    D3D12_PROGRAM_IDENTIFIER programIdentifier_{};
    WorkGraphCapability workGraphCapability_{};
    WorkGraphRuntimeProperties workGraphRuntimeProperties_{};
    ProgramToken programToken_{};
    std::vector<FrameSlotResources> frameSlots_{};
    lgp::framework::Extent2D size_{};
    LabConfiguration headlessConfiguration_{};
    LabConfiguration interactiveConfiguration_{};
    LabConfiguration lastRenderedConfiguration_{};
    LabReference currentReference_{};
    StructuralEvidence lastStructuralEvidence_{};
    ExecutionOutcome lastExecutionOutcome_{ExecutionOutcome::RejectedBeforeSubmission};
    std::uint32_t lastRenderedFrameSlot_{};
};

} // namespace ch20::work_graphs::gpu
