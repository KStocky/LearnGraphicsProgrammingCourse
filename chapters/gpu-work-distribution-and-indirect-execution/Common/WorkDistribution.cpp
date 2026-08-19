#include "WorkDistribution.hpp"

#include <algorithm>
#include <limits>

namespace ch13::work_distribution
{
namespace
{

[[nodiscard]] std::expected<std::uint32_t, ContractError> CandidateCount(std::size_t size) noexcept
{
    if (size > std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected(ContractError::TooManyCandidates);
    }
    return static_cast<std::uint32_t>(size);
}

[[nodiscard]] std::expected<void, ContractError> ValidateFlags(std::span<std::uint32_t const> visibilityFlags) noexcept
{
    for (std::uint32_t const flag : visibilityFlags)
    {
        if (flag > 1U)
        {
            return std::unexpected(ContractError::NonBinaryFlag);
        }
    }
    return {};
}

[[nodiscard]] DistributionStatistics MakeStatistics(std::uint32_t candidateCount, std::uint32_t visibleCount,
                                                    std::uint32_t capacity) noexcept
{
    std::uint32_t const emittedCount = std::min(visibleCount, capacity);
    return {
        .candidateCount = candidateCount,
        .visibleCount = visibleCount,
        .emittedCount = emittedCount,
        .overflowCount = visibleCount - emittedCount,
    };
}

[[nodiscard]] bool RangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t bufferBytes) noexcept
{
    return offset <= bufferBytes && size <= bufferBytes - offset;
}

} // namespace

std::expected<ScanResult, ContractError> ExclusiveBinaryScan(std::span<std::uint32_t const> visibilityFlags)
{
    auto const count = CandidateCount(visibilityFlags.size());
    if (!count)
    {
        return std::unexpected(count.error());
    }
    if (auto const valid = ValidateFlags(visibilityFlags); !valid)
    {
        return std::unexpected(valid.error());
    }

    ScanResult result{};
    result.exclusiveOffsets.resize(visibilityFlags.size());
    for (std::size_t index = 0U; index < visibilityFlags.size(); ++index)
    {
        result.exclusiveOffsets[index] = result.visibleCount;
        result.visibleCount += visibilityFlags[index];
    }
    return result;
}

std::expected<DistributionResult, ContractError> StableCompactVisible(std::span<std::uint32_t const> visibilityFlags,
                                                                      std::uint32_t const capacity)
{
    auto const scan = ExclusiveBinaryScan(visibilityFlags);
    if (!scan)
    {
        return std::unexpected(scan.error());
    }

    DistributionResult result{};
    result.statistics =
        MakeStatistics(static_cast<std::uint32_t>(visibilityFlags.size()), scan->visibleCount, capacity);
    result.candidateIndices.resize(result.statistics.emittedCount);
    for (std::size_t index = 0U; index < visibilityFlags.size(); ++index)
    {
        if (visibilityFlags[index] == 0U || scan->exclusiveOffsets[index] >= capacity)
        {
            continue;
        }
        result.candidateIndices[scan->exclusiveOffsets[index]] = static_cast<std::uint32_t>(index);
    }
    return result;
}

std::expected<DistributionResult, ContractError> SimulateBoundedAtomicAppend(
    std::span<std::uint32_t const> visibilityFlags, std::span<std::uint32_t const> arrivalOrder,
    std::uint32_t const capacity)
{
    auto const count = CandidateCount(visibilityFlags.size());
    if (!count)
    {
        return std::unexpected(count.error());
    }
    if (auto const valid = ValidateFlags(visibilityFlags); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (arrivalOrder.size() != visibilityFlags.size())
    {
        return std::unexpected(ContractError::InvalidArrivalOrder);
    }

    std::vector<bool> seen(visibilityFlags.size(), false);
    DistributionResult result{};
    result.candidateIndices.reserve(std::min<std::size_t>(visibilityFlags.size(), capacity));
    for (std::uint32_t const candidateIndex : arrivalOrder)
    {
        if (candidateIndex >= *count)
        {
            return std::unexpected(ContractError::InvalidArrivalOrder);
        }
        if (seen[candidateIndex])
        {
            return std::unexpected(ContractError::DuplicateArrival);
        }
        seen[candidateIndex] = true;
        if (visibilityFlags[candidateIndex] == 0U)
        {
            continue;
        }

        ++result.statistics.visibleCount;
        if (result.candidateIndices.size() < capacity)
        {
            result.candidateIndices.push_back(candidateIndex);
        }
    }

    result.statistics = MakeStatistics(*count, result.statistics.visibleCount, capacity);
    return result;
}

std::expected<std::vector<IndirectDrawCommand>, ContractError> BuildIndirectDrawCommands(
    std::span<std::uint32_t const> candidateIndices, std::uint32_t const candidateCount, std::uint32_t const capacity,
    std::uint32_t const vertexCountPerInstance)
{
    if (candidateIndices.size() > capacity)
    {
        return std::unexpected(ContractError::CapacityExceeded);
    }
    if (vertexCountPerInstance == 0U)
    {
        return std::unexpected(ContractError::InvalidVertexCount);
    }

    std::vector<IndirectDrawCommand> commands{};
    commands.reserve(candidateIndices.size());
    for (std::uint32_t const candidateIndex : candidateIndices)
    {
        if (candidateIndex >= candidateCount)
        {
            return std::unexpected(ContractError::InvalidCandidateIndex);
        }
        commands.push_back({
            .candidateIndex = candidateIndex,
            .draw =
                {
                    .vertexCountPerInstance = vertexCountPerInstance,
                    .instanceCount = 1U,
                    .startVertexLocation = 0U,
                    .startInstanceLocation = 0U,
                },
        });
    }
    return commands;
}

std::expected<void, ContractError> ValidateIndirectExecutionLayout(IndirectExecutionLayout const &layout) noexcept
{
    if ((layout.byteStride % 4U) != 0U || layout.byteStride < kIndirectDrawCommandStride)
    {
        return std::unexpected(ContractError::InvalidStride);
    }
    if ((layout.argumentBufferOffset % 4U) != 0U ||
        (layout.countBufferOffset && ((*layout.countBufferOffset % 4U) != 0U)))
    {
        return std::unexpected(ContractError::MisalignedOffset);
    }
    if (layout.maximumCommandCount != 0U &&
        layout.byteStride >
            (std::numeric_limits<std::uint64_t>::max() - layout.argumentBufferOffset) / layout.maximumCommandCount)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }

    std::uint64_t const argumentBytes = static_cast<std::uint64_t>(layout.byteStride) * layout.maximumCommandCount;
    if (!RangeFits(layout.argumentBufferOffset, argumentBytes, layout.argumentBufferBytes))
    {
        return std::unexpected(ContractError::BufferRangeExceeded);
    }
    if (layout.countBufferOffset &&
        !RangeFits(*layout.countBufferOffset, sizeof(std::uint32_t), layout.countBufferBytes))
    {
        return std::unexpected(ContractError::BufferRangeExceeded);
    }
    return {};
}

} // namespace ch13::work_distribution
