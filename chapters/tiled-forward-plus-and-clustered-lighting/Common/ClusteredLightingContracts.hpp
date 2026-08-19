#pragma once

#include "GBufferContracts.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace ch14::clustered_lighting
{

using DepthConvention = ch12::gbuffer::DepthConvention;
using Float3 = ch12::gbuffer::Float3;
using PerspectiveProjection = ch12::gbuffer::PerspectiveProjection;

enum class ContractError : std::uint8_t
{
    NonFinite = 0U,
    InvalidDimension,
    InvalidTileGrid,
    InvalidTileIndex,
    InvalidPixelCoordinate,
    ArithmeticOverflow,
    InvalidProjection,
    InvalidLight,
    LightBehindCamera,
    InvalidDepth,
    TooManySamples,
    InvalidDepthRange,
    InvalidSlicing,
    InvalidSliceIndex,
    InvalidOverlapData,
    InvalidCapacity,
    InvalidCellIndex,
    InvalidLightIndex,
    MalformedLightList,
    IncompleteLightList,
};

struct TileGrid final
{
    std::uint32_t renderWidth{};
    std::uint32_t renderHeight{};
    std::uint32_t tileWidth{};
    std::uint32_t tileHeight{};
    std::uint32_t tileCountX{};
    std::uint32_t tileCountY{};
    std::uint32_t tileCount{};

    [[nodiscard]] bool operator==(TileGrid const &) const noexcept = default;
};

struct TileCoordinate final
{
    std::uint32_t x{};
    std::uint32_t y{};

    [[nodiscard]] bool operator==(TileCoordinate const &) const noexcept = default;
};

[[nodiscard]] std::expected<TileGrid, ContractError> MakeTileGrid(std::uint32_t renderWidth, std::uint32_t renderHeight,
                                                                  std::uint32_t tileWidth,
                                                                  std::uint32_t tileHeight) noexcept;
[[nodiscard]] std::expected<std::uint32_t, ContractError> CheckedTileIndex(TileGrid const &grid, std::uint32_t tileX,
                                                                           std::uint32_t tileY) noexcept;
[[nodiscard]] std::expected<TileCoordinate, ContractError> CheckedTileCoordinate(TileGrid const &grid,
                                                                                 std::uint32_t tileIndex) noexcept;
[[nodiscard]] std::expected<TileCoordinate, ContractError> TileForPixel(TileGrid const &grid, std::uint32_t pixelX,
                                                                        std::uint32_t pixelY) noexcept;

inline constexpr std::uint32_t kMaximumClusterSliceCount = 65'536U;

struct ClusterGrid final
{
    TileGrid tiles{};
    std::uint32_t sliceCount{};
    std::uint32_t clusterCount{};

    [[nodiscard]] bool operator==(ClusterGrid const &) const noexcept = default;
};

struct ClusterCoordinate final
{
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};

    [[nodiscard]] bool operator==(ClusterCoordinate const &) const noexcept = default;
};

[[nodiscard]] std::expected<ClusterGrid, ContractError> MakeClusterGrid(TileGrid const &tiles,
                                                                        std::uint32_t sliceCount) noexcept;
[[nodiscard]] std::expected<std::uint32_t, ContractError> CheckedClusterIndex(ClusterGrid const &grid,
                                                                              ClusterCoordinate coordinate) noexcept;
[[nodiscard]] std::expected<ClusterCoordinate, ContractError> CheckedClusterCoordinate(
    ClusterGrid const &grid, std::uint32_t clusterIndex) noexcept;

struct PixelRect final
{
    std::uint32_t minimumX{};
    std::uint32_t minimumY{};
    std::uint32_t maximumXExclusive{};
    std::uint32_t maximumYExclusive{};

    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        return minimumX == maximumXExclusive || minimumY == maximumYExclusive;
    }

    [[nodiscard]] bool operator==(PixelRect const &) const noexcept = default;
};

struct TileRect final
{
    std::uint32_t minimumX{};
    std::uint32_t minimumY{};
    std::uint32_t maximumXExclusive{};
    std::uint32_t maximumYExclusive{};

    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        return minimumX == maximumXExclusive || minimumY == maximumYExclusive;
    }

    [[nodiscard]] bool operator==(TileRect const &) const noexcept = default;
};

