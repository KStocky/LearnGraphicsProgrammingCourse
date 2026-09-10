#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Chapter 33 teaching contracts for DXR foundations: acceleration structures, ray dispatch, and the shader table.
//
// Scope and honesty rules for this phase:
//
//   * Everything here is a deterministic CPU model of the decisions a DXR frame normally hides inside a driver: how
//     a triangle geometry description is legal or not, how much memory a build asks for and where it may live, what
//     has to happen between a build and the first read of its result, how an instance is packed into 64 bytes, how
//     a state object's exports become shader-table records, how a record address is selected at trace time, and
//     what a ray/triangle intersection reports. None of it is the D3D12 runtime, and none of it pretends to be.
//   * What D3D12 owns and this chapter only mirrors: the real prebuild sizes, which come from
//     `ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo` and are driver-specific numbers this chapter
//     never invents; the actual traversal order inside `TraceRay`, which is deliberately unspecified; the real
//     shader identifiers, which come from `ID3D12StateObjectProperties::GetShaderIdentifier` and are opaque bytes;
//     the acceleration-structure data layout, which is opaque and non-portable; and every residency, aliasing, and
//     lifetime rule the runtime and the debug layer enforce. This chapter validates *descriptions* of those things.
//   * What this chapter pins, so the lab can be deterministic: the numeric limits copied from `d3d12.h` (each one
//     is documented with the macro it mirrors and is asserted by a test so that a silent edit fails), the record
//     and table alignment arithmetic, a total traversal order for the reference intersector, and the barrier
//     ordering that a build/consume pair must present as evidence.
//   * No D3D12 header is included. The contracts are pure C++23 so that they build, run, and can be reasoned about
//     on any toolchain the course targets, including a machine with no raytracing device. The cost of that choice
//     is that the mirrored constants can drift from the SDK; the mitigation is that every one of them is named,
//     documented with its origin, and pinned by an explicit test rather than being used inline as a literal.
//   * Nothing here silently repairs an invalid input. Every entry point returns `std::expected` with a specific
//     `ContractError`; there are no exceptions, no clamped-to-something-plausible defaults, and no "0 means use the
//     default" parameters. A caller that gets a value back knows every precondition held.
//
// Units, spaces, and sign conventions:
//
//   * Sizes, strides, offsets, and alignments are unsigned byte counts, always named `...Bytes`. Addresses are GPU
//     virtual addresses in bytes, named `...Address`, and an address of zero means "no buffer" everywhere it is
//     legal at all; a zero address is never a valid buffer.
//   * Distances along a ray (`tMin`, `tMax`, `tHit`) are in metres in world space, because the lab's scene is
//     metric and because an interval expressed in the same unit as the scene is the only way a learner can judge
//     whether a self-intersection epsilon is sane. The fixed-function triangle interval is *open*: D3D12 accepts a
//     triangle hit only when `tMin < t < tMax`, so a hit landing exactly on either endpoint is a miss. (Procedural
//     primitives use the closed test `tMin <= t <= tMax`; this chapter models triangles only, and says so rather
//     than letting the difference be discovered on hardware.) See DirectX-Specs `d3d/Raytracing.md`, "Ray extents".
//   * Positions, directions, and normals are world space unless a name says `object`. The instance transform is a
//     3x4 row-major object-to-world matrix, matching `D3D12_RAYTRACING_INSTANCE_DESC::Transform`, with the implied
//     fourth row (0, 0, 0, 1). Direction vectors are not required to be unit length: `tHit` is measured in units of
//     the supplied direction exactly as `TraceRay` measures it, and the reference intersector reports the direction
//     length so a test can prove that.
//   * Front facing follows D3D12, and D3D12 decides it *in object space*: a triangle is front facing when its
//     vertices appear clockwise from the ray origin in object space, in a left-handed coordinate system. The
//     chapter evaluates that as `dot(objectRayDirection, cross(v1 - v0, v2 - v0)) < 0` on the untransformed
//     vertices. `D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE` inverts the test. An instance
//     transform carries the ray into object space but does not rewind those vertices: the chapter's x mirror
//     preserves the ray's z direction and therefore preserves facing, while its z flip reverses the object-space
//     ray direction and facing. A *per-geometry* `Transform3x4` is different because the BLAS build folds it into
//     the vertices in object space, so a negative-determinant geometry transform really does flip winding. See
//     DirectX-Specs `d3d/Raytracing.md`, the description of
//     `D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE`.
//   * Barycentrics use the DXR attribute convention: the reported pair (b1, b2) weights the vertices as
//     v0 * (1 - b1 - b2) + v1 * b1 + v2 * b2, which is what a triangle hit group receives in
//     `BuiltInTriangleIntersectionAttributes`.
//
// Deliberately out of scope, and not approximated here: procedural primitives and intersection shaders beyond the
// state-object rules that keep a procedural hit group well formed; compaction and serialization of acceleration
// structures, which need real driver sizes; inline raytracing (`RayQuery`), which has no shader table at all and
// which a later chapter owns; any-hit shading policy, alpha-tested geometry, and the ordering guarantees an
// any-hit shader must not assume; multi-level instancing, which DXR does not have; and denoising, sampling, or
// shading of the hit, all of which belong to the chapters that already own those subjects.

