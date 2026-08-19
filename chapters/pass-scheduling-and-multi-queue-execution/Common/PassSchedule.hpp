#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "PassGraph.hpp"

namespace ch10::pass_scheduling
{

enum class QueueKind : std::uint8_t
{
    Direct = 0U,
    Compute,
};

enum class PassCapability : std::uint8_t
{
    DirectOnly = 0U,
    DirectOrCompute,
};

struct PassSchedulingMetadata final
{
    ch08::frame_graph::PassId passId{};
    std::string educationalName{};
    std::uint64_t abstractDurationTicks{};
    PassCapability capability{PassCapability::DirectOnly};
};

struct ScheduleCandidate final
{
    std::vector<ch08::frame_graph::PassId> directQueuePasses{};
    std::vector<ch08::frame_graph::PassId> computeQueuePasses{};
};

struct TransientTextureByteSize final
{
    ch08::frame_graph::TextureResourceId resourceId{};
    std::uint64_t byteSize{};
};

enum class ScheduleDiagnosticKind : std::uint8_t
{
    InvalidGraphPassId = 0U,
    InvalidGraphTextureResourceId,
    UnknownPassId,
    UnknownTextureResourceId,
    DuplicateCandidatePass,
    MissingCandidatePass,
    MissingPassMetadata,
    DuplicatePassMetadata,
    ZeroDuration,
    CapabilityViolation,
    DuplicateTextureByteSize,
    ZeroTextureByteSize,
    QueueOrderCycle,
    ArithmeticOverflow,
    EmptyEducationalName,
};

struct ScheduleDiagnostic final
{
    ScheduleDiagnosticKind kind{};
    std::string message{};
    std::optional<ch08::frame_graph::PassId> passId{};
    std::optional<ch08::frame_graph::PassId> relatedPassId{};
    std::optional<ch08::frame_graph::TextureResourceId> resourceId{};
    std::vector<ch08::frame_graph::PassId> cyclePassIds{};
};

struct ScheduledPass final
{
    ch08::frame_graph::PassId passId{};
    std::string educationalName{};
    QueueKind queue{QueueKind::Direct};
    std::uint32_t queuePosition{};
    std::uint64_t abstractStartTick{};
    std::uint64_t abstractEndTick{};
};

struct FenceDependencyReason final
{
    std::optional<ch08::frame_graph::TextureResourceId> resourceId{};
    ch08::frame_graph::DependencyKind dependencyKind{ch08::frame_graph::DependencyKind::Explicit};
};

struct CrossQueueFenceDependency final
{
    ch08::frame_graph::PassId producerPassId{};
    ch08::frame_graph::PassId consumerPassId{};
    QueueKind producerQueue{QueueKind::Direct};
    QueueKind consumerQueue{QueueKind::Direct};
    std::uint32_t signalAfterProducerQueuePosition{};
    std::uint32_t waitBeforeConsumerQueuePosition{};
    std::vector<FenceDependencyReason> reasons{};
};

struct HalfOpenScheduledTimeInterval final
{
    std::uint64_t abstractStartTick{};
    std::uint64_t abstractEndTickExclusive{};

    [[nodiscard]] constexpr bool operator==(HalfOpenScheduledTimeInterval const &) const noexcept = default;
};

struct TransientTextureScheduledInterval final
{
    ch08::frame_graph::TextureResourceId resourceId{};
    std::string name{};
    std::optional<HalfOpenScheduledTimeInterval> scheduledTimeInterval{};
    std::optional<std::uint64_t> physicalByteSize{};
};

enum class TransientMemoryEventKind : std::uint8_t
{
    End = 0U,
    Start,
};

struct TransientMemoryTimelineEvent final
{
    std::uint64_t abstractTick{};
    TransientMemoryEventKind kind{TransientMemoryEventKind::End};
    ch08::frame_graph::TextureResourceId resourceId{};
    std::uint64_t byteSize{};
    std::uint64_t knownLiveBytesAfterEvent{};
};

struct SchedulePlan final
{
    std::vector<ScheduledPass> passes{};
    std::vector<CrossQueueFenceDependency> crossQueueFenceDependencies{};
    std::vector<TransientTextureScheduledInterval> transientTextureIntervals{};
    std::vector<TransientMemoryTimelineEvent> knownTransientByteTimeline{};
    std::optional<std::uint64_t> peakTransientBytes{};
    std::uint64_t abstractMakespanTicks{};
};

using ScheduleCompileResult = std::expected<SchedulePlan, std::vector<ScheduleDiagnostic>>;

[[nodiscard]] ScheduleCompileResult CompileSchedule(
    ch08::frame_graph::CompiledPassGraph const &graph, std::vector<PassSchedulingMetadata> const &passMetadata,
    ScheduleCandidate const &candidate, std::vector<TransientTextureByteSize> const &transientTextureByteSizes = {});

} // namespace ch10::pass_scheduling
