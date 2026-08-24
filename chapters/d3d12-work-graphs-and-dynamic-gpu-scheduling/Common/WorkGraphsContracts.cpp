#include "WorkGraphsContracts.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace ch20::work_graphs
{
namespace
{

inline constexpr std::uint64_t kFnvOffsetBasis = 14'695'981'039'346'656'037ULL;
inline constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

[[nodiscard]] bool IsPowerOfTwo(std::uint64_t const value) noexcept
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

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

[[nodiscard]] std::expected<std::uint64_t, ContractError> AlignUp(std::uint64_t const value,
                                                                  std::uint64_t const alignment) noexcept
{
    std::uint64_t const remainder = value % alignment;
    if (remainder == 0U)
    {
        return value;
    }
    return CheckedAdd(value, alignment - remainder);
}

[[nodiscard]] std::uint64_t MaximumRecordCount(LaunchMode const launchMode) noexcept
{
    if (launchMode == LaunchMode::Thread)
    {
        return kMaximumThreadRecords;
    }
    return kMaximumBroadcastOrCoalescingRecords;
}

[[nodiscard]] std::size_t FindRoot(std::vector<std::size_t> &parents, std::size_t index) noexcept
{
    while (parents[index] != index)
    {
        parents[index] = parents[parents[index]];
        index = parents[index];
    }
    return index;
}

void UnionRoots(std::vector<std::size_t> &parents, std::size_t const left, std::size_t const right) noexcept
{
    std::size_t const leftRoot = FindRoot(parents, left);
    std::size_t const rightRoot = FindRoot(parents, right);
    if (leftRoot != rightRoot)
    {
        parents[rightRoot] = leftRoot;
    }
}

[[nodiscard]] bool IsShaderModelBelowBaseline(ShaderModel const shaderModel) noexcept
{
    return shaderModel.major < kShaderModelBaselineMajor ||
           (shaderModel.major == kShaderModelBaselineMajor && shaderModel.minor < kShaderModelBaselineMinor);
}

[[nodiscard]] bool IsProgramTokenValid(ProgramToken const &program) noexcept
{
    return program.stateObjectLifetime != 0U && program.programIdentifierLifetime != 0U && program.graphIdentity != 0U;
}

[[nodiscard]] bool IsCpuDispatch(DispatchMode const mode) noexcept
{
    return mode == DispatchMode::NodeCpuInput || mode == DispatchMode::MultiNodeCpuInput;
}

[[nodiscard]] bool IsMultiNodeDispatch(DispatchMode const mode) noexcept
{
    return mode == DispatchMode::MultiNodeCpuInput || mode == DispatchMode::MultiNodeGpuInput;
}

void HashByte(std::uint64_t &hash, std::uint8_t const value) noexcept
{
    hash ^= value;
    hash *= kFnvPrime;
}

void HashUnsigned(std::uint64_t &hash, std::uint64_t value, std::uint32_t const byteCount) noexcept
{
    for (std::uint32_t byteIndex = 0U; byteIndex < byteCount; ++byteIndex)
    {
        HashByte(hash, static_cast<std::uint8_t>(value & 0xFFU));
        value >>= 8U;
    }
}

void AppendUnsigned(std::vector<std::byte> &bytes, std::uint64_t value, std::uint32_t const byteCount)
{
    for (std::uint32_t byteIndex = 0U; byteIndex < byteCount; ++byteIndex)
    {
        bytes.push_back(static_cast<std::byte>(value & 0xFFU));
        value >>= 8U;
    }
}

[[nodiscard]] bool WouldSignedAddOverflow(std::int64_t const left, std::int64_t const right) noexcept
{
    if (right > 0)
    {
        return left > std::numeric_limits<std::int64_t>::max() - right;
    }
    if (right < 0)
    {
        return left < std::numeric_limits<std::int64_t>::min() - right;
    }
    return false;
}

} // namespace

std::expected<RecordLayout, ContractError> ValidateRecordLayout(std::span<RecordFieldDescription const> fields)
{
    if (fields.empty())
    {
        return std::unexpected(ContractError::EmptyRecordLayout);
    }

    RecordLayout layout{};
    layout.fields.reserve(fields.size());
    std::set<std::string> fieldNames{};
    std::uint64_t cursor = 0U;
    std::uint64_t largestScalarAlignment = 1U;

    for (RecordFieldDescription const &field : fields)
    {
        if (field.name.empty())
        {
            return std::unexpected(ContractError::EmptyRecordFieldName);
        }
        if (!fieldNames.insert(field.name).second)
        {
            return std::unexpected(ContractError::DuplicateRecordFieldName);
        }
        if (field.scalarSizeBytes == 0U)
        {
            return std::unexpected(ContractError::ZeroScalarSize);
        }
        if (!IsPowerOfTwo(field.scalarAlignmentBytes))
        {
            return std::unexpected(ContractError::InvalidScalarAlignment);
        }
        if (field.scalarSizeBytes < field.scalarAlignmentBytes ||
            (field.scalarSizeBytes % field.scalarAlignmentBytes) != 0U)
        {
            return std::unexpected(ContractError::ScalarSizeMisaligned);
        }
        if (field.elementCount == 0U)
        {
            return std::unexpected(ContractError::ZeroArrayElementCount);
        }

        auto const fieldSize = CheckedMultiply(field.scalarSizeBytes, field.elementCount);
        if (!fieldSize)
        {
            return std::unexpected(fieldSize.error());
        }

        std::uint64_t fieldOffset = 0U;
        if (field.explicitOffsetBytes)
        {
            fieldOffset = *field.explicitOffsetBytes;
            if ((fieldOffset % field.scalarAlignmentBytes) != 0U)
            {
                return std::unexpected(ContractError::RecordFieldOffsetMisaligned);
            }
            if (fieldOffset < cursor)
            {
                return std::unexpected(ContractError::RecordFieldOverlap);
            }
        }
        else
        {
            auto const alignedOffset = AlignUp(cursor, field.scalarAlignmentBytes);
            if (!alignedOffset)
            {
                return std::unexpected(alignedOffset.error());
            }
            fieldOffset = *alignedOffset;
        }

        auto const fieldEnd = CheckedAdd(fieldOffset, *fieldSize);
        if (!fieldEnd)
        {
            return std::unexpected(fieldEnd.error());
        }

        layout.fields.push_back({
            .name = field.name,
            .offsetBytes = fieldOffset,
            .sizeBytes = *fieldSize,
        });
        cursor = *fieldEnd;
        largestScalarAlignment = std::max(largestScalarAlignment, field.scalarAlignmentBytes);
    }

    layout.scalarAlignmentBytes = largestScalarAlignment;
    layout.strideAlignmentBytes = std::max<std::uint64_t>(4U, largestScalarAlignment);
    auto const stride = AlignUp(cursor, layout.strideAlignmentBytes);
    if (!stride)
    {
        return std::unexpected(stride.error());
    }
    layout.strideBytes = *stride;
    return layout;
}

std::expected<void, ContractError> ValidateOutputBudget(LaunchMode const launchMode,
                                                        std::uint64_t const sharedMemoryBytes,
                                                        OutputDeclaration const &output) noexcept
{
    if (sharedMemoryBytes > kMaximumNodeSharedMemoryBytes)
    {
        return std::unexpected(ContractError::NodeSharedMemoryExceeded);
    }
    if (output.maxRecords == 0U)
    {
        return std::unexpected(ContractError::ZeroOutputCapacity);
    }

    std::optional<std::uint64_t> trackedHeaderBytes{};
    std::optional<std::uint64_t> trackedRecordBytes{};
    if (output.trackedReadWriteInput)
    {
        auto const headerBytes = CheckedMultiply(4U, output.maxRecords);
        if (!headerBytes)
        {
            return std::unexpected(headerBytes.error());
        }
        trackedHeaderBytes = *headerBytes;

        auto const recordBytes = CheckedMultiply(8U, output.maxRecords);
        if (!recordBytes)
        {
            return std::unexpected(recordBytes.error());
        }
        trackedRecordBytes = *recordBytes;
    }

    if (output.maxRecords > MaximumRecordCount(launchMode))
    {
        return std::unexpected(ContractError::OutputRecordCountExceeded);
    }
    if (launchMode == LaunchMode::Thread && output.maxOutputSizeBytes > kMaximumThreadOutputBytes)
    {
        return std::unexpected(ContractError::ThreadOutputRecordSizeExceeded);
    }
    if (output.maxOutputSizeBytes > kMaximumNodeProducedInputBytes)
    {
        return std::unexpected(ContractError::OutputRecordSizeExceeded);
    }

    if (output.trackedReadWriteInput)
    {
        if (*trackedHeaderBytes > kMaximumNodeProducedInputBytes ||
            output.maxOutputSizeBytes > kMaximumNodeProducedInputBytes - *trackedHeaderBytes)
        {
            return std::unexpected(ContractError::TrackedRwOutputSizeExceeded);
        }

        auto const outputAndShared = CheckedAdd(output.maxOutputSizeBytes, sharedMemoryBytes);
        if (!outputAndShared)
        {
            return std::unexpected(outputAndShared.error());
        }
        auto const total = CheckedAdd(*outputAndShared, *trackedRecordBytes);
        if (!total)
        {
            return std::unexpected(total.error());
        }
        if (*total > kTrackedRwTotalMemoryBytes)
        {
            return std::unexpected(ContractError::TrackedRwTotalMemoryExceeded);
        }
    }
    return {};
}

std::expected<OutputBudgetValidation, ContractError> ValidateOutputBudgets(LaunchMode const launchMode,
                                                                           std::uint64_t const sharedMemoryBytes,
                                                                           std::span<OutputDeclaration const> outputs)
{
    if (sharedMemoryBytes > kMaximumNodeSharedMemoryBytes)
    {
        return std::unexpected(ContractError::NodeSharedMemoryExceeded);
    }
    if (outputs.size() > kMaximumOutputDeclarations)
    {
        return std::unexpected(ContractError::TooManyOutputDeclarations);
    }

    std::map<std::string, std::size_t> outputIndices{};
    for (std::size_t index = 0U; index < outputs.size(); ++index)
    {
        OutputDeclaration const &output = outputs[index];
        if (output.name.empty())
        {
            return std::unexpected(ContractError::EmptyOutputName);
        }
        if (!outputIndices.emplace(output.name, index).second)
        {
            return std::unexpected(ContractError::DuplicateOutputName);
        }
        if (auto const valid = ValidateOutputBudget(launchMode, sharedMemoryBytes, output); !valid)
        {
            return std::unexpected(valid.error());
        }
    }

    std::vector<std::size_t> parents(outputs.size());
    for (std::size_t index = 0U; index < parents.size(); ++index)
    {
        parents[index] = index;
    }

    for (std::size_t index = 0U; index < outputs.size(); ++index)
    {
        OutputDeclaration const &output = outputs[index];
        if (!output.maxRecordsSharedWith)
        {
            continue;
        }
        if (output.maxRecordsSharedWith->empty())
        {
            return std::unexpected(ContractError::InvalidSharedOutputReference);
        }
        auto const target = outputIndices.find(*output.maxRecordsSharedWith);
        if (target == outputIndices.end() || target->second == index)
        {
            return std::unexpected(ContractError::InvalidSharedOutputReference);
        }
        if (output.maxRecords != outputs[target->second].maxRecords)
        {
            return std::unexpected(ContractError::IncompatibleSharedOutputBudget);
        }
        UnionRoots(parents, index, target->second);
    }

    std::map<std::size_t, SharedOutputBudgetGroup> groups{};
    for (std::size_t index = 0U; index < outputs.size(); ++index)
    {
        std::size_t const root = FindRoot(parents, index);
        groups[root].outputNames.push_back(outputs[index].name);
        groups[root].maxRecords = outputs[index].maxRecords;
    }

    OutputBudgetValidation validation{};
    for (auto &[root, group] : groups)
    {
        static_cast<void>(root);
        if (group.outputNames.size() > 1U)
        {
            validation.sharedGroups.push_back(std::move(group));
        }
    }
    return validation;
}

OutputRecordLedger::OutputRecordLedger(std::uint64_t const capacity) noexcept : capacity_(capacity) {}

std::expected<OutputRecordLedger, ContractError> OutputRecordLedger::Create(std::uint64_t const capacity)
{
    if (capacity == 0U)
    {
        return std::unexpected(ContractError::InvalidLedgerCapacity);
    }
    return OutputRecordLedger{capacity};
}

std::expected<RecordReservation, ContractError> OutputRecordLedger::Acquire(std::uint64_t const count)
{
    if (count == 0U)
    {
        return std::unexpected(ContractError::ZeroRecordAcquisition);
    }
    if (count > capacity_ - outstandingCount_)
    {
        return std::unexpected(ContractError::OutputCapacityExceeded);
    }
    if (nextReservationId_ == std::numeric_limits<std::uint64_t>::max())
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }

    RecordReservation const reservation{
        .id = nextReservationId_,
        .count = count,
    };
    ++nextReservationId_;
    outstanding_.push_back(reservation);
    outstandingCount_ += count;
    return reservation;
}

