#include "PassSchedule.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ch10::pass_scheduling
{
namespace
{

struct CandidatePlacement final
{
    QueueKind queue{QueueKind::Direct};
    std::uint32_t position{};
};

struct IndexedGraph final
{
    std::vector<ch08::frame_graph::CompiledPass const *> passes{};
    std::vector<ch08::frame_graph::TextureResource const *> resources{};
};

[[nodiscard]] bool CheckedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t &sum) noexcept
{
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left)
    {
        return false;
    }
    sum = left + right;
    return true;
}

[[nodiscard]] std::string IdText(std::uint32_t value)
{
    return std::to_string(value);
}

void AddInvalidGraphDiagnostics(ch08::frame_graph::CompiledPassGraph const &graph,
                                std::vector<ScheduleDiagnostic> &diagnostics, IndexedGraph &indexedGraph)
{
    indexedGraph.passes.resize(graph.scheduledPasses.size(), nullptr);
    for (ch08::frame_graph::CompiledPass const &pass : graph.scheduledPasses)
    {
        std::size_t const index = pass.id.value;
        if (index >= indexedGraph.passes.size() || indexedGraph.passes[index] != nullptr)
        {
            diagnostics.push_back(
                {ScheduleDiagnosticKind::InvalidGraphPassId,
                 "Compiled graph contains invalid or duplicate pass id " + IdText(pass.id.value) + ".", pass.id});
            continue;
        }
        indexedGraph.passes[index] = &pass;
    }
    for (std::size_t index = 0U; index < indexedGraph.passes.size(); ++index)
    {
        if (indexedGraph.passes[index] == nullptr)
        {
            diagnostics.push_back(
                {ScheduleDiagnosticKind::InvalidGraphPassId,
                 "Compiled graph has no pass for stable pass id " + IdText(static_cast<std::uint32_t>(index)) + ".",
                 ch08::frame_graph::PassId{static_cast<std::uint32_t>(index)}});
        }
    }

    indexedGraph.resources.resize(graph.textureResources.size(), nullptr);
    for (ch08::frame_graph::TextureResource const &resource : graph.textureResources)
    {
        std::size_t const index = resource.id.value;
        if (index >= indexedGraph.resources.size() || indexedGraph.resources[index] != nullptr)
        {
            ScheduleDiagnostic diagnostic{};
            diagnostic.kind = ScheduleDiagnosticKind::InvalidGraphTextureResourceId;
            diagnostic.message =
                "Compiled graph contains invalid or duplicate texture resource id " + IdText(resource.id.value) + ".";
            diagnostic.resourceId = resource.id;
            diagnostics.push_back(std::move(diagnostic));
            continue;
        }
        indexedGraph.resources[index] = &resource;
    }
    for (std::size_t index = 0U; index < indexedGraph.resources.size(); ++index)
    {
        if (indexedGraph.resources[index] == nullptr)
        {
            ScheduleDiagnostic diagnostic{};
            diagnostic.kind = ScheduleDiagnosticKind::InvalidGraphTextureResourceId;
            diagnostic.message = "Compiled graph has no texture for stable texture resource id " +
                                 IdText(static_cast<std::uint32_t>(index)) + ".";
            diagnostic.resourceId = ch08::frame_graph::TextureResourceId{static_cast<std::uint32_t>(index)};
            diagnostics.push_back(std::move(diagnostic));
        }
    }

    for (ch08::frame_graph::CompiledPass const &pass : graph.scheduledPasses)
    {
        for (ch08::frame_graph::CompiledTextureUsage const &usage : pass.textureUsages)
        {
            if (usage.resourceId.value >= indexedGraph.resources.size())
            {
                ScheduleDiagnostic diagnostic{};
                diagnostic.kind = ScheduleDiagnosticKind::InvalidGraphTextureResourceId;
                diagnostic.message = "Pass id " + IdText(pass.id.value) + " uses invalid texture resource id " +
                                     IdText(usage.resourceId.value) + ".";
                diagnostic.passId = pass.id;
                diagnostic.resourceId = usage.resourceId;
                diagnostics.push_back(std::move(diagnostic));
            }
        }
    }

    for (ch08::frame_graph::DependencyEdge const &edge : graph.dependencyEdges)
    {
        if (edge.beforePassId.value >= indexedGraph.passes.size())
        {
            diagnostics.push_back(
                {ScheduleDiagnosticKind::InvalidGraphPassId,
                 "Graph dependency references invalid producer pass id " + IdText(edge.beforePassId.value) + ".",
                 edge.beforePassId, edge.afterPassId});
        }
        if (edge.afterPassId.value >= indexedGraph.passes.size())
        {
            diagnostics.push_back(
                {ScheduleDiagnosticKind::InvalidGraphPassId,
                 "Graph dependency references invalid consumer pass id " + IdText(edge.afterPassId.value) + ".",
                 edge.afterPassId, edge.beforePassId});
        }
        if (edge.textureResourceId.has_value() && edge.textureResourceId->value >= indexedGraph.resources.size())
        {
            ScheduleDiagnostic diagnostic{};
            diagnostic.kind = ScheduleDiagnosticKind::InvalidGraphTextureResourceId;
            diagnostic.message = "Graph dependency references invalid texture resource id " +
                                 IdText(edge.textureResourceId->value) + ".";
            diagnostic.passId = edge.beforePassId;
            diagnostic.relatedPassId = edge.afterPassId;
            diagnostic.resourceId = edge.textureResourceId;
            diagnostics.push_back(std::move(diagnostic));
        }
    }
}

