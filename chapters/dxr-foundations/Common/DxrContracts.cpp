#include "DxrContracts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ch33::dxr
{
namespace
{

// One row per `ContractError`. The table is the single source of truth for the enumerator list, the printable name,
// and the stage attribution, so that the three can never disagree. A `std::nullopt` stage is a deliberate statement
// that the error genuinely occurs at more than one stage; it is not a placeholder for "not classified yet".
struct ErrorTableRow final
{
    ContractError error{};
    std::string_view name{};
    std::optional<PipelineStage> stage{};
};

constexpr std::optional<PipelineStage> kSharedStage{};
constexpr std::optional<PipelineStage> kBottomLevel{PipelineStage::BottomLevelBuild};
constexpr std::optional<PipelineStage> kTopLevel{PipelineStage::TopLevelBuild};
constexpr std::optional<PipelineStage> kStateObject{PipelineStage::StateObject};
constexpr std::optional<PipelineStage> kShaderTable{PipelineStage::ShaderTable};
constexpr std::optional<PipelineStage> kDispatch{PipelineStage::Dispatch};

constexpr std::array<ErrorTableRow, 123U> kErrorTable{{
    {ContractError::NonFinite, "NonFinite", kSharedStage},
    {ContractError::ArithmeticOverflow, "ArithmeticOverflow", kSharedStage},
    {ContractError::InvalidAlignment, "InvalidAlignment", kSharedStage},

    {ContractError::UnsupportedVertexFormat, "UnsupportedVertexFormat", kBottomLevel},
    {ContractError::UnsupportedIndexFormat, "UnsupportedIndexFormat", kBottomLevel},
    {ContractError::ZeroVertexCount, "ZeroVertexCount", kBottomLevel},
    {ContractError::VertexCountNotTriangleMultiple, "VertexCountNotTriangleMultiple", kBottomLevel},
    {ContractError::VertexStrideTooSmall, "VertexStrideTooSmall", kBottomLevel},
    {ContractError::VertexStrideMisaligned, "VertexStrideMisaligned", kBottomLevel},
    {ContractError::VertexBufferAddressNull, "VertexBufferAddressNull", kBottomLevel},
    {ContractError::VertexBufferAddressMisaligned, "VertexBufferAddressMisaligned", kBottomLevel},
    {ContractError::IndexedGeometryMissingIndexCount, "IndexedGeometryMissingIndexCount", kBottomLevel},
    {ContractError::IndexCountNotTriangleMultiple, "IndexCountNotTriangleMultiple", kBottomLevel},
    {ContractError::IndexBufferAddressNull, "IndexBufferAddressNull", kBottomLevel},
    {ContractError::IndexBufferAddressMisaligned, "IndexBufferAddressMisaligned", kBottomLevel},
    {ContractError::NonIndexedGeometryDeclaresIndexBuffer, "NonIndexedGeometryDeclaresIndexBuffer", kBottomLevel},
    {ContractError::TransformAddressMisaligned, "TransformAddressMisaligned", kBottomLevel},
    {ContractError::ConflictingGeometryFlags, "ConflictingGeometryFlags", kBottomLevel},
    {ContractError::PrimitiveCountExceeded, "PrimitiveCountExceeded", kBottomLevel},
    {ContractError::GeometryCountExceeded, "GeometryCountExceeded", kBottomLevel},
    {ContractError::EmptyGeometrySet, "EmptyGeometrySet", kBottomLevel},

    {ContractError::ZeroResultSize, "ZeroResultSize", kSharedStage},
    {ContractError::ResultSizeMisaligned, "ResultSizeMisaligned", kSharedStage},
    {ContractError::ZeroScratchSize, "ZeroScratchSize", kSharedStage},
    {ContractError::UnexpectedUpdateScratchSize, "UnexpectedUpdateScratchSize", kSharedStage},
    {ContractError::MissingUpdateScratchSize, "MissingUpdateScratchSize", kSharedStage},
    {ContractError::DestinationAddressNull, "DestinationAddressNull", kSharedStage},
    {ContractError::DestinationAddressMisaligned, "DestinationAddressMisaligned", kSharedStage},
    {ContractError::DestinationTooSmall, "DestinationTooSmall", kSharedStage},
    {ContractError::ScratchAddressNull, "ScratchAddressNull", kSharedStage},
    {ContractError::ScratchAddressMisaligned, "ScratchAddressMisaligned", kSharedStage},
    {ContractError::ScratchTooSmall, "ScratchTooSmall", kSharedStage},
    {ContractError::SourceRequiredForUpdate, "SourceRequiredForUpdate", kSharedStage},
    {ContractError::UnexpectedSourceForBuild, "UnexpectedSourceForBuild", kSharedStage},
    {ContractError::SourceAddressMisaligned, "SourceAddressMisaligned", kSharedStage},
    {ContractError::SourceTooSmall, "SourceTooSmall", kSharedStage},
    {ContractError::UpdateWithoutAllowUpdate, "UpdateWithoutAllowUpdate", kSharedStage},
    {ContractError::UpdateFlagsMismatch, "UpdateFlagsMismatch", kSharedStage},
    {ContractError::UpdateTopologyMismatch, "UpdateTopologyMismatch", kSharedStage},
    {ContractError::ConflictingBuildFlags, "ConflictingBuildFlags", kSharedStage},
    {ContractError::OverlappingBuildRanges, "OverlappingBuildRanges", kSharedStage},
    {ContractError::ZeroBuildElementCount, "ZeroBuildElementCount", kSharedStage},

    {ContractError::EmptyTimeline, "EmptyTimeline", kSharedStage},
    {ContractError::InvalidResourceIdentifier, "InvalidResourceIdentifier", kSharedStage},
    {ContractError::ConsumesUnbuiltAccelerationStructure, "ConsumesUnbuiltAccelerationStructure", kSharedStage},
    {ContractError::MissingProducerBarrier, "MissingProducerBarrier", kSharedStage},
    {ContractError::MissingScratchBarrier, "MissingScratchBarrier", kSharedStage},
    {ContractError::WrongAccelerationStructureKindConsumed, "WrongAccelerationStructureKindConsumed", kSharedStage},
    {ContractError::DispatchWithoutTopLevel, "DispatchWithoutTopLevel", kDispatch},
    {ContractError::RebuildWithoutConsumerBarrier, "RebuildWithoutConsumerBarrier", kSharedStage},

    {ContractError::InstanceIdOutOfRange, "InstanceIdOutOfRange", kTopLevel},
    {ContractError::InstanceMaskOutOfRange, "InstanceMaskOutOfRange", kTopLevel},
    {ContractError::InstanceMaskSelectsNothing, "InstanceMaskSelectsNothing", kTopLevel},
    {ContractError::InstanceContributionOutOfRange, "InstanceContributionOutOfRange", kTopLevel},
    {ContractError::ConflictingInstanceFlags, "ConflictingInstanceFlags", kTopLevel},
    {ContractError::BottomLevelAddressNull, "BottomLevelAddressNull", kTopLevel},
    {ContractError::BottomLevelAddressMisaligned, "BottomLevelAddressMisaligned", kTopLevel},
    {ContractError::SingularInstanceTransform, "SingularInstanceTransform", kTopLevel},
    {ContractError::DuplicateInstanceIdentity, "DuplicateInstanceIdentity", kTopLevel},
    {ContractError::InstanceCountExceeded, "InstanceCountExceeded", kTopLevel},
    {ContractError::EmptyInstanceSet, "EmptyInstanceSet", kTopLevel},
    {ContractError::InstanceBufferAddressNull, "InstanceBufferAddressNull", kTopLevel},
    {ContractError::InstanceBufferAddressMisaligned, "InstanceBufferAddressMisaligned", kTopLevel},
    {ContractError::InstanceBufferTooSmall, "InstanceBufferTooSmall", kTopLevel},

    {ContractError::NegativeRayTMin, "NegativeRayTMin", kDispatch},
    {ContractError::InvertedRayInterval, "InvertedRayInterval", kDispatch},
    {ContractError::RayIntervalTooLarge, "RayIntervalTooLarge", kDispatch},
    {ContractError::ZeroLengthRayDirection, "ZeroLengthRayDirection", kDispatch},
    {ContractError::ConflictingRayFlags, "ConflictingRayFlags", kDispatch},
    {ContractError::RayMaskOutOfRange, "RayMaskOutOfRange", kDispatch},
    {ContractError::RayMaskSelectsNothing, "RayMaskSelectsNothing", kDispatch},

    {ContractError::EmptyExportName, "EmptyExportName", kStateObject},
    {ContractError::DuplicateExportName, "DuplicateExportName", kStateObject},
    {ContractError::MissingRayGenerationShader, "MissingRayGenerationShader", kStateObject},
    {ContractError::MissingMissShader, "MissingMissShader", kStateObject},
    {ContractError::HitGroupMissingShaders, "HitGroupMissingShaders", kStateObject},
    {ContractError::TriangleHitGroupDeclaresIntersection, "TriangleHitGroupDeclaresIntersection", kStateObject},
    {ContractError::ProceduralHitGroupMissingIntersection, "ProceduralHitGroupMissingIntersection", kStateObject},
    {ContractError::HitGroupReferencesUnknownExport, "HitGroupReferencesUnknownExport", kStateObject},
    {ContractError::HitGroupExportStageMismatch, "HitGroupExportStageMismatch", kStateObject},
    {ContractError::InvalidPayloadSize, "InvalidPayloadSize", kStateObject},
    {ContractError::InvalidAttributeSize, "InvalidAttributeSize", kStateObject},
    {ContractError::InvalidTraceRecursionDepth, "InvalidTraceRecursionDepth", kStateObject},
    {ContractError::DuplicateLocalRootSignatureName, "DuplicateLocalRootSignatureName", kStateObject},
    {ContractError::UnknownLocalRootSignature, "UnknownLocalRootSignature", kStateObject},
    {ContractError::AssociationReferencesUnknownExport, "AssociationReferencesUnknownExport", kStateObject},
    {ContractError::ConflictingLocalRootSignatureAssociation, "ConflictingLocalRootSignatureAssociation", kStateObject},
    {ContractError::MismatchedHitGroupLocalRootSignature, "MismatchedHitGroupLocalRootSignature", kStateObject},
    {ContractError::InvalidLocalRootArgumentSize, "InvalidLocalRootArgumentSize", kSharedStage},
    {ContractError::ShaderRecordStrideExceeded, "ShaderRecordStrideExceeded", kSharedStage},

    {ContractError::EmptyPayloadLayout, "EmptyPayloadLayout", kStateObject},
    {ContractError::EmptyPayloadFieldName, "EmptyPayloadFieldName", kStateObject},
    {ContractError::DuplicatePayloadFieldName, "DuplicatePayloadFieldName", kStateObject},
    {ContractError::InvalidPayloadFieldSize, "InvalidPayloadFieldSize", kStateObject},
    {ContractError::InvalidPayloadFieldAlignment, "InvalidPayloadFieldAlignment", kStateObject},
    {ContractError::ZeroPayloadElementCount, "ZeroPayloadElementCount", kStateObject},
    {ContractError::PayloadSizeExceeded, "PayloadSizeExceeded", kStateObject},

    {ContractError::InvalidShaderIdentifierSize, "InvalidShaderIdentifierSize", kShaderTable},
    {ContractError::ZeroShaderIdentifier, "ZeroShaderIdentifier", kShaderTable},
    {ContractError::EmptyShaderTable, "EmptyShaderTable", kShaderTable},
    {ContractError::EmptyShaderTableSection, "EmptyShaderTableSection", kShaderTable},
    {ContractError::DuplicateShaderTableSection, "DuplicateShaderTableSection", kShaderTable},
    {ContractError::RayGenerationSectionMustHoldOneRecord, "RayGenerationSectionMustHoldOneRecord", kShaderTable},
    {ContractError::ShaderTableSizeExceeded, "ShaderTableSizeExceeded", kShaderTable},
    {ContractError::ShaderTableSectionMissing, "ShaderTableSectionMissing", kShaderTable},
    {ContractError::TableBaseAddressNull, "TableBaseAddressNull", kShaderTable},
    {ContractError::TableBaseAddressMisaligned, "TableBaseAddressMisaligned", kShaderTable},

    {ContractError::RayContributionOutOfRange, "RayContributionOutOfRange", kDispatch},
    {ContractError::GeometryContributionMultiplierOutOfRange, "GeometryContributionMultiplierOutOfRange", kDispatch},
    {ContractError::GeometryContributionOutOfRange, "GeometryContributionOutOfRange", kDispatch},
    {ContractError::HitGroupRecordIndexOutOfRange, "HitGroupRecordIndexOutOfRange", kDispatch},
    {ContractError::MissShaderIndexOutOfRange, "MissShaderIndexOutOfRange", kDispatch},
    {ContractError::MissRecordIndexOutOfRange, "MissRecordIndexOutOfRange", kDispatch},
    {ContractError::CallableRecordIndexOutOfRange, "CallableRecordIndexOutOfRange", kDispatch},
    {ContractError::ZeroDispatchDimension, "ZeroDispatchDimension", kDispatch},
    {ContractError::DispatchThreadCountExceeded, "DispatchThreadCountExceeded", kDispatch},

    {ContractError::EmptyReferenceGeometry, "EmptyReferenceGeometry", kDispatch},
    {ContractError::ReferenceSceneTooLarge, "ReferenceSceneTooLarge", kDispatch},
    {ContractError::DegenerateTriangle, "DegenerateTriangle", kDispatch},

    {ContractError::StageOutOfOrder, "StageOutOfOrder", kSharedStage},
    {ContractError::StageAlreadyRecorded, "StageAlreadyRecorded", kSharedStage},
    {ContractError::MissingStageEvidence, "MissingStageEvidence", kSharedStage},
    {ContractError::StageAfterFailure, "StageAfterFailure", kSharedStage},
}};

// The table is indexed by the enumerator's value, so every row has to sit at its own index. If an enumerator is
// inserted, reordered, or removed without the table being updated, this fails at compile time rather than silently
// printing the wrong name for the rest of the chapter.
constexpr bool ErrorTableIsDense() noexcept
{
    for (std::size_t index = 0U; index < kErrorTable.size(); ++index)
    {
        if (static_cast<std::size_t>(kErrorTable[index].error) != index)
        {
            return false;
        }
    }

    return true;
}

static_assert(ErrorTableIsDense(), "ContractError rows must appear in enumerator order with no gaps");
static_assert(kErrorTable.back().error == ContractError::StageAfterFailure,
              "The last table row must be the last enumerator, so that appended errors are noticed here");

constexpr std::array<ContractError, kErrorTable.size()> kAllErrors = []()
{
    std::array<ContractError, kErrorTable.size()> values{};
    for (std::size_t index = 0U; index < kErrorTable.size(); ++index)
    {
        values[index] = kErrorTable[index].error;
    }

    return values;
}();

} // namespace

std::span<ContractError const> AllContractErrors() noexcept
{
    return std::span<ContractError const>{kAllErrors};
}

std::string_view ContractErrorName(ContractError error) noexcept
{
    auto const index = static_cast<std::size_t>(error);
    if (index >= kErrorTable.size())
    {
        return std::string_view{"UnknownContractError"};
    }

    return kErrorTable[index].name;
}

std::string_view PipelineStageName(PipelineStage stage) noexcept
{
    switch (stage)
    {
    case PipelineStage::BottomLevelBuild:
        return std::string_view{"BottomLevelBuild"};
    case PipelineStage::TopLevelBuild:
        return std::string_view{"TopLevelBuild"};
    case PipelineStage::StateObject:
        return std::string_view{"StateObject"};
    case PipelineStage::ShaderTable:
        return std::string_view{"ShaderTable"};
    case PipelineStage::Dispatch:
        return std::string_view{"Dispatch"};
    }

    return std::string_view{"UnknownPipelineStage"};
}

std::optional<PipelineStage> StageForError(ContractError error) noexcept
{
    auto const index = static_cast<std::size_t>(error);
    if (index >= kErrorTable.size())
    {
        return std::nullopt;
    }

    return kErrorTable[index].stage;
}

PipelineStage StageForBuild(AccelerationStructureKind kind) noexcept
{
    return kind == AccelerationStructureKind::BottomLevel ? PipelineStage::BottomLevelBuild
                                                          : PipelineStage::TopLevelBuild;
}

StageDiagnostic MakeStageDiagnostic(PipelineStage reportingStage, ContractError error) noexcept
{
    auto const intrinsicStage = StageForError(error);

    StageDiagnostic diagnostic{};
    diagnostic.stage = intrinsicStage.value_or(reportingStage);
    diagnostic.error = error;
    diagnostic.stageName = PipelineStageName(diagnostic.stage);
    diagnostic.errorName = ContractErrorName(error);
    diagnostic.stageIsIntrinsic = intrinsicStage.has_value();
    return diagnostic;
}

// -------------------------------------------------------------------------------------------------------------
// Shared arithmetic and geometry helpers
// -------------------------------------------------------------------------------------------------------------

namespace
{

constexpr std::uint64_t kUnsignedMaximum = std::numeric_limits<std::uint64_t>::max();

bool IsFiniteValue(double value) noexcept
{
    return std::isfinite(value);
}

bool IsFiniteVector(Float3 value) noexcept
{
    return IsFiniteValue(value.x) && IsFiniteValue(value.y) && IsFiniteValue(value.z);
}

bool IsFiniteTransform(Transform3x4 const &transform) noexcept
{
    return std::ranges::all_of(transform.values, [](double value) { return IsFiniteValue(value); });
}

std::expected<std::uint64_t, ContractError> CheckedAdd(std::uint64_t left, std::uint64_t right) noexcept
{
    if (left > kUnsignedMaximum - right)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }

    return left + right;
}

std::expected<std::uint64_t, ContractError> CheckedMultiply(std::uint64_t left, std::uint64_t right) noexcept
{
    if (left != 0U && right > kUnsignedMaximum / left)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }

    return left * right;
}

