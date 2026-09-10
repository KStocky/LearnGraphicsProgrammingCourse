#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "DxrContracts.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Chapter 33 paired GPU lab: the DXR 1.0 frame written out as the ordered stages it actually is, with every stage
// publishing the evidence that says it ran, what it decided, and what the next stage was allowed to assume.
//
// What the two variants do, and why they are a pair:
//
//   * The Starter answers the same question with no raytracing pipeline at all. It uploads the same scene tables,
//     generates the same primary rays, transforms each ray into each instance's object space, and runs
//     Moller-Trumbore against the same two triangles in a compute shader. It applies the same instance inclusion
//     mask, the same strict `TMin < t < TMax` interval, the same object-space facing rule and the same hit-group
//     record arithmetic, and it writes the same per-pixel record ABI. It builds no acceleration structure, creates
//     no state object and uploads no shader table, and the stage ledger it publishes says exactly that. It is an
//     honest baseline: everything a learner can already do with a compute shader, and nothing more.
//   * The Solution runs the real thing: a one-geometry triangle bottom-level structure, a top-level structure of
//     three instances that is rebuilt or refitted on request, a runtime `lib_6_3` DXIL library, a raytracing state
//     object with a global root signature and a hit-group-local root signature, a shader table whose records are
//     laid out by the chapter's own contracts, `SetPipelineState1`, and `DispatchRays`. The observable difference
//     is not the picture - both variants agree pixel for pixel on the same scene - it is *who decided*: the Starter
//     selects a material with an `if`, and the Solution proves the fixed-function traversal selected a shader-table
//     record for it.
//
// This header owns the ABI, the analytic scene, the configuration and its validation, the device objects, the
// barriers and the submission order. It owns none of the traversal: the analytic intersector and the raytracing
// shaders both live in the learner-owned HLSL of each variant, because they are the lesson.
//
// Honesty rules this lab holds itself to:
//
//   * Nothing is silently emulated. If the adapter reports no raytracing tier or no Shader Model 6.3, the Solution
//     refuses to render and names the capability it was missing; it never falls back to the analytic path and
//     calls the result raytracing.
//   * The per-frame stage ledger reports the stages the frame actually submitted. State-object creation happens
//     once, at initialization, so the ledger records the state object as *resolved* rather than created, and a
//     test can tell the difference.
//   * Every acceleration-structure barrier is the narrowest truthful one, and every barrier the frame submitted is
//     published so a test can assert the set rather than trust a comment.
//   * Floating-point results are compared with declared tolerances. The scene is arranged so that no ray lands
//     near a triangle edge and so that every interesting `t` is an exact power-of-two multiple, but the lab still
//     never demands bit identity from a driver it does not own.
//   * The lab does not bind a null acceleration structure. D3D12 defines a null acceleration-structure descriptor
//     as a guaranteed miss for every ray, and it would be the tidiest possible proof that the picture comes from
//     the traversal, but WARP 1.0.20 removes the device when a `DispatchRays` reaches one - through a root
//     descriptor and through a descriptor table alike, with no debug-layer message first. The chapter proves the
//     same thing with an instance inclusion mask that intersects no instance, which every implementation has to
//     honour, and records the WARP behaviour here so the next reader does not rediscover it at a device removal.