void ValidateMetadata(std::vector<PassSchedulingMetadata> const &passMetadata, IndexedGraph const &graph,
                      std::vector<ScheduleDiagnostic> &diagnostics,
                      std::vector<PassSchedulingMetadata const *> &metadataByPass)
{
    metadataByPass.resize(graph.passes.size(), nullptr);
    for (PassSchedulingMetadata const &metadata : passMetadata)
    {
        std::size_t const index = metadata.passId.value;
        if (index >= graph.passes.size())
        {
            diagnostics.push_back(
                {ScheduleDiagnosticKind::UnknownPassId,
                 "Scheduling metadata references unknown pass id " + IdText(metadata.passId.value) + ".",
                 metadata.passId});
            continue;
        }
        if (metadataByPass[index] != nullptr)
        {
            diagnostics.push_back(
                {ScheduleDiagnosticKind::DuplicatePassMetadata,
                 "Scheduling metadata for pass id " + IdText(metadata.passId.value) + " is provided more than once.",
                 metadata.passId});
            continue;
        }
        metadataByPass[index] = &metadata;
        if (metadata.educationalName.empty())
        {
            diagnostics.push_back({ScheduleDiagnosticKind::EmptyEducationalName,
                                   "Scheduling metadata for pass id " + IdText(metadata.passId.value) +
                                       " must provide an educational name.",
                                   metadata.passId});
        }
        if (metadata.abstractDurationTicks == 0U)
        {
            diagnostics.push_back({ScheduleDiagnosticKind::ZeroDuration,
                                   "Pass '" + metadata.educationalName + "' must have a positive abstract duration.",
                                   metadata.passId});
        }
    }
    for (std::size_t index = 0U; index < metadataByPass.size(); ++index)
    {
        if (metadataByPass[index] == nullptr)
        {
            diagnostics.push_back(
                {ScheduleDiagnosticKind::MissingPassMetadata,
                 "Scheduling metadata is missing for pass id " + IdText(static_cast<std::uint32_t>(index)) + ".",
                 ch08::frame_graph::PassId{static_cast<std::uint32_t>(index)}});
        }
    }
}

