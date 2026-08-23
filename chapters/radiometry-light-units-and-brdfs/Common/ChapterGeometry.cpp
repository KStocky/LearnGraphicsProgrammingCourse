#include "ChapterGeometry.hpp"

#include <cmath>
#include <cstddef>

namespace ch05::lighting
{
namespace
{

inline constexpr float kGeometryPi = 3.14159265358979323846F;
inline constexpr std::size_t kMaximumVertexCount = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaximumIndexCount = 24U * 1024U * 1024U;

[[nodiscard]] bool IsFinitePositive(float value) noexcept
{
    return std::isfinite(value) && value > 0.0F;
}

[[nodiscard]] std::uint32_t SphereRingVertex(std::uint32_t ring, std::uint32_t longitude,
                                             std::uint32_t longitudeSegments) noexcept
{
    return 1U + (ring * longitudeSegments) + longitude;
}

} // namespace

std::expected<GeometryMesh, GeometryError> GenerateSphere(float radius, std::uint32_t latitudeSegments,
                                                          std::uint32_t longitudeSegments)
{
    if (!IsFinitePositive(radius))
    {
        return std::unexpected(GeometryError::InvalidRadius);
    }
    if (latitudeSegments < 2U || longitudeSegments < 3U)
    {
        return std::unexpected(GeometryError::InsufficientSegments);
    }

    std::uint64_t const ringCount = static_cast<std::uint64_t>(latitudeSegments) - 1U;
    std::uint64_t const longitudeCount = longitudeSegments;
    if (ringCount > (kMaximumVertexCount - 2U) / longitudeCount ||
        ringCount > kMaximumIndexCount / (6U * longitudeCount))
    {
        return std::unexpected(GeometryError::CountOverflow);
    }
    std::uint64_t const vertexCount = 2U + (ringCount * longitudeCount);
    std::uint64_t const indexCount = 6U * longitudeCount * ringCount;

    GeometryMesh mesh{};
    mesh.vertices.reserve(static_cast<std::size_t>(vertexCount));
    mesh.indices.reserve(static_cast<std::size_t>(indexCount));
    mesh.vertices.push_back({{0.0F, radius, 0.0F}, {0.0F, 1.0F, 0.0F}});

    for (std::uint32_t latitude = 1U; latitude < latitudeSegments; ++latitude)
    {
        float const theta = kGeometryPi * static_cast<float>(latitude) / static_cast<float>(latitudeSegments);
        float const sinTheta = std::sin(theta);
        float const cosTheta = std::cos(theta);
        for (std::uint32_t longitude = 0U; longitude < longitudeSegments; ++longitude)
        {
            float const phi =
                2.0F * kGeometryPi * static_cast<float>(longitude) / static_cast<float>(longitudeSegments);
            Float3 const normal{sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi)};
            mesh.vertices.push_back({{radius * normal.x, radius * normal.y, radius * normal.z}, normal});
        }
    }

    std::uint32_t const bottomVertex = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({{0.0F, -radius, 0.0F}, {0.0F, -1.0F, 0.0F}});

    for (std::uint32_t longitude = 0U; longitude < longitudeSegments; ++longitude)
    {
        std::uint32_t const nextLongitude = (longitude + 1U) % longitudeSegments;
        mesh.indices.push_back(0U);
        mesh.indices.push_back(SphereRingVertex(0U, nextLongitude, longitudeSegments));
        mesh.indices.push_back(SphereRingVertex(0U, longitude, longitudeSegments));
    }

    for (std::uint32_t ring = 0U; ring + 1U < latitudeSegments - 1U; ++ring)
    {
        for (std::uint32_t longitude = 0U; longitude < longitudeSegments; ++longitude)
        {
            std::uint32_t const nextLongitude = (longitude + 1U) % longitudeSegments;
            std::uint32_t const upper = SphereRingVertex(ring, longitude, longitudeSegments);
            std::uint32_t const upperNext = SphereRingVertex(ring, nextLongitude, longitudeSegments);
            std::uint32_t const lower = SphereRingVertex(ring + 1U, longitude, longitudeSegments);
            std::uint32_t const lowerNext = SphereRingVertex(ring + 1U, nextLongitude, longitudeSegments);

            mesh.indices.insert(mesh.indices.end(), {upper, upperNext, lower, upperNext, lowerNext, lower});
        }
    }

    std::uint32_t const lastRing = latitudeSegments - 2U;
    for (std::uint32_t longitude = 0U; longitude < longitudeSegments; ++longitude)
    {
        std::uint32_t const nextLongitude = (longitude + 1U) % longitudeSegments;
        mesh.indices.push_back(SphereRingVertex(lastRing, longitude, longitudeSegments));
        mesh.indices.push_back(SphereRingVertex(lastRing, nextLongitude, longitudeSegments));
        mesh.indices.push_back(bottomVertex);
    }

    return mesh;
}

std::expected<GeometryMesh, GeometryError> GeneratePlane(float width, float depth, std::uint32_t widthSegments,
                                                         std::uint32_t depthSegments)
{
    if (!IsFinitePositive(width) || !IsFinitePositive(depth))
    {
        return std::unexpected(GeometryError::InvalidDimensions);
    }
    if (widthSegments < 1U || depthSegments < 1U)
    {
        return std::unexpected(GeometryError::InsufficientSegments);
    }

    std::uint64_t const verticesWide = static_cast<std::uint64_t>(widthSegments) + 1U;
    std::uint64_t const verticesDeep = static_cast<std::uint64_t>(depthSegments) + 1U;
    std::uint64_t const widthSegmentCount = widthSegments;
    std::uint64_t const depthSegmentCount = depthSegments;
    if (verticesWide > kMaximumVertexCount / verticesDeep ||
        widthSegmentCount > kMaximumIndexCount / (6U * depthSegmentCount))
    {
        return std::unexpected(GeometryError::CountOverflow);
    }
    std::uint64_t const vertexCount = verticesWide * verticesDeep;
    std::uint64_t const indexCount = 6U * widthSegmentCount * depthSegmentCount;

    GeometryMesh mesh{};
    mesh.vertices.reserve(static_cast<std::size_t>(vertexCount));
    mesh.indices.reserve(static_cast<std::size_t>(indexCount));

    for (std::uint32_t z = 0U; z <= depthSegments; ++z)
    {
        float const zPosition = (-0.5F * depth) + (depth * static_cast<float>(z) / static_cast<float>(depthSegments));
        for (std::uint32_t x = 0U; x <= widthSegments; ++x)
        {
            float const xPosition =
                (-0.5F * width) + (width * static_cast<float>(x) / static_cast<float>(widthSegments));
            mesh.vertices.push_back({{xPosition, 0.0F, zPosition}, {0.0F, 1.0F, 0.0F}});
        }
    }

    std::uint32_t const rowStride = widthSegments + 1U;
    for (std::uint32_t z = 0U; z < depthSegments; ++z)
    {
        for (std::uint32_t x = 0U; x < widthSegments; ++x)
        {
            std::uint32_t const nearLeft = (z * rowStride) + x;
            std::uint32_t const nearRight = nearLeft + 1U;
            std::uint32_t const farLeft = nearLeft + rowStride;
            std::uint32_t const farRight = farLeft + 1U;
            mesh.indices.insert(mesh.indices.end(), {nearLeft, farLeft, nearRight, farLeft, farRight, nearRight});
        }
    }

    return mesh;
}

} // namespace ch05::lighting