namespace ch33::dxr::gpu
{

// -------------------------------------------------------------------------------------------------------------
// Extent, scene, and ABI constants
// -------------------------------------------------------------------------------------------------------------

// The lab dispatches one ray per pixel and reads every record back, so the extent is chosen to keep a WARP frame
// quick while leaving each instance dozens of pixels wide. 96 x 64 gives a 1/32-metre sample pitch in both axes;
// pixel centres are odd multiples of 1/64 metre from the window edge, safely away from triangle edges.
inline constexpr std::uint32_t kMaximumWidth = 96U;
inline constexpr std::uint32_t kMaximumHeight = 64U;
inline constexpr std::uint32_t kMinimumWidth = 16U;
inline constexpr std::uint32_t kMinimumHeight = 16U;
inline constexpr std::uint32_t kGroupWidth = 8U;
inline constexpr std::uint32_t kGroupHeight = 8U;

// The orthographic ray window, in metres. Rays start on the plane z = kCameraOriginZ and travel along -Z, so the
// distance to a plane at z = c is exactly kCameraOriginZ - c: every interesting `t` in this lab is an exact
// dyadic rational, and a strict interval endpoint can be stated rather than approximated.
inline constexpr double kCameraOriginZ = 2.0;
inline constexpr double kWindowHalfExtentX = 1.5;
inline constexpr double kWindowHalfExtentY = 1.0;

// One geometry of two disjoint triangles. They are disjoint on purpose: a shared edge would put rays exactly on a
// boundary that hardware and a CPU model are allowed to disagree about, and the gap between them makes a miss
// observable in the middle of the picture rather than only at its border.
inline constexpr std::uint32_t kGeometryCount = 1U;
inline constexpr std::uint32_t kTrianglesPerGeometry = 2U;
inline constexpr std::uint32_t kVertexCount = kTrianglesPerGeometry * 3U;

inline constexpr std::uint32_t kMaximumInstanceCount = 3U;
inline constexpr std::uint32_t kMaterialCount = 4U;
inline constexpr std::uint32_t kHitGroupRecordCount = kMaterialCount;
inline constexpr std::uint32_t kMissRecordCount = 2U;
inline constexpr std::uint32_t kRayGenerationRecordCount = 1U;

// The five 32-bit local root arguments each hit-group record carries after its 32-byte identifier: the material
// identity, the record index the record believes it is, and a colour. The record index is the point: a closest-hit
// shader that reports the index baked into its own record proves which record the traversal selected.
inline constexpr std::uint32_t kHitGroupLocalConstantCount = 5U;
inline constexpr std::uint32_t kHitGroupLocalArgumentSizeBytes = kHitGroupLocalConstantCount * 4U;

// The payload the chapter declares, as a byte count. It is checked against the contract's payload layout at
// initialization so that `MaxPayloadSizeInBytes` cannot drift away from the HLSL structure.
inline constexpr std::uint32_t kPayloadSizeBytes = 56U;
inline constexpr std::uint32_t kAttributeSizeBytes = kTriangleAttributeSizeBytes;
inline constexpr std::uint32_t kTraceRecursionDepth = 1U;

inline constexpr std::uint32_t kAbiMarker = 0x4478'5233U;

// The tolerance the lab compares GPU floats against its double-precision model with. A triangle intersection is a
// handful of multiplies and one divide, so a few ULP of a float is the honest budget; anything larger would hide a
// real disagreement and anything smaller would demand bit identity from a driver the course does not own.
inline constexpr double kDistanceToleranceMetres = 1.0e-5;
inline constexpr double kBarycentricTolerance = 1.0e-5;
inline constexpr double kPositionToleranceMetres = 1.0e-5;

// The smallest barycentric coordinate any ray in a scene is allowed to produce. A ray closer to an edge than this
// is a ray whose hit/miss answer depends on rounding, so the lab refuses to build a scene that contains one rather
// than shipping a test that fails once a month.
inline constexpr double kMinimumEdgeMargin = 1.0e-3;

// The distance any ray-interval endpoint must keep from a surface in the scene, in metres.
//
// D3D12 specifies a triangle hit as strictly inside the interval - `TMin < t < TMax` - and this lab's model and
// its analytic shader both implement exactly that. Hardware is not obliged to agree at the endpoint, and WARP
// 1.0.20 does not: it accepts a hit at `t == TMin` and at `t == TMax`, and it quantises `TMin` conservatively, so
// a `TMin` about one part in a thousand above a surface still accepts the hit. That is a defensible
// implementation choice - a traversal that culled a hit it should have kept would be far worse than one that kept
// a hit it could have culled - and it means an endpoint-exact test measures a driver rather than DXR. The chapter
// therefore refuses any configuration that places an endpoint within this margin of a surface and tests the
// interval with values every implementation has to agree about. The margin is roughly thirty times the slack WARP
// was measured to need.
inline constexpr double kIntervalEndpointMarginMetres = 0.05;

enum class LabVariant : std::uint8_t
{
    Starter,
    Solution,
};

// The scene the frame traces. Every variant exists because some claim is only checkable in it.
enum class SceneVariant : std::uint32_t
{
    // Three instances: a left panel at t = 2, a mirrored right panel at t = 2.5, and a small near overlay at
    // t = 1.5 that covers part of the left panel. This is the picture the chapter teaches with, and the overlay is
    // what makes "nearest hit wins" and the strict interval endpoints observable.
    Paired = 0U,
    // The same three instances, except the right panel's transform is a positive-determinant rotation that turns
    // its triangles away from the camera. It is the back-facing case, and pairing it with `Paired` - whose right
    // panel is *mirrored*, and therefore still front facing - is how the object-space winding rule is measured
    // instead of asserted.
    FacingPair = 1U,
    // The left panel alone. The instance count changes and nothing else does.
    SingleInstance = 2U,
    // The two panels without the near overlay, so the only distances in the frame are 2 and 2.5 and an interval
    // test has nothing else to reject.
    NoOverlay = 3U,
};

inline constexpr std::uint32_t kSceneVariantCount = static_cast<std::uint32_t>(SceneVariant::NoOverlay) + 1U;

// Whether the frame rebuilds the top-level structure or refits the one this frame slot already holds. `Update` is
// D3D12's `PERFORM_UPDATE`, and it is a separate mode rather than a flag because "updated a structure that never
// allowed updates" and "updated with different flags than the build" have to stay distinguishable.
enum class TopLevelBuildMode : std::uint32_t
{
    Rebuild = 0U,
    Update = 1U,
};

enum class DebugView : std::uint32_t
{
    // The shaded picture: the record's colour, which is the hit-group record's own local colour modulated by the
    // barycentrics, or the miss shader's background.
    Final = 0U,
    // Hit or miss only. The cheapest answer to "is anything being traced at all".
    HitMiss = 1U,
    // The instance the traversal reported, by `InstanceID()`.
    InstanceIdentity = 2U,
    // `PrimitiveIndex()`, which is the only way to see that a bottom-level structure holds more than one triangle.
    PrimitiveIdentity = 3U,
    // The raw attribute pair, as red and green.
    Barycentrics = 4U,
    // `RayTCurrent()` mapped across the configured interval.
    RayDistance = 5U,
    // Front face, back face, or miss.
    FaceOrientation = 6U,
    // The hit-group record index the traversal selected, taken from the record's own local root arguments.
    ShaderRecord = 7U,
    // The stage ledger as a column per stage: submitted, skipped, or unavailable to this variant.
    StageLedger = 8U,
};

inline constexpr std::uint32_t kDebugViewCount = static_cast<std::uint32_t>(DebugView::StageLedger) + 1U;

// The stages a frame can submit, in submission order. The five D3D12 stages the contracts model are a subset: the
// two barriers and the readback are stages here because they are things a frame either did or did not do, and a
// learner debugging a black image needs to see them in the same list.
enum class LabStage : std::uint32_t
{
    SceneUpload = 0U,
    BottomLevelBuild = 1U,
    BottomLevelBarrier = 2U,
    TopLevelBuild = 3U,
    TopLevelBarrier = 4U,
    StateObjectResolved = 5U,
    ShaderTableRecorded = 6U,
    RayDispatch = 7U,
    OutputReadback = 8U,
};

inline constexpr std::uint32_t kLabStageCount = static_cast<std::uint32_t>(LabStage::OutputReadback) + 1U;

[[nodiscard]] std::string_view LabStageName(LabStage stage) noexcept;

// The resources the recorded barrier evidence refers to. Identifiers rather than pointers, so a test can assert
// the barrier set without owning the resources.
enum class LabResource : std::uint32_t
{
    BottomLevelStructure = 1U,
    BottomLevelScratch = 2U,
    TopLevelStructure = 3U,
    TopLevelScratch = 4U,
    RayRecords = 5U,
    FrameCounters = 6U,
};

// Per-pixel status bits. Everything here is a fact the shader observed, not an inference the CPU drew afterwards.
inline constexpr std::uint32_t kRayStatusTraversalRan = 1U << 0U;
inline constexpr std::uint32_t kRayStatusHit = 1U << 1U;
inline constexpr std::uint32_t kRayStatusMiss = 1U << 2U;
inline constexpr std::uint32_t kRayStatusFrontFace = 1U << 3U;
inline constexpr std::uint32_t kRayStatusBackFace = 1U << 4U;
inline constexpr std::uint32_t kRayStatusClosestHitRan = 1U << 5U;
inline constexpr std::uint32_t kRayStatusMissShaderRan = 1U << 6U;
// Set by a closest-hit shader that read its own record's local root arguments. It is the difference between "a hit
// group ran" and "the hit group the shader table selected ran".
inline constexpr std::uint32_t kRayStatusLocalRootArgumentsRead = 1U << 7U;
// Exactly one of these two is set by every record. They are the lab's refusal to let either variant claim the
// other's mechanism: the Solution's bit can only be written by a shader in the raytracing state object, and the
// Starter's can only be written by its compute shader.
inline constexpr std::uint32_t kRayStatusFixedFunctionTraversal = 1U << 8U;
inline constexpr std::uint32_t kRayStatusAnalyticTraversal = 1U << 9U;

// The status bits a variant owns because only its mechanism can produce them. Comparing the Starter's records with
// the Solution's means comparing everything outside this mask for equality and everything inside it for
// difference: the two must agree about the scene and disagree about who traversed it.
inline constexpr std::uint32_t kVariantOwnedStatusMask = kRayStatusClosestHitRan | kRayStatusMissShaderRan |
                                                         kRayStatusLocalRootArgumentsRead |
                                                         kRayStatusFixedFunctionTraversal | kRayStatusAnalyticTraversal;
inline constexpr std::uint32_t kSharedStatusMask =
    kRayStatusTraversalRan | kRayStatusHit | kRayStatusMiss | kRayStatusFrontFace | kRayStatusBackFace;

// The HLSL `HitKind()` values for the fixed-function triangle intersector. They are mirrored rather than derived
// because the record stores what the shader reported.
inline constexpr std::uint32_t kHitKindTriangleFrontFace = 254U;
inline constexpr std::uint32_t kHitKindTriangleBackFace = 255U;

inline constexpr std::uint32_t kInvalidIndex = 0xFFFF'FFFFU;

// One object-space triangle vertex. The bottom-level build reads this array directly as
// `DXGI_FORMAT_R32G32B32_FLOAT` with a 12-byte stride, so the packing is the vertex buffer's packing.
struct VertexPosition final
{
    float x{};
    float y{};
    float z{};

