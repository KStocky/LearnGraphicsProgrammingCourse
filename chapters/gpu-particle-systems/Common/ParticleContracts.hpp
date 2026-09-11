#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Chapter 34 teaching contracts for GPU particle systems: state layout, deterministic emission, fixed-step
// simulation, lifetime management, compaction, and indirect rendering.
//
// Scope and honesty rules for this phase:
//
//   * Everything here is a deterministic CPU model of the decisions a GPU particle frame normally hides inside a
//     handful of compute dispatches: how a particle's fields are laid out and addressed, which slot a new particle
//     lands in, what identity it carries, how much simulation time a frame is allowed to consume, which integrator
//     advanced it, when it died, how the live set is compacted, and how the draw that renders it is described
//     without the CPU ever learning how many particles survived. None of it is D3D12, and none of it pretends to
//     be.
//   * What D3D12 and the hardware own, and this chapter only mirrors: the actual byte layout the driver gives a
//     structured buffer view, the order in which parallel threads reach an append counter, the wave size that
//     decides how a prefix scan is actually implemented, the real cost of any of it, and every residency and
//     lifetime rule the runtime enforces. This chapter validates *descriptions* and *reference results*.
//   * What this chapter pins, so the later lab can be deterministic: the numeric limits copied from `d3d12.h`
//     (each named after its macro and pinned by a test), the byte arithmetic of a sub-allocated
//     structure-of-arrays state buffer, one total order for slot allocation, one total order for compaction
//     output, the exact per-substep sequence of emission, integration, collision, and ageing, and the closed-form
//     truncation and energy behaviour of each integrator under a constant field.
//   * No D3D12 header is included. The contracts are pure C++23 so that they build, run, and can be reasoned
//     about on a machine with no GPU at all. The cost is that mirrored constants can drift from the SDK; the
//     mitigation is that each one is named, documented with its origin, and pinned by an explicit test rather than
//     appearing inline as a literal.
//   * Nothing here silently repairs an invalid input. Every entry point returns `std::expected` with a specific
//     `ContractError`; there are no exceptions, no clamp-to-something-plausible defaults, and no "0 means use the
//     default" parameters. A caller holding a value knows every precondition held. Where an input is legal but
//     the system cannot honour it -- an emission that does not fit in the remaining capacity, a live set larger
//     than the draw can address -- the result is a *counter*, not an error, because dropping work silently is the
//     failure this chapter exists to make visible.
//   * Determinism here is bit-exact on the CPU. The GPU lab that consumes these contracts will compute in 32-bit
//     floats and will therefore not reproduce the double-precision reference exactly; the reference publishes a
//     float32 checksum for the comparisons that must be exact (identity, alive flags, counts, indices) and leaves
//     the continuous quantities to a declared tolerance. Claiming exact float agreement across two different
//     arithmetic pipelines would be a lie the tests could not defend.
//
// Units, spaces, and sign conventions:
//
//   * Positions are metres in world space, velocities metres per second, accelerations metres per second squared,
//     ages and lifetimes seconds. Every bound is stated in those units and named for them.
//   * The ground plane is the half-space y >= height. Gravity is an ordinary acceleration vector with no assumed
//     direction, so a test can point it sideways and still be modelling the same contract.
//   * "Specific energy" is energy per unit mass, in joules per kilogram: 0.5 * |v|^2 - dot(gravity, position). The
//     chapter never introduces mass, because no contract here depends on it and a particle system that pretends to
//     integrate momentum without ever dividing by a mass is teaching a fiction.
//   * Sizes, strides, offsets, and alignments are unsigned byte counts, always named `...Bytes`.
//   * Counts of particles are `std::uint32_t` where they index a bounded array and `std::uint64_t` where they
//     accumulate over a run, because a per-frame count and a lifetime total have different overflow stories.
//
// Deliberately out of scope, and not approximated here: sorting particles for correct alpha compositing, which
// Chapter 32 owns and which the later spatial-binning chapter revisits; neighbour queries, spatial hashing, and
// any force that couples two particles, which the next simulation chapters own; constraint solvers and
// position-based dynamics; continuous collision detection and time-of-impact solves, which a fixed-step
// half-space projection is explicitly *not*; mass, inertia, and angular motion; the visual quality of a particle,
// which is a shading question and not a simulation one; and the real performance of any of this, which only the
// paired GPU lab and Chapter 22's methodology can answer.