namespace ch33::dxr
{

// -------------------------------------------------------------------------------------------------------------
// Mirrored D3D12 constants. Each value is copied from `d3d12.h` and named after the macro it mirrors, because a
// literal 32 sitting in an expression is indistinguishable from a literal 32 that means something else. Tests pin
// every one of them, so a mistyped mirror fails loudly instead of producing a table the GPU reads at the wrong
// stride.
// -------------------------------------------------------------------------------------------------------------

// D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES. Every shader-table record begins with exactly this many opaque bytes.
inline constexpr std::uint64_t kShaderIdentifierSizeBytes = 32U;

// D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT. Record strides and record offsets are multiples of this.
inline constexpr std::uint64_t kShaderRecordAlignmentBytes = 32U;

// D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT. Each table section's start address is a multiple of this, which is
// why a section start is *not* simply the end of the previous section.
inline constexpr std::uint64_t kShaderTableAlignmentBytes = 64U;

// D3D12_RAYTRACING_MAX_SHADER_RECORD_STRIDE. A record cannot be wider than this, so local root arguments are a
// bounded budget rather than an unbounded per-record payload.
inline constexpr std::uint64_t kMaximumShaderRecordStrideBytes = 4'096U;

// D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT. Destination, source, and scratch addresses of a build are
// all required to be multiples of this, and prebuild result sizes are reported as multiples of it.
inline constexpr std::uint64_t kAccelerationStructureAlignmentBytes = 256U;

// D3D12_RAYTRACING_INSTANCE_DESC_BYTE_ALIGNMENT. The instance description array consumed by a top-level build.
inline constexpr std::uint64_t kInstanceDescriptionAlignmentBytes = 16U;

// sizeof(D3D12_RAYTRACING_INSTANCE_DESC): 12 floats of transform, two packed 32-bit words, and a 64-bit address.
inline constexpr std::uint64_t kInstanceDescriptionSizeBytes = 64U;

// D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT. The optional per-geometry transform buffer in a triangle geometry.
inline constexpr std::uint64_t kTransform3x4AlignmentBytes = 16U;

// D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES. The hit attribute structure an intersection shader may pass on.
inline constexpr std::uint32_t kMaximumAttributeSizeBytes = 32U;

// sizeof(BuiltInTriangleIntersectionAttributes): two 32-bit barycentrics. A triangle hit group cannot declare a
// smaller attribute size than the fixed-function triangle intersector already produces.
inline constexpr std::uint32_t kTriangleAttributeSizeBytes = 8U;

// D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH.
inline constexpr std::uint32_t kMaximumTraceRecursionDepth = 31U;

// D3D12_RAYTRACING_MAX_RAY_GENERATION_SHADER_THREADS: the product of the three DispatchRays dimensions.
inline constexpr std::uint64_t kMaximumRayGenerationThreads = 1ULL << 30U;

// D3D12_RAYTRACING_MAX_GEOMETRIES_PER_BOTTOM_LEVEL_ACCELERATION_STRUCTURE.
inline constexpr std::uint64_t kMaximumGeometriesPerBottomLevel = 1ULL << 24U;

// D3D12_RAYTRACING_MAX_INSTANCES_PER_TOP_LEVEL_ACCELERATION_STRUCTURE.
inline constexpr std::uint64_t kMaximumInstancesPerTopLevel = 1ULL << 24U;

// D3D12_RAYTRACING_MAX_PRIMITIVES_PER_BOTTOM_LEVEL_ACCELERATION_STRUCTURE.
inline constexpr std::uint64_t kMaximumPrimitivesPerBottomLevel = 1ULL << 29U;

// The bit widths of the packed fields of D3D12_RAYTRACING_INSTANCE_DESC. They are separate constants because the
// two 24-bit fields mean completely different things and are packed against different neighbours.
inline constexpr std::uint32_t kInstanceIdBitCount = 24U;
inline constexpr std::uint32_t kInstanceMaskBitCount = 8U;
inline constexpr std::uint32_t kInstanceContributionBitCount = 24U;
inline constexpr std::uint32_t kInstanceFlagsBitCount = 8U;

inline constexpr std::uint32_t kMaximumInstanceId = (1U << kInstanceIdBitCount) - 1U;
inline constexpr std::uint32_t kMaximumInstanceMask = (1U << kInstanceMaskBitCount) - 1U;
inline constexpr std::uint32_t kMaximumInstanceContribution = (1U << kInstanceContributionBitCount) - 1U;

// TraceRay's RayContributionToHitGroupIndex and MultiplierForGeometryContributionToHitGroupIndex are documented as
// 4-bit values, and MissShaderIndex as a 16-bit value. Passing a wider value is a silent aliasing bug at trace
// time, which is exactly the class of mistake these contracts exist to catch on the CPU.
inline constexpr std::uint32_t kMaximumRayContributionToHitGroupIndex = 15U;
inline constexpr std::uint32_t kMaximumGeometryContributionMultiplier = 15U;
inline constexpr std::uint32_t kMaximumMissShaderIndex = 65'535U;

// GeometryContributionToHitGroupIndex is the geometry's index inside its bottom-level structure, so it is bounded
// by the geometry count rather than by a separate field width.
inline constexpr std::uint32_t kMaximumGeometryContribution =
    static_cast<std::uint32_t>(kMaximumGeometriesPerBottomLevel - 1ULL);

// -------------------------------------------------------------------------------------------------------------
// Chapter budgets. These are *not* D3D12 limits. They exist so the lab stays inspectable and so a runaway value
// fails on the CPU with a name instead of failing on the GPU with a device removal.
// -------------------------------------------------------------------------------------------------------------

// D3D12 does not define a maximum ray payload size; the practical limit is register pressure and it varies by
// hardware. The chapter caps it because a payload this size already forces the interesting conversation about what
// belongs in a payload, and because an unbounded payload makes the lab's occupancy numbers meaningless.
inline constexpr std::uint32_t kMaximumPayloadSizeBytes = 128U;

// A payload field's alignment cannot exceed the alignment of the widest scalar the lab uses (a 16-byte vector).
inline constexpr std::uint32_t kMaximumPayloadFieldAlignmentBytes = 16U;

// The largest shader table the lab builds. Real applications go far past this; the bound exists so that overflow
// tests have something concrete to overflow and so that a mistaken record count is caught before it becomes a
// multi-gigabyte upload.
inline constexpr std::uint64_t kMaximumShaderTableSizeBytes = 16ULL * 1'024ULL * 1'024ULL;

// The reference intersector's scene bound. A CPU reference is a teaching instrument, not a renderer.
inline constexpr std::uint64_t kMaximumReferenceInstanceCount = 1'024U;
inline constexpr std::uint64_t kMaximumReferenceTrianglesPerGeometry = 4'096U;
inline constexpr std::uint64_t kMaximumReferenceGeometriesPerInstance = 64U;

// The largest ray interval the lab accepts, in metres. A `tMax` of infinity is the usual shorthand for "as far as
// the scene goes"; this chapter refuses it, because an interval that cannot be compared is an interval whose
// endpoint behaviour cannot be taught.
inline constexpr double kMaximumRayDistanceMetres = 1.0e6;

// The absolute area, in squared world units, below which a triangle is rejected as degenerate rather than being
// intersected. A zero-area triangle has no plane and therefore no barycentrics; producing "a hit" for one is how a
// reference intersector quietly disagrees with hardware.
inline constexpr double kMinimumTriangleArea = 1.0e-12;

// The magnitude of the Moller-Trumbore determinant below which the ray is treated as parallel to the triangle's
// plane and the candidate is rejected. It is a rejection threshold, not a fudge factor applied to a hit.
inline constexpr double kParallelRayDeterminantEpsilon = 1.0e-14;

// The smallest 3x3 determinant an instance transform may have. A singular transform collapses the instance to a
// plane or a point, which no amount of downstream care can intersect meaningfully.
inline constexpr double kMinimumInstanceTransformDeterminant = 1.0e-9;

// The shortest ray direction the chapter accepts. `TraceRay` measures `t` in units of the supplied direction, so a
// direction this short makes every reported distance meaningless long before it becomes exactly zero.
inline constexpr double kMinimumRayDirectionLength = 1.0e-9;

// How close a direction's length must be to one before it is reported as unit length. The reference intersector
// works with any length; the flag exists so a test can say which case it is exercising.
inline constexpr double kUnitLengthTolerance = 1.0e-12;

// -------------------------------------------------------------------------------------------------------------
// Errors and stages
// -------------------------------------------------------------------------------------------------------------

// The stage of the DXR frame a failure belongs to. The value exists so that a diagnostic can say "the top-level
// build is wrong" rather than "raytracing failed": the five stages have different owners, different fix-up costs,
// and different evidence, and collapsing them is what makes DXR bring-up feel unfixable.
enum class PipelineStage : std::uint8_t
{
    BottomLevelBuild = 0U,
    TopLevelBuild,
    StateObject,
    ShaderTable,
    Dispatch,
};

inline constexpr std::size_t kPipelineStageCount = 5U;

// Which of the two acceleration-structure levels a description, build, or failure belongs to. It is declared here
// because the diagnostics below need it: a sizing failure is only attributable once the caller says which level it
// was building.
enum class AccelerationStructureKind : std::uint8_t
{
    BottomLevel = 0U,
    TopLevel,
};

// Whether a build creates a structure from scratch or refits an existing one. D3D12 spells the refit as the
// `PERFORM_UPDATE` build flag; separating it from the flag set is what makes "updated without ALLOW_UPDATE" and
// "updated with different flags than the original build" two distinct, nameable failures.
enum class BuildMode : std::uint8_t
{
    Build = 0U,
    Update,
};

// Every way a contract in this chapter can refuse an input. The enumeration is deliberately fine grained: a single
// `InvalidArgument` would be cheaper to write and would teach nothing, because the whole point of validating a DXR
// description on the CPU is to name the field that is wrong.
enum class ContractError : std::uint16_t
{
    // Shared numeric failures.
    NonFinite = 0U,
    ArithmeticOverflow,
    InvalidAlignment,

    // Triangle geometry description.
    UnsupportedVertexFormat,
    UnsupportedIndexFormat,
    ZeroVertexCount,
    VertexCountNotTriangleMultiple,
    VertexStrideTooSmall,
    VertexStrideMisaligned,
    VertexBufferAddressNull,
    VertexBufferAddressMisaligned,
    IndexedGeometryMissingIndexCount,
    IndexCountNotTriangleMultiple,
    IndexBufferAddressNull,
    IndexBufferAddressMisaligned,
    NonIndexedGeometryDeclaresIndexBuffer,
    TransformAddressMisaligned,
    ConflictingGeometryFlags,
    PrimitiveCountExceeded,
    GeometryCountExceeded,
    EmptyGeometrySet,

    // Prebuild sizing and build requests.
    ZeroResultSize,
    ResultSizeMisaligned,
    ZeroScratchSize,
    UnexpectedUpdateScratchSize,
    MissingUpdateScratchSize,
    DestinationAddressNull,
    DestinationAddressMisaligned,
    DestinationTooSmall,
    ScratchAddressNull,
    ScratchAddressMisaligned,
    ScratchTooSmall,
    SourceRequiredForUpdate,
    UnexpectedSourceForBuild,
    SourceAddressMisaligned,
    SourceTooSmall,
    UpdateWithoutAllowUpdate,
    UpdateFlagsMismatch,
    UpdateTopologyMismatch,
    ConflictingBuildFlags,
    OverlappingBuildRanges,
    ZeroBuildElementCount,

    // Build/consume ordering evidence.
    EmptyTimeline,
    InvalidResourceIdentifier,
    ConsumesUnbuiltAccelerationStructure,
    MissingProducerBarrier,
    MissingScratchBarrier,
    WrongAccelerationStructureKindConsumed,
    DispatchWithoutTopLevel,
    RebuildWithoutConsumerBarrier,

    // Instances and top-level packing.
    InstanceIdOutOfRange,
    InstanceMaskOutOfRange,
    InstanceMaskSelectsNothing,
    InstanceContributionOutOfRange,
    ConflictingInstanceFlags,
    BottomLevelAddressNull,
    BottomLevelAddressMisaligned,
    SingularInstanceTransform,
    DuplicateInstanceIdentity,
    InstanceCountExceeded,
    EmptyInstanceSet,
    InstanceBufferAddressNull,
    InstanceBufferAddressMisaligned,
    InstanceBufferTooSmall,

    // Rays.
    NegativeRayTMin,
    InvertedRayInterval,
    RayIntervalTooLarge,
    ZeroLengthRayDirection,
    ConflictingRayFlags,
    RayMaskOutOfRange,
    RayMaskSelectsNothing,