    [[nodiscard]] bool operator==(VertexPosition const &) const noexcept = default;
};
static_assert(sizeof(VertexPosition) == 12U);
static_assert(alignof(VertexPosition) == 4U);

// One instance, in the form both the CPU model and the analytic compute shader consume. The inverse is stored
// rather than inverted at trace time because every transform in this lab is a dyadic scale and translation, whose
// inverse is exactly representable: the object-space ray the Starter forms is the object-space ray the hardware
// forms, without an inversion error to explain away.
struct InstanceRecord final
{
    std::array<float, 12U> objectToWorld{};
    std::array<float, 12U> worldToObject{};
    std::uint32_t instanceId{};
    std::uint32_t instanceMask{};
    std::uint32_t instanceContributionToHitGroupIndex{};
    std::uint32_t flags{};

    [[nodiscard]] bool operator==(InstanceRecord const &) const noexcept = default;
};
static_assert(sizeof(InstanceRecord) == 112U);
static_assert(alignof(InstanceRecord) == 4U);

// One hit-group record's local root arguments, and the Starter's material table entry. The same 20 bytes are
// written into a shader-table record by the Solution and into a structured buffer by the Starter, which is what
// makes "the traversal selected this record" and "an if selected this material" comparable.
struct HitGroupLocalConstants final
{
    std::uint32_t materialId{};
    std::uint32_t recordIndex{};
    float colorR{};
    float colorG{};
    float colorB{};