// Half-open ranges overlap when each starts before the other ends. A zero-sized range covers nothing and therefore
// overlaps nothing, which is what makes "no source buffer" express itself as an empty range rather than a special
// case here.
bool RangesOverlap(BufferRange const &left, BufferRange const &right) noexcept
{
    if (left.sizeBytes == 0U || right.sizeBytes == 0U)
    {
        return false;
    }

    auto const leftEnd = left.address + std::min(left.sizeBytes, kUnsignedMaximum - left.address);
    auto const rightEnd = right.address + std::min(right.sizeBytes, kUnsignedMaximum - right.address);
    return left.address < rightEnd && right.address < leftEnd;
}

Float3 Subtract(Float3 left, Float3 right) noexcept
{
    return Float3{.x = left.x - right.x, .y = left.y - right.y, .z = left.z - right.z};
}

Float3 Cross(Float3 left, Float3 right) noexcept
{
    return Float3{.x = (left.y * right.z) - (left.z * right.y),
                  .y = (left.z * right.x) - (left.x * right.z),
                  .z = (left.x * right.y) - (left.y * right.x)};
}

double Dot(Float3 left, Float3 right) noexcept
{
    return (left.x * right.x) + (left.y * right.y) + (left.z * right.z);
}

double Length(Float3 value) noexcept
{
    return std::sqrt(Dot(value, value));
}

template <DxrFlagEnum FlagsT> bool HasUndefinedBits(FlagsT value, FlagsT definedMask) noexcept
{
    using Underlying = std::underlying_type_t<FlagsT>;
    auto const bits = static_cast<std::uint32_t>(static_cast<Underlying>(value));
    auto const defined = static_cast<std::uint32_t>(static_cast<Underlying>(definedMask));
    return (bits & ~defined) != 0U;
}

constexpr GeometryFlags kDefinedGeometryFlags = GeometryFlags::Opaque | GeometryFlags::NoDuplicateAnyHitInvocation;

constexpr BuildFlags kDefinedBuildFlags = BuildFlags::AllowUpdate | BuildFlags::AllowCompaction |
                                          BuildFlags::PreferFastTrace | BuildFlags::PreferFastBuild |
                                          BuildFlags::MinimizeMemory;

constexpr InstanceFlags kDefinedInstanceFlags = InstanceFlags::TriangleCullDisable |
                                                InstanceFlags::TriangleFrontCounterclockwise |
                                                InstanceFlags::ForceOpaque | InstanceFlags::ForceNonOpaque;

constexpr RayFlags kDefinedRayFlags =
    RayFlags::ForceOpaque | RayFlags::ForceNonOpaque | RayFlags::AcceptFirstHitAndEndSearch |
    RayFlags::SkipClosestHitShader | RayFlags::CullBackFacingTriangles | RayFlags::CullFrontFacingTriangles |
    RayFlags::CullOpaque | RayFlags::CullNonOpaque | RayFlags::SkipTriangles | RayFlags::SkipProceduralPrimitives;

} // namespace

// -------------------------------------------------------------------------------------------------------------
// Small shared vocabulary
// -------------------------------------------------------------------------------------------------------------

Transform3x4 IdentityTransform() noexcept
{
    Transform3x4 transform{};
    transform.values[0U] = 1.0;
    transform.values[5U] = 1.0;
    transform.values[10U] = 1.0;
    return transform;
}

Transform3x4 TranslationTransform(Float3 translationMetres) noexcept
{
    Transform3x4 transform = IdentityTransform();
    transform.values[3U] = translationMetres.x;
    transform.values[7U] = translationMetres.y;
    transform.values[11U] = translationMetres.z;
    return transform;
}

Transform3x4 ScaleTransform(Float3 scale) noexcept
{
    Transform3x4 transform{};
    transform.values[0U] = scale.x;
    transform.values[5U] = scale.y;
    transform.values[10U] = scale.z;
    return transform;
}

double TransformDeterminant(Transform3x4 const &transform) noexcept
{
    auto const &m = transform.values;
    return (m[0U] * ((m[5U] * m[10U]) - (m[6U] * m[9U]))) - (m[1U] * ((m[4U] * m[10U]) - (m[6U] * m[8U]))) +
           (m[2U] * ((m[4U] * m[9U]) - (m[5U] * m[8U])));
}

Float3 TransformPoint(Transform3x4 const &transform, Float3 point) noexcept
{
    auto const &m = transform.values;
    return Float3{.x = (m[0U] * point.x) + (m[1U] * point.y) + (m[2U] * point.z) + m[3U],
                  .y = (m[4U] * point.x) + (m[5U] * point.y) + (m[6U] * point.z) + m[7U],
                  .z = (m[8U] * point.x) + (m[9U] * point.y) + (m[10U] * point.z) + m[11U]};
}

Float3 TransformDirection(Transform3x4 const &transform, Float3 direction) noexcept
{
    auto const &m = transform.values;
    return Float3{.x = (m[0U] * direction.x) + (m[1U] * direction.y) + (m[2U] * direction.z),
                  .y = (m[4U] * direction.x) + (m[5U] * direction.y) + (m[6U] * direction.z),
                  .z = (m[8U] * direction.x) + (m[9U] * direction.y) + (m[10U] * direction.z)};
}

std::expected<std::uint64_t, ContractError> AlignUp(std::uint64_t value, std::uint64_t alignment) noexcept
{
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U)
    {
        return std::unexpected(ContractError::InvalidAlignment);
    }

    if (value > kUnsignedMaximum - (alignment - 1U))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }

    return (value + alignment - 1U) & ~(alignment - 1U);
}

bool IsAligned(std::uint64_t value, std::uint64_t alignment) noexcept
{
    if (alignment == 0U)
    {
        return false;
    }

    return (value % alignment) == 0U;
}

// -------------------------------------------------------------------------------------------------------------
// A. Triangle geometry description
// -------------------------------------------------------------------------------------------------------------

std::expected<std::uint32_t, ContractError> VertexFormatComponentSizeBytes(VertexFormat format) noexcept
{
    switch (format)
    {
    case VertexFormat::R32G32B32Float:
    case VertexFormat::R32G32Float:
        return 4U;
    case VertexFormat::R16G16B16A16Float:
    case VertexFormat::R16G16Float:
    case VertexFormat::R16G16B16A16Snorm:
    case VertexFormat::R16G16Snorm:
        return 2U;
    case VertexFormat::R32G32B32A32Float:
        break;
    }

    return std::unexpected(ContractError::UnsupportedVertexFormat);
}

std::expected<std::uint32_t, ContractError> VertexFormatElementSizeBytes(VertexFormat format) noexcept
{
    switch (format)
    {
    case VertexFormat::R32G32B32Float:
        return 12U;
    case VertexFormat::R32G32Float:
    case VertexFormat::R16G16B16A16Float:
    case VertexFormat::R16G16B16A16Snorm:
        return 8U;
    case VertexFormat::R16G16Float:
    case VertexFormat::R16G16Snorm:
        return 4U;
    case VertexFormat::R32G32B32A32Float:
        break;
    }

    return std::unexpected(ContractError::UnsupportedVertexFormat);
}

// What the build actually reads out of one vertex. The four-component 16-bit formats are the interesting case:
// DXR ignores the A component, so the position occupies six bytes even though the DXGI element is eight, and a
// stride of six is explicitly blessed by the specification. The two-component formats assume a third component of
// zero, which costs no bytes at all.
std::expected<std::uint32_t, ContractError> VertexFormatPositionSizeBytes(VertexFormat format) noexcept
{
    switch (format)
    {
    case VertexFormat::R32G32B32Float:
        return 12U;
    case VertexFormat::R32G32Float:
        return 8U;
    case VertexFormat::R16G16B16A16Float:
    case VertexFormat::R16G16B16A16Snorm:
        return 6U;
    case VertexFormat::R16G16Float:
    case VertexFormat::R16G16Snorm:
        return 4U;
    case VertexFormat::R32G32B32A32Float:
        break;
    }

    return std::unexpected(ContractError::UnsupportedVertexFormat);
}

std::expected<std::uint32_t, ContractError> IndexFormatSizeBytes(IndexFormat format) noexcept
{
    switch (format)
    {
    case IndexFormat::None:
        return 0U;
    case IndexFormat::R16Uint:
        return 2U;
    case IndexFormat::R32Uint:
        return 4U;
    }

    return std::unexpected(ContractError::UnsupportedIndexFormat);
}