    // State object.
    EmptyExportName,
    DuplicateExportName,
    MissingRayGenerationShader,
    MissingMissShader,
    HitGroupMissingShaders,
    TriangleHitGroupDeclaresIntersection,
    ProceduralHitGroupMissingIntersection,
    HitGroupReferencesUnknownExport,
    HitGroupExportStageMismatch,
    InvalidPayloadSize,
    InvalidAttributeSize,
    InvalidTraceRecursionDepth,
    DuplicateLocalRootSignatureName,
    UnknownLocalRootSignature,
    AssociationReferencesUnknownExport,
    ConflictingLocalRootSignatureAssociation,
    MismatchedHitGroupLocalRootSignature,
    InvalidLocalRootArgumentSize,
    ShaderRecordStrideExceeded,

    // Payload ABI.
    EmptyPayloadLayout,
    EmptyPayloadFieldName,
    DuplicatePayloadFieldName,
    InvalidPayloadFieldSize,
    InvalidPayloadFieldAlignment,
    ZeroPayloadElementCount,
    PayloadSizeExceeded,

    // Shader identifiers and table layout.
    InvalidShaderIdentifierSize,
    ZeroShaderIdentifier,
    EmptyShaderTable,
    EmptyShaderTableSection,
    DuplicateShaderTableSection,
    RayGenerationSectionMustHoldOneRecord,
    ShaderTableSizeExceeded,
    ShaderTableSectionMissing,
    TableBaseAddressNull,
    TableBaseAddressMisaligned,

    // Trace-time record selection and dispatch.
    RayContributionOutOfRange,
    GeometryContributionMultiplierOutOfRange,
    GeometryContributionOutOfRange,
    HitGroupRecordIndexOutOfRange,
    MissShaderIndexOutOfRange,
    MissRecordIndexOutOfRange,
    CallableRecordIndexOutOfRange,
    ZeroDispatchDimension,
    DispatchThreadCountExceeded,

    // Reference intersection scene description.
    EmptyReferenceGeometry,
    ReferenceSceneTooLarge,
    DegenerateTriangle,

    // Stage status ledger.
    StageOutOfOrder,
    StageAlreadyRecorded,
    MissingStageEvidence,
    StageAfterFailure,
};

// Every enumerator, in declaration order. Diagnostics tests iterate this to prove that the name table and the
// stage mapping are total: a new error added without a name or a stage is a compile-time-silent, run-time-loud
// omission otherwise.
[[nodiscard]] std::span<ContractError const> AllContractErrors() noexcept;

// A stable, unique, human-readable name. Diagnostics print it; nothing parses it.
[[nodiscard]] std::string_view ContractErrorName(ContractError error) noexcept;

[[nodiscard]] std::string_view PipelineStageName(PipelineStage stage) noexcept;

// The stage a failure intrinsically belongs to, when it has one. Most errors do: a misaligned index buffer can only
// come from a bottom-level build, an ambiguous local root signature association can only come from a state object.
// Some do not, and saying otherwise would be a lie the diagnostics then repeat: `ArithmeticOverflow`, the shared
// numeric failures, the build sizing and ordering errors, and the ledger's own errors all occur at more than one
// stage. Those return `std::nullopt` and are tagged with the stage that reported them instead.
[[nodiscard]] std::optional<PipelineStage> StageForError(ContractError error) noexcept;

// The stage a build of the given kind belongs to. It exists so a caller validating a build request can tag a
// stage-agnostic sizing failure with the structure it was building.
[[nodiscard]] PipelineStage StageForBuild(AccelerationStructureKind kind) noexcept;

// One line of stage-attributed evidence: the stage the failure is attributed to, the error, and the two names a
// status line prints. The attributed stage is the error's intrinsic stage when it has one and the reporting stage
// otherwise, so a caller cannot mislabel a misaligned index buffer as a shader-table problem.
struct StageDiagnostic final
{
    PipelineStage stage{PipelineStage::BottomLevelBuild};
    ContractError error{ContractError::NonFinite};
    std::string_view stageName{};
    std::string_view errorName{};
    // True when the error named its own stage, false when the reporting stage supplied it.
    bool stageIsIntrinsic{};

    [[nodiscard]] bool operator==(StageDiagnostic const &) const noexcept = default;
};

[[nodiscard]] StageDiagnostic MakeStageDiagnostic(PipelineStage reportingStage, ContractError error) noexcept;

// -------------------------------------------------------------------------------------------------------------
// Small shared vocabulary
// -------------------------------------------------------------------------------------------------------------

struct Float2 final
{
    double x{};
    double y{};

    [[nodiscard]] bool operator==(Float2 const &) const noexcept = default;
};

struct Float3 final
{
    double x{};
    double y{};
    double z{};

    [[nodiscard]] bool operator==(Float3 const &) const noexcept = default;
};

// A half-open GPU buffer range, [address, address + sizeBytes). A zero address means "absent"; a zero size with a
// non-zero address is a range that cannot hold anything and is refused wherever it is used.
struct BufferRange final
{
    std::uint64_t address{};
    std::uint64_t sizeBytes{};

    [[nodiscard]] bool operator==(BufferRange const &) const noexcept = default;
};

// The object-to-world matrix D3D12 stores in an instance description: three rows of four, row major, with the
// fourth row implied to be (0, 0, 0, 1). Storing it as doubles rather than floats is deliberate: the reference
// intersector is the thing tests compare against, so it should not inherit the float rounding of the GPU copy.
struct Transform3x4 final
{
    std::array<double, 12U> values{};