    [[nodiscard]] bool operator==(HitGroupLocalConstants const &) const noexcept = default;
};
static_assert(sizeof(HitGroupLocalConstants) == kHitGroupLocalArgumentSizeBytes);
static_assert(alignof(HitGroupLocalConstants) == 4U);

// The per-pixel record both variants write. Its layout is the shared HLSL structure's layout; the static asserts
// below are what keeps the two from drifting.
struct RayRecord final
{
    std::uint32_t status{};
    std::uint32_t instanceIndex{kInvalidIndex};
    std::uint32_t instanceId{kInvalidIndex};
    std::uint32_t primitiveIndex{kInvalidIndex};
    std::uint32_t hitKind{};
    std::uint32_t hitGroupRecordIndex{kInvalidIndex};
    std::uint32_t materialId{kInvalidIndex};
    std::uint32_t missShaderIndex{kInvalidIndex};
    float tHit{};
    float barycentricB1{};
    float barycentricB2{};
    float worldPositionX{};
    float worldPositionY{};
    float worldPositionZ{};
    float colorR{};
    float colorG{};
    float colorB{};
    std::uint32_t stageMask{};
    std::uint32_t reservedA{};
    std::uint32_t reservedB{};

    [[nodiscard]] bool operator==(RayRecord const &) const noexcept = default;
};
static_assert(sizeof(RayRecord) == 80U);
static_assert(alignof(RayRecord) == 4U);

// Frame-wide counters the shaders accumulate. They exist because a per-pixel buffer cannot prove that a dispatch
// covered the grid it was asked to cover, and because a counter that disagrees with the records is a much louder
// symptom than a picture that looks plausible.
struct FrameRecord final
{
    std::uint32_t abiMarker{};
    std::uint32_t rayCount{};
    std::uint32_t hitCount{};
    std::uint32_t missCount{};
    std::uint32_t frontFaceCount{};
    std::uint32_t backFaceCount{};
    std::uint32_t traversalMask{};
    std::uint32_t dispatchWidth{};
    std::uint32_t dispatchHeight{};
    std::uint32_t reserved{};
    std::array<std::uint32_t, kHitGroupRecordCount> hitGroupRecordCounts{};
    std::array<std::uint32_t, kMissRecordCount> missRecordCounts{};
    std::array<std::uint32_t, kMaximumInstanceCount> instanceHitCounts{};

    [[nodiscard]] bool operator==(FrameRecord const &) const noexcept = default;
};
static_assert(sizeof(FrameRecord) == 76U);
static_assert(alignof(FrameRecord) == 4U);

// The constants both dispatches read. Scalars only: a `float3` in a constant buffer would move to the next 16-byte
// boundary and the C++ mirror would silently disagree with the HLSL one.
struct DispatchConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t instanceCount{};
    std::uint32_t instanceInclusionMask{};
    std::uint32_t rayContributionToHitGroupIndex{};
    std::uint32_t multiplierForGeometryContribution{};
    std::uint32_t missShaderIndex{};
    std::uint32_t rayFlags{};
    float windowHalfExtentX{};
    float windowHalfExtentY{};
    float cameraOriginZ{};
    float rayTMin{};
    float rayTMax{};
    std::uint32_t hitGroupRecordCount{};
    std::uint32_t materialCount{};
    std::uint32_t frameIndex{};
};
static_assert(sizeof(DispatchConstants) % 4U == 0U);

struct DisplayConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    // The render target's size. The ray grid is capped so that a WARP frame stays quick, so an interactive window
    // is usually larger than the grid and the display magnifies it. Headless frames set the two equal.
    std::uint32_t surfaceWidth{};
    std::uint32_t surfaceHeight{};
    std::uint32_t debugView{};
    std::uint32_t variant{};
    std::uint32_t stageMask{};
    std::uint32_t stageSkippedMask{};
    std::uint32_t stageUnavailableMask{};
    std::uint32_t hitGroupRecordCount{};
    float rayTMin{};
    float rayTMax{};
    std::uint32_t hitCount{};
    std::uint32_t missCount{};
};
static_assert(sizeof(DisplayConstants) % 4U == 0U);

