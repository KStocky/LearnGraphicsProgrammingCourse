#include "TransientTextureAllocation.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <utility>

namespace ch09::transient_allocation
{
namespace
{

using ch08::frame_graph::ResourceLifetime;
using ch08::frame_graph::TextureResourceId;

[[nodiscard]] bool IsPowerOfTwo(std::uint64_t value) noexcept
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

[[nodiscard]] bool TryAdd(std::uint64_t left, std::uint64_t right, std::uint64_t &result) noexcept
{
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left)
    {
        return false;
    }

    result = left + right;
    return true;
}

[[nodiscard]] bool TryAlignUp(std::uint64_t value, std::uint64_t alignment, std::uint64_t &result) noexcept
{
    std::uint64_t const mask = alignment - 1U;
    if (value > (std::numeric_limits<std::uint64_t>::max)() - mask)
    {
        return false;
    }

    result = (value + mask) & ~mask;
    return true;
}

[[nodiscard]] bool LifetimesOverlap(ResourceLifetime const &left, ResourceLifetime const &right) noexcept
{
    return *left.firstExecutionIndex <= *right.lastExecutionIndex &&
           *right.firstExecutionIndex <= *left.lastExecutionIndex;
}

[[nodiscard]] bool RangesOverlap(std::uint64_t leftOffset, std::uint64_t leftSize, std::uint64_t rightOffset,
                                 std::uint64_t rightSize) noexcept
{
    return leftOffset < rightOffset + rightSize && rightOffset < leftOffset + leftSize;
}

[[nodiscard]] ByteRange IntersectRanges(TexturePlacement const &left, TexturePlacement const &right) noexcept
{
    std::uint64_t const begin = (std::max)(left.offset, right.offset);
    std::uint64_t const end = (std::min)(left.offset + left.allocationSize, right.offset + right.allocationSize);
    return {begin, end - begin};
}

void MergeRanges(std::vector<ByteRange> &ranges)
{
    std::ranges::sort(ranges, {}, &ByteRange::offset);
    std::vector<ByteRange> merged{};
    merged.reserve(ranges.size());

    for (ByteRange const &range : ranges)
    {
        if (merged.empty() || merged.back().offset + merged.back().size < range.offset)
        {
            merged.push_back(range);
            continue;
        }

        ByteRange &back = merged.back();
        std::uint64_t const end = (std::max)(back.offset + back.size, range.offset + range.size);
        back.size = end - back.offset;
    }

    ranges = std::move(merged);
}

void DeriveAliasPredecessors(TexturePlacement &placement, std::vector<TexturePlacement> const &placements)
{
    struct PriorIntersection final
    {
        TexturePlacement const *placement{};
        ByteRange range{};
    };

    std::vector<PriorIntersection> intersections{};
    std::vector<std::uint64_t> boundaries{};
    for (TexturePlacement const &candidate : placements)
    {
        if (candidate.heapIndex != placement.heapIndex || candidate.resourceId == placement.resourceId ||
            *candidate.lifetime.lastExecutionIndex >= *placement.lifetime.firstExecutionIndex ||
            !RangesOverlap(candidate.offset, candidate.allocationSize, placement.offset, placement.allocationSize))
        {
            continue;
        }

        ByteRange const intersection = IntersectRanges(candidate, placement);
        intersections.push_back({&candidate, intersection});
        placement.reusedRanges.push_back(intersection);
        boundaries.push_back(intersection.offset);
        boundaries.push_back(intersection.offset + intersection.size);
    }

    if (intersections.empty())
    {
        return;
    }

    MergeRanges(placement.reusedRanges);
    std::ranges::sort(boundaries);
    auto const uniqueEnd = std::ranges::unique(boundaries).begin();
    boundaries.erase(uniqueEnd, boundaries.end());

    for (std::size_t boundaryIndex = 0U; boundaryIndex + 1U < boundaries.size(); ++boundaryIndex)
    {
        std::uint64_t const segmentBegin = boundaries[boundaryIndex];
        std::uint64_t const segmentEnd = boundaries[boundaryIndex + 1U];
        PriorIntersection const *latest = nullptr;

        for (PriorIntersection const &intersection : intersections)
        {
            if (intersection.range.offset <= segmentBegin &&
                intersection.range.offset + intersection.range.size >= segmentEnd &&
                (latest == nullptr || *intersection.placement->lifetime.lastExecutionIndex >
                                          *latest->placement->lifetime.lastExecutionIndex))
            {
                latest = &intersection;
            }
        }

        if (latest != nullptr && std::ranges::find(placement.predecessorResourceIds, latest->placement->resourceId) ==
                                     placement.predecessorResourceIds.end())
        {
            placement.predecessorResourceIds.push_back(latest->placement->resourceId);
        }
    }
}

void AddDiagnostic(std::vector<AllocationDiagnostic> &diagnostics, AllocationDiagnosticKind kind, std::string message,
                   TransientTextureRequest const &request, std::size_t requestIndex)
{
    diagnostics.push_back({kind, std::move(message), request.resourceId, requestIndex});
}

[[nodiscard]] std::vector<AllocationDiagnostic> ValidateRequests(std::vector<TransientTextureRequest> const &requests)
{
    std::vector<AllocationDiagnostic> diagnostics{};
    std::vector<TextureResourceId> seenIds{};
    seenIds.reserve(requests.size());

    for (std::size_t requestIndex = 0U; requestIndex < requests.size(); ++requestIndex)
    {
        TransientTextureRequest const &request = requests[requestIndex];
        if (request.resourceId.value == (std::numeric_limits<std::uint32_t>::max)())
        {
            AddDiagnostic(diagnostics, AllocationDiagnosticKind::InvalidTextureResourceId,
                          "Transient texture request has the invalid sentinel resource ID.", request, requestIndex);
        }
        if (request.name.empty())
        {
            AddDiagnostic(diagnostics, AllocationDiagnosticKind::EmptyTextureName,
                          "Transient texture request must have a non-empty educational/debug name.", request,
                          requestIndex);
        }
        if (request.lifetime.resourceId != request.resourceId)
        {
            AddDiagnostic(diagnostics, AllocationDiagnosticKind::LifetimeResourceIdMismatch,
                          "ResourceLifetime.resourceId must match the transient texture request resource ID.", request,
                          requestIndex);
        }
        if (!request.lifetime.firstExecutionIndex.has_value() || !request.lifetime.lastExecutionIndex.has_value())
        {
            AddDiagnostic(diagnostics, AllocationDiagnosticKind::UnusedLifetime,
                          "Transient allocation requires a used lifetime with both inclusive execution indices.",
                          request, requestIndex);
        }
        else if (*request.lifetime.firstExecutionIndex > *request.lifetime.lastExecutionIndex)
        {
            AddDiagnostic(diagnostics, AllocationDiagnosticKind::InvalidLifetimeRange,
                          "Lifetime firstExecutionIndex must not exceed lastExecutionIndex.", request, requestIndex);
        }
        if (request.allocationSize == 0U)
        {
            AddDiagnostic(diagnostics, AllocationDiagnosticKind::ZeroAllocationSize,
                          "D3D12 allocation size must be nonzero.", request, requestIndex);
        }
        if (!IsPowerOfTwo(request.allocationAlignment))
        {
            AddDiagnostic(diagnostics, AllocationDiagnosticKind::InvalidAllocationAlignment,
                          "D3D12 allocation alignment must be nonzero and a power of two.", request, requestIndex);
        }
        if (std::ranges::find(seenIds, request.resourceId) != seenIds.end())
        {
            AddDiagnostic(diagnostics, AllocationDiagnosticKind::DuplicateTextureResourceId,
                          "Each logical texture resource ID may appear only once.", request, requestIndex);
        }
        else
        {
            seenIds.push_back(request.resourceId);
        }
    }

    return diagnostics;
}

} // namespace