std::expected<void, ContractError> OutputRecordLedger::Complete(RecordReservation const &reservation)
{
    auto const acquisition =
        std::find_if(outstanding_.begin(), outstanding_.end(),
                     [&reservation](RecordReservation const &outstanding) { return outstanding.id == reservation.id; });
    if (acquisition == outstanding_.end())
    {
        return std::unexpected(ContractError::UnknownRecordAcquisition);
    }
    if (acquisition->count != reservation.count)
    {
        return std::unexpected(ContractError::CompletionCountMismatch);
    }

    outstandingCount_ -= acquisition->count;
    outstanding_.erase(acquisition);
    return {};
}

std::uint64_t OutputRecordLedger::Capacity() const noexcept
{
    return capacity_;
}

std::uint64_t OutputRecordLedger::OutstandingCount() const noexcept
{
    return outstandingCount_;
}

std::size_t OutputRecordLedger::OutstandingAcquisitionCount() const noexcept
{
    return outstanding_.size();
}

std::expected<GraphValidation, ContractError> ValidateGraphDescription(GraphDescription const &description)
{
    if (IsShaderModelBelowBaseline(description.shaderModel))
    {
        return std::unexpected(ContractError::ShaderModelBelowBaseline);
    }
    if (description.reportedTier == WorkGraphsTier::NotSupported)
    {
        return std::unexpected(ContractError::WorkGraphsUnsupported);
    }
    struct NodeIdRange final
    {
        std::uint64_t begin{};
        std::uint64_t end{};
    };
    struct SharedInputContract final
    {
        std::uint64_t sizeBytes{};
        bool trackedReadWrite{};
        std::size_t nodeCount{};
    };

    GraphValidation validation{};
    validation.nodeLaunches.reserve(description.nodes.size());
    std::map<std::string, std::size_t> nodeIndices{};
    std::vector<NodeIdRange> nodeIdRanges{};
    nodeIdRanges.reserve(description.nodes.size());
    std::map<std::string, SharedInputContract> sharedInputs{};
    std::size_t entryPointCount = 0U;

    for (std::size_t index = 0U; index < description.nodes.size(); ++index)
    {
        NodeDescription const &node = description.nodes[index];
        if (node.name.empty())
        {
            return std::unexpected(ContractError::EmptyNodeName);
        }
        if (!nodeIndices.emplace(node.name, index).second)
        {
            return std::unexpected(ContractError::DuplicateNodeName);
        }
        if (node.nodeArraySize == 0U || node.recursionMultiplicity == 0U)
        {
            return std::unexpected(ContractError::InvalidNodeMultiplicity);
        }

        auto const rangeSize = CheckedMultiply(node.nodeArraySize, node.recursionMultiplicity);
        if (!rangeSize)
        {
            return std::unexpected(rangeSize.error());
        }
        auto const rangeEnd = CheckedAdd(node.nodeId, *rangeSize);
        if (!rangeEnd)
        {
            return std::unexpected(rangeEnd.error());
        }
        if (*rangeEnd > kNodeIdSpaceSize)
        {
            return std::unexpected(ContractError::NodeIdSpaceExceeded);
        }
        nodeIdRanges.push_back({node.nodeId, *rangeEnd});
        validation.nodeIdSpaceUsed = std::max(validation.nodeIdSpaceUsed, *rangeEnd);

        NodeLaunchValidation launch{
            .nodeName = node.name,
        };
        if (node.launchMode == LaunchMode::Broadcasting)
        {
            if (!node.maximumDispatchGrid)
            {
                return std::unexpected(ContractError::MissingBroadcastGrid);
            }
            DispatchGrid const grid = *node.maximumDispatchGrid;
            if (grid.x > kMaximumBroadcastGridAxis || grid.y > kMaximumBroadcastGridAxis ||
                grid.z > kMaximumBroadcastGridAxis)
            {
                return std::unexpected(ContractError::DispatchGridAxisExceeded);
            }
            auto const xy = CheckedMultiply(grid.x, grid.y);
            if (!xy)
            {
                return std::unexpected(xy.error());
            }
            auto const dispatchCount = CheckedMultiply(*xy, grid.z);
            if (!dispatchCount)
            {
                return std::unexpected(dispatchCount.error());
            }
            if (*dispatchCount > kMaximumBroadcastDispatchCount)
            {
                return std::unexpected(ContractError::DispatchGridProductExceeded);
            }
            launch.maximumDispatchCount = *dispatchCount;
            launch.explicitZeroWork = *dispatchCount == 0U;
        }
        else if (node.maximumDispatchGrid)
        {
            return std::unexpected(ContractError::UnexpectedDispatchGrid);
        }
        validation.nodeLaunches.push_back(std::move(launch));

        if (node.nodeInput)
        {
            if (node.nodeInput->sizeBytes > kMaximumNodeProducedInputBytes)
            {
                return std::unexpected(ContractError::NodeInputRecordSizeExceeded);
            }
            if (node.nodeInput->sharedInputName)
            {
                if (node.nodeInput->sharedInputName->empty())
                {
                    return std::unexpected(ContractError::InvalidSharedInputName);
                }
                auto [sharedInput, inserted] = sharedInputs.try_emplace(
                    *node.nodeInput->sharedInputName, SharedInputContract{
                                                          .sizeBytes = node.nodeInput->sizeBytes,
                                                          .trackedReadWrite = node.nodeInput->trackedReadWrite,
                                                      });
                if (!inserted && (sharedInput->second.sizeBytes != node.nodeInput->sizeBytes ||
                                  sharedInput->second.trackedReadWrite != node.nodeInput->trackedReadWrite))
                {
                    return std::unexpected(ContractError::IncompatibleSharedInput);
                }
                ++sharedInput->second.nodeCount;
                if (sharedInput->second.nodeCount > kMaximumNodesSharingInput)
                {
                    return std::unexpected(ContractError::SharedInputNodeLimitExceeded);
                }
            }
        }

        if (node.entryPointInputSizeBytes)
        {
            ++entryPointCount;
        }
        if (auto const outputs = ValidateOutputBudgets(node.launchMode, node.sharedMemoryBytes, node.outputs); !outputs)
        {
            return std::unexpected(outputs.error());
        }
    }

    std::ranges::sort(nodeIdRanges, {}, &NodeIdRange::begin);
    for (std::size_t index = 1U; index < nodeIdRanges.size(); ++index)
    {
        if (nodeIdRanges[index].begin < nodeIdRanges[index - 1U].end)
        {
            return std::unexpected(ContractError::OverlappingNodeIdRange);
        }
    }
    if (entryPointCount == 0U)
    {
        return std::unexpected(ContractError::MissingEntrypoint);
    }

    std::vector<std::vector<std::size_t>> adjacency(description.nodes.size());
    std::vector<std::uint64_t> indegree(description.nodes.size(), 0U);
    std::vector<bool> hasSelfRecursion(description.nodes.size(), false);

    for (std::size_t sourceIndex = 0U; sourceIndex < description.nodes.size(); ++sourceIndex)
    {
        NodeDescription const &source = description.nodes[sourceIndex];
        for (OutputDeclaration const &output : source.outputs)
        {
            auto const target = nodeIndices.find(output.targetNodeName);
            if (target == nodeIndices.end())
            {
                return std::unexpected(ContractError::DanglingOutputTarget);
            }
            NodeDescription const &targetNode = description.nodes[target->second];
            if (!targetNode.nodeInput)
            {
                return std::unexpected(ContractError::OutputTargetMissingNodeInput);
            }
            if (output.maxOutputSizeBytes != targetNode.nodeInput->sizeBytes)
            {
                return std::unexpected(ContractError::OutputInputRecordMismatch);
            }
            if (output.trackedReadWriteInput != targetNode.nodeInput->trackedReadWrite)
            {
                return std::unexpected(ContractError::TrackedRwInputMismatch);
            }

            if (target->second == sourceIndex)
            {
                hasSelfRecursion[sourceIndex] = true;
                continue;
            }
            auto &targets = adjacency[sourceIndex];
            if (std::find(targets.begin(), targets.end(), target->second) == targets.end())
            {
                targets.push_back(target->second);
                ++indegree[target->second];
            }
        }
    }

    for (std::size_t index = 0U; index < description.nodes.size(); ++index)
    {
        NodeDescription const &node = description.nodes[index];
        if (hasSelfRecursion[index])
        {
            if (node.maximumSelfRecursionDepth == 0U)
            {
                return std::unexpected(ContractError::IllegalSelfRecursion);
            }
            if (node.maximumSelfRecursionDepth > kMaximumGraphDepth)
            {
                return std::unexpected(ContractError::SelfRecursionDepthExceeded);
            }
        }
        else if (node.maximumSelfRecursionDepth != 0U)
        {
            return std::unexpected(ContractError::IllegalSelfRecursion);
        }
    }

    std::vector<std::size_t> ready{};
    ready.reserve(description.nodes.size());
    std::vector<std::uint64_t> depths(description.nodes.size(), 1U);
    for (std::size_t index = 0U; index < indegree.size(); ++index)
    {
        if (indegree[index] == 0U)
        {
            ready.push_back(index);
        }
    }

    std::size_t processedCount = 0U;
    for (std::size_t readyIndex = 0U; readyIndex < ready.size(); ++readyIndex)
    {
        std::size_t const source = ready[readyIndex];
        ++processedCount;
        for (std::size_t const target : adjacency[source])
        {
            depths[target] = std::max(depths[target], depths[source] + 1U);
            --indegree[target];
            if (indegree[target] == 0U)
            {
                ready.push_back(target);
            }
        }
    }
    if (processedCount != description.nodes.size())
    {
        return std::unexpected(ContractError::MultiNodeCycle);
    }

    std::uint64_t const maximumDepth = depths.empty() ? 0U : *std::ranges::max_element(depths);
    if (maximumDepth > kMaximumGraphDepth)
    {
        return std::unexpected(ContractError::GraphDepthExceeded);
    }
    validation.maximumGraphDepth = static_cast<std::uint32_t>(maximumDepth);
    return validation;
}

