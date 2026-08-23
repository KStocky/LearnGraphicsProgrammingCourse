#include "ShaderOccupancyContracts.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace ch18::shader_occupancy
{
namespace
{

[[nodiscard]] std::expected<std::uint64_t, ContractError> CheckedAdd(std::uint64_t const left,
                                                                     std::uint64_t const right) noexcept
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return left + right;
}

[[nodiscard]] std::expected<std::uint64_t, ContractError> CheckedMultiply(std::uint64_t const left,
                                                                          std::uint64_t const right) noexcept
{
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return left * right;
}

[[nodiscard]] std::expected<std::uint64_t, ContractError> RoundUpToMultiple(std::uint64_t const value,
                                                                            std::uint64_t const granularity) noexcept
{
    std::uint64_t const remainder = value % granularity;
    if (remainder == 0U)
    {
        return value;
    }
    return CheckedAdd(value, granularity - remainder);
}

[[nodiscard]] std::uint64_t DivideRoundUp(std::uint64_t const numerator, std::uint64_t const denominator) noexcept
{
    return (numerator / denominator) + ((numerator % denominator) == 0U ? 0U : 1U);
}

[[nodiscard]] std::expected<void, ContractError> ValidateHardwareModel(HardwareModel const &hardware) noexcept
{
    if (hardware.waveSizeInLanes == 0U)
    {
        return std::unexpected(ContractError::InvalidWaveSize);
    }
    if (hardware.maximumThreadsPerGroup == 0U)
    {
        return std::unexpected(ContractError::InvalidMaximumThreadsPerGroup);
    }
    if (hardware.maximumResidentGroupsPerProcessingBlock == 0U)
    {
        return std::unexpected(ContractError::InvalidResidentGroupCapacity);
    }
    if (hardware.maximumResidentWavesPerProcessingBlock == 0U)
    {
        return std::unexpected(ContractError::InvalidResidentWaveCapacity);
    }
    if (hardware.vectorRegisterCapacityPerProcessingBlockInLane32BitValues == 0U)
    {
        return std::unexpected(ContractError::InvalidVectorRegisterCapacity);
    }
    if (hardware.vectorRegisterAllocationGranularityPerThreadInLane32BitValues == 0U)
    {
        return std::unexpected(ContractError::InvalidVectorRegisterAllocationGranularity);
    }
    if (hardware.scalarRegisters)
    {
        if (hardware.scalarRegisters->capacityPerProcessingBlockIn32BitRegisters == 0U)
        {
            return std::unexpected(ContractError::InvalidScalarRegisterCapacity);
        }
        if (hardware.scalarRegisters->allocationGranularityPerWaveIn32BitRegisters == 0U)
        {
            return std::unexpected(ContractError::InvalidScalarRegisterAllocationGranularity);
        }
    }
    if (hardware.groupsharedCapacityPerProcessingBlockInBytes == 0U)
    {
        return std::unexpected(ContractError::InvalidGroupsharedCapacity);
    }
    if (hardware.groupsharedAllocationGranularityPerGroupInBytes == 0U)
    {
        return std::unexpected(ContractError::InvalidGroupsharedAllocationGranularity);
    }
    return {};
}

[[nodiscard]] bool ContainsLimiter(std::vector<OccupancyLimiter> const &resources,
                                   OccupancyLimiter const resource) noexcept
{
    return std::find(resources.begin(), resources.end(), resource) != resources.end();
}

[[nodiscard]] bool SameLimiterSet(std::vector<OccupancyLimiter> const &left,
                                  std::vector<OccupancyLimiter> const &right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    return std::all_of(left.begin(), left.end(),
                       [&right](OccupancyLimiter const resource) { return ContainsLimiter(right, resource); });
}

[[nodiscard]] std::expected<void, ContractError> ValidateRatio(Rational const &ratio) noexcept
{
    if (ratio.denominator == 0U || ratio.numerator > ratio.denominator)
    {
        return std::unexpected(ContractError::InvalidComparisonEvidence);
    }
    return {};
}

[[nodiscard]] int CompareRationals(Rational left, Rational right) noexcept
{
    bool reverse = false;
    while (true)
    {
        std::uint64_t const leftQuotient = left.numerator / left.denominator;
        std::uint64_t const rightQuotient = right.numerator / right.denominator;
        if (leftQuotient != rightQuotient)
        {
            int const comparison = leftQuotient < rightQuotient ? -1 : 1;
            return reverse ? -comparison : comparison;
        }

        std::uint64_t const leftRemainder = left.numerator % left.denominator;
        std::uint64_t const rightRemainder = right.numerator % right.denominator;
        if (leftRemainder == 0U || rightRemainder == 0U)
        {
            if (leftRemainder == 0U && rightRemainder == 0U)
            {
                return 0;
            }
            int const comparison = leftRemainder == 0U ? -1 : 1;
            return reverse ? -comparison : comparison;
        }

        left = {.numerator = left.denominator, .denominator = leftRemainder};
        right = {.numerator = right.denominator, .denominator = rightRemainder};
        reverse = !reverse;
    }
}

} // namespace