// The order of the checks is part of the contract, because a description with two problems has to fail the same way
// every time for a test to be able to name the failure. It runs from the cheapest and most structural facts (flags,
// formats) through counts, then strides, then addresses, and finally the per-structure limits.
std::expected<TriangleGeometryValidation, ContractError> ValidateTriangleGeometry(
    TriangleGeometryDescription const &geometry) noexcept
{
    if (HasUndefinedBits(geometry.flags, kDefinedGeometryFlags))
    {
        return std::unexpected(ContractError::ConflictingGeometryFlags);
    }

    auto const componentSizeBytes = VertexFormatComponentSizeBytes(geometry.vertexFormat);
    if (!componentSizeBytes.has_value())
    {
        return std::unexpected(componentSizeBytes.error());
    }

    // The build reads the position bytes, not the whole DXGI element, so this is what bounds the stride and what
    // the last vertex contributes to the footprint. The element size is a separate query and is not used here.
    auto const positionSizeBytes = VertexFormatPositionSizeBytes(geometry.vertexFormat);
    if (!positionSizeBytes.has_value())
    {
        return std::unexpected(positionSizeBytes.error());
    }

    auto const indexSizeBytes = IndexFormatSizeBytes(geometry.indexFormat);
    if (!indexSizeBytes.has_value())
    {
        return std::unexpected(indexSizeBytes.error());
    }

    if (geometry.vertexCount == 0U)
    {
        return std::unexpected(ContractError::ZeroVertexCount);
    }

    bool const indexed = geometry.indexFormat != IndexFormat::None;
    std::uint64_t primitiveCount = 0U;
    if (indexed)
    {
        if (geometry.indexCount == 0U)
        {
            return std::unexpected(ContractError::IndexedGeometryMissingIndexCount);
        }

        if ((geometry.indexCount % 3U) != 0U)
        {
            return std::unexpected(ContractError::IndexCountNotTriangleMultiple);
        }

        primitiveCount = geometry.indexCount / 3U;
    }
    else
    {
        // A non-indexed geometry that carries an index buffer is a description whose two halves disagree; silently
        // preferring either one is how a mesh renders as a subset of itself.
        if (geometry.indexCount != 0U || geometry.indexBufferAddress != 0U)
        {
            return std::unexpected(ContractError::NonIndexedGeometryDeclaresIndexBuffer);
        }

        if ((geometry.vertexCount % 3U) != 0U)
        {
            return std::unexpected(ContractError::VertexCountNotTriangleMultiple);
        }

        primitiveCount = geometry.vertexCount / 3U;
    }

    if (geometry.vertexStrideBytes < static_cast<std::uint64_t>(*positionSizeBytes))
    {
        return std::unexpected(ContractError::VertexStrideTooSmall);
    }

    // D3D12 requires the vertex stride and the vertex buffer address to be multiples of the format's component
    // size, not of the whole vertex: a 12-byte float3 position may sit at any 4-byte boundary.
    if (!IsAligned(geometry.vertexStrideBytes, *componentSizeBytes))
    {
        return std::unexpected(ContractError::VertexStrideMisaligned);
    }

    if (geometry.vertexBufferAddress == 0U)
    {
        return std::unexpected(ContractError::VertexBufferAddressNull);
    }

    if (!IsAligned(geometry.vertexBufferAddress, *componentSizeBytes))
    {
        return std::unexpected(ContractError::VertexBufferAddressMisaligned);
    }

    if (indexed)
    {
        if (geometry.indexBufferAddress == 0U)
        {
            return std::unexpected(ContractError::IndexBufferAddressNull);
        }

        if (!IsAligned(geometry.indexBufferAddress, *indexSizeBytes))
        {
            return std::unexpected(ContractError::IndexBufferAddressMisaligned);
        }
    }

    if (geometry.transform3x4Address != 0U && !IsAligned(geometry.transform3x4Address, kTransform3x4AlignmentBytes))
    {
        return std::unexpected(ContractError::TransformAddressMisaligned);
    }

    if (primitiveCount > kMaximumPrimitivesPerBottomLevel)
    {
        return std::unexpected(ContractError::PrimitiveCountExceeded);
    }

    // The last vertex contributes only the bytes the build reads rather than a whole stride: the padding after it,
    // and the ignored A component where the format has one, are never read. Counting them is how a buffer that is
    // exactly large enough gets rejected as too small.
    auto const stridedSpan =
        CheckedMultiply(static_cast<std::uint64_t>(geometry.vertexCount - 1U), geometry.vertexStrideBytes);
    if (!stridedSpan.has_value())
    {
        return std::unexpected(stridedSpan.error());
    }

    auto const vertexFootprint = CheckedAdd(*stridedSpan, static_cast<std::uint64_t>(*positionSizeBytes));
    if (!vertexFootprint.has_value())
    {
        return std::unexpected(vertexFootprint.error());
    }

    auto const indexFootprint =
        CheckedMultiply(static_cast<std::uint64_t>(geometry.indexCount), static_cast<std::uint64_t>(*indexSizeBytes));
    if (!indexFootprint.has_value())
    {
        return std::unexpected(indexFootprint.error());
    }

    bool const opaque = HasFlag(geometry.flags, GeometryFlags::Opaque);
    bool const noDuplicateAnyHit = HasFlag(geometry.flags, GeometryFlags::NoDuplicateAnyHitInvocation);

    TriangleGeometryValidation validation{};
    validation.primitiveCount = static_cast<std::uint32_t>(primitiveCount);
    validation.indexed = indexed;
    validation.opaque = opaque;
    validation.anyHitCanRun = !opaque;
    validation.duplicateAnyHitSuppressed = noDuplicateAnyHit;
    validation.duplicateAnyHitSuppressionRedundant = opaque && noDuplicateAnyHit;
    validation.hasBuildTimeTransform = geometry.transform3x4Address != 0U;
    validation.vertexBufferFootprintBytes = *vertexFootprint;
    validation.indexBufferFootprintBytes = *indexFootprint;
    return validation;
}

std::expected<BottomLevelGeometrySetValidation, ContractError> ValidateBottomLevelGeometrySet(
    std::span<TriangleGeometryDescription const> geometries)
{
    if (geometries.empty())
    {
        return std::unexpected(ContractError::EmptyGeometrySet);
    }

    if (geometries.size() > kMaximumGeometriesPerBottomLevel)
    {
        return std::unexpected(ContractError::GeometryCountExceeded);
    }

    BottomLevelGeometrySetValidation validation{};
    validation.geometryCount = static_cast<std::uint32_t>(geometries.size());
    validation.maximumGeometryContribution = validation.geometryCount - 1U;

    for (auto const &geometry : geometries)
    {
        auto const geometryValidation = ValidateTriangleGeometry(geometry);
        if (!geometryValidation.has_value())
        {
            return std::unexpected(geometryValidation.error());
        }

        auto const total =
            CheckedAdd(validation.totalPrimitiveCount, static_cast<std::uint64_t>(geometryValidation->primitiveCount));
        if (!total.has_value())
        {
            return std::unexpected(total.error());
        }

        validation.totalPrimitiveCount = *total;
        validation.anyGeometryAllowsAnyHit = validation.anyGeometryAllowsAnyHit || geometryValidation->anyHitCanRun;
    }

    // Individually legal geometries can still exceed the per-structure primitive budget, which is why the sum is
    // checked here and not inside the per-geometry validation.
    if (validation.totalPrimitiveCount > kMaximumPrimitivesPerBottomLevel)
    {
        return std::unexpected(ContractError::PrimitiveCountExceeded);
    }

    return validation;
}

// -------------------------------------------------------------------------------------------------------------
// B. Build sizing, alignment, and update rules
// -------------------------------------------------------------------------------------------------------------

std::expected<void, ContractError> ValidatePrebuildInfo(BuildFlags flags, PrebuildInfo const &prebuild) noexcept
{
    if (HasUndefinedBits(flags, kDefinedBuildFlags))
    {
        return std::unexpected(ContractError::ConflictingBuildFlags);
    }

    if (prebuild.resultDataMaxSizeBytes == 0U)
    {
        return std::unexpected(ContractError::ZeroResultSize);
    }

    // D3D12 reports the result size already rounded to the acceleration-structure alignment. A triple that is not
    // is a triple the caller invented, and inventing prebuild sizes is the one thing this chapter cannot do.
    if (!IsAligned(prebuild.resultDataMaxSizeBytes, kAccelerationStructureAlignmentBytes))
    {
        return std::unexpected(ContractError::ResultSizeMisaligned);
    }

    if (prebuild.scratchDataSizeBytes == 0U)
    {
        return std::unexpected(ContractError::ZeroScratchSize);
    }

    if (HasFlag(flags, BuildFlags::AllowUpdate))
    {
        if (prebuild.updateScratchDataSizeBytes == 0U)
        {
            return std::unexpected(ContractError::MissingUpdateScratchSize);
        }
    }
    else if (prebuild.updateScratchDataSizeBytes != 0U)
    {
        return std::unexpected(ContractError::UnexpectedUpdateScratchSize);
    }

    return {};
}

std::expected<AccelerationStructureBuildPlan, ContractError> ValidateBuildRequest(
    AccelerationStructureBuildRequest const &request) noexcept
{
    if (HasUndefinedBits(request.flags, kDefinedBuildFlags) ||
        HasUndefinedBits(request.sourceFlags, kDefinedBuildFlags))
    {
        return std::unexpected(ContractError::ConflictingBuildFlags);
    }

    // `PreferFastTrace` and `PreferFastBuild` ask the driver for opposite tradeoffs. D3D12 rejects the pair rather
    // than picking one, and so does this chapter.
    if (HasFlag(request.flags, BuildFlags::PreferFastTrace) && HasFlag(request.flags, BuildFlags::PreferFastBuild))
    {
        return std::unexpected(ContractError::ConflictingBuildFlags);
    }

    bool const isUpdate = request.mode == BuildMode::Update;
    if (isUpdate)
    {
        // The permission belongs to the *source*, not to this request: "The source acceleration structure must have
        // specified ALLOW_UPDATE". A request that omits `AllowUpdate` is not an error, it is the final update.
        if (!HasFlag(request.sourceFlags, BuildFlags::AllowUpdate))
        {
            return std::unexpected(ContractError::UpdateWithoutAllowUpdate);
        }

        // "The other flags selections, aside from ALLOW_UPDATE and PERFORM_UPDATE, must match the flags in the
        // source acceleration structure." `PerformUpdate` is this chapter's `BuildMode` and is not in the flag set,
        // so `AllowUpdate` is the only bit masked out of the comparison.
        constexpr auto comparedFlags = static_cast<std::underlying_type_t<BuildFlags>>(
            static_cast<std::underlying_type_t<BuildFlags>>(kDefinedBuildFlags) &
            ~static_cast<std::underlying_type_t<BuildFlags>>(BuildFlags::AllowUpdate));
        auto const requestedComparable = static_cast<std::underlying_type_t<BuildFlags>>(request.flags) & comparedFlags;
        auto const sourceComparable =
            static_cast<std::underlying_type_t<BuildFlags>>(request.sourceFlags) & comparedFlags;
        if (requestedComparable != sourceComparable)
        {
            return std::unexpected(ContractError::UpdateFlagsMismatch);
        }
    }

    // The prebuild triple an update works from is the one that described the *source* structure, because that is
    // where `UpdateScratchDataSizeInBytes` came from. Validating a final update against its own flags would demand
    // a zero update scratch size for the very operation that needs it.
    auto const prebuildValidation =
        ValidatePrebuildInfo(isUpdate ? request.sourceFlags : request.flags, request.prebuild);
    if (!prebuildValidation.has_value())
    {
        return std::unexpected(prebuildValidation.error());
    }

    if (request.elementCount == 0U)
    {
        return std::unexpected(ContractError::ZeroBuildElementCount);
    }

    if (request.kind == AccelerationStructureKind::BottomLevel)
    {
        if (static_cast<std::uint64_t>(request.elementCount) > kMaximumGeometriesPerBottomLevel)
        {
            return std::unexpected(ContractError::GeometryCountExceeded);
        }
    }
    else if (static_cast<std::uint64_t>(request.elementCount) > kMaximumInstancesPerTopLevel)
    {
        return std::unexpected(ContractError::InstanceCountExceeded);
    }

    if (request.destination.address == 0U)
    {
        return std::unexpected(ContractError::DestinationAddressNull);
    }

    if (!IsAligned(request.destination.address, kAccelerationStructureAlignmentBytes))
    {
        return std::unexpected(ContractError::DestinationAddressMisaligned);
    }

    if (request.destination.sizeBytes < request.prebuild.resultDataMaxSizeBytes)
    {
        return std::unexpected(ContractError::DestinationTooSmall);
    }

    auto const requiredScratchBytes =
        isUpdate ? request.prebuild.updateScratchDataSizeBytes : request.prebuild.scratchDataSizeBytes;

    if (request.scratch.address == 0U)
    {
        return std::unexpected(ContractError::ScratchAddressNull);
    }

    if (!IsAligned(request.scratch.address, kAccelerationStructureAlignmentBytes))
    {
        return std::unexpected(ContractError::ScratchAddressMisaligned);
    }

    if (request.scratch.sizeBytes < requiredScratchBytes)
    {
        return std::unexpected(ContractError::ScratchTooSmall);
    }

    bool inPlaceUpdate = false;
    if (isUpdate)
    {
        if (request.source.address == 0U)
        {
            return std::unexpected(ContractError::SourceRequiredForUpdate);
        }

        if (!IsAligned(request.source.address, kAccelerationStructureAlignmentBytes))
        {
            return std::unexpected(ContractError::SourceAddressMisaligned);
        }

        if (request.source.sizeBytes < request.prebuild.resultDataMaxSizeBytes)
        {
            return std::unexpected(ContractError::SourceTooSmall);
        }

        // An update refits the existing hierarchy. DXR's update constraints allow some changes this rule forbids;
        // requiring the element count to match exactly is *this chapter's* conservative tightening, chosen so that
        // the lab's evidence about a refit means one thing. It is not a D3D12 rule.
        if (request.sourceElementCount != request.elementCount)
        {
            return std::unexpected(ContractError::UpdateTopologyMismatch);
        }

        inPlaceUpdate = request.source.address == request.destination.address;
        if (inPlaceUpdate)
        {
            // An in-place update is legal only when source and destination are the very same range. Two ranges that
            // share a start address and disagree about their size are two different views of one allocation, which
            // is an aliasing bug wearing an in-place update's clothes.
            if (request.source.sizeBytes != request.destination.sizeBytes)
            {
                return std::unexpected(ContractError::OverlappingBuildRanges);
            }
        }
        else if (RangesOverlap(request.source, request.destination))
        {
            return std::unexpected(ContractError::OverlappingBuildRanges);
        }

        if (RangesOverlap(request.scratch, request.source))
        {
            return std::unexpected(ContractError::OverlappingBuildRanges);
        }
    }
    else if (request.source.address != 0U || request.source.sizeBytes != 0U)
    {
        return std::unexpected(ContractError::UnexpectedSourceForBuild);
    }

    if (RangesOverlap(request.scratch, request.destination))
    {
        return std::unexpected(ContractError::OverlappingBuildRanges);
    }

    auto const destinationEnd = CheckedAdd(request.destination.address, request.prebuild.resultDataMaxSizeBytes);
    if (!destinationEnd.has_value())
    {
        return std::unexpected(destinationEnd.error());
    }

    AccelerationStructureBuildPlan plan{};
    plan.requiredResultBytes = request.prebuild.resultDataMaxSizeBytes;
    plan.requiredScratchBytes = requiredScratchBytes;
    plan.inPlaceUpdate = inPlaceUpdate;
    plan.updateAllowed = HasFlag(request.flags, BuildFlags::AllowUpdate);
    plan.finalUpdate = isUpdate && !HasFlag(request.flags, BuildFlags::AllowUpdate);
    plan.compactionAllowed = HasFlag(request.flags, BuildFlags::AllowCompaction);
    plan.destinationEndAddress = *destinationEnd;
    return plan;
}