// -------------------------------------------------------------------------------------------------------------
// Capability
// -------------------------------------------------------------------------------------------------------------

enum class RaytracingSupportStatus : std::uint32_t
{
    Supported = 0U,
    OptionsQueryFailed = 1U,
    TierUnsupported = 2U,
    ShaderModelQueryFailed = 3U,
    ShaderModelUnsupported = 4U,
    StateObjectUnavailable = 5U,
    ShaderIdentifierUnavailable = 6U,
};

// What the adapter said, in the adapter's own numbers. `reportedTier` is the raw `D3D12_RAYTRACING_TIER` value and
// `reportedShaderModel` the raw `D3D_SHADER_MODEL`, because a diagnostic that prints "unsupported" without the
// number it read is a diagnostic a learner cannot act on.
struct RaytracingCapability final
{
    RaytracingSupportStatus status{RaytracingSupportStatus::OptionsQueryFailed};
    std::uint32_t reportedTier{};
    std::uint32_t reportedShaderModel{};
    bool tier10Available{};
    bool shaderModel63Available{};
    bool dispatchable{};

    [[nodiscard]] bool operator==(RaytracingCapability const &) const noexcept = default;
};

[[nodiscard]] std::string_view RaytracingSupportDiagnostic(RaytracingSupportStatus status) noexcept;

// The adapter's highest shader model as `major.minor`. `D3D_SHADER_MODEL` packs one nibble per component, so the
// raw value is only readable in hexadecimal and printing it in decimal - or in decimal behind an `0x` - turns
// Shader Model 6.3 into "99" or "0x99".
[[nodiscard]] std::string DescribeShaderModel(std::uint32_t shaderModel);

// -------------------------------------------------------------------------------------------------------------
// Published evidence
// -------------------------------------------------------------------------------------------------------------

struct ShaderArtifact final
{
    std::wstring entryPoint{};
    std::wstring targetProfile{};
    std::size_t bytecodeSizeBytes{};
    bool diagnosticsEmpty{};

    [[nodiscard]] bool operator==(ShaderArtifact const &) const noexcept = default;
};

// One barrier the frame actually submitted, in the enumerators D3D12 was given. Tests assert this set rather than
// trusting that a comment describes the code below it.
struct RecordedBufferBarrier final
{
    LabResource resource{LabResource::BottomLevelStructure};
    std::uint32_t syncBefore{};
    std::uint32_t syncAfter{};
    std::uint32_t accessBefore{};
    std::uint32_t accessAfter{};

    [[nodiscard]] bool operator==(RecordedBufferBarrier const &) const noexcept = default;
};

// The prebuild sizes for one acceleration-structure build, kept as two separate triples.
//
// D3D12 owns the first one: `GetRaytracingAccelerationStructurePrebuildInfo` returns three driver-specific
// numbers, and this lab never edits them, because a diagnostic that prints an adjusted number as if the driver
// said it is worse than no diagnostic at all. The second is the lab's own decision: the sizes it allocated buffers
// for and handed to `ValidateBuildRequest`. Publishing both is what lets a reader see which of the two a
// disagreement belongs to.
struct PrebuildEvidence final
{
    // Exactly what the adapter reported, untouched.
    PrebuildInfo reported{};
    // What the lab allocated and validated against: the reported result size rounded up to the
    // acceleration-structure alignment, the reported build scratch unchanged, zero refit scratch for a build that
    // cannot update, and - for a build that declares `ALLOW_UPDATE` - a refit scratch raised to at least the build
    // scratch.
    PrebuildInfo allocationBudget{};

    [[nodiscard]] bool operator==(PrebuildEvidence const &) const noexcept = default;
};

struct AccelerationStructureEvidence final
{
    PrebuildEvidence bottomLevelPrebuild{};
    PrebuildEvidence topLevelPrebuild{};
    AccelerationStructureBuildPlan bottomLevelPlan{};
    AccelerationStructureBuildPlan topLevelPlan{};
    TopLevelPacking packing{};
    BuildTimelineValidation timeline{};
    std::uint64_t bottomLevelAddress{};
    std::uint64_t bottomLevelScratchAddress{};
    std::uint64_t topLevelAddress{};
    std::uint64_t topLevelScratchAddress{};
    std::uint64_t instanceBufferAddress{};
    std::uint32_t instanceCount{};
    std::uint32_t geometryPrimitiveCount{};
    bool topLevelUpdateRequested{};
    bool topLevelUpdated{};
    // The two builds own separate scratch buffers. Sharing one is legal and needs a barrier between the builds;
    // this lab does not share, so the absence of that barrier is a fact rather than an omission.
    bool scratchBuffersDistinct{};

    [[nodiscard]] bool operator==(AccelerationStructureEvidence const &) const noexcept = default;
};