std::expected<void, ContractError> ValidateBackingMemoryRequest(BackingMemoryRequirements const &requirements,
                                                                BackingMemoryAllocation const &allocation) noexcept
{
    if (requirements.maximumSizeBytes == 0U)
    {
        if (requirements.minimumSizeBytes != 0U || requirements.sizeGranularityBytes != 0U)
        {
            return std::unexpected(ContractError::InvalidBackingMemoryQuery);
        }
        if (allocation.gpuAddress != 0U || allocation.sizeBytes != 0U)
        {
            return std::unexpected(ContractError::UnexpectedBackingMemory);
        }
        return {};
    }

    if (requirements.minimumSizeBytes > requirements.maximumSizeBytes)
    {
        return std::unexpected(ContractError::InvalidBackingMemoryQuery);
    }
    if (allocation.sizeBytes == 0U)
    {
        if (allocation.gpuAddress != 0U)
        {
            return std::unexpected(ContractError::UnexpectedBackingMemory);
        }
        if (requirements.minimumSizeBytes != 0U)
        {
            return std::unexpected(ContractError::BackingMemorySizeOutOfRange);
        }
        return {};
    }
    if (allocation.gpuAddress == 0U)
    {
        return std::unexpected(ContractError::BackingMemoryAddressRequired);
    }
    if ((allocation.gpuAddress % kBackingMemoryAlignmentBytes) != 0U)
    {
        return std::unexpected(ContractError::BackingMemoryAddressMisaligned);
    }
    if (allocation.sizeBytes < requirements.minimumSizeBytes || allocation.sizeBytes > requirements.maximumSizeBytes)
    {
        return std::unexpected(ContractError::BackingMemorySizeOutOfRange);
    }
    if (requirements.sizeGranularityBytes == 0U)
    {
        if (allocation.sizeBytes != requirements.minimumSizeBytes)
        {
            return std::unexpected(ContractError::BackingMemoryGranularityMismatch);
        }
        return {};
    }
    if (((allocation.sizeBytes - requirements.minimumSizeBytes) % requirements.sizeGranularityBytes) != 0U)
    {
        return std::unexpected(ContractError::BackingMemoryGranularityMismatch);
    }
    return {};
}