// -------------------------------------------------------------------------------------------------------------
// C. Producer/consumer ordering evidence
// -------------------------------------------------------------------------------------------------------------

namespace
{

// What the timeline knows about one resource identifier. `dirty` means "written since the last barrier that covered
// it", and `readSinceBarrier` means "read since the last barrier that covered it". The two together are exactly the
// state a UAV hazard needs: write-after-write and read-after-write both look at `dirty`, and write-after-read looks
// at `readSinceBarrier`.
struct TimelineResource final
{
    std::uint64_t id{};
    bool usedAsAccelerationStructure{};
    bool usedAsScratch{};
    bool built{};
    AccelerationStructureKind kind{AccelerationStructureKind::BottomLevel};
    bool dirty{};
    bool readSinceBarrier{};
};

TimelineResource &FindOrCreateResource(std::vector<TimelineResource> &resources, std::uint64_t id)
{
    for (auto &resource : resources)
    {
        if (resource.id == id)
        {
            return resource;
        }
    }

    resources.push_back(TimelineResource{.id = id});
    return resources.back();
}

} // namespace

std::expected<BuildTimelineValidation, ContractError> ValidateBuildTimeline(std::span<BuildStep const> steps)
{
    if (steps.empty())
    {
        return std::unexpected(ContractError::EmptyTimeline);
    }

    std::vector<TimelineResource> resources{};
    BuildTimelineValidation validation{};

    for (auto const &step : steps)
    {
        bool const isBuild = step.kind == BuildStepKind::BuildBottomLevel || step.kind == BuildStepKind::BuildTopLevel;

        if (step.kind != BuildStepKind::DispatchRays && step.resourceId == 0U)
        {
            return std::unexpected(ContractError::InvalidResourceIdentifier);
        }

        if (isBuild && step.scratchId == 0U)
        {
            return std::unexpected(ContractError::InvalidResourceIdentifier);
        }

        if (isBuild && step.scratchId == step.resourceId)
        {
            return std::unexpected(ContractError::InvalidResourceIdentifier);
        }

        if (std::ranges::any_of(step.inputs, [](std::uint64_t input) { return input == 0U; }))
        {
            return std::unexpected(ContractError::InvalidResourceIdentifier);
        }

        switch (step.kind)
        {
        case BuildStepKind::BuildBottomLevel:
        case BuildStepKind::BuildTopLevel:
        {
            auto const kind = step.kind == BuildStepKind::BuildBottomLevel ? AccelerationStructureKind::BottomLevel
                                                                           : AccelerationStructureKind::TopLevel;

            // A bottom-level build has no acceleration-structure inputs at all; naming one is a description that
            // has confused the two levels, and DXR has no second level of instancing to make it mean anything.
            if (kind == AccelerationStructureKind::BottomLevel && !step.inputs.empty())
            {
                return std::unexpected(ContractError::WrongAccelerationStructureKindConsumed);
            }

            for (auto const input : step.inputs)
            {
                auto &inputResource = FindOrCreateResource(resources, input);
                if (!inputResource.built)
                {
                    return std::unexpected(ContractError::ConsumesUnbuiltAccelerationStructure);
                }

                if (inputResource.kind != AccelerationStructureKind::BottomLevel)
                {
                    return std::unexpected(ContractError::WrongAccelerationStructureKindConsumed);
                }

                if (inputResource.dirty)
                {
                    return std::unexpected(ContractError::MissingProducerBarrier);
                }

                inputResource.readSinceBarrier = true;
            }

            {
                auto &scratch = FindOrCreateResource(resources, step.scratchId);
                if (scratch.usedAsAccelerationStructure)
                {
                    return std::unexpected(ContractError::InvalidResourceIdentifier);
                }

                // Two builds sharing one scratch buffer are two writers of the same memory. Without a barrier
                // between them the second build's scratch traffic races the first build's, and the symptom is a
                // structure that is intermittently, subtly wrong rather than obviously broken.
                if (scratch.dirty)
                {
                    return std::unexpected(ContractError::MissingScratchBarrier);
                }

                scratch.usedAsScratch = true;
                scratch.dirty = true;
            }

            auto &destination = FindOrCreateResource(resources, step.resourceId);
            if (destination.usedAsScratch)
            {
                return std::unexpected(ContractError::InvalidResourceIdentifier);
            }

            if (destination.built && destination.kind != kind)
            {
                return std::unexpected(ContractError::WrongAccelerationStructureKindConsumed);
            }

            // Rebuilding over a structure that something has read since the last barrier is a write-after-read
            // hazard: the reader may still be in flight.
            if (destination.readSinceBarrier)
            {
                return std::unexpected(ContractError::RebuildWithoutConsumerBarrier);
            }

            destination.usedAsAccelerationStructure = true;
            destination.built = true;
            destination.kind = kind;
            destination.dirty = true;

            if (kind == AccelerationStructureKind::BottomLevel)
            {
                ++validation.bottomLevelBuildCount;
            }
            else
            {
                ++validation.topLevelBuildCount;
            }

            break;
        }
        case BuildStepKind::UavBarrier:
        {
            auto &resource = FindOrCreateResource(resources, step.resourceId);
            ++validation.barrierCount;
            if (!resource.dirty)
            {
                ++validation.redundantBarrierCount;
            }

            resource.dirty = false;
            resource.readSinceBarrier = false;
            break;
        }
        case BuildStepKind::DispatchRays:
        {
            if (step.inputs.empty())
            {
                return std::unexpected(ContractError::DispatchWithoutTopLevel);
            }

            bool sawTopLevel = false;
            for (auto const input : step.inputs)
            {
                auto &inputResource = FindOrCreateResource(resources, input);
                if (!inputResource.built)
                {
                    return std::unexpected(ContractError::ConsumesUnbuiltAccelerationStructure);
                }

                if (inputResource.kind != AccelerationStructureKind::TopLevel)
                {
                    return std::unexpected(ContractError::WrongAccelerationStructureKindConsumed);
                }

                if (inputResource.dirty)
                {
                    return std::unexpected(ContractError::MissingProducerBarrier);
                }

                inputResource.readSinceBarrier = true;
                sawTopLevel = true;
            }

            if (!sawTopLevel)
            {
                return std::unexpected(ContractError::DispatchWithoutTopLevel);
            }

            ++validation.dispatchCount;
            break;
        }
        }
    }

    return validation;
}

// -------------------------------------------------------------------------------------------------------------
// D. Instances and top-level packing
// -------------------------------------------------------------------------------------------------------------

namespace
{

// The field checks shared by packing and validation. Packing permits a zero mask because the packed word has a
// perfectly well defined encoding for it; validation refuses it, because an instance no ray can see is a scene
// authoring mistake and not a rendering technique.
std::expected<void, ContractError> ValidateInstanceFields(std::uint32_t instanceId, std::uint32_t instanceMask,
                                                          std::uint32_t contribution, InstanceFlags flags,
                                                          bool requireVisibleMask) noexcept
{
    if (instanceId > kMaximumInstanceId)
    {
        return std::unexpected(ContractError::InstanceIdOutOfRange);
    }

    if (instanceMask > kMaximumInstanceMask)
    {
        return std::unexpected(ContractError::InstanceMaskOutOfRange);
    }

    if (requireVisibleMask && instanceMask == 0U)
    {
        return std::unexpected(ContractError::InstanceMaskSelectsNothing);
    }

    if (contribution > kMaximumInstanceContribution)
    {
        return std::unexpected(ContractError::InstanceContributionOutOfRange);
    }

    if (HasUndefinedBits(flags, kDefinedInstanceFlags))
    {
        return std::unexpected(ContractError::ConflictingInstanceFlags);
    }

    if (HasFlag(flags, InstanceFlags::ForceOpaque) && HasFlag(flags, InstanceFlags::ForceNonOpaque))
    {
        return std::unexpected(ContractError::ConflictingInstanceFlags);
    }

    return {};
}

std::expected<void, ContractError> ValidateInstanceTransform(Transform3x4 const &transform) noexcept
{
    if (!IsFiniteTransform(transform))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    if (std::abs(TransformDeterminant(transform)) < kMinimumInstanceTransformDeterminant)
    {
        return std::unexpected(ContractError::SingularInstanceTransform);
    }

    return {};
}

} // namespace

std::expected<PackedInstanceHeader, ContractError> PackInstanceHeader(InstanceDescription const &instance) noexcept
{
    auto const fields = ValidateInstanceFields(instance.instanceId, instance.instanceMask,
                                               instance.instanceContributionToHitGroupIndex, instance.flags, false);
    if (!fields.has_value())
    {
        return std::unexpected(fields.error());
    }

    PackedInstanceHeader packed{};
    packed.instanceIdAndMask = instance.instanceId | (instance.instanceMask << kInstanceIdBitCount);
    packed.instanceContributionAndFlags = instance.instanceContributionToHitGroupIndex |
                                          (static_cast<std::uint32_t>(instance.flags) << kInstanceContributionBitCount);
    return packed;
}

UnpackedInstanceHeader UnpackInstanceHeader(PackedInstanceHeader const &packed) noexcept
{
    UnpackedInstanceHeader unpacked{};
    unpacked.instanceId = packed.instanceIdAndMask & kMaximumInstanceId;
    unpacked.instanceMask = (packed.instanceIdAndMask >> kInstanceIdBitCount) & kMaximumInstanceMask;
    unpacked.instanceContributionToHitGroupIndex = packed.instanceContributionAndFlags & kMaximumInstanceContribution;
    unpacked.flags = static_cast<InstanceFlags>((packed.instanceContributionAndFlags >> kInstanceContributionBitCount) &
                                                kMaximumInstanceMask);
    return unpacked;
}

std::expected<void, ContractError> ValidateInstance(InstanceDescription const &instance) noexcept
{
    auto const fields = ValidateInstanceFields(instance.instanceId, instance.instanceMask,
                                               instance.instanceContributionToHitGroupIndex, instance.flags, true);
    if (!fields.has_value())
    {
        return std::unexpected(fields.error());
    }

    if (instance.bottomLevelAddress == 0U)
    {
        return std::unexpected(ContractError::BottomLevelAddressNull);
    }

    if (!IsAligned(instance.bottomLevelAddress, kAccelerationStructureAlignmentBytes))
    {
        return std::unexpected(ContractError::BottomLevelAddressMisaligned);
    }

    return ValidateInstanceTransform(instance.objectToWorld);
}

std::expected<TopLevelPacking, ContractError> ValidateInstances(std::span<InstanceDescription const> instances)
{
    if (instances.empty())
    {
        return std::unexpected(ContractError::EmptyInstanceSet);
    }

    if (instances.size() > kMaximumInstancesPerTopLevel)
    {
        return std::unexpected(ContractError::InstanceCountExceeded);
    }

    std::vector<std::uint32_t> seenIds{};
    seenIds.reserve(instances.size());
    std::vector<std::uint64_t> distinctAddresses{};

    TopLevelPacking packing{};
    packing.instanceCount = static_cast<std::uint32_t>(instances.size());

    for (auto const &instance : instances)
    {
        auto const validation = ValidateInstance(instance);
        if (!validation.has_value())
        {
            return std::unexpected(validation.error());
        }

        if (std::ranges::find(seenIds, instance.instanceId) != seenIds.end())
        {
            return std::unexpected(ContractError::DuplicateInstanceIdentity);
        }

        seenIds.push_back(instance.instanceId);

        if (std::ranges::find(distinctAddresses, instance.bottomLevelAddress) == distinctAddresses.end())
        {
            distinctAddresses.push_back(instance.bottomLevelAddress);
        }

        packing.maskUnion |= instance.instanceMask;
        packing.maximumInstanceContribution =
            std::max(packing.maximumInstanceContribution, instance.instanceContributionToHitGroupIndex);
    }

    auto const bufferSize =
        CheckedMultiply(static_cast<std::uint64_t>(packing.instanceCount), kInstanceDescriptionSizeBytes);
    if (!bufferSize.has_value())
    {
        return std::unexpected(bufferSize.error());
    }

    packing.instanceBufferSizeBytes = *bufferSize;
    packing.distinctBottomLevelCount = static_cast<std::uint32_t>(distinctAddresses.size());
    return packing;
}

std::expected<void, ContractError> ValidateInstanceBuffer(BufferRange const &buffer,
                                                          std::uint32_t instanceCount) noexcept
{
    if (instanceCount == 0U)
    {
        return std::unexpected(ContractError::EmptyInstanceSet);
    }

    if (static_cast<std::uint64_t>(instanceCount) > kMaximumInstancesPerTopLevel)
    {
        return std::unexpected(ContractError::InstanceCountExceeded);
    }

    if (buffer.address == 0U)
    {
        return std::unexpected(ContractError::InstanceBufferAddressNull);
    }

    if (!IsAligned(buffer.address, kInstanceDescriptionAlignmentBytes))
    {
        return std::unexpected(ContractError::InstanceBufferAddressMisaligned);
    }

    auto const requiredBytes =
        CheckedMultiply(static_cast<std::uint64_t>(instanceCount), kInstanceDescriptionSizeBytes);
    if (!requiredBytes.has_value())
    {
        return std::unexpected(requiredBytes.error());
    }

    if (buffer.sizeBytes < *requiredBytes)
    {
        return std::unexpected(ContractError::InstanceBufferTooSmall);
    }

    return {};
}

// -------------------------------------------------------------------------------------------------------------
// E. Rays
// -------------------------------------------------------------------------------------------------------------

