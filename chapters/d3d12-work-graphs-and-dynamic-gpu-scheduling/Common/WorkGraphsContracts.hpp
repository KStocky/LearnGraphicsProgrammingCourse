#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ch20::work_graphs
{

inline constexpr std::uint32_t kShaderModelBaselineMajor = 6U;
inline constexpr std::uint32_t kShaderModelBaselineMinor = 8U;
inline constexpr std::uint32_t kMaximumGraphDepth = 32U;
inline constexpr std::uint64_t kNodeIdSpaceSize = 1ULL << 24U;
inline constexpr std::uint32_t kMaximumBroadcastGridAxis = 65'535U;
inline constexpr std::uint64_t kMaximumBroadcastDispatchCount = (1ULL << 24U) - 1ULL;
inline constexpr std::uint64_t kMaximumNodeSharedMemoryBytes = 32ULL * 1'024ULL;
inline constexpr std::uint64_t kMaximumBroadcastOrCoalescingRecords = 256ULL;
inline constexpr std::uint64_t kMaximumThreadRecords = 8ULL;
inline constexpr std::uint64_t kMaximumThreadOutputBytes = 128ULL;
inline constexpr std::uint64_t kMaximumNodeProducedInputBytes = 32ULL * 1'024ULL;
inline constexpr std::size_t kMaximumOutputDeclarations = 1'024U;
inline constexpr std::size_t kMaximumNodesSharingInput = 256U;
inline constexpr std::uint64_t kTrackedRwTotalMemoryBytes = 48ULL * 1'024ULL;
inline constexpr std::uint64_t kBackingMemoryAlignmentBytes = 256ULL;

enum class ContractError : std::uint8_t
{
    ShaderModelBelowBaseline = 0U,
    WorkGraphsUnsupported,
    EmptyRecordLayout,
    EmptyRecordFieldName,
    DuplicateRecordFieldName,
    ZeroScalarSize,
    InvalidScalarAlignment,
    ScalarSizeMisaligned,
    ZeroArrayElementCount,
    ArithmeticOverflow,
    RecordFieldOffsetMisaligned,
    RecordFieldOverlap,
    ZeroOutputCapacity,
    OutputRecordCountExceeded,
    OutputRecordSizeExceeded,
    ThreadOutputRecordSizeExceeded,
    NodeSharedMemoryExceeded,
    TooManyOutputDeclarations,
    EmptyOutputName,
    TrackedRwOutputSizeExceeded,
    TrackedRwTotalMemoryExceeded,
    DuplicateOutputName,
    InvalidSharedOutputReference,
    IncompatibleSharedOutputBudget,
    InvalidLedgerCapacity,
    ZeroRecordAcquisition,
    OutputCapacityExceeded,
    UnknownRecordAcquisition,
    CompletionCountMismatch,
    EmptyNodeName,
    DuplicateNodeName,
    InvalidNodeMultiplicity,
    NodeIdSpaceExceeded,
    OverlappingNodeIdRange,
    MissingBroadcastGrid,
    UnexpectedDispatchGrid,
    DispatchGridAxisExceeded,
    DispatchGridProductExceeded,
    NodeInputRecordSizeExceeded,
    InvalidSharedInputName,
    IncompatibleSharedInput,
    SharedInputNodeLimitExceeded,
    MissingEntrypoint,
    DanglingOutputTarget,
    OutputTargetMissingNodeInput,
    OutputInputRecordMismatch,
    TrackedRwInputMismatch,
    IllegalSelfRecursion,
    SelfRecursionDepthExceeded,
    MultiNodeCycle,
    GraphDepthExceeded,
    InvalidBackingMemoryQuery,
    UnexpectedBackingMemory,
    BackingMemoryAddressRequired,
    BackingMemoryAddressMisaligned,
    BackingMemorySizeOutOfRange,
    BackingMemoryGranularityMismatch,
    InvalidProgramToken,
    StaleProgramToken,
    InitializeRequired,
    InvalidQueueIdentifier,
    QueueOwnershipConflict,
    QueueReleaseMismatch,
    BundleCommandListForbidden,
    MissingCpuInput,
    UnexpectedCpuInput,
    MissingGpuInputAddress,
    UnexpectedGpuInputAddress,
    GpuInputAddressMisaligned,
    GpuInputStateNotReadable,
    InvalidDispatchNodeCount,
    InvalidFixtureCapacity,
    FixtureInputCapacityExceeded,
    FixtureBucketCapacityExceeded,
    FixtureClassifiedCapacityExceeded,
    FixtureOutputCapacityExceeded,
    DuplicateStableIdentity,
    FixtureSumOverflow,
};

struct RecordFieldDescription final
{
    std::string name{};
    std::uint64_t scalarSizeBytes{};
    std::uint64_t scalarAlignmentBytes{};
    std::uint64_t elementCount{1U};
    std::optional<std::uint64_t> explicitOffsetBytes{};
};

struct RecordFieldLayout final
{
    std::string name{};
    std::uint64_t offsetBytes{};
    std::uint64_t sizeBytes{};

    [[nodiscard]] bool operator==(RecordFieldLayout const &) const noexcept = default;
};

struct RecordLayout final
{
    std::vector<RecordFieldLayout> fields{};
    std::uint64_t strideBytes{};
    std::uint64_t scalarAlignmentBytes{};
    std::uint64_t strideAlignmentBytes{};

    [[nodiscard]] bool operator==(RecordLayout const &) const noexcept = default;
};

[[nodiscard]] std::expected<RecordLayout, ContractError> ValidateRecordLayout(
    std::span<RecordFieldDescription const> fields);

enum class LaunchMode : std::uint8_t
{
    Broadcasting = 0U,
    Coalescing,
    Thread,
};

struct OutputDeclaration final
{
    std::string name{};
    std::string targetNodeName{};
    std::uint64_t maxRecords{};
    std::uint64_t maxOutputSizeBytes{};
    bool trackedReadWriteInput{};
    std::optional<std::string> maxRecordsSharedWith{};
};

struct SharedOutputBudgetGroup final
{
    std::vector<std::string> outputNames{};
    std::uint64_t maxRecords{};

    [[nodiscard]] bool operator==(SharedOutputBudgetGroup const &) const noexcept = default;
};

struct OutputBudgetValidation final
{
    std::vector<SharedOutputBudgetGroup> sharedGroups{};

    [[nodiscard]] bool operator==(OutputBudgetValidation const &) const noexcept = default;
};

[[nodiscard]] std::expected<void, ContractError> ValidateOutputBudget(LaunchMode launchMode,
                                                                      std::uint64_t sharedMemoryBytes,
                                                                      OutputDeclaration const &output) noexcept;
[[nodiscard]] std::expected<OutputBudgetValidation, ContractError> ValidateOutputBudgets(
    LaunchMode launchMode, std::uint64_t sharedMemoryBytes, std::span<OutputDeclaration const> outputs);

struct RecordReservation final
{
    std::uint64_t id{};
    std::uint64_t count{};

    [[nodiscard]] bool operator==(RecordReservation const &) const noexcept = default;
};

class OutputRecordLedger final
{
  public:
    [[nodiscard]] static std::expected<OutputRecordLedger, ContractError> Create(std::uint64_t capacity);

    [[nodiscard]] std::expected<RecordReservation, ContractError> Acquire(std::uint64_t count);
    [[nodiscard]] std::expected<void, ContractError> Complete(RecordReservation const &reservation);

    [[nodiscard]] std::uint64_t Capacity() const noexcept;
    [[nodiscard]] std::uint64_t OutstandingCount() const noexcept;
    [[nodiscard]] std::size_t OutstandingAcquisitionCount() const noexcept;

  private:
    explicit OutputRecordLedger(std::uint64_t capacity) noexcept;

    std::uint64_t capacity_{};
    std::uint64_t outstandingCount_{};
    std::uint64_t nextReservationId_{1U};
    std::vector<RecordReservation> outstanding_{};
};

struct ShaderModel final
{
    std::uint32_t major{};
    std::uint32_t minor{};
};

enum class WorkGraphsTier : std::uint8_t
{
    NotSupported = 0U,
    Tier1_0,
    Tier1_1,
};

struct DispatchGrid final
{
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
};

struct NodeInputDescription final
{
    std::uint64_t sizeBytes{};
    bool trackedReadWrite{};
    std::optional<std::string> sharedInputName{};
};

struct NodeDescription final
{
    std::uint32_t nodeId{};
    std::string name{};
    LaunchMode launchMode{LaunchMode::Broadcasting};
    std::optional<DispatchGrid> maximumDispatchGrid{};
    std::uint32_t nodeArraySize{1U};
    std::uint32_t recursionMultiplicity{1U};
    std::uint32_t maximumSelfRecursionDepth{};
    std::uint64_t sharedMemoryBytes{};
    std::optional<std::uint64_t> entryPointInputSizeBytes{};
    std::optional<NodeInputDescription> nodeInput{};
    std::vector<OutputDeclaration> outputs{};
};

struct GraphDescription final
{
    ShaderModel shaderModel{kShaderModelBaselineMajor, kShaderModelBaselineMinor};
    WorkGraphsTier reportedTier{WorkGraphsTier::Tier1_0};
    std::vector<NodeDescription> nodes{};
};

struct NodeLaunchValidation final
{
    std::string nodeName{};
    std::optional<std::uint64_t> maximumDispatchCount{};
    bool explicitZeroWork{};

    [[nodiscard]] bool operator==(NodeLaunchValidation const &) const noexcept = default;
};

struct GraphValidation final
{
    WorkGraphsTier validatedContractTier{WorkGraphsTier::Tier1_0};
    std::uint64_t nodeIdSpaceUsed{};
    std::uint32_t maximumGraphDepth{};
    std::vector<NodeLaunchValidation> nodeLaunches{};

    [[nodiscard]] bool operator==(GraphValidation const &) const noexcept = default;
};

[[nodiscard]] std::expected<GraphValidation, ContractError> ValidateGraphDescription(
    GraphDescription const &description);

struct BackingMemoryRequirements final
{
    std::uint64_t minimumSizeBytes{};
    std::uint64_t maximumSizeBytes{};
    std::uint64_t sizeGranularityBytes{};

    [[nodiscard]] bool operator==(BackingMemoryRequirements const &) const noexcept = default;
};

struct BackingMemoryAllocation final
{
    std::uint64_t gpuAddress{};
    std::uint64_t sizeBytes{};

    [[nodiscard]] bool operator==(BackingMemoryAllocation const &) const noexcept = default;
};

[[nodiscard]] std::expected<void, ContractError> ValidateBackingMemoryRequest(
    BackingMemoryRequirements const &requirements, BackingMemoryAllocation const &allocation) noexcept;

struct ProgramToken final
{
    // These lifetime identities never expose or serialize opaque program-identifier bytes.
    std::uint64_t stateObjectLifetime{};
    std::uint64_t programIdentifierLifetime{};
    std::uint64_t graphIdentity{};

    [[nodiscard]] bool operator==(ProgramToken const &) const noexcept = default;
};

struct ProgramLifetime final
{
    ProgramToken token{};
    bool stateObjectAlive{};
    bool programIdentifierAlive{};
};

class WorkGraphBackingState final
{
  public:
    [[nodiscard]] static std::expected<WorkGraphBackingState, ContractError> Create(
        BackingMemoryRequirements requirements, BackingMemoryAllocation allocation, ProgramToken program);

    [[nodiscard]] std::expected<void, ContractError> BeginUse(ProgramLifetime const &program,
                                                              std::uint64_t queueIdentity, bool initialize);
    [[nodiscard]] std::expected<void, ContractError> EndUse(std::uint64_t queueIdentity) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> ActiveQueue() const noexcept;
    [[nodiscard]] BackingMemoryAllocation Allocation() const noexcept;
    [[nodiscard]] ProgramToken Program() const noexcept;

  private:
    WorkGraphBackingState(BackingMemoryRequirements requirements, BackingMemoryAllocation allocation,
                          ProgramToken program) noexcept;

    BackingMemoryRequirements requirements_{};
    BackingMemoryAllocation allocation_{};
    ProgramToken program_{};
    bool initialized_{};
    std::optional<std::uint64_t> activeQueue_{};
};

enum class DispatchMode : std::uint8_t
{
    NodeCpuInput = 0U,
    NodeGpuInput,
    MultiNodeCpuInput,
    MultiNodeGpuInput,
};

enum class CommandListType : std::uint8_t
{
    Direct = 0U,
    Compute,
    Bundle,
};

enum class GpuInputState : std::uint8_t
{
    NotShaderResourceReadable = 0U,
    ShaderResourceReadable,
};

enum class DispatchInputOwnership : std::uint8_t
{
    CpuCopiedAtCommandRecording = 0U,
    GpuReadAtExecution,
};

struct DispatchRequest final
{
    DispatchMode mode{DispatchMode::NodeCpuInput};
    CommandListType commandListType{CommandListType::Direct};
    std::uint64_t recordCount{};
    std::uint64_t workCount{};
    std::uint32_t nodeInputCount{};
    bool cpuInputAvailable{};
    std::optional<std::uint64_t> gpuInputAddress{};
    GpuInputState gpuInputState{GpuInputState::NotShaderResourceReadable};
};

struct DispatchValidation final
{
    bool noOp{};
    DispatchInputOwnership inputOwnership{DispatchInputOwnership::CpuCopiedAtCommandRecording};

    [[nodiscard]] bool operator==(DispatchValidation const &) const noexcept = default;
};

[[nodiscard]] std::expected<DispatchValidation, ContractError> ValidateDispatchRequest(
    DispatchRequest const &request) noexcept;

struct StableRecordIdentity final
{
    std::uint64_t inputStableId{};
    std::uint32_t expansionOrdinal{};

    [[nodiscard]] bool operator==(StableRecordIdentity const &) const noexcept = default;
};

struct ReferenceInputRecord final
{
    std::uint64_t stableId{};
    std::uint32_t bucketSeed{};
    std::int64_t contribution{};
    std::uint32_t expansionCount{1U};
};

struct ReferenceExpansionLimits final
{
    std::uint64_t maximumInputRecords{};
    std::uint32_t bucketCount{};
    std::uint32_t maximumBuckets{};
    std::uint64_t maximumClassifiedRecords{};
    std::uint64_t maximumOutputRecords{};
};

struct ClassifiedRecord final
{
    StableRecordIdentity identity{};
    std::uint32_t bucketIndex{};
    std::int64_t contribution{};

    [[nodiscard]] bool operator==(ClassifiedRecord const &) const noexcept = default;
};

struct BucketReduction final
{
    std::uint32_t bucketIndex{};
    std::uint64_t recordCount{};
    std::int64_t contributionSum{};
    std::optional<StableRecordIdentity> firstIdentity{};
    std::optional<StableRecordIdentity> lastIdentity{};
    std::uint64_t checksum{};

    [[nodiscard]] bool operator==(BucketReduction const &) const noexcept = default;
};

struct FinalizedRecord final
{
    std::uint32_t bucketIndex{};
    std::uint64_t recordCount{};
    std::int64_t contributionSum{};
    StableRecordIdentity firstIdentity{};
    StableRecordIdentity lastIdentity{};
    std::uint64_t checksum{};

    [[nodiscard]] bool operator==(FinalizedRecord const &) const noexcept = default;
};

struct ReferenceExpansionCounters final
{
    std::uint64_t inputRecordCount{};
    std::uint64_t classifiedRecordCount{};
    std::uint64_t nonEmptyBucketCount{};
    std::uint64_t finalizedRecordCount{};

    [[nodiscard]] bool operator==(ReferenceExpansionCounters const &) const noexcept = default;
};

struct ReferenceExpansion final
{
    std::vector<ClassifiedRecord> classifiedRecords{};
    std::vector<BucketReduction> bucketReductions{};
    std::vector<FinalizedRecord> finalizedRecords{};
    ReferenceExpansionCounters counters{};
    std::vector<std::byte> finalizedBytes{};
    std::uint64_t checksum{};

    [[nodiscard]] bool operator==(ReferenceExpansion const &) const noexcept = default;
};

[[nodiscard]] std::expected<ReferenceExpansion, ContractError> BuildReferenceExpansion(
    ReferenceExpansionLimits const &limits, std::span<ReferenceInputRecord const> inputs);

} // namespace ch20::work_graphs