std::expected<OccupancyResult, ContractError> ComputeOccupancy(HardwareModel const &hardware,
                                                               ShaderResourceUsage const &usage)
{
    if (auto const validation = ValidateHardwareModel(hardware); !validation)
    {
        return std::unexpected(validation.error());
    }
    if (usage.threadsPerGroup == 0U)
    {
        return std::unexpected(ContractError::InvalidThreadGroupSize);
    }
    if (usage.threadsPerGroup > hardware.maximumThreadsPerGroup)
    {
        return std::unexpected(ContractError::ThreadGroupExceedsModelLimit);
    }
    if (hardware.scalarRegisters.has_value() != usage.scalarRegistersPerWaveIn32BitRegisters.has_value())
    {
        return std::unexpected(ContractError::InconsistentScalarRegisterModel);
    }

    OccupancyResult result{};
    result.wavesPerGroup = DivideRoundUp(usage.threadsPerGroup, hardware.waveSizeInLanes);
    if (result.wavesPerGroup > hardware.maximumResidentWavesPerProcessingBlock)
    {
        return std::unexpected(ContractError::ThreadGroupWaveCountExceedsModelLimit);
    }

    result.requestedVectorRegistersPerThreadInLane32BitValues = usage.vectorRegistersPerThreadInLane32BitValues;
    auto const allocatedVectorPerThread =
        RoundUpToMultiple(usage.vectorRegistersPerThreadInLane32BitValues,
                          hardware.vectorRegisterAllocationGranularityPerThreadInLane32BitValues);
    if (!allocatedVectorPerThread)
    {
        return std::unexpected(allocatedVectorPerThread.error());
    }
    result.allocatedVectorRegistersPerThreadInLane32BitValues = *allocatedVectorPerThread;

    auto const allocatedVectorPerWave = CheckedMultiply(*allocatedVectorPerThread, hardware.waveSizeInLanes);
    if (!allocatedVectorPerWave)
    {
        return std::unexpected(allocatedVectorPerWave.error());
    }
    result.allocatedVectorRegisterLane32BitValuesPerWave = *allocatedVectorPerWave;

    auto const allocatedVectorPerGroup = CheckedMultiply(*allocatedVectorPerWave, result.wavesPerGroup);
    if (!allocatedVectorPerGroup)
    {
        return std::unexpected(allocatedVectorPerGroup.error());
    }
    result.allocatedVectorRegisterLane32BitValuesPerGroup = *allocatedVectorPerGroup;
    if (*allocatedVectorPerGroup != 0U)
    {
        result.capacities.groupsFromVectorRegisters =
            hardware.vectorRegisterCapacityPerProcessingBlockInLane32BitValues / *allocatedVectorPerGroup;
    }

    result.requestedScalarRegistersPerWaveIn32BitRegisters = usage.scalarRegistersPerWaveIn32BitRegisters;
    if (hardware.scalarRegisters && usage.scalarRegistersPerWaveIn32BitRegisters)
    {
        auto const allocatedScalarPerWave =
            RoundUpToMultiple(*usage.scalarRegistersPerWaveIn32BitRegisters,
                              hardware.scalarRegisters->allocationGranularityPerWaveIn32BitRegisters);
        if (!allocatedScalarPerWave)
        {
            return std::unexpected(allocatedScalarPerWave.error());
        }
        result.allocatedScalarRegistersPerWaveIn32BitRegisters = *allocatedScalarPerWave;

        auto const allocatedScalarPerGroup = CheckedMultiply(*allocatedScalarPerWave, result.wavesPerGroup);
        if (!allocatedScalarPerGroup)
        {
            return std::unexpected(allocatedScalarPerGroup.error());
        }
        result.allocatedScalarRegistersPerGroupIn32BitRegisters = *allocatedScalarPerGroup;
        if (*allocatedScalarPerGroup != 0U)
        {
            result.capacities.groupsFromScalarRegisters =
                hardware.scalarRegisters->capacityPerProcessingBlockIn32BitRegisters / *allocatedScalarPerGroup;
        }
    }

    result.requestedGroupsharedBytesPerGroup = usage.groupsharedBytesPerGroup;
    auto const allocatedGroupshared =
        RoundUpToMultiple(usage.groupsharedBytesPerGroup, hardware.groupsharedAllocationGranularityPerGroupInBytes);
    if (!allocatedGroupshared)
    {
        return std::unexpected(allocatedGroupshared.error());
    }
    result.allocatedGroupsharedBytesPerGroup = *allocatedGroupshared;
    if (*allocatedGroupshared != 0U)
    {
        result.capacities.groupsFromGroupsharedBytes =
            hardware.groupsharedCapacityPerProcessingBlockInBytes / *allocatedGroupshared;
    }

    result.capacities.groupsFromResidentGroupSlots = hardware.maximumResidentGroupsPerProcessingBlock;
    result.capacities.groupsFromResidentWaveSlots =
        hardware.maximumResidentWavesPerProcessingBlock / result.wavesPerGroup;

    result.residentGroups =
        std::min(result.capacities.groupsFromResidentGroupSlots, result.capacities.groupsFromResidentWaveSlots);
    if (result.capacities.groupsFromVectorRegisters)
    {
        result.residentGroups = std::min(result.residentGroups, *result.capacities.groupsFromVectorRegisters);
    }
    if (result.capacities.groupsFromScalarRegisters)
    {
        result.residentGroups = std::min(result.residentGroups, *result.capacities.groupsFromScalarRegisters);
    }
    if (result.capacities.groupsFromGroupsharedBytes)
    {
        result.residentGroups = std::min(result.residentGroups, *result.capacities.groupsFromGroupsharedBytes);
    }
    if (result.residentGroups == 0U)
    {
        return std::unexpected(ContractError::OneGroupCannotReside);
    }

    auto const residentWaves = CheckedMultiply(result.residentGroups, result.wavesPerGroup);
    if (!residentWaves)
    {
        return std::unexpected(residentWaves.error());
    }
    result.residentWaves = *residentWaves;
    result.occupancyRatio = {
        .numerator = result.residentWaves,
        .denominator = hardware.maximumResidentWavesPerProcessingBlock,
    };

    if (result.capacities.groupsFromVectorRegisters == result.residentGroups)
    {
        result.limitingResources.push_back(OccupancyLimiter::VectorRegisters);
    }
    if (result.capacities.groupsFromScalarRegisters == result.residentGroups)
    {
        result.limitingResources.push_back(OccupancyLimiter::ScalarRegisters);
    }
    if (result.capacities.groupsFromGroupsharedBytes == result.residentGroups)
    {
        result.limitingResources.push_back(OccupancyLimiter::GroupsharedMemory);
    }
    if (result.capacities.groupsFromResidentGroupSlots == result.residentGroups)
    {
        result.limitingResources.push_back(OccupancyLimiter::ResidentGroupSlots);
    }
    if (result.capacities.groupsFromResidentWaveSlots == result.residentGroups)
    {
        result.limitingResources.push_back(OccupancyLimiter::ResidentWaveSlots);
    }

    return result;
}

