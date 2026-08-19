#include "ClusteredLightingContracts.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace ch14::clustered_lighting
{
namespace
{

struct ViewAabb final
{
    double minimumX{};
    double minimumY{};
    double minimumZ{};
    double maximumX{};
    double maximumY{};
    double maximumZ{};
};

[[nodiscard]] bool IsFinite(float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool IsFinite(Float3 value) noexcept
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

[[nodiscard]] std::uint32_t CeilDivide(std::uint32_t numerator, std::uint32_t denominator) noexcept
{
    return (numerator / denominator) + (numerator % denominator != 0U ? 1U : 0U);
}

[[nodiscard]] std::expected<void, ContractError> ValidateProjection(PerspectiveProjection projection) noexcept
{
    auto const coefficients = ch12::gbuffer::MakeDeviceDepthCoefficients(projection);
    if (coefficients)
    {
        return {};
    }
    if (coefficients.error() == ch12::gbuffer::ContractError::NonFinite)
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return std::unexpected(ContractError::InvalidProjection);
}

[[nodiscard]] std::expected<void, ContractError> ValidateLight(PointLightView light) noexcept
{
    if (!IsFinite(light.position) || !IsFinite(light.radius))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (light.radius <= 0.0F)
    {
        return std::unexpected(ContractError::InvalidLight);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateGrid(TileGrid const &grid) noexcept
{
    auto const rebuilt = MakeTileGrid(grid.renderWidth, grid.renderHeight, grid.tileWidth, grid.tileHeight);
    if (!rebuilt)
    {
        return std::unexpected(rebuilt.error());
    }
    if (*rebuilt != grid)
    {
        return std::unexpected(ContractError::InvalidTileGrid);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateClusterGrid(ClusterGrid const &grid) noexcept
{
    auto const rebuilt = MakeClusterGrid(grid.tiles, grid.sliceCount);
    if (!rebuilt)
    {
        return std::unexpected(rebuilt.error());
    }
    if (*rebuilt != grid)
    {
        return std::unexpected(ContractError::InvalidTileGrid);
    }
    return {};
}

[[nodiscard]] ContractError MapDepthError(ch12::gbuffer::ContractError error) noexcept
{
    if (error == ch12::gbuffer::ContractError::NonFinite)
    {
        return ContractError::NonFinite;
    }
    if (error == ch12::gbuffer::ContractError::InvalidProjection)
    {
        return ContractError::InvalidProjection;
    }
    return ContractError::InvalidDepth;
}

[[nodiscard]] float SliceBoundaryUnchecked(LogDepthSlicing slicing, std::uint32_t boundaryIndex) noexcept
{
    if (boundaryIndex == 0U)
    {
        return slicing.nearDepth;
    }
    if (boundaryIndex == slicing.sliceCount)
    {
        return slicing.farDepth;
    }

    double const nearDepth = static_cast<double>(slicing.nearDepth);
    double const logRange = std::log(static_cast<double>(slicing.farDepth) / nearDepth);
    double const fraction = static_cast<double>(boundaryIndex) / static_cast<double>(slicing.sliceCount);
    return static_cast<float>(nearDepth * std::exp(logRange * fraction));
}

[[nodiscard]] std::expected<void, ContractError> ValidateDepthInterval(ViewDepthInterval interval,
                                                                       PerspectiveProjection projection) noexcept
{
    if (!IsFinite(interval.minimum) || !IsFinite(interval.maximum))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (interval.minimum < projection.nearPlane || interval.maximum > projection.farPlane ||
        interval.minimum > interval.maximum)
    {
        return std::unexpected(ContractError::InvalidDepthRange);
    }
    return {};
}

[[nodiscard]] std::expected<ViewAabb, ContractError> MakeTileViewAabb(TileGrid const &grid, std::uint32_t tileX,
                                                                      std::uint32_t tileY, ViewDepthInterval interval,
                                                                      PerspectiveProjection projection) noexcept
{
    if (auto const validGrid = ValidateGrid(grid); !validGrid)
    {
        return std::unexpected(validGrid.error());
    }
    if (auto const validProjection = ValidateProjection(projection); !validProjection)
    {
        return std::unexpected(validProjection.error());
    }
    if (tileX >= grid.tileCountX || tileY >= grid.tileCountY)
    {
        return std::unexpected(ContractError::InvalidTileIndex);
    }
    if (auto const validDepth = ValidateDepthInterval(interval, projection); !validDepth)
    {
        return std::unexpected(validDepth.error());
    }

    std::uint64_t const pixelMinimumX = static_cast<std::uint64_t>(tileX) * grid.tileWidth;
    std::uint64_t const pixelMinimumY = static_cast<std::uint64_t>(tileY) * grid.tileHeight;
    std::uint64_t const pixelMaximumX = std::min<std::uint64_t>(pixelMinimumX + grid.tileWidth, grid.renderWidth);
    std::uint64_t const pixelMaximumY = std::min<std::uint64_t>(pixelMinimumY + grid.tileHeight, grid.renderHeight);

    double const tangentHalfFov = std::tan(static_cast<double>(projection.verticalFieldOfViewRadians) * 0.5);
    double const horizontalScale = tangentHalfFov * static_cast<double>(projection.aspectRatio);
    double const minimumNdcX = (2.0 * static_cast<double>(pixelMinimumX) / static_cast<double>(grid.renderWidth)) - 1.0;
    double const maximumNdcX = (2.0 * static_cast<double>(pixelMaximumX) / static_cast<double>(grid.renderWidth)) - 1.0;
    double const maximumNdcY =
        1.0 - (2.0 * static_cast<double>(pixelMinimumY) / static_cast<double>(grid.renderHeight));
    double const minimumNdcY =
        1.0 - (2.0 * static_cast<double>(pixelMaximumY) / static_cast<double>(grid.renderHeight));

    double const minimumSlopeX = minimumNdcX * horizontalScale;
    double const maximumSlopeX = maximumNdcX * horizontalScale;
    double const minimumSlopeY = minimumNdcY * tangentHalfFov;
    double const maximumSlopeY = maximumNdcY * tangentHalfFov;
    double const minimumDepth = static_cast<double>(interval.minimum);
    double const maximumDepth = static_cast<double>(interval.maximum);

    double const xValues[]{
        minimumSlopeX * minimumDepth,
        minimumSlopeX * maximumDepth,
        maximumSlopeX * minimumDepth,
        maximumSlopeX * maximumDepth,
    };
    double const yValues[]{
        minimumSlopeY * minimumDepth,
        minimumSlopeY * maximumDepth,
        maximumSlopeY * minimumDepth,
        maximumSlopeY * maximumDepth,
    };
    auto const xBounds = std::minmax_element(std::begin(xValues), std::end(xValues));
    auto const yBounds = std::minmax_element(std::begin(yValues), std::end(yValues));
    return ViewAabb{
        .minimumX = *xBounds.first,
        .minimumY = *yBounds.first,
        .minimumZ = minimumDepth,
        .maximumX = *xBounds.second,
        .maximumY = *yBounds.second,
        .maximumZ = maximumDepth,
    };
}

[[nodiscard]] double DistanceToInterval(double value, double minimum, double maximum) noexcept
{
    if (value < minimum)
    {
        return minimum - value;
    }
    if (value > maximum)
    {
        return value - maximum;
    }
    return 0.0;
}

[[nodiscard]] bool SphereOverlapsAabb(PointLightView light, ViewAabb const &bounds) noexcept
{
    double const distanceX =
        DistanceToInterval(static_cast<double>(light.position.x), bounds.minimumX, bounds.maximumX);
    double const distanceY =
        DistanceToInterval(static_cast<double>(light.position.y), bounds.minimumY, bounds.maximumY);
    double const distanceZ =
        DistanceToInterval(static_cast<double>(light.position.z), bounds.minimumZ, bounds.maximumZ);
    double const squaredDistance = (distanceX * distanceX) + (distanceY * distanceY) + (distanceZ * distanceZ);
    double const radius = static_cast<double>(light.radius);
    double const squaredRadius = radius * radius;
    double const scale = std::max({1.0, squaredDistance, squaredRadius});
    double const tolerance = 32.0 * std::numeric_limits<double>::epsilon() * scale;
    return squaredDistance <= squaredRadius + tolerance;
}

[[nodiscard]] std::expected<std::size_t, ContractError> ExpectedOverlapCount(std::uint32_t cellCount,
                                                                             std::uint32_t lightCount) noexcept
{
    std::uint64_t const count = static_cast<std::uint64_t>(cellCount) * lightCount;
    if (count > std::numeric_limits<std::size_t>::max())
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return static_cast<std::size_t>(count);
}

} // namespace

std::expected<TileGrid, ContractError> MakeTileGrid(std::uint32_t renderWidth, std::uint32_t renderHeight,
                                                    std::uint32_t tileWidth, std::uint32_t tileHeight) noexcept
{
    if (renderWidth == 0U || renderHeight == 0U || tileWidth == 0U || tileHeight == 0U)
    {
        return std::unexpected(ContractError::InvalidDimension);
    }

    std::uint32_t const tileCountX = CeilDivide(renderWidth, tileWidth);
    std::uint32_t const tileCountY = CeilDivide(renderHeight, tileHeight);
    std::uint64_t const tileCount = static_cast<std::uint64_t>(tileCountX) * tileCountY;
    if (tileCount > std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return TileGrid{
        .renderWidth = renderWidth,
        .renderHeight = renderHeight,
        .tileWidth = tileWidth,
        .tileHeight = tileHeight,
        .tileCountX = tileCountX,
        .tileCountY = tileCountY,
        .tileCount = static_cast<std::uint32_t>(tileCount),
    };
}

std::expected<std::uint32_t, ContractError> CheckedTileIndex(TileGrid const &grid, std::uint32_t tileX,
                                                             std::uint32_t tileY) noexcept
{
    if (auto const valid = ValidateGrid(grid); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (tileX >= grid.tileCountX || tileY >= grid.tileCountY)
    {
        return std::unexpected(ContractError::InvalidTileIndex);
    }
    return tileY * grid.tileCountX + tileX;
}

std::expected<TileCoordinate, ContractError> CheckedTileCoordinate(TileGrid const &grid,
                                                                   std::uint32_t tileIndex) noexcept
{
    if (auto const valid = ValidateGrid(grid); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (tileIndex >= grid.tileCount)
    {
        return std::unexpected(ContractError::InvalidTileIndex);
    }
    return TileCoordinate{
        .x = tileIndex % grid.tileCountX,
        .y = tileIndex / grid.tileCountX,
    };
}

std::expected<TileCoordinate, ContractError> TileForPixel(TileGrid const &grid, std::uint32_t pixelX,
                                                          std::uint32_t pixelY) noexcept
{
    if (auto const valid = ValidateGrid(grid); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (pixelX >= grid.renderWidth || pixelY >= grid.renderHeight)
    {
        return std::unexpected(ContractError::InvalidPixelCoordinate);
    }
    return TileCoordinate{
        .x = pixelX / grid.tileWidth,
        .y = pixelY / grid.tileHeight,
    };
}

std::expected<ClusterGrid, ContractError> MakeClusterGrid(TileGrid const &tiles, std::uint32_t sliceCount) noexcept
{
    if (auto const valid = ValidateGrid(tiles); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (sliceCount == 0U || sliceCount > kMaximumClusterSliceCount)
    {
        return std::unexpected(ContractError::InvalidSlicing);
    }

    std::uint64_t const clusterCount = static_cast<std::uint64_t>(tiles.tileCount) * sliceCount;
    if (clusterCount > std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return ClusterGrid{
        .tiles = tiles,
        .sliceCount = sliceCount,
        .clusterCount = static_cast<std::uint32_t>(clusterCount),
    };
}

std::expected<std::uint32_t, ContractError> CheckedClusterIndex(ClusterGrid const &grid,
                                                                ClusterCoordinate coordinate) noexcept
{
    if (auto const valid = ValidateClusterGrid(grid); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (coordinate.x >= grid.tiles.tileCountX || coordinate.y >= grid.tiles.tileCountY)
    {
        return std::unexpected(ContractError::InvalidTileIndex);
    }
    if (coordinate.z >= grid.sliceCount)
    {
        return std::unexpected(ContractError::InvalidSliceIndex);
    }

    std::uint32_t const tileIndex = coordinate.y * grid.tiles.tileCountX + coordinate.x;
    return coordinate.z * grid.tiles.tileCount + tileIndex;
}

std::expected<ClusterCoordinate, ContractError> CheckedClusterCoordinate(ClusterGrid const &grid,
                                                                         std::uint32_t clusterIndex) noexcept
{
    if (auto const valid = ValidateClusterGrid(grid); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (clusterIndex >= grid.clusterCount)
    {
        return std::unexpected(ContractError::InvalidCellIndex);
    }

    std::uint32_t const tileIndex = clusterIndex % grid.tiles.tileCount;
    return ClusterCoordinate{
        .x = tileIndex % grid.tiles.tileCountX,
        .y = tileIndex / grid.tiles.tileCountX,
        .z = clusterIndex / grid.tiles.tileCount,
    };
}

std::expected<ConservativeScreenBounds, ContractError> ComputeConservativeScreenBounds(PointLightView light,
                                                                                       PerspectiveProjection projection,
                                                                                       TileGrid const &grid) noexcept
{
    if (auto const validLight = ValidateLight(light); !validLight)
    {
        return std::unexpected(validLight.error());
    }
    if (auto const validProjection = ValidateProjection(projection); !validProjection)
    {
        return std::unexpected(validProjection.error());
    }
    if (auto const validGrid = ValidateGrid(grid); !validGrid)
    {
        return std::unexpected(validGrid.error());
    }

    double const lightDepth = static_cast<double>(light.position.z);
    double const radius = static_cast<double>(light.radius);
    double const nearestDepth = lightDepth - radius;
    double const farthestDepth = lightDepth + radius;
    if (farthestDepth <= 0.0)
    {
        return std::unexpected(ContractError::LightBehindCamera);
    }
    if (nearestDepth <= static_cast<double>(projection.nearPlane))
    {
        return ConservativeScreenBounds{
            .pixels = {0U, 0U, grid.renderWidth, grid.renderHeight},
            .tiles = {0U, 0U, grid.tileCountX, grid.tileCountY},
            .mode = ScreenBoundsMode::NearPlaneFullscreenFallback,
        };
    }

    double const minimumX = static_cast<double>(light.position.x) - radius;
    double const maximumX = static_cast<double>(light.position.x) + radius;
    double const minimumY = static_cast<double>(light.position.y) - radius;
    double const maximumY = static_cast<double>(light.position.y) + radius;
    double const xRatios[]{
        minimumX / nearestDepth,
        minimumX / farthestDepth,
        maximumX / nearestDepth,
        maximumX / farthestDepth,
    };
    double const yRatios[]{
        minimumY / nearestDepth,
        minimumY / farthestDepth,
        maximumY / nearestDepth,
        maximumY / farthestDepth,
    };
    auto const projectedX = std::minmax_element(std::begin(xRatios), std::end(xRatios));
    auto const projectedY = std::minmax_element(std::begin(yRatios), std::end(yRatios));

    double const tangentHalfFov = std::tan(static_cast<double>(projection.verticalFieldOfViewRadians) * 0.5);
    double const horizontalScale = tangentHalfFov * static_cast<double>(projection.aspectRatio);
    double const minimumNdcX = *projectedX.first / horizontalScale;
    double const maximumNdcX = *projectedX.second / horizontalScale;
    double const minimumNdcY = *projectedY.first / tangentHalfFov;
    double const maximumNdcY = *projectedY.second / tangentHalfFov;
    double const pixelMinimumX = (minimumNdcX + 1.0) * 0.5 * static_cast<double>(grid.renderWidth);
    double const pixelMaximumX = (maximumNdcX + 1.0) * 0.5 * static_cast<double>(grid.renderWidth);
    double const pixelMinimumY = (1.0 - maximumNdcY) * 0.5 * static_cast<double>(grid.renderHeight);
    double const pixelMaximumY = (1.0 - minimumNdcY) * 0.5 * static_cast<double>(grid.renderHeight);

    if (pixelMaximumX < 0.0 || pixelMinimumX > static_cast<double>(grid.renderWidth) || pixelMaximumY < 0.0 ||
        pixelMinimumY > static_cast<double>(grid.renderHeight))
    {
        return ConservativeScreenBounds{.mode = ScreenBoundsMode::Offscreen};
    }

    double const clippedMinimumX = std::clamp(pixelMinimumX, 0.0, static_cast<double>(grid.renderWidth));
    double const clippedMaximumX = std::clamp(pixelMaximumX, 0.0, static_cast<double>(grid.renderWidth));
    double const clippedMinimumY = std::clamp(pixelMinimumY, 0.0, static_cast<double>(grid.renderHeight));
    double const clippedMaximumY = std::clamp(pixelMaximumY, 0.0, static_cast<double>(grid.renderHeight));

    PixelRect const pixels{
        .minimumX = static_cast<std::uint32_t>(std::floor(clippedMinimumX)),
        .minimumY = static_cast<std::uint32_t>(std::floor(clippedMinimumY)),
        .maximumXExclusive = static_cast<std::uint32_t>(
            std::min(std::floor(clippedMaximumX) + 1.0, static_cast<double>(grid.renderWidth))),
        .maximumYExclusive = static_cast<std::uint32_t>(
            std::min(std::floor(clippedMaximumY) + 1.0, static_cast<double>(grid.renderHeight))),
    };
    if (pixels.IsEmpty())
    {
        return ConservativeScreenBounds{.mode = ScreenBoundsMode::Offscreen};
    }

    TileRect const tiles{
        .minimumX = pixels.minimumX / grid.tileWidth,
        .minimumY = pixels.minimumY / grid.tileHeight,
        .maximumXExclusive = std::min(CeilDivide(pixels.maximumXExclusive, grid.tileWidth), grid.tileCountX),
        .maximumYExclusive = std::min(CeilDivide(pixels.maximumYExclusive, grid.tileHeight), grid.tileCountY),
    };
    return ConservativeScreenBounds{
        .pixels = pixels,
        .tiles = tiles,
        .mode = ScreenBoundsMode::Projected,
    };
}

std::expected<TileDepthRange, ContractError> ReduceTileDepthRange(std::span<float const> deviceDepthSamples,
                                                                  PerspectiveProjection projection) noexcept
{
    if (deviceDepthSamples.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected(ContractError::TooManySamples);
    }
    if (auto const validProjection = ValidateProjection(projection); !validProjection)
    {
        return std::unexpected(validProjection.error());
    }

    TileDepthRange result{};
    float minimumDepth = std::numeric_limits<float>::max();
    float maximumDepth = std::numeric_limits<float>::lowest();
    for (float const deviceDepth : deviceDepthSamples)
    {
        if (!IsFinite(deviceDepth))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (deviceDepth < 0.0F || deviceDepth > 1.0F)
        {
            return std::unexpected(ContractError::InvalidDepth);
        }
        if (ch12::gbuffer::IsBackgroundDepth(deviceDepth, projection.depthConvention))
        {
            continue;
        }

        auto const viewDepth = ch12::gbuffer::ViewDepthFromDeviceDepth(deviceDepth, projection);
        if (!viewDepth)
        {
            return std::unexpected(MapDepthError(viewDepth.error()));
        }
        minimumDepth = std::min(minimumDepth, *viewDepth);
        maximumDepth = std::max(maximumDepth, *viewDepth);
        ++result.foregroundSampleCount;
    }

    if (result.foregroundSampleCount != 0U)
    {
        result.viewDepths = ViewDepthInterval{minimumDepth, maximumDepth};
    }
    return result;
}

std::expected<void, ContractError> ValidateLogDepthSlicing(LogDepthSlicing slicing) noexcept
{
    if (!IsFinite(slicing.nearDepth) || !IsFinite(slicing.farDepth))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (slicing.nearDepth <= 0.0F || slicing.farDepth <= slicing.nearDepth || slicing.sliceCount == 0U ||
        slicing.sliceCount > kMaximumClusterSliceCount)
    {
        return std::unexpected(ContractError::InvalidSlicing);
    }

    float previousBoundary = slicing.nearDepth;
    for (std::uint32_t boundaryIndex = 1U; boundaryIndex <= slicing.sliceCount; ++boundaryIndex)
    {
        float const boundary = SliceBoundaryUnchecked(slicing, boundaryIndex);
        if (!IsFinite(boundary) || boundary <= previousBoundary)
        {
            return std::unexpected(ContractError::InvalidSlicing);
        }
        previousBoundary = boundary;
    }
    return {};
}

std::expected<std::uint32_t, ContractError> DepthToSlice(float viewDepth, LogDepthSlicing slicing) noexcept
{
    if (!IsFinite(viewDepth))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (auto const valid = ValidateLogDepthSlicing(slicing); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (viewDepth < slicing.nearDepth || viewDepth > slicing.farDepth)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }
    if (viewDepth == slicing.farDepth)
    {
        return slicing.sliceCount - 1U;
    }

    std::uint32_t lowerSlice = 0U;
    std::uint32_t upperBoundary = slicing.sliceCount;
    while (lowerSlice + 1U < upperBoundary)
    {
        std::uint32_t const middleBoundary = lowerSlice + ((upperBoundary - lowerSlice) / 2U);
        if (viewDepth < SliceBoundaryUnchecked(slicing, middleBoundary))
        {
            upperBoundary = middleBoundary;
        }
        else
        {
            lowerSlice = middleBoundary;
        }
    }
    return lowerSlice;
}

std::expected<ViewDepthInterval, ContractError> SliceDepthBounds(std::uint32_t sliceIndex,
                                                                 LogDepthSlicing slicing) noexcept
{
    if (auto const valid = ValidateLogDepthSlicing(slicing); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (sliceIndex >= slicing.sliceCount)
    {
        return std::unexpected(ContractError::InvalidSliceIndex);
    }
    return ViewDepthInterval{
        .minimum = SliceBoundaryUnchecked(slicing, sliceIndex),
        .maximum = SliceBoundaryUnchecked(slicing, sliceIndex + 1U),
    };
}

std::expected<bool, ContractError> SphereOverlapsTileDepthRange(PointLightView light, TileGrid const &grid,
                                                                std::uint32_t tileX, std::uint32_t tileY,
                                                                TileDepthRange const &depthRange,
                                                                PerspectiveProjection projection) noexcept
{
    if (auto const validLight = ValidateLight(light); !validLight)
    {
        return std::unexpected(validLight.error());
    }
    if (auto const validProjection = ValidateProjection(projection); !validProjection)
    {
        return std::unexpected(validProjection.error());
    }
    if (auto const validGrid = ValidateGrid(grid); !validGrid)
    {
        return std::unexpected(validGrid.error());
    }
    if (tileX >= grid.tileCountX || tileY >= grid.tileCountY)
    {
        return std::unexpected(ContractError::InvalidTileIndex);
    }
    if (depthRange.IsEmpty())
    {
        if (depthRange.foregroundSampleCount != 0U)
        {
            return std::unexpected(ContractError::InvalidDepthRange);
        }
        return false;
    }
    if (depthRange.foregroundSampleCount == 0U)
    {
        return std::unexpected(ContractError::InvalidDepthRange);
    }

    auto const bounds = MakeTileViewAabb(grid, tileX, tileY, *depthRange.viewDepths, projection);
    if (!bounds)
    {
        return std::unexpected(bounds.error());
    }
    return SphereOverlapsAabb(light, *bounds);
}

std::expected<bool, ContractError> SphereOverlapsCluster(PointLightView light, TileGrid const &grid,
                                                         std::uint32_t tileX, std::uint32_t tileY,
                                                         std::uint32_t sliceIndex, LogDepthSlicing slicing,
                                                         PerspectiveProjection projection) noexcept
{
    if (auto const validLight = ValidateLight(light); !validLight)
    {
        return std::unexpected(validLight.error());
    }
    if (auto const validProjection = ValidateProjection(projection); !validProjection)
    {
        return std::unexpected(validProjection.error());
    }
    if (auto const validSlicing = ValidateLogDepthSlicing(slicing); !validSlicing)
    {
        return std::unexpected(validSlicing.error());
    }
    if (slicing.nearDepth < projection.nearPlane || slicing.farDepth > projection.farPlane)
    {
        return std::unexpected(ContractError::InvalidDepthRange);
    }

    auto const interval = SliceDepthBounds(sliceIndex, slicing);
    if (!interval)
    {
        return std::unexpected(interval.error());
    }
    auto const bounds = MakeTileViewAabb(grid, tileX, tileY, *interval, projection);
    if (!bounds)
    {
        return std::unexpected(bounds.error());
    }
    return SphereOverlapsAabb(light, *bounds);
}

std::expected<BoundedLightLists, ContractError> BuildBoundedLightLists(std::uint32_t cellCount,
                                                                       std::uint32_t lightCount,
                                                                       std::span<std::uint8_t const> cellMajorOverlaps,
                                                                       std::uint64_t capacity)
{
    auto const expectedOverlapCount = ExpectedOverlapCount(cellCount, lightCount);
    if (!expectedOverlapCount)
    {
        return std::unexpected(expectedOverlapCount.error());
    }
    if (cellMajorOverlaps.size() != *expectedOverlapCount)
    {
        return std::unexpected(ContractError::InvalidOverlapData);
    }
    if (capacity > std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected(ContractError::InvalidCapacity);
    }
    if (std::ranges::any_of(cellMajorOverlaps, [](std::uint8_t overlaps) { return overlaps > 1U; }))
    {
        return std::unexpected(ContractError::InvalidOverlapData);
    }

    BoundedLightLists result{};
    result.cells.resize(cellCount);
    result.lightIndices.reserve(std::min<std::size_t>(cellMajorOverlaps.size(), static_cast<std::size_t>(capacity)));

    for (std::uint32_t cellIndex = 0U; cellIndex < cellCount; ++cellIndex)
    {
        CellLightRange &range = result.cells[cellIndex];
        range.offset = static_cast<std::uint32_t>(result.lightIndices.size());
        std::size_t const rowOffset = static_cast<std::size_t>(cellIndex) * lightCount;
        for (std::uint32_t lightIndex = 0U; lightIndex < lightCount; ++lightIndex)
        {
            std::uint8_t const overlaps = cellMajorOverlaps[rowOffset + lightIndex];
            if (overlaps == 0U)
            {
                continue;
            }

            ++range.attemptedCount;
            ++result.statistics.attemptedCount;
            if (result.lightIndices.size() < capacity)
            {
                result.lightIndices.push_back(lightIndex);
                ++range.count;
                ++result.statistics.emittedCount;
            }
            else
            {
                ++range.overflowCount;
                ++result.statistics.overflowCount;
            }
        }
    }
    return result;
}

std::expected<void, ContractError> ValidateBoundedLightLists(BoundedLightLists const &lists,
                                                             std::uint32_t lightCount) noexcept
{
    if (lists.cells.size() > std::numeric_limits<std::uint32_t>::max() ||
        lists.lightIndices.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected(ContractError::MalformedLightList);
    }

    std::uint64_t attemptedCount = 0U;
    std::uint64_t emittedCount = 0U;
    std::uint64_t overflowCount = 0U;
    std::uint32_t expectedOffset = 0U;
    for (CellLightRange const range : lists.cells)
    {
        std::uint64_t const rangeEnd = static_cast<std::uint64_t>(range.offset) + range.count;
        std::uint64_t const rangeAttempted = static_cast<std::uint64_t>(range.count) + range.overflowCount;
        if (range.offset != expectedOffset || rangeEnd > lists.lightIndices.size() ||
            range.attemptedCount != rangeAttempted)
        {
            return std::unexpected(ContractError::MalformedLightList);
        }

        std::optional<std::uint32_t> previousLight{};
        for (std::uint64_t flatIndex = range.offset; flatIndex < rangeEnd; ++flatIndex)
        {
            std::uint32_t const lightIndex = lists.lightIndices[static_cast<std::size_t>(flatIndex)];
            if (lightIndex >= lightCount)
            {
                return std::unexpected(ContractError::InvalidLightIndex);
            }
            if (previousLight && lightIndex <= *previousLight)
            {
                return std::unexpected(ContractError::MalformedLightList);
            }
            previousLight = lightIndex;
        }

        expectedOffset += range.count;
        attemptedCount += range.attemptedCount;
        emittedCount += range.count;
        overflowCount += range.overflowCount;
    }

    if (expectedOffset != lists.lightIndices.size() || attemptedCount != lists.statistics.attemptedCount ||
        emittedCount != lists.statistics.emittedCount || overflowCount != lists.statistics.overflowCount ||
        attemptedCount != emittedCount + overflowCount)
    {
        return std::unexpected(ContractError::MalformedLightList);
    }
    return {};
}

std::expected<ScalarReferenceComparison, ContractError> CompareCellAccumulation(
    std::uint32_t cellIndex, std::uint32_t lightCount, std::span<std::uint8_t const> lightOverlaps,
    std::span<float const> lightContributions, BoundedLightLists const &lists) noexcept
{
    if (lightOverlaps.size() != lightCount || lightContributions.size() != lightCount)
    {
        return std::unexpected(ContractError::InvalidOverlapData);
    }
    if (auto const validLists = ValidateBoundedLightLists(lists, lightCount); !validLists)
    {
        return std::unexpected(validLists.error());
    }
    if (cellIndex >= lists.cells.size())
    {
        return std::unexpected(ContractError::InvalidCellIndex);
    }

    std::uint32_t bruteForceCount = 0U;
    for (std::uint32_t lightIndex = 0U; lightIndex < lightCount; ++lightIndex)
    {
        std::uint8_t const overlaps = lightOverlaps[lightIndex];
        float const contribution = lightContributions[lightIndex];
        if (overlaps > 1U)
        {
            return std::unexpected(ContractError::InvalidOverlapData);
        }
        if (!IsFinite(contribution))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (overlaps == 0U)
        {
            continue;
        }
        ++bruteForceCount;
    }

    CellLightRange const range = lists.cells[cellIndex];
    if (range.overflowCount != 0U)
    {
        return std::unexpected(ContractError::IncompleteLightList);
    }

    ScalarReferenceComparison comparison{.equivalent = true};
    for (std::uint32_t lightIndex = 0U; lightIndex < lightCount; ++lightIndex)
    {
        if (lightOverlaps[lightIndex] != 0U)
        {
            comparison.bruteForceAccumulation += static_cast<double>(lightContributions[lightIndex]);
        }
    }

    std::uint32_t expectedLightIndex = 0U;
    for (std::uint32_t listedOffset = 0U; listedOffset < range.count; ++listedOffset)
    {
        while (expectedLightIndex < lightCount && lightOverlaps[expectedLightIndex] == 0U)
        {
            ++expectedLightIndex;
        }
        std::uint32_t const listedLightIndex = lists.lightIndices[range.offset + listedOffset];
        comparison.listedAccumulation += static_cast<double>(lightContributions[listedLightIndex]);
        if (expectedLightIndex >= lightCount || listedLightIndex != expectedLightIndex)
        {
            comparison.equivalent = false;
        }
        if (expectedLightIndex < lightCount)
        {
            ++expectedLightIndex;
        }
    }
    while (expectedLightIndex < lightCount && lightOverlaps[expectedLightIndex] == 0U)
    {
        ++expectedLightIndex;
    }

    comparison.equivalent = comparison.equivalent && expectedLightIndex == lightCount &&
                            bruteForceCount == range.attemptedCount &&
                            comparison.listedAccumulation == comparison.bruteForceAccumulation;
    return comparison;
}

} // namespace ch14::clustered_lighting