void ValidateCandidateQueue(std::vector<ch08::frame_graph::PassId> const &queuePasses, QueueKind queue,
                            std::vector<PassSchedulingMetadata const *> const &metadataByPass,
                            std::vector<std::optional<CandidatePlacement>> &placements,
                            std::vector<ScheduleDiagnostic> &diagnostics)
{
    for (std::size_t position = 0U; position < queuePasses.size(); ++position)
    {
        ch08::frame_graph::PassId const passId = queuePasses[position];
        std::size_t const index = passId.value;
        if (index >= placements.size())
        {
            diagnostics.push_back({ScheduleDiagnosticKind::UnknownPassId,
                                   "Schedule candidate references unknown pass id " + IdText(passId.value) + ".",
                                   passId});
            continue;
        }
        if (placements[index].has_value())
        {
            diagnostics.push_back({ScheduleDiagnosticKind::DuplicateCandidatePass,
                                   "Schedule candidate contains pass id " + IdText(passId.value) + " more than once.",
                                   passId});
            continue;
        }
        placements[index] = CandidatePlacement{queue, static_cast<std::uint32_t>(position)};
        if (queue == QueueKind::Compute && metadataByPass[index] != nullptr &&
            metadataByPass[index]->capability == PassCapability::DirectOnly)
        {
            diagnostics.push_back({ScheduleDiagnosticKind::CapabilityViolation,
                                   "Direct-only pass '" + metadataByPass[index]->educationalName +
                                       "' cannot be placed on the Compute queue.",
                                   passId});
        }
    }
}

void ValidateCandidate(ScheduleCandidate const &candidate,
                       std::vector<PassSchedulingMetadata const *> const &metadataByPass,
                       std::vector<ScheduleDiagnostic> &diagnostics,
                       std::vector<std::optional<CandidatePlacement>> &placements)
{
    placements.resize(metadataByPass.size());
    ValidateCandidateQueue(candidate.directQueuePasses, QueueKind::Direct, metadataByPass, placements, diagnostics);
    ValidateCandidateQueue(candidate.computeQueuePasses, QueueKind::Compute, metadataByPass, placements, diagnostics);
    for (std::size_t index = 0U; index < placements.size(); ++index)
    {
        if (!placements[index].has_value())
        {
            diagnostics.push_back(
                {ScheduleDiagnosticKind::MissingCandidatePass,
                 "Schedule candidate is missing pass id " + IdText(static_cast<std::uint32_t>(index)) + ".",
                 ch08::frame_graph::PassId{static_cast<std::uint32_t>(index)}});
        }
    }
}

void ValidateByteSizes(std::vector<TransientTextureByteSize> const &byteSizes, IndexedGraph const &graph,
                       std::vector<ScheduleDiagnostic> &diagnostics,
                       std::vector<std::optional<std::uint64_t>> &byteSizeByResource)
{
    byteSizeByResource.resize(graph.resources.size());
    for (TransientTextureByteSize const &byteSize : byteSizes)
    {
        std::size_t const index = byteSize.resourceId.value;
        if (index >= graph.resources.size())
        {
            ScheduleDiagnostic diagnostic{};
            diagnostic.kind = ScheduleDiagnosticKind::UnknownTextureResourceId;
            diagnostic.message =
                "Transient byte size references unknown texture resource id " + IdText(byteSize.resourceId.value) + ".";
            diagnostic.resourceId = byteSize.resourceId;
            diagnostics.push_back(std::move(diagnostic));
            continue;
        }
        if (byteSizeByResource[index].has_value())
        {
            ScheduleDiagnostic diagnostic{};
            diagnostic.kind = ScheduleDiagnosticKind::DuplicateTextureByteSize;
            diagnostic.message = "Transient byte size for texture resource id " + IdText(byteSize.resourceId.value) +
                                 " is provided more than once.";
            diagnostic.resourceId = byteSize.resourceId;
            diagnostics.push_back(std::move(diagnostic));
            continue;
        }
        byteSizeByResource[index] = byteSize.byteSize;
        if (byteSize.byteSize == 0U)
        {
            ScheduleDiagnostic diagnostic{};
            diagnostic.kind = ScheduleDiagnosticKind::ZeroTextureByteSize;
            diagnostic.message =
                "Texture resource id " + IdText(byteSize.resourceId.value) + " must have a positive byte size.";
            diagnostic.resourceId = byteSize.resourceId;
            diagnostics.push_back(std::move(diagnostic));
        }
    }
}

void AddQueueEdges(std::vector<ch08::frame_graph::PassId> const &queuePasses, std::vector<std::vector<bool>> &hasEdge)
{
    for (std::size_t index = 1U; index < queuePasses.size(); ++index)
    {
        hasEdge[queuePasses[index - 1U].value][queuePasses[index].value] = true;
    }
}