HardwareModel MakeAbstractNarrowWaveTeachingArchitecture()
{
    return {
        .waveSizeInLanes = 32U,
        .maximumThreadsPerGroup = 1024U,
        .maximumResidentGroupsPerProcessingBlock = 16U,
        .maximumResidentWavesPerProcessingBlock = 32U,
        .vectorRegisterCapacityPerProcessingBlockInLane32BitValues = 65'536U,
        .vectorRegisterAllocationGranularityPerThreadInLane32BitValues = 8U,
        .scalarRegisters =
            ScalarRegisterModel{
                .capacityPerProcessingBlockIn32BitRegisters = 2'048U,
                .allocationGranularityPerWaveIn32BitRegisters = 16U,
            },
        .groupsharedCapacityPerProcessingBlockInBytes = 65'536U,
        .groupsharedAllocationGranularityPerGroupInBytes = 256U,
    };
}

HardwareModel MakeAbstractWideWaveTeachingArchitecture()
{
    return {
        .waveSizeInLanes = 64U,
        .maximumThreadsPerGroup = 1024U,
        .maximumResidentGroupsPerProcessingBlock = 8U,
        .maximumResidentWavesPerProcessingBlock = 16U,
        .vectorRegisterCapacityPerProcessingBlockInLane32BitValues = 65'536U,
        .vectorRegisterAllocationGranularityPerThreadInLane32BitValues = 4U,
        .scalarRegisters =
            ScalarRegisterModel{
                .capacityPerProcessingBlockIn32BitRegisters = 1'024U,
                .allocationGranularityPerWaveIn32BitRegisters = 8U,
            },
        .groupsharedCapacityPerProcessingBlockInBytes = 131'072U,
        .groupsharedAllocationGranularityPerGroupInBytes = 512U,
    };
}