std::expected<RayValidation, ContractError> ValidateRay(RayDescription const &ray) noexcept
{
    // An infinite `tMax` lands here: it is not finite, so it is refused by name rather than being replaced by a
    // large number that would silently change what the interval means.
    if (!IsFiniteVector(ray.origin) || !IsFiniteVector(ray.direction) || !IsFiniteValue(ray.tMinMetres) ||
        !IsFiniteValue(ray.tMaxMetres))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    auto const directionLength = Length(ray.direction);
    if (directionLength < kMinimumRayDirectionLength)
    {
        return std::unexpected(ContractError::ZeroLengthRayDirection);
    }

    if (ray.tMinMetres < 0.0)
    {
        return std::unexpected(ContractError::NegativeRayTMin);
    }

    // "Ray TMin must be nonnegative and <= TMax." An equal pair is therefore legal, and only an inverted interval
    // is refused. The equal case is reported through `intervalIsEmpty` and misses every triangle downstream.
    if (ray.tMaxMetres < ray.tMinMetres)
    {
        return std::unexpected(ContractError::InvertedRayInterval);
    }

    if (ray.tMaxMetres > kMaximumRayDistanceMetres)
    {
        return std::unexpected(ContractError::RayIntervalTooLarge);
    }

    RayValidation validation{};
    validation.directionLength = directionLength;
    validation.intervalLengthMetres = (ray.tMaxMetres - ray.tMinMetres) * directionLength;
    validation.directionIsUnitLength = std::abs(directionLength - 1.0) <= kUnitLengthTolerance;
    validation.intervalIsEmpty = ray.tMaxMetres == ray.tMinMetres;
    return validation;
}

std::expected<void, ContractError> ValidateRayFlags(RayFlags flags) noexcept
{
    if (HasUndefinedBits(flags, kDefinedRayFlags))
    {
        return std::unexpected(ContractError::ConflictingRayFlags);
    }

    // DXR declares `RAY_FLAG_FORCE_OPAQUE` and `RAY_FLAG_FORCE_NON_OPAQUE` each mutually exclusive with the other
    // *and* with both cull-by-opacity flags, and the two cull-by-opacity flags mutually exclusive with each other.
    // That is one rule, not four: at most one flag from the whole opacity group may be set. An earlier reading of
    // this contract accepted the cross pairs and called them "guaranteed misses"; they are not misses, they are
    // invalid arguments, and D3D12's own flag documentation says so.
    bool const forcesOpacity = HasAnyFlag(flags, RayFlags::ForceOpaque | RayFlags::ForceNonOpaque);
    bool const cullsByOpacity = HasAnyFlag(flags, RayFlags::CullOpaque | RayFlags::CullNonOpaque);
    bool const opacityConflict = (HasFlag(flags, RayFlags::ForceOpaque) && HasFlag(flags, RayFlags::ForceNonOpaque)) ||
                                 (HasFlag(flags, RayFlags::CullOpaque) && HasFlag(flags, RayFlags::CullNonOpaque)) ||
                                 (forcesOpacity && cullsByOpacity);

    bool const faceConflict =
        HasFlag(flags, RayFlags::CullBackFacingTriangles) && HasFlag(flags, RayFlags::CullFrontFacingTriangles);

    // `RAY_FLAG_SKIP_TRIANGLES` is mutually exclusive with `CULL_FRONT_FACING_TRIANGLES`,
    // `CULL_BACK_FACING_TRIANGLES`, and `SKIP_PROCEDURAL_PRIMITIVES`. Skipping every triangle and then asking which
    // triangles to cull is a contradiction the flag's own documentation forbids.
    bool const skipConflict = HasFlag(flags, RayFlags::SkipTriangles) &&
                              HasAnyFlag(flags, RayFlags::SkipProceduralPrimitives | RayFlags::CullBackFacingTriangles |
                                                    RayFlags::CullFrontFacingTriangles);

    if (opacityConflict || faceConflict || skipConflict)
    {
        return std::unexpected(ContractError::ConflictingRayFlags);
    }

    return {};
}

std::expected<void, ContractError> ValidateInstanceInclusionMask(std::uint32_t mask) noexcept
{
    if (mask > kMaximumInstanceMask)
    {
        return std::unexpected(ContractError::RayMaskOutOfRange);
    }

    if (mask == 0U)
    {
        return std::unexpected(ContractError::RayMaskSelectsNothing);
    }

    return {};
}

// -------------------------------------------------------------------------------------------------------------
// F. State object consistency
// -------------------------------------------------------------------------------------------------------------

namespace
{

ShaderExport const *FindExport(StateObjectDescription const &description, std::string_view name) noexcept
{
    for (auto const &shaderExport : description.exports)
    {
        if (shaderExport.name == name)
        {
            return &shaderExport;
        }
    }

    return nullptr;
}

bool IsHitGroupName(StateObjectDescription const &description, std::string_view name) noexcept
{
    return std::ranges::any_of(description.hitGroups,
                               [name](HitGroupDescription const &group) { return group.name == name; });
}

// Checks that a hit group's reference to a shader export exists and has the stage the slot requires. An empty name
// means the slot is unused, which the shape rules above have already accepted or rejected.
std::expected<void, ContractError> ValidateHitGroupMember(StateObjectDescription const &description,
                                                          std::string const &exportName, ShaderStage requiredStage)
{
    if (exportName.empty())
    {
        return {};
    }

    auto const *const shaderExport = FindExport(description, exportName);
    if (shaderExport == nullptr)
    {
        return std::unexpected(ContractError::HitGroupReferencesUnknownExport);
    }

    if (shaderExport->stage != requiredStage)
    {
        return std::unexpected(ContractError::HitGroupExportStageMismatch);
    }

    return {};
}

struct ExportAssociationEntry final
{
    std::string name{};
    std::string signatureName{};
    std::uint32_t rootArgumentSizeBytes{};
};

ExportAssociationEntry const *FindAssociation(std::vector<ExportAssociationEntry> const &associations,
                                              std::string const &name) noexcept
{
    if (name.empty())
    {
        return nullptr;
    }

    for (auto const &association : associations)
    {
        if (association.name == name)
        {
            return &association;
        }
    }

    return nullptr;
}

std::uint32_t AssociatedArgumentSize(std::vector<ExportAssociationEntry> const &associations, std::string const &name)
{
    auto const *const association = FindAssociation(associations, name);
    return association == nullptr ? 0U : association->rootArgumentSizeBytes;
}

// Resolves the one local root signature a hit group's record is laid out for, applying DXR's matching rule. The
// group's own association wins and unassociated members inherit it; with no group association every member must
// agree, and "no association" is a value members have to agree on too.
std::expected<std::uint32_t, ContractError> ResolveHitGroupArgumentSize(
    std::vector<ExportAssociationEntry> const &associations, HitGroupDescription const &hitGroup)
{
    std::array<std::string const *, 3U> const members{&hitGroup.closestHitExport, &hitGroup.anyHitExport,
                                                      &hitGroup.intersectionExport};

    auto const *const groupAssociation = FindAssociation(associations, hitGroup.name);
    if (groupAssociation != nullptr)
    {
        for (auto const *const member : members)
        {
            if (member->empty())
            {
                continue;
            }

            auto const *const memberAssociation = FindAssociation(associations, *member);
            if (memberAssociation != nullptr && memberAssociation->signatureName != groupAssociation->signatureName)
            {
                return std::unexpected(ContractError::MismatchedHitGroupLocalRootSignature);
            }
        }

        return groupAssociation->rootArgumentSizeBytes;
    }

    bool resolved = false;
    ExportAssociationEntry const *agreed = nullptr;
    for (auto const *const member : members)
    {
        if (member->empty())
        {
            continue;
        }

        auto const *const memberAssociation = FindAssociation(associations, *member);
        if (!resolved)
        {
            agreed = memberAssociation;
            resolved = true;
            continue;
        }

        bool const bothAbsent = agreed == nullptr && memberAssociation == nullptr;
        bool const bothPresentAndEqual = agreed != nullptr && memberAssociation != nullptr &&
                                         agreed->signatureName == memberAssociation->signatureName;
        if (!bothAbsent && !bothPresentAndEqual)
        {
            return std::unexpected(ContractError::MismatchedHitGroupLocalRootSignature);
        }
    }

    return agreed == nullptr ? 0U : agreed->rootArgumentSizeBytes;
}

std::expected<ShaderTableEntryRequirement, ContractError> MakeEntryRequirement(std::string const &name,
                                                                               ShaderTableSectionKind section,
                                                                               std::uint32_t argumentSizeBytes)
{
    auto const stride = AlignUp(kShaderIdentifierSizeBytes + static_cast<std::uint64_t>(argumentSizeBytes),
                                kShaderRecordAlignmentBytes);
    if (!stride.has_value())
    {
        return std::unexpected(stride.error());
    }

    if (*stride > kMaximumShaderRecordStrideBytes)
    {
        return std::unexpected(ContractError::ShaderRecordStrideExceeded);
    }

    ShaderTableEntryRequirement requirement{};
    requirement.name = name;
    requirement.section = section;
    requirement.localRootArgumentSizeBytes = argumentSizeBytes;
    requirement.minimumRecordStrideBytes = *stride;
    return requirement;
}

} // namespace

std::expected<StateObjectValidation, ContractError> ValidateStateObject(StateObjectDescription const &description)
{
    // The shader config is checked first because every record in the table inherits its consequences, and because a
    // payload size that is not a multiple of four is a description no compiler will accept later either.
    if (description.shaderConfig.maxPayloadSizeBytes == 0U ||
        description.shaderConfig.maxPayloadSizeBytes > kMaximumPayloadSizeBytes ||
        (description.shaderConfig.maxPayloadSizeBytes % 4U) != 0U)
    {
        return std::unexpected(ContractError::InvalidPayloadSize);
    }

    // A triangle hit group cannot declare fewer attribute bytes than the fixed-function intersector already writes,
    // and no hit group may declare more than D3D12 allows an intersection shader to pass on.
    if (description.shaderConfig.maxAttributeSizeBytes < kTriangleAttributeSizeBytes ||
        description.shaderConfig.maxAttributeSizeBytes > kMaximumAttributeSizeBytes ||
        (description.shaderConfig.maxAttributeSizeBytes % 4U) != 0U)
    {
        return std::unexpected(ContractError::InvalidAttributeSize);
    }

    if (description.pipelineConfig.maxTraceRecursionDepth == 0U ||
        description.pipelineConfig.maxTraceRecursionDepth > kMaximumTraceRecursionDepth)
    {
        return std::unexpected(ContractError::InvalidTraceRecursionDepth);
    }

    std::vector<std::string> usedNames{};
    usedNames.reserve(description.exports.size() + description.hitGroups.size());

    for (auto const &shaderExport : description.exports)
    {
        if (shaderExport.name.empty())
        {
            return std::unexpected(ContractError::EmptyExportName);
        }

        if (std::ranges::find(usedNames, shaderExport.name) != usedNames.end())
        {
            return std::unexpected(ContractError::DuplicateExportName);
        }

        usedNames.push_back(shaderExport.name);
    }

    for (auto const &hitGroup : description.hitGroups)
    {
        if (hitGroup.name.empty())
        {
            return std::unexpected(ContractError::EmptyExportName);
        }

        // A hit group shares one name space with the shader exports, because both are things a shader table record
        // and a local root signature association can name.
        if (std::ranges::find(usedNames, hitGroup.name) != usedNames.end())
        {
            return std::unexpected(ContractError::DuplicateExportName);
        }

        usedNames.push_back(hitGroup.name);

        if (hitGroup.closestHitExport.empty() && hitGroup.anyHitExport.empty())
        {
            return std::unexpected(ContractError::HitGroupMissingShaders);
        }

        if (hitGroup.type == HitGroupType::Triangles)
        {
            if (!hitGroup.intersectionExport.empty())
            {
                return std::unexpected(ContractError::TriangleHitGroupDeclaresIntersection);
            }
        }
        else if (hitGroup.intersectionExport.empty())
        {
            return std::unexpected(ContractError::ProceduralHitGroupMissingIntersection);
        }

        auto const closestHit = ValidateHitGroupMember(description, hitGroup.closestHitExport, ShaderStage::ClosestHit);
        if (!closestHit.has_value())
        {
            return std::unexpected(closestHit.error());
        }

        auto const anyHit = ValidateHitGroupMember(description, hitGroup.anyHitExport, ShaderStage::AnyHit);
        if (!anyHit.has_value())
        {
            return std::unexpected(anyHit.error());
        }

        auto const intersection =
            ValidateHitGroupMember(description, hitGroup.intersectionExport, ShaderStage::Intersection);
        if (!intersection.has_value())
        {
            return std::unexpected(intersection.error());
        }
    }

    std::vector<std::string> signatureNames{};
    signatureNames.reserve(description.localRootSignatures.size());
    for (auto const &signature : description.localRootSignatures)
    {
        if (signature.name.empty())
        {
            return std::unexpected(ContractError::EmptyExportName);
        }

        if (std::ranges::find(signatureNames, signature.name) != signatureNames.end())
        {
            return std::unexpected(ContractError::DuplicateLocalRootSignatureName);
        }

        // Root arguments are 32-bit words in a shader record, so a size that is not a multiple of four describes a
        // record no shader can read the far end of.
        if ((signature.rootArgumentSizeBytes % 4U) != 0U)
        {
            return std::unexpected(ContractError::InvalidLocalRootArgumentSize);
        }

        signatureNames.push_back(signature.name);
    }

    std::vector<ExportAssociationEntry> associations{};
    for (auto const &association : description.associations)
    {
        auto const signature =
            std::ranges::find_if(description.localRootSignatures, [&association](LocalRootSignature const &candidate)
                                 { return candidate.name == association.localRootSignatureName; });
        if (signature == description.localRootSignatures.end())
        {
            return std::unexpected(ContractError::UnknownLocalRootSignature);
        }

        // D3D12 treats an association with no exports as a default association covering everything. This chapter
        // requires the exports to be named, because an implicit association is exactly the thing that makes a
        // record stride surprising.
        if (association.exportNames.empty())
        {
            return std::unexpected(ContractError::AssociationReferencesUnknownExport);
        }

        for (auto const &exportName : association.exportNames)
        {
            if (exportName.empty())
            {
                return std::unexpected(ContractError::EmptyExportName);
            }

            if (FindExport(description, exportName) == nullptr && !IsHitGroupName(description, exportName))
            {
                return std::unexpected(ContractError::AssociationReferencesUnknownExport);
            }

            bool const alreadyAssociated = std::ranges::any_of(
                associations, [&exportName](ExportAssociationEntry const &entry) { return entry.name == exportName; });
            if (alreadyAssociated)
            {
                return std::unexpected(ContractError::ConflictingLocalRootSignatureAssociation);
            }

            associations.push_back(ExportAssociationEntry{.name = exportName,
                                                          .signatureName = signature->name,
                                                          .rootArgumentSizeBytes = signature->rootArgumentSizeBytes});
        }
    }

    StateObjectValidation validation{};
    validation.shaderConfig = description.shaderConfig;
    validation.pipelineConfig = description.pipelineConfig;

    // Entries are emitted grouped by section kind, in declaration order within each group, because that is the
    // order the shader table is laid out in and because a stable order is what lets a test compare whole tables.
    auto const appendExportEntries = [&](ShaderStage stage, ShaderTableSectionKind section,
                                         std::uint32_t &counter) -> std::expected<void, ContractError>
    {
        for (auto const &shaderExport : description.exports)
        {
            if (shaderExport.stage != stage)
            {
                continue;
            }

            auto entry = MakeEntryRequirement(shaderExport.name, section,
                                              AssociatedArgumentSize(associations, shaderExport.name));
            if (!entry.has_value())
            {
                return std::unexpected(entry.error());
            }

            validation.entries.push_back(std::move(*entry));
            ++counter;
        }

        return {};
    };

    if (auto const rayGeneration = appendExportEntries(
            ShaderStage::RayGeneration, ShaderTableSectionKind::RayGeneration, validation.rayGenerationCount);
        !rayGeneration.has_value())
    {
        return std::unexpected(rayGeneration.error());
    }

    if (auto const miss = appendExportEntries(ShaderStage::Miss, ShaderTableSectionKind::Miss, validation.missCount);
        !miss.has_value())
    {
        return std::unexpected(miss.error());
    }

    for (auto const &hitGroup : description.hitGroups)
    {
        // A hit-group record holds one set of local root arguments shared by every shader in the group, so the
        // group has to resolve to exactly one local root signature. Taking the widest of several would invent a
        // record layout no driver agrees with.
        auto const argumentSizeBytes = ResolveHitGroupArgumentSize(associations, hitGroup);
        if (!argumentSizeBytes.has_value())
        {
            return std::unexpected(argumentSizeBytes.error());
        }

        auto entry = MakeEntryRequirement(hitGroup.name, ShaderTableSectionKind::HitGroup, *argumentSizeBytes);
        if (!entry.has_value())
        {
            return std::unexpected(entry.error());
        }

        validation.entries.push_back(std::move(*entry));
        ++validation.hitGroupCount;
    }

    if (auto const callable =
            appendExportEntries(ShaderStage::Callable, ShaderTableSectionKind::Callable, validation.callableCount);
        !callable.has_value())
    {
        return std::unexpected(callable.error());
    }

    if (validation.rayGenerationCount == 0U)
    {
        return std::unexpected(ContractError::MissingRayGenerationShader);
    }

    // D3D12 permits a pipeline with no miss shader; a ray that misses then simply returns. The chapter requires one
    // so that a miss is observable in the lab's output instead of being indistinguishable from an unwritten pixel.
    if (validation.missCount == 0U)
    {
        return std::unexpected(ContractError::MissingMissShader);
    }

    for (auto const &entry : validation.entries)
    {
        validation.maximumLocalRootArgumentSizeBytes =
            std::max(validation.maximumLocalRootArgumentSizeBytes, entry.localRootArgumentSizeBytes);
    }

    return validation;
}