namespace ch34::particles
{

// ---------------------------------------------------------------------------------------------------------------
// A. Mirrored D3D12 constants
//
// Each value is copied from `d3d12.h` and named after the macro it mirrors, because a literal 4096 in an
// expression is indistinguishable from a literal 4096 that means something else. Tests pin every one of them.
// ---------------------------------------------------------------------------------------------------------------

// sizeof(D3D12_DRAW_ARGUMENTS): four 32-bit fields, in the order `DrawInstanced` takes them.
inline constexpr std::uint64_t kDrawArgumentsSizeBytes = 16U;

// The number of 32-bit words in D3D12_DRAW_ARGUMENTS. Named because the compute shader that writes the argument
// buffer writes exactly this many `uint`s and a mismatch is a silently misaligned draw rather than a crash.
inline constexpr std::uint32_t kDrawArgumentsWordCount = 4U;

// The core runtime validates that `ArgumentBufferOffset` and `CountBufferOffset` are 4-byte aligned. See
// `ID3D12GraphicsCommandList::ExecuteIndirect`, "Remarks".
inline constexpr std::uint64_t kIndirectArgumentOffsetAlignmentBytes = 4U;

// D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT. The offset of a UAV counter inside its counter resource. A dead list
// implemented as an append/consume buffer carries exactly one of these.
inline constexpr std::uint64_t kUavCounterPlacementAlignmentBytes = 4'096U;

// D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT. The byte offset a raw (byte-address) view may start at.
inline constexpr std::uint64_t kRawUavByteAlignmentBytes = 16U;

// D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT. Used here only to size the per-frame constants block that
// carries the simulation parameters, so the lab's suballocation arithmetic has a real number in it.
inline constexpr std::uint64_t kConstantBufferPlacementAlignmentBytes = 256U;

// D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT. Two ping-pong state buffers placed in one heap are separated by a
// multiple of this, which is why the ping-pong size is not simply twice the single-slot size.
inline constexpr std::uint64_t kDefaultResourcePlacementAlignmentBytes = 65'536U;

// D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP.
inline constexpr std::uint32_t kMaximumThreadsPerGroup = 1'024U;

// D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION.
inline constexpr std::uint32_t kMaximumThreadGroupsPerDimension = 65'535U;

// D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP: a buffer view addresses at most 2^27 elements.
inline constexpr std::uint32_t kBufferViewElementCountExponent = 27U;
inline constexpr std::uint64_t kMaximumBufferViewElementCount = 1ULL << kBufferViewElementCountExponent;

// ---------------------------------------------------------------------------------------------------------------
// B. Chapter budgets
//
// These are *not* D3D12 limits. They exist so the lab stays inspectable, so overflow tests have something concrete
// to overflow, and so a runaway value fails on the CPU with a name instead of on the GPU with a device removal.
// ---------------------------------------------------------------------------------------------------------------

// The largest particle system the chapter builds. A real system goes far past this; the bound keeps a full CPU
// reference run cheap enough to be a unit test and keeps every derived byte count inside 64 bits with room to
// spare.
inline constexpr std::uint32_t kMaximumParticleCapacity = 1U << 20U;

// The reference simulation refuses to leave the box, because a position that has run away to 1e30 makes every
// downstream diagnostic meaningless while still being a perfectly finite float.
inline constexpr double kMaximumPositionMetres = 1.0e4;
inline constexpr double kMaximumSpeedMetresPerSecond = 1.0e4;
inline constexpr double kMaximumAccelerationMetresPerSecondSquared = 1.0e4;

// Fixed-step bounds. The lower bound stops a "more accurate" configuration from turning one frame into millions of
// substeps; the upper bound stops a step so long that the integrator comparison stops being about integration.
inline constexpr double kMinimumFixedTimestepSeconds = 1.0e-4;
inline constexpr double kMaximumFixedTimestepSeconds = 0.1;

// The largest number of fixed steps one frame may run. Beyond this the frame is *late* and the chapter says so
// with a counter instead of catching up forever.
inline constexpr std::uint32_t kMaximumSubstepCount = 16U;

// The largest frame delta the scheduler will accept before clamping, and the largest backlog the accumulator may
// carry. A backlog past this is the spiral of death, which the chapter refuses rather than rides.
inline constexpr double kMaximumFrameDeltaSeconds = 1.0;
inline constexpr double kMaximumAccumulatorSeconds = 1.0;

inline constexpr double kMaximumEmissionRatePerSecond = 1.0e6;
inline constexpr double kMaximumLifetimeSeconds = 3'600.0;
inline constexpr double kMaximumJitterMetres = 1.0e3;

// The largest spawn count one emission tick may ask for. `kMaximumEmissionRatePerSecond * kMaximumFixedTimestepSeconds`
// is 100'000, so this bound is reachable from legal settings and the refusal it produces is a real path.
inline constexpr std::uint32_t kMaximumSpawnPerTick = 65'536U;

// The reference draws one quad per particle by default; the bound exists so the vertex count in an indirect
// argument cannot be a number that only overflows on the GPU.
inline constexpr std::uint32_t kMaximumVertexCountPerParticle = 1'024U;

// The largest indirect command count the chapter's argument buffer describes. One draw per frame is the norm here;
// the bound leaves room for a small fixed set without letting a mistaken count become a gigabyte of arguments.
inline constexpr std::uint32_t kMaximumIndirectCommandCount = 1'024U;

// How close two doubles must be before the chapter calls a float32 round trip lossless. `float` carries 24 bits of
// mantissa, so a relative difference below 2^-23 is exactly what a float can and cannot distinguish.
inline constexpr double kSinglePrecisionRelativeEpsilon = 1.1920928955078125e-07;

// ---------------------------------------------------------------------------------------------------------------
// C. Identity
// ---------------------------------------------------------------------------------------------------------------

// A particle's stable identity. It is *not* a slot index: a slot is recycled, an identity is not. Everything that
// has to survive compaction, ping-pong, and recycling is keyed on this.
using ParticleId = std::uint32_t;

// The sentinel stored in a dead slot. It is the only reserved identity, which is why the largest issuable identity
// is one less than the width of the type.
inline constexpr ParticleId kInvalidParticleId = 0xFFFF'FFFFU;
inline constexpr ParticleId kMaximumParticleId = kInvalidParticleId - 1U;

// ---------------------------------------------------------------------------------------------------------------
// D. Frame stages and errors
// ---------------------------------------------------------------------------------------------------------------

// The stage of a particle frame a failure belongs to. The stages are separate because they have different owners,
// different evidence, and different fixes: an emission that produced nothing and a compaction that dropped
// everything look identical on screen and are not remotely the same bug.
enum class FrameStage : std::uint8_t
{
    Reset = 0U,
    Emission,
    Simulation,
    Compaction,
    IndirectArguments,
    Render,
    Readback,
};

inline constexpr std::size_t kFrameStageCount = 7U;

enum class ContractError : std::uint16_t
{
    // Shared arithmetic and domain failures. They are shared on purpose: an overflow is the same mistake wherever
    // it happens, and pretending otherwise would multiply the enumerator list without adding information.
    NonFinite = 0U,
    ArithmeticOverflow,
    AlignmentNotPowerOfTwo,
    ZeroAlignment,

    // Layout and capacity
    ZeroCapacity,
    CapacityExceeded,
    UnknownAttribute,
    BufferTooSmall,
    OffsetMisaligned,
    OffsetNotStrideMultiple,
    ElementIndexOutOfRange,
    StateSizeMismatch,

    // Identity
    ParticleIdSpaceExhausted,
    LiveSlotHasInvalidId,
    DeadSlotHasValidId,
    DuplicateParticleId,
    IdentityNotMonotonic,

    // Emission
    NegativeEmissionRate,
    EmissionRateTooLarge,
    NonPositiveLifetime,
    LifetimeTooLarge,
    NegativeJitter,
    JitterTooLarge,
    EmissionCarryOutOfRange,
    SpawnCountExceedsLimit,
    UnknownDeadSlotPolicy,

    // Fixed-step scheduling
    NegativeFrameDelta,
    FrameDeltaTooLarge,
    FixedTimestepOutOfRange,
    SubstepLimitOutOfRange,
    FrameDeltaClampOutOfRange,
    NegativeAccumulator,
    SimulationTimeBacklogExceeded,
    UnknownOverflowPolicy,

    // Force fields and integration
    UnknownIntegrator,
    NegativeDragCoefficient,
    DragCoefficientTooLarge,
    VerletRequiresVelocityIndependentForce,
    NonPositiveTimestep,
    PositionOutOfDomain,
    SpeedOutOfDomain,
    AccelerationOutOfDomain,
    ZeroStepCount,
    DegenerateConvergenceEstimate,
    EnergyUndefinedWithDissipation,

    // Lifetime and collision
    NegativeAge,
    AgeExceedsLifetime,
    RestitutionOutOfRange,
    FrictionOutOfRange,
    NegativeRestingSpeed,
    PlaneHeightOutOfDomain,

    // Compaction and dispatch
    NonBinaryAliveFlag,
    ParallelArrayLengthMismatch,
    AliveCountMismatch,
    ZeroThreadGroupSize,
    ThreadGroupSizeExceeded,
    ThreadGroupCountExceeded,

    // Indirect arguments
    ZeroVertexCountPerParticle,
    VertexCountPerParticleTooLarge,
    ZeroCommandCount,
    CommandCountExceeded,
    IndirectStrideMismatch,
    ReadbackDependencyInSteadyState,

    // Stage ledger, ping-pong ownership, and barriers
    StageOutOfOrder,
    StageAlreadyRecorded,
    EvidenceRequired,
    StagePartitionOverlap,
    StagePartitionIncomplete,
    PingPongAliasViolation,
    PingPongOwnershipMismatch,
    UnknownTransition,
    DuplicateTransition,
    IncorrectBarrierSync,
    IncorrectBarrierAccess,
    MissingBarrier,

