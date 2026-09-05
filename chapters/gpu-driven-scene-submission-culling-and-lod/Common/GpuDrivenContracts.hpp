#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace ch21::gpu_driven
{

enum class ContractError : std::uint8_t
{
    NonFiniteValue = 0U,
    NegativeSphereRadius,
    InvalidFrustumPlaneCount,
    DegenerateFrustumPlane,
    InvalidViewportHeight,
    InvalidProjectionScale,
    InvalidNearPlane,
    SphereBehindNearPlane,
    EmptyLodPolicy,
    InvalidLodThreshold,
    LodThresholdsNotDescending,
    InvalidLodHysteresis,
    PreviousLodOutOfRange,
    InvalidDepth,
    InvalidOcclusionBias,
    EmptyDrawTemplateSet,
    EmptyDraw,
    IndexRangeExceeded,
    EmptyInstanceSet,
    DuplicateStableIdentity,
    DuplicateInstanceDataIndex,
    InstanceDataIndexOutOfRange,
    EmptyInstanceLodRange,
    DrawTemplateRangeExceeded,
    PreviousInstanceLodOutOfRange,
    UnknownStableIdentity,
    DuplicateVisibilityDecision,
    DecisionInstanceIndexMismatch,
    DecisionLodOutOfRange,
    OutputCapacityExceeded,
    DuplicateSubmissionIdentity,
};

struct Float3 final
{
    double x{};
    double y{};
    double z{};

    [[nodiscard]] bool operator==(Float3 const &) const noexcept = default;
};

struct Plane final
{
    Float3 normal{};
    double distance{};
};

struct Sphere final
{
    Float3 center{};
    double radius{};
};

enum class FrustumClassification : std::uint8_t
{
    Outside = 0U,
    Intersecting,
    Inside,
};

[[nodiscard]] std::expected<FrustumClassification, ContractError> ClassifySphereAgainstFrustum(
    std::span<Plane const> planes, Sphere const &sphere) noexcept;

struct ProjectionParameters final
{
    std::uint32_t viewportHeightPixels{};
    double verticalProjectionScale{};
    double nearPlane{};
};

struct ProjectedSphere final
{
    double nearestViewDepth{};
    double radiusPixels{};
    bool intersectsNearPlane{};
};

[[nodiscard]] std::expected<ProjectedSphere, ContractError> ProjectSphereConservatively(
    double centerViewDepth, double sphereRadius, ProjectionParameters const &parameters) noexcept;

struct LodPolicy final
{
    std::vector<double> transitionRadiiPixels{};
    double hysteresisPixels{};
};

struct LodSelection final
{
    std::uint32_t rawLod{};
    std::uint32_t selectedLod{};
    bool heldByHysteresis{};

    [[nodiscard]] bool operator==(LodSelection const &) const noexcept = default;
};

[[nodiscard]] std::expected<LodSelection, ContractError> SelectLod(
    double projectedRadiusPixels, LodPolicy const &policy,
    std::optional<std::uint32_t> previousLod = std::nullopt) noexcept;

enum class DepthConvention : std::uint8_t
{
    Forward = 0U,
    Reversed,
};

enum class OcclusionReason : std::uint8_t
{
    Occluded = 0U,
    DepthTestVisible,
    MissingOccluder,
    HistoryInvalid,
    CameraCut,
    ViewportChanged,
};

struct OcclusionEvidence final
{
    double objectNearestDepth{};
    double farthestOccluderDepth{};
    double bias{};
    bool occluderPresent{true};
    bool historyValid{true};
    bool cameraStable{true};
    bool viewportMatches{true};
};

struct OcclusionResult final
{
    bool occluded{};
    OcclusionReason reason{OcclusionReason::DepthTestVisible};

    [[nodiscard]] bool operator==(OcclusionResult const &) const noexcept = default;
};

[[nodiscard]] std::expected<OcclusionResult, ContractError> ClassifyHiZOcclusion(
    DepthConvention convention, OcclusionEvidence const &evidence) noexcept;

struct DrawIndexedArguments final
{
    std::uint32_t indexCountPerInstance{};
    std::uint32_t instanceCount{};
    std::uint32_t startIndexLocation{};
    std::int32_t baseVertexLocation{};
    std::uint32_t startInstanceLocation{};

    [[nodiscard]] bool operator==(DrawIndexedArguments const &) const noexcept = default;
};

struct DrawTemplate final
{
    std::uint32_t indexCount{};
    std::uint32_t startIndex{};
    std::int32_t baseVertex{};
    std::uint32_t materialIndex{};

    [[nodiscard]] bool operator==(DrawTemplate const &) const noexcept = default;
};

struct InstanceRecord final
{
    std::uint32_t stableId{};
    std::uint32_t instanceDataIndex{};
    std::uint32_t firstDrawTemplate{};
    std::uint32_t lodCount{};
    std::uint32_t previousLod{};
    Sphere bounds{};
};

struct SceneValidation final
{
    std::uint32_t instanceCount{};
    std::uint32_t drawTemplateCount{};
    std::uint32_t maximumLodCount{};

    [[nodiscard]] bool operator==(SceneValidation const &) const noexcept = default;
};

[[nodiscard]] std::expected<SceneValidation, ContractError> ValidateScene(std::span<InstanceRecord const> instances,
                                                                          std::span<DrawTemplate const> drawTemplates,
                                                                          std::uint32_t instanceBufferCapacity,
                                                                          std::uint32_t indexBufferCount);

struct VisibilityDecision final
{
    std::uint32_t stableId{};
    std::uint32_t instanceDataIndex{};
    std::uint32_t lod{};
    bool visible{};
};

struct IndirectCommand final
{
    std::uint32_t stableId{};
    std::uint32_t lod{};
    std::uint32_t drawTemplateIndex{};
    DrawIndexedArguments draw{};

    [[nodiscard]] bool operator==(IndirectCommand const &) const noexcept = default;
};

[[nodiscard]] std::expected<std::vector<IndirectCommand>, ContractError> BuildIndirectSubmission(
    std::span<InstanceRecord const> instances, std::span<DrawTemplate const> drawTemplates,
    std::span<VisibilityDecision const> decisions, std::uint32_t outputCapacity);

struct SubmissionDifference final
{
    std::vector<std::uint32_t> missingStableIds{};
    std::vector<std::uint32_t> unexpectedStableIds{};
    std::vector<std::uint32_t> mismatchedStableIds{};

    [[nodiscard]] bool Equivalent() const noexcept;
    [[nodiscard]] bool operator==(SubmissionDifference const &) const noexcept = default;
};

[[nodiscard]] std::expected<SubmissionDifference, ContractError> CompareLogicalSubmissions(
    std::span<IndirectCommand const> expected, std::span<IndirectCommand const> observed);

} // namespace ch21::gpu_driven