struct ShaderTableEvidence final
{
    ShaderTableLayout layout{};
    DispatchRaysDescription dispatchDescription{};
    std::uint64_t baseAddress{};
    std::uint64_t totalSizeBytes{};
    std::array<std::uint64_t, kHitGroupRecordCount> hitGroupRecordAddresses{};
    std::array<std::uint64_t, kMissRecordCount> missRecordAddresses{};
    std::uint64_t rayGenerationRecordAddress{};
    // The hit-group record index the configuration selects for each instance, computed by the contract from the
    // same four terms D3D12 adds together at trace time.
    std::array<std::uint64_t, kMaximumInstanceCount> instanceRecordIndices{};
    std::uint32_t identifierByteCount{};
    // Shader identifiers are only meaningful while the state object that produced them is alive, so the lab copies
    // them into the table during initialization and records that it did.
    bool identifiersCopiedWhileStateObjectAlive{};

    [[nodiscard]] bool operator==(ShaderTableEvidence const &) const noexcept = default;
};

struct StateObjectEvidence final
{
    StateObjectValidation validation{};
    PayloadLayout payload{};
    std::uint32_t subobjectCount{};
    std::uint32_t exportCount{};
    std::uint32_t hitGroupCount{};
    bool created{};
    bool localRootSignatureAssociated{};

    [[nodiscard]] bool operator==(StateObjectEvidence const &) const noexcept = default;
};

// -------------------------------------------------------------------------------------------------------------
// Configuration
// -------------------------------------------------------------------------------------------------------------

struct LabConfiguration final
{
    DebugView debugView{DebugView::Final};
    SceneVariant scene{SceneVariant::Paired};
    TopLevelBuildMode topLevelBuild{TopLevelBuildMode::Rebuild};

    // The 8-bit `InstanceInclusionMask` a `TraceRay` carries. Zero is refused: D3D12 accepts it and guarantees a
    // miss, which is never what a caller meant.
    std::uint32_t instanceInclusionMask{0xFFU};
    std::uint32_t rayContributionToHitGroupIndex{0U};
    std::uint32_t multiplierForGeometryContribution{1U};
    std::uint32_t missShaderIndex{0U};
    std::uint32_t frameIndex{0U};

    float rayTMin{0.0F};
    float rayTMax{16.0F};

    bool cullBackFacingTriangles{false};
    bool cullFrontFacingTriangles{false};

    [[nodiscard]] bool operator==(LabConfiguration const &) const noexcept = default;
};

[[nodiscard]] LabConfiguration DefaultConfiguration(LabVariant variant) noexcept;
[[nodiscard]] lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration,
                                                              LabVariant variant);

// The `RayFlags` the configuration implies, in the contract's vocabulary.
[[nodiscard]] RayFlags MakeRayFlags(LabConfiguration const &configuration) noexcept;
// The same flags as the `RAY_FLAG` bits HLSL uses, which is what the constant buffer carries.
[[nodiscard]] std::uint32_t MakeHlslRayFlags(LabConfiguration const &configuration) noexcept;

// -------------------------------------------------------------------------------------------------------------
// The analytic scene
// -------------------------------------------------------------------------------------------------------------

[[nodiscard]] std::span<VertexPosition const> GeometryVertices() noexcept;
[[nodiscard]] std::span<HitGroupLocalConstants const> MaterialTable() noexcept;
[[nodiscard]] std::vector<InstanceRecord> BuildInstances(SceneVariant scene);
// The distance along every ray at which each instance's plane sits, in instance order. Every surface in this lab
// is a plane perpendicular to the ray direction, so a scene has exactly one distance per instance and a test can
// state the interval it wants without measuring anything.
[[nodiscard]] std::vector<double> ScenePlaneDistances(SceneVariant scene);
// The same instances in the contract's own type, so that packing, masking and identity rules are checked by the
// contract rather than re-implemented here.
[[nodiscard]] std::vector<InstanceDescription> BuildInstanceDescriptions(SceneVariant scene,
                                                                         std::uint64_t bottomLevelAddress);
[[nodiscard]] std::vector<ReferenceInstance> BuildReferenceInstances(SceneVariant scene);

// The stages a variant submits for a configuration, in submission order.
[[nodiscard]] std::uint32_t BuildFrameStages(LabConfiguration const &configuration, LabVariant variant,
                                             std::span<LabStage> stages) noexcept;
// Four bits per submitted stage, holding the stage enumerator plus one so an unused nibble reads as zero.
[[nodiscard]] std::uint64_t EncodeStageOrder(std::span<LabStage const> stages) noexcept;
// A bit per submitted stage. It is derived from the submitted list rather than accumulated alongside it, so the
// mask and the ordered list cannot disagree about which stages the frame ran.
[[nodiscard]] std::uint32_t StageMask(std::span<LabStage const> stages) noexcept;
// A bit per stage the variant can never submit, because it has no such object at all.
[[nodiscard]] std::uint32_t UnavailableStageMask(LabVariant variant) noexcept;

// -------------------------------------------------------------------------------------------------------------
// The deterministic CPU model
//
// The model is written from the same formulation the shaders use - transform the ray into object space, intersect
// with Moller-Trumbore, keep the nearest accepted hit - because the thing being checked is the GPU's arithmetic,
// not a second algorithm. It is deliberately *not* the contract's reference intersector: that one evaluates facing
// on world-space vertices, which disagrees with DXR for a mirrored instance by exactly the sign of the transform
// determinant. Tests cross-check the two on the scenes where both formulations must agree.
// -------------------------------------------------------------------------------------------------------------