    // Accounting
    AccountingMismatch,
    CounterOverflow,
};

inline constexpr std::size_t kContractErrorCount = 77U;

// Every enumerator, in declaration order. A test walks this to prove the name and stage tables are total, which is
// what stops a newly added error from being invisible to diagnostics.
[[nodiscard]] std::span<ContractError const> AllContractErrors() noexcept;
[[nodiscard]] std::string_view ContractErrorName(ContractError error) noexcept;
[[nodiscard]] std::string_view FrameStageName(FrameStage stage) noexcept;

// The stage an error belongs to, or `std::nullopt` when the error genuinely occurs at more than one stage. The
// nullopt is a statement, not a placeholder.
[[nodiscard]] std::optional<FrameStage> StageForError(ContractError error) noexcept;

struct StageDiagnostic final
{
    FrameStage reportingStage{FrameStage::Reset};
    ContractError error{ContractError::NonFinite};
    // The stage the error is *attributed* to, which differs from the reporting stage when a later stage discovers
    // an earlier stage's mistake. That difference is the whole value of the record.
    std::optional<FrameStage> attributedStage{};

    [[nodiscard]] bool operator==(StageDiagnostic const &) const noexcept = default;
};

[[nodiscard]] StageDiagnostic MakeStageDiagnostic(FrameStage reportingStage, ContractError error) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// E. Vectors and checked arithmetic
// ---------------------------------------------------------------------------------------------------------------

// Doubles, not floats, because this type is what tests compare against. The GPU copy is float32; the conversion is
// explicit and appears exactly once, in the checksum.
struct Float3 final
{
    double x{};
    double y{};
    double z{};

    [[nodiscard]] bool operator==(Float3 const &) const noexcept = default;
};

[[nodiscard]] Float3 Add(Float3 left, Float3 right) noexcept;
[[nodiscard]] Float3 Subtract(Float3 left, Float3 right) noexcept;
[[nodiscard]] Float3 Scale(Float3 value, double factor) noexcept;
[[nodiscard]] double Dot(Float3 left, Float3 right) noexcept;
[[nodiscard]] double LengthSquared(Float3 value) noexcept;
[[nodiscard]] double Length(Float3 value) noexcept;
[[nodiscard]] bool IsFinite(Float3 value) noexcept;

[[nodiscard]] bool IsPowerOfTwo(std::uint64_t value) noexcept;
[[nodiscard]] std::expected<std::uint64_t, ContractError> CheckedAdd(std::uint64_t left, std::uint64_t right) noexcept;
[[nodiscard]] std::expected<std::uint64_t, ContractError> CheckedMultiply(std::uint64_t left,
                                                                          std::uint64_t right) noexcept;
// Rounds up to a multiple of a power of two. D3D12's own alignments are all powers of two, so a value that is not
// one is a mistake rather than a slow path, and it is refused.
[[nodiscard]] std::expected<std::uint64_t, ContractError> AlignUp(std::uint64_t value,
                                                                  std::uint64_t alignmentBytes) noexcept;
[[nodiscard]] bool IsAligned(std::uint64_t value, std::uint64_t alignmentBytes) noexcept;
// Rounds up to a multiple of *any* non-zero value. It exists because a structured view of a 12-byte element forces
// section offsets that are multiples of 48, and 48 is not a power of two, so the masking form above cannot express
// the constraint the hardware actually imposes.
[[nodiscard]] std::expected<std::uint64_t, ContractError> RoundUpToMultiple(std::uint64_t value,
                                                                            std::uint64_t multiple) noexcept;
[[nodiscard]] std::expected<std::uint64_t, ContractError> LeastCommonMultiple(std::uint64_t left,
                                                                              std::uint64_t right) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// F. Particle ABI: the structure-of-arrays state buffer
//
// The state is six parallel arrays sharing one buffer, not one array of a six-field struct. The reason is the one
// the lab measures: a simulation pass that only advances position and velocity should not pull lifetime and
// identity through the cache, and a compaction pass that only reads the alive flag should read a contiguous run of
// alive flags. The cost is the arithmetic below, which is exactly what this section exists to make checkable.
//
// Two D3D12 rules decide the offsets, and they are different rules:
//
//   * A raw (byte-address) view may start only at a multiple of `kRawUavByteAlignmentBytes`.
//   * A structured view addresses element `FirstElement` at byte offset `FirstElement * StructureByteStride`, so a
//     section that is to be viewed as a structured buffer must start at a multiple of its own stride.
//
// A section therefore starts at a multiple of the least common multiple of the two, which is 16 for the 4-byte
// attributes and 48 for the 12-byte ones. The 48 is not a typo and is not padding for its own sake: it is the
// smallest offset that satisfies both views of a `float3` array.
// ---------------------------------------------------------------------------------------------------------------

enum class ParticleAttribute : std::uint8_t
{
    Position = 0U,
    Velocity,
    Age,
    Lifetime,
    Id,
    Alive,
};

inline constexpr std::size_t kParticleAttributeCount = 6U;

[[nodiscard]] std::span<ParticleAttribute const> AllParticleAttributes() noexcept;
[[nodiscard]] std::string_view ParticleAttributeName(ParticleAttribute attribute) noexcept;

// The GPU element size, in bytes: 12 for the two `float3` attributes, 4 for the rest. The alive flag is a 32-bit
// unsigned integer rather than a byte because HLSL has no addressable 8-bit type in a structured buffer, so
// "one bit of state" costs four bytes per particle and the chapter says so instead of hiding it.
[[nodiscard]] std::expected<std::uint32_t, ContractError> AttributeElementSizeBytes(
    ParticleAttribute attribute) noexcept;
[[nodiscard]] std::expected<std::uint64_t, ContractError> AttributeSectionAlignmentBytes(
    ParticleAttribute attribute) noexcept;

struct AttributeRange final
{
    std::uint64_t offsetBytes{};
    std::uint64_t strideBytes{};
    std::uint64_t sizeBytes{};
    std::uint32_t elementCount{};

    [[nodiscard]] bool operator==(AttributeRange const &) const noexcept = default;
};

struct ParticleStateLayout final
{
    std::uint32_t capacity{};
    std::array<AttributeRange, kParticleAttributeCount> attributes{};
    // The size of one ping-pong slot: every section, in declaration order, each aligned to its own section
    // alignment, with the total rounded up to the raw-view alignment so that the next slot also starts legally.
    std::uint64_t slotSizeBytes{};
    // Two slots placed in one heap, each starting at a multiple of `kDefaultResourcePlacementAlignmentBytes`.
    std::uint64_t pingPongSizeBytes{};
    // The byte offset of the second slot inside that heap.
    std::uint64_t secondSlotOffsetBytes{};
    // The bytes a single particle costs across all six sections, ignoring section padding. Reported because the
    // difference between this and `slotSizeBytes / capacity` is the padding the layout spends, and a learner who
    // cannot see that number cannot judge the layout.
    std::uint64_t bytesPerParticle{};

    [[nodiscard]] bool operator==(ParticleStateLayout const &) const noexcept = default;
};

[[nodiscard]] std::expected<ParticleStateLayout, ContractError> ComputeParticleStateLayout(
    std::uint32_t capacity) noexcept;
[[nodiscard]] std::expected<AttributeRange, ContractError> FindAttributeRange(ParticleStateLayout const &layout,
                                                                              ParticleAttribute attribute) noexcept;
[[nodiscard]] std::expected<std::uint64_t, ContractError> AttributeElementOffsetBytes(ParticleStateLayout const &layout,
                                                                                      ParticleAttribute attribute,
                                                                                      std::uint32_t index) noexcept;
[[nodiscard]] std::expected<void, ContractError> ValidateStateBuffer(ParticleStateLayout const &layout,
                                                                     std::uint64_t bufferSizeBytes) noexcept;

// The free list -- the "dead list" -- is a bounded array of slot indices plus one 32-bit counter. On the GPU the
// counter is a UAV counter, which is why its offset carries the 4096-byte placement alignment rather than sitting
// immediately after the indices.
struct FreeListLayout final
{
    std::uint32_t capacity{};
    std::uint64_t indicesOffsetBytes{};
    std::uint64_t indicesSizeBytes{};
    std::uint64_t counterOffsetBytes{};
    std::uint64_t totalSizeBytes{};

