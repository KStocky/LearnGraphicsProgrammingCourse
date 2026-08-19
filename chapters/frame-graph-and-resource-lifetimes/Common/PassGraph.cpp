#include "PassGraph.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ch08::frame_graph
{
namespace
{

enum class VisitState : std::uint8_t
{
    Unvisited = 0U,
    Visiting,
    Visited,
};

struct ValidatedTextureUsage final
{
    std::size_t passIndex{};
    std::size_t resourceIndex{};
    TextureUsageKind kind{TextureUsageKind::Read};
    lgp::framework::TextureBarrierState state{};
};

[[nodiscard]] bool IsValidPassId(PassId passId, std::size_t passCount) noexcept
{
    return static_cast<std::size_t>(passId.value) < passCount;
}

[[nodiscard]] bool IsValidTextureResourceId(TextureResourceId resourceId, std::size_t resourceCount) noexcept
{
    return static_cast<std::size_t>(resourceId.value) < resourceCount;
}

[[nodiscard]] PassId MakePassId(std::size_t passIndex) noexcept
{
    return PassId{static_cast<std::uint32_t>(passIndex)};
}

[[nodiscard]] TextureResourceId MakeTextureResourceId(std::size_t resourceIndex) noexcept
{
    return TextureResourceId{static_cast<std::uint32_t>(resourceIndex)};
}

[[nodiscard]] std::string Quoted(std::string_view text)
{
    return "'" + std::string{text} + "'";
}

[[nodiscard]] std::string UsageVerb(TextureUsageKind kind)
{
    return kind == TextureUsageKind::Read ? "read" : "write";
}

void AddDependencyEdge(std::vector<DependencyEdge> &dependencyEdges, std::vector<std::vector<bool>> &hasDependency,
                       std::size_t beforePassIndex, std::size_t afterPassIndex,
                       std::optional<std::size_t> textureResourceIndex, DependencyKind kind)
{
    DependencyEdge edge{};
    edge.beforePassId = MakePassId(beforePassIndex);
    edge.afterPassId = MakePassId(afterPassIndex);
    edge.kind = kind;
    if (textureResourceIndex.has_value())
    {
        edge.textureResourceId = MakeTextureResourceId(*textureResourceIndex);
    }

    dependencyEdges.push_back(std::move(edge));
    hasDependency[beforePassIndex][afterPassIndex] = true;
}

[[nodiscard]] bool FindCyclePasses(std::vector<std::vector<bool>> const &hasDependency,
                                   std::vector<bool> const &includedPasses, std::size_t passIndex,
                                   std::vector<VisitState> &visitStates, std::vector<std::size_t> &stack,
                                   std::vector<std::size_t> &cyclePasses)
{
    visitStates[passIndex] = VisitState::Visiting;
    stack.push_back(passIndex);

    for (std::size_t nextPassIndex = 0U; nextPassIndex < hasDependency.size(); ++nextPassIndex)
    {
        if (!includedPasses[nextPassIndex] || !hasDependency[passIndex][nextPassIndex])
        {
            continue;
        }

        if (visitStates[nextPassIndex] == VisitState::Unvisited)
        {
            if (FindCyclePasses(hasDependency, includedPasses, nextPassIndex, visitStates, stack, cyclePasses))
            {
                return true;
            }
            continue;
        }

        if (visitStates[nextPassIndex] == VisitState::Visiting)
        {
            auto const cycleBegin = std::find(stack.begin(), stack.end(), nextPassIndex);
            cyclePasses.assign(cycleBegin, stack.end());
            return true;
        }
    }

    stack.pop_back();
    visitStates[passIndex] = VisitState::Visited;
    return false;
}

} // namespace

lgp::framework::TextureBarrierState TransientTextureInitialState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        D3D12_BARRIER_LAYOUT_UNDEFINED,
    };
}

TextureResourceId PassGraph::AddImportedTexture(std::string_view name, lgp::framework::TextureBarrierState initialState)
{
    TextureResourceId const resourceId{static_cast<std::uint32_t>(textureResources_.size())};
    textureResources_.push_back({resourceId, std::string{name}, true, initialState});
    return resourceId;
}

TextureResourceId PassGraph::AddTransientTexture(std::string_view name)
{
    TextureResourceId const resourceId{static_cast<std::uint32_t>(textureResources_.size())};
    textureResources_.push_back({resourceId, std::string{name}, false, TransientTextureInitialState()});
    return resourceId;
}