// -------------------------------------------------------------------------------------------------------------
// G. Payload ABI
// -------------------------------------------------------------------------------------------------------------

std::expected<PayloadLayout, ContractError> ValidatePayloadLayout(std::span<PayloadFieldDescription const> fields,
                                                                  std::uint32_t maximumPayloadSizeBytes)
{
    if (maximumPayloadSizeBytes == 0U || maximumPayloadSizeBytes > kMaximumPayloadSizeBytes ||
        (maximumPayloadSizeBytes % 4U) != 0U)
    {
        return std::unexpected(ContractError::InvalidPayloadSize);
    }

    if (fields.empty())
    {
        return std::unexpected(ContractError::EmptyPayloadLayout);
    }

    PayloadLayout layout{};
    layout.fields.reserve(fields.size());

    std::uint64_t cursorBytes = 0U;
    std::uint64_t usedBytes = 0U;
    std::uint32_t structureAlignmentBytes = 1U;

    for (auto const &field : fields)
    {
        if (field.name.empty())
        {
            return std::unexpected(ContractError::EmptyPayloadFieldName);
        }

        bool const duplicate = std::ranges::any_of(layout.fields, [&field](PayloadFieldLayout const &placed)
                                                   { return placed.name == field.name; });
        if (duplicate)
        {
            return std::unexpected(ContractError::DuplicatePayloadFieldName);
        }

        if (field.scalarSizeBytes == 0U || field.scalarSizeBytes > kMaximumPayloadFieldAlignmentBytes ||
            (field.scalarSizeBytes & (field.scalarSizeBytes - 1U)) != 0U)
        {
            return std::unexpected(ContractError::InvalidPayloadFieldSize);
        }

        if (field.scalarAlignmentBytes == 0U || field.scalarAlignmentBytes > kMaximumPayloadFieldAlignmentBytes ||
            (field.scalarAlignmentBytes & (field.scalarAlignmentBytes - 1U)) != 0U ||
            (field.scalarSizeBytes % field.scalarAlignmentBytes) != 0U)
        {
            return std::unexpected(ContractError::InvalidPayloadFieldAlignment);
        }

        if (field.elementCount == 0U)
        {
            return std::unexpected(ContractError::ZeroPayloadElementCount);
        }

        auto const fieldSizeBytes = CheckedMultiply(static_cast<std::uint64_t>(field.scalarSizeBytes),
                                                    static_cast<std::uint64_t>(field.elementCount));
        if (!fieldSizeBytes.has_value())
        {
            return std::unexpected(fieldSizeBytes.error());
        }

        auto const offsetBytes = AlignUp(cursorBytes, field.scalarAlignmentBytes);
        if (!offsetBytes.has_value())
        {
            return std::unexpected(offsetBytes.error());
        }

        auto const endBytes = CheckedAdd(*offsetBytes, *fieldSizeBytes);
        if (!endBytes.has_value())
        {
            return std::unexpected(endBytes.error());
        }

        if (*endBytes > kMaximumPayloadSizeBytes)
        {
            return std::unexpected(ContractError::PayloadSizeExceeded);
        }

        layout.fields.push_back(PayloadFieldLayout{.name = field.name,
                                                   .offsetBytes = static_cast<std::uint32_t>(*offsetBytes),
                                                   .sizeBytes = static_cast<std::uint32_t>(*fieldSizeBytes)});
        cursorBytes = *endBytes;
        usedBytes += *fieldSizeBytes;
        structureAlignmentBytes = std::max(structureAlignmentBytes, field.scalarAlignmentBytes);
    }

    auto const sizeBytes = AlignUp(cursorBytes, structureAlignmentBytes);
    if (!sizeBytes.has_value())
    {
        return std::unexpected(sizeBytes.error());
    }

    if (*sizeBytes > maximumPayloadSizeBytes)
    {
        return std::unexpected(ContractError::PayloadSizeExceeded);
    }

    layout.sizeBytes = static_cast<std::uint32_t>(*sizeBytes);
    layout.alignmentBytes = structureAlignmentBytes;
    layout.paddingBytes = static_cast<std::uint32_t>(*sizeBytes - usedBytes);
    return layout;
}

// -------------------------------------------------------------------------------------------------------------
// H. Shader identifiers and shader-table layout
// -------------------------------------------------------------------------------------------------------------

namespace
{

bool IsZeroIdentifier(ShaderIdentifier const &identifier) noexcept
{
    return std::ranges::all_of(identifier.bytes, [](std::byte value) { return value == std::byte{0}; });
}

std::expected<std::uint64_t, ContractError> RecordStrideForArguments(std::uint64_t argumentSizeBytes) noexcept
{
    if ((argumentSizeBytes % 4U) != 0U)
    {
        return std::unexpected(ContractError::InvalidLocalRootArgumentSize);
    }

    auto const used = CheckedAdd(kShaderIdentifierSizeBytes, argumentSizeBytes);
    if (!used.has_value())
    {
        return std::unexpected(used.error());
    }

    auto const stride = AlignUp(*used, kShaderRecordAlignmentBytes);
    if (!stride.has_value())
    {
        return std::unexpected(stride.error());
    }

    if (*stride > kMaximumShaderRecordStrideBytes)
    {
        return std::unexpected(ContractError::ShaderRecordStrideExceeded);
    }

    return *stride;
}

} // namespace

std::expected<ShaderIdentifier, ContractError> MakeShaderIdentifier(std::span<std::byte const> bytes)
{
    if (bytes.size() != static_cast<std::size_t>(kShaderIdentifierSizeBytes))
    {
        return std::unexpected(ContractError::InvalidShaderIdentifierSize);
    }

    ShaderIdentifier identifier{};
    std::ranges::copy(bytes, identifier.bytes.begin());

    // All-zero is what a caller gets from an uninitialized upload buffer and from asking a state object for an
    // export it does not have. Both produce a record the GPU jumps into and neither reports itself.
    if (IsZeroIdentifier(identifier))
    {
        return std::unexpected(ContractError::ZeroShaderIdentifier);
    }

    return identifier;
}

std::expected<std::uint64_t, ContractError> ComputeSectionSizeBytes(std::uint64_t recordCount,
                                                                    std::uint64_t strideBytes) noexcept
{
    if (recordCount == 0U)
    {
        return std::unexpected(ContractError::EmptyShaderTableSection);
    }

    if (strideBytes == 0U || !IsAligned(strideBytes, kShaderRecordAlignmentBytes))
    {
        return std::unexpected(ContractError::InvalidAlignment);
    }

    if (strideBytes > kMaximumShaderRecordStrideBytes)
    {
        return std::unexpected(ContractError::ShaderRecordStrideExceeded);
    }

    auto const sizeBytes = CheckedMultiply(recordCount, strideBytes);
    if (!sizeBytes.has_value())
    {
        return std::unexpected(sizeBytes.error());
    }

    if (*sizeBytes > kMaximumShaderTableSizeBytes)
    {
        return std::unexpected(ContractError::ShaderTableSizeExceeded);
    }

    return *sizeBytes;
}

std::expected<ShaderTableLayout, ContractError> BuildShaderTableLayout(
    std::span<ShaderTableSectionRequest const> sections)
{
    if (sections.empty())
    {
        return std::unexpected(ContractError::EmptyShaderTable);
    }

    ShaderTableLayout layout{};
    layout.sections.reserve(sections.size());

    std::vector<ShaderTableSectionKind> seenKinds{};
    std::uint64_t cursorBytes = 0U;
    std::uint64_t usedBytes = 0U;

    for (auto const &request : sections)
    {
        if (std::ranges::find(seenKinds, request.kind) != seenKinds.end())
        {
            return std::unexpected(ContractError::DuplicateShaderTableSection);
        }

        seenKinds.push_back(request.kind);

        if (request.records.empty())
        {
            return std::unexpected(ContractError::EmptyShaderTableSection);
        }

        // `DispatchRays` takes a single address for the ray generation shader and no stride, so a ray generation
        // section that holds anything other than exactly one record cannot be expressed at dispatch time.
        if (request.kind == ShaderTableSectionKind::RayGeneration && request.records.size() != 1U)
        {
            return std::unexpected(ContractError::RayGenerationSectionMustHoldOneRecord);
        }

        std::uint64_t strideBytes = 0U;
        for (auto const &record : request.records)
        {
            if (IsZeroIdentifier(record.identifier))
            {
                return std::unexpected(ContractError::ZeroShaderIdentifier);
            }

            auto const recordStride = RecordStrideForArguments(record.localRootArgumentSizeBytes);
            if (!recordStride.has_value())
            {
                return std::unexpected(recordStride.error());
            }

            strideBytes = std::max(strideBytes, *recordStride);
        }

        auto const sectionOffset = AlignUp(cursorBytes, kShaderTableAlignmentBytes);
        if (!sectionOffset.has_value())
        {
            return std::unexpected(sectionOffset.error());
        }

        auto const sectionSize =
            ComputeSectionSizeBytes(static_cast<std::uint64_t>(request.records.size()), strideBytes);
        if (!sectionSize.has_value())
        {
            return std::unexpected(sectionSize.error());
        }

        ShaderTableSectionLayout section{};
        section.kind = request.kind;
        section.offsetBytes = *sectionOffset;
        section.strideBytes = strideBytes;
        section.sizeBytes = *sectionSize;
        section.recordCount = static_cast<std::uint64_t>(request.records.size());
        section.leadingPaddingBytes = *sectionOffset - cursorBytes;
        section.records.reserve(request.records.size());

        for (std::size_t index = 0U; index < request.records.size(); ++index)
        {
            auto const &record = request.records[index];
            auto const recordOffsetInSection = CheckedMultiply(static_cast<std::uint64_t>(index), strideBytes);
            if (!recordOffsetInSection.has_value())
            {
                return std::unexpected(recordOffsetInSection.error());
            }

            auto const recordOffset = CheckedAdd(*sectionOffset, *recordOffsetInSection);
            if (!recordOffset.has_value())
            {
                return std::unexpected(recordOffset.error());
            }

            ShaderRecordLayout recordLayout{};
            recordLayout.offsetBytes = *recordOffset;
            recordLayout.localRootArgumentOffsetBytes = *recordOffset + kShaderIdentifierSizeBytes;
            recordLayout.localRootArgumentSizeBytes = static_cast<std::uint64_t>(record.localRootArgumentSizeBytes);
            recordLayout.paddingBytes =
                strideBytes - kShaderIdentifierSizeBytes - recordLayout.localRootArgumentSizeBytes;
            section.records.push_back(recordLayout);

            usedBytes += kShaderIdentifierSizeBytes + recordLayout.localRootArgumentSizeBytes;
        }

        auto const sectionEnd = CheckedAdd(*sectionOffset, *sectionSize);
        if (!sectionEnd.has_value())
        {
            return std::unexpected(sectionEnd.error());
        }

        cursorBytes = *sectionEnd;
        layout.sections.push_back(std::move(section));
    }

    // The table is rounded to the table alignment so that another table can be suballocated immediately after it
    // without the next section start having to move.
    auto const totalSize = AlignUp(cursorBytes, kShaderTableAlignmentBytes);
    if (!totalSize.has_value())
    {
        return std::unexpected(totalSize.error());
    }

    if (*totalSize > kMaximumShaderTableSizeBytes)
    {
        return std::unexpected(ContractError::ShaderTableSizeExceeded);
    }

    layout.totalSizeBytes = *totalSize;
    layout.paddingBytes = *totalSize - usedBytes;
    return layout;
}