struct ReferenceFrame final
{
    std::vector<RayRecord> records{};
    FrameRecord frame{};
    // The smallest barycentric coordinate any hit in the frame produced. It is published so a test can prove the
    // scene keeps every ray away from an edge instead of assuming it does.
    double minimumEdgeMargin{};
    std::uint32_t hitCount{};
    std::uint32_t missCount{};
};

[[nodiscard]] std::expected<ReferenceFrame, lgp::framework::Error> BuildReferenceFrame(
    LabConfiguration const &configuration, LabVariant variant, lgp::framework::Extent2D extent);

// -------------------------------------------------------------------------------------------------------------
// Readback
// -------------------------------------------------------------------------------------------------------------

struct FrameReadback final
{
    LabConfiguration configuration{};
    LabVariant variant{};
    lgp::framework::Extent2D displaySize{};
    std::vector<RayRecord> records{};
    FrameRecord frame{};

    ReferenceFrame reference{};
    RaytracingCapability capability{};
    AccelerationStructureEvidence accelerationStructures{};
    ShaderTableEvidence shaderTable{};
    StateObjectEvidence stateObject{};
    std::vector<RecordedBufferBarrier> barriers{};
    std::vector<BuildStep> buildTimeline{};
    std::array<StageStatus, kPipelineStageCount> ledger{};
    bool ledgerReadyToDispatch{};
    std::uint32_t ledgerEvidenceCount{};

    std::array<LabStage, kLabStageCount> stages{};
    std::uint32_t stageCount{};
    std::uint64_t stageOrderWord{};
    std::uint32_t stageMask{};
    // The stages this variant could have submitted and did not. It is the complement of the submitted mask within
    // the stages the variant owns at all, so the three masks partition the stage list exactly.
    std::uint32_t stageSkippedMask{};
    std::uint32_t stageUnavailableMask{};

    std::vector<InstanceRecord> instances{};
    std::uint32_t frameSlot{};
    // The identity of the per-frame-slot resources this frame used. Two frames in flight must not share them, and
    // an address a test can compare is how that is proved rather than promised.
    std::uint64_t rayRecordGpuAddress{};
    std::uint64_t topLevelGpuAddress{};
    std::uint64_t shaderTableGpuAddress{};
};

// -------------------------------------------------------------------------------------------------------------
// Device objects
// -------------------------------------------------------------------------------------------------------------

struct BufferBarrierState final
{
    D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};
    D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};
};

class BufferResource final
{
  public:
    BufferResource() = default;
    BufferResource(BufferResource &&other) noexcept;
    BufferResource &operator=(BufferResource &&other) noexcept;
    BufferResource(BufferResource const &) = delete;
    BufferResource &operator=(BufferResource const &) = delete;
    ~BufferResource();

    [[nodiscard]] ID3D12Resource *Get() const noexcept
    {
        return resource_.Get();
    }
    [[nodiscard]] std::uint64_t size_in_bytes() const noexcept
    {
        return sizeInBytes_;
    }
    [[nodiscard]] std::uint64_t gpu_virtual_address() const noexcept;
    [[nodiscard]] std::byte const *mapped_data() const noexcept
    {
        return mappedData_;
    }
    [[nodiscard]] std::byte *mapped_data() noexcept
    {
        return mappedData_;
    }

  private:
    friend std::expected<BufferResource, lgp::framework::Error> CreateBuffer(ID3D12Device10 &, std::uint64_t,
                                                                             D3D12_HEAP_TYPE, D3D12_RESOURCE_FLAGS,
                                                                             std::wstring_view, bool);
    void Reset() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::uint64_t sizeInBytes_{};
    std::byte *mappedData_{};
};

[[nodiscard]] std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
    ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
    std::wstring_view name, bool mapPersistently = false);

class RendererCore : public lgp::framework::IChapterRenderer
{
  public:
    RendererCore(std::filesystem::path shaderPath, LabVariant variant);

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void ConfigureHeadlessTest(LabConfiguration const &configuration) noexcept;
    [[nodiscard]] std::expected<FrameReadback, lgp::framework::Error> ReadBackOutputs();
    [[nodiscard]] RaytracingCapability capability() const noexcept;
    [[nodiscard]] std::span<ShaderArtifact const> shader_artifacts() const noexcept;

  private:
    // Every mutable resource a frame touches is owned by the frame slot that submitted it. Two frames are in
    // flight, both rebuild an acceleration structure and both write a records buffer, so sharing any of these
    // would be a race whose symptom is an image that is right most of the time.
    struct FrameSlotResources final
    {
        BufferResource vertices{};
        BufferResource instanceDescriptions{};
        BufferResource bottomLevel{};
        BufferResource bottomLevelScratch{};
        BufferResource topLevel{};
        BufferResource topLevelScratch{};
        BufferResource shaderTable{};
        BufferResource materials{};
        BufferResource instances{};
        BufferResource rayRecords{};
        BufferResource frameCounters{};
        BufferResource frameCountersZero{};
        BufferResource rayRecordsReadback{};
        BufferResource frameCountersReadback{};
        lgp::framework::DescriptorAllocation descriptors{};
        bool structuresInitialized{};
        bool topLevelBuilt{};
        std::uint32_t topLevelInstanceCount{};
        bool recordsInitialized{};
        bool countersInitialized{};
    };