[[nodiscard]] bool BuildSchedule(std::vector<std::vector<bool>> const &hasEdge,
                                 std::vector<PassSchedulingMetadata const *> const &metadataByPass,
                                 std::vector<std::optional<CandidatePlacement>> const &placements, SchedulePlan &plan,
                                 std::vector<ScheduleDiagnostic> &diagnostics)
{
    std::vector<std::uint32_t> inDegrees(hasEdge.size(), 0U);
    for (std::size_t predecessor = 0U; predecessor < hasEdge.size(); ++predecessor)
    {
        for (std::size_t successor = 0U; successor < hasEdge.size(); ++successor)
        {
            if (hasEdge[predecessor][successor])
            {
                ++inDegrees[successor];
            }
        }
    }

    std::vector<std::uint64_t> endTicks(hasEdge.size(), 0U);
    std::vector<bool> scheduled(hasEdge.size(), false);
    std::vector<ScheduledPass> passesById(hasEdge.size());
    for (std::size_t scheduledCount = 0U; scheduledCount < hasEdge.size(); ++scheduledCount)
    {
        std::optional<std::size_t> next{};
        for (std::size_t index = 0U; index < hasEdge.size(); ++index)
        {
            if (!scheduled[index] && inDegrees[index] == 0U)
            {
                next = index;
                break;
            }
        }
        if (!next.has_value())
        {
            ScheduleDiagnostic diagnostic{};
            diagnostic.kind = ScheduleDiagnosticKind::QueueOrderCycle;
            diagnostic.message = "Graph dependencies combined with queue order contain a cycle.";
            for (std::size_t index = 0U; index < scheduled.size(); ++index)
            {
                if (!scheduled[index])
                {
                    diagnostic.cyclePassIds.push_back(ch08::frame_graph::PassId{static_cast<std::uint32_t>(index)});
                }
            }
            diagnostics.push_back(std::move(diagnostic));
            return false;
        }

        std::uint64_t startTick = 0U;
        for (std::size_t predecessor = 0U; predecessor < hasEdge.size(); ++predecessor)
        {
            if (hasEdge[predecessor][*next])
            {
                startTick = (std::max)(startTick, endTicks[predecessor]);
            }
        }
        std::uint64_t endTick = 0U;
        if (!CheckedAdd(startTick, metadataByPass[*next]->abstractDurationTicks, endTick))
        {
            diagnostics.push_back({ScheduleDiagnosticKind::ArithmeticOverflow,
                                   "Abstract tick arithmetic overflowed while scheduling pass '" +
                                       metadataByPass[*next]->educationalName + "'.",
                                   metadataByPass[*next]->passId});
            return false;
        }

        CandidatePlacement const placement = *placements[*next];
        passesById[*next] = {metadataByPass[*next]->passId,
                             metadataByPass[*next]->educationalName,
                             placement.queue,
                             placement.position,
                             startTick,
                             endTick};
        endTicks[*next] = endTick;
        plan.abstractMakespanTicks = (std::max)(plan.abstractMakespanTicks, endTick);
        scheduled[*next] = true;
        for (std::size_t successor = 0U; successor < hasEdge.size(); ++successor)
        {
            if (hasEdge[*next][successor])
            {
                --inDegrees[successor];
            }
        }
    }

    plan.passes = std::move(passesById);
    return true;
}