std::expected<ShaderTableSectionLayout, ContractError> FindShaderTableSection(ShaderTableLayout const &layout,
                                                                              ShaderTableSectionKind kind)
{
    for (auto const &section : layout.sections)
    {
        if (section.kind == kind)
        {
            return section;
        }
    }

    return std::unexpected(ContractError::ShaderTableSectionMissing);
}

// -------------------------------------------------------------------------------------------------------------
// I. Dispatch description and trace-time record selection
// -------------------------------------------------------------------------------------------------------------

namespace
{

std::expected<void, ContractError> ValidateTableBaseAddress(std::uint64_t tableBaseAddress) noexcept
{
    if (tableBaseAddress == 0U)
    {
        return std::unexpected(ContractError::TableBaseAddressNull);
    }

    if (!IsAligned(tableBaseAddress, kShaderTableAlignmentBytes))
    {
        return std::unexpected(ContractError::TableBaseAddressMisaligned);
    }

    return {};
}

std::expected<ShaderTableRange, ContractError> MakeRange(ShaderTableLayout const &layout, std::uint64_t baseAddress,
                                                         ShaderTableSectionKind kind, bool includeStride)
{
    auto const section = FindShaderTableSection(layout, kind);
    if (!section.has_value())
    {
        // An absent section is the legal way to say "this pipeline has no callable shaders". It is expressed as an
        // all-zero range, which is what `D3D12_DISPATCH_RAYS_DESC` expects for an unused table.
        return ShaderTableRange{};
    }

    auto const startAddress = CheckedAdd(baseAddress, section->offsetBytes);
    if (!startAddress.has_value())
    {
        return std::unexpected(startAddress.error());
    }

    return ShaderTableRange{.startAddress = *startAddress,
                            .sizeBytes = section->sizeBytes,
                            .strideBytes = includeStride ? section->strideBytes : 0U};
}

} // namespace

std::expected<std::uint64_t, ContractError> ValidateDispatchDimensions(DispatchDimensions dimensions) noexcept
{
    if (dimensions.width == 0U || dimensions.height == 0U || dimensions.depth == 0U)
    {
        return std::unexpected(ContractError::ZeroDispatchDimension);
    }

    auto const widthTimesHeight =
        CheckedMultiply(static_cast<std::uint64_t>(dimensions.width), static_cast<std::uint64_t>(dimensions.height));
    if (!widthTimesHeight.has_value())
    {
        return std::unexpected(widthTimesHeight.error());
    }

    auto const threadCount = CheckedMultiply(*widthTimesHeight, static_cast<std::uint64_t>(dimensions.depth));
    if (!threadCount.has_value())
    {
        return std::unexpected(threadCount.error());
    }

    if (*threadCount > kMaximumRayGenerationThreads)
    {
        return std::unexpected(ContractError::DispatchThreadCountExceeded);
    }

    return *threadCount;
}

std::expected<DispatchRaysDescription, ContractError> MakeDispatchRaysDescription(ShaderTableLayout const &layout,
                                                                                  std::uint64_t tableBaseAddress,
                                                                                  DispatchDimensions dimensions)
{
    auto const baseValidation = ValidateTableBaseAddress(tableBaseAddress);
    if (!baseValidation.has_value())
    {
        return std::unexpected(baseValidation.error());
    }

    auto const threadCount = ValidateDispatchDimensions(dimensions);
    if (!threadCount.has_value())
    {
        return std::unexpected(threadCount.error());
    }

    // Every other section may be absent; the ray generation section may not, because a dispatch with no ray
    // generation shader has nothing to run.
    auto const rayGenerationSection = FindShaderTableSection(layout, ShaderTableSectionKind::RayGeneration);
    if (!rayGenerationSection.has_value())
    {
        return std::unexpected(rayGenerationSection.error());
    }

    auto const rayGeneration = MakeRange(layout, tableBaseAddress, ShaderTableSectionKind::RayGeneration, false);
    if (!rayGeneration.has_value())
    {
        return std::unexpected(rayGeneration.error());
    }

    auto const miss = MakeRange(layout, tableBaseAddress, ShaderTableSectionKind::Miss, true);
    if (!miss.has_value())
    {
        return std::unexpected(miss.error());
    }

    auto const hitGroup = MakeRange(layout, tableBaseAddress, ShaderTableSectionKind::HitGroup, true);
    if (!hitGroup.has_value())
    {
        return std::unexpected(hitGroup.error());
    }

    auto const callable = MakeRange(layout, tableBaseAddress, ShaderTableSectionKind::Callable, true);
    if (!callable.has_value())
    {
        return std::unexpected(callable.error());
    }

    DispatchRaysDescription description{};
    description.rayGeneration = *rayGeneration;
    description.miss = *miss;
    description.hitGroup = *hitGroup;
    description.callable = *callable;
    description.dimensions = dimensions;
    description.threadCount = *threadCount;
    return description;
}

std::expected<std::uint64_t, ContractError> ComputeHitGroupRecordIndex(
    HitGroupIndexParameters const &parameters) noexcept
{
    if (parameters.rayContributionToHitGroupIndex > kMaximumRayContributionToHitGroupIndex)
    {
        return std::unexpected(ContractError::RayContributionOutOfRange);
    }

    if (parameters.multiplierForGeometryContributionToHitGroupIndex > kMaximumGeometryContributionMultiplier)
    {
        return std::unexpected(ContractError::GeometryContributionMultiplierOutOfRange);
    }

    if (parameters.geometryContributionToHitGroupIndex > kMaximumGeometryContribution)
    {
        return std::unexpected(ContractError::GeometryContributionOutOfRange);
    }

    if (parameters.instanceContributionToHitGroupIndex > kMaximumInstanceContribution)
    {
        return std::unexpected(ContractError::InstanceContributionOutOfRange);
    }

    // The three terms are each bounded well below 2^32, so the sum cannot overflow a 64-bit accumulator. It is
    // still accumulated in 64 bits, because the point of the computation is that the caller can compare it against
    // a record count without a truncation hiding in the middle.
    auto const geometryTerm = static_cast<std::uint64_t>(parameters.multiplierForGeometryContributionToHitGroupIndex) *
                              static_cast<std::uint64_t>(parameters.geometryContributionToHitGroupIndex);
    return static_cast<std::uint64_t>(parameters.rayContributionToHitGroupIndex) + geometryTerm +
           static_cast<std::uint64_t>(parameters.instanceContributionToHitGroupIndex);
}

namespace
{

// `tableBase + sectionOffset + index * stride`, with every step checked. A shader-record address that wrapped
// would be an address the GPU reads happily and wrongly, so the wrap is refused here instead.
std::expected<std::uint64_t, ContractError> ResolveRecordAddress(std::uint64_t tableBaseAddress,
                                                                 ShaderTableSectionLayout const &section,
                                                                 std::uint64_t recordIndex) noexcept
{
    auto const recordOffset = CheckedMultiply(recordIndex, section.strideBytes);
    if (!recordOffset.has_value())
    {
        return std::unexpected(recordOffset.error());
    }

    auto const sectionStart = CheckedAdd(tableBaseAddress, section.offsetBytes);
    if (!sectionStart.has_value())
    {
        return std::unexpected(sectionStart.error());
    }

    return CheckedAdd(*sectionStart, *recordOffset);
}

} // namespace

std::expected<std::uint64_t, ContractError> ResolveHitGroupRecordAddress(ShaderTableLayout const &layout,
                                                                         std::uint64_t tableBaseAddress,
                                                                         HitGroupIndexParameters const &parameters)
{
    auto const baseValidation = ValidateTableBaseAddress(tableBaseAddress);
    if (!baseValidation.has_value())
    {
        return std::unexpected(baseValidation.error());
    }

    auto const index = ComputeHitGroupRecordIndex(parameters);
    if (!index.has_value())
    {
        return std::unexpected(index.error());
    }

    auto const section = FindShaderTableSection(layout, ShaderTableSectionKind::HitGroup);
    if (!section.has_value())
    {
        return std::unexpected(section.error());
    }

    if (*index >= section->recordCount)
    {
        return std::unexpected(ContractError::HitGroupRecordIndexOutOfRange);
    }

    return ResolveRecordAddress(tableBaseAddress, *section, *index);
}

std::expected<std::uint64_t, ContractError> ResolveMissRecordAddress(ShaderTableLayout const &layout,
                                                                     std::uint64_t tableBaseAddress,
                                                                     std::uint32_t missShaderIndex)
{
    auto const baseValidation = ValidateTableBaseAddress(tableBaseAddress);
    if (!baseValidation.has_value())
    {
        return std::unexpected(baseValidation.error());
    }

    if (missShaderIndex > kMaximumMissShaderIndex)
    {
        return std::unexpected(ContractError::MissShaderIndexOutOfRange);
    }

    auto const section = FindShaderTableSection(layout, ShaderTableSectionKind::Miss);
    if (!section.has_value())
    {
        return std::unexpected(section.error());
    }

    if (static_cast<std::uint64_t>(missShaderIndex) >= section->recordCount)
    {
        return std::unexpected(ContractError::MissRecordIndexOutOfRange);
    }

    return ResolveRecordAddress(tableBaseAddress, *section, static_cast<std::uint64_t>(missShaderIndex));
}

std::expected<std::uint64_t, ContractError> ResolveCallableRecordAddress(ShaderTableLayout const &layout,
                                                                         std::uint64_t tableBaseAddress,
                                                                         std::uint32_t callableIndex)
{
    auto const baseValidation = ValidateTableBaseAddress(tableBaseAddress);
    if (!baseValidation.has_value())
    {
        return std::unexpected(baseValidation.error());
    }

    auto const section = FindShaderTableSection(layout, ShaderTableSectionKind::Callable);
    if (!section.has_value())
    {
        return std::unexpected(section.error());
    }

    if (static_cast<std::uint64_t>(callableIndex) >= section->recordCount)
    {
        return std::unexpected(ContractError::CallableRecordIndexOutOfRange);
    }

    return ResolveRecordAddress(tableBaseAddress, *section, static_cast<std::uint64_t>(callableIndex));
}

// -------------------------------------------------------------------------------------------------------------
// J. Deterministic CPU reference intersection
// -------------------------------------------------------------------------------------------------------------

namespace
{

struct WorldTriangle final
{
    Float3 v0{};
    Float3 v1{};
    Float3 v2{};
    Float3 normal{};
};

WorldTriangle ToWorldTriangle(Transform3x4 const &objectToWorld, ReferenceTriangle const &triangle) noexcept
{
    WorldTriangle world{};
    world.v0 = TransformPoint(objectToWorld, triangle.v0);
    world.v1 = TransformPoint(objectToWorld, triangle.v1);
    world.v2 = TransformPoint(objectToWorld, triangle.v2);
    world.normal = Cross(Subtract(world.v1, world.v0), Subtract(world.v2, world.v0));
    return world;
}

// The scene is validated in full before a single triangle is intersected, so that a failure never leaves half of
// the counters filled in. A partially traversed scene reporting counters would be evidence about nothing.
std::expected<void, ContractError> ValidateReferenceScene(std::span<ReferenceInstance const> instances)
{
    if (instances.size() > kMaximumReferenceInstanceCount)
    {
        return std::unexpected(ContractError::ReferenceSceneTooLarge);
    }

    for (auto const &instance : instances)
    {
        auto const fields = ValidateInstanceFields(instance.instanceId, instance.instanceMask,
                                                   instance.instanceContributionToHitGroupIndex, instance.flags, true);
        if (!fields.has_value())
        {
            return std::unexpected(fields.error());
        }

        auto const transform = ValidateInstanceTransform(instance.objectToWorld);
        if (!transform.has_value())
        {
            return std::unexpected(transform.error());
        }

        if (instance.geometries.empty())
        {
            return std::unexpected(ContractError::EmptyReferenceGeometry);
        }

        if (instance.geometries.size() > kMaximumReferenceGeometriesPerInstance)
        {
            return std::unexpected(ContractError::ReferenceSceneTooLarge);
        }

        for (auto const &geometry : instance.geometries)
        {
            if (HasUndefinedBits(geometry.flags, kDefinedGeometryFlags))
            {
                return std::unexpected(ContractError::ConflictingGeometryFlags);
            }

            if (geometry.triangles.empty())
            {
                return std::unexpected(ContractError::EmptyReferenceGeometry);
            }

            if (geometry.triangles.size() > kMaximumReferenceTrianglesPerGeometry)
            {
                return std::unexpected(ContractError::ReferenceSceneTooLarge);
            }

            for (auto const &triangle : geometry.triangles)
            {
                if (!IsFiniteVector(triangle.v0) || !IsFiniteVector(triangle.v1) || !IsFiniteVector(triangle.v2))
                {
                    return std::unexpected(ContractError::NonFinite);
                }

                // Degeneracy is measured after the instance transform, because a scale can collapse a triangle that
                // was perfectly well formed in object space.
                auto const world = ToWorldTriangle(instance.objectToWorld, triangle);
                if ((0.5 * Length(world.normal)) < kMinimumTriangleArea)
                {
                    return std::unexpected(ContractError::DegenerateTriangle);
                }
            }
        }
    }

    return {};
}

// The opacity a hit resolves to. DXR applies the overrides in a fixed order: the geometry states its own opacity,
// the instance may override it, and the ray's flags override both.
bool ResolveOpacity(GeometryFlags geometryFlags, InstanceFlags instanceFlags, RayFlags rayFlags) noexcept
{
    bool opaque = HasFlag(geometryFlags, GeometryFlags::Opaque);

    if (HasFlag(instanceFlags, InstanceFlags::ForceOpaque))
    {
        opaque = true;
    }
    else if (HasFlag(instanceFlags, InstanceFlags::ForceNonOpaque))
    {
        opaque = false;
    }

    if (HasFlag(rayFlags, RayFlags::ForceOpaque))
    {
        opaque = true;
    }
    else if (HasFlag(rayFlags, RayFlags::ForceNonOpaque))
    {
        opaque = false;
    }

    return opaque;
}

} // namespace

