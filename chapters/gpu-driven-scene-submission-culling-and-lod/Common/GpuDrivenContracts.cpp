#include "GpuDrivenContracts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace ch21::gpu_driven
{
namespace
{

[[nodiscard]] bool IsFinite(double const value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool IsFinite(Float3 const &value) noexcept
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

[[nodiscard]] double Dot(Float3 const &lhs, Float3 const &rhs) noexcept
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

[[nodiscard]] std::expected<void, ContractError> ValidateSphere(Sphere const &sphere) noexcept
{
    if (!IsFinite(sphere.center) || !IsFinite(sphere.radius))
    {
        return std::unexpected(ContractError::NonFiniteValue);
    }
    if (sphere.radius < 0.0)
    {
        return std::unexpected(ContractError::NegativeSphereRadius);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateLodPolicy(LodPolicy const &policy) noexcept
{
    if (policy.transitionRadiiPixels.empty())
    {
        return std::unexpected(ContractError::EmptyLodPolicy);
    }
    if (!IsFinite(policy.hysteresisPixels) || policy.hysteresisPixels < 0.0)
    {
        return std::unexpected(ContractError::InvalidLodHysteresis);
    }

    double minimumGap = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0U; index < policy.transitionRadiiPixels.size(); ++index)
    {
        double const threshold = policy.transitionRadiiPixels[index];
        if (!IsFinite(threshold) || threshold <= 0.0)
        {
            return std::unexpected(ContractError::InvalidLodThreshold);
        }
        if (index != 0U)
        {
            double const previous = policy.transitionRadiiPixels[index - 1U];
            if (threshold >= previous)
            {
                return std::unexpected(ContractError::LodThresholdsNotDescending);
            }
            minimumGap = std::min(minimumGap, previous - threshold);
        }
    }

    double const smallestThreshold = policy.transitionRadiiPixels.back();
    if (policy.hysteresisPixels >= smallestThreshold ||
        (policy.transitionRadiiPixels.size() > 1U && policy.hysteresisPixels * 2.0 >= minimumGap))
    {
        return std::unexpected(ContractError::InvalidLodHysteresis);
    }
    return {};
}

[[nodiscard]] std::uint32_t RawLod(double const radiusPixels, LodPolicy const &policy) noexcept
{
    std::uint32_t lod = 0U;
    while (lod < policy.transitionRadiiPixels.size() && radiusPixels < policy.transitionRadiiPixels[lod])
    {
        ++lod;
    }
    return lod;
}

[[nodiscard]] bool IndexRangeFits(DrawTemplate const &drawTemplate, std::uint32_t const indexBufferCount) noexcept
{
    return drawTemplate.startIndex <= indexBufferCount &&
           drawTemplate.indexCount <= indexBufferCount - drawTemplate.startIndex;
}

[[nodiscard]] std::expected<void, ContractError> ValidateDrawTemplates(std::span<DrawTemplate const> drawTemplates,
                                                                       std::uint32_t const indexBufferCount) noexcept
{
    if (drawTemplates.empty())
    {
        return std::unexpected(ContractError::EmptyDrawTemplateSet);
    }
    for (DrawTemplate const &drawTemplate : drawTemplates)
    {
        if (drawTemplate.indexCount == 0U)
        {
            return std::unexpected(ContractError::EmptyDraw);
        }
        if (!IndexRangeFits(drawTemplate, indexBufferCount))
        {
            return std::unexpected(ContractError::IndexRangeExceeded);
        }
    }
    return {};
}

[[nodiscard]] std::expected<std::unordered_map<std::uint32_t, InstanceRecord const *>, ContractError>
BuildInstanceLookup(std::span<InstanceRecord const> instances)
{
    std::unordered_map<std::uint32_t, InstanceRecord const *> lookup{};
    lookup.reserve(instances.size());
    for (InstanceRecord const &instance : instances)
    {
        if (!lookup.emplace(instance.stableId, &instance).second)
        {
            return std::unexpected(ContractError::DuplicateStableIdentity);
        }
    }
    return lookup;
}

[[nodiscard]] bool SameLogicalDraw(IndirectCommand const &lhs, IndirectCommand const &rhs) noexcept
{
    return lhs.lod == rhs.lod && lhs.drawTemplateIndex == rhs.drawTemplateIndex && lhs.draw == rhs.draw;
}

[[nodiscard]] std::expected<std::vector<IndirectCommand const *>, ContractError> SortSubmission(
    std::span<IndirectCommand const> commands)
{
    std::vector<IndirectCommand const *> sorted{};
    sorted.reserve(commands.size());
    for (IndirectCommand const &command : commands)
    {
        sorted.push_back(&command);
    }
    std::ranges::sort(sorted, {}, [](IndirectCommand const *command) { return command->stableId; });
    for (std::size_t index = 1U; index < sorted.size(); ++index)
    {
        if (sorted[index - 1U]->stableId == sorted[index]->stableId)
        {
            return std::unexpected(ContractError::DuplicateSubmissionIdentity);
        }
    }
    return sorted;
}

} // namespace

std::expected<FrustumClassification, ContractError> ClassifySphereAgainstFrustum(std::span<Plane const> planes,
                                                                                 Sphere const &sphere) noexcept
{
    if (planes.size() != 6U)
    {
        return std::unexpected(ContractError::InvalidFrustumPlaneCount);
    }
    if (auto const valid = ValidateSphere(sphere); !valid)
    {
        return std::unexpected(valid.error());
    }

    FrustumClassification classification = FrustumClassification::Inside;
    for (Plane const &plane : planes)
    {
        if (!IsFinite(plane.normal) || !IsFinite(plane.distance))
        {
            return std::unexpected(ContractError::NonFiniteValue);
        }

        double const normalLengthSquared = Dot(plane.normal, plane.normal);
        if (!(normalLengthSquared > 0.0) || !IsFinite(normalLengthSquared))
        {
            return std::unexpected(ContractError::DegenerateFrustumPlane);
        }

        double const scaledRadius = sphere.radius * std::sqrt(normalLengthSquared);
        double const signedDistance = Dot(plane.normal, sphere.center) + plane.distance;
        if (signedDistance < -scaledRadius)
        {
            return FrustumClassification::Outside;
        }
        if (signedDistance < scaledRadius)
        {
            classification = FrustumClassification::Intersecting;
        }
    }
    return classification;
}

std::expected<ProjectedSphere, ContractError> ProjectSphereConservatively(
    double const centerViewDepth, double const sphereRadius, ProjectionParameters const &parameters) noexcept
{
    if (!IsFinite(centerViewDepth) || !IsFinite(sphereRadius) || !IsFinite(parameters.verticalProjectionScale) ||
        !IsFinite(parameters.nearPlane))
    {
        return std::unexpected(ContractError::NonFiniteValue);
    }
    if (sphereRadius < 0.0)
    {
        return std::unexpected(ContractError::NegativeSphereRadius);
    }
    if (parameters.viewportHeightPixels == 0U)
    {
        return std::unexpected(ContractError::InvalidViewportHeight);
    }
    if (parameters.verticalProjectionScale <= 0.0)
    {
        return std::unexpected(ContractError::InvalidProjectionScale);
    }
    if (parameters.nearPlane <= 0.0)
    {
        return std::unexpected(ContractError::InvalidNearPlane);
    }
    if (centerViewDepth + sphereRadius < parameters.nearPlane)
    {
        return std::unexpected(ContractError::SphereBehindNearPlane);
    }

    double const unboundedNearestDepth = centerViewDepth - sphereRadius;
    double const nearestDepth = std::max(unboundedNearestDepth, parameters.nearPlane);
    double const halfHeight = static_cast<double>(parameters.viewportHeightPixels) * 0.5;
    return ProjectedSphere{
        .nearestViewDepth = nearestDepth,
        .radiusPixels = sphereRadius * parameters.verticalProjectionScale * halfHeight / nearestDepth,
        .intersectsNearPlane = unboundedNearestDepth < parameters.nearPlane,
    };
}

std::expected<LodSelection, ContractError> SelectLod(double const projectedRadiusPixels, LodPolicy const &policy,
                                                     std::optional<std::uint32_t> const previousLod) noexcept
{
    if (!IsFinite(projectedRadiusPixels) || projectedRadiusPixels < 0.0)
    {
        return std::unexpected(ContractError::NonFiniteValue);
    }
    if (auto const valid = ValidateLodPolicy(policy); !valid)
    {
        return std::unexpected(valid.error());
    }

    std::uint32_t const rawLod = RawLod(projectedRadiusPixels, policy);
    if (!previousLod)
    {
        return LodSelection{.rawLod = rawLod, .selectedLod = rawLod};
    }
    if (*previousLod > policy.transitionRadiiPixels.size())
    {
        return std::unexpected(ContractError::PreviousLodOutOfRange);
    }

    std::uint32_t selectedLod = *previousLod;
    while (selectedLod > 0U &&
           projectedRadiusPixels >
               policy.transitionRadiiPixels[static_cast<std::size_t>(selectedLod - 1U)] + policy.hysteresisPixels)
    {
        --selectedLod;
    }
    while (selectedLod < policy.transitionRadiiPixels.size() &&
           projectedRadiusPixels <
               policy.transitionRadiiPixels[static_cast<std::size_t>(selectedLod)] - policy.hysteresisPixels)
    {
        ++selectedLod;
    }
    return LodSelection{
        .rawLod = rawLod,
        .selectedLod = selectedLod,
        .heldByHysteresis = rawLod != selectedLod,
    };
}

std::expected<OcclusionResult, ContractError> ClassifyHiZOcclusion(DepthConvention const convention,
                                                                   OcclusionEvidence const &evidence) noexcept
{
    if (!IsFinite(evidence.objectNearestDepth) || !IsFinite(evidence.farthestOccluderDepth))
    {
        return std::unexpected(ContractError::NonFiniteValue);
    }
    if (evidence.objectNearestDepth < 0.0 || evidence.objectNearestDepth > 1.0 ||
        evidence.farthestOccluderDepth < 0.0 || evidence.farthestOccluderDepth > 1.0)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }
    if (!IsFinite(evidence.bias) || evidence.bias < 0.0 || evidence.bias > 1.0)
    {
        return std::unexpected(ContractError::InvalidOcclusionBias);
    }
    if (!evidence.historyValid)
    {
        return OcclusionResult{.reason = OcclusionReason::HistoryInvalid};
    }
    if (!evidence.cameraStable)
    {
        return OcclusionResult{.reason = OcclusionReason::CameraCut};
    }
    if (!evidence.viewportMatches)
    {
        return OcclusionResult{.reason = OcclusionReason::ViewportChanged};
    }
    if (!evidence.occluderPresent)
    {
        return OcclusionResult{.reason = OcclusionReason::MissingOccluder};
    }

    bool const occluded = convention == DepthConvention::Forward
                              ? evidence.objectNearestDepth > evidence.farthestOccluderDepth + evidence.bias
                              : evidence.objectNearestDepth < evidence.farthestOccluderDepth - evidence.bias;
    return OcclusionResult{
        .occluded = occluded,
        .reason = occluded ? OcclusionReason::Occluded : OcclusionReason::DepthTestVisible,
    };
}

std::expected<SceneValidation, ContractError> ValidateScene(std::span<InstanceRecord const> instances,
                                                            std::span<DrawTemplate const> drawTemplates,
                                                            std::uint32_t const instanceBufferCapacity,
                                                            std::uint32_t const indexBufferCount)
{
    if (instances.empty())
    {
        return std::unexpected(ContractError::EmptyInstanceSet);
    }
    if (auto const valid = ValidateDrawTemplates(drawTemplates, indexBufferCount); !valid)
    {
        return std::unexpected(valid.error());
    }

    std::unordered_set<std::uint32_t> stableIds{};
    std::unordered_set<std::uint32_t> instanceDataIndices{};
    stableIds.reserve(instances.size());
    instanceDataIndices.reserve(instances.size());
    std::uint32_t maximumLodCount = 0U;

    for (InstanceRecord const &instance : instances)
    {
        if (auto const valid = ValidateSphere(instance.bounds); !valid)
        {
            return std::unexpected(valid.error());
        }
        if (!stableIds.insert(instance.stableId).second)
        {
            return std::unexpected(ContractError::DuplicateStableIdentity);
        }
        if (!instanceDataIndices.insert(instance.instanceDataIndex).second)
        {
            return std::unexpected(ContractError::DuplicateInstanceDataIndex);
        }
        if (instance.instanceDataIndex >= instanceBufferCapacity)
        {
            return std::unexpected(ContractError::InstanceDataIndexOutOfRange);
        }
        if (instance.lodCount == 0U)
        {
            return std::unexpected(ContractError::EmptyInstanceLodRange);
        }
        if (instance.firstDrawTemplate > drawTemplates.size() ||
            instance.lodCount > drawTemplates.size() - instance.firstDrawTemplate)
        {
            return std::unexpected(ContractError::DrawTemplateRangeExceeded);
        }
        if (instance.previousLod >= instance.lodCount)
        {
            return std::unexpected(ContractError::PreviousInstanceLodOutOfRange);
        }
        maximumLodCount = std::max(maximumLodCount, instance.lodCount);
    }

    return SceneValidation{
        .instanceCount = static_cast<std::uint32_t>(instances.size()),
        .drawTemplateCount = static_cast<std::uint32_t>(drawTemplates.size()),
        .maximumLodCount = maximumLodCount,
    };
}

std::expected<std::vector<IndirectCommand>, ContractError> BuildIndirectSubmission(
    std::span<InstanceRecord const> instances, std::span<DrawTemplate const> drawTemplates,
    std::span<VisibilityDecision const> decisions, std::uint32_t const outputCapacity)
{
    auto const lookup = BuildInstanceLookup(instances);
    if (!lookup)
    {
        return std::unexpected(lookup.error());
    }

    std::unordered_set<std::uint32_t> decidedIds{};
    decidedIds.reserve(decisions.size());
    std::vector<IndirectCommand> commands{};
    commands.reserve(std::min<std::size_t>(decisions.size(), outputCapacity));

    for (VisibilityDecision const &decision : decisions)
    {
        if (!decidedIds.insert(decision.stableId).second)
        {
            return std::unexpected(ContractError::DuplicateVisibilityDecision);
        }
        auto const instanceIt = lookup->find(decision.stableId);
        if (instanceIt == lookup->end())
        {
            return std::unexpected(ContractError::UnknownStableIdentity);
        }

        InstanceRecord const &instance = *instanceIt->second;
        if (decision.instanceDataIndex != instance.instanceDataIndex)
        {
            return std::unexpected(ContractError::DecisionInstanceIndexMismatch);
        }
        if (decision.lod >= instance.lodCount)
        {
            return std::unexpected(ContractError::DecisionLodOutOfRange);
        }
        if (!decision.visible)
        {
            continue;
        }
        if (commands.size() >= outputCapacity)
        {
            return std::unexpected(ContractError::OutputCapacityExceeded);
        }

        std::uint32_t const drawTemplateIndex = instance.firstDrawTemplate + decision.lod;
        if (drawTemplateIndex >= drawTemplates.size())
        {
            return std::unexpected(ContractError::DrawTemplateRangeExceeded);
        }
        DrawTemplate const &drawTemplate = drawTemplates[drawTemplateIndex];
        commands.push_back({
            .stableId = decision.stableId,
            .lod = decision.lod,
            .drawTemplateIndex = drawTemplateIndex,
            .draw =
                {
                    .indexCountPerInstance = drawTemplate.indexCount,
                    .instanceCount = 1U,
                    .startIndexLocation = drawTemplate.startIndex,
                    .baseVertexLocation = drawTemplate.baseVertex,
                    .startInstanceLocation = decision.instanceDataIndex,
                },
        });
    }
    return commands;
}

bool SubmissionDifference::Equivalent() const noexcept
{
    return missingStableIds.empty() && unexpectedStableIds.empty() && mismatchedStableIds.empty();
}

std::expected<SubmissionDifference, ContractError> CompareLogicalSubmissions(std::span<IndirectCommand const> expected,
                                                                             std::span<IndirectCommand const> observed)
{
    auto const sortedExpected = SortSubmission(expected);
    if (!sortedExpected)
    {
        return std::unexpected(sortedExpected.error());
    }
    auto const sortedObserved = SortSubmission(observed);
    if (!sortedObserved)
    {
        return std::unexpected(sortedObserved.error());
    }

    SubmissionDifference difference{};
    std::size_t expectedIndex = 0U;
    std::size_t observedIndex = 0U;
    while (expectedIndex < sortedExpected->size() || observedIndex < sortedObserved->size())
    {
        if (observedIndex == sortedObserved->size() ||
            (expectedIndex < sortedExpected->size() &&
             (*sortedExpected)[expectedIndex]->stableId < (*sortedObserved)[observedIndex]->stableId))
        {
            difference.missingStableIds.push_back((*sortedExpected)[expectedIndex]->stableId);
            ++expectedIndex;
            continue;
        }
        if (expectedIndex == sortedExpected->size() ||
            (*sortedObserved)[observedIndex]->stableId < (*sortedExpected)[expectedIndex]->stableId)
        {
            difference.unexpectedStableIds.push_back((*sortedObserved)[observedIndex]->stableId);
            ++observedIndex;
            continue;
        }

        if (!SameLogicalDraw(*(*sortedExpected)[expectedIndex], *(*sortedObserved)[observedIndex]))
        {
            difference.mismatchedStableIds.push_back((*sortedExpected)[expectedIndex]->stableId);
        }
        ++expectedIndex;
        ++observedIndex;
    }
    return difference;
}

} // namespace ch21::gpu_driven