PassId PassGraph::AddPass(std::string_view name)
{
    PassId const passId{static_cast<std::uint32_t>(passNames_.size())};
    passNames_.emplace_back(name);
    return passId;
}

void PassGraph::DeclareTextureRead(PassId passId, TextureResourceId resourceId,
                                   lgp::framework::TextureBarrierState state)
{
    textureUsageDeclarations_.push_back({passId, resourceId, TextureUsageKind::Read, state});
}

void PassGraph::DeclareTextureWrite(PassId passId, TextureResourceId resourceId,
                                    lgp::framework::TextureBarrierState state)
{
    textureUsageDeclarations_.push_back({passId, resourceId, TextureUsageKind::Write, state});
}

void PassGraph::AddDependency(PassId beforePassId, PassId afterPassId)
{
    explicitDependencyDeclarations_.push_back({beforePassId, afterPassId});
}

PassGraphCompileResult PassGraph::Compile() const
{
    std::vector<PassGraphDiagnostic> diagnostics{};
    std::vector<std::vector<ValidatedTextureUsage>> usagesByPass(passNames_.size());
    std::vector<std::vector<ValidatedTextureUsage>> usagesByResource(textureResources_.size());
    std::vector<std::vector<std::optional<TextureUsageKind>>> seenUsageKinds(
        passNames_.size(), std::vector<std::optional<TextureUsageKind>>(textureResources_.size()));

    for (TextureUsageDeclaration const &usageDeclaration : textureUsageDeclarations_)
    {
        bool const validPassId = IsValidPassId(usageDeclaration.passId, passNames_.size());
        bool const validTextureResourceId =
            IsValidTextureResourceId(usageDeclaration.resourceId, textureResources_.size());

        if (!validPassId)
        {
            PassGraphDiagnostic diagnostic{};
            diagnostic.kind = PassGraphDiagnosticKind::InvalidPassId;
            diagnostic.message = "Declared texture " + UsageVerb(usageDeclaration.kind) +
                                 " references invalid pass id " + std::to_string(usageDeclaration.passId.value) + ".";
            diagnostic.passId = usageDeclaration.passId;
            if (validTextureResourceId)
            {
                diagnostic.textureResourceId = usageDeclaration.resourceId;
            }
            diagnostics.push_back(std::move(diagnostic));
        }

        if (!validTextureResourceId)
        {
            PassGraphDiagnostic diagnostic{};
            diagnostic.kind = PassGraphDiagnosticKind::InvalidTextureResourceId;
            diagnostic.message = validPassId ? "Pass " + Quoted(passNames_[usageDeclaration.passId.value]) +
                                                   " declared a texture " + UsageVerb(usageDeclaration.kind) +
                                                   " for invalid texture resource id " +
                                                   std::to_string(usageDeclaration.resourceId.value) + "."
                                             : "Declared texture " + UsageVerb(usageDeclaration.kind) +
                                                   " references invalid texture resource id " +
                                                   std::to_string(usageDeclaration.resourceId.value) + ".";
            if (validPassId)
            {
                diagnostic.passId = usageDeclaration.passId;
            }
            diagnostic.textureResourceId = usageDeclaration.resourceId;
            diagnostics.push_back(std::move(diagnostic));
        }

        if (!validPassId || !validTextureResourceId)
        {
            continue;
        }

        std::size_t const passIndex = usageDeclaration.passId.value;
        std::size_t const resourceIndex = usageDeclaration.resourceId.value;
        std::optional<TextureUsageKind> const existingUsageKind = seenUsageKinds[passIndex][resourceIndex];
        if (existingUsageKind.has_value())
        {
            PassGraphDiagnostic diagnostic{};
            diagnostic.kind = *existingUsageKind == usageDeclaration.kind
                                  ? PassGraphDiagnosticKind::DuplicateSamePassTextureUsage
                                  : PassGraphDiagnosticKind::ConflictingSamePassTextureUsage;
            diagnostic.message = *existingUsageKind == usageDeclaration.kind
                                     ? "Pass " + Quoted(passNames_[passIndex]) + " declares texture " +
                                           Quoted(textureResources_[resourceIndex].name) + " more than once as a " +
                                           UsageVerb(usageDeclaration.kind) + "."
                                     : "Pass " + Quoted(passNames_[passIndex]) +
                                           " declares conflicting read/write usages "
                                           "for texture " +
                                           Quoted(textureResources_[resourceIndex].name) + " in the same pass.";
            diagnostic.passId = usageDeclaration.passId;
            diagnostic.textureResourceId = usageDeclaration.resourceId;
            diagnostics.push_back(std::move(diagnostic));
            continue;
        }

        seenUsageKinds[passIndex][resourceIndex] = usageDeclaration.kind;
        ValidatedTextureUsage const validatedUsage{
            passIndex,
            resourceIndex,
            usageDeclaration.kind,
            usageDeclaration.state,
        };
        usagesByPass[passIndex].push_back(validatedUsage);
        usagesByResource[resourceIndex].push_back(validatedUsage);
    }

    for (std::vector<ValidatedTextureUsage> &resourceUsages : usagesByResource)
    {
        std::sort(resourceUsages.begin(), resourceUsages.end(),
                  [](ValidatedTextureUsage const &left, ValidatedTextureUsage const &right)
                  { return left.passIndex < right.passIndex; });
    }

    std::vector<DependencyEdge> dependencyEdges{};
    std::vector<std::vector<bool>> hasDependency(passNames_.size(), std::vector<bool>(passNames_.size(), false));
    for (std::size_t resourceIndex = 0U; resourceIndex < usagesByResource.size(); ++resourceIndex)
    {
        std::optional<std::size_t> lastWriterPassIndex{};
        std::vector<std::size_t> readersSinceLastWrite{};
        bool produced = textureResources_[resourceIndex].imported;

        for (ValidatedTextureUsage const &usage : usagesByResource[resourceIndex])
        {
            if (usage.kind == TextureUsageKind::Read)
            {
                if (!produced)
                {
                    PassGraphDiagnostic diagnostic{};
                    diagnostic.kind = PassGraphDiagnosticKind::ReadBeforeProduce;
                    diagnostic.message = "Pass " + Quoted(passNames_[usage.passIndex]) + " reads transient texture " +
                                         Quoted(textureResources_[resourceIndex].name) + " before any pass writes it.";
                    diagnostic.passId = MakePassId(usage.passIndex);
                    diagnostic.textureResourceId = MakeTextureResourceId(resourceIndex);
                    diagnostics.push_back(std::move(diagnostic));
                }

                if (lastWriterPassIndex.has_value())
                {
                    AddDependencyEdge(dependencyEdges, hasDependency, *lastWriterPassIndex, usage.passIndex,
                                      resourceIndex, DependencyKind::ReadAfterWrite);
                }

                readersSinceLastWrite.push_back(usage.passIndex);
                continue;
            }

            for (std::size_t const readerPassIndex : readersSinceLastWrite)
            {
                AddDependencyEdge(dependencyEdges, hasDependency, readerPassIndex, usage.passIndex, resourceIndex,
                                  DependencyKind::WriteAfterRead);
            }

            if (lastWriterPassIndex.has_value())
            {
                AddDependencyEdge(dependencyEdges, hasDependency, *lastWriterPassIndex, usage.passIndex, resourceIndex,
                                  DependencyKind::WriteAfterWrite);
            }

            lastWriterPassIndex = usage.passIndex;
            readersSinceLastWrite.clear();
            produced = true;
        }
    }

    std::vector<std::vector<bool>> hasExplicitDependency(passNames_.size(),
                                                         std::vector<bool>(passNames_.size(), false));
    for (ExplicitDependencyDeclaration const &explicitDependency : explicitDependencyDeclarations_)
    {
        bool const validBeforePassId = IsValidPassId(explicitDependency.beforePassId, passNames_.size());
        bool const validAfterPassId = IsValidPassId(explicitDependency.afterPassId, passNames_.size());

        if (!validBeforePassId)
        {
            PassGraphDiagnostic diagnostic{};
            diagnostic.kind = PassGraphDiagnosticKind::InvalidPassId;
            diagnostic.message = "Explicit dependency references invalid predecessor pass id " +
                                 std::to_string(explicitDependency.beforePassId.value) + ".";
            diagnostic.passId = explicitDependency.beforePassId;
            if (validAfterPassId)
            {
                diagnostic.relatedPassId = explicitDependency.afterPassId;
            }
            diagnostics.push_back(std::move(diagnostic));
        }

        if (!validAfterPassId)
        {
            PassGraphDiagnostic diagnostic{};
            diagnostic.kind = PassGraphDiagnosticKind::InvalidPassId;
            diagnostic.message = validBeforePassId ? "Explicit dependency from pass " +
                                                         Quoted(passNames_[explicitDependency.beforePassId.value]) +
                                                         " references invalid successor pass id " +
                                                         std::to_string(explicitDependency.afterPassId.value) + "."
                                                   : "Explicit dependency references invalid successor pass id " +
                                                         std::to_string(explicitDependency.afterPassId.value) + ".";
            if (validBeforePassId)
            {
                diagnostic.passId = explicitDependency.beforePassId;
            }
            diagnostic.relatedPassId = explicitDependency.afterPassId;
            diagnostics.push_back(std::move(diagnostic));
        }

        if (!validBeforePassId || !validAfterPassId)
        {
            continue;
        }

        std::size_t const beforePassIndex = explicitDependency.beforePassId.value;
        std::size_t const afterPassIndex = explicitDependency.afterPassId.value;
        if (beforePassIndex == afterPassIndex)
        {
            PassGraphDiagnostic diagnostic{};
            diagnostic.kind = PassGraphDiagnosticKind::SelfDependency;
            diagnostic.message = "Pass " + Quoted(passNames_[beforePassIndex]) + " cannot depend on itself.";
            diagnostic.passId = explicitDependency.beforePassId;
            diagnostic.relatedPassId = explicitDependency.afterPassId;
            diagnostics.push_back(std::move(diagnostic));
            continue;
        }

        if (hasExplicitDependency[beforePassIndex][afterPassIndex])
        {
            PassGraphDiagnostic diagnostic{};
            diagnostic.kind = PassGraphDiagnosticKind::DuplicateExplicitDependency;
            diagnostic.message = "Explicit dependency from pass " + Quoted(passNames_[beforePassIndex]) + " to pass " +
                                 Quoted(passNames_[afterPassIndex]) + " is declared more than once.";
            diagnostic.passId = explicitDependency.beforePassId;
            diagnostic.relatedPassId = explicitDependency.afterPassId;
            diagnostics.push_back(std::move(diagnostic));
            continue;
        }

        hasExplicitDependency[beforePassIndex][afterPassIndex] = true;
        AddDependencyEdge(dependencyEdges, hasDependency, beforePassIndex, afterPassIndex, std::nullopt,
                          DependencyKind::Explicit);
    }

    if (!diagnostics.empty())
    {
        return std::unexpected(std::move(diagnostics));
    }

    std::vector<std::uint32_t> inDegrees(passNames_.size(), 0U);
    for (std::size_t beforePassIndex = 0U; beforePassIndex < hasDependency.size(); ++beforePassIndex)
    {
        for (std::size_t afterPassIndex = 0U; afterPassIndex < hasDependency.size(); ++afterPassIndex)
        {
            if (hasDependency[beforePassIndex][afterPassIndex])
            {
                ++inDegrees[afterPassIndex];
            }
        }
    }

    std::vector<bool> scheduled(passNames_.size(), false);
    std::vector<std::size_t> scheduledPassIndices{};
    scheduledPassIndices.reserve(passNames_.size());
    while (scheduledPassIndices.size() < passNames_.size())
    {
        std::optional<std::size_t> nextPassIndex{};
        for (std::size_t passIndex = 0U; passIndex < passNames_.size(); ++passIndex)
        {
            if (!scheduled[passIndex] && inDegrees[passIndex] == 0U)
            {
                nextPassIndex = passIndex;
                break;
            }
        }

        if (!nextPassIndex.has_value())
        {
            break;
        }

        scheduled[*nextPassIndex] = true;
        scheduledPassIndices.push_back(*nextPassIndex);
        for (std::size_t dependentPassIndex = 0U; dependentPassIndex < passNames_.size(); ++dependentPassIndex)
        {
            if (hasDependency[*nextPassIndex][dependentPassIndex])
            {
                --inDegrees[dependentPassIndex];
            }
        }
    }

    if (scheduledPassIndices.size() != passNames_.size())
    {
        std::vector<bool> unresolvedPasses(passNames_.size(), false);
        for (std::size_t passIndex = 0U; passIndex < passNames_.size(); ++passIndex)
        {
            unresolvedPasses[passIndex] = !scheduled[passIndex];
        }

        std::vector<VisitState> visitStates(passNames_.size(), VisitState::Unvisited);
        std::vector<std::size_t> stack{};
        std::vector<std::size_t> cyclePassIndices{};
        for (std::size_t passIndex = 0U; passIndex < passNames_.size(); ++passIndex)
        {
            if (!unresolvedPasses[passIndex])
            {
                continue;
            }

            if (FindCyclePasses(hasDependency, unresolvedPasses, passIndex, visitStates, stack, cyclePassIndices))
            {
                break;
            }
        }

        if (cyclePassIndices.empty())
        {
            for (std::size_t passIndex = 0U; passIndex < passNames_.size(); ++passIndex)
            {
                if (unresolvedPasses[passIndex])
                {
                    cyclePassIndices.push_back(passIndex);
                }
            }
        }

        PassGraphDiagnostic diagnostic{};
        diagnostic.kind = PassGraphDiagnosticKind::Cycle;
        diagnostic.message = "Dependency cycle detected: ";
        for (std::size_t cycleIndex = 0U; cycleIndex < cyclePassIndices.size(); ++cycleIndex)
        {
            if (cycleIndex > 0U)
            {
                diagnostic.message += " -> ";
            }

            diagnostic.cyclePassIds.push_back(MakePassId(cyclePassIndices[cycleIndex]));
            diagnostic.cyclePassNames.push_back(passNames_[cyclePassIndices[cycleIndex]]);
            diagnostic.message += Quoted(passNames_[cyclePassIndices[cycleIndex]]);
        }
        if (!cyclePassIndices.empty())
        {
            diagnostic.message += " -> " + Quoted(passNames_[cyclePassIndices.front()]);
        }
        diagnostic.message += ".";

        diagnostics.push_back(std::move(diagnostic));
        return std::unexpected(std::move(diagnostics));
    }

    CompiledPassGraph compiledGraph{};
    compiledGraph.textureResources = textureResources_;
    compiledGraph.dependencyEdges = std::move(dependencyEdges);

    std::vector<lgp::framework::TextureBarrierState> currentStates{};
    currentStates.reserve(textureResources_.size());
    for (TextureResource const &textureResource : textureResources_)
    {
        currentStates.push_back(textureResource.initialState);
    }

    compiledGraph.resourceLifetimes.reserve(textureResources_.size());
    for (std::size_t resourceIndex = 0U; resourceIndex < textureResources_.size(); ++resourceIndex)
    {
        compiledGraph.resourceLifetimes.push_back({MakeTextureResourceId(resourceIndex), std::nullopt, std::nullopt});
    }

    compiledGraph.scheduledPasses.reserve(scheduledPassIndices.size());
    for (std::size_t executionIndex = 0U; executionIndex < scheduledPassIndices.size(); ++executionIndex)
    {
        std::size_t const passIndex = scheduledPassIndices[executionIndex];
        CompiledPass compiledPass{};
        compiledPass.id = MakePassId(passIndex);
        compiledPass.name = passNames_[passIndex];
        compiledPass.executionIndex = static_cast<std::uint32_t>(executionIndex);

        TextureBarrierGroup barrierGroup{};
        for (ValidatedTextureUsage const &usage : usagesByPass[passIndex])
        {
            compiledPass.textureUsages.push_back({MakeTextureResourceId(usage.resourceIndex), usage.kind, usage.state});

            lgp::framework::TextureBarrierState const beforeState = currentStates[usage.resourceIndex];
            if (beforeState != usage.state)
            {
                barrierGroup.records.push_back({MakeTextureResourceId(usage.resourceIndex), beforeState, usage.state});
            }
            currentStates[usage.resourceIndex] = usage.state;

            ResourceLifetime &resourceLifetime = compiledGraph.resourceLifetimes[usage.resourceIndex];
            if (!resourceLifetime.firstExecutionIndex.has_value())
            {
                resourceLifetime.firstExecutionIndex = static_cast<std::uint32_t>(executionIndex);
            }
            resourceLifetime.lastExecutionIndex = static_cast<std::uint32_t>(executionIndex);
        }

        if (!barrierGroup.records.empty())
        {
            compiledPass.barrierGroups.push_back(std::move(barrierGroup));
        }

        compiledGraph.scheduledPasses.push_back(std::move(compiledPass));
    }

    return compiledGraph;
}

} // namespace ch08::frame_graph
