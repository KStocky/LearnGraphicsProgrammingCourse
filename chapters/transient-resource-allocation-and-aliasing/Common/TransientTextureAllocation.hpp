#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "PassGraph.hpp"

namespace ch09::transient_allocation
{

struct HeapCompatibilityKey final
{
    std::uint32_t value{};

    [[nodiscard]] constexpr bool operator==(HeapCompatibilityKey const &) const noexcept = default;
};

struct TransientTextureRequest final
{
    ch08::frame_graph::TextureResourceId resourceId{};
    std::string name{};
    std::uint64_t allocationSize{};
    std::uint64_t allocationAlignment{};
    HeapCompatibilityKey heapCompatibility{};
    ch08::frame_graph::ResourceLifetime lifetime{};
};

enum class AllocationDiagnosticKind : std::uint8_t
{
    InvalidTextureResourceId = 0U,
    EmptyTextureName,
    LifetimeResourceIdMismatch,
    UnusedLifetime,
    InvalidLifetimeRange,
    ZeroAllocationSize,
    InvalidAllocationAlignment,
    DuplicateTextureResourceId,
    ArithmeticOverflow,
};

struct AllocationDiagnostic final
{
    AllocationDiagnosticKind kind{};
    std::string message{};
    std::optional<ch08::frame_graph::TextureResourceId> resourceId{};
    std::optional<std::size_t> requestIndex{};
};

struct ByteRange final
{
    std::uint64_t offset{};
    std::uint64_t size{};

    [[nodiscard]] constexpr bool operator==(ByteRange const &) const noexcept = default;
};

struct TexturePlacement final
{
    ch08::frame_graph::TextureResourceId resourceId{};
    std::string name{};
    HeapCompatibilityKey heapCompatibility{};
    std::size_t heapIndex{};
    std::uint64_t offset{};
    std::uint64_t allocationSize{};
    std::uint64_t allocationAlignment{};
    ch08::frame_graph::ResourceLifetime lifetime{};
    std::vector<ByteRange> reusedRanges{};
    std::vector<ch08::frame_graph::TextureResourceId> predecessorResourceIds{};
    std::optional<std::uint32_t> activationPassIndex{};
};

struct HeapAllocation final
{
    HeapCompatibilityKey heapCompatibility{};
    std::uint64_t size{};
    std::uint64_t alignment{};
    std::uint64_t requestedBytes{};
    std::uint64_t savedBytes{};
};

struct AliasActivation final
{
    ch08::frame_graph::TextureResourceId resourceId{};
    std::size_t heapIndex{};
    std::uint32_t passIndex{};
    std::vector<ByteRange> reusedRanges{};
    std::vector<ch08::frame_graph::TextureResourceId> predecessorResourceIds{};
};

enum class SavingsExplanation : std::uint8_t
{
    SavedBytes = 0U,
    NoAliasing,
    AlignmentPaddingConsumedSavings,
};

struct TransientTextureAllocationPlan final
{
    std::vector<TexturePlacement> placements{};
    std::vector<HeapAllocation> heaps{};
    std::vector<AliasActivation> aliasActivations{};
    std::uint64_t totalRequestedBytes{};
    std::uint64_t totalHeapBytes{};
    std::uint64_t savedBytes{};
    SavingsExplanation savingsExplanation{SavingsExplanation::NoAliasing};
};

using TransientTextureAllocationResult =
    std::expected<TransientTextureAllocationPlan, std::vector<AllocationDiagnostic>>;

[[nodiscard]] TransientTextureAllocationResult PlanTransientTextureAllocations(
    std::vector<TransientTextureRequest> const &requests);

} // namespace ch09::transient_allocation