std::expected<TeachingLivenessResult, ContractError> AnalyzeTeachingLiveness(
    TeachingLivenessBounds const &bounds, std::span<LiveValueInterval const> intervals)
{
    if (bounds.maximumProgramPointInclusive > kMaximumTeachingProgramPointInclusive ||
        bounds.maximumLiveIntervals == 0U || bounds.maximumLiveIntervals > kMaximumTeachingLiveIntervals ||
        bounds.maximumVectorRegisterWidth == 0U ||
        bounds.maximumVectorRegisterWidth > kMaximumTeachingVectorRegisterWidth)
    {
        return std::unexpected(ContractError::InvalidLivenessBounds);
    }
    if (intervals.empty())
    {
        return std::unexpected(ContractError::NoLiveIntervals);
    }
    if (intervals.size() > bounds.maximumLiveIntervals)
    {
        return std::unexpected(ContractError::TooManyLiveIntervals);
    }

    for (std::size_t intervalIndex = 0U; intervalIndex < intervals.size(); ++intervalIndex)
    {
        LiveValueInterval const &interval = intervals[intervalIndex];
        if (interval.definitionPointInclusive > interval.lastUsePointInclusive ||
            interval.lastUsePointInclusive > bounds.maximumProgramPointInclusive)
        {
            return std::unexpected(ContractError::InvalidLiveInterval);
        }
        if (interval.vectorRegisterWidth == 0U || interval.vectorRegisterWidth > bounds.maximumVectorRegisterWidth)
        {
            return std::unexpected(ContractError::InvalidVectorRegisterWidth);
        }
        for (std::size_t previousIndex = 0U; previousIndex < intervalIndex; ++previousIndex)
        {
            if (intervals[previousIndex].variableId == interval.variableId)
            {
                return std::unexpected(ContractError::DuplicateVariableId);
            }
        }
    }

    TeachingLivenessResult result{};
    for (std::uint32_t point = 0U; point <= bounds.maximumProgramPointInclusive; ++point)
    {
        std::uint32_t liveValueCount = 0U;
        std::uint64_t liveRegisterUnits = 0U;
        for (LiveValueInterval const &interval : intervals)
        {
            if (point < interval.definitionPointInclusive || point > interval.lastUsePointInclusive)
            {
                continue;
            }
            ++liveValueCount;
            auto const accumulated = CheckedAdd(liveRegisterUnits, interval.vectorRegisterWidth);
            if (!accumulated)
            {
                return std::unexpected(accumulated.error());
            }
            liveRegisterUnits = *accumulated;
        }

        if (liveValueCount > result.peakSimultaneouslyLiveValueCount)
        {
            result.peakSimultaneouslyLiveValueCount = liveValueCount;
            result.programPointsAtPeakLiveValueCount = {point};
        }
        else if (liveValueCount == result.peakSimultaneouslyLiveValueCount)
        {
            result.programPointsAtPeakLiveValueCount.push_back(point);
        }

        if (liveRegisterUnits > result.peakLiveVectorRegisterUnits)
        {
            result.peakLiveVectorRegisterUnits = liveRegisterUnits;
            result.programPointsAtPeakLiveVectorRegisterUnits = {point};
        }
        else if (liveRegisterUnits == result.peakLiveVectorRegisterUnits)
        {
            result.programPointsAtPeakLiveVectorRegisterUnits.push_back(point);
        }
    }
    return result;
}