    [[nodiscard]] bool operator==(FreeListLayout const &) const noexcept = default;
};

[[nodiscard]] std::expected<FreeListLayout, ContractError> ComputeFreeListLayout(std::uint32_t capacity) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// G. Deterministic seeded initialization
//
// The initial position and velocity of a particle are a pure function of (seed, identity, stream). Nothing is
// carried between particles, so the GPU emits in any thread order and still produces the same particle, and a test
// can reproduce any particle in isolation without replaying the run that made it.
//
// The mixing function is integer-only and 32-bit, so an HLSL implementation reproduces it bit-exactly. The unit
// conversion keeps 24 bits and scales by 2^-24, which is exact in both `float` and `double`; that is why the
// jittered values below are exactly representable and the tests can assert equality rather than a tolerance.
// ---------------------------------------------------------------------------------------------------------------

enum class SeedStream : std::uint32_t
{
    PositionX = 0U,
    PositionY,
    PositionZ,
    VelocityX,
    VelocityY,
    VelocityZ,
};

inline constexpr std::size_t kSeedStreamCount = 6U;

[[nodiscard]] std::uint32_t MixSeed(std::uint64_t seed, ParticleId identity, SeedStream stream) noexcept;

// A value in [0, 1), derived from the top 24 bits, and a value in [-1, 1) derived from the same bits. Both are
// exact multiples of 2^-24 and 2^-23 respectively.
[[nodiscard]] double UnitFromBits(std::uint32_t bits) noexcept;
[[nodiscard]] double SignedUnitFromBits(std::uint32_t bits) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// H. Emission
// ---------------------------------------------------------------------------------------------------------------

// Which free slot a spawn takes. Both orders are deterministic *here*; the point of naming them is that a GPU dead
// list built from an append/consume counter has neither order, and reproducing a CPU reference therefore requires
// the emission pass to allocate from a scanned, sorted free list rather than from a raw counter. The two policies
// differ in which slot holds which identity and agree on every count and on the set of live identities, which is
// exactly the invariant a test should be asserting.
enum class DeadSlotPolicy : std::uint8_t
{
    AscendingIndex = 0U,
    DescendingIndex,
};

struct EmissionSettings final
{
    double ratePerSecond{};
    double lifetimeSeconds{};
    Float3 originMetres{};
    double positionJitterMetres{};
    Float3 baseVelocityMetresPerSecond{};
    double velocityJitterMetresPerSecond{};
    DeadSlotPolicy deadSlotPolicy{DeadSlotPolicy::AscendingIndex};

    [[nodiscard]] bool operator==(EmissionSettings const &) const noexcept = default;
};

[[nodiscard]] std::expected<void, ContractError> ValidateEmissionSettings(EmissionSettings const &settings) noexcept;

// The rate accumulator. `carry` is the fractional particle owed from previous ticks and is always in [0, 1): a
// carry of 1 or more means a whole particle was not spawned when it was earned, which is a bug, not a rounding
// choice.
struct EmissionSchedule final
{
    double ratePerSecond{};
    double carry{};

    [[nodiscard]] bool operator==(EmissionSchedule const &) const noexcept = default;
};

struct EmissionTick final
{
    std::uint32_t spawnCount{};
    double carry{};

    [[nodiscard]] bool operator==(EmissionTick const &) const noexcept = default;
};

[[nodiscard]] std::expected<EmissionTick, ContractError> AccumulateEmission(EmissionSchedule const &schedule,
                                                                            double timestepSeconds) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// I. Motion, force fields, and integrators
// ---------------------------------------------------------------------------------------------------------------

struct MotionState final
{
    Float3 positionMetres{};
    Float3 velocityMetresPerSecond{};

    [[nodiscard]] bool operator==(MotionState const &) const noexcept = default;
};

// A constant acceleration plus a linear drag: a = gravity - drag * velocity. Two terms is the smallest field that
// still distinguishes a conservative force from a dissipative one, which is the distinction the energy diagnostics
// below depend on.
struct ForceField final
{
    Float3 gravityMetresPerSecondSquared{};
    double linearDragPerSecond{};

    [[nodiscard]] bool operator==(ForceField const &) const noexcept = default;
};

[[nodiscard]] std::expected<void, ContractError> ValidateForceField(ForceField const &field) noexcept;
[[nodiscard]] std::expected<void, ContractError> ValidateMotionState(MotionState const &state) noexcept;
[[nodiscard]] std::expected<Float3, ContractError> EvaluateAcceleration(ForceField const &field,
                                                                        Float3 velocityMetresPerSecond) noexcept;

enum class Integrator : std::uint8_t
{
    // x' = x + h*v; v' = v + h*a(v). Position uses the *old* velocity. First order, and it gains energy under a
    // constant field by exactly 0.5 * h^2 * |a|^2 per step.
    ExplicitEuler = 0U,
    // v' = v + h*a(v); x' = x + h*v'. Position uses the *new* velocity. Also first order in truncation, and it
    // loses energy under a constant field by exactly 0.5 * h^2 * |a|^2 per step. The sign of that drift, not its
    // order, is why it is the default in interactive simulation.
    SemiImplicitEuler,
    // x' = x + h*v + 0.5*h^2*a; v' = v + 0.5*h*(a + a'). With a velocity-independent force this is exact for a
    // constant field, which makes it the reference the other two are measured against -- and it is refused
    // outright when drag is non-zero, because the usual formulation would then need the new acceleration before
    // the new velocity exists.
    VelocityVerlet,
};

inline constexpr std::size_t kIntegratorCount = 3U;

[[nodiscard]] std::span<Integrator const> AllIntegrators() noexcept;
[[nodiscard]] std::string_view IntegratorName(Integrator integrator) noexcept;

[[nodiscard]] std::expected<MotionState, ContractError> IntegrateMotion(Integrator integrator, MotionState const &state,
                                                                        ForceField const &field,
                                                                        double timestepSeconds) noexcept;

// The exact solution of x'' = a for constant a. Not an integrator: it is what the integrators are wrong against.
[[nodiscard]] std::expected<MotionState, ContractError> AnalyticConstantAcceleration(MotionState const &initial,
                                                                                     Float3 accelerationMetres,
                                                                                     double elapsedSeconds) noexcept;

// The exact solution of v' = g - k*v, which is the whole field rather than only its constant part. It is a
// separate entry point because it needs an exponential and therefore is *not* exact in floating point, and a test
// that compares against it must say so.
[[nodiscard]] std::expected<MotionState, ContractError> AnalyticLinearDrag(MotionState const &initial,
                                                                           ForceField const &field,
                                                                           double elapsedSeconds) noexcept;

// Specific mechanical energy, joules per kilogram: 0.5*|v|^2 - dot(gravity, position). Refused when the field
// dissipates, because energy drift only measures the integrator when the physics conserves it; with drag, the
// exact solution loses energy too and the number stops separating the two causes.
[[nodiscard]] std::expected<double, ContractError> SpecificEnergy(MotionState const &state,
                                                                  ForceField const &field) noexcept;

struct IntegrationDiagnostics final
{
    MotionState finalState{};
    MotionState referenceState{};
    double positionErrorMetres{};
    double velocityErrorMetresPerSecond{};
    double elapsedSeconds{};
    std::uint32_t stepCount{};
    // Present only for a conservative field. `measuredEnergyDrift` is the end-minus-start difference the run
    // actually produced; `predictedEnergyDrift` is the closed form for this integrator under a constant field:
    // +n*0.5*h^2*|a|^2 for explicit Euler, the negative of that for semi-implicit, and exactly zero for velocity
    // Verlet. Publishing both is what turns "the simulation gained energy" into "the integrator gained exactly the
    // energy its update rule says it must".
    std::optional<double> measuredEnergyDrift{};
    std::optional<double> predictedEnergyDrift{};