WorkGraphBackingState::WorkGraphBackingState(BackingMemoryRequirements const requirements,
                                             BackingMemoryAllocation const allocation,
                                             ProgramToken const program) noexcept
    : requirements_(requirements), allocation_(allocation), program_(program)
{
}

std::expected<WorkGraphBackingState, ContractError> WorkGraphBackingState::Create(
    BackingMemoryRequirements const requirements, BackingMemoryAllocation const allocation, ProgramToken const program)
{
    if (!IsProgramTokenValid(program))
    {
        return std::unexpected(ContractError::InvalidProgramToken);
    }
    if (auto const valid = ValidateBackingMemoryRequest(requirements, allocation); !valid)
    {
        return std::unexpected(valid.error());
    }
    return WorkGraphBackingState{requirements, allocation, program};
}

std::expected<void, ContractError> WorkGraphBackingState::BeginUse(ProgramLifetime const &program,
                                                                   std::uint64_t const queueIdentity,
                                                                   bool const initialize)
{
    if (!IsProgramTokenValid(program.token))
    {
        return std::unexpected(ContractError::InvalidProgramToken);
    }
    if (!program.stateObjectAlive || !program.programIdentifierAlive || program.token != program_)
    {
        return std::unexpected(ContractError::StaleProgramToken);
    }
    if (queueIdentity == 0U)
    {
        return std::unexpected(ContractError::InvalidQueueIdentifier);
    }
    if (activeQueue_)
    {
        return std::unexpected(ContractError::QueueOwnershipConflict);
    }
    if (!initialized_ && !initialize)
    {
        return std::unexpected(ContractError::InitializeRequired);
    }

    initialized_ = initialized_ || initialize;
    activeQueue_ = queueIdentity;
    return {};
}

