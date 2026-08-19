#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace ch13::work_distribution
{

enum class ContractError : std::uint8_t
{
    TooManyCandidates = 0U,
    NonBinaryFlag,
    InvalidArrivalOrder,
    DuplicateArrival,
    CapacityExceeded,
    InvalidCandidateIndex,
    InvalidVertexCount,
    InvalidStride,
    MisalignedOffset,
    BufferRangeExceeded,
    ArithmeticOverflow,
};

struct ScanResult final
{
    std::vector<std::uint32_t> exclusiveOffsets{};
    std::uint32_t visibleCount{};

    [[nodiscard]] bool operator==(ScanResult const &) const noexcept = default;
};

struct DistributionStatistics final
{
    std::uint32_t candidateCount{};
    std::uint32_t visibleCount{};
    std::uint32_t emittedCount{};
    std::uint32_t overflowCount{};

    [[nodiscard]] bool operator==(DistributionStatistics const &) const noexcept = default;
};

struct DistributionResult final
{
    std::vector<std::uint32_t> candidateIndices{};
    DistributionStatistics statistics{};

    [[nodiscard]] bool operator==(DistributionResult const &) const noexcept = default;
};

[[nodiscard]] std::expected<ScanResult, ContractError> ExclusiveBinaryScan(
    std::span<std::uint32_t const> visibilityFlags);
[[nodiscard]] std::expected<DistributionResult, ContractError> StableCompactVisible(
    std::span<std::uint32_t const> visibilityFlags, std::uint32_t capacity);
[[nodiscard]] std::expected<DistributionResult, ContractError> SimulateBoundedAtomicAppend(
    std::span<std::uint32_t const> visibilityFlags, std::span<std::uint32_t const> arrivalOrder,
    std::uint32_t capacity);

struct DrawArguments final
{
    std::uint32_t vertexCountPerInstance{};
    std::uint32_t instanceCount{};
    std::uint32_t startVertexLocation{};
    std::uint32_t startInstanceLocation{};

    [[nodiscard]] bool operator==(DrawArguments const &) const noexcept = default;
};

struct IndirectDrawCommand final
{
    std::uint32_t candidateIndex{};
    DrawArguments draw{};

    [[nodiscard]] bool operator==(IndirectDrawCommand const &) const noexcept = default;
};

inline constexpr std::uint32_t kIndirectDrawCommandStride = sizeof(IndirectDrawCommand);
static_assert(kIndirectDrawCommandStride == 20U);

[[nodiscard]] std::expected<std::vector<IndirectDrawCommand>, ContractError> BuildIndirectDrawCommands(
    std::span<std::uint32_t const> candidateIndices, std::uint32_t candidateCount, std::uint32_t capacity,
    std::uint32_t vertexCountPerInstance);

struct IndirectExecutionLayout final
{
    std::uint32_t byteStride{kIndirectDrawCommandStride};
    std::uint64_t argumentBufferOffset{};
    std::uint64_t argumentBufferBytes{};
    std::uint32_t maximumCommandCount{};
    std::optional<std::uint64_t> countBufferOffset{};
    std::uint64_t countBufferBytes{};
};

[[nodiscard]] std::expected<void, ContractError> ValidateIndirectExecutionLayout(
    IndirectExecutionLayout const &layout) noexcept;
[[nodiscard]] constexpr std::uint32_t ResolveExecutionCount(std::uint32_t maximumCommandCount,
                                                            std::optional<std::uint32_t> gpuGeneratedCount) noexcept
{
    return gpuGeneratedCount ? std::min(maximumCommandCount, *gpuGeneratedCount) : maximumCommandCount;
}

} // namespace ch13::work_distribution