    [[nodiscard]] bool operator==(IntegrationDiagnostics const &) const noexcept = default;
};

[[nodiscard]] std::expected<IntegrationDiagnostics, ContractError> IntegrateAndCompare(
    Integrator integrator, MotionState const &initial, ForceField const &field, double timestepSeconds,
    std::uint32_t stepCount) noexcept;

// log2(error(h) / error(h/2)) over the same total time. One for a first-order scheme. Refused, rather than
// reported as infinity, when the coarse error is zero -- an exact scheme has no observable convergence order, and
// saying so is more useful than dividing by zero.
[[nodiscard]] std::expected<double, ContractError> EstimateConvergenceOrder(Integrator integrator,
                                                                            MotionState const &initial,
                                                                            ForceField const &field,
                                                                            double baseTimestepSeconds,
                                                                            std::uint32_t baseStepCount) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// J. Collision
//
// One ground plane, y >= height, resolved by projection. Three honesty notes, because this is the part of a
// particle system most likely to be described as more than it is:
//
//   * This is discrete collision detection. The test is on the position the integrator *produced*, so a particle
//     never tunnels through this half-space no matter how fast it moves -- a half-space has no far side to reach.
//     A thin slab, a moving plane, or a triangle mesh is a different problem entirely, and a fixed-step position
//     test *will* miss those; the chapter does not model them rather than modelling them badly.
//   * The position correction places the particle on the plane. It does not rewind to the time of impact and
//     re-integrate the remainder of the step, so even with restitution 1 the bounce loses the sub-step travel that
//     the correction discarded. `penetrationDepthMetres` is reported so that loss is measurable rather than
//     mysterious.
//   * The tangential term is a velocity scale, not Coulomb friction: there is no normal force, no friction cone,
//     and no distinction between sticking and sliding. It is named `friction` because that is what the parameter
//     does to the picture, and its limitations are stated here so a learner does not carry it into a rigid-body
//     chapter.
// ---------------------------------------------------------------------------------------------------------------

struct GroundPlane final
{
    bool enabled{};
    double heightMetres{};
    double restitution{};
    double friction{};
    // Below this outgoing normal speed the bounce is dropped and the particle is placed at rest on the plane. It
    // exists because a geometric series of ever-smaller bounces never terminates in a fixed-step integrator; it is
    // a stability device with a named cost, not a physical effect.
    double restingSpeedMetresPerSecond{};

    [[nodiscard]] bool operator==(GroundPlane const &) const noexcept = default;
};

[[nodiscard]] std::expected<void, ContractError> ValidateGroundPlane(GroundPlane const &plane) noexcept;

struct CollisionOutcome final
{
    MotionState state{};
    bool contacted{};
    bool cameToRest{};
    double penetrationDepthMetres{};
    double normalSpeedBeforeMetresPerSecond{};
    double normalSpeedAfterMetresPerSecond{};

    [[nodiscard]] bool operator==(CollisionOutcome const &) const noexcept = default;
};

[[nodiscard]] std::expected<CollisionOutcome, ContractError> ResolveGroundCollision(GroundPlane const &plane,
                                                                                    MotionState const &state) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// K. Fixed-step scheduling
// ---------------------------------------------------------------------------------------------------------------

// What to do with simulation time the substep limit refuses to run.
enum class SubstepOverflowPolicy : std::uint8_t
{
    // Throw the excess away, keeping only the sub-step remainder. The simulation runs slower than wall clock and
    // says how much time it discarded.
    DropExcessTime = 0U,
    // Keep the excess in the accumulator and catch up over later frames. Bounded by `kMaximumAccumulatorSeconds`,
    // past which the frame is refused: an unbounded backlog is the spiral of death, and riding it silently is how
    // a simulation stops responding.
    CarryExcessTime,
};

struct FixedStepSettings final
{
    double fixedTimestepSeconds{};
    std::uint32_t maximumSubstepCount{};
    double maximumFrameDeltaSeconds{};
    SubstepOverflowPolicy overflowPolicy{SubstepOverflowPolicy::DropExcessTime};

    [[nodiscard]] bool operator==(FixedStepSettings const &) const noexcept = default;
};

[[nodiscard]] std::expected<void, ContractError> ValidateFixedStepSettings(FixedStepSettings const &settings) noexcept;

struct FixedStepPlan final
{
    std::uint32_t substepCount{};
    double clampedFrameDeltaSeconds{};
    double consumedSeconds{};
    double carrySeconds{};
    // Time the frame delta clamp refused. It is simulation time that never entered the accumulator.
    double droppedByClampSeconds{};
    // Time the substep limit refused. It entered the accumulator and was then discarded.
    double droppedBySubstepLimitSeconds{};
    bool frameDeltaWasClamped{};
    bool substepLimitReached{};

    [[nodiscard]] bool operator==(FixedStepPlan const &) const noexcept = default;
};

// `accumulatorSeconds` is the carry from the previous frame; the plan reports the new carry rather than mutating
// anything, so a caller can plan a frame without committing to it.
[[nodiscard]] std::expected<FixedStepPlan, ContractError> PlanFixedSteps(FixedStepSettings const &settings,
                                                                         double accumulatorSeconds,
                                                                         double frameDeltaSeconds) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// L. The particle system: state, reset, and one frame
//
// The canonical per-substep order, which every test in this chapter depends on and which a GPU implementation must
// reproduce pass for pass:
//
//   1. Emission. Free slots are allocated in the policy's order, identities are issued monotonically, and the
//      initial state is seeded from (seed, identity). Emission writes in place, into the slots the simulation is
//      about to read.
//   2. Simulation, for every live particle including the ones just emitted:
//      a. acceleration from the field at the *current* velocity;
//      b. the integrator's position and velocity update;
//      c. the ground collision response, applied to the post-integration state;
//      d. age += h;
//      e. death when age >= lifetime, which frees the slot for the *next* substep's emission.
//
// Every one of those is a choice with a visible consequence. Emitting before simulating means a particle moves on
// the substep it is born; ageing after collision means a particle that dies this substep still bounced; freeing
// after death means a slot recycles one substep later, which on the GPU is the barrier between the simulation
// dispatch and the next emission dispatch. Swapping any of them changes the reference, which is why each is
// pinned by a test rather than left to the implementation.
// ---------------------------------------------------------------------------------------------------------------

struct Particle final
{
    ParticleId identity{kInvalidParticleId};
    MotionState motion{};
    double ageSeconds{};
    double lifetimeSeconds{};
    bool alive{};

