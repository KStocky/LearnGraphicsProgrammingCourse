#include "ChapterGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ch07::shadows
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
        float const theta = kPi * static_cast<float>(latitude) / static_cast<float>(latitudeSegments);
        float const sinTheta = std::sin(theta);
        float const cosTheta = std::cos(theta);
        for (std::uint32_t longitude = 0U; longitude < longitudeSegments; ++longitude)
        {
            float const phi = 2.0F * kPi * static_cast<float>(longitude) / static_cast<float>(longitudeSegments);
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

std::expected<GeometryMesh, GeometryError> GenerateBox(Float3 dimensions)
{
    if (!IsFinitePositive(dimensions.x) || !IsFinitePositive(dimensions.y) || !IsFinitePositive(dimensions.z))
    {
        return std::unexpected(GeometryError::InvalidDimensions);
    }

    float const x = dimensions.x * 0.5F;
    float const y = dimensions.y * 0.5F;
    float const z = dimensions.z * 0.5F;
    GeometryMesh mesh{};
    mesh.vertices = {
        {{-x, -y, -z}, {0.0F, 0.0F, -1.0F}}, {{-x, y, -z}, {0.0F, 0.0F, -1.0F}}, {{x, y, -z}, {0.0F, 0.0F, -1.0F}},
        {{x, -y, -z}, {0.0F, 0.0F, -1.0F}},  {{x, -y, z}, {0.0F, 0.0F, 1.0F}},   {{x, y, z}, {0.0F, 0.0F, 1.0F}},
        {{-x, y, z}, {0.0F, 0.0F, 1.0F}},    {{-x, -y, z}, {0.0F, 0.0F, 1.0F}},  {{-x, -y, z}, {-1.0F, 0.0F, 0.0F}},
        {{-x, y, z}, {-1.0F, 0.0F, 0.0F}},   {{-x, y, -z}, {-1.0F, 0.0F, 0.0F}}, {{-x, -y, -z}, {-1.0F, 0.0F, 0.0F}},
        {{x, -y, -z}, {1.0F, 0.0F, 0.0F}},   {{x, y, -z}, {1.0F, 0.0F, 0.0F}},   {{x, y, z}, {1.0F, 0.0F, 0.0F}},
        {{x, -y, z}, {1.0F, 0.0F, 0.0F}},    {{-x, y, -z}, {0.0F, 1.0F, 0.0F}},  {{-x, y, z}, {0.0F, 1.0F, 0.0F}},
        {{x, y, z}, {0.0F, 1.0F, 0.0F}},     {{x, y, -z}, {0.0F, 1.0F, 0.0F}},   {{-x, -y, z}, {0.0F, -1.0F, 0.0F}},
        {{-x, -y, -z}, {0.0F, -1.0F, 0.0F}}, {{x, -y, -z}, {0.0F, -1.0F, 0.0F}}, {{x, -y, z}, {0.0F, -1.0F, 0.0F}},
    };
    for (std::uint32_t face = 0U; face < 6U; ++face)
    {
        std::uint32_t const first = face * 4U;
        mesh.indices.insert(mesh.indices.end(), {first, first + 1U, first + 2U, first, first + 2U, first + 3U});
    }
    return mesh;
}

std::expected<GeometryMesh, GeometryError> TransformGeometry(GeometryMesh const &mesh, Matrix4 const &transform)
{
    for (auto const &row : transform.elements)
    {
        for (float const value : row)
        {
            if (!std::isfinite(value))
            {
                return std::unexpected(GeometryError::InvalidTransform);
            }
        }
    }
    if (transform.elements[0][3] != 0.0F || transform.elements[1][3] != 0.0F || transform.elements[2][3] != 0.0F ||
        transform.elements[3][3] != 1.0F)
    {
        return std::unexpected(GeometryError::InvalidTransform);
    }

    double linearScale = 0.0;
    for (std::size_t row = 0U; row < 3U; ++row)
    {
        for (std::size_t column = 0U; column < 3U; ++column)
        {
            linearScale = (std::max)(linearScale, std::fabs(static_cast<double>(transform.elements[row][column])));
        }
    }
    if (linearScale == 0.0)
    {
        return std::unexpected(GeometryError::SingularTransform);
    }

    double const a = static_cast<double>(transform.elements[0][0]) / linearScale;
    double const b = static_cast<double>(transform.elements[0][1]) / linearScale;
    double const c = static_cast<double>(transform.elements[0][2]) / linearScale;
    double const d = static_cast<double>(transform.elements[1][0]) / linearScale;
    double const e = static_cast<double>(transform.elements[1][1]) / linearScale;
    double const f = static_cast<double>(transform.elements[1][2]) / linearScale;
    double const g = static_cast<double>(transform.elements[2][0]) / linearScale;
    double const h = static_cast<double>(transform.elements[2][1]) / linearScale;
    double const i = static_cast<double>(transform.elements[2][2]) / linearScale;
    double const cofactors[3][3]{
        {(e * i) - (f * h), (f * g) - (d * i), (d * h) - (e * g)},
        {(c * h) - (b * i), (a * i) - (c * g), (b * g) - (a * h)},
        {(b * f) - (c * e), (c * d) - (a * f), (a * e) - (b * d)},
    };
    double const determinant = (a * cofactors[0][0]) + (b * cofactors[0][1]) + (c * cofactors[0][2]);
    double const absoluteDeterminant = std::fabs(determinant);
    if (absoluteDeterminant == 0.0)
    {
        return std::unexpected(GeometryError::SingularTransform);
    }

    double const linearNorm = (std::max)({
        std::fabs(a) + std::fabs(b) + std::fabs(c),
        std::fabs(d) + std::fabs(e) + std::fabs(f),
        std::fabs(g) + std::fabs(h) + std::fabs(i),
    });
    double const adjugateNorm = (std::max)({
        std::fabs(cofactors[0][0]) + std::fabs(cofactors[1][0]) + std::fabs(cofactors[2][0]),
        std::fabs(cofactors[0][1]) + std::fabs(cofactors[1][1]) + std::fabs(cofactors[2][1]),
        std::fabs(cofactors[0][2]) + std::fabs(cofactors[1][2]) + std::fabs(cofactors[2][2]),
    });
    double const conditionNumber = linearNorm * adjugateNorm / absoluteDeterminant;
    double constexpr maximumConditionNumber = 1.0 / static_cast<double>(std::numeric_limits<float>::epsilon());
    if (!std::isfinite(conditionNumber) || conditionNumber >= maximumConditionNumber)
    {
        return std::unexpected(GeometryError::SingularTransform);
    }

    GeometryMesh result = mesh;
    for (GeometryVertex &vertex : result.vertices)
    {
        std::expected<Float4, ShadowError> const position = TransformPoint(vertex.position, transform);
        if (!position)
        {
            return std::unexpected(GeometryError::InvalidTransform);
        }
        vertex.position = {position->x, position->y, position->z};
        double transformedNormal[3]{
            (static_cast<double>(vertex.normal.x) * cofactors[0][0]) +
                (static_cast<double>(vertex.normal.y) * cofactors[1][0]) +
                (static_cast<double>(vertex.normal.z) * cofactors[2][0]),
            (static_cast<double>(vertex.normal.x) * cofactors[0][1]) +
                (static_cast<double>(vertex.normal.y) * cofactors[1][1]) +
                (static_cast<double>(vertex.normal.z) * cofactors[2][1]),
            (static_cast<double>(vertex.normal.x) * cofactors[0][2]) +
                (static_cast<double>(vertex.normal.y) * cofactors[1][2]) +
                (static_cast<double>(vertex.normal.z) * cofactors[2][2]),
        };
        double const normalScale = (std::max)({
            std::fabs(transformedNormal[0]),
            std::fabs(transformedNormal[1]),
            std::fabs(transformedNormal[2]),
        });
        if (!std::isfinite(normalScale) || normalScale == 0.0)
        {
            return std::unexpected(GeometryError::SingularTransform);
        }
        for (double &component : transformedNormal)
        {
            component /= normalScale;
        }
        double const inverseLength = std::copysign(1.0 / std::sqrt((transformedNormal[0] * transformedNormal[0]) +
                                                                   (transformedNormal[1] * transformedNormal[1]) +
                                                                   (transformedNormal[2] * transformedNormal[2])),
                                                   determinant);
        vertex.normal = {
            static_cast<float>(transformedNormal[0] * inverseLength),
            static_cast<float>(transformedNormal[1] * inverseLength),
            static_cast<float>(transformedNormal[2] * inverseLength),
        };
    }
    if (determinant < 0.0F)
    {
        for (std::size_t index = 0U; index + 2U < result.indices.size(); index += 3U)
        {
            std::swap(result.indices[index + 1U], result.indices[index + 2U]);
        }
    }
    return result;
}

} // namespace ch07::shadows
