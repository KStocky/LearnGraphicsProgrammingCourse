#include "ChapterGeometry.hpp"

#include <cmath>
#include <cstddef>
#include <limits>

namespace ch06::surface_frames
{
namespace
{

inline constexpr float kPi = 3.14159265358979323846F;
inline constexpr std::size_t kMaximumVertexCount = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaximumIndexCount = 24U * 1024U * 1024U;

[[nodiscard]] bool IsFinitePositive(float value) noexcept
{
    return std::isfinite(value) && value > 0.0F;
}

[[nodiscard]] Float3 SphereNormal(std::uint32_t latitude, std::uint32_t longitude, std::uint32_t latitudeSegments,
                                  std::uint32_t longitudeSegments) noexcept
{
    float const theta = kPi * static_cast<float>(latitude) / static_cast<float>(latitudeSegments);
    float const phi = 2.0F * kPi * static_cast<float>(longitude) / static_cast<float>(longitudeSegments);
    float const sinTheta = std::sin(theta);
    return {sinTheta * std::cos(phi), std::cos(theta), sinTheta * std::sin(phi)};
}

[[nodiscard]] Float3 Scale(Float3 value, float scalar) noexcept
{
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

void AddTriangle(std::vector<TangentInputVertex> &vertices, std::vector<std::uint32_t> &indices,
                 TangentInputVertex first, TangentInputVertex second, TangentInputVertex third)
{
    std::uint32_t const base = static_cast<std::uint32_t>(vertices.size());
    vertices.insert(vertices.end(), {first, second, third});
    indices.insert(indices.end(), {base, base + 1U, base + 2U});
}

void AddQuad(std::vector<TangentInputVertex> &vertices, std::vector<std::uint32_t> &indices,
             TangentInputVertex upperLeft, TangentInputVertex upperRight, TangentInputVertex lowerLeft,
             TangentInputVertex lowerRight)
{
    std::uint32_t const base = static_cast<std::uint32_t>(vertices.size());
    vertices.insert(vertices.end(), {upperLeft, upperRight, lowerLeft, lowerRight});
    indices.insert(indices.end(), {base, base + 1U, base + 2U, base + 1U, base + 3U, base + 2U});
}

[[nodiscard]] std::expected<GeometryMesh, GeometryError> FinishMesh(std::vector<TangentInputVertex> inputVertices,
                                                                    std::vector<std::uint32_t> indices)
{
    std::expected<std::vector<Float4>, SurfaceFrameError> tangents = BuildMeshTangents(inputVertices, indices);
    if (!tangents)
    {
        return std::unexpected(GeometryError::TangentBuildFailed);
    }

    GeometryMesh mesh{};
    mesh.vertices.reserve(inputVertices.size());
    for (std::size_t index = 0U; index < inputVertices.size(); ++index)
    {
        TangentInputVertex const &input = inputVertices[index];
        mesh.vertices.push_back({input.position, input.normal, input.textureCoordinates, (*tangents)[index]});
    }
    mesh.indices = std::move(indices);
    return mesh;
}

} // namespace

std::expected<GeometryMesh, GeometryError> GenerateUvSphere(float radius, std::uint32_t latitudeSegments,
                                                            std::uint32_t longitudeSegments)
{
    if (!IsFinitePositive(radius))
    {
        return std::unexpected(GeometryError::InvalidRadius);
    }
    if (latitudeSegments < 2U || longitudeSegments < 4U || (longitudeSegments % 2U) != 0U)
    {
        return std::unexpected(GeometryError::InsufficientSegments);
    }

    std::uint64_t const triangleCount =
        2ULL * static_cast<std::uint64_t>(longitudeSegments) * (static_cast<std::uint64_t>(latitudeSegments) - 1ULL);
    std::uint64_t const middleQuadCount =
        static_cast<std::uint64_t>(longitudeSegments) * (static_cast<std::uint64_t>(latitudeSegments) - 2ULL);
    std::uint64_t const vertexCount = (6ULL * static_cast<std::uint64_t>(longitudeSegments)) + (4ULL * middleQuadCount);
    std::uint64_t const indexCount = triangleCount * 3ULL;
    if (vertexCount > kMaximumVertexCount || indexCount > kMaximumIndexCount ||
        vertexCount > std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected(GeometryError::CountOverflow);
    }

    std::vector<TangentInputVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(static_cast<std::size_t>(vertexCount));
    indices.reserve(static_cast<std::size_t>(indexCount));

    for (std::uint32_t longitude = 0U; longitude < longitudeSegments; ++longitude)
    {
        bool const mirrored = longitude >= longitudeSegments / 2U;
        std::uint32_t const nextLongitude = longitude + 1U;
        Float2 const leftTop = mirrored ? Float2{1.0F, 0.0F} : Float2{0.0F, 0.0F};
        Float2 const rightTop = mirrored ? Float2{0.0F, 0.0F} : Float2{1.0F, 0.0F};
        Float2 const leftBottom = mirrored ? Float2{1.0F, 1.0F} : Float2{0.0F, 1.0F};
        Float2 const rightBottom = mirrored ? Float2{0.0F, 1.0F} : Float2{1.0F, 1.0F};

        Float3 const topNormal{0.0F, 1.0F, 0.0F};
        Float3 const firstRingLeft = SphereNormal(1U, longitude, latitudeSegments, longitudeSegments);
        Float3 const firstRingRight = SphereNormal(1U, nextLongitude, latitudeSegments, longitudeSegments);
        AddTriangle(vertices, indices, {Scale(topNormal, radius), topNormal, {0.5F, 0.0F}},
                    {Scale(firstRingRight, radius), firstRingRight, rightBottom},
                    {Scale(firstRingLeft, radius), firstRingLeft, leftBottom});

        for (std::uint32_t latitude = 1U; latitude + 1U < latitudeSegments; ++latitude)
        {
            Float3 const upperLeft = SphereNormal(latitude, longitude, latitudeSegments, longitudeSegments);
            Float3 const upperRight = SphereNormal(latitude, nextLongitude, latitudeSegments, longitudeSegments);
            Float3 const lowerLeft = SphereNormal(latitude + 1U, longitude, latitudeSegments, longitudeSegments);
            Float3 const lowerRight = SphereNormal(latitude + 1U, nextLongitude, latitudeSegments, longitudeSegments);
            AddQuad(vertices, indices, {Scale(upperLeft, radius), upperLeft, leftTop},
                    {Scale(upperRight, radius), upperRight, rightTop},
                    {Scale(lowerLeft, radius), lowerLeft, leftBottom},
                    {Scale(lowerRight, radius), lowerRight, rightBottom});
        }

        Float3 const lastRingLeft = SphereNormal(latitudeSegments - 1U, longitude, latitudeSegments, longitudeSegments);
        Float3 const lastRingRight =
            SphereNormal(latitudeSegments - 1U, nextLongitude, latitudeSegments, longitudeSegments);
        Float3 const bottomNormal{0.0F, -1.0F, 0.0F};
        AddTriangle(vertices, indices, {Scale(lastRingLeft, radius), lastRingLeft, leftTop},
                    {Scale(lastRingRight, radius), lastRingRight, rightTop},
                    {Scale(bottomNormal, radius), bottomNormal, {0.5F, 1.0F}});
    }

    return FinishMesh(std::move(vertices), std::move(indices));
}

std::expected<GeometryMesh, GeometryError> GenerateGroundPlane(float width, float depth)
{
    if (!IsFinitePositive(width) || !IsFinitePositive(depth))
    {
        return std::unexpected(GeometryError::InvalidDimensions);
    }

    float const halfWidth = width * 0.5F;
    float const halfDepth = depth * 0.5F;
    Float3 const normal{0.0F, 1.0F, 0.0F};
    std::vector<TangentInputVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(8U);
    indices.reserve(12U);
    AddQuad(vertices, indices, {{-halfWidth, 0.0F, -halfDepth}, normal, {0.0F, 0.0F}},
            {{0.0F, 0.0F, -halfDepth}, normal, {1.0F, 0.0F}}, {{-halfWidth, 0.0F, halfDepth}, normal, {0.0F, 1.0F}},
            {{0.0F, 0.0F, halfDepth}, normal, {1.0F, 1.0F}});
    AddQuad(vertices, indices, {{0.0F, 0.0F, -halfDepth}, normal, {1.0F, 0.0F}},
            {{halfWidth, 0.0F, -halfDepth}, normal, {0.0F, 0.0F}}, {{0.0F, 0.0F, halfDepth}, normal, {1.0F, 1.0F}},
            {{halfWidth, 0.0F, halfDepth}, normal, {0.0F, 1.0F}});
    for (std::size_t index = 0U; index < indices.size(); index += 3U)
    {
        std::swap(indices[index + 1U], indices[index + 2U]);
    }
    return FinishMesh(std::move(vertices), std::move(indices));
}

} // namespace ch06::surface_frames