    [[nodiscard]] bool operator==(Particle const &) const noexcept = default;
};

// Lifetime totals. `requested` counts what emission asked for, `spawned` what it placed, `dropped` what capacity
// refused, `died` what reached its lifetime. The accounting identity the chapter defends is
//     requested == spawned + dropped   and   spawned == live + died,
// hence requested == live + died + dropped. `live` is not stored: it is counted from the slots, so a corrupted
// counter cannot hide a corrupted array.
struct SystemCounters final
{
    std::uint64_t requestedCount{};
    std::uint64_t spawnedCount{};
    std::uint64_t droppedByCapacityCount{};
    std::uint64_t diedCount{};
    std::uint64_t contactCount{};

    [[nodiscard]] bool operator==(SystemCounters const &) const noexcept = default;
};

struct AccountingReport final
{
    std::uint64_t requestedCount{};
    std::uint64_t spawnedCount{};
    std::uint64_t droppedByCapacityCount{};
    std::uint64_t diedCount{};
    std::uint64_t liveCount{};

    [[nodiscard]] bool operator==(AccountingReport const &) const noexcept = default;
};

// Which of the two state buffers a frame reads and which it writes.
enum class BufferSlot : std::uint8_t
{
    A = 0U,
    B,
};

[[nodiscard]] BufferSlot OtherSlot(BufferSlot slot) noexcept;

struct FrameSlots final
{
    BufferSlot readSlot{BufferSlot::A};
    BufferSlot writeSlot{BufferSlot::B};

    [[nodiscard]] bool operator==(FrameSlots const &) const noexcept = default;
};

[[nodiscard]] std::expected<void, ContractError> ValidateFrameSlots(FrameSlots const &slots) noexcept;
[[nodiscard]] FrameSlots AdvanceFrameSlots(FrameSlots const &slots) noexcept;

struct ParticleSystemState final
{
    std::uint32_t capacity{};
    std::vector<Particle> slots{};
    ParticleId nextIdentity{};
    double emissionCarry{};
    double accumulatorSeconds{};
    SystemCounters counters{};
    std::uint64_t seed{};
    std::uint64_t frameIndex{};
    FrameSlots slotOwnership{};
};

// A reset is a *value*, not a mutation of whatever was there: two resets with the same capacity and seed produce
// states that compare equal and checksum equal. Identity restarts at zero, which is exactly why a checksum can
// compare two runs at all.
[[nodiscard]] std::expected<ParticleSystemState, ContractError> ResetParticleSystem(std::uint32_t capacity,
                                                                                    std::uint64_t seed);

[[nodiscard]] std::expected<void, ContractError> ValidateParticleSystem(ParticleSystemState const &state) noexcept;
[[nodiscard]] std::uint32_t CountLiveParticles(ParticleSystemState const &state) noexcept;
[[nodiscard]] std::expected<AccountingReport, ContractError> ValidateAccounting(
    ParticleSystemState const &state) noexcept;

// The free slots, in the policy's order. This is the CPU model of the dead list, and it is a scan over the alive
// flags rather than a counter, which is the only formulation whose output order is defined.
[[nodiscard]] std::expected<std::vector<std::uint32_t>, ContractError> BuildFreeSlotList(
    ParticleSystemState const &state, DeadSlotPolicy policy);

struct EmissionRequest final
{
    std::uint32_t requestedCount{};
    EmissionSettings settings{};

    [[nodiscard]] bool operator==(EmissionRequest const &) const noexcept = default;
};

struct EmissionResult final
{
    std::uint32_t spawnedCount{};
    std::uint32_t droppedByCapacityCount{};
    std::optional<ParticleId> firstIdentity{};
    std::optional<ParticleId> lastIdentity{};
    std::vector<std::uint32_t> slots{};

    [[nodiscard]] bool operator==(EmissionResult const &) const noexcept = default;
};

// Emits into `state`. Capacity saturation is counted, never an error. Identity exhaustion *is* an error, and it is
// an all-or-nothing one: when the remaining identity space cannot cover the whole request the state is left
// exactly as it was, so a caller can retry after a reset without wondering how much got through.
[[nodiscard]] std::expected<EmissionResult, ContractError> EmitParticles(ParticleSystemState &state,
                                                                         EmissionRequest const &request);

struct SubstepReport final
{
    std::uint32_t spawnedCount{};
    std::uint32_t droppedByCapacityCount{};
    std::uint32_t diedCount{};
    std::uint32_t contactCount{};
    std::uint32_t liveCountAfter{};

    [[nodiscard]] bool operator==(SubstepReport const &) const noexcept = default;
};

struct FrameSettings final
{
    FixedStepSettings step{};
    EmissionSettings emission{};
    ForceField field{};
    Integrator integrator{Integrator::SemiImplicitEuler};
    GroundPlane ground{};

    [[nodiscard]] bool operator==(FrameSettings const &) const noexcept = default;
};

struct FrameReport final
{
    FixedStepPlan plan{};
    std::vector<SubstepReport> substeps{};
    std::uint32_t spawnedCount{};
    std::uint32_t droppedByCapacityCount{};
    std::uint32_t diedCount{};
    std::uint32_t contactCount{};
    std::uint32_t liveCount{};
    std::uint64_t checksum{};
    // The slots the *last* substep used. Each substep flips the pair, exactly as a GPU implementation must, so a
    // frame that ran an even number of substeps ends with the pair it started with.
    FrameSlots slotsUsed{};
    // Where the frame's result actually lives. It is the last substep's write slot, or -- when the frame ran no
    // substeps at all -- the slot the frame started reading. A render stage that assumes the answer is always in
    // the write slot draws last frame's particles on any frame the scheduler decided to skip.
    BufferSlot resultSlot{BufferSlot::A};

    [[nodiscard]] bool operator==(FrameReport const &) const noexcept = default;
};

// Advances one frame. On failure `state` is untouched, so a rejected frame cannot leave a half-simulated system
// behind; the reference buys that guarantee with a copy, which is the right trade for a teaching model and the
// wrong one for a shipping simulation.
[[nodiscard]] std::expected<FrameReport, ContractError> SimulateFrame(ParticleSystemState &state,
                                                                      FrameSettings const &settings,
                                                                      double frameDeltaSeconds);

// FNV-1a over the float32 bit patterns of every slot, in slot order: identity, alive flag, position, velocity,
// age, lifetime. It is the float32 image *because* that is what a GPU buffer would hold, so the same checksum can
// one day be computed from a readback. Non-finite state is refused rather than checksummed, because a NaN has many
// bit patterns and a checksum over one of them proves nothing.
[[nodiscard]] std::expected<std::uint64_t, ContractError> ChecksumParticleState(
    ParticleSystemState const &state) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// M. Compaction and dispatch
//
// The compaction is an exclusive prefix scan over the alive flags followed by a scatter, which is the formulation
// a GPU actually uses. Its output order is ascending source slot -- a *stable* order -- and that is a decision,
// not an accident: an append-counter compaction produces the same set in an order that changes run to run, and
// every diagnostic that compares two frames needs the stable one.
// ---------------------------------------------------------------------------------------------------------------

struct CompactionInput final
{
    std::span<std::uint32_t const> aliveFlags{};
    std::span<ParticleId const> identities{};
    std::uint32_t outputCapacity{};
};

struct CompactionResult final
{
    // One entry per source slot: how many live particles precede it. Defined for dead slots too, which is what
    // makes it a scan rather than a filtered list.
    std::vector<std::uint32_t> exclusiveOffsets{};
    std::vector<std::uint32_t> compactedSlots{};
    std::vector<ParticleId> compactedIdentities{};
    std::uint32_t aliveCount{};
    std::uint32_t emittedCount{};
    std::uint32_t droppedCount{};
    bool overflowed{};