struct PointLightView final
{
    Float3 position{};
    float radius{};
};

enum class ScreenBoundsMode : std::uint8_t
{
    Projected = 0U,
    NearPlaneFullscreenFallback,
    Offscreen,
};

struct ConservativeScreenBounds final
{
    PixelRect pixels{};
    TileRect tiles{};
    ScreenBoundsMode mode{ScreenBoundsMode::Offscreen};
};

[[nodiscard]] std::expected<ConservativeScreenBounds, ContractError> ComputeConservativeScreenBounds(
    PointLightView light, PerspectiveProjection projection, TileGrid const &grid) noexcept;

struct ViewDepthInterval final
{
    float minimum{};
    float maximum{};

    [[nodiscard]] bool operator==(ViewDepthInterval const &) const noexcept = default;
};

struct TileDepthRange final
{
    std::optional<ViewDepthInterval> viewDepths{};
    std::uint32_t foregroundSampleCount{};

    [[nodiscard]] constexpr bool IsEmpty() const noexcept
    {
        return !viewDepths.has_value();
    }
};

[[nodiscard]] std::expected<TileDepthRange, ContractError> ReduceTileDepthRange(
    std::span<float const> deviceDepthSamples, PerspectiveProjection projection) noexcept;

struct LogDepthSlicing final
{
    float nearDepth{};
    float farDepth{};
    std::uint32_t sliceCount{};
};

[[nodiscard]] std::expected<void, ContractError> ValidateLogDepthSlicing(LogDepthSlicing slicing) noexcept;
[[nodiscard]] std::expected<std::uint32_t, ContractError> DepthToSlice(float viewDepth,
                                                                       LogDepthSlicing slicing) noexcept;
[[nodiscard]] std::expected<ViewDepthInterval, ContractError> SliceDepthBounds(std::uint32_t sliceIndex,
                                                                               LogDepthSlicing slicing) noexcept;

// These helpers test an enclosing view-space AABB, so they may admit false positives but cannot reject an
// intersection with the represented tile or cluster frustum.
[[nodiscard]] std::expected<bool, ContractError> SphereOverlapsTileDepthRange(
    PointLightView light, TileGrid const &grid, std::uint32_t tileX, std::uint32_t tileY,
    TileDepthRange const &depthRange, PerspectiveProjection projection) noexcept;
[[nodiscard]] std::expected<bool, ContractError> SphereOverlapsCluster(PointLightView light, TileGrid const &grid,
                                                                       std::uint32_t tileX, std::uint32_t tileY,
                                                                       std::uint32_t sliceIndex,
                                                                       LogDepthSlicing slicing,
                                                                       PerspectiveProjection projection) noexcept;

struct CellLightRange final
{
    std::uint32_t offset{};
    std::uint32_t count{};
    std::uint32_t attemptedCount{};
    std::uint32_t overflowCount{};

    [[nodiscard]] bool operator==(CellLightRange const &) const noexcept = default;
};

struct LightListStatistics final
{
    std::uint64_t attemptedCount{};
    std::uint64_t emittedCount{};
    std::uint64_t overflowCount{};

    [[nodiscard]] bool operator==(LightListStatistics const &) const noexcept = default;
};

struct BoundedLightLists final
{
    std::vector<CellLightRange> cells{};
    std::vector<std::uint32_t> lightIndices{};
    LightListStatistics statistics{};
};

[[nodiscard]] std::expected<BoundedLightLists, ContractError> BuildBoundedLightLists(
    std::uint32_t cellCount, std::uint32_t lightCount, std::span<std::uint8_t const> cellMajorOverlaps,
    std::uint64_t capacity);
[[nodiscard]] std::expected<void, ContractError> ValidateBoundedLightLists(BoundedLightLists const &lists,
                                                                           std::uint32_t lightCount) noexcept;

struct ScalarReferenceComparison final
{
    double listedAccumulation{};
    double bruteForceAccumulation{};
    bool equivalent{};
};

[[nodiscard]] std::expected<ScalarReferenceComparison, ContractError> CompareCellAccumulation(
    std::uint32_t cellIndex, std::uint32_t lightCount, std::span<std::uint8_t const> lightOverlaps,
    std::span<float const> lightContributions, BoundedLightLists const &lists) noexcept;

} // namespace ch14::clustered_lighting