std::expected<BranchExecutionResult, ContractError> AccountBranchExecution(BranchExecutionInput const &input)
{
    if (input.waveSizeInLanes == 0U || input.waveSizeInLanes > kMaximumBranchWaveSizeInLanes)
    {
        return std::unexpected(ContractError::InvalidBranchWaveSize);
    }

    std::uint64_t const validLaneMask = input.waveSizeInLanes == 64U
                                            ? std::numeric_limits<std::uint64_t>::max()
                                            : (std::uint64_t{1U} << input.waveSizeInLanes) - 1U;
    if (((input.thenLaneMask | input.elseLaneMask) & ~validLaneMask) != 0U)
    {
        return std::unexpected(ContractError::InvalidLaneMask);
    }
    if ((input.thenLaneMask & input.elseLaneMask) != 0U)
    {
        return std::unexpected(ContractError::OverlappingLaneMasks);
    }

    std::uint64_t const activeLaneMask = input.thenLaneMask | input.elseLaneMask;
    if (activeLaneMask == 0U)
    {
        return std::unexpected(ContractError::NoActiveLanes);
    }

    BranchExecutionResult result{};
    result.activeLanesInThenPath = std::popcount(input.thenLaneMask);
    result.activeLanesInElsePath = std::popcount(input.elseLaneMask);
    result.activeLanesAtConvergence = std::popcount(activeLaneMask);
    result.thenActiveLaneFraction = {
        .numerator = result.activeLanesInThenPath,
        .denominator = input.waveSizeInLanes,
    };
    result.elseActiveLaneFraction = {
        .numerator = result.activeLanesInElsePath,
        .denominator = input.waveSizeInLanes,
    };
    result.thenPathInstructionCount = input.thenPathInstructionCount;
    result.elsePathInstructionCount = input.elsePathInstructionCount;
    result.convergedInstructionCount = input.convergedInstructionCount;

    auto const usefulThen = CheckedMultiply(input.thenPathInstructionCount, result.activeLanesInThenPath);
    auto const usefulElse = CheckedMultiply(input.elsePathInstructionCount, result.activeLanesInElsePath);
    auto const usefulConverged = CheckedMultiply(input.convergedInstructionCount, result.activeLanesAtConvergence);
    if (!usefulThen || !usefulElse || !usefulConverged)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    auto const usefulPaths = CheckedAdd(*usefulThen, *usefulElse);
    if (!usefulPaths)
    {
        return std::unexpected(usefulPaths.error());
    }
    auto const usefulTotal = CheckedAdd(*usefulPaths, *usefulConverged);
    if (!usefulTotal)
    {
        return std::unexpected(usefulTotal.error());
    }
    result.usefulLaneInstructions = *usefulTotal;

    std::uint64_t issuedInstructionCount = input.convergedInstructionCount;
    if (result.activeLanesInThenPath != 0U)
    {
        auto const withThen = CheckedAdd(issuedInstructionCount, input.thenPathInstructionCount);
        if (!withThen)
        {
            return std::unexpected(withThen.error());
        }
        issuedInstructionCount = *withThen;
    }
    if (result.activeLanesInElsePath != 0U)
    {
        auto const withElse = CheckedAdd(issuedInstructionCount, input.elsePathInstructionCount);
        if (!withElse)
        {
            return std::unexpected(withElse.error());
        }
        issuedInstructionCount = *withElse;
    }
    auto const issuedLaneSlots = CheckedMultiply(issuedInstructionCount, input.waveSizeInLanes);
    if (!issuedLaneSlots)
    {
        return std::unexpected(issuedLaneSlots.error());
    }
    if (*issuedLaneSlots == 0U)
    {
        return std::unexpected(ContractError::NoIssuedLaneSlots);
    }
    result.issuedLaneSlots = *issuedLaneSlots;
    result.laneEfficiency = {
        .numerator = result.usefulLaneInstructions,
        .denominator = result.issuedLaneSlots,
    };
    if (result.activeLanesInThenPath != 0U && result.activeLanesInElsePath != 0U)
    {
        result.coherence = BranchCoherence::Divergent;
    }
    else if (result.activeLanesInThenPath != 0U)
    {
        result.coherence = BranchCoherence::CoherentThen;
    }
    else
    {
        result.coherence = BranchCoherence::CoherentElse;
    }
    return result;
}

