#pragma once

#include "GBufferContracts.hpp"
#include "SurfaceFrame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

namespace ch15::visibility_buffer
{

using Float2 = ch06::surface_frames::Float2;
using Float3 = ch06::surface_frames::Float3;
using Float4 = ch06::surface_frames::Float4;
using SurfaceVertex = ch06::surface_frames::SurfaceVertex;

// Low-to-high bits: word 0 is draw[16] | primitive-low[16], and word 1 is primitive-high[4] |
// barycentric-first[14] | barycentric-second[14]. The third component is the remaining simplex weight.
inline constexpr std::uint32_t kDrawIdentifierBitCount = 16U;
inline constexpr std::uint32_t kPrimitiveIdentifierBitCount = 20U;
inline constexpr std::uint32_t kStoredBarycentricComponentBitCount = 14U;
inline constexpr std::uint32_t kPrimitiveIdentifierLowBitCount = 16U;
inline constexpr std::uint32_t kPrimitiveIdentifierHighBitCount =
    kPrimitiveIdentifierBitCount - kPrimitiveIdentifierLowBitCount;

inline constexpr std::uint32_t kMaximumDrawIdentifier = (1U << kDrawIdentifierBitCount) - 1U;
inline constexpr std::uint32_t kMaximumPrimitiveIdentifier = (1U << kPrimitiveIdentifierBitCount) - 1U;
inline constexpr std::uint32_t kMaximumStoredBarycentricComponent = (1U << kStoredBarycentricComponentBitCount) - 1U;

inline constexpr std::uint32_t kDrawIdentifierShift = 0U;
inline constexpr std::uint32_t kPrimitiveIdentifierLowShift = kDrawIdentifierBitCount;
inline constexpr std::uint32_t kPrimitiveIdentifierHighShift = 0U;
inline constexpr std::uint32_t kFirstBarycentricShift = kPrimitiveIdentifierHighBitCount;
inline constexpr std::uint32_t kSecondBarycentricShift = kFirstBarycentricShift + kStoredBarycentricComponentBitCount;

static_assert(kDrawIdentifierBitCount + kPrimitiveIdentifierLowBitCount == 32U);
static_assert(kPrimitiveIdentifierHighBitCount + (2U * kStoredBarycentricComponentBitCount) == 32U);

struct VisibilityRecord final
{
    std::uint32_t drawAndPrimitiveLowWord{};
    std::uint32_t primitiveHighAndBarycentricWord{};

    [[nodiscard]] constexpr bool operator==(VisibilityRecord const &) const noexcept = default;
};

inline constexpr VisibilityRecord kBackgroundVisibilityRecord{};

static_assert(sizeof(VisibilityRecord) == 2U * sizeof(std::uint32_t));
static_assert(alignof(VisibilityRecord) == alignof(std::uint32_t));
static_assert(offsetof(VisibilityRecord, drawAndPrimitiveLowWord) == 0U);
static_assert(offsetof(VisibilityRecord, primitiveHighAndBarycentricWord) == sizeof(std::uint32_t));
static_assert(std::is_standard_layout_v<VisibilityRecord>);
static_assert(std::is_trivially_copyable_v<VisibilityRecord>);

struct BarycentricCoordinates final
{
    float first{};
    float second{};
    float third{};

    [[nodiscard]] constexpr bool operator==(BarycentricCoordinates const &) const noexcept = default;
};

struct QuantizedBarycentrics final
{
    std::uint32_t first{};
    std::uint32_t second{};