    [[nodiscard]] bool operator==(Transform3x4 const &) const noexcept = default;
};

[[nodiscard]] Transform3x4 IdentityTransform() noexcept;
[[nodiscard]] Transform3x4 TranslationTransform(Float3 translationMetres) noexcept;
[[nodiscard]] Transform3x4 ScaleTransform(Float3 scale) noexcept;

// The determinant of the upper-left 3x3 block. Its magnitude says whether the instance is invertible and its sign
// says whether the instance mirrors. Mirroring does *not* flip triangle facing, which D3D12 evaluates in object
// space, but it does flip the world-space geometric normal, so the sign is exactly the factor the reference
// intersector divides out to recover the object-space facing test.
[[nodiscard]] double TransformDeterminant(Transform3x4 const &transform) noexcept;

// Applies the full 3x4 transform, translation included.
[[nodiscard]] Float3 TransformPoint(Transform3x4 const &transform, Float3 point) noexcept;

// Applies only the 3x3 block, which is what a direction needs.
[[nodiscard]] Float3 TransformDirection(Transform3x4 const &transform, Float3 direction) noexcept;

// Rounds `value` up to a multiple of `alignment`. The alignment must be a non-zero power of two, and the rounded
// value must fit in 64 bits: both failures are reported instead of wrapping, because every table offset in this
// chapter is computed through this function and a wrapped offset is a GPU read at a wild address.
[[nodiscard]] std::expected<std::uint64_t, ContractError> AlignUp(std::uint64_t value,
                                                                  std::uint64_t alignment) noexcept;

[[nodiscard]] bool IsAligned(std::uint64_t value, std::uint64_t alignment) noexcept;

// -------------------------------------------------------------------------------------------------------------
// Flag enumerations. The operators are provided through one opt-in trait so that every flag type behaves the same
// way and so that an unrelated enumeration cannot accidentally acquire bitwise operators.
// -------------------------------------------------------------------------------------------------------------

template <typename FlagsT> struct IsDxrFlagEnum : std::false_type
{
};

template <typename FlagsT>
concept DxrFlagEnum = IsDxrFlagEnum<FlagsT>::value;

template <DxrFlagEnum FlagsT> [[nodiscard]] constexpr FlagsT operator|(FlagsT left, FlagsT right) noexcept
{
    using Underlying = std::underlying_type_t<FlagsT>;
    return static_cast<FlagsT>(static_cast<Underlying>(left) | static_cast<Underlying>(right));
}

template <DxrFlagEnum FlagsT> [[nodiscard]] constexpr FlagsT operator&(FlagsT left, FlagsT right) noexcept
{
    using Underlying = std::underlying_type_t<FlagsT>;
    return static_cast<FlagsT>(static_cast<Underlying>(left) & static_cast<Underlying>(right));
}

template <DxrFlagEnum FlagsT> [[nodiscard]] constexpr bool HasFlag(FlagsT value, FlagsT flag) noexcept
{
    using Underlying = std::underlying_type_t<FlagsT>;
    return (static_cast<Underlying>(value) & static_cast<Underlying>(flag)) == static_cast<Underlying>(flag) &&
           static_cast<Underlying>(flag) != Underlying{0};
}

template <DxrFlagEnum FlagsT> [[nodiscard]] constexpr bool HasAnyFlag(FlagsT value, FlagsT mask) noexcept
{
    using Underlying = std::underlying_type_t<FlagsT>;
    return (static_cast<Underlying>(value) & static_cast<Underlying>(mask)) != Underlying{0};
}

// D3D12_RAYTRACING_GEOMETRY_FLAGS. `Opaque` means the fixed-function traversal may skip any-hit invocation
// entirely; `NoDuplicateAnyHitInvocation` promises that any-hit runs at most once per primitive per ray.
//
// The two are *not* mutually exclusive. D3D12 documents no such restriction, and declaring both is legal: the
// second flag simply has nothing left to constrain once the first has removed any-hit invocation altogether. The
// validation therefore accepts the pair and reports the redundancy as evidence rather than refusing a legal
// description. Refusing it would have been this chapter inventing a rule and blaming D3D12 for it.
enum class GeometryFlags : std::uint8_t
{
    None = 0U,
    Opaque = 0x1U,
    NoDuplicateAnyHitInvocation = 0x2U,
};

template <> struct IsDxrFlagEnum<GeometryFlags> : std::true_type
{
};

// D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS, minus `PERFORM_UPDATE`, which this chapter expresses as a
// `BuildMode` instead. Keeping the mode out of the flag set is what makes "update without ALLOW_UPDATE" and "update
// whose flags differ from the build's flags" two distinguishable errors.
enum class BuildFlags : std::uint8_t
{
    None = 0U,
    AllowUpdate = 0x1U,
    AllowCompaction = 0x2U,
    PreferFastTrace = 0x4U,
    PreferFastBuild = 0x8U,
    MinimizeMemory = 0x10U,
};

template <> struct IsDxrFlagEnum<BuildFlags> : std::true_type
{
};

// D3D12_RAYTRACING_INSTANCE_FLAGS.
enum class InstanceFlags : std::uint8_t
{
    None = 0U,
    TriangleCullDisable = 0x1U,
    TriangleFrontCounterclockwise = 0x2U,
    ForceOpaque = 0x4U,
    ForceNonOpaque = 0x8U,
};

template <> struct IsDxrFlagEnum<InstanceFlags> : std::true_type
{
};

// The RAY_FLAG values a triangle-only chapter can act on. `SkipProceduralPrimitives` is included because DXR
// declares it mutually exclusive with `SkipTriangles`, and because that pair is the canonical "traversal that can
// never hit anything" mistake.
//
// DXR states four mutual-exclusion groups for these flags, and `ValidateRayFlags` enforces all of them:
//   * `ForceOpaque` excludes `ForceNonOpaque`, `CullOpaque`, and `CullNonOpaque`.
//   * `ForceNonOpaque` excludes `ForceOpaque`, `CullOpaque`, and `CullNonOpaque`.
//   * `CullOpaque` excludes `CullNonOpaque` (and both force flags, per the two rules above).
//   * `CullBackFacingTriangles` excludes `CullFrontFacingTriangles`.
//   * `SkipTriangles` excludes `CullFrontFacingTriangles`, `CullBackFacingTriangles`, and
//     `SkipProceduralPrimitives`.
// See DirectX-Specs `d3d/Raytracing.md`, "Ray flags". None of these are chapter policy; every one is quoted from
// the flag's own documentation.
enum class RayFlags : std::uint16_t
{
    None = 0U,
    ForceOpaque = 0x1U,
    ForceNonOpaque = 0x2U,
    AcceptFirstHitAndEndSearch = 0x4U,
    SkipClosestHitShader = 0x8U,
    CullBackFacingTriangles = 0x10U,
    CullFrontFacingTriangles = 0x20U,
    CullOpaque = 0x40U,
    CullNonOpaque = 0x80U,
    SkipTriangles = 0x100U,
    SkipProceduralPrimitives = 0x200U,
};

template <> struct IsDxrFlagEnum<RayFlags> : std::true_type
{
};

// -------------------------------------------------------------------------------------------------------------
// A. Triangle geometry description
//
// A bottom-level triangle geometry is the smallest DXR object a learner can get wrong in a way the debug layer
// reports as an unhelpful E_INVALIDARG. The rules below are the ones D3D12 enforces on
// `D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC`, expressed as data so that a description can be checked before a
// device exists.
// -------------------------------------------------------------------------------------------------------------

// The vertex position formats DXR accepts, plus one deliberately unsupported member. `R32G32B32A32Float` is a
// legal DXGI format and an illegal DXR vertex position format; naming it here means a test can prove the
// validation rejects a *plausible* format rather than only rejecting garbage.
enum class VertexFormat : std::uint8_t
{
    R32G32B32Float = 0U,
    R32G32Float,
    R16G16B16A16Float,
    R16G16Float,
    R16G16B16A16Snorm,
    R16G16Snorm,
    R32G32B32A32Float,
};

// The index formats DXR accepts. `None` is the non-indexed case and is not a substitute for "unknown".
enum class IndexFormat : std::uint8_t
{
    None = 0U,
    R16Uint,
    R32Uint,
};

// The size of one component of the position format. Vertex buffer addresses and strides are required to be
// multiples of this value, which is why it is a separate query from the whole-vertex size.
[[nodiscard]] std::expected<std::uint32_t, ContractError> VertexFormatComponentSizeBytes(VertexFormat format) noexcept;

// The size of one DXGI element in the format, that is, what `sizeof` a fully populated element would be. This is
// *not* what a build reads: for the four-component 16-bit formats DXR ignores the A component entirely, so the
// element is 8 bytes wide while the position it carries is only 6.
[[nodiscard]] std::expected<std::uint32_t, ContractError> VertexFormatElementSizeBytes(VertexFormat format) noexcept;

// The number of bytes of a vertex the build actually consumes, and therefore the smallest legal vertex stride and
// the number of bytes the last vertex contributes to the buffer footprint.
//
// DXR spells this out for the ignored-alpha formats: `DXGI_FORMAT_R16G16B16A16_FLOAT` and
// `..._SNORM` say "A16 component is ignored, other data can be packed there, such as setting vertex stride to 6
// bytes" (DirectX-Specs `d3d/Raytracing.md`, `D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC::VertexFormat`). A stride of
// 6 with an 8-byte element is therefore legal and common, and treating the element size as the minimum stride
// would reject a description D3D12 accepts.
[[nodiscard]] std::expected<std::uint32_t, ContractError> VertexFormatPositionSizeBytes(VertexFormat format) noexcept;

[[nodiscard]] std::expected<std::uint32_t, ContractError> IndexFormatSizeBytes(IndexFormat format) noexcept;

struct TriangleGeometryDescription final
{
    VertexFormat vertexFormat{VertexFormat::R32G32B32Float};
    std::uint64_t vertexBufferAddress{};
    std::uint64_t vertexStrideBytes{};
    std::uint32_t vertexCount{};
    IndexFormat indexFormat{IndexFormat::None};
    std::uint64_t indexBufferAddress{};
    std::uint32_t indexCount{};
    // Optional per-geometry object-space transform, applied at build time. Zero means "no transform", which is not
    // the same as an identity transform: a transform buffer costs build-time work and pins the geometry to the
    // transform's values at build time.
    std::uint64_t transform3x4Address{};
    GeometryFlags flags{GeometryFlags::None};
};

struct TriangleGeometryValidation final
{
    std::uint32_t primitiveCount{};
    bool indexed{};
    bool opaque{};
    // True when a hit on this geometry can invoke an any-hit shader, that is, when the geometry is not opaque.
    // Instance flags and ray flags can still force opacity later; this is the geometry's own contribution.
    bool anyHitCanRun{};
    bool duplicateAnyHitSuppressed{};
    // True when `NoDuplicateAnyHitInvocation` was declared alongside `Opaque`. The combination is legal and has no
    // effect, because an opaque geometry runs no any-hit shader to deduplicate. It is reported so that a lesson can
    // point at a flag that does nothing instead of pretending it was an error.
    bool duplicateAnyHitSuppressionRedundant{};
    bool hasBuildTimeTransform{};
    // The half-open byte span the build reads from each buffer, computed with the declared stride so that the last
    // vertex contributes only the position bytes the build consumes rather than a whole stride or a whole element.
    std::uint64_t vertexBufferFootprintBytes{};
    std::uint64_t indexBufferFootprintBytes{};

    [[nodiscard]] bool operator==(TriangleGeometryValidation const &) const noexcept = default;
};

[[nodiscard]] std::expected<TriangleGeometryValidation, ContractError> ValidateTriangleGeometry(
    TriangleGeometryDescription const &geometry) noexcept;

struct BottomLevelGeometrySetValidation final
{
    std::uint32_t geometryCount{};
    std::uint64_t totalPrimitiveCount{};
    // The largest GeometryContributionToHitGroupIndex a trace against this structure can produce, which is the
    // geometry count minus one. It is reported because it is one of the three terms of the hit-group record index.
    std::uint32_t maximumGeometryContribution{};
    bool anyGeometryAllowsAnyHit{};