std::expected<BeforeAfterComparison, ContractError> CompareBeforeAfter(OccupancyResult const &beforeOccupancy,
                                                                       BranchExecutionResult const &beforeBranch,
                                                                       OccupancyResult const &afterOccupancy,
                                                                       BranchExecutionResult const &afterBranch)
{
    if (auto const beforeOccupancyRatio = ValidateRatio(beforeOccupancy.occupancyRatio); !beforeOccupancyRatio)
    {
        return std::unexpected(beforeOccupancyRatio.error());
    }
    if (auto const afterOccupancyRatio = ValidateRatio(afterOccupancy.occupancyRatio); !afterOccupancyRatio)
    {
        return std::unexpected(afterOccupancyRatio.error());
    }
    if (auto const beforeEfficiency = ValidateRatio(beforeBranch.laneEfficiency); !beforeEfficiency)
    {
        return std::unexpected(beforeEfficiency.error());
    }
    if (auto const afterEfficiency = ValidateRatio(afterBranch.laneEfficiency); !afterEfficiency)
    {
        return std::unexpected(afterEfficiency.error());
    }
    if (beforeOccupancy.occupancyRatio.denominator != afterOccupancy.occupancyRatio.denominator)
    {
        return std::unexpected(ContractError::IncomparableOccupancyModels);
    }

    BeforeAfterComparison result{
        .beforeResidentWaves = beforeOccupancy.residentWaves,
        .afterResidentWaves = afterOccupancy.residentWaves,
        .beforeOccupancyRatio = beforeOccupancy.occupancyRatio,
        .afterOccupancyRatio = afterOccupancy.occupancyRatio,
        .beforeBranchEfficiency = beforeBranch.laneEfficiency,
        .afterBranchEfficiency = afterBranch.laneEfficiency,
    };
    if (afterOccupancy.residentWaves > beforeOccupancy.residentWaves)
    {
        result.residencyChange = ResidencyChange::MoreResidentWaves;
    }
    else if (afterOccupancy.residentWaves < beforeOccupancy.residentWaves)
    {
        result.residencyChange = ResidencyChange::LessResidentWaves;
    }

    int const occupancyRatioComparison =
        CompareRationals(afterOccupancy.occupancyRatio, beforeOccupancy.occupancyRatio);
    if (occupancyRatioComparison > 0)
    {
        result.occupancyRatioChange = OccupancyRatioChange::HigherOccupancyRatio;
    }
    else if (occupancyRatioComparison < 0)
    {
        result.occupancyRatioChange = OccupancyRatioChange::LowerOccupancyRatio;
    }

    if (!SameLimiterSet(beforeOccupancy.limitingResources, afterOccupancy.limitingResources))
    {
        result.limitingResourceChange = LimitingResourceChange::DifferentLimitingResources;
    }

    int const branchEfficiencyComparison = CompareRationals(afterBranch.laneEfficiency, beforeBranch.laneEfficiency);
    if (branchEfficiencyComparison > 0)
    {
        result.branchEfficiencyChange = BranchEfficiencyChange::HigherLaneEfficiency;
    }
    else if (branchEfficiencyComparison < 0)
    {
        result.branchEfficiencyChange = BranchEfficiencyChange::LowerLaneEfficiency;
    }
    return result;
}

} // namespace ch18::shader_occupancy
