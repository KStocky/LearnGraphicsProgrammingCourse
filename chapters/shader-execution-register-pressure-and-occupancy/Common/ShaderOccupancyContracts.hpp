#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace ch18::shader_occupancy
{

enum class ContractError : std::uint8_t
{
    InvalidWaveSize = 0U,
    InvalidMaximumThreadsPerGroup,
    InvalidResidentGroupCapacity,
    InvalidResidentWaveCapacity,
    InvalidVectorRegisterCapacity,
    InvalidVectorRegisterAllocationGranularity,
    InvalidScalarRegisterCapacity,
    InvalidScalarRegisterAllocationGranularity,
    InvalidGroupsharedCapacity,
    InvalidGroupsharedAllocationGranularity,
    InconsistentScalarRegisterModel,
    InvalidThreadGroupSize,
    ThreadGroupExceedsModelLimit,
    ThreadGroupWaveCountExceedsModelLimit,
    ArithmeticOverflow,
    OneGroupCannotReside,
    InvalidLivenessBounds,
    NoLiveIntervals,
    TooManyLiveIntervals,
    DuplicateVariableId,
    InvalidLiveInterval,
    InvalidVectorRegisterWidth,
    InvalidBranchWaveSize,
    InvalidLaneMask,
    OverlappingLaneMasks,
    NoActiveLanes,
    NoIssuedLaneSlots,
    InvalidComparisonEvidence,
    IncomparableOccupancyModels,
};

struct ScalarRegisterModel final
{
    std::uint64_t capacityPerProcessingBlockIn32BitRegisters{};
    std::uint64_t allocationGranularityPerWaveIn32BitRegisters{};

    [[nodiscard]] bool operator==(ScalarRegisterModel const &) const noexcept = default;
};

struct HardwareModel final
{
    std::uint32_t waveSizeInLanes{};
    std::uint32_t maximumThreadsPerGroup{};
    std::uint64_t maximumResidentGroupsPerProcessingBlock{};
    std::uint64_t maximumResidentWavesPerProcessingBlock{};
    std::uint64_t vectorRegisterCapacityPerProcessingBlockInLane32BitValues{};
    std::uint64_t vectorRegisterAllocationGranularityPerThreadInLane32BitValues{};
    std::optional<ScalarRegisterModel> scalarRegisters{};
    std::uint64_t groupsharedCapacityPerProcessingBlockInBytes{};
    std::uint64_t groupsharedAllocationGranularityPerGroupInBytes{};

    [[nodiscard]] bool operator==(HardwareModel const &) const noexcept = default;
};

struct ShaderResourceUsage final
{
    std::uint32_t threadsPerGroup{};
    std::uint64_t vectorRegistersPerThreadInLane32BitValues{};
    std::optional<std::uint64_t> scalarRegistersPerWaveIn32BitRegisters{};
    std::uint64_t groupsharedBytesPerGroup{};

    [[nodiscard]] bool operator==(ShaderResourceUsage const &) const noexcept = default;
};

enum class OccupancyLimiter : std::uint8_t
{
    VectorRegisters = 0U,
    ScalarRegisters,
    GroupsharedMemory,
    ResidentGroupSlots,
    ResidentWaveSlots,
};

struct OccupancyCapacities final
{
    std::optional<std::uint64_t> groupsFromVectorRegisters{};
    std::optional<std::uint64_t> groupsFromScalarRegisters{};
    std::optional<std::uint64_t> groupsFromGroupsharedBytes{};
    std::uint64_t groupsFromResidentGroupSlots{};
    std::uint64_t groupsFromResidentWaveSlots{};

    [[nodiscard]] bool operator==(OccupancyCapacities const &) const noexcept = default;
};

struct Rational final
{
    std::uint64_t numerator{};
    std::uint64_t denominator{};

    [[nodiscard]] bool operator==(Rational const &) const noexcept = default;
};

struct OccupancyResult final
{
    std::uint64_t wavesPerGroup{};
    std::uint64_t requestedVectorRegistersPerThreadInLane32BitValues{};
    std::uint64_t allocatedVectorRegistersPerThreadInLane32BitValues{};
    std::uint64_t allocatedVectorRegisterLane32BitValuesPerWave{};
    std::uint64_t allocatedVectorRegisterLane32BitValuesPerGroup{};
    std::optional<std::uint64_t> requestedScalarRegistersPerWaveIn32BitRegisters{};
    std::optional<std::uint64_t> allocatedScalarRegistersPerWaveIn32BitRegisters{};
    std::optional<std::uint64_t> allocatedScalarRegistersPerGroupIn32BitRegisters{};
    std::uint64_t requestedGroupsharedBytesPerGroup{};
    std::uint64_t allocatedGroupsharedBytesPerGroup{};
    OccupancyCapacities capacities{};
    std::uint64_t residentGroups{};
    std::uint64_t residentWaves{};
    Rational occupancyRatio{};
    std::vector<OccupancyLimiter> limitingResources{};

    [[nodiscard]] bool operator==(OccupancyResult const &) const noexcept = default;
};

[[nodiscard]] std::expected<OccupancyResult, ContractError> ComputeOccupancy(HardwareModel const &hardware,
                                                                             ShaderResourceUsage const &usage);

// These are intentionally abstract teaching configurations, not vendor SKU descriptions.
[[nodiscard]] HardwareModel MakeAbstractNarrowWaveTeachingArchitecture();
[[nodiscard]] HardwareModel MakeAbstractWideWaveTeachingArchitecture();

inline constexpr std::uint32_t kMaximumTeachingProgramPointInclusive = 4095U;
inline constexpr std::uint32_t kMaximumTeachingLiveIntervals = 1024U;
inline constexpr std::uint64_t kMaximumTeachingVectorRegisterWidth = 4096U;

struct TeachingLivenessBounds final
{
    std::uint32_t maximumProgramPointInclusive{};
    std::uint32_t maximumLiveIntervals{};
    std::uint64_t maximumVectorRegisterWidth{};

    [[nodiscard]] bool operator==(TeachingLivenessBounds const &) const noexcept = default;
};

struct LiveValueInterval final
{
    std::uint32_t variableId{};
    std::uint32_t definitionPointInclusive{};
    std::uint32_t lastUsePointInclusive{};
    std::uint64_t vectorRegisterWidth{};

    [[nodiscard]] bool operator==(LiveValueInterval const &) const noexcept = default;
};

// This bounded interval model illustrates simultaneous liveness. It does not predict a compiler's physical allocation.
struct TeachingLivenessResult final
{
    std::uint32_t peakSimultaneouslyLiveValueCount{};
    std::uint64_t peakLiveVectorRegisterUnits{};
    std::vector<std::uint32_t> programPointsAtPeakLiveValueCount{};
    std::vector<std::uint32_t> programPointsAtPeakLiveVectorRegisterUnits{};

    [[nodiscard]] bool operator==(TeachingLivenessResult const &) const noexcept = default;
};

[[nodiscard]] std::expected<TeachingLivenessResult, ContractError> AnalyzeTeachingLiveness(
    TeachingLivenessBounds const &bounds, std::span<LiveValueInterval const> intervals);

inline constexpr std::uint32_t kMaximumBranchWaveSizeInLanes = 64U;

struct BranchExecutionInput final
{
    std::uint32_t waveSizeInLanes{};
    std::uint64_t thenLaneMask{};
    std::uint64_t elseLaneMask{};
    std::uint64_t thenPathInstructionCount{};
    std::uint64_t elsePathInstructionCount{};
    std::uint64_t convergedInstructionCount{};

    [[nodiscard]] bool operator==(BranchExecutionInput const &) const noexcept = default;
};

enum class BranchCoherence : std::uint8_t
{
    CoherentThen = 0U,
    CoherentElse,
    Divergent,
};

// Issued lane slots are an execution-efficiency accounting model, not time.
struct BranchExecutionResult final
{
    std::uint32_t activeLanesInThenPath{};
    std::uint32_t activeLanesInElsePath{};
    std::uint32_t activeLanesAtConvergence{};
    Rational thenActiveLaneFraction{};
    Rational elseActiveLaneFraction{};
    std::uint64_t thenPathInstructionCount{};
    std::uint64_t elsePathInstructionCount{};
    std::uint64_t convergedInstructionCount{};
    std::uint64_t usefulLaneInstructions{};
    std::uint64_t issuedLaneSlots{};
    Rational laneEfficiency{};
    BranchCoherence coherence{BranchCoherence::CoherentThen};

    [[nodiscard]] bool operator==(BranchExecutionResult const &) const noexcept = default;
};

[[nodiscard]] std::expected<BranchExecutionResult, ContractError> AccountBranchExecution(
    BranchExecutionInput const &input);

enum class ResidencyChange : std::uint8_t
{
    MoreResidentWaves = 0U,
    LessResidentWaves,
    SameResidency,
};

enum class OccupancyRatioChange : std::uint8_t
{
    HigherOccupancyRatio = 0U,
    LowerOccupancyRatio,
    SameOccupancyRatio,
};

enum class LimitingResourceChange : std::uint8_t
{
    SameLimitingResources = 0U,
    DifferentLimitingResources,
};

enum class BranchEfficiencyChange : std::uint8_t
{
    HigherLaneEfficiency = 0U,
    LowerLaneEfficiency,
    SameLaneEfficiency,
};

enum class PerformanceConclusion : std::uint8_t
{
    Unresolved = 0U,
};

struct BeforeAfterComparison final
{
    std::uint64_t beforeResidentWaves{};
    std::uint64_t afterResidentWaves{};
    Rational beforeOccupancyRatio{};
    Rational afterOccupancyRatio{};
    ResidencyChange residencyChange{ResidencyChange::SameResidency};
    OccupancyRatioChange occupancyRatioChange{OccupancyRatioChange::SameOccupancyRatio};
    LimitingResourceChange limitingResourceChange{LimitingResourceChange::SameLimitingResources};
    Rational beforeBranchEfficiency{};
    Rational afterBranchEfficiency{};
    BranchEfficiencyChange branchEfficiencyChange{BranchEfficiencyChange::SameLaneEfficiency};
    PerformanceConclusion performanceConclusion{PerformanceConclusion::Unresolved};

    [[nodiscard]] bool operator==(BeforeAfterComparison const &) const noexcept = default;
};

[[nodiscard]] std::expected<BeforeAfterComparison, ContractError> CompareBeforeAfter(
    OccupancyResult const &beforeOccupancy, BranchExecutionResult const &beforeBranch,
    OccupancyResult const &afterOccupancy, BranchExecutionResult const &afterBranch);

} // namespace ch18::shader_occupancy