TransientTextureAllocationResult PlanTransientTextureAllocations(std::vector<TransientTextureRequest> const &requests)
{
    std::vector<AllocationDiagnostic> diagnostics = ValidateRequests(requests);
    if (!diagnostics.empty())
    {
        return std::unexpected(std::move(diagnostics));
    }

    std::vector<std::size_t> requestOrder(requests.size());
    std::iota(requestOrder.begin(), requestOrder.end(), 0U);
    std::ranges::stable_sort(requestOrder, {},
                             [&requests](std::size_t index) { return *requests[index].lifetime.firstExecutionIndex; });

    TransientTextureAllocationPlan plan{};
    plan.placements.reserve(requests.size());

    for (std::size_t const requestIndex : requestOrder)
    {
        TransientTextureRequest const &request = requests[requestIndex];
        auto const heapIterator =
            std::ranges::find(plan.heaps, request.heapCompatibility, &HeapAllocation::heapCompatibility);
        std::size_t heapIndex = static_cast<std::size_t>(std::distance(plan.heaps.begin(), heapIterator));
        if (heapIterator == plan.heaps.end())
        {
            heapIndex = plan.heaps.size();
            plan.heaps.push_back({request.heapCompatibility, 0U, 0U, 0U, 0U});
        }

        std::vector<TexturePlacement const *> blockers{};
        for (TexturePlacement const &placement : plan.placements)
        {
            if (placement.heapIndex == heapIndex && LifetimesOverlap(placement.lifetime, request.lifetime))
            {
                blockers.push_back(&placement);
            }
        }
        std::ranges::stable_sort(blockers, {}, [](TexturePlacement const *placement) { return placement->offset; });

        std::uint64_t candidateOffset = 0U;
        if (!TryAlignUp(candidateOffset, request.allocationAlignment, candidateOffset))
        {
            diagnostics.push_back({AllocationDiagnosticKind::ArithmeticOverflow,
                                   "Alignment overflow while finding a placement offset.", request.resourceId,
                                   requestIndex});
            return std::unexpected(std::move(diagnostics));
        }

        for (TexturePlacement const *blocker : blockers)
        {
            std::uint64_t candidateEnd{};
            if (!TryAdd(candidateOffset, request.allocationSize, candidateEnd))
            {
                diagnostics.push_back({AllocationDiagnosticKind::ArithmeticOverflow,
                                       "Allocation range exceeds the 64-bit byte address space.", request.resourceId,
                                       requestIndex});
                return std::unexpected(std::move(diagnostics));
            }

            if (candidateEnd <= blocker->offset)
            {
                break;
            }
            if (RangesOverlap(candidateOffset, request.allocationSize, blocker->offset, blocker->allocationSize))
            {
                std::uint64_t blockerEnd{};
                if (!TryAdd(blocker->offset, blocker->allocationSize, blockerEnd) ||
                    !TryAlignUp(blockerEnd, request.allocationAlignment, candidateOffset))
                {
                    diagnostics.push_back({AllocationDiagnosticKind::ArithmeticOverflow,
                                           "Alignment overflow after an occupied byte range.", request.resourceId,
                                           requestIndex});
                    return std::unexpected(std::move(diagnostics));
                }
            }
        }

        std::uint64_t placementEnd{};
        if (!TryAdd(candidateOffset, request.allocationSize, placementEnd))
        {
            diagnostics.push_back({AllocationDiagnosticKind::ArithmeticOverflow,
                                   "Allocation range exceeds the 64-bit byte address space.", request.resourceId,
                                   requestIndex});
            return std::unexpected(std::move(diagnostics));
        }

        HeapAllocation &heap = plan.heaps[heapIndex];
        heap.size = (std::max)(heap.size, placementEnd);
        heap.alignment = (std::max)(heap.alignment, request.allocationAlignment);
        if (!TryAdd(heap.requestedBytes, request.allocationSize, heap.requestedBytes) ||
            !TryAdd(plan.totalRequestedBytes, request.allocationSize, plan.totalRequestedBytes))
        {
            diagnostics.push_back({AllocationDiagnosticKind::ArithmeticOverflow,
                                   "Requested-byte accounting exceeds the 64-bit range.", request.resourceId,
                                   requestIndex});
            return std::unexpected(std::move(diagnostics));
        }

        plan.placements.push_back({request.resourceId,
                                   request.name,
                                   request.heapCompatibility,
                                   heapIndex,
                                   candidateOffset,
                                   request.allocationSize,
                                   request.allocationAlignment,
                                   request.lifetime,
                                   {},
                                   {},
                                   std::nullopt});
    }

    for (HeapAllocation &heap : plan.heaps)
    {
        if (!TryAlignUp(heap.size, heap.alignment, heap.size) ||
            !TryAdd(plan.totalHeapBytes, heap.size, plan.totalHeapBytes))
        {
            diagnostics.push_back({AllocationDiagnosticKind::ArithmeticOverflow,
                                   "Heap-size alignment or accounting exceeds the 64-bit range.", std::nullopt,
                                   std::nullopt});
            return std::unexpected(std::move(diagnostics));
        }
        heap.savedBytes = heap.requestedBytes > heap.size ? heap.requestedBytes - heap.size : 0U;
    }

    for (TexturePlacement &placement : plan.placements)
    {
        DeriveAliasPredecessors(placement, plan.placements);
        if (!placement.predecessorResourceIds.empty())
        {
            placement.activationPassIndex = placement.lifetime.firstExecutionIndex;
            plan.aliasActivations.push_back({placement.resourceId, placement.heapIndex, *placement.activationPassIndex,
                                             placement.reusedRanges, placement.predecessorResourceIds});
        }
    }

    plan.savedBytes =
        plan.totalRequestedBytes > plan.totalHeapBytes ? plan.totalRequestedBytes - plan.totalHeapBytes : 0U;
    if (plan.savedBytes > 0U)
    {
        plan.savingsExplanation = SavingsExplanation::SavedBytes;
    }
    else if (!plan.aliasActivations.empty())
    {
        plan.savingsExplanation = SavingsExplanation::AlignmentPaddingConsumedSavings;
    }

    return plan;
}

} // namespace ch09::transient_allocation