std::expected<void, ContractError> WorkGraphBackingState::EndUse(std::uint64_t const queueIdentity) noexcept
{
    if (!activeQueue_ || *activeQueue_ != queueIdentity)
    {
        return std::unexpected(ContractError::QueueReleaseMismatch);
    }
    activeQueue_.reset();
    return {};
}

bool WorkGraphBackingState::IsInitialized() const noexcept
{
    return initialized_;
}

std::optional<std::uint64_t> WorkGraphBackingState::ActiveQueue() const noexcept
{
    return activeQueue_;
}

BackingMemoryAllocation WorkGraphBackingState::Allocation() const noexcept
{
    return allocation_;
}

ProgramToken WorkGraphBackingState::Program() const noexcept
{
    return program_;
}

std::expected<DispatchValidation, ContractError> ValidateDispatchRequest(DispatchRequest const &request) noexcept
{
    if (request.commandListType == CommandListType::Bundle)
    {
        return std::unexpected(ContractError::BundleCommandListForbidden);
    }

    bool const noOp = request.recordCount == 0U || request.workCount == 0U;
    bool const cpuDispatch = IsCpuDispatch(request.mode);
    bool const multiNodeDispatch = IsMultiNodeDispatch(request.mode);
    if (!noOp)
    {
        if ((!multiNodeDispatch && request.nodeInputCount != 1U) || (multiNodeDispatch && request.nodeInputCount == 0U))
        {
            return std::unexpected(ContractError::InvalidDispatchNodeCount);
        }
    }

    if (cpuDispatch)
    {
        if (request.gpuInputAddress)
        {
            return std::unexpected(ContractError::UnexpectedGpuInputAddress);
        }
        if (!noOp && !request.cpuInputAvailable)
        {
            return std::unexpected(ContractError::MissingCpuInput);
        }
        return DispatchValidation{
            .noOp = noOp,
            .inputOwnership = DispatchInputOwnership::CpuCopiedAtCommandRecording,
        };
    }

    if (request.cpuInputAvailable)
    {
        return std::unexpected(ContractError::UnexpectedCpuInput);
    }
    if (!noOp && (!request.gpuInputAddress || *request.gpuInputAddress == 0U))
    {
        return std::unexpected(ContractError::MissingGpuInputAddress);
    }
    if (request.gpuInputAddress && *request.gpuInputAddress != 0U && (*request.gpuInputAddress % 8U) != 0U)
    {
        return std::unexpected(ContractError::GpuInputAddressMisaligned);
    }
    if (!noOp && request.gpuInputState != GpuInputState::ShaderResourceReadable)
    {
        return std::unexpected(ContractError::GpuInputStateNotReadable);
    }
    return DispatchValidation{
        .noOp = noOp,
        .inputOwnership = DispatchInputOwnership::GpuReadAtExecution,
    };
}