    [[nodiscard]] bool operator==(BottomLevelGeometrySetValidation const &) const noexcept = default;
};

// Validates the geometries of one bottom-level structure as a set: individually legal geometries can still exceed
// the per-structure geometry and primitive limits.
[[nodiscard]] std::expected<BottomLevelGeometrySetValidation, ContractError> ValidateBottomLevelGeometrySet(
    std::span<TriangleGeometryDescription const> geometries);

// -------------------------------------------------------------------------------------------------------------
// B. Build sizing, alignment, and update rules
// -------------------------------------------------------------------------------------------------------------

// The three sizes `GetRaytracingAccelerationStructurePrebuildInfo` reports. This chapter never computes them; it
// only checks that a reported triple is self-consistent and that the buffers a build was given satisfy it.
struct PrebuildInfo final
{
    std::uint64_t resultDataMaxSizeBytes{};
    std::uint64_t scratchDataSizeBytes{};
    // Zero unless the build declared `AllowUpdate`. D3D12 reports zero in that case, so a non-zero value paired
    // with a non-updatable build is a sign the caller queried with different flags than it built with.
    std::uint64_t updateScratchDataSizeBytes{};

    [[nodiscard]] bool operator==(PrebuildInfo const &) const noexcept = default;
};

[[nodiscard]] std::expected<void, ContractError> ValidatePrebuildInfo(BuildFlags flags,
                                                                      PrebuildInfo const &prebuild) noexcept;

struct AccelerationStructureBuildRequest final
{
    AccelerationStructureKind kind{AccelerationStructureKind::BottomLevel};
    BuildMode mode{BuildMode::Build};
    // The flags this build declares. For an update, DXR requires every flag *except* `AllowUpdate` to match the
    // flags the source structure was built with, because the driver chose its data layout from them. `AllowUpdate`
    // itself may be dropped, which declares the result final and forbids any further update from it.
    BuildFlags flags{BuildFlags::None};
    PrebuildInfo prebuild{};
    BufferRange destination{};
    BufferRange scratch{};
    // Present only for an update. An update may be in place, in which case the source and destination addresses are
    // equal and the ranges must match exactly.
    BufferRange source{};
    // Geometry count for a bottom-level build, instance count for a top-level build.
    std::uint32_t elementCount{};
    // The count and flags the source structure was built with. Ignored for `BuildMode::Build`.
    std::uint32_t sourceElementCount{};
    BuildFlags sourceFlags{BuildFlags::None};
};

struct AccelerationStructureBuildPlan final
{
    std::uint64_t requiredResultBytes{};
    std::uint64_t requiredScratchBytes{};
    bool inPlaceUpdate{};
    // Whether the *result* of this build or update can itself be updated later, that is, whether the request
    // declared `AllowUpdate`.
    bool updateAllowed{};
    // True for an update that deliberately drops `AllowUpdate`: the last update of a structure's life. DXR allows
    // exactly this transition and no other change to the `ALLOW_*_UPDATE` set, so it is worth naming rather than
    // leaving the caller to infer it from two flag words.
    bool finalUpdate{};
    bool compactionAllowed{};
    // The address one byte past the destination's required result data. A subsequent suballocation must start at or
    // after this address, rounded up to the acceleration-structure alignment.
    std::uint64_t destinationEndAddress{};

    [[nodiscard]] bool operator==(AccelerationStructureBuildPlan const &) const noexcept = default;
};

// Validates one `BuildRaytracingAccelerationStructure` call's memory and permission preconditions: alignments,
// capacities, the update permission chain, and the requirement that scratch never overlaps the structures it
// serves. It does not and cannot validate residency, resource state, or the opaque contents of a source structure.
//
// The update permission chain is DXR's, quoted rather than invented (DirectX-Specs `d3d/Raytracing.md`,
// `D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE`):
//   * "The source acceleration structure must have specified `ALLOW_UPDATE`." So the permission is a property of
//     `sourceFlags`, not of the request's own flags.
//   * "The other flags selections, aside from `ALLOW_UPDATE` and `PERFORM_UPDATE`, must match the flags in the
//     source acceleration structure." So `AllowUpdate` is excluded from the comparison and everything else is not.
//   * "…if opting to continue allowing future updates, the build specifies the same combination of `ALLOW_*_UPDATE`
//     flags (set can't change other than setting none if updates are forever finished)." So dropping `AllowUpdate`
//     is legal and is reported as `finalUpdate`; there is no way to add it back.
//
// One rule here is the chapter's own and is labelled as such: the element count may not change across an update.
// DXR's update constraints are looser in places, and the conservative reading is what the lab's evidence needs.
[[nodiscard]] std::expected<AccelerationStructureBuildPlan, ContractError> ValidateBuildRequest(
    AccelerationStructureBuildRequest const &request) noexcept;

// -------------------------------------------------------------------------------------------------------------
// C. Producer/consumer ordering evidence
//
// A build writes a UAV; every later read of that structure, whether by another build or by a dispatch, needs a UAV
// barrier on it first. The same is true of a scratch buffer shared by two builds. D3D12 will not tell you when you
// forgot: the result is a race whose symptom is an intermittently wrong image. The timeline below is the CPU-side
// evidence that the ordering exists.
// -------------------------------------------------------------------------------------------------------------

enum class BuildStepKind : std::uint8_t
{
    BuildBottomLevel = 0U,
    BuildTopLevel,
    UavBarrier,
    DispatchRays,
};

struct BuildStep final
{
    BuildStepKind kind{BuildStepKind::BuildBottomLevel};
    // The structure being built, or the resource the barrier covers. Unused by `DispatchRays`.
    std::uint64_t resourceId{};
    // The scratch buffer a build consumes. Unused by the other kinds.
    std::uint64_t scratchId{};
    // The structures this step reads: the bottom-level structures a top-level build references, or the top-level
    // structure a dispatch traverses.
    std::vector<std::uint64_t> inputs{};
};

struct BuildTimelineValidation final
{
    std::uint32_t bottomLevelBuildCount{};
    std::uint32_t topLevelBuildCount{};
    std::uint32_t barrierCount{};
    // Barriers that covered a resource nothing had written since the previous barrier. They are legal and wasteful,
    // and counting them is how the lab can show that "barrier after every command" is not the same as correct.
    std::uint32_t redundantBarrierCount{};
    std::uint32_t dispatchCount{};

    [[nodiscard]] bool operator==(BuildTimelineValidation const &) const noexcept = default;
};

// Replays a recorded command sequence and proves that every consumer of a build result is separated from its
// producer by a UAV barrier on that resource, that two builds sharing scratch are separated by a barrier on the
// scratch, and that nothing consumes a structure that was never built or is of the wrong kind.
[[nodiscard]] std::expected<BuildTimelineValidation, ContractError> ValidateBuildTimeline(
    std::span<BuildStep const> steps);

// -------------------------------------------------------------------------------------------------------------
// D. Instances and top-level packing
// -------------------------------------------------------------------------------------------------------------

struct InstanceDescription final
{
    Transform3x4 objectToWorld{IdentityTransform()};
    // 24 bits, readable in a shader as `InstanceID()`. It is the application's own identifier and means nothing to
    // the traversal.
    std::uint32_t instanceId{};
    // 8 bits, intersected with the ray's inclusion mask. Zero means the instance is invisible to every ray.
    std::uint32_t instanceMask{kMaximumInstanceMask};
    // 24 bits, the per-instance term of the hit-group record index.
    std::uint32_t instanceContributionToHitGroupIndex{};
    InstanceFlags flags{InstanceFlags::None};
    std::uint64_t bottomLevelAddress{};
};

// The two packed 32-bit words of `D3D12_RAYTRACING_INSTANCE_DESC`. Modelling the packing explicitly is the point:
// the fields are bitfields in the real structure, and a value that silently loses its top bits is the difference
// between "the wrong shader ran" and "no shader ran".
struct PackedInstanceHeader final
{
    // Low 24 bits: instance id. High 8 bits: instance mask.
    std::uint32_t instanceIdAndMask{};
    // Low 24 bits: hit-group contribution. High 8 bits: instance flags.
    std::uint32_t instanceContributionAndFlags{};

    [[nodiscard]] bool operator==(PackedInstanceHeader const &) const noexcept = default;
};

struct UnpackedInstanceHeader final
{
    std::uint32_t instanceId{};
    std::uint32_t instanceMask{};
    std::uint32_t instanceContributionToHitGroupIndex{};
    InstanceFlags flags{InstanceFlags::None};

    [[nodiscard]] bool operator==(UnpackedInstanceHeader const &) const noexcept = default;
};

[[nodiscard]] std::expected<PackedInstanceHeader, ContractError> PackInstanceHeader(
    InstanceDescription const &instance) noexcept;

// The exact inverse of `PackInstanceHeader` for any packed value it produced. It cannot fail: every bit pattern of
// the two words decodes to some field combination, which is precisely why the packing side has to validate.
[[nodiscard]] UnpackedInstanceHeader UnpackInstanceHeader(PackedInstanceHeader const &packed) noexcept;

[[nodiscard]] std::expected<void, ContractError> ValidateInstance(InstanceDescription const &instance) noexcept;

struct TopLevelPacking final
{
    std::uint32_t instanceCount{};
    std::uint64_t instanceBufferSizeBytes{};
    // The union of every instance mask. A ray whose inclusion mask does not intersect this can never hit anything
    // in the structure, which is a cheap CPU-side answer to "why is my image empty".
    std::uint32_t maskUnion{};
    std::uint32_t distinctBottomLevelCount{};
    std::uint32_t maximumInstanceContribution{};