    [[nodiscard]] bool operator==(CompactionResult const &) const noexcept = default;
};

[[nodiscard]] std::expected<CompactionResult, ContractError> CompactAliveSlots(CompactionInput const &input);
[[nodiscard]] std::expected<CompactionResult, ContractError> CompactParticleSystem(ParticleSystemState const &state,
                                                                                   std::uint32_t outputCapacity);

struct DispatchDimensions final
{
    std::uint32_t threadGroupCountX{};
    std::uint32_t threadsPerGroup{};
    std::uint32_t dispatchedThreadCount{};
    // Threads in the final group that have no work item. A shader must discard them, which is why the number is
    // reported instead of being left as a comment in a shader nobody reads.
    std::uint32_t tailThreadCount{};
    bool requiresBoundsGuard{};

    [[nodiscard]] bool operator==(DispatchDimensions const &) const noexcept = default;
};

// A zero work-item count is legal and produces zero thread groups: an empty particle system dispatches nothing,
// and the caller skips the pass rather than launching one group that immediately returns.
[[nodiscard]] std::expected<DispatchDimensions, ContractError> ComputeDispatchDimensions(
    std::uint32_t workItemCount, std::uint32_t threadsPerGroup) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// N. Indirect draw arguments
// ---------------------------------------------------------------------------------------------------------------

// D3D12_DRAW_ARGUMENTS, field for field and in order. The static assert below is the contract: a compute shader
// writes four `uint`s at this stride and the command signature reads them back.
struct DrawInstancedArguments final
{
    std::uint32_t vertexCountPerInstance{};
    std::uint32_t instanceCount{};
    std::uint32_t startVertexLocation{};
    std::uint32_t startInstanceLocation{};

    [[nodiscard]] bool operator==(DrawInstancedArguments const &) const noexcept = default;
};

static_assert(sizeof(DrawInstancedArguments) == kDrawArgumentsSizeBytes);
static_assert(alignof(DrawInstancedArguments) == 4U);

struct IndirectDrawRequest final
{
    // The live count as the *GPU* knows it: the compaction counter, not a number the CPU read back.
    std::uint32_t liveCount{};
    // How many instances the bound vertex data and the compacted index list can actually address.
    std::uint32_t drawCapacity{};
    std::uint32_t vertexCountPerParticle{};
    std::uint32_t startVertexLocation{};
    std::uint32_t startInstanceLocation{};

    [[nodiscard]] bool operator==(IndirectDrawRequest const &) const noexcept = default;
};

struct IndirectDrawArgumentsResult final
{
    DrawInstancedArguments arguments{};
    std::uint32_t clampedInstanceCount{};
    bool clamped{};

    [[nodiscard]] bool operator==(IndirectDrawArgumentsResult const &) const noexcept = default;
};

// Clamping is a counted outcome, not an error: a live set larger than the draw can address is a real state of a
// bounded system, and refusing the frame would hide it behind an exception path nobody reads.
[[nodiscard]] std::expected<IndirectDrawArgumentsResult, ContractError> BuildIndirectDrawArguments(
    IndirectDrawRequest const &request) noexcept;

struct IndirectArgumentBufferLayout final
{
    std::uint64_t argumentOffsetBytes{};
    std::uint64_t strideBytes{kDrawArgumentsSizeBytes};
    std::uint32_t maximumCommandCount{};
    std::optional<std::uint64_t> countOffsetBytes{};
    std::uint64_t bufferSizeBytes{};

    [[nodiscard]] bool operator==(IndirectArgumentBufferLayout const &) const noexcept = default;
};

// Returns the last byte the layout touches. Mirrors the runtime validation `ExecuteIndirect` documents: both
// offsets 4-byte aligned, and `offset + maximumCommandCount * stride` inside the buffer.
[[nodiscard]] std::expected<std::uint64_t, ContractError> ValidateIndirectArgumentBufferLayout(
    IndirectArgumentBufferLayout const &layout) noexcept;

// `min(count buffer value, MaxCommandCount)`, exactly as `ExecuteIndirect` defines it. A missing count buffer
// means the maximum is the exact count.
[[nodiscard]] std::expected<std::uint32_t, ContractError> ResolveExecutedCommandCount(
    std::uint32_t maximumCommandCount, std::optional<std::uint32_t> countBufferValue) noexcept;

// Where the live count the draw uses came from. The steady-state frame must say `GpuCounterBuffer`: a readback
// costs a full pipeline round trip and the whole point of writing arguments on the GPU is to not pay it.
enum class LiveCountSource : std::uint8_t
{
    GpuCounterBuffer = 0U,
    CpuReadback,
};

struct FramePlan final
{
    FrameSlots slots{};
    LiveCountSource liveCountSource{LiveCountSource::GpuCounterBuffer};
    // A diagnostic frame is allowed to stall: it is the frame a test uses to prove the GPU agrees with the CPU
    // reference. Marking it is what keeps the *steady-state* frame honest.
    bool diagnosticFrame{};
    bool readbackRequested{};

    [[nodiscard]] bool operator==(FramePlan const &) const noexcept = default;
};

[[nodiscard]] std::expected<void, ContractError> ValidateFramePlan(FramePlan const &plan) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// O. Stage evidence, ping-pong ownership, and barriers
// ---------------------------------------------------------------------------------------------------------------

enum class StageOutcome : std::uint8_t
{
    // The stage ran and published evidence.
    Submitted = 0U,
    // The stage was deliberately not run this frame: nothing to emit, nothing alive to draw, no readback wanted.
    // A skipped stage is a decision and must publish no evidence.
    Skipped,
    // The stage could not run: the device, the feature, or the resource is not there. Distinguished from Skipped
    // because "we chose not to" and "we could not" have different fixes and the frame that produced them looks the
    // same on screen.
    Unavailable,
};

struct StageOwnership final
{
    BufferSlot readSlot{BufferSlot::A};
    // Absent when the stage writes no particle-state slot at all: compaction writes an index list, the render
    // stage writes pixels. Saying "no state slot" explicitly is what stops a read-only stage from being described
    // with a write it does not perform.
    std::optional<BufferSlot> writeSlot{};
    // Reset is the only stage that legitimately writes both slots, because it is the only stage whose output is
    // not a function of a previous slot.
    bool writesBothSlots{};

    [[nodiscard]] bool operator==(StageOwnership const &) const noexcept = default;
};

struct StageRecord final
{
    FrameStage stage{FrameStage::Reset};
    StageOutcome outcome{StageOutcome::Skipped};
    bool recorded{};
    // How many checked facts the stage published: particles emitted, substeps run, slots compacted, commands
    // written. A submitted stage with no evidence has not been exercised and is refused.
    std::uint32_t evidenceCount{};
    std::optional<StageOwnership> ownership{};