std::expected<ReferenceExpansion, ContractError> BuildReferenceExpansion(ReferenceExpansionLimits const &limits,
                                                                         std::span<ReferenceInputRecord const> inputs)
{
    if (limits.bucketCount == 0U)
    {
        return std::unexpected(ContractError::InvalidFixtureCapacity);
    }
    if (limits.bucketCount > limits.maximumBuckets)
    {
        return std::unexpected(ContractError::FixtureBucketCapacityExceeded);
    }
    if (inputs.size() > limits.maximumInputRecords)
    {
        return std::unexpected(ContractError::FixtureInputCapacityExceeded);
    }

    std::set<std::uint64_t> stableIds{};
    std::uint64_t classifiedCount = 0U;
    for (ReferenceInputRecord const &input : inputs)
    {
        if (!stableIds.insert(input.stableId).second)
        {
            return std::unexpected(ContractError::DuplicateStableIdentity);
        }
        auto const newCount = CheckedAdd(classifiedCount, input.expansionCount);
        if (!newCount)
        {
            return std::unexpected(newCount.error());
        }
        classifiedCount = *newCount;
        if (classifiedCount > limits.maximumClassifiedRecords)
        {
            return std::unexpected(ContractError::FixtureClassifiedCapacityExceeded);
        }
    }
    if (classifiedCount > std::numeric_limits<std::size_t>::max())
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }

    ReferenceExpansion result{};
    result.classifiedRecords.reserve(static_cast<std::size_t>(classifiedCount));
    result.bucketReductions.resize(limits.bucketCount);
    for (std::uint32_t bucketIndex = 0U; bucketIndex < limits.bucketCount; ++bucketIndex)
    {
        result.bucketReductions[bucketIndex].bucketIndex = bucketIndex;
        result.bucketReductions[bucketIndex].checksum = kFnvOffsetBasis;
    }

    for (ReferenceInputRecord const &input : inputs)
    {
        for (std::uint32_t ordinal = 0U; ordinal < input.expansionCount; ++ordinal)
        {
            std::uint32_t const bucketIndex = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(input.bucketSeed) + ordinal) % limits.bucketCount);
            StableRecordIdentity const identity{
                .inputStableId = input.stableId,
                .expansionOrdinal = ordinal,
            };
            result.classifiedRecords.push_back({
                .identity = identity,
                .bucketIndex = bucketIndex,
                .contribution = input.contribution,
            });

            BucketReduction &bucket = result.bucketReductions[bucketIndex];
            if (WouldSignedAddOverflow(bucket.contributionSum, input.contribution))
            {
                return std::unexpected(ContractError::FixtureSumOverflow);
            }
            auto const newRecordCount = CheckedAdd(bucket.recordCount, 1U);
            if (!newRecordCount)
            {
                return std::unexpected(newRecordCount.error());
            }
            bucket.recordCount = *newRecordCount;
            bucket.contributionSum += input.contribution;
            if (!bucket.firstIdentity)
            {
                bucket.firstIdentity = identity;
            }
            bucket.lastIdentity = identity;
            HashUnsigned(bucket.checksum, identity.inputStableId, 8U);
            HashUnsigned(bucket.checksum, identity.expansionOrdinal, 4U);
            HashUnsigned(bucket.checksum, static_cast<std::uint64_t>(input.contribution), 8U);
        }
    }

    for (BucketReduction const &bucket : result.bucketReductions)
    {
        if (bucket.recordCount == 0U)
        {
            continue;
        }
        if (result.finalizedRecords.size() >= limits.maximumOutputRecords)
        {
            return std::unexpected(ContractError::FixtureOutputCapacityExceeded);
        }

        FinalizedRecord const output{
            .bucketIndex = bucket.bucketIndex,
            .recordCount = bucket.recordCount,
            .contributionSum = bucket.contributionSum,
            .firstIdentity = *bucket.firstIdentity,
            .lastIdentity = *bucket.lastIdentity,
            .checksum = bucket.checksum,
        };
        result.finalizedRecords.push_back(output);

        AppendUnsigned(result.finalizedBytes, output.bucketIndex, 4U);
        AppendUnsigned(result.finalizedBytes, output.recordCount, 8U);
        AppendUnsigned(result.finalizedBytes, static_cast<std::uint64_t>(output.contributionSum), 8U);
        AppendUnsigned(result.finalizedBytes, output.firstIdentity.inputStableId, 8U);
        AppendUnsigned(result.finalizedBytes, output.firstIdentity.expansionOrdinal, 4U);
        AppendUnsigned(result.finalizedBytes, output.lastIdentity.inputStableId, 8U);
        AppendUnsigned(result.finalizedBytes, output.lastIdentity.expansionOrdinal, 4U);
        AppendUnsigned(result.finalizedBytes, output.checksum, 8U);
    }

    result.counters = {
        .inputRecordCount = static_cast<std::uint64_t>(inputs.size()),
        .classifiedRecordCount = classifiedCount,
        .nonEmptyBucketCount = static_cast<std::uint64_t>(result.finalizedRecords.size()),
        .finalizedRecordCount = static_cast<std::uint64_t>(result.finalizedRecords.size()),
    };
    result.checksum = kFnvOffsetBasis;
    for (std::byte const value : result.finalizedBytes)
    {
        HashByte(result.checksum, std::to_integer<std::uint8_t>(value));
    }
    return result;
}

} // namespace ch20::work_graphs