    [[nodiscard]] constexpr bool operator==(QuantizedBarycentrics const &) const noexcept = default;
};

struct DecodedVisibility final
{
    bool isBackground{};
    std::uint32_t drawIdentifier{};
    std::uint32_t primitiveIdentifier{};
    BarycentricCoordinates screenBarycentrics{};
};

enum class ContractError : std::uint8_t
{
    NonFinite = 0U,
    ValueOutOfRange,
    InvalidBarycentrics,
    UnrepresentableIdentifier,
    MalformedVisibilityRecord,
    BackgroundVisibility,
    InvalidExtent,
    InvalidSamplePosition,
    InvalidClipW,
    DegenerateTriangle,
    OutsideTriangle,
    MalformedDrawRange,
    InvalidDrawIdentifier,
    InvalidPrimitiveIdentifier,
    InvalidIndex,
    ArithmeticOverflow,
    InvalidSurfaceFrame,
    InvalidLogicalStorage,
    PayloadAccountingFailure,
};

[[nodiscard]] constexpr bool IsBackground(VisibilityRecord record) noexcept
{
    return record == kBackgroundVisibilityRecord;
}

[[nodiscard]] std::expected<QuantizedBarycentrics, ContractError> QuantizeBarycentrics(
    BarycentricCoordinates barycentrics) noexcept;
[[nodiscard]] std::expected<BarycentricCoordinates, ContractError> DecodeBarycentrics(
    QuantizedBarycentrics barycentrics) noexcept;
[[nodiscard]] std::expected<VisibilityRecord, ContractError> PackVisibility(
    std::uint32_t oneBasedDrawIdentifier, std::uint32_t primitiveIdentifier,
    BarycentricCoordinates screenBarycentrics) noexcept;
[[nodiscard]] std::expected<DecodedVisibility, ContractError> UnpackVisibility(VisibilityRecord record) noexcept;

struct IndexedDrawRange final
{
    std::uint32_t vertexOffset{};
    std::uint32_t vertexCount{};
    std::uint32_t indexOffset{};
    std::uint32_t indexCount{};
};

struct GeometryVertex final
{
    Float4 clipPosition{};
    SurfaceVertex surface{};
};

struct IndexedTriangle final
{
    std::array<std::uint32_t, 3U> vertexIndices{};
};

struct TriangleVertices final
{
    std::array<GeometryVertex, 3U> vertices{};
};

[[nodiscard]] std::expected<IndexedTriangle, ContractError> CheckedIndexedTriangle(
    std::uint32_t oneBasedDrawIdentifier, std::uint32_t primitiveIdentifier, std::span<IndexedDrawRange const> draws,
    std::span<std::uint32_t const> indices, std::span<GeometryVertex const> vertices) noexcept;
[[nodiscard]] std::expected<TriangleVertices, ContractError> ResolveVisibilityTriangle(
    VisibilityRecord record, std::span<IndexedDrawRange const> draws, std::span<std::uint32_t const> indices,
    std::span<GeometryVertex const> vertices) noexcept;

struct RenderExtent final
{
    std::uint32_t width{};
    std::uint32_t height{};
};

inline constexpr std::uint32_t kMaximumRenderDimensionForFloatPixelCenters = 1U << 23U;

[[nodiscard]] std::expected<Float2, ContractError> PixelCenter(std::uint32_t pixelX, std::uint32_t pixelY,
                                                               RenderExtent extent) noexcept;
[[nodiscard]] std::expected<BarycentricCoordinates, ContractError> ComputeScreenBarycentrics(
    Float2 samplePosition, RenderExtent extent, TriangleVertices const &triangle) noexcept;
[[nodiscard]] std::expected<BarycentricCoordinates, ContractError> ComputePerspectiveCorrectWeights(
    BarycentricCoordinates screenBarycentrics, TriangleVertices const &triangle) noexcept;

struct ReconstructedSurface final
{
    Float3 viewPosition{};
    Float2 textureCoordinates{};
    Float3 normal{};
    Float4 tangent{};
    Float3 bitangent{};
};

[[nodiscard]] std::expected<Float2, ContractError> InterpolateTextureCoordinates(
    BarycentricCoordinates screenBarycentrics, TriangleVertices const &triangle) noexcept;
[[nodiscard]] std::expected<ReconstructedSurface, ContractError> ReconstructSurface(
    BarycentricCoordinates screenBarycentrics, TriangleVertices const &triangle) noexcept;
[[nodiscard]] std::expected<ReconstructedSurface, ContractError> ReconstructSurfaceAtSample(
    Float2 samplePosition, RenderExtent extent, TriangleVertices const &triangle) noexcept;
[[nodiscard]] std::expected<ReconstructedSurface, ContractError> ReconstructSurfaceFromVisibility(
    VisibilityRecord record, std::span<IndexedDrawRange const> draws, std::span<std::uint32_t const> indices,
    std::span<GeometryVertex const> vertices) noexcept;

struct TextureGradients final
{
    Float2 ddx{};
    Float2 ddy{};
};

// Derivatives are with respect to pixel x increasing right and pixel y increasing down from a top-left origin.
[[nodiscard]] std::expected<TextureGradients, ContractError> ComputeAnalyticTextureGradients(
    Float2 samplePosition, RenderExtent extent, TriangleVertices const &triangle) noexcept;

enum class VisibilityLoadRule : std::uint8_t
{
    ExactIntegerLoad = 0U,
};

enum class MaterialEvaluationAction : std::uint8_t
{
    SkipBackground = 0U,
    EvaluateForeground,
};

enum class TextureGradientSource : std::uint8_t
{
    None = 0U,
    AnalyticSameTriangle,
};

struct DiscontinuityPolicy final
{
    MaterialEvaluationAction action{MaterialEvaluationAction::SkipBackground};
    TextureGradientSource gradientSource{TextureGradientSource::None};
    bool allNeighborIdentifiersMatch{};
};

[[nodiscard]] constexpr VisibilityLoadRule RequiredVisibilityLoadRule() noexcept
{
    return VisibilityLoadRule::ExactIntegerLoad;
}

[[nodiscard]] std::expected<DiscontinuityPolicy, ContractError> DetermineDiscontinuityPolicy(
    VisibilityRecord center, std::span<VisibilityRecord const> neighbors) noexcept;

inline constexpr std::uint32_t kVisibilityRecordLogicalBytesPerPixel = sizeof(VisibilityRecord);
inline constexpr std::uint32_t kExistingDepthLogicalBytesPerPixel = 4U;
inline constexpr std::uint32_t kChapter12CoreSurfaceLogicalBytesPerPixelExcludingDepth = 9U;
inline constexpr std::uint32_t kChapter12ExtendedSurfaceLogicalBytesPerPixelExcludingDepth = 17U;

struct LogicalPayloadAccounting final
{
    std::uint32_t logicalSurfaceBytesPerPixelExcludingDepth{};
    std::uint32_t logicalDepthBytesPerPixel{};
    std::uint32_t logicalTotalBytesPerPixel{};
    ch12::gbuffer::LogicalGBufferTraffic logicalTraffic{};
};

struct LogicalPayloadSavings final
{
    std::uint32_t logicalBytesPerPixel{};
    std::uint64_t rasterWriteBytes{};
    std::uint64_t materialReadBytes{};
    std::uint64_t totalPayloadBytes{};
};

struct LogicalPayloadComparison final
{
    LogicalPayloadAccounting visibilityAndDepth{};
    LogicalPayloadAccounting chapter12Core{};
    LogicalPayloadAccounting chapter12Extended{};
    LogicalPayloadSavings visibilitySavingsVersusCore{};
    LogicalPayloadSavings visibilitySavingsVersusExtended{};
};

[[nodiscard]] std::expected<std::uint32_t, ContractError> CheckedLogicalBytesPerPixel(
    std::uint32_t logicalSurfaceBytesPerPixelExcludingDepth, std::uint32_t logicalDepthBytesPerPixel) noexcept;

// These values describe logical storage and payload only; they are not measured physical bandwidth.
[[nodiscard]] std::expected<LogicalPayloadComparison, ContractError> ComputeLogicalPayloadComparison(
    std::uint32_t renderWidth, std::uint32_t renderHeight) noexcept;

} // namespace ch15::visibility_buffer