    [[nodiscard]] lgp::framework::Status QueryCapability();
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateStateObject();
    [[nodiscard]] lgp::framework::Status ValidateStateObjectDescription();
    [[nodiscard]] lgp::framework::Status CreateFrameSlotResources(lgp::framework::Extent2D size);
    [[nodiscard]] lgp::framework::Status CreateSlotDescriptors(FrameSlotResources &slot);
    [[nodiscard]] lgp::framework::Status UploadShaderTable(FrameSlotResources &slot);
    [[nodiscard]] lgp::framework::Status UploadScene(FrameSlotResources &slot, LabConfiguration const &configuration);
    // Appends one barrier to the frame's published barrier list and returns the D3D12 structure that records it.
    // Every barrier the frame submits goes through here, so the published list is the submitted list rather than a
    // description of it.
    [[nodiscard]] D3D12_BUFFER_BARRIER TrackBufferBarrier(LabResource resource, ID3D12Resource &d3dResource,
                                                          BufferBarrierState before, BufferBarrierState after);
    void RecordCounterReset(ID3D12GraphicsCommandList7 &commandList, FrameSlotResources &slot);
    [[nodiscard]] lgp::framework::Status RecordAccelerationStructures(ID3D12GraphicsCommandList7 &commandList,
                                                                      FrameSlotResources &slot,
                                                                      LabConfiguration const &configuration);
    void RecordAnalyticTraversal(ID3D12GraphicsCommandList7 &commandList, FrameSlotResources &slot,
                                 LabConfiguration const &configuration);
    [[nodiscard]] lgp::framework::Status RecordRayDispatch(ID3D12GraphicsCommandList7 &commandList,
                                                           FrameSlotResources &slot,
                                                           LabConfiguration const &configuration);
    void RecordDisplayAndReadback(lgp::framework::FrameContext const &frameContext, FrameSlotResources &slot,
                                  LabConfiguration const &configuration, std::span<LabStage const> stages);
    void PrintHeadlessEvidence() const noexcept;
    void DestroyFrameSlotResources(lgp::framework::DeviceResources &deviceResources) noexcept;
    [[nodiscard]] LabConfiguration ActiveConfiguration() const noexcept;
    [[nodiscard]] DispatchConstants MakeDispatchConstants(LabConfiguration const &configuration) const noexcept;

    std::filesystem::path shaderPath_{};
    LabVariant variant_{};
    bool headless_{};
    bool hasRendered_{};
    lgp::framework::DeviceResources *deviceResources_{};

    lgp::framework::CompiledShader analyticShader_{};
    lgp::framework::CompiledShader raytracingLibrary_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    std::vector<ShaderArtifact> shaderArtifacts_{};

    Microsoft::WRL::ComPtr<ID3D12RootSignature> globalRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> hitGroupLocalRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> analyticPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12StateObject> stateObject_{};

    // The identifiers are copied out of the state object during initialization, while the state object that owns
    // them is alive, and the copies are what every frame's table is built from.
    std::array<std::byte, kShaderIdentifierSizeBytes> rayGenerationIdentifier_{};
    std::array<std::array<std::byte, kShaderIdentifierSizeBytes>, kMissRecordCount> missIdentifiers_{};
    std::array<std::byte, kShaderIdentifierSizeBytes> hitGroupIdentifier_{};

    std::vector<FrameSlotResources> frameSlots_{};
    lgp::framework::Extent2D size_{};

    RaytracingCapability capability_{};
    StateObjectEvidence stateObjectEvidence_{};
    ShaderTableLayout shaderTableLayout_{};

    std::optional<LabConfiguration> headlessConfiguration_{};
    LabConfiguration interactiveConfiguration_{};
    LabConfiguration lastRenderedConfiguration_{};
    ReferenceFrame currentReference_{};

    AccelerationStructureEvidence lastAccelerationEvidence_{};
    ShaderTableEvidence lastShaderTableEvidence_{};
    std::vector<RecordedBufferBarrier> lastBarriers_{};
    std::vector<BuildStep> lastBuildTimeline_{};
    std::array<StageStatus, kPipelineStageCount> lastLedger_{};
    bool lastLedgerReady_{};
    std::uint32_t lastLedgerEvidence_{};
    std::array<LabStage, kLabStageCount> lastStages_{};
    std::uint32_t lastStageCount_{};
    std::uint64_t lastStageOrderWord_{};
    std::uint32_t lastStageMask_{};
    std::uint32_t lastStageSkippedMask_{};
    std::vector<InstanceRecord> lastInstances_{};
    std::uint32_t lastRenderedFrameSlot_{};
    std::uint64_t lastRayRecordAddress_{};
    std::uint64_t lastTopLevelAddress_{};
    std::uint64_t lastShaderTableAddress_{};
};

} // namespace ch33::dxr::gpu
