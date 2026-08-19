#pragma once

#include "SurfaceFrame.hpp"

#include <cstdint>
#include <expected>
#include <vector>

namespace ch06::surface_frames
{

struct GeometryMesh final
{
    std::vector<SurfaceVertex> vertices;
    std::vector<std::uint32_t> indices;
};

enum class GeometryError : std::uint8_t
{
    InvalidRadius = 0,
    InvalidDimensions,
    InsufficientSegments,
    CountOverflow,
    TangentBuildFailed,
};

[[nodiscard]] std::expected<GeometryMesh, GeometryError> GenerateUvSphere(float radius, std::uint32_t latitudeSegments,
                                                                          std::uint32_t longitudeSegments);
[[nodiscard]] std::expected<GeometryMesh, GeometryError> GenerateGroundPlane(float width, float depth);

} // namespace ch06::surface_frames
