#pragma once

#include "ChapterTypes.hpp"

#include <cstdint>
#include <expected>
#include <vector>

namespace ch05::lighting
{

struct GeometryVertex final
{
    Float3 position{};
    Float3 normal{};
};

struct GeometryMesh final
{
    std::vector<GeometryVertex> vertices;
    std::vector<std::uint32_t> indices;
};

enum class GeometryError : std::uint8_t
{
    InvalidRadius = 0,
    InvalidDimensions,
    InsufficientSegments,
    CountOverflow,
};

[[nodiscard]] std::expected<GeometryMesh, GeometryError> GenerateSphere(float radius, std::uint32_t latitudeSegments,
                                                                        std::uint32_t longitudeSegments);
[[nodiscard]] std::expected<GeometryMesh, GeometryError> GeneratePlane(float width, float depth,
                                                                       std::uint32_t widthSegments,
                                                                       std::uint32_t depthSegments);

} // namespace ch05::lighting