    [[nodiscard]] bool operator==(TopLevelPacking const &) const noexcept = default;
};

// Validates the instance array of one top-level build. Beyond D3D12's own field limits, this chapter requires that
// instance ids are unique. D3D12 permits duplicates; the lab forbids them so that a readback can attribute a hit to
// exactly one instance, and so that "the identity is stable across a rebuild" is a property a test can assert.
[[nodiscard]] std::expected<TopLevelPacking, ContractError> ValidateInstances(
    std::span<InstanceDescription const> instances);

// Validates the buffer the instance array is uploaded into: 16-byte alignment, non-null, and large enough for
// `count` 64-byte descriptions.
[[nodiscard]] std::expected<void, ContractError> ValidateInstanceBuffer(BufferRange const &buffer,
                                                                        std::uint32_t instanceCount) noexcept;

// -------------------------------------------------------------------------------------------------------------
// E. Rays
// -------------------------------------------------------------------------------------------------------------

struct RayDescription final
{
    Float3 origin{};
    Float3 direction{};
    double tMinMetres{};
    double tMaxMetres{};

    [[nodiscard]] bool operator==(RayDescription const &) const noexcept = default;
};

struct RayValidation final
{
    double directionLength{};
    // The distance the interval covers in world units, that is, (tMax - tMin) times the direction length. It is
    // reported because `tMax - tMin` alone is meaningless when the direction is not unit length, and confusing the
    // two is how self-intersection epsilons end up scaled by an arbitrary factor.
    double intervalLengthMetres{};
    bool directionIsUnitLength{};
    // True when `tMin == tMax`. Legal, and a guaranteed triangle miss, because the triangle test is open at both
    // ends. Reported rather than refused so that a caller can tell "I asked a question with no answer" apart from
    // "I asked an illegal question".
    bool intervalIsEmpty{};

    [[nodiscard]] bool operator==(RayValidation const &) const noexcept = default;
};

// Validates one ray interval. DXR states the rule in one sentence: "Ray TMin must be nonnegative and <= TMax"
// (DirectX-Specs `d3d/Raytracing.md`, "Ray extents"). So `tMin == tMax` is *legal*, and only an inverted interval
// is not. An empty interval is accepted here and reported through `intervalIsEmpty`; it then misses every triangle,
// because the triangle test is the open `tMin < t < tMax`. Refusing it would have been the chapter tightening a
// rule and charging D3D12 for it.
//
// The direction must have a usable length and every component must be finite. An infinite `tMax` is refused rather
// than silently replaced by a large number, which is a chapter constraint: DXR accepts +INF, but a lab whose
// intervals are all finite can report an interval length that means something.
[[nodiscard]] std::expected<RayValidation, ContractError> ValidateRay(RayDescription const &ray) noexcept;

// Rejects flag combinations that contradict each other or that describe a traversal which cannot hit anything.
[[nodiscard]] std::expected<void, ContractError> ValidateRayFlags(RayFlags flags) noexcept;

// The 8-bit `InstanceInclusionMask`. Zero is refused: D3D12 accepts it and returns a guaranteed miss, which is
// never what a caller meant to write.
[[nodiscard]] std::expected<void, ContractError> ValidateInstanceInclusionMask(std::uint32_t mask) noexcept;

// -------------------------------------------------------------------------------------------------------------
// F. State object consistency
// -------------------------------------------------------------------------------------------------------------

enum class ShaderStage : std::uint8_t
{
    RayGeneration = 0U,
    Miss,
    ClosestHit,
    AnyHit,
    Intersection,
    Callable,
};

enum class HitGroupType : std::uint8_t
{
    Triangles = 0U,
    ProceduralPrimitive,
};

struct ShaderExport final
{
    std::string name{};
    ShaderStage stage{ShaderStage::RayGeneration};
};

// An empty shader name means "not present". A triangle hit group needs at least one of closest-hit and any-hit and
// must not name an intersection shader; a procedural hit group must name one.
struct HitGroupDescription final
{
    std::string name{};
    HitGroupType type{HitGroupType::Triangles};
    std::string closestHitExport{};
    std::string anyHitExport{};
    std::string intersectionExport{};
};

struct LocalRootSignature final
{
    std::string name{};
    // The number of bytes the signature's arguments occupy inside a shader record, after the 32-byte identifier.
    std::uint32_t rootArgumentSizeBytes{};
};

struct LocalRootSignatureAssociation final
{
    std::string localRootSignatureName{};
    // Export names or hit-group names. An export associated with two different local root signatures is ambiguous
    // and refused, because the record stride it implies is ambiguous too.
    std::vector<std::string> exportNames{};
};
struct RaytracingShaderConfig final
{
    std::uint32_t maxPayloadSizeBytes{};
    std::uint32_t maxAttributeSizeBytes{kTriangleAttributeSizeBytes};

    [[nodiscard]] bool operator==(RaytracingShaderConfig const &) const noexcept = default;
};

struct RaytracingPipelineConfig final
{
    std::uint32_t maxTraceRecursionDepth{1U};

    [[nodiscard]] bool operator==(RaytracingPipelineConfig const &) const noexcept = default;
};

struct StateObjectDescription final
{
    std::vector<ShaderExport> exports{};
    std::vector<HitGroupDescription> hitGroups{};
    RaytracingShaderConfig shaderConfig{};
    RaytracingPipelineConfig pipelineConfig{};
    std::vector<LocalRootSignature> localRootSignatures{};
    std::vector<LocalRootSignatureAssociation> associations{};
};

enum class ShaderTableSectionKind : std::uint8_t
{
    RayGeneration = 0U,
    Miss,
    HitGroup,
    Callable,
};

inline constexpr std::size_t kShaderTableSectionKindCount = 4U;

// One shader-table entry a validated state object requires. This is the bridge between the state object and the
// table: the table cannot be laid out without knowing which exports need records and how wide each record is.
struct ShaderTableEntryRequirement final
{
    std::string name{};
    ShaderTableSectionKind section{ShaderTableSectionKind::RayGeneration};
    std::uint32_t localRootArgumentSizeBytes{};
    // The smallest legal stride for a record holding this entry: the 32-byte identifier plus the local root
    // arguments, rounded up to the 32-byte record alignment.
    std::uint64_t minimumRecordStrideBytes{};

    [[nodiscard]] bool operator==(ShaderTableEntryRequirement const &) const noexcept = default;
};

struct StateObjectValidation final
{
    std::vector<ShaderTableEntryRequirement> entries{};
    std::uint32_t rayGenerationCount{};
    std::uint32_t missCount{};
    std::uint32_t hitGroupCount{};
    std::uint32_t callableCount{};
    std::uint32_t maximumLocalRootArgumentSizeBytes{};
    RaytracingShaderConfig shaderConfig{};
    RaytracingPipelineConfig pipelineConfig{};

    [[nodiscard]] bool operator==(StateObjectValidation const &) const noexcept = default;
};

// Checks the consistency a raytracing state object needs before `CreateStateObject` is worth calling: unique
// export and hit-group names, hit groups whose referenced exports exist and have the right stage, a shader config
// within the payload and attribute budgets, a declarable recursion depth, and local root signature associations
// that are unambiguous and that keep every implied record within the 4096-byte stride limit.
//
// Hit-group local root signatures follow DXR's rule verbatim (DirectX-Specs `d3d/Raytracing.md`, "Subobject
// associations for hit groups"): "If both a hit group has an association and its component shaders have
// associations, they must match. If a hit group doesn't have a particular subobject association, the associations
// for all component shaders must match. So different component shaders can't use different local root signatures."
// Modelled precisely, that is:
//   * The hit group's own association, when present, is authoritative. A component shader may either carry no
//     association at all, and inherit the group's, or carry one naming the *same* local root signature.
//   * With no association on the hit group, every component shader must resolve to the same association, and "no
//     association" is itself an association value: one associated member and one unassociated member do not match.
//   * A hit group's record width is that single resolved local root signature's argument size, or zero when
//     nothing is associated. It is never the widest of several, because a record with several widths is not a
//     record: it is a fiction that would make the table stride disagree with what the driver binds.
// A violation is `MismatchedHitGroupLocalRootSignature`, which is deliberately distinct from
// `ConflictingLocalRootSignatureAssociation` (one export named by two association subobjects).
[[nodiscard]] std::expected<StateObjectValidation, ContractError> ValidateStateObject(
    StateObjectDescription const &description);

// -------------------------------------------------------------------------------------------------------------
// G. Payload ABI
// -------------------------------------------------------------------------------------------------------------

struct PayloadFieldDescription final
{
    std::string name{};
    std::uint32_t scalarSizeBytes{};
    std::uint32_t scalarAlignmentBytes{};
    std::uint32_t elementCount{1U};
};

struct PayloadFieldLayout final
{
    std::string name{};
    std::uint32_t offsetBytes{};
    std::uint32_t sizeBytes{};