void BuildFenceDependencies(ch08::frame_graph::CompiledPassGraph const &graph,
                            std::vector<std::optional<CandidatePlacement>> const &placements, SchedulePlan &plan)
{
    for (ch08::frame_graph::DependencyEdge const &edge : graph.dependencyEdges)
    {
        CandidatePlacement const producer = *placements[edge.beforePassId.value];
        CandidatePlacement const consumer = *placements[edge.afterPassId.value];
        if (producer.queue == consumer.queue)
        {
            continue;
        }

        auto const existing = std::ranges::find_if(plan.crossQueueFenceDependencies,
                                                   [&edge](CrossQueueFenceDependency const &dependency)
                                                   {
                                                       return dependency.producerPassId == edge.beforePassId &&
                                                              dependency.consumerPassId == edge.afterPassId;
                                                   });
        FenceDependencyReason const reason{edge.textureResourceId, edge.kind};
        if (existing != plan.crossQueueFenceDependencies.end())
        {
            existing->reasons.push_back(reason);
            continue;
        }
        plan.crossQueueFenceDependencies.push_back({edge.beforePassId,
                                                    edge.afterPassId,
                                                    producer.queue,
                                                    consumer.queue,
                                                    producer.position,
                                                    consumer.position,
                                                    {reason}});
    }

    std::ranges::sort(plan.crossQueueFenceDependencies,
                      [](CrossQueueFenceDependency const &left, CrossQueueFenceDependency const &right)
                      {
                          if (left.producerPassId.value != right.producerPassId.value)
                          {
                              return left.producerPassId.value < right.producerPassId.value;
                          }
                          return left.consumerPassId.value < right.consumerPassId.value;
                      });
    for (CrossQueueFenceDependency &dependency : plan.crossQueueFenceDependencies)
    {
        std::ranges::sort(dependency.reasons,
                          [](FenceDependencyReason const &left, FenceDependencyReason const &right)
                          {
                              std::uint32_t const leftResource = left.resourceId.has_value()
                                                                     ? left.resourceId->value
                                                                     : (std::numeric_limits<std::uint32_t>::max)();
                              std::uint32_t const rightResource = right.resourceId.has_value()
                                                                      ? right.resourceId->value
                                                                      : (std::numeric_limits<std::uint32_t>::max)();
                              if (leftResource != rightResource)
                              {
                                  return leftResource < rightResource;
                              }
                              return left.dependencyKind < right.dependencyKind;
                          });
    }
}

[[nodiscard]] bool BuildTransientTimeline(ch08::frame_graph::CompiledPassGraph const &graph,
                                          IndexedGraph const &indexedGraph,
                                          std::vector<std::optional<std::uint64_t>> const &byteSizeByResource,
                                          SchedulePlan &plan, std::vector<ScheduleDiagnostic> &diagnostics)
{
    std::vector<std::optional<HalfOpenScheduledTimeInterval>> intervals(indexedGraph.resources.size());
    bool hasCompleteTransientByteSizes = true;
    for (ch08::frame_graph::CompiledPass const &pass : graph.scheduledPasses)
    {
        ScheduledPass const &scheduledPass = plan.passes[pass.id.value];
        for (ch08::frame_graph::CompiledTextureUsage const &usage : pass.textureUsages)
        {
            std::optional<HalfOpenScheduledTimeInterval> &interval = intervals[usage.resourceId.value];
            if (!interval.has_value())
            {
                interval =
                    HalfOpenScheduledTimeInterval{scheduledPass.abstractStartTick, scheduledPass.abstractEndTick};
            }
            else
            {
                interval->abstractStartTick = (std::min)(interval->abstractStartTick, scheduledPass.abstractStartTick);
                interval->abstractEndTickExclusive =
                    (std::max)(interval->abstractEndTickExclusive, scheduledPass.abstractEndTick);
            }
        }
    }

    for (std::size_t resourceIndex = 0U; resourceIndex < indexedGraph.resources.size(); ++resourceIndex)
    {
        ch08::frame_graph::TextureResource const &resource = *indexedGraph.resources[resourceIndex];
        if (resource.imported)
        {
            continue;
        }
        plan.transientTextureIntervals.push_back(
            {resource.id, resource.name, intervals[resourceIndex], byteSizeByResource[resourceIndex]});
        if (!intervals[resourceIndex].has_value())
        {
            continue;
        }
        if (!byteSizeByResource[resourceIndex].has_value())
        {
            hasCompleteTransientByteSizes = false;
            continue;
        }
        plan.knownTransientByteTimeline.push_back({intervals[resourceIndex]->abstractStartTick,
                                                   TransientMemoryEventKind::Start, resource.id,
                                                   *byteSizeByResource[resourceIndex], 0U});
        plan.knownTransientByteTimeline.push_back({intervals[resourceIndex]->abstractEndTickExclusive,
                                                   TransientMemoryEventKind::End, resource.id,
                                                   *byteSizeByResource[resourceIndex], 0U});
    }

    std::ranges::sort(plan.knownTransientByteTimeline,
                      [](TransientMemoryTimelineEvent const &left, TransientMemoryTimelineEvent const &right)
                      {
                          if (left.abstractTick != right.abstractTick)
                          {
                              return left.abstractTick < right.abstractTick;
                          }
                          if (left.kind != right.kind)
                          {
                              return left.kind < right.kind;
                          }
                          return left.resourceId.value < right.resourceId.value;
                      });

    std::uint64_t liveBytes = 0U;
    std::uint64_t knownPeakTransientBytes = 0U;
    for (TransientMemoryTimelineEvent &event : plan.knownTransientByteTimeline)
    {
        if (event.kind == TransientMemoryEventKind::End)
        {
            liveBytes -= event.byteSize;
        }
        else
        {
            std::uint64_t nextLiveBytes = 0U;
            if (!CheckedAdd(liveBytes, event.byteSize, nextLiveBytes))
            {
                ScheduleDiagnostic diagnostic{};
                diagnostic.kind = ScheduleDiagnosticKind::ArithmeticOverflow;
                diagnostic.message = "Peak transient byte arithmetic overflowed at abstract tick " +
                                     std::to_string(event.abstractTick) + ".";
                diagnostic.resourceId = event.resourceId;
                diagnostics.push_back(std::move(diagnostic));
                return false;
            }
            liveBytes = nextLiveBytes;
            knownPeakTransientBytes = (std::max)(knownPeakTransientBytes, liveBytes);
        }
        event.knownLiveBytesAfterEvent = liveBytes;
    }
    if (hasCompleteTransientByteSizes)
    {
        plan.peakTransientBytes = knownPeakTransientBytes;
    }
    return true;
}

} // namespace