std::expected<TraceResult, ContractError> TraceReferenceRay(std::span<ReferenceInstance const> instances,
                                                            TraceParameters const &parameters)
{
    auto const rayValidation = ValidateRay(parameters.ray);
    if (!rayValidation.has_value())
    {
        return std::unexpected(rayValidation.error());
    }

    auto const flagValidation = ValidateRayFlags(parameters.flags);
    if (!flagValidation.has_value())
    {
        return std::unexpected(flagValidation.error());
    }

    auto const maskValidation = ValidateInstanceInclusionMask(parameters.instanceInclusionMask);
    if (!maskValidation.has_value())
    {
        return std::unexpected(maskValidation.error());
    }

    if (parameters.rayContributionToHitGroupIndex > kMaximumRayContributionToHitGroupIndex)
    {
        return std::unexpected(ContractError::RayContributionOutOfRange);
    }

    if (parameters.multiplierForGeometryContributionToHitGroupIndex > kMaximumGeometryContributionMultiplier)
    {
        return std::unexpected(ContractError::GeometryContributionMultiplierOutOfRange);
    }

    if (parameters.missShaderIndex > kMaximumMissShaderIndex)
    {
        return std::unexpected(ContractError::MissShaderIndexOutOfRange);
    }

    auto const sceneValidation = ValidateReferenceScene(instances);
    if (!sceneValidation.has_value())
    {
        return std::unexpected(sceneValidation.error());
    }

    TraceResult result{};
    result.missShaderIndex = parameters.missShaderIndex;

    bool const skipTriangles = HasFlag(parameters.flags, RayFlags::SkipTriangles);
    bool const acceptFirstHit = HasFlag(parameters.flags, RayFlags::AcceptFirstHitAndEndSearch);

    auto const &ray = parameters.ray;
    std::optional<TriangleHit> best{};
    std::uint32_t bestGeometryIndex = 0U;
    std::uint32_t bestInstanceContribution = 0U;
    bool searchEnded = false;

    for (std::size_t instanceIndex = 0U; instanceIndex < instances.size() && !searchEnded; ++instanceIndex)
    {
        auto const &instance = instances[instanceIndex];
        ++result.counters.instancesTested;

        if ((instance.instanceMask & parameters.instanceInclusionMask) == 0U)
        {
            ++result.counters.instancesCulledByMask;
            continue;
        }

        bool const cullDisabled = HasFlag(instance.flags, InstanceFlags::TriangleCullDisable);
        bool const counterclockwiseIsFront = HasFlag(instance.flags, InstanceFlags::TriangleFrontCounterclockwise);
        // A negative determinant reverses the world-space winding. D3D12 instead decides facing from the unchanged
        // object-space vertices and the ray after the inverse instance transform has carried it into that space.
        bool const instanceMirrors = TransformDeterminant(instance.objectToWorld) < 0.0;

        for (std::size_t geometryIndex = 0U; geometryIndex < instance.geometries.size() && !searchEnded;
             ++geometryIndex)
        {
            auto const &geometry = instance.geometries[geometryIndex];
            bool const opaque = ResolveOpacity(geometry.flags, instance.flags, parameters.flags);

            for (std::size_t primitiveIndex = 0U; primitiveIndex < geometry.triangles.size(); ++primitiveIndex)
            {
                ++result.counters.trianglesTested;

                if (skipTriangles)
                {
                    ++result.counters.culledBySkipTriangles;
                    continue;
                }

                auto const world = ToWorldTriangle(instance.objectToWorld, geometry.triangles[primitiveIndex]);
                auto const edge1 = Subtract(world.v1, world.v0);
                auto const edge2 = Subtract(world.v2, world.v0);

                auto const pVector = Cross(ray.direction, edge2);
                auto const determinant = Dot(edge1, pVector);
                if (std::abs(determinant) < kParallelRayDeterminantEpsilon)
                {
                    ++result.counters.culledByParallelRay;
                    continue;
                }

                auto const inverseDeterminant = 1.0 / determinant;
                auto const tVector = Subtract(ray.origin, world.v0);
                auto const barycentric1 = Dot(tVector, pVector) * inverseDeterminant;
                if (barycentric1 < 0.0 || barycentric1 > 1.0)
                {
                    continue;
                }

                auto const qVector = Cross(tVector, edge1);
                auto const barycentric2 = Dot(ray.direction, qVector) * inverseDeterminant;
                if (barycentric2 < 0.0 || (barycentric1 + barycentric2) > 1.0)
                {
                    continue;
                }

                auto const tHit = Dot(edge2, qVector) * inverseDeterminant;
                ++result.counters.candidateHits;

                // The fixed-function triangle interval is open at both ends: D3D12 accepts a triangle hit only when
                // `TMin < t < TMax` (DirectX-Specs `d3d/Raytracing.md`, "Ray extents"). A hit landing exactly on an
                // endpoint is a miss, so the endpoints are rejected here rather than kept "for safety". Procedural
                // primitives use the closed test instead, which this chapter does not model.
                if (!(tHit > ray.tMinMetres) || !(tHit < ray.tMaxMetres))
                {
                    ++result.counters.culledByInterval;
                    continue;
                }

                // Facing is object space. The world-space geometric normal is cross(M*e1, M*e2), which equals
                // det(M) * inverse-transpose(M) * cross(e1, e2), so dot(M*d, worldNormal) is the object-space
                // facing dot product scaled by det(M). Dividing the determinant's sign back out is therefore the
                // object-space test. This removes the transform's world-space winding reversal without removing
                // the effect that transforming the ray itself can have on facing. The parallel-ray rejection above
                // guarantees the world-space dot product is non-zero, and the scene validation guarantees the
                // determinant is.
                bool frontFacing = (Dot(ray.direction, world.normal) < 0.0) != instanceMirrors;
                if (counterclockwiseIsFront)
                {
                    frontFacing = !frontFacing;
                }

                if (!cullDisabled)
                {
                    if (frontFacing && HasFlag(parameters.flags, RayFlags::CullFrontFacingTriangles))
                    {
                        ++result.counters.culledByFace;
                        continue;
                    }

                    if (!frontFacing && HasFlag(parameters.flags, RayFlags::CullBackFacingTriangles))
                    {
                        ++result.counters.culledByFace;
                        continue;
                    }
                }

                if ((opaque && HasFlag(parameters.flags, RayFlags::CullOpaque)) ||
                    (!opaque && HasFlag(parameters.flags, RayFlags::CullNonOpaque)))
                {
                    ++result.counters.culledByOpacity;
                    continue;
                }

                // Strictly closer wins, so a tie is resolved by traversal order: instance, then geometry, then
                // primitive. Hardware makes no such promise, which is the whole reason this is documented.
                if (best.has_value() && tHit >= best->tHitMetres)
                {
                    continue;
                }

                auto normal = world.normal;
                auto const normalLength = Length(normal);
                normal =
                    Float3{.x = normal.x / normalLength, .y = normal.y / normalLength, .z = normal.z / normalLength};
                if (parameters.normalPolicy == FaceNormalPolicy::OpposeRayDirection && Dot(ray.direction, normal) > 0.0)
                {
                    normal = Float3{.x = -normal.x, .y = -normal.y, .z = -normal.z};
                }

                TriangleHit hit{};
                hit.tHitMetres = tHit;
                hit.barycentrics = Float2{.x = barycentric1, .y = barycentric2};
                hit.instanceIndex = static_cast<std::uint32_t>(instanceIndex);
                hit.instanceId = instance.instanceId;
                hit.geometryIndex = static_cast<std::uint32_t>(geometryIndex);
                hit.primitiveIndex = static_cast<std::uint32_t>(primitiveIndex);
                hit.hitKind = frontFacing ? HitKind::FrontFace : HitKind::BackFace;
                hit.worldPosition = Float3{.x = ray.origin.x + (tHit * ray.direction.x),
                                           .y = ray.origin.y + (tHit * ray.direction.y),
                                           .z = ray.origin.z + (tHit * ray.direction.z)};
                hit.worldNormal = normal;
                hit.anyHitWouldRun = !opaque;

                best = hit;
                bestGeometryIndex = hit.geometryIndex;
                bestInstanceContribution = instance.instanceContributionToHitGroupIndex;

                if (acceptFirstHit)
                {
                    result.acceptedFirstHit = true;
                    searchEnded = true;
                    break;
                }
            }
        }
    }

    if (best.has_value())
    {
        HitGroupIndexParameters indexParameters{};
        indexParameters.rayContributionToHitGroupIndex = parameters.rayContributionToHitGroupIndex;
        indexParameters.multiplierForGeometryContributionToHitGroupIndex =
            parameters.multiplierForGeometryContributionToHitGroupIndex;
        indexParameters.geometryContributionToHitGroupIndex = bestGeometryIndex;
        indexParameters.instanceContributionToHitGroupIndex = bestInstanceContribution;

        auto const recordIndex = ComputeHitGroupRecordIndex(indexParameters);
        if (!recordIndex.has_value())
        {
            return std::unexpected(recordIndex.error());
        }

        best->hitGroupRecordIndex = *recordIndex;
        result.hit = best;
        result.closestHitWouldRun = !HasFlag(parameters.flags, RayFlags::SkipClosestHitShader);
    }

    return result;
}

// -------------------------------------------------------------------------------------------------------------
// K. Stage status evidence
// -------------------------------------------------------------------------------------------------------------

PipelineStatusLedger::PipelineStatusLedger() noexcept
{
    for (std::size_t index = 0U; index < stages_.size(); ++index)
    {
        stages_[index].stage = static_cast<PipelineStage>(index);
    }
}

std::expected<void, ContractError> PipelineStatusLedger::RecordSuccess(PipelineStage stage,
                                                                       std::uint32_t evidenceCount) noexcept
{
    auto const index = static_cast<std::size_t>(stage);
    if (index >= stages_.size())
    {
        return std::unexpected(ContractError::StageOutOfOrder);
    }

    // Once a stage has failed, later stages have not run: recording one would be evidence about a pipeline that
    // never reached that point.
    if (firstFailedStage_.has_value())
    {
        return std::unexpected(ContractError::StageAfterFailure);
    }

    if (stages_[index].recorded)
    {
        return std::unexpected(ContractError::StageAlreadyRecorded);
    }

    if (index != nextStageIndex_)
    {
        return std::unexpected(ContractError::StageOutOfOrder);
    }

    // A stage that succeeded without publishing a single checked fact has not been exercised; accepting it would
    // let an empty pipeline claim to be ready to dispatch.
    if (evidenceCount == 0U)
    {
        return std::unexpected(ContractError::MissingStageEvidence);
    }

    stages_[index].recorded = true;
    stages_[index].succeeded = true;
    stages_[index].failure = std::nullopt;
    stages_[index].evidenceCount = evidenceCount;
    ++nextStageIndex_;
    return {};
}

std::expected<void, ContractError> PipelineStatusLedger::RecordFailure(PipelineStage stage,
                                                                       ContractError failure) noexcept
{
    auto const index = static_cast<std::size_t>(stage);
    if (index >= stages_.size())
    {
        return std::unexpected(ContractError::StageOutOfOrder);
    }

    if (firstFailedStage_.has_value())
    {
        return std::unexpected(ContractError::StageAfterFailure);
    }

    if (stages_[index].recorded)
    {
        return std::unexpected(ContractError::StageAlreadyRecorded);
    }

    if (index != nextStageIndex_)
    {
        return std::unexpected(ContractError::StageOutOfOrder);
    }

    stages_[index].recorded = true;
    stages_[index].succeeded = false;
    stages_[index].failure = failure;
    stages_[index].evidenceCount = 0U;
    firstFailedStage_ = stage;
    ++nextStageIndex_;
    return {};
}

StageStatus PipelineStatusLedger::Status(PipelineStage stage) const noexcept
{
    auto const index = static_cast<std::size_t>(stage);
    if (index >= stages_.size())
    {
        return StageStatus{};
    }

    return stages_[index];
}

std::optional<PipelineStage> PipelineStatusLedger::FirstFailedStage() const noexcept
{
    return firstFailedStage_;
}

std::optional<PipelineStage> PipelineStatusLedger::NextExpectedStage() const noexcept
{
    if (nextStageIndex_ >= stages_.size())
    {
        return std::nullopt;
    }

    return static_cast<PipelineStage>(nextStageIndex_);
}

bool PipelineStatusLedger::IsReadyToDispatch() const noexcept
{
    return std::ranges::all_of(stages_, [](StageStatus const &status) { return status.recorded && status.succeeded; });
}

std::uint32_t PipelineStatusLedger::TotalEvidenceCount() const noexcept
{
    std::uint32_t total = 0U;
    for (auto const &status : stages_)
    {
        total += status.evidenceCount;
    }

    return total;
}

} // namespace ch33::dxr
