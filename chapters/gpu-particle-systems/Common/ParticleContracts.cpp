#include "ParticleContracts.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace ch34::particles
{
namespace
{

// One row per `ContractError`. The table is the single source of truth for the enumerator list, the printable
// name, and the stage attribution, so the three can never disagree. A `std::nullopt` stage is a deliberate
// statement that the error genuinely occurs at more than one stage; it is not a placeholder for "unclassified".
struct ErrorTableRow final
{
    ContractError error{};
    std::string_view name{};
    std::optional<FrameStage> stage{};
};

constexpr std::optional<FrameStage> kShared{};
constexpr std::optional<FrameStage> kReset{FrameStage::Reset};
constexpr std::optional<FrameStage> kEmission{FrameStage::Emission};
constexpr std::optional<FrameStage> kSimulation{FrameStage::Simulation};
constexpr std::optional<FrameStage> kCompaction{FrameStage::Compaction};
constexpr std::optional<FrameStage> kIndirect{FrameStage::IndirectArguments};

constexpr std::array<ErrorTableRow, kContractErrorCount> kErrorTable{{
    {ContractError::NonFinite, "NonFinite", kShared},
    {ContractError::ArithmeticOverflow, "ArithmeticOverflow", kShared},
    {ContractError::AlignmentNotPowerOfTwo, "AlignmentNotPowerOfTwo", kShared},
    {ContractError::ZeroAlignment, "ZeroAlignment", kShared},

    {ContractError::ZeroCapacity, "ZeroCapacity", kReset},
    {ContractError::CapacityExceeded, "CapacityExceeded", kReset},
    {ContractError::UnknownAttribute, "UnknownAttribute", kReset},
    {ContractError::BufferTooSmall, "BufferTooSmall", kReset},
    {ContractError::OffsetMisaligned, "OffsetMisaligned", kReset},
    {ContractError::OffsetNotStrideMultiple, "OffsetNotStrideMultiple", kReset},
    {ContractError::ElementIndexOutOfRange, "ElementIndexOutOfRange", kReset},
    {ContractError::StateSizeMismatch, "StateSizeMismatch", kReset},

    {ContractError::ParticleIdSpaceExhausted, "ParticleIdSpaceExhausted", kEmission},
    {ContractError::LiveSlotHasInvalidId, "LiveSlotHasInvalidId", kShared},
    {ContractError::DeadSlotHasValidId, "DeadSlotHasValidId", kShared},
    {ContractError::DuplicateParticleId, "DuplicateParticleId", kShared},
    {ContractError::IdentityNotMonotonic, "IdentityNotMonotonic", kEmission},

    {ContractError::NegativeEmissionRate, "NegativeEmissionRate", kEmission},
    {ContractError::EmissionRateTooLarge, "EmissionRateTooLarge", kEmission},
    {ContractError::NonPositiveLifetime, "NonPositiveLifetime", kEmission},
    {ContractError::LifetimeTooLarge, "LifetimeTooLarge", kEmission},
    {ContractError::NegativeJitter, "NegativeJitter", kEmission},
    {ContractError::JitterTooLarge, "JitterTooLarge", kEmission},
    {ContractError::EmissionCarryOutOfRange, "EmissionCarryOutOfRange", kEmission},
    {ContractError::SpawnCountExceedsLimit, "SpawnCountExceedsLimit", kEmission},
    {ContractError::UnknownDeadSlotPolicy, "UnknownDeadSlotPolicy", kEmission},

    {ContractError::NegativeFrameDelta, "NegativeFrameDelta", kSimulation},
    {ContractError::FrameDeltaTooLarge, "FrameDeltaTooLarge", kSimulation},
    {ContractError::FixedTimestepOutOfRange, "FixedTimestepOutOfRange", kSimulation},
    {ContractError::SubstepLimitOutOfRange, "SubstepLimitOutOfRange", kSimulation},
    {ContractError::FrameDeltaClampOutOfRange, "FrameDeltaClampOutOfRange", kSimulation},
    {ContractError::NegativeAccumulator, "NegativeAccumulator", kSimulation},
    {ContractError::SimulationTimeBacklogExceeded, "SimulationTimeBacklogExceeded", kSimulation},
    {ContractError::UnknownOverflowPolicy, "UnknownOverflowPolicy", kSimulation},

    {ContractError::UnknownIntegrator, "UnknownIntegrator", kSimulation},
    {ContractError::NegativeDragCoefficient, "NegativeDragCoefficient", kSimulation},
    {ContractError::DragCoefficientTooLarge, "DragCoefficientTooLarge", kSimulation},
    {ContractError::VerletRequiresVelocityIndependentForce, "VerletRequiresVelocityIndependentForce", kSimulation},
    {ContractError::NonPositiveTimestep, "NonPositiveTimestep", kSimulation},
    {ContractError::PositionOutOfDomain, "PositionOutOfDomain", kSimulation},
    {ContractError::SpeedOutOfDomain, "SpeedOutOfDomain", kSimulation},
    {ContractError::AccelerationOutOfDomain, "AccelerationOutOfDomain", kSimulation},
    {ContractError::ZeroStepCount, "ZeroStepCount", kSimulation},
    {ContractError::DegenerateConvergenceEstimate, "DegenerateConvergenceEstimate", kSimulation},
    {ContractError::EnergyUndefinedWithDissipation, "EnergyUndefinedWithDissipation", kSimulation},

    {ContractError::NegativeAge, "NegativeAge", kSimulation},
    {ContractError::AgeExceedsLifetime, "AgeExceedsLifetime", kSimulation},
    {ContractError::RestitutionOutOfRange, "RestitutionOutOfRange", kSimulation},
    {ContractError::FrictionOutOfRange, "FrictionOutOfRange", kSimulation},
    {ContractError::NegativeRestingSpeed, "NegativeRestingSpeed", kSimulation},
    {ContractError::PlaneHeightOutOfDomain, "PlaneHeightOutOfDomain", kSimulation},

    {ContractError::NonBinaryAliveFlag, "NonBinaryAliveFlag", kCompaction},
    {ContractError::ParallelArrayLengthMismatch, "ParallelArrayLengthMismatch", kCompaction},
    {ContractError::AliveCountMismatch, "AliveCountMismatch", kCompaction},
    {ContractError::ZeroThreadGroupSize, "ZeroThreadGroupSize", kShared},
    {ContractError::ThreadGroupSizeExceeded, "ThreadGroupSizeExceeded", kShared},
    {ContractError::ThreadGroupCountExceeded, "ThreadGroupCountExceeded", kShared},

    {ContractError::ZeroVertexCountPerParticle, "ZeroVertexCountPerParticle", kIndirect},
    {ContractError::VertexCountPerParticleTooLarge, "VertexCountPerParticleTooLarge", kIndirect},
    {ContractError::ZeroCommandCount, "ZeroCommandCount", kIndirect},
    {ContractError::CommandCountExceeded, "CommandCountExceeded", kIndirect},
    {ContractError::IndirectStrideMismatch, "IndirectStrideMismatch", kIndirect},
    {ContractError::ReadbackDependencyInSteadyState, "ReadbackDependencyInSteadyState", kIndirect},

    {ContractError::StageOutOfOrder, "StageOutOfOrder", kShared},
    {ContractError::StageAlreadyRecorded, "StageAlreadyRecorded", kShared},
    {ContractError::EvidenceRequired, "EvidenceRequired", kShared},
    {ContractError::StagePartitionOverlap, "StagePartitionOverlap", kShared},
    {ContractError::StagePartitionIncomplete, "StagePartitionIncomplete", kShared},
    {ContractError::PingPongAliasViolation, "PingPongAliasViolation", kSimulation},
    {ContractError::PingPongOwnershipMismatch, "PingPongOwnershipMismatch", kShared},
    {ContractError::UnknownTransition, "UnknownTransition", kShared},
    {ContractError::DuplicateTransition, "DuplicateTransition", kShared},
    {ContractError::IncorrectBarrierSync, "IncorrectBarrierSync", kShared},
    {ContractError::IncorrectBarrierAccess, "IncorrectBarrierAccess", kShared},
    {ContractError::MissingBarrier, "MissingBarrier", kShared},

    {ContractError::AccountingMismatch, "AccountingMismatch", kShared},
    {ContractError::CounterOverflow, "CounterOverflow", kShared},
}};

constexpr std::array<ContractError, kContractErrorCount> MakeErrorList()
{
    std::array<ContractError, kContractErrorCount> errors{};
    for (std::size_t index = 0U; index < kContractErrorCount; ++index)
    {
        errors[index] = kErrorTable[index].error;
    }
    return errors;
}

constexpr std::array<ContractError, kContractErrorCount> kAllErrors = MakeErrorList();

constexpr std::array<std::string_view, kFrameStageCount> kStageNames{
    "Reset", "Emission", "Simulation", "Compaction", "IndirectArguments", "Render", "Readback"};

constexpr std::array<ParticleAttribute, kParticleAttributeCount> kAllAttributes{
    ParticleAttribute::Position, ParticleAttribute::Velocity, ParticleAttribute::Age,
    ParticleAttribute::Lifetime, ParticleAttribute::Id,       ParticleAttribute::Alive};

constexpr std::array<std::string_view, kParticleAttributeCount> kAttributeNames{"Position", "Velocity", "Age",
                                                                                "Lifetime", "Id",       "Alive"};

constexpr std::array<Integrator, kIntegratorCount> kAllIntegrators{
    Integrator::ExplicitEuler, Integrator::SemiImplicitEuler, Integrator::VelocityVerlet};

constexpr std::array<std::string_view, kIntegratorCount> kIntegratorNames{"ExplicitEuler", "SemiImplicitEuler",
                                                                          "VelocityVerlet"};

constexpr std::array<std::string_view, kParticleResourceCount> kResourceNames{
    "StateSlotA",    "StateSlotB", "FreeList", "LiveIndexList", "CounterBuffer", "IndirectArgumentBuffer",
    "ReadbackBuffer"};

[[nodiscard]] bool IsKnownAttribute(ParticleAttribute attribute) noexcept
{
    return static_cast<std::size_t>(attribute) < kParticleAttributeCount;
}

[[nodiscard]] bool IsKnownIntegrator(Integrator integrator) noexcept
{
    return static_cast<std::size_t>(integrator) < kIntegratorCount;
}

[[nodiscard]] bool IsKnownDeadSlotPolicy(DeadSlotPolicy policy) noexcept
{
    return policy == DeadSlotPolicy::AscendingIndex || policy == DeadSlotPolicy::DescendingIndex;
}

[[nodiscard]] bool IsKnownOverflowPolicy(SubstepOverflowPolicy policy) noexcept
{
    return policy == SubstepOverflowPolicy::DropExcessTime || policy == SubstepOverflowPolicy::CarryExcessTime;
}

// The 32-bit finalizer the seeding uses. Integer-only and multiplication-wrapping, so an HLSL `uint` port
// reproduces it bit for bit; that reproducibility is the entire reason the emitter is seeded this way instead of
// carrying an RNG state from particle to particle.
[[nodiscard]] std::uint32_t Finalize32(std::uint32_t value) noexcept
{
    value ^= value >> 16U;
    value *= 0x7FEB'352DU;
    value ^= value >> 15U;
    value *= 0x846C'A68BU;
    value ^= value >> 16U;
    return value;
}

[[nodiscard]] bool WithinAbsolute(double value, double bound) noexcept
{
    return std::isfinite(value) && std::abs(value) <= bound;
}

[[nodiscard]] bool WithinAbsolute(Float3 value, double bound) noexcept
{
    return WithinAbsolute(value.x, bound) && WithinAbsolute(value.y, bound) && WithinAbsolute(value.z, bound);
}

[[nodiscard]] std::uint64_t HashBytes(std::uint64_t hash, std::uint32_t value) noexcept
{
    constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
    {
        hash ^= static_cast<std::uint64_t>((value >> shift) & 0xFFU);
        hash *= kPrime;
    }
    return hash;
}

// The float32 image of a double, as a 32-bit pattern. `std::bit_cast` rather than a reinterpretation, so there is
// no aliasing question and no unsafe cast anywhere in the checksum path.
[[nodiscard]] std::uint32_t FloatBits(double value) noexcept
{
    return std::bit_cast<std::uint32_t>(static_cast<float>(value));
}

} // namespace

std::span<ContractError const> AllContractErrors() noexcept
{
    return std::span<ContractError const>{kAllErrors};
}

std::string_view ContractErrorName(ContractError error) noexcept
{
    for (ErrorTableRow const &row : kErrorTable)
    {
        if (row.error == error)
        {
            return row.name;
        }
    }
    return "UnknownContractError";
}

std::string_view FrameStageName(FrameStage stage) noexcept
{
    std::size_t const index = static_cast<std::size_t>(stage);
    if (index >= kFrameStageCount)
    {
        return "UnknownFrameStage";
    }
    return kStageNames[index];
}

std::optional<FrameStage> StageForError(ContractError error) noexcept
{
    for (ErrorTableRow const &row : kErrorTable)
    {
        if (row.error == error)
        {
            return row.stage;
        }
    }
    return std::nullopt;
}

StageDiagnostic MakeStageDiagnostic(FrameStage reportingStage, ContractError error) noexcept
{
    return {.reportingStage = reportingStage, .error = error, .attributedStage = StageForError(error)};
}

// ---------------------------------------------------------------------------------------------------------------
// Vectors and checked arithmetic
// ---------------------------------------------------------------------------------------------------------------

Float3 Add(Float3 left, Float3 right) noexcept
{
    return {.x = left.x + right.x, .y = left.y + right.y, .z = left.z + right.z};
}

Float3 Subtract(Float3 left, Float3 right) noexcept
{
    return {.x = left.x - right.x, .y = left.y - right.y, .z = left.z - right.z};
}

Float3 Scale(Float3 value, double factor) noexcept
{
    return {.x = value.x * factor, .y = value.y * factor, .z = value.z * factor};
}

double Dot(Float3 left, Float3 right) noexcept
{
    return (left.x * right.x) + (left.y * right.y) + (left.z * right.z);
}

double LengthSquared(Float3 value) noexcept
{
    return Dot(value, value);
}

double Length(Float3 value) noexcept
{
    return std::sqrt(LengthSquared(value));
}

bool IsFinite(Float3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool IsPowerOfTwo(std::uint64_t value) noexcept
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

std::expected<std::uint64_t, ContractError> CheckedAdd(std::uint64_t left, std::uint64_t right) noexcept
{
    if (left > std::numeric_limits<std::uint64_t>::max() - right)
    {
        return std::unexpected{ContractError::ArithmeticOverflow};
    }
    return left + right;
}

std::expected<std::uint64_t, ContractError> CheckedMultiply(std::uint64_t left, std::uint64_t right) noexcept
{
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
    {
        return std::unexpected{ContractError::ArithmeticOverflow};
    }
    return left * right;
}

std::expected<std::uint64_t, ContractError> AlignUp(std::uint64_t value, std::uint64_t alignmentBytes) noexcept
{
    if (!IsPowerOfTwo(alignmentBytes))
    {
        return std::unexpected{ContractError::AlignmentNotPowerOfTwo};
    }
    std::uint64_t const mask = alignmentBytes - 1U;
    if (value > std::numeric_limits<std::uint64_t>::max() - mask)
    {
        return std::unexpected{ContractError::ArithmeticOverflow};
    }
    return (value + mask) & ~mask;
}

bool IsAligned(std::uint64_t value, std::uint64_t alignmentBytes) noexcept
{
    if (!IsPowerOfTwo(alignmentBytes))
    {
        return false;
    }
    return (value & (alignmentBytes - 1U)) == 0U;
}

std::expected<std::uint64_t, ContractError> RoundUpToMultiple(std::uint64_t value, std::uint64_t multiple) noexcept
{
    if (multiple == 0U)
    {
        return std::unexpected{ContractError::ZeroAlignment};
    }
    std::uint64_t const remainder = value % multiple;
    if (remainder == 0U)
    {
        return value;
    }
    return CheckedAdd(value, multiple - remainder);
}

std::expected<std::uint64_t, ContractError> LeastCommonMultiple(std::uint64_t left, std::uint64_t right) noexcept
{
    if (left == 0U || right == 0U)
    {
        return std::unexpected{ContractError::ArithmeticOverflow};
    }
    std::uint64_t first = left;
    std::uint64_t second = right;
    while (second != 0U)
    {
        std::uint64_t const remainder = first % second;
        first = second;
        second = remainder;
    }
    return CheckedMultiply(left / first, right);
}

// ---------------------------------------------------------------------------------------------------------------
// Particle ABI
// ---------------------------------------------------------------------------------------------------------------

std::span<ParticleAttribute const> AllParticleAttributes() noexcept
{
    return std::span<ParticleAttribute const>{kAllAttributes};
}

std::string_view ParticleAttributeName(ParticleAttribute attribute) noexcept
{
    if (!IsKnownAttribute(attribute))
    {
        return "UnknownAttribute";
    }
    return kAttributeNames[static_cast<std::size_t>(attribute)];
}

std::expected<std::uint32_t, ContractError> AttributeElementSizeBytes(ParticleAttribute attribute) noexcept
{
    switch (attribute)
    {
    case ParticleAttribute::Position:
    case ParticleAttribute::Velocity:
        return 12U;
    case ParticleAttribute::Age:
    case ParticleAttribute::Lifetime:
    case ParticleAttribute::Id:
    case ParticleAttribute::Alive:
        return 4U;
    }
    return std::unexpected{ContractError::UnknownAttribute};
}

std::expected<std::uint64_t, ContractError> AttributeSectionAlignmentBytes(ParticleAttribute attribute) noexcept
{
    auto const elementSize = AttributeElementSizeBytes(attribute);
    if (!elementSize)
    {
        return std::unexpected{elementSize.error()};
    }
    return LeastCommonMultiple(static_cast<std::uint64_t>(*elementSize), kRawUavByteAlignmentBytes);
}

std::expected<ParticleStateLayout, ContractError> ComputeParticleStateLayout(std::uint32_t capacity) noexcept
{
    if (capacity == 0U)
    {
        return std::unexpected{ContractError::ZeroCapacity};
    }
    if (capacity > kMaximumParticleCapacity)
    {
        return std::unexpected{ContractError::CapacityExceeded};
    }
    ParticleStateLayout layout{};
    layout.capacity = capacity;

    std::uint64_t cursor = 0U;
    std::uint64_t bytesPerParticle = 0U;
    for (std::size_t index = 0U; index < kParticleAttributeCount; ++index)
    {
        ParticleAttribute const attribute = kAllAttributes[index];
        auto const elementSize = AttributeElementSizeBytes(attribute);
        if (!elementSize)
        {
            return std::unexpected{elementSize.error()};
        }
        auto const sectionAlignment = AttributeSectionAlignmentBytes(attribute);
        if (!sectionAlignment)
        {
            return std::unexpected{sectionAlignment.error()};
        }
        auto const offset = RoundUpToMultiple(cursor, *sectionAlignment);
        if (!offset)
        {
            return std::unexpected{offset.error()};
        }
        auto const sectionBytes =
            CheckedMultiply(static_cast<std::uint64_t>(*elementSize), static_cast<std::uint64_t>(capacity));
        if (!sectionBytes)
        {
            return std::unexpected{sectionBytes.error()};
        }
        auto const sectionEnd = CheckedAdd(*offset, *sectionBytes);
        if (!sectionEnd)
        {
            return std::unexpected{sectionEnd.error()};
        }

        layout.attributes[index] = {.offsetBytes = *offset,
                                    .strideBytes = static_cast<std::uint64_t>(*elementSize),
                                    .sizeBytes = *sectionBytes,
                                    .elementCount = capacity};
        cursor = *sectionEnd;

        auto const accumulated = CheckedAdd(bytesPerParticle, static_cast<std::uint64_t>(*elementSize));
        if (!accumulated)
        {
            return std::unexpected{accumulated.error()};
        }
        bytesPerParticle = *accumulated;
    }

    auto const slotSize = AlignUp(cursor, kRawUavByteAlignmentBytes);
    if (!slotSize)
    {
        return std::unexpected{slotSize.error()};
    }
    layout.slotSizeBytes = *slotSize;
    layout.bytesPerParticle = bytesPerParticle;

    auto const secondSlotOffset = AlignUp(*slotSize, kDefaultResourcePlacementAlignmentBytes);
    if (!secondSlotOffset)
    {
        return std::unexpected{secondSlotOffset.error()};
    }
    layout.secondSlotOffsetBytes = *secondSlotOffset;

    auto const pingPongSize = CheckedAdd(*secondSlotOffset, *slotSize);
    if (!pingPongSize)
    {
        return std::unexpected{pingPongSize.error()};
    }
    layout.pingPongSizeBytes = *pingPongSize;
    return layout;
}

std::expected<AttributeRange, ContractError> FindAttributeRange(ParticleStateLayout const &layout,
                                                                ParticleAttribute attribute) noexcept
{
    if (!IsKnownAttribute(attribute))
    {
        return std::unexpected{ContractError::UnknownAttribute};
    }
    return layout.attributes[static_cast<std::size_t>(attribute)];
}

std::expected<std::uint64_t, ContractError> AttributeElementOffsetBytes(ParticleStateLayout const &layout,
                                                                        ParticleAttribute attribute,
                                                                        std::uint32_t index) noexcept
{
    auto const range = FindAttributeRange(layout, attribute);
    if (!range)
    {
        return std::unexpected{range.error()};
    }
    if (index >= range->elementCount)
    {
        return std::unexpected{ContractError::ElementIndexOutOfRange};
    }
    auto const scaled = CheckedMultiply(range->strideBytes, static_cast<std::uint64_t>(index));
    if (!scaled)
    {
        return std::unexpected{scaled.error()};
    }
    return CheckedAdd(range->offsetBytes, *scaled);
}

std::expected<void, ContractError> ValidateStateBuffer(ParticleStateLayout const &layout,
                                                       std::uint64_t bufferSizeBytes) noexcept
{
    if (layout.capacity == 0U)
    {
        return std::unexpected{ContractError::ZeroCapacity};
    }
    if (bufferSizeBytes < layout.pingPongSizeBytes)
    {
        return std::unexpected{ContractError::BufferTooSmall};
    }
    for (std::size_t index = 0U; index < kParticleAttributeCount; ++index)
    {
        AttributeRange const &range = layout.attributes[index];
        if (range.strideBytes == 0U)
        {
            return std::unexpected{ContractError::StateSizeMismatch};
        }
        if (!IsAligned(range.offsetBytes, kRawUavByteAlignmentBytes))
        {
            return std::unexpected{ContractError::OffsetMisaligned};
        }
        if ((range.offsetBytes % range.strideBytes) != 0U)
        {
            return std::unexpected{ContractError::OffsetNotStrideMultiple};
        }
        auto const expectedBytes = CheckedMultiply(range.strideBytes, static_cast<std::uint64_t>(range.elementCount));
        if (!expectedBytes)
        {
            return std::unexpected{expectedBytes.error()};
        }
        if (*expectedBytes != range.sizeBytes)
        {
            return std::unexpected{ContractError::StateSizeMismatch};
        }
        auto const sectionEnd = CheckedAdd(range.offsetBytes, range.sizeBytes);
        if (!sectionEnd)
        {
            return std::unexpected{sectionEnd.error()};
        }
        if (*sectionEnd > layout.slotSizeBytes)
        {
            return std::unexpected{ContractError::BufferTooSmall};
        }
    }
    return {};
}

std::expected<FreeListLayout, ContractError> ComputeFreeListLayout(std::uint32_t capacity) noexcept
{
    if (capacity == 0U)
    {
        return std::unexpected{ContractError::ZeroCapacity};
    }
    if (capacity > kMaximumParticleCapacity)
    {
        return std::unexpected{ContractError::CapacityExceeded};
    }

    FreeListLayout layout{};
    layout.capacity = capacity;
    layout.indicesOffsetBytes = 0U;

    auto const indicesBytes = CheckedMultiply(4U, static_cast<std::uint64_t>(capacity));
    if (!indicesBytes)
    {
        return std::unexpected{indicesBytes.error()};
    }
    layout.indicesSizeBytes = *indicesBytes;

    auto const counterOffset = AlignUp(*indicesBytes, kUavCounterPlacementAlignmentBytes);
    if (!counterOffset)
    {
        return std::unexpected{counterOffset.error()};
    }
    layout.counterOffsetBytes = *counterOffset;

    auto const total = CheckedAdd(*counterOffset, 4U);
    if (!total)
    {
        return std::unexpected{total.error()};
    }
    layout.totalSizeBytes = *total;
    return layout;
}

// ---------------------------------------------------------------------------------------------------------------
// Deterministic seeded initialization
// ---------------------------------------------------------------------------------------------------------------

std::uint32_t MixSeed(std::uint64_t seed, ParticleId identity, SeedStream stream) noexcept
{
    std::uint32_t state = Finalize32(static_cast<std::uint32_t>(seed & 0xFFFF'FFFFULL) ^ 0x9E37'79B9U);
    state = Finalize32(state ^ static_cast<std::uint32_t>(seed >> 32U));
    state = Finalize32(state ^ identity);
    state = Finalize32(state ^ static_cast<std::uint32_t>(stream));
    return state;
}

double UnitFromBits(std::uint32_t bits) noexcept
{
    constexpr double kScale = 1.0 / 16'777'216.0;
    return static_cast<double>(bits >> 8U) * kScale;
}

double SignedUnitFromBits(std::uint32_t bits) noexcept
{
    return (UnitFromBits(bits) * 2.0) - 1.0;
}

// ---------------------------------------------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------------------------------------------

std::expected<void, ContractError> ValidateEmissionSettings(EmissionSettings const &settings) noexcept
{
    if (!std::isfinite(settings.ratePerSecond) || !std::isfinite(settings.lifetimeSeconds) ||
        !std::isfinite(settings.positionJitterMetres) || !std::isfinite(settings.velocityJitterMetresPerSecond) ||
        !IsFinite(settings.originMetres) || !IsFinite(settings.baseVelocityMetresPerSecond))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (settings.ratePerSecond < 0.0)
    {
        return std::unexpected{ContractError::NegativeEmissionRate};
    }
    if (settings.ratePerSecond > kMaximumEmissionRatePerSecond)
    {
        return std::unexpected{ContractError::EmissionRateTooLarge};
    }
    if (settings.lifetimeSeconds <= 0.0)
    {
        return std::unexpected{ContractError::NonPositiveLifetime};
    }
    if (settings.lifetimeSeconds > kMaximumLifetimeSeconds)
    {
        return std::unexpected{ContractError::LifetimeTooLarge};
    }
    if (settings.positionJitterMetres < 0.0 || settings.velocityJitterMetresPerSecond < 0.0)
    {
        return std::unexpected{ContractError::NegativeJitter};
    }
    if (settings.positionJitterMetres > kMaximumJitterMetres ||
        settings.velocityJitterMetresPerSecond > kMaximumJitterMetres)
    {
        return std::unexpected{ContractError::JitterTooLarge};
    }
    if (!WithinAbsolute(settings.originMetres, kMaximumPositionMetres))
    {
        return std::unexpected{ContractError::PositionOutOfDomain};
    }
    if (!WithinAbsolute(settings.baseVelocityMetresPerSecond, kMaximumSpeedMetresPerSecond))
    {
        return std::unexpected{ContractError::SpeedOutOfDomain};
    }
    if (!IsKnownDeadSlotPolicy(settings.deadSlotPolicy))
    {
        return std::unexpected{ContractError::UnknownDeadSlotPolicy};
    }
    return {};
}

std::expected<EmissionTick, ContractError> AccumulateEmission(EmissionSchedule const &schedule,
                                                              double timestepSeconds) noexcept
{
    if (!std::isfinite(schedule.ratePerSecond) || !std::isfinite(schedule.carry) || !std::isfinite(timestepSeconds))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (schedule.ratePerSecond < 0.0)
    {
        return std::unexpected{ContractError::NegativeEmissionRate};
    }
    if (schedule.ratePerSecond > kMaximumEmissionRatePerSecond)
    {
        return std::unexpected{ContractError::EmissionRateTooLarge};
    }
    if (schedule.carry < 0.0 || schedule.carry >= 1.0)
    {
        return std::unexpected{ContractError::EmissionCarryOutOfRange};
    }
    if (timestepSeconds <= 0.0)
    {
        return std::unexpected{ContractError::NonPositiveTimestep};
    }
    if (timestepSeconds > kMaximumFixedTimestepSeconds)
    {
        return std::unexpected{ContractError::FixedTimestepOutOfRange};
    }

    double const owed = schedule.carry + (schedule.ratePerSecond * timestepSeconds);
    double const whole = std::floor(owed);
    if (whole > static_cast<double>(kMaximumSpawnPerTick))
    {
        return std::unexpected{ContractError::SpawnCountExceedsLimit};
    }
    return EmissionTick{.spawnCount = static_cast<std::uint32_t>(whole), .carry = owed - whole};
}

// ---------------------------------------------------------------------------------------------------------------
// Motion, force fields, and integrators
// ---------------------------------------------------------------------------------------------------------------

std::span<Integrator const> AllIntegrators() noexcept
{
    return std::span<Integrator const>{kAllIntegrators};
}

std::string_view IntegratorName(Integrator integrator) noexcept
{
    if (!IsKnownIntegrator(integrator))
    {
        return "UnknownIntegrator";
    }
    return kIntegratorNames[static_cast<std::size_t>(integrator)];
}

std::expected<void, ContractError> ValidateForceField(ForceField const &field) noexcept
{
    if (!IsFinite(field.gravityMetresPerSecondSquared) || !std::isfinite(field.linearDragPerSecond))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (!WithinAbsolute(field.gravityMetresPerSecondSquared, kMaximumAccelerationMetresPerSecondSquared))
    {
        return std::unexpected{ContractError::AccelerationOutOfDomain};
    }
    if (field.linearDragPerSecond < 0.0)
    {
        return std::unexpected{ContractError::NegativeDragCoefficient};
    }
    // A drag coefficient is an inverse time. Past this the explicit schemes are unstable for every timestep the
    // chapter permits, so the bound is a statement about the lab's step range and not an arbitrary round number.
    if (field.linearDragPerSecond > 1.0 / kMinimumFixedTimestepSeconds)
    {
        return std::unexpected{ContractError::DragCoefficientTooLarge};
    }
    return {};
}

std::expected<void, ContractError> ValidateMotionState(MotionState const &state) noexcept
{
    if (!IsFinite(state.positionMetres) || !IsFinite(state.velocityMetresPerSecond))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (!WithinAbsolute(state.positionMetres, kMaximumPositionMetres))
    {
        return std::unexpected{ContractError::PositionOutOfDomain};
    }
    if (!WithinAbsolute(state.velocityMetresPerSecond, kMaximumSpeedMetresPerSecond))
    {
        return std::unexpected{ContractError::SpeedOutOfDomain};
    }
    return {};
}

std::expected<Float3, ContractError> EvaluateAcceleration(ForceField const &field,
                                                          Float3 velocityMetresPerSecond) noexcept
{
    auto const fieldCheck = ValidateForceField(field);
    if (!fieldCheck)
    {
        return std::unexpected{fieldCheck.error()};
    }
    if (!IsFinite(velocityMetresPerSecond))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (!WithinAbsolute(velocityMetresPerSecond, kMaximumSpeedMetresPerSecond))
    {
        return std::unexpected{ContractError::SpeedOutOfDomain};
    }
    Float3 const acceleration =
        Subtract(field.gravityMetresPerSecondSquared, Scale(velocityMetresPerSecond, field.linearDragPerSecond));
    if (!WithinAbsolute(acceleration, kMaximumAccelerationMetresPerSecondSquared))
    {
        return std::unexpected{ContractError::AccelerationOutOfDomain};
    }
    return acceleration;
}

std::expected<MotionState, ContractError> IntegrateMotion(Integrator integrator, MotionState const &state,
                                                          ForceField const &field, double timestepSeconds) noexcept
{
    if (!IsKnownIntegrator(integrator))
    {
        return std::unexpected{ContractError::UnknownIntegrator};
    }
    auto const stateCheck = ValidateMotionState(state);
    if (!stateCheck)
    {
        return std::unexpected{stateCheck.error()};
    }
    if (!std::isfinite(timestepSeconds))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (timestepSeconds <= 0.0)
    {
        return std::unexpected{ContractError::NonPositiveTimestep};
    }
    if (timestepSeconds > kMaximumFixedTimestepSeconds)
    {
        return std::unexpected{ContractError::FixedTimestepOutOfRange};
    }
    if (integrator == Integrator::VelocityVerlet && field.linearDragPerSecond != 0.0)
    {
        auto const fieldCheck = ValidateForceField(field);
        if (!fieldCheck)
        {
            return std::unexpected{fieldCheck.error()};
        }
        return std::unexpected{ContractError::VerletRequiresVelocityIndependentForce};
    }

    auto const acceleration = EvaluateAcceleration(field, state.velocityMetresPerSecond);
    if (!acceleration)
    {
        return std::unexpected{acceleration.error()};
    }

    MotionState updated{};
    switch (integrator)
    {
    case Integrator::ExplicitEuler:
        updated.positionMetres = Add(state.positionMetres, Scale(state.velocityMetresPerSecond, timestepSeconds));
        updated.velocityMetresPerSecond = Add(state.velocityMetresPerSecond, Scale(*acceleration, timestepSeconds));
        break;
    case Integrator::SemiImplicitEuler:
        updated.velocityMetresPerSecond = Add(state.velocityMetresPerSecond, Scale(*acceleration, timestepSeconds));
        updated.positionMetres = Add(state.positionMetres, Scale(updated.velocityMetresPerSecond, timestepSeconds));
        break;
    case Integrator::VelocityVerlet:
        updated.positionMetres = Add(Add(state.positionMetres, Scale(state.velocityMetresPerSecond, timestepSeconds)),
                                     Scale(*acceleration, 0.5 * timestepSeconds * timestepSeconds));
        updated.velocityMetresPerSecond = Add(state.velocityMetresPerSecond, Scale(*acceleration, timestepSeconds));
        break;
    }

    auto const updatedCheck = ValidateMotionState(updated);
    if (!updatedCheck)
    {
        return std::unexpected{updatedCheck.error()};
    }
    return updated;
}

std::expected<MotionState, ContractError> AnalyticConstantAcceleration(MotionState const &initial,
                                                                       Float3 accelerationMetres,
                                                                       double elapsedSeconds) noexcept
{
    auto const stateCheck = ValidateMotionState(initial);
    if (!stateCheck)
    {
        return std::unexpected{stateCheck.error()};
    }
    if (!IsFinite(accelerationMetres) || !std::isfinite(elapsedSeconds))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (!WithinAbsolute(accelerationMetres, kMaximumAccelerationMetresPerSecondSquared))
    {
        return std::unexpected{ContractError::AccelerationOutOfDomain};
    }
    if (elapsedSeconds < 0.0)
    {
        return std::unexpected{ContractError::NonPositiveTimestep};
    }

    MotionState result{};
    result.positionMetres = Add(Add(initial.positionMetres, Scale(initial.velocityMetresPerSecond, elapsedSeconds)),
                                Scale(accelerationMetres, 0.5 * elapsedSeconds * elapsedSeconds));
    result.velocityMetresPerSecond = Add(initial.velocityMetresPerSecond, Scale(accelerationMetres, elapsedSeconds));
    if (!IsFinite(result.positionMetres) || !IsFinite(result.velocityMetresPerSecond))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    return result;
}

std::expected<MotionState, ContractError> AnalyticLinearDrag(MotionState const &initial, ForceField const &field,
                                                             double elapsedSeconds) noexcept
{
    auto const fieldCheck = ValidateForceField(field);
    if (!fieldCheck)
    {
        return std::unexpected{fieldCheck.error()};
    }
    if (field.linearDragPerSecond == 0.0)
    {
        return AnalyticConstantAcceleration(initial, field.gravityMetresPerSecondSquared, elapsedSeconds);
    }

    auto const stateCheck = ValidateMotionState(initial);
    if (!stateCheck)
    {
        return std::unexpected{stateCheck.error()};
    }
    if (!std::isfinite(elapsedSeconds))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (elapsedSeconds < 0.0)
    {
        return std::unexpected{ContractError::NonPositiveTimestep};
    }

    double const drag = field.linearDragPerSecond;
    Float3 const terminal = Scale(field.gravityMetresPerSecondSquared, 1.0 / drag);
    Float3 const offset = Subtract(initial.velocityMetresPerSecond, terminal);
    double const decay = std::exp(-drag * elapsedSeconds);

    MotionState result{};
    result.velocityMetresPerSecond = Add(terminal, Scale(offset, decay));
    result.positionMetres =
        Add(Add(initial.positionMetres, Scale(terminal, elapsedSeconds)), Scale(offset, (1.0 - decay) / drag));
    if (!IsFinite(result.positionMetres) || !IsFinite(result.velocityMetresPerSecond))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    return result;
}

std::expected<double, ContractError> SpecificEnergy(MotionState const &state, ForceField const &field) noexcept
{
    auto const stateCheck = ValidateMotionState(state);
    if (!stateCheck)
    {
        return std::unexpected{stateCheck.error()};
    }
    auto const fieldCheck = ValidateForceField(field);
    if (!fieldCheck)
    {
        return std::unexpected{fieldCheck.error()};
    }
    if (field.linearDragPerSecond != 0.0)
    {
        return std::unexpected{ContractError::EnergyUndefinedWithDissipation};
    }
    return (0.5 * LengthSquared(state.velocityMetresPerSecond)) -
           Dot(field.gravityMetresPerSecondSquared, state.positionMetres);
}

std::expected<IntegrationDiagnostics, ContractError> IntegrateAndCompare(Integrator integrator,
                                                                         MotionState const &initial,
                                                                         ForceField const &field,
                                                                         double timestepSeconds,
                                                                         std::uint32_t stepCount) noexcept
{
    if (!IsKnownIntegrator(integrator))
    {
        return std::unexpected{ContractError::UnknownIntegrator};
    }
    if (stepCount == 0U)
    {
        return std::unexpected{ContractError::ZeroStepCount};
    }
    if (!std::isfinite(timestepSeconds))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (timestepSeconds <= 0.0)
    {
        return std::unexpected{ContractError::NonPositiveTimestep};
    }

    MotionState current = initial;
    for (std::uint32_t step = 0U; step < stepCount; ++step)
    {
        auto const advanced = IntegrateMotion(integrator, current, field, timestepSeconds);
        if (!advanced)
        {
            return std::unexpected{advanced.error()};
        }
        current = *advanced;
    }

    double const elapsedSeconds = timestepSeconds * static_cast<double>(stepCount);
    auto const reference = AnalyticLinearDrag(initial, field, elapsedSeconds);
    if (!reference)
    {
        return std::unexpected{reference.error()};
    }

    IntegrationDiagnostics diagnostics{};
    diagnostics.finalState = current;
    diagnostics.referenceState = *reference;
    diagnostics.positionErrorMetres = Length(Subtract(current.positionMetres, reference->positionMetres));
    diagnostics.velocityErrorMetresPerSecond =
        Length(Subtract(current.velocityMetresPerSecond, reference->velocityMetresPerSecond));
    diagnostics.elapsedSeconds = elapsedSeconds;
    diagnostics.stepCount = stepCount;

    if (field.linearDragPerSecond == 0.0)
    {
        auto const startEnergy = SpecificEnergy(initial, field);
        auto const endEnergy = SpecificEnergy(current, field);
        if (!startEnergy)
        {
            return std::unexpected{startEnergy.error()};
        }
        if (!endEnergy)
        {
            return std::unexpected{endEnergy.error()};
        }
        diagnostics.measuredEnergyDrift = *endEnergy - *startEnergy;

        double const perStep =
            0.5 * timestepSeconds * timestepSeconds * LengthSquared(field.gravityMetresPerSecondSquared);
        double const total = perStep * static_cast<double>(stepCount);
        switch (integrator)
        {
        case Integrator::ExplicitEuler:
            diagnostics.predictedEnergyDrift = total;
            break;
        case Integrator::SemiImplicitEuler:
            diagnostics.predictedEnergyDrift = -total;
            break;
        case Integrator::VelocityVerlet:
            diagnostics.predictedEnergyDrift = 0.0;
            break;
        }
    }
    return diagnostics;
}

std::expected<double, ContractError> EstimateConvergenceOrder(Integrator integrator, MotionState const &initial,
                                                              ForceField const &field, double baseTimestepSeconds,
                                                              std::uint32_t baseStepCount) noexcept
{
    if (baseStepCount == 0U)
    {
        return std::unexpected{ContractError::ZeroStepCount};
    }
    if (baseStepCount > std::numeric_limits<std::uint32_t>::max() / 2U)
    {
        return std::unexpected{ContractError::ArithmeticOverflow};
    }

    auto const coarse = IntegrateAndCompare(integrator, initial, field, baseTimestepSeconds, baseStepCount);
    if (!coarse)
    {
        return std::unexpected{coarse.error()};
    }
    auto const fine = IntegrateAndCompare(integrator, initial, field, baseTimestepSeconds * 0.5, baseStepCount * 2U);
    if (!fine)
    {
        return std::unexpected{fine.error()};
    }
    if (coarse->positionErrorMetres == 0.0 || fine->positionErrorMetres == 0.0)
    {
        return std::unexpected{ContractError::DegenerateConvergenceEstimate};
    }
    return std::log2(coarse->positionErrorMetres / fine->positionErrorMetres);
}

// ---------------------------------------------------------------------------------------------------------------
// Collision
// ---------------------------------------------------------------------------------------------------------------

std::expected<void, ContractError> ValidateGroundPlane(GroundPlane const &plane) noexcept
{
    if (!std::isfinite(plane.heightMetres) || !std::isfinite(plane.restitution) || !std::isfinite(plane.friction) ||
        !std::isfinite(plane.restingSpeedMetresPerSecond))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (!WithinAbsolute(plane.heightMetres, kMaximumPositionMetres))
    {
        return std::unexpected{ContractError::PlaneHeightOutOfDomain};
    }
    if (plane.restitution < 0.0 || plane.restitution > 1.0)
    {
        return std::unexpected{ContractError::RestitutionOutOfRange};
    }
    if (plane.friction < 0.0 || plane.friction > 1.0)
    {
        return std::unexpected{ContractError::FrictionOutOfRange};
    }
    if (plane.restingSpeedMetresPerSecond < 0.0)
    {
        return std::unexpected{ContractError::NegativeRestingSpeed};
    }
    if (plane.restingSpeedMetresPerSecond > kMaximumSpeedMetresPerSecond)
    {
        return std::unexpected{ContractError::SpeedOutOfDomain};
    }
    return {};
}

std::expected<CollisionOutcome, ContractError> ResolveGroundCollision(GroundPlane const &plane,
                                                                      MotionState const &state) noexcept
{
    auto const planeCheck = ValidateGroundPlane(plane);
    if (!planeCheck)
    {
        return std::unexpected{planeCheck.error()};
    }
    auto const stateCheck = ValidateMotionState(state);
    if (!stateCheck)
    {
        return std::unexpected{stateCheck.error()};
    }

    CollisionOutcome outcome{};
    outcome.state = state;
    outcome.normalSpeedBeforeMetresPerSecond = state.velocityMetresPerSecond.y;
    outcome.normalSpeedAfterMetresPerSecond = state.velocityMetresPerSecond.y;
    if (!plane.enabled)
    {
        return outcome;
    }

    double const penetration = plane.heightMetres - state.positionMetres.y;
    if (penetration <= 0.0)
    {
        return outcome;
    }

    outcome.contacted = true;
    outcome.penetrationDepthMetres = penetration;
    outcome.state.positionMetres.y = plane.heightMetres;

    double normalSpeed = state.velocityMetresPerSecond.y;
    if (normalSpeed < 0.0)
    {
        normalSpeed = -plane.restitution * normalSpeed;
        // The tangential scale is applied only on an *approaching* contact. A particle already moving away from
        // the plane is being lifted out of a penetration it did not create this step, and scaling its tangential
        // velocity there would brake it twice for one impact.
        double const tangentialScale = 1.0 - plane.friction;
        outcome.state.velocityMetresPerSecond.x = state.velocityMetresPerSecond.x * tangentialScale;
        outcome.state.velocityMetresPerSecond.z = state.velocityMetresPerSecond.z * tangentialScale;
    }
    if (normalSpeed <= plane.restingSpeedMetresPerSecond)
    {
        normalSpeed = 0.0;
        outcome.cameToRest = true;
    }
    outcome.state.velocityMetresPerSecond.y = normalSpeed;
    outcome.normalSpeedAfterMetresPerSecond = normalSpeed;

    auto const resolvedCheck = ValidateMotionState(outcome.state);
    if (!resolvedCheck)
    {
        return std::unexpected{resolvedCheck.error()};
    }
    return outcome;
}

// ---------------------------------------------------------------------------------------------------------------
// Fixed-step scheduling
// ---------------------------------------------------------------------------------------------------------------

std::expected<void, ContractError> ValidateFixedStepSettings(FixedStepSettings const &settings) noexcept
{
    if (!std::isfinite(settings.fixedTimestepSeconds) || !std::isfinite(settings.maximumFrameDeltaSeconds))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (settings.fixedTimestepSeconds < kMinimumFixedTimestepSeconds ||
        settings.fixedTimestepSeconds > kMaximumFixedTimestepSeconds)
    {
        return std::unexpected{ContractError::FixedTimestepOutOfRange};
    }
    if (settings.maximumSubstepCount == 0U || settings.maximumSubstepCount > kMaximumSubstepCount)
    {
        return std::unexpected{ContractError::SubstepLimitOutOfRange};
    }
    if (settings.maximumFrameDeltaSeconds <= 0.0 || settings.maximumFrameDeltaSeconds > kMaximumFrameDeltaSeconds)
    {
        return std::unexpected{ContractError::FrameDeltaClampOutOfRange};
    }
    if (!IsKnownOverflowPolicy(settings.overflowPolicy))
    {
        return std::unexpected{ContractError::UnknownOverflowPolicy};
    }
    return {};
}

std::expected<FixedStepPlan, ContractError> PlanFixedSteps(FixedStepSettings const &settings, double accumulatorSeconds,
                                                           double frameDeltaSeconds) noexcept
{
    auto const settingsCheck = ValidateFixedStepSettings(settings);
    if (!settingsCheck)
    {
        return std::unexpected{settingsCheck.error()};
    }
    if (!std::isfinite(accumulatorSeconds) || !std::isfinite(frameDeltaSeconds))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (frameDeltaSeconds < 0.0)
    {
        return std::unexpected{ContractError::NegativeFrameDelta};
    }
    if (frameDeltaSeconds > kMaximumFrameDeltaSeconds)
    {
        return std::unexpected{ContractError::FrameDeltaTooLarge};
    }
    if (accumulatorSeconds < 0.0)
    {
        return std::unexpected{ContractError::NegativeAccumulator};
    }
    if (accumulatorSeconds > kMaximumAccumulatorSeconds)
    {
        return std::unexpected{ContractError::SimulationTimeBacklogExceeded};
    }

    FixedStepPlan plan{};
    double const clamped = std::min(frameDeltaSeconds, settings.maximumFrameDeltaSeconds);
    plan.clampedFrameDeltaSeconds = clamped;
    plan.frameDeltaWasClamped = clamped < frameDeltaSeconds;
    plan.droppedByClampSeconds = frameDeltaSeconds - clamped;

    // Counting by repeated subtraction rather than by a floor division: the loop is bounded by the substep limit,
    // and it cannot disagree with the subtraction that produces the remainder the way a separately rounded
    // division can.
    double remaining = accumulatorSeconds + clamped;
    std::uint32_t substeps = 0U;
    while (substeps < settings.maximumSubstepCount && remaining >= settings.fixedTimestepSeconds)
    {
        remaining -= settings.fixedTimestepSeconds;
        ++substeps;
    }
    plan.substepCount = substeps;
    plan.consumedSeconds = settings.fixedTimestepSeconds * static_cast<double>(substeps);
    plan.substepLimitReached = remaining >= settings.fixedTimestepSeconds;

    if (plan.substepLimitReached && settings.overflowPolicy == SubstepOverflowPolicy::DropExcessTime)
    {
        // `std::fmod` is exact for finite operands, so the carry it leaves is the same number on every
        // conforming implementation and the dropped total is exactly the time the frame refused to simulate.
        double const carry = std::fmod(remaining, settings.fixedTimestepSeconds);
        plan.droppedBySubstepLimitSeconds = remaining - carry;
        plan.carrySeconds = carry;
    }
    else
    {
        plan.carrySeconds = remaining;
    }

    if (plan.carrySeconds > kMaximumAccumulatorSeconds)
    {
        return std::unexpected{ContractError::SimulationTimeBacklogExceeded};
    }
    return plan;
}

// ---------------------------------------------------------------------------------------------------------------
// The particle system
// ---------------------------------------------------------------------------------------------------------------

BufferSlot OtherSlot(BufferSlot slot) noexcept
{
    return slot == BufferSlot::A ? BufferSlot::B : BufferSlot::A;
}

std::expected<void, ContractError> ValidateFrameSlots(FrameSlots const &slots) noexcept
{
    if (slots.readSlot != BufferSlot::A && slots.readSlot != BufferSlot::B)
    {
        return std::unexpected{ContractError::PingPongOwnershipMismatch};
    }
    if (slots.writeSlot != BufferSlot::A && slots.writeSlot != BufferSlot::B)
    {
        return std::unexpected{ContractError::PingPongOwnershipMismatch};
    }
    if (slots.readSlot == slots.writeSlot)
    {
        return std::unexpected{ContractError::PingPongAliasViolation};
    }
    return {};
}

FrameSlots AdvanceFrameSlots(FrameSlots const &slots) noexcept
{
    return {.readSlot = slots.writeSlot, .writeSlot = slots.readSlot};
}

std::expected<ParticleSystemState, ContractError> ResetParticleSystem(std::uint32_t capacity, std::uint64_t seed)
{
    // The reset refuses a capacity whose GPU layout cannot be described, because a CPU system that cannot be
    // mirrored into a buffer is not a model of anything.
    auto const layout = ComputeParticleStateLayout(capacity);
    if (!layout)
    {
        return std::unexpected{layout.error()};
    }

    ParticleSystemState state{};
    state.capacity = capacity;
    state.slots.assign(static_cast<std::size_t>(capacity), Particle{});
    state.nextIdentity = 0U;
    state.emissionCarry = 0.0;
    state.accumulatorSeconds = 0.0;
    state.counters = SystemCounters{};
    state.seed = seed;
    state.frameIndex = 0U;
    state.slotOwnership = FrameSlots{.readSlot = BufferSlot::A, .writeSlot = BufferSlot::B};
    return state;
}

std::expected<void, ContractError> ValidateParticleSystem(ParticleSystemState const &state) noexcept
{
    if (state.capacity == 0U)
    {
        return std::unexpected{ContractError::ZeroCapacity};
    }
    if (state.capacity > kMaximumParticleCapacity)
    {
        return std::unexpected{ContractError::CapacityExceeded};
    }
    if (state.slots.size() != static_cast<std::size_t>(state.capacity))
    {
        return std::unexpected{ContractError::StateSizeMismatch};
    }
    if (!std::isfinite(state.emissionCarry) || !std::isfinite(state.accumulatorSeconds))
    {
        return std::unexpected{ContractError::NonFinite};
    }
    if (state.emissionCarry < 0.0 || state.emissionCarry >= 1.0)
    {
        return std::unexpected{ContractError::EmissionCarryOutOfRange};
    }
    if (state.accumulatorSeconds < 0.0)
    {
        return std::unexpected{ContractError::NegativeAccumulator};
    }
    if (state.accumulatorSeconds > kMaximumAccumulatorSeconds)
    {
        return std::unexpected{ContractError::SimulationTimeBacklogExceeded};
    }
    auto const slotCheck = ValidateFrameSlots(state.slotOwnership);
    if (!slotCheck)
    {
        return std::unexpected{slotCheck.error()};
    }

    std::vector<ParticleId> liveIdentities{};
    liveIdentities.reserve(state.slots.size());
    for (Particle const &particle : state.slots)
    {
        if (particle.alive != (particle.identity != kInvalidParticleId))
        {
            return std::unexpected{particle.alive ? ContractError::LiveSlotHasInvalidId
                                                  : ContractError::DeadSlotHasValidId};
        }
        if (!particle.alive)
        {
            continue;
        }
        auto const motionCheck = ValidateMotionState(particle.motion);
        if (!motionCheck)
        {
            return std::unexpected{motionCheck.error()};
        }
        if (!std::isfinite(particle.ageSeconds) || !std::isfinite(particle.lifetimeSeconds))
        {
            return std::unexpected{ContractError::NonFinite};
        }
        if (particle.ageSeconds < 0.0)
        {
            return std::unexpected{ContractError::NegativeAge};
        }
        if (particle.lifetimeSeconds <= 0.0)
        {
            return std::unexpected{ContractError::NonPositiveLifetime};
        }
        if (particle.lifetimeSeconds > kMaximumLifetimeSeconds)
        {
            return std::unexpected{ContractError::LifetimeTooLarge};
        }
        // The death boundary is closed on the lifetime: a live particle's age is strictly below it, so a slot
        // holding age == lifetime is a slot the simulation forgot to retire.
        if (particle.ageSeconds >= particle.lifetimeSeconds)
        {
            return std::unexpected{ContractError::AgeExceedsLifetime};
        }
        if (state.nextIdentity != kInvalidParticleId && particle.identity >= state.nextIdentity)
        {
            return std::unexpected{ContractError::IdentityNotMonotonic};
        }
        liveIdentities.push_back(particle.identity);
    }

    std::sort(liveIdentities.begin(), liveIdentities.end());
    if (std::adjacent_find(liveIdentities.begin(), liveIdentities.end()) != liveIdentities.end())
    {
        return std::unexpected{ContractError::DuplicateParticleId};
    }
    return {};
}

std::uint32_t CountLiveParticles(ParticleSystemState const &state) noexcept
{
    std::uint32_t count = 0U;
    for (Particle const &particle : state.slots)
    {
        if (particle.alive)
        {
            ++count;
        }
    }
    return count;
}

std::expected<AccountingReport, ContractError> ValidateAccounting(ParticleSystemState const &state) noexcept
{
    auto const stateCheck = ValidateParticleSystem(state);
    if (!stateCheck)
    {
        return std::unexpected{stateCheck.error()};
    }

    AccountingReport report{};
    report.requestedCount = state.counters.requestedCount;
    report.spawnedCount = state.counters.spawnedCount;
    report.droppedByCapacityCount = state.counters.droppedByCapacityCount;
    report.diedCount = state.counters.diedCount;
    report.liveCount = static_cast<std::uint64_t>(CountLiveParticles(state));

    auto const requested = CheckedAdd(report.spawnedCount, report.droppedByCapacityCount);
    if (!requested)
    {
        return std::unexpected{requested.error()};
    }
    if (*requested != report.requestedCount)
    {
        return std::unexpected{ContractError::AccountingMismatch};
    }
    auto const spawned = CheckedAdd(report.liveCount, report.diedCount);
    if (!spawned)
    {
        return std::unexpected{spawned.error()};
    }
    if (*spawned != report.spawnedCount)
    {
        return std::unexpected{ContractError::AccountingMismatch};
    }
    return report;
}

std::expected<std::vector<std::uint32_t>, ContractError> BuildFreeSlotList(ParticleSystemState const &state,
                                                                           DeadSlotPolicy policy)
{
    auto const stateCheck = ValidateParticleSystem(state);
    if (!stateCheck)
    {
        return std::unexpected{stateCheck.error()};
    }
    if (!IsKnownDeadSlotPolicy(policy))
    {
        return std::unexpected{ContractError::UnknownDeadSlotPolicy};
    }

    std::vector<std::uint32_t> free{};
    free.reserve(state.slots.size());
    for (std::size_t index = 0U; index < state.slots.size(); ++index)
    {
        if (!state.slots[index].alive)
        {
            free.push_back(static_cast<std::uint32_t>(index));
        }
    }
    if (policy == DeadSlotPolicy::DescendingIndex)
    {
        std::reverse(free.begin(), free.end());
    }
    return free;
}

std::expected<EmissionResult, ContractError> EmitParticles(ParticleSystemState &state, EmissionRequest const &request)
{
    auto const stateCheck = ValidateParticleSystem(state);
    if (!stateCheck)
    {
        return std::unexpected{stateCheck.error()};
    }
    auto const settingsCheck = ValidateEmissionSettings(request.settings);
    if (!settingsCheck)
    {
        return std::unexpected{settingsCheck.error()};
    }
    if (request.requestedCount > kMaximumSpawnPerTick)
    {
        return std::unexpected{ContractError::SpawnCountExceedsLimit};
    }

    std::uint64_t const remainingIdentities =
        state.nextIdentity == kInvalidParticleId
            ? 0ULL
            : (static_cast<std::uint64_t>(kMaximumParticleId) - static_cast<std::uint64_t>(state.nextIdentity)) + 1ULL;
    if (static_cast<std::uint64_t>(request.requestedCount) > remainingIdentities)
    {
        return std::unexpected{ContractError::ParticleIdSpaceExhausted};
    }

    auto const freeSlots = BuildFreeSlotList(state, request.settings.deadSlotPolicy);
    if (!freeSlots)
    {
        return std::unexpected{freeSlots.error()};
    }

    std::uint32_t const available = static_cast<std::uint32_t>(freeSlots->size());
    std::uint32_t const spawnCount = std::min(request.requestedCount, available);
    std::uint32_t const droppedCount = request.requestedCount - spawnCount;

    // Everything is built before anything is committed, so a seeded particle that lands outside the domain leaves
    // the system exactly as it was instead of half-emitted.
    std::vector<Particle> staged{};
    staged.reserve(static_cast<std::size_t>(spawnCount));
    for (std::uint32_t index = 0U; index < spawnCount; ++index)
    {
        ParticleId const identity = state.nextIdentity + index;
        Float3 const positionOffset{.x = request.settings.positionJitterMetres *
                                         SignedUnitFromBits(MixSeed(state.seed, identity, SeedStream::PositionX)),
                                    .y = request.settings.positionJitterMetres *
                                         SignedUnitFromBits(MixSeed(state.seed, identity, SeedStream::PositionY)),
                                    .z = request.settings.positionJitterMetres *
                                         SignedUnitFromBits(MixSeed(state.seed, identity, SeedStream::PositionZ))};
        Float3 const velocityOffset{.x = request.settings.velocityJitterMetresPerSecond *
                                         SignedUnitFromBits(MixSeed(state.seed, identity, SeedStream::VelocityX)),
                                    .y = request.settings.velocityJitterMetresPerSecond *
                                         SignedUnitFromBits(MixSeed(state.seed, identity, SeedStream::VelocityY)),
                                    .z = request.settings.velocityJitterMetresPerSecond *
                                         SignedUnitFromBits(MixSeed(state.seed, identity, SeedStream::VelocityZ))};

        Particle particle{};
        particle.identity = identity;
        particle.motion.positionMetres = Add(request.settings.originMetres, positionOffset);
        particle.motion.velocityMetresPerSecond = Add(request.settings.baseVelocityMetresPerSecond, velocityOffset);
        particle.ageSeconds = 0.0;
        particle.lifetimeSeconds = request.settings.lifetimeSeconds;
        particle.alive = true;

        auto const motionCheck = ValidateMotionState(particle.motion);
        if (!motionCheck)
        {
            return std::unexpected{motionCheck.error()};
        }
        staged.push_back(particle);
    }

    auto const requestedTotal =
        CheckedAdd(state.counters.requestedCount, static_cast<std::uint64_t>(request.requestedCount));
    if (!requestedTotal)
    {
        return std::unexpected{ContractError::CounterOverflow};
    }
    auto const spawnedTotal = CheckedAdd(state.counters.spawnedCount, static_cast<std::uint64_t>(spawnCount));
    if (!spawnedTotal)
    {
        return std::unexpected{ContractError::CounterOverflow};
    }
    auto const droppedTotal =
        CheckedAdd(state.counters.droppedByCapacityCount, static_cast<std::uint64_t>(droppedCount));
    if (!droppedTotal)
    {
        return std::unexpected{ContractError::CounterOverflow};
    }

    EmissionResult result{};
    result.spawnedCount = spawnCount;
    result.droppedByCapacityCount = droppedCount;
    result.slots.reserve(static_cast<std::size_t>(spawnCount));
    for (std::uint32_t index = 0U; index < spawnCount; ++index)
    {
        std::uint32_t const slot = (*freeSlots)[index];
        state.slots[static_cast<std::size_t>(slot)] = staged[static_cast<std::size_t>(index)];
        result.slots.push_back(slot);
    }
    if (spawnCount != 0U)
    {
        result.firstIdentity = state.nextIdentity;
        result.lastIdentity = state.nextIdentity + (spawnCount - 1U);
        state.nextIdentity = state.nextIdentity + spawnCount;
    }
    state.counters.requestedCount = *requestedTotal;
    state.counters.spawnedCount = *spawnedTotal;
    state.counters.droppedByCapacityCount = *droppedTotal;
    return result;
}

std::expected<std::uint64_t, ContractError> ChecksumParticleState(ParticleSystemState const &state) noexcept
{
    auto const stateCheck = ValidateParticleSystem(state);
    if (!stateCheck)
    {
        return std::unexpected{stateCheck.error()};
    }

    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (Particle const &particle : state.slots)
    {
        hash = HashBytes(hash, particle.identity);
        hash = HashBytes(hash, particle.alive ? 1U : 0U);
        if (!particle.alive)
        {
            // A dead slot's payload is stale on the GPU and cleared here, so hashing it would compare a value the
            // hardware never promised. Only the sentinel and the flag are evidence.
            continue;
        }
        hash = HashBytes(hash, FloatBits(particle.motion.positionMetres.x));
        hash = HashBytes(hash, FloatBits(particle.motion.positionMetres.y));
        hash = HashBytes(hash, FloatBits(particle.motion.positionMetres.z));
        hash = HashBytes(hash, FloatBits(particle.motion.velocityMetresPerSecond.x));
        hash = HashBytes(hash, FloatBits(particle.motion.velocityMetresPerSecond.y));
        hash = HashBytes(hash, FloatBits(particle.motion.velocityMetresPerSecond.z));
        hash = HashBytes(hash, FloatBits(particle.ageSeconds));
        hash = HashBytes(hash, FloatBits(particle.lifetimeSeconds));
    }
    return hash;
}

std::expected<FrameReport, ContractError> SimulateFrame(ParticleSystemState &state, FrameSettings const &settings,
                                                        double frameDeltaSeconds)
{
    auto const stateCheck = ValidateParticleSystem(state);
    if (!stateCheck)
    {
        return std::unexpected{stateCheck.error()};
    }
    auto const emissionCheck = ValidateEmissionSettings(settings.emission);
    if (!emissionCheck)
    {
        return std::unexpected{emissionCheck.error()};
    }
    auto const fieldCheck = ValidateForceField(settings.field);
    if (!fieldCheck)
    {
        return std::unexpected{fieldCheck.error()};
    }
    auto const planeCheck = ValidateGroundPlane(settings.ground);
    if (!planeCheck)
    {
        return std::unexpected{planeCheck.error()};
    }
    if (!IsKnownIntegrator(settings.integrator))
    {
        return std::unexpected{ContractError::UnknownIntegrator};
    }
    auto const plan = PlanFixedSteps(settings.step, state.accumulatorSeconds, frameDeltaSeconds);
    if (!plan)
    {
        return std::unexpected{plan.error()};
    }

    // The working copy is what buys the "state is untouched on failure" guarantee. It is the right trade for a
    // reference model and the wrong one for a simulation that owns a million particles.
    ParticleSystemState working = state;
    double const timestep = settings.step.fixedTimestepSeconds;

    FrameReport report{};
    report.plan = *plan;
    report.substeps.reserve(static_cast<std::size_t>(plan->substepCount));
    FrameSlots slots = state.slotOwnership;

    for (std::uint32_t substep = 0U; substep < plan->substepCount; ++substep)
    {
        SubstepReport substepReport{};

        EmissionSchedule const schedule{.ratePerSecond = settings.emission.ratePerSecond,
                                        .carry = working.emissionCarry};
        auto const tick = AccumulateEmission(schedule, timestep);
        if (!tick)
        {
            return std::unexpected{tick.error()};
        }
        working.emissionCarry = tick->carry;
        auto const emission =
            EmitParticles(working, EmissionRequest{.requestedCount = tick->spawnCount, .settings = settings.emission});
        if (!emission)
        {
            return std::unexpected{emission.error()};
        }
        substepReport.spawnedCount = emission->spawnedCount;
        substepReport.droppedByCapacityCount = emission->droppedByCapacityCount;

        for (Particle &particle : working.slots)
        {
            if (!particle.alive)
            {
                continue;
            }
            auto const integrated = IntegrateMotion(settings.integrator, particle.motion, settings.field, timestep);
            if (!integrated)
            {
                return std::unexpected{integrated.error()};
            }
            auto const collision = ResolveGroundCollision(settings.ground, *integrated);
            if (!collision)
            {
                return std::unexpected{collision.error()};
            }
            particle.motion = collision->state;
            if (collision->contacted)
            {
                ++substepReport.contactCount;
            }
            particle.ageSeconds += timestep;
            if (particle.ageSeconds >= particle.lifetimeSeconds)
            {
                particle = Particle{};
                ++substepReport.diedCount;
            }
        }

        auto const contactTotal =
            CheckedAdd(working.counters.contactCount, static_cast<std::uint64_t>(substepReport.contactCount));
        if (!contactTotal)
        {
            return std::unexpected{ContractError::CounterOverflow};
        }
        auto const diedTotal =
            CheckedAdd(working.counters.diedCount, static_cast<std::uint64_t>(substepReport.diedCount));
        if (!diedTotal)
        {
            return std::unexpected{ContractError::CounterOverflow};
        }
        working.counters.contactCount = *contactTotal;
        working.counters.diedCount = *diedTotal;

        substepReport.liveCountAfter = CountLiveParticles(working);
        report.spawnedCount += substepReport.spawnedCount;
        report.droppedByCapacityCount += substepReport.droppedByCapacityCount;
        report.diedCount += substepReport.diedCount;
        report.contactCount += substepReport.contactCount;
        report.substeps.push_back(substepReport);

        slots = AdvanceFrameSlots(slots);
    }

    working.accumulatorSeconds = plan->carrySeconds;
    working.frameIndex = state.frameIndex + 1U;
    working.slotOwnership = slots;

    auto const checksum = ChecksumParticleState(working);
    if (!checksum)
    {
        return std::unexpected{checksum.error()};
    }
    report.checksum = *checksum;
    report.liveCount = CountLiveParticles(working);
    report.slotsUsed = plan->substepCount == 0U ? state.slotOwnership : AdvanceFrameSlots(slots);
    report.resultSlot = plan->substepCount == 0U ? state.slotOwnership.readSlot : report.slotsUsed.writeSlot;

    state = std::move(working);
    return report;
}

// ---------------------------------------------------------------------------------------------------------------
// Compaction and dispatch
// ---------------------------------------------------------------------------------------------------------------

std::expected<CompactionResult, ContractError> CompactAliveSlots(CompactionInput const &input)
{
    if (input.aliveFlags.size() != input.identities.size())
    {
        return std::unexpected{ContractError::ParallelArrayLengthMismatch};
    }
    if (input.aliveFlags.size() > static_cast<std::size_t>(kMaximumParticleCapacity))
    {
        return std::unexpected{ContractError::CapacityExceeded};
    }
    if (input.outputCapacity > kMaximumParticleCapacity)
    {
        return std::unexpected{ContractError::CapacityExceeded};
    }

    CompactionResult result{};
    result.exclusiveOffsets.reserve(input.aliveFlags.size());
    result.compactedSlots.reserve(std::min(static_cast<std::size_t>(input.outputCapacity), input.aliveFlags.size()));
    result.compactedIdentities.reserve(result.compactedSlots.capacity());

    std::uint32_t running = 0U;
    for (std::size_t index = 0U; index < input.aliveFlags.size(); ++index)
    {
        std::uint32_t const flag = input.aliveFlags[index];
        if (flag > 1U)
        {
            return std::unexpected{ContractError::NonBinaryAliveFlag};
        }
        ParticleId const identity = input.identities[index];
        if (flag == 1U && identity == kInvalidParticleId)
        {
            return std::unexpected{ContractError::LiveSlotHasInvalidId};
        }
        if (flag == 0U && identity != kInvalidParticleId)
        {
            return std::unexpected{ContractError::DeadSlotHasValidId};
        }

        result.exclusiveOffsets.push_back(running);
        if (flag == 1U)
        {
            if (running < input.outputCapacity)
            {
                result.compactedSlots.push_back(static_cast<std::uint32_t>(index));
                result.compactedIdentities.push_back(identity);
            }
            ++running;
        }
    }

    result.aliveCount = running;
    result.emittedCount = std::min(running, input.outputCapacity);
    result.droppedCount = running - result.emittedCount;
    result.overflowed = result.droppedCount != 0U;
    if (result.compactedSlots.size() != static_cast<std::size_t>(result.emittedCount))
    {
        return std::unexpected{ContractError::AliveCountMismatch};
    }
    return result;
}

std::expected<CompactionResult, ContractError> CompactParticleSystem(ParticleSystemState const &state,
                                                                     std::uint32_t outputCapacity)
{
    auto const stateCheck = ValidateParticleSystem(state);
    if (!stateCheck)
    {
        return std::unexpected{stateCheck.error()};
    }

    std::vector<std::uint32_t> flags{};
    std::vector<ParticleId> identities{};
    flags.reserve(state.slots.size());
    identities.reserve(state.slots.size());
    for (Particle const &particle : state.slots)
    {
        flags.push_back(particle.alive ? 1U : 0U);
        identities.push_back(particle.identity);
    }
    return CompactAliveSlots(
        CompactionInput{.aliveFlags = flags, .identities = identities, .outputCapacity = outputCapacity});
}

std::expected<DispatchDimensions, ContractError> ComputeDispatchDimensions(std::uint32_t workItemCount,
                                                                           std::uint32_t threadsPerGroup) noexcept
{
    if (threadsPerGroup == 0U)
    {
        return std::unexpected{ContractError::ZeroThreadGroupSize};
    }
    if (threadsPerGroup > kMaximumThreadsPerGroup)
    {
        return std::unexpected{ContractError::ThreadGroupSizeExceeded};
    }

    std::uint64_t const groups =
        (static_cast<std::uint64_t>(workItemCount) + static_cast<std::uint64_t>(threadsPerGroup) - 1ULL) /
        static_cast<std::uint64_t>(threadsPerGroup);
    if (groups > static_cast<std::uint64_t>(kMaximumThreadGroupsPerDimension))
    {
        return std::unexpected{ContractError::ThreadGroupCountExceeded};
    }

    auto const dispatched = CheckedMultiply(groups, static_cast<std::uint64_t>(threadsPerGroup));
    if (!dispatched)
    {
        return std::unexpected{dispatched.error()};
    }

    DispatchDimensions dimensions{};
    dimensions.threadGroupCountX = static_cast<std::uint32_t>(groups);
    dimensions.threadsPerGroup = threadsPerGroup;
    dimensions.dispatchedThreadCount = static_cast<std::uint32_t>(*dispatched);
    dimensions.tailThreadCount = dimensions.dispatchedThreadCount - workItemCount;
    dimensions.requiresBoundsGuard = dimensions.tailThreadCount != 0U;
    return dimensions;
}

// ---------------------------------------------------------------------------------------------------------------
// Indirect draw arguments
// ---------------------------------------------------------------------------------------------------------------

std::expected<IndirectDrawArgumentsResult, ContractError> BuildIndirectDrawArguments(
    IndirectDrawRequest const &request) noexcept
{
    if (request.vertexCountPerParticle == 0U)
    {
        return std::unexpected{ContractError::ZeroVertexCountPerParticle};
    }
    if (request.vertexCountPerParticle > kMaximumVertexCountPerParticle)
    {
        return std::unexpected{ContractError::VertexCountPerParticleTooLarge};
    }
    if (request.liveCount > kMaximumParticleCapacity || request.drawCapacity > kMaximumParticleCapacity)
    {
        return std::unexpected{ContractError::CapacityExceeded};
    }

    std::uint32_t const instanceCount = std::min(request.liveCount, request.drawCapacity);
    auto const instanceEnd = CheckedAdd(static_cast<std::uint64_t>(request.startInstanceLocation),
                                        static_cast<std::uint64_t>(instanceCount));
    if (!instanceEnd)
    {
        return std::unexpected{instanceEnd.error()};
    }
    if (*instanceEnd > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        return std::unexpected{ContractError::ArithmeticOverflow};
    }
    auto const vertexEnd = CheckedAdd(static_cast<std::uint64_t>(request.startVertexLocation),
                                      static_cast<std::uint64_t>(request.vertexCountPerParticle));
    if (!vertexEnd)
    {
        return std::unexpected{vertexEnd.error()};
    }
    if (*vertexEnd > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        return std::unexpected{ContractError::ArithmeticOverflow};
    }

    IndirectDrawArgumentsResult result{};
    result.arguments = {.vertexCountPerInstance = request.vertexCountPerParticle,
                        .instanceCount = instanceCount,
                        .startVertexLocation = request.startVertexLocation,
                        .startInstanceLocation = request.startInstanceLocation};
    result.clampedInstanceCount = request.liveCount - instanceCount;
    result.clamped = result.clampedInstanceCount != 0U;
    return result;
}

std::expected<std::uint64_t, ContractError> ValidateIndirectArgumentBufferLayout(
    IndirectArgumentBufferLayout const &layout) noexcept
{
    if (layout.strideBytes != kDrawArgumentsSizeBytes)
    {
        return std::unexpected{ContractError::IndirectStrideMismatch};
    }
    if (layout.maximumCommandCount == 0U)
    {
        return std::unexpected{ContractError::ZeroCommandCount};
    }
    if (layout.maximumCommandCount > kMaximumIndirectCommandCount)
    {
        return std::unexpected{ContractError::CommandCountExceeded};
    }
    if (!IsAligned(layout.argumentOffsetBytes, kIndirectArgumentOffsetAlignmentBytes))
    {
        return std::unexpected{ContractError::OffsetMisaligned};
    }

    auto const argumentBytes =
        CheckedMultiply(layout.strideBytes, static_cast<std::uint64_t>(layout.maximumCommandCount));
    if (!argumentBytes)
    {
        return std::unexpected{argumentBytes.error()};
    }
    auto const argumentEnd = CheckedAdd(layout.argumentOffsetBytes, *argumentBytes);
    if (!argumentEnd)
    {
        return std::unexpected{argumentEnd.error()};
    }
    if (*argumentEnd > layout.bufferSizeBytes)
    {
        return std::unexpected{ContractError::BufferTooSmall};
    }

    std::uint64_t lastByte = *argumentEnd;
    if (layout.countOffsetBytes)
    {
        if (!IsAligned(*layout.countOffsetBytes, kIndirectArgumentOffsetAlignmentBytes))
        {
            return std::unexpected{ContractError::OffsetMisaligned};
        }
        auto const countEnd = CheckedAdd(*layout.countOffsetBytes, 4U);
        if (!countEnd)
        {
            return std::unexpected{countEnd.error()};
        }
        if (*countEnd > layout.bufferSizeBytes)
        {
            return std::unexpected{ContractError::BufferTooSmall};
        }
        lastByte = std::max(lastByte, *countEnd);
    }
    return lastByte;
}

std::expected<std::uint32_t, ContractError> ResolveExecutedCommandCount(
    std::uint32_t maximumCommandCount, std::optional<std::uint32_t> countBufferValue) noexcept
{
    if (maximumCommandCount == 0U)
    {
        return std::unexpected{ContractError::ZeroCommandCount};
    }
    if (maximumCommandCount > kMaximumIndirectCommandCount)
    {
        return std::unexpected{ContractError::CommandCountExceeded};
    }
    if (!countBufferValue)
    {
        return maximumCommandCount;
    }
    return std::min(maximumCommandCount, *countBufferValue);
}

std::expected<void, ContractError> ValidateFramePlan(FramePlan const &plan) noexcept
{
    auto const slotCheck = ValidateFrameSlots(plan.slots);
    if (!slotCheck)
    {
        return std::unexpected{slotCheck.error()};
    }
    if (plan.liveCountSource == LiveCountSource::CpuReadback && !plan.diagnosticFrame)
    {
        return std::unexpected{ContractError::ReadbackDependencyInSteadyState};
    }
    if (plan.readbackRequested && !plan.diagnosticFrame)
    {
        return std::unexpected{ContractError::ReadbackDependencyInSteadyState};
    }
    return {};
}

// ---------------------------------------------------------------------------------------------------------------
// Stage evidence, ping-pong ownership, and barriers
// ---------------------------------------------------------------------------------------------------------------

std::uint32_t StageBit(FrameStage stage) noexcept
{
    std::size_t const index = static_cast<std::size_t>(stage);
    if (index >= kFrameStageCount)
    {
        return 0U;
    }
    return 1U << index;
}

std::expected<void, ContractError> ValidateStagePartition(StagePartition const &partition,
                                                          bool requireComplete) noexcept
{
    std::uint32_t const union0 = partition.submittedMask & partition.skippedMask;
    std::uint32_t const union1 = partition.submittedMask & partition.unavailableMask;
    std::uint32_t const union2 = partition.skippedMask & partition.unavailableMask;
    if (union0 != 0U || union1 != 0U || union2 != 0U)
    {
        return std::unexpected{ContractError::StagePartitionOverlap};
    }
    std::uint32_t const combined = partition.submittedMask | partition.skippedMask | partition.unavailableMask;
    if (combined != partition.recordedMask)
    {
        return std::unexpected{ContractError::StagePartitionOverlap};
    }
    if ((partition.recordedMask & ~kAllFrameStagesMask) != 0U)
    {
        return std::unexpected{ContractError::StagePartitionOverlap};
    }
    if (requireComplete && partition.recordedMask != kAllFrameStagesMask)
    {
        return std::unexpected{ContractError::StagePartitionIncomplete};
    }
    return {};
}

FrameStageLedger::FrameStageLedger() noexcept
{
    for (std::size_t index = 0U; index < kFrameStageCount; ++index)
    {
        stages_[index].stage = static_cast<FrameStage>(index);
    }
}

std::expected<void, ContractError> FrameStageLedger::Accept(FrameStage stage) noexcept
{
    std::size_t const index = static_cast<std::size_t>(stage);
    if (index >= kFrameStageCount)
    {
        return std::unexpected{ContractError::StageOutOfOrder};
    }
    if (stages_[index].recorded)
    {
        return std::unexpected{ContractError::StageAlreadyRecorded};
    }
    if (index != nextStageIndex_)
    {
        return std::unexpected{ContractError::StageOutOfOrder};
    }
    return {};
}

std::expected<void, ContractError> FrameStageLedger::RecordSubmitted(FrameStage stage, std::uint32_t evidenceCount,
                                                                     std::optional<StageOwnership> ownership) noexcept
{
    auto const accepted = Accept(stage);
    if (!accepted)
    {
        return std::unexpected{accepted.error()};
    }
    if (evidenceCount == 0U)
    {
        return std::unexpected{ContractError::EvidenceRequired};
    }

    std::size_t const index = static_cast<std::size_t>(stage);
    stages_[index] = {.stage = stage,
                      .outcome = StageOutcome::Submitted,
                      .recorded = true,
                      .evidenceCount = evidenceCount,
                      .ownership = ownership};
    nextStageIndex_ = index + 1U;
    return {};
}

std::expected<void, ContractError> FrameStageLedger::RecordSkipped(FrameStage stage) noexcept
{
    auto const accepted = Accept(stage);
    if (!accepted)
    {
        return std::unexpected{accepted.error()};
    }
    std::size_t const index = static_cast<std::size_t>(stage);
    stages_[index] = {.stage = stage,
                      .outcome = StageOutcome::Skipped,
                      .recorded = true,
                      .evidenceCount = 0U,
                      .ownership = std::nullopt};
    nextStageIndex_ = index + 1U;
    return {};
}

std::expected<void, ContractError> FrameStageLedger::RecordUnavailable(FrameStage stage) noexcept
{
    auto const accepted = Accept(stage);
    if (!accepted)
    {
        return std::unexpected{accepted.error()};
    }
    std::size_t const index = static_cast<std::size_t>(stage);
    stages_[index] = {.stage = stage,
                      .outcome = StageOutcome::Unavailable,
                      .recorded = true,
                      .evidenceCount = 0U,
                      .ownership = std::nullopt};
    nextStageIndex_ = index + 1U;
    return {};
}

StageRecord FrameStageLedger::Status(FrameStage stage) const noexcept
{
    std::size_t const index = static_cast<std::size_t>(stage);
    if (index >= kFrameStageCount)
    {
        return StageRecord{};
    }
    return stages_[index];
}

std::optional<FrameStage> FrameStageLedger::NextExpectedStage() const noexcept
{
    if (nextStageIndex_ >= kFrameStageCount)
    {
        return std::nullopt;
    }
    return static_cast<FrameStage>(nextStageIndex_);
}

bool FrameStageLedger::IsComplete() const noexcept
{
    return nextStageIndex_ >= kFrameStageCount;
}

std::uint32_t FrameStageLedger::TotalEvidenceCount() const noexcept
{
    std::uint32_t total = 0U;
    for (StageRecord const &record : stages_)
    {
        total += record.evidenceCount;
    }
    return total;
}

std::expected<StagePartition, ContractError> FrameStageLedger::Partition() const noexcept
{
    if (!IsComplete())
    {
        return std::unexpected{ContractError::StagePartitionIncomplete};
    }

    StagePartition partition{};
    for (StageRecord const &record : stages_)
    {
        std::uint32_t const bit = StageBit(record.stage);
        switch (record.outcome)
        {
        case StageOutcome::Submitted:
            partition.submittedMask |= bit;
            break;
        case StageOutcome::Skipped:
            partition.skippedMask |= bit;
            break;
        case StageOutcome::Unavailable:
            partition.unavailableMask |= bit;
            break;
        }
        partition.recordedMask |= bit;
    }

    auto const partitionCheck = ValidateStagePartition(partition, true);
    if (!partitionCheck)
    {
        return std::unexpected{partitionCheck.error()};
    }
    return partition;
}

std::expected<void, ContractError> ValidateStageOwnership(FrameStageLedger const &ledger,
                                                          FrameSlots const &slots) noexcept
{
    auto const slotCheck = ValidateFrameSlots(slots);
    if (!slotCheck)
    {
        return std::unexpected{slotCheck.error()};
    }

    for (std::size_t index = 0U; index < kFrameStageCount; ++index)
    {
        FrameStage const stage = static_cast<FrameStage>(index);
        StageRecord const record = ledger.Status(stage);
        if (!record.recorded)
        {
            continue;
        }
        if (record.outcome != StageOutcome::Submitted)
        {
            if (record.ownership)
            {
                return std::unexpected{ContractError::PingPongOwnershipMismatch};
            }
            continue;
        }

        bool const touchesState = stage == FrameStage::Reset || stage == FrameStage::Emission ||
                                  stage == FrameStage::Simulation || stage == FrameStage::Compaction ||
                                  stage == FrameStage::Render;
        if (touchesState != record.ownership.has_value())
        {
            return std::unexpected{ContractError::PingPongOwnershipMismatch};
        }
        if (!record.ownership)
        {
            continue;
        }

        StageOwnership const ownership = *record.ownership;
        if (ownership.writesBothSlots != (stage == FrameStage::Reset))
        {
            return std::unexpected{ContractError::PingPongOwnershipMismatch};
        }

        switch (stage)
        {
        case FrameStage::Reset:
            if (ownership.writeSlot)
            {
                return std::unexpected{ContractError::PingPongOwnershipMismatch};
            }
            break;
        case FrameStage::Emission:
            if (ownership.readSlot != slots.readSlot)
            {
                return std::unexpected{ContractError::PingPongOwnershipMismatch};
            }
            if (!ownership.writeSlot || *ownership.writeSlot != slots.readSlot)
            {
                return std::unexpected{ContractError::PingPongOwnershipMismatch};
            }
            break;
        case FrameStage::Simulation:
            if (ownership.readSlot != slots.readSlot)
            {
                return std::unexpected{ContractError::PingPongOwnershipMismatch};
            }
            if (!ownership.writeSlot)
            {
                return std::unexpected{ContractError::PingPongOwnershipMismatch};
            }
            if (*ownership.writeSlot == ownership.readSlot)
            {
                return std::unexpected{ContractError::PingPongAliasViolation};
            }
            if (*ownership.writeSlot != slots.writeSlot)
            {
                return std::unexpected{ContractError::PingPongOwnershipMismatch};
            }
            break;
        case FrameStage::Compaction:
        case FrameStage::Render:
            if (ownership.readSlot != slots.writeSlot)
            {
                return std::unexpected{ContractError::PingPongOwnershipMismatch};
            }
            if (ownership.writeSlot)
            {
                return std::unexpected{ContractError::PingPongOwnershipMismatch};
            }
            break;
        case FrameStage::IndirectArguments:
        case FrameStage::Readback:
            return std::unexpected{ContractError::PingPongOwnershipMismatch};
        }
    }
    return {};
}

std::string_view ParticleResourceName(ParticleResource resource) noexcept
{
    std::size_t const index = static_cast<std::size_t>(resource);
    if (index >= kParticleResourceCount)
    {
        return "UnknownParticleResource";
    }
    return kResourceNames[index];
}

std::vector<ResourceTransition> RequiredFrameTransitions(FrameSlots const &slots, bool readbackRequested)
{
    ParticleResource const readResource =
        slots.readSlot == BufferSlot::A ? ParticleResource::StateSlotA : ParticleResource::StateSlotB;
    ParticleResource const writeResource =
        slots.writeSlot == BufferSlot::A ? ParticleResource::StateSlotA : ParticleResource::StateSlotB;

    std::vector<ResourceTransition> required{};
    required.reserve(readbackRequested ? 7U : 6U);

    // The previous frame's simulation appended the slots it retired; this frame's emission consumes them. Without
    // this barrier the emitter allocates from a list the simulation has not finished writing.
    required.push_back({.resource = ParticleResource::FreeList,
                        .producer = FrameStage::Simulation,
                        .consumer = FrameStage::Emission,
                        .syncBefore = BarrierSync::ComputeShading,
                        .accessBefore = BarrierAccess::UnorderedAccess,
                        .syncAfter = BarrierSync::ComputeShading,
                        .accessAfter = BarrierAccess::UnorderedAccess});
    // Emission scattered new particles in place into the slot the simulation is about to read.
    required.push_back({.resource = readResource,
                        .producer = FrameStage::Emission,
                        .consumer = FrameStage::Simulation,
                        .syncBefore = BarrierSync::ComputeShading,
                        .accessBefore = BarrierAccess::UnorderedAccess,
                        .syncAfter = BarrierSync::ComputeShading,
                        .accessAfter = BarrierAccess::ShaderResource});
    required.push_back({.resource = writeResource,
                        .producer = FrameStage::Simulation,
                        .consumer = FrameStage::Compaction,
                        .syncBefore = BarrierSync::ComputeShading,
                        .accessBefore = BarrierAccess::UnorderedAccess,
                        .syncAfter = BarrierSync::ComputeShading,
                        .accessAfter = BarrierAccess::ShaderResource});
    required.push_back({.resource = ParticleResource::CounterBuffer,
                        .producer = FrameStage::Compaction,
                        .consumer = FrameStage::IndirectArguments,
                        .syncBefore = BarrierSync::ComputeShading,
                        .accessBefore = BarrierAccess::UnorderedAccess,
                        .syncAfter = BarrierSync::ComputeShading,
                        .accessAfter = BarrierAccess::ShaderResource});
    required.push_back({.resource = ParticleResource::LiveIndexList,
                        .producer = FrameStage::Compaction,
                        .consumer = FrameStage::Render,
                        .syncBefore = BarrierSync::ComputeShading,
                        .accessBefore = BarrierAccess::UnorderedAccess,
                        .syncAfter = BarrierSync::Draw,
                        .accessAfter = BarrierAccess::ShaderResource});
    // The barrier the whole chapter turns on: a compute write becoming an indirect argument read.
    required.push_back({.resource = ParticleResource::IndirectArgumentBuffer,
                        .producer = FrameStage::IndirectArguments,
                        .consumer = FrameStage::Render,
                        .syncBefore = BarrierSync::ComputeShading,
                        .accessBefore = BarrierAccess::UnorderedAccess,
                        .syncAfter = BarrierSync::ExecuteIndirect,
                        .accessAfter = BarrierAccess::IndirectArgument});
    if (readbackRequested)
    {
        required.push_back({.resource = ParticleResource::CounterBuffer,
                            .producer = FrameStage::IndirectArguments,
                            .consumer = FrameStage::Readback,
                            .syncBefore = BarrierSync::ComputeShading,
                            .accessBefore = BarrierAccess::ShaderResource,
                            .syncAfter = BarrierSync::Copy,
                            .accessAfter = BarrierAccess::CopySource});
    }
    return required;
}

std::expected<BarrierValidation, ContractError> ValidateFrameBarriers(std::span<ResourceTransition const> transitions,
                                                                      FrameSlots const &slots, bool readbackRequested)
{
    auto const slotCheck = ValidateFrameSlots(slots);
    if (!slotCheck)
    {
        return std::unexpected{slotCheck.error()};
    }

    std::vector<ResourceTransition> const required = RequiredFrameTransitions(slots, readbackRequested);
    std::vector<bool> matched(required.size(), false);

    BarrierValidation validation{};
    validation.suppliedCount = static_cast<std::uint32_t>(transitions.size());

    for (ResourceTransition const &supplied : transitions)
    {
        std::size_t found = required.size();
        for (std::size_t index = 0U; index < required.size(); ++index)
        {
            if (required[index].resource == supplied.resource && required[index].producer == supplied.producer &&
                required[index].consumer == supplied.consumer)
            {
                found = index;
                break;
            }
        }
        if (found == required.size())
        {
            return std::unexpected{ContractError::UnknownTransition};
        }
        if (matched[found])
        {
            return std::unexpected{ContractError::DuplicateTransition};
        }
        if (required[found].syncBefore != supplied.syncBefore || required[found].syncAfter != supplied.syncAfter)
        {
            return std::unexpected{ContractError::IncorrectBarrierSync};
        }
        if (required[found].accessBefore != supplied.accessBefore ||
            required[found].accessAfter != supplied.accessAfter)
        {
            return std::unexpected{ContractError::IncorrectBarrierAccess};
        }
        matched[found] = true;
        ++validation.matchedCount;
    }

    for (bool const entry : matched)
    {
        if (!entry)
        {
            return std::unexpected{ContractError::MissingBarrier};
        }
    }
    return validation;
}

} // namespace ch34::particles