    [[nodiscard]] bool operator==(PayloadFieldLayout const &) const noexcept = default;
};

struct PayloadLayout final
{
    std::vector<PayloadFieldLayout> fields{};
    // The size the shader config must declare: the packed field extent rounded up to the payload's own alignment.
    std::uint32_t sizeBytes{};
    std::uint32_t alignmentBytes{};
    std::uint32_t paddingBytes{};

    [[nodiscard]] bool operator==(PayloadLayout const &) const noexcept = default;
};

// Lays out a payload structure the way HLSL would: fields in declaration order, each at the next offset that
// satisfies its alignment, with the whole structure rounded up to its widest field alignment. The result is what
// `MaxPayloadSizeInBytes` must be set to, and the padding it reports is the payload budget the lab is paying for
// and not using.
[[nodiscard]] std::expected<PayloadLayout, ContractError> ValidatePayloadLayout(
    std::span<PayloadFieldDescription const> fields, std::uint32_t maximumPayloadSizeBytes);

// -------------------------------------------------------------------------------------------------------------
// H. Shader identifiers and shader-table layout
// -------------------------------------------------------------------------------------------------------------

// The 32 opaque bytes `GetShaderIdentifier` returns. The contents are meaningless to this chapter; what matters is
// that a record carries exactly 32 of them and that they are not the zeroes a caller gets from an uninitialized
// buffer or from asking a state object for an export it does not have.
struct ShaderIdentifier final
{
    std::array<std::byte, static_cast<std::size_t>(kShaderIdentifierSizeBytes)> bytes{};

    [[nodiscard]] bool operator==(ShaderIdentifier const &) const noexcept = default;
};

[[nodiscard]] std::expected<ShaderIdentifier, ContractError> MakeShaderIdentifier(std::span<std::byte const> bytes);

struct ShaderRecordDescription final
{
    ShaderIdentifier identifier{};
    std::uint32_t localRootArgumentSizeBytes{};
};

struct ShaderTableSectionRequest final
{
    ShaderTableSectionKind kind{ShaderTableSectionKind::RayGeneration};
    std::vector<ShaderRecordDescription> records{};
};

struct ShaderRecordLayout final
{
    // Offset of the record from the start of the table.
    std::uint64_t offsetBytes{};
    // Offset of the local root arguments from the start of the table. Always `offsetBytes + 32`.
    std::uint64_t localRootArgumentOffsetBytes{};
    std::uint64_t localRootArgumentSizeBytes{};
    // Bytes between the end of this record's arguments and the start of the next record. Reported rather than
    // implied, because uninitialized padding inside a shader table is read by nothing and is still the most common
    // place a stale pointer survives.
    std::uint64_t paddingBytes{};

    [[nodiscard]] bool operator==(ShaderRecordLayout const &) const noexcept = default;
};

struct ShaderTableSectionLayout final
{
    ShaderTableSectionKind kind{ShaderTableSectionKind::RayGeneration};
    std::uint64_t offsetBytes{};
    // The stride `DispatchRays` uses for this section. Every record in a section shares one stride, so a section's
    // stride is set by its widest record and every narrower record pays the difference in padding.
    std::uint64_t strideBytes{};
    std::uint64_t sizeBytes{};
    std::uint64_t recordCount{};
    // Bytes of alignment padding between the previous section's end and this section's 64-byte aligned start.
    std::uint64_t leadingPaddingBytes{};
    std::vector<ShaderRecordLayout> records{};

    [[nodiscard]] bool operator==(ShaderTableSectionLayout const &) const noexcept = default;
};

struct ShaderTableLayout final
{
    std::vector<ShaderTableSectionLayout> sections{};
    std::uint64_t totalSizeBytes{};
    std::uint64_t paddingBytes{};

    [[nodiscard]] bool operator==(ShaderTableLayout const &) const noexcept = default;
};

// The byte size of a table section holding `recordCount` records of `strideBytes`. It is public because the
// multiplication is exactly where a record count that came from a scene, a material list, or a ray-type count
// overflows, and a test needs to be able to overflow it on purpose without allocating the records.
[[nodiscard]] std::expected<std::uint64_t, ContractError> ComputeSectionSizeBytes(std::uint64_t recordCount,
                                                                                  std::uint64_t strideBytes) noexcept;

// Lays out a whole shader table: sections in the order given, each starting at a 64-byte aligned offset, each with
// a 32-byte aligned stride set by its widest record, and the total rounded up to 64 bytes so that the buffer can be
// suballocated from a larger upload heap without disturbing the next table. Every addition and multiplication is
// checked, because the arithmetic here is exactly the arithmetic that produces an out-of-bounds GPU read when a
// record count is wrong.
[[nodiscard]] std::expected<ShaderTableLayout, ContractError> BuildShaderTableLayout(
    std::span<ShaderTableSectionRequest const> sections);

[[nodiscard]] std::expected<ShaderTableSectionLayout, ContractError> FindShaderTableSection(
    ShaderTableLayout const &layout, ShaderTableSectionKind kind);

// -------------------------------------------------------------------------------------------------------------
// I. Dispatch description and trace-time record selection
// -------------------------------------------------------------------------------------------------------------

// One of the four ranges of `D3D12_DISPATCH_RAYS_DESC`. The ray generation range has no stride, because it holds
// exactly one record; the field is present and zero so the shape of the four ranges stays uniform.
struct ShaderTableRange final
{
    std::uint64_t startAddress{};
    std::uint64_t sizeBytes{};
    std::uint64_t strideBytes{};

    [[nodiscard]] bool operator==(ShaderTableRange const &) const noexcept = default;
};

struct DispatchDimensions final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{};

    [[nodiscard]] bool operator==(DispatchDimensions const &) const noexcept = default;
};

struct DispatchRaysDescription final
{
    ShaderTableRange rayGeneration{};
    ShaderTableRange miss{};
    ShaderTableRange hitGroup{};
    ShaderTableRange callable{};
    DispatchDimensions dimensions{};
    std::uint64_t threadCount{};

    [[nodiscard]] bool operator==(DispatchRaysDescription const &) const noexcept = default;
};

[[nodiscard]] std::expected<std::uint64_t, ContractError> ValidateDispatchDimensions(
    DispatchDimensions dimensions) noexcept;

// Turns a table layout plus the base address the table was uploaded to into the four ranges `DispatchRays` needs.
// Absent sections produce all-zero ranges, which is the legal way to say "this pipeline has no callable shaders".
[[nodiscard]] std::expected<DispatchRaysDescription, ContractError> MakeDispatchRaysDescription(
    ShaderTableLayout const &layout, std::uint64_t tableBaseAddress, DispatchDimensions dimensions);

// The three caller-supplied terms of the hit-group record index, together with the geometry's own index. The
// arithmetic D3D12 performs is:
//
//     index = RayContributionToHitGroupIndex
//           + MultiplierForGeometryContributionToHitGroupIndex * GeometryContributionToHitGroupIndex
//           + InstanceContributionToHitGroupIndex
//
// and the record address is the hit-group table start plus `index * stride`. Two of the terms come from the
// `TraceRay` call, one from the bottom-level structure, and one from the instance, which is why a wrong ray type
// count and a wrong instance contribution produce indistinguishable symptoms at runtime and very distinguishable
// errors here.
struct HitGroupIndexParameters final
{
    std::uint32_t rayContributionToHitGroupIndex{};
    std::uint32_t multiplierForGeometryContributionToHitGroupIndex{};
    std::uint32_t geometryContributionToHitGroupIndex{};
    std::uint32_t instanceContributionToHitGroupIndex{};
};

[[nodiscard]] std::expected<std::uint64_t, ContractError> ComputeHitGroupRecordIndex(
    HitGroupIndexParameters const &parameters) noexcept;

// The three resolvers below all compute `tableBase + sectionOffset + index * stride`. Every step of that is a
// checked 64-bit operation and reports `ArithmeticOverflow` rather than wrapping. A wrapped shader-record address
// is not a diagnosable failure on a GPU: it is a read of unrelated memory that produces a plausible-looking wrong
// image, so the one place the chapter can catch it is here, on the CPU, before the address is ever written into a
// dispatch description.
[[nodiscard]] std::expected<std::uint64_t, ContractError> ResolveHitGroupRecordAddress(
    ShaderTableLayout const &layout, std::uint64_t tableBaseAddress, HitGroupIndexParameters const &parameters);

[[nodiscard]] std::expected<std::uint64_t, ContractError> ResolveMissRecordAddress(ShaderTableLayout const &layout,
                                                                                   std::uint64_t tableBaseAddress,
                                                                                   std::uint32_t missShaderIndex);