    [[nodiscard]] bool operator==(StageRecord const &) const noexcept = default;
};

// The three masks partition the recorded stages: every recorded stage appears in exactly one, and no stage appears
// in two. `recordedMask` is their union, and a complete frame has all seven bits.
struct StagePartition final
{
    std::uint32_t submittedMask{};
    std::uint32_t skippedMask{};
    std::uint32_t unavailableMask{};
    std::uint32_t recordedMask{};

    [[nodiscard]] bool operator==(StagePartition const &) const noexcept = default;
};

[[nodiscard]] std::uint32_t StageBit(FrameStage stage) noexcept;
inline constexpr std::uint32_t kAllFrameStagesMask = (1U << kFrameStageCount) - 1U;

// The partition invariants, checkable on a partition from anywhere: the three masks are pairwise disjoint, their
// union is the recorded mask, and a frame that claims to be complete has recorded all seven stages. It is a free
// function rather than a ledger method because the masks a GPU implementation reports come from its own
// bookkeeping, and those are exactly the ones worth checking.
[[nodiscard]] std::expected<void, ContractError> ValidateStagePartition(StagePartition const &partition,
                                                                        bool requireComplete) noexcept;

// Records the seven stages in order. The ledger is this chapter's answer to "nothing is on screen": it names the
// first stage that did not submit, refuses a stage recorded out of order, refuses a submitted stage that published
// nothing, and refuses to call a frame renderable until the stages that must run have run.
class FrameStageLedger final
{
  public:
    FrameStageLedger() noexcept;

    [[nodiscard]] std::expected<void, ContractError> RecordSubmitted(FrameStage stage, std::uint32_t evidenceCount,
                                                                     std::optional<StageOwnership> ownership) noexcept;
    [[nodiscard]] std::expected<void, ContractError> RecordSkipped(FrameStage stage) noexcept;
    [[nodiscard]] std::expected<void, ContractError> RecordUnavailable(FrameStage stage) noexcept;

    [[nodiscard]] StageRecord Status(FrameStage stage) const noexcept;
    [[nodiscard]] std::optional<FrameStage> NextExpectedStage() const noexcept;
    [[nodiscard]] bool IsComplete() const noexcept;
    [[nodiscard]] std::uint32_t TotalEvidenceCount() const noexcept;
    // Refused until every stage has been recorded: a partition of a frame that is still being recorded would name
    // stages that have not decided yet, and a mask that means "not yet" is the one a diagnostic misreads.
    [[nodiscard]] std::expected<StagePartition, ContractError> Partition() const noexcept;

  private:
    [[nodiscard]] std::expected<void, ContractError> Accept(FrameStage stage) noexcept;

    std::array<StageRecord, kFrameStageCount> stages_{};
    std::size_t nextStageIndex_{};
};

// Checks the ping-pong ownership the ledger recorded against the frame's declared slots:
//   * Reset writes both slots and reads neither.
//   * Emission writes in place into the read slot, so its read and write slots are the same one.
//   * Simulation reads the read slot and writes the write slot, and they must differ.
//   * Compaction and Render read the write slot and write no state slot at all.
//   * IndirectArguments and Readback touch no state slot and must declare no ownership.
// A stage that reads and writes the same state slot when it must not is a read-write hazard that on the GPU shows
// up as a different answer per wave scheduling, which is the worst kind of bug to find late.
[[nodiscard]] std::expected<void, ContractError> ValidateStageOwnership(FrameStageLedger const &ledger,
                                                                        FrameSlots const &slots) noexcept;

enum class ParticleResource : std::uint8_t
{
    StateSlotA = 0U,
    StateSlotB,
    FreeList,
    LiveIndexList,
    CounterBuffer,
    IndirectArgumentBuffer,
    ReadbackBuffer,
};

inline constexpr std::size_t kParticleResourceCount = 7U;

[[nodiscard]] std::string_view ParticleResourceName(ParticleResource resource) noexcept;

// Enhanced-barrier vocabulary, mirrored as chapter-local flag enums so the contracts stay header-free. The values
// are not the D3D12 bit values -- nothing here is serialized into a D3D12 structure -- but the names are exactly
// the D3D12 ones, because the whole point is that a learner recognises them in `D3D12_BARRIER_SYNC` and
// `D3D12_BARRIER_ACCESS` when the lab writes the real thing.
enum class BarrierSync : std::uint16_t
{
    None = 0U,
    ComputeShading = 1U << 0U,
    Draw = 1U << 1U,
    ExecuteIndirect = 1U << 2U,
    Copy = 1U << 3U,
    All = 1U << 4U,
};

enum class BarrierAccess : std::uint16_t
{
    Common = 0U,
    UnorderedAccess = 1U << 0U,
    ShaderResource = 1U << 1U,
    IndirectArgument = 1U << 2U,
    CopySource = 1U << 3U,
    CopyDest = 1U << 4U,
    NoAccess = 1U << 5U,
};

[[nodiscard]] constexpr BarrierSync operator|(BarrierSync left, BarrierSync right) noexcept
{
    return static_cast<BarrierSync>(static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
}

[[nodiscard]] constexpr BarrierAccess operator|(BarrierAccess left, BarrierAccess right) noexcept
{
    return static_cast<BarrierAccess>(static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
}

[[nodiscard]] constexpr bool HasFlag(BarrierSync value, BarrierSync flag) noexcept
{
    return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(flag)) == static_cast<std::uint16_t>(flag);
}

[[nodiscard]] constexpr bool HasFlag(BarrierAccess value, BarrierAccess flag) noexcept
{
    return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(flag)) == static_cast<std::uint16_t>(flag);
}

// One `ID3D12GraphicsCommandList7::Barrier` buffer barrier, named by the stage whose access precedes it and the
// stage that consumes the data after it. Naming both ends is what lets the validator say *which* barrier is
// missing instead of counting them.
struct ResourceTransition final
{
    ParticleResource resource{ParticleResource::StateSlotA};
    FrameStage producer{FrameStage::Reset};
    FrameStage consumer{FrameStage::Reset};
    BarrierSync syncBefore{BarrierSync::None};
    BarrierAccess accessBefore{BarrierAccess::Common};
    BarrierSync syncAfter{BarrierSync::None};
    BarrierAccess accessAfter{BarrierAccess::Common};

    [[nodiscard]] bool operator==(ResourceTransition const &) const noexcept = default;
};

// The transitions a frame with these slots must present, in the order the frame records them. The state-slot
// entries are resolved from the frame's read and write slots, so the same required list describes both parities
// and a test can flip the parity and watch the required resources swap.
[[nodiscard]] std::vector<ResourceTransition> RequiredFrameTransitions(FrameSlots const &slots, bool readbackRequested);

struct BarrierValidation final
{
    std::uint32_t matchedCount{};
    std::uint32_t suppliedCount{};

    [[nodiscard]] bool operator==(BarrierValidation const &) const noexcept = default;
};

// Every required transition must be supplied exactly once with exactly the stated sync and access on both sides,
// and nothing else may be supplied: an unrecognised transition is refused rather than ignored, because a barrier
// nobody models is a barrier nobody reviews.
[[nodiscard]] std::expected<BarrierValidation, ContractError> ValidateFrameBarriers(
    std::span<ResourceTransition const> transitions, FrameSlots const &slots, bool readbackRequested);

} // namespace ch34::particles