ScheduleCompileResult CompileSchedule(ch08::frame_graph::CompiledPassGraph const &graph,
                                      std::vector<PassSchedulingMetadata> const &passMetadata,
                                      ScheduleCandidate const &candidate,
                                      std::vector<TransientTextureByteSize> const &transientTextureByteSizes)
{
    std::vector<ScheduleDiagnostic> diagnostics{};
    IndexedGraph indexedGraph{};
    AddInvalidGraphDiagnostics(graph, diagnostics, indexedGraph);
    if (!diagnostics.empty())
    {
        return std::unexpected(std::move(diagnostics));
    }

    std::vector<PassSchedulingMetadata const *> metadataByPass{};
    ValidateMetadata(passMetadata, indexedGraph, diagnostics, metadataByPass);
    std::vector<std::optional<CandidatePlacement>> placements{};
    ValidateCandidate(candidate, metadataByPass, diagnostics, placements);
    std::vector<std::optional<std::uint64_t>> byteSizeByResource{};
    ValidateByteSizes(transientTextureByteSizes, indexedGraph, diagnostics, byteSizeByResource);
    if (!diagnostics.empty())
    {
        return std::unexpected(std::move(diagnostics));
    }

    std::vector<std::vector<bool>> hasEdge(indexedGraph.passes.size(),
                                           std::vector<bool>(indexedGraph.passes.size(), false));
    for (ch08::frame_graph::DependencyEdge const &edge : graph.dependencyEdges)
    {
        hasEdge[edge.beforePassId.value][edge.afterPassId.value] = true;
    }
    AddQueueEdges(candidate.directQueuePasses, hasEdge);
    AddQueueEdges(candidate.computeQueuePasses, hasEdge);

    SchedulePlan plan{};
    if (!BuildSchedule(hasEdge, metadataByPass, placements, plan, diagnostics))
    {
        return std::unexpected(std::move(diagnostics));
    }
    BuildFenceDependencies(graph, placements, plan);
    if (!BuildTransientTimeline(graph, indexedGraph, byteSizeByResource, plan, diagnostics))
    {
        return std::unexpected(std::move(diagnostics));
    }
    return plan;
}

} // namespace ch10::pass_scheduling