[[nodiscard]] std::expected<std::uint64_t, ContractError> ResolveCallableRecordAddress(ShaderTableLayout const &layout,
                                                                                       std::uint64_t tableBaseAddress,
                                                                                       std::uint32_t callableIndex);

// -------------------------------------------------------------------------------------------------------------
// J. Deterministic CPU reference intersection
//
// The reference exists so the lab has something to compare a GPU trace against, and so the chapter can talk about
// facing, masking, opacity, and the t interval with a result in hand. Four honesty notes:
//
//   * Traversal order here is total and documented: instances in array order, geometries in array order, triangles
//     in array order. D3D12 guarantees no such order. The reference is deterministic *because* a test needs it to
//     be, and `AcceptFirstHitAndEndSearch` returning the first hit in this order is precisely the behaviour a
//     shader must not rely on.
//   * The intersection is computed on world-space vertices, obtained by applying the instance transform, rather
//     than by transforming the ray into object space as hardware does. For the affine transforms an instance can
//     carry, the two agree on `t` and on barycentrics; the world-space form is used because it needs no matrix
//     inverse and therefore has no inversion error to explain. They do *not* automatically agree on facing, which
//     is the next note.
//   * Facing is object space, exactly as D3D12 defines it. Because the world-space geometric normal is
//     `cross(M*e1, M*e2) == det(M) * inverse-transpose(M) * cross(e1, e2)`, the world-space facing dot product is
//     the object-space one scaled by `det(M)`. The reference therefore divides the instance transform's
//     determinant sign back out. This removes the world-space winding reversal caused by a negative determinant;
//     the remaining answer still depends on how the inverse instance transform carries the ray into object space.
//     A per-geometry `Transform3x4` behaves differently in D3D12 because the BLAS build bakes it into object space;
//     this reference models the post-bake object space directly, so a caller reproduces a mirroring geometry
//     transform by baking it into `ReferenceTriangle` vertices, and facing then flips as it should.
//   * The triangle t interval is open at both ends: `tMin < t < tMax`. A hit exactly on either endpoint is counted
//     as `culledByInterval`, not as a hit.
// -------------------------------------------------------------------------------------------------------------

struct ReferenceTriangle final
{
    Float3 v0{};
    Float3 v1{};
    Float3 v2{};

    [[nodiscard]] bool operator==(ReferenceTriangle const &) const noexcept = default;
};

struct ReferenceGeometry final
{
    std::vector<ReferenceTriangle> triangles{};
    GeometryFlags flags{GeometryFlags::None};
};

struct ReferenceInstance final
{
    Transform3x4 objectToWorld{IdentityTransform()};
    std::uint32_t instanceId{};
    std::uint32_t instanceMask{kMaximumInstanceMask};
    std::uint32_t instanceContributionToHitGroupIndex{};
    InstanceFlags flags{InstanceFlags::None};
    std::vector<ReferenceGeometry> geometries{};
};

// How the reported world normal is oriented. The geometric normal follows the winding of the world-space vertices
// and is what a shader gets from a cross product of the transformed edges; the ray-opposing form is what shading
// usually wants. Neither is a default: the caller says which one it is asking for.
//
// Beware the interaction with facing. `HitKind` is decided in object space, but `GeometricWinding` reports a world
// normal, so under a mirroring instance transform a `FrontFace` hit can report a normal pointing *along* the ray.
// That is not a bug in either quantity, it is the reason a shader that wants a shading normal either asks for
// `OpposeRayDirection` or flips the geometric normal by the sign of the instance transform's determinant.
enum class FaceNormalPolicy : std::uint8_t
{
    GeometricWinding = 0U,
    OpposeRayDirection,
};

enum class HitKind : std::uint8_t
{
    // Mirrors HIT_KIND_TRIANGLE_FRONT_FACE (254) and HIT_KIND_TRIANGLE_BACK_FACE (255). The numeric values are not
    // mirrored here because nothing in this chapter serializes them. Decided in object space, so it is unaffected
    // by the instance transform's handedness.
    FrontFace = 0U,
    BackFace,
};

struct TraceParameters final
{
    RayDescription ray{};
    RayFlags flags{RayFlags::None};
    std::uint32_t instanceInclusionMask{kMaximumInstanceMask};
    std::uint32_t rayContributionToHitGroupIndex{};
    std::uint32_t multiplierForGeometryContributionToHitGroupIndex{};
    std::uint32_t missShaderIndex{};
    FaceNormalPolicy normalPolicy{FaceNormalPolicy::GeometricWinding};
};

struct TriangleHit final
{
    double tHitMetres{};
    // The DXR attribute pair: v0 * (1 - b1 - b2) + v1 * b1 + v2 * b2.
    Float2 barycentrics{};
    std::uint32_t instanceIndex{};
    std::uint32_t instanceId{};
    std::uint32_t geometryIndex{};
    std::uint32_t primitiveIndex{};
    HitKind hitKind{HitKind::FrontFace};
    Float3 worldPosition{};
    Float3 worldNormal{};
    // The record index a `TraceRay` with these parameters would select for this hit.
    std::uint64_t hitGroupRecordIndex{};
    // Whether an any-hit shader would run for this hit once geometry, instance, and ray opacity flags are resolved.
    bool anyHitWouldRun{};

    [[nodiscard]] bool operator==(TriangleHit const &) const noexcept = default;
};

// Counted evidence about what the traversal did. Counters are the difference between "no hit" and "no hit, because
// every instance was masked out", which is the single most common lost hour in DXR bring-up.
struct TraceCounters final
{
    std::uint64_t instancesTested{};
    std::uint64_t instancesCulledByMask{};
    std::uint64_t trianglesTested{};
    std::uint64_t candidateHits{};
    std::uint64_t culledByParallelRay{};
    // Candidates whose `t` fell outside the open interval `tMin < t < tMax`, endpoints included in the rejection.
    std::uint64_t culledByInterval{};
    // Candidates rejected by `CullFrontFacingTriangles` or `CullBackFacingTriangles`, using object-space facing.
    std::uint64_t culledByFace{};
    std::uint64_t culledByOpacity{};
    std::uint64_t culledBySkipTriangles{};

    [[nodiscard]] bool operator==(TraceCounters const &) const noexcept = default;
};

struct TraceResult final
{
    std::optional<TriangleHit> hit{};
    // The miss record index a miss would select. Reported whether or not the trace hit, because the miss index is
    // chosen by the caller and not by the traversal.
    std::uint32_t missShaderIndex{};
    // True when the search stopped at the first accepted hit rather than continuing to the nearest one.
    bool acceptedFirstHit{};
    // True when a closest-hit shader would run: there is a hit and `SkipClosestHitShader` was not set.
    bool closestHitWouldRun{};
    TraceCounters counters{};

    [[nodiscard]] bool operator==(TraceResult const &) const noexcept = default;
};

// Traces one ray against a reference scene. Every rejection reason is counted, the winning hit is the smallest `t`
// with ties broken by traversal order, and an empty scene is a legitimate miss rather than an error.
[[nodiscard]] std::expected<TraceResult, ContractError> TraceReferenceRay(std::span<ReferenceInstance const> instances,
                                                                          TraceParameters const &parameters);

// -------------------------------------------------------------------------------------------------------------
// K. Stage status evidence
// -------------------------------------------------------------------------------------------------------------

struct StageStatus final
{
    PipelineStage stage{PipelineStage::BottomLevelBuild};
    bool recorded{};
    bool succeeded{};
    std::optional<ContractError> failure{};
    // How many checked facts the stage published: geometries validated, instances packed, records laid out. A stage
    // that succeeds with no evidence has not been exercised, and is refused.
    std::uint32_t evidenceCount{};

    [[nodiscard]] bool operator==(StageStatus const &) const noexcept = default;
};

// Records the outcome of the five stages in order. The ledger is the chapter's answer to "the screen is black":
// it names the first stage that failed, refuses to let a later stage be recorded before an earlier one, and refuses
// to call a pipeline dispatchable until every stage has published evidence.
class PipelineStatusLedger final
{
  public:
    PipelineStatusLedger() noexcept;

    [[nodiscard]] std::expected<void, ContractError> RecordSuccess(PipelineStage stage,
                                                                   std::uint32_t evidenceCount) noexcept;
    [[nodiscard]] std::expected<void, ContractError> RecordFailure(PipelineStage stage, ContractError failure) noexcept;

    [[nodiscard]] StageStatus Status(PipelineStage stage) const noexcept;
    [[nodiscard]] std::optional<PipelineStage> FirstFailedStage() const noexcept;
    [[nodiscard]] std::optional<PipelineStage> NextExpectedStage() const noexcept;
    [[nodiscard]] bool IsReadyToDispatch() const noexcept;
    [[nodiscard]] std::uint32_t TotalEvidenceCount() const noexcept;

  private:
    std::array<StageStatus, kPipelineStageCount> stages_{};
    std::size_t nextStageIndex_{};
    std::optional<PipelineStage> firstFailedStage_{};
};

} // namespace ch33::dxr
