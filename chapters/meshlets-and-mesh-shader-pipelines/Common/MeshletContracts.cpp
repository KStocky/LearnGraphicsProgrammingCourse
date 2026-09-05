#include "MeshletContracts.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace ch22::meshlets
{
namespace
{

struct Vec3 final
{
    double x{};
    double y{};
    double z{};
};

[[nodiscard]] Vec3 ToVec3(Float3 const &value) noexcept
{
    return {static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z)};
}

[[nodiscard]] Vec3 Subtract(Vec3 const &lhs, Vec3 const &rhs) noexcept
{
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

[[nodiscard]] double Dot(Vec3 const &lhs, Vec3 const &rhs) noexcept
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

[[nodiscard]] Vec3 Cross(Vec3 const &lhs, Vec3 const &rhs) noexcept
{
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

[[nodiscard]] double LengthSquared(Vec3 const &value) noexcept
{
    return Dot(value, value);
}

[[nodiscard]] bool IsFinite(Float3 const &value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

// Relative degeneracy test: the squared doubled-area (cross length squared) scales
// as the fourth power of edge length, so comparing it against a relative fraction
// of the largest squared edge length keeps the test scale invariant.
constexpr double kRelativeDegenerateArea = 1e-12;

[[nodiscard]] bool TriangleIsDegenerate(Vec3 const &p0, Vec3 const &p1, Vec3 const &p2) noexcept
{
    Vec3 const edge0 = Subtract(p1, p0);
    Vec3 const edge1 = Subtract(p2, p0);
    Vec3 const edge2 = Subtract(p2, p1);
    double const crossLengthSquared = LengthSquared(Cross(edge0, edge1));
    double const longestEdgeSquared = std::max({LengthSquared(edge0), LengthSquared(edge1), LengthSquared(edge2)});
    double const threshold = kRelativeDegenerateArea * longestEdgeSquared * longestEdgeSquared;
    return !(crossLengthSquared > threshold);
}

[[nodiscard]] bool CheckedAddToU32(std::uint64_t &accumulator, std::uint64_t addend) noexcept
{
    accumulator += addend;
    return accumulator <= static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
}

[[nodiscard]] std::expected<BuildStatistics, MeshError> ComputeStatistics(std::span<Meshlet const> meshlets,
                                                                          std::size_t const positionCount)
{
    if (meshlets.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected(MeshError::ArithmeticOverflow);
    }

    std::uint64_t primitiveCount = 0U;
    std::uint64_t vertexReferenceCount = 0U;
    std::vector<bool> referenced(positionCount, false);
    for (Meshlet const &meshlet : meshlets)
    {
        if (!CheckedAddToU32(primitiveCount, meshlet.primitives.size()) ||
            !CheckedAddToU32(vertexReferenceCount, meshlet.vertexRemap.size()))
        {
            return std::unexpected(MeshError::ArithmeticOverflow);
        }
        for (GlobalIndex const global : meshlet.vertexRemap)
        {
            if (static_cast<std::size_t>(global) < positionCount)
            {
                referenced[global] = true;
            }
        }
    }

    std::uint64_t referencedVertexCount = 0U;
    for (bool const isReferenced : referenced)
    {
        if (isReferenced && !CheckedAddToU32(referencedVertexCount, 1U))
        {
            return std::unexpected(MeshError::ArithmeticOverflow);
        }
    }

    return BuildStatistics{
        .meshletCount = static_cast<std::uint32_t>(meshlets.size()),
        .primitiveCount = static_cast<std::uint32_t>(primitiveCount),
        .vertexReferenceCount = static_cast<std::uint32_t>(vertexReferenceCount),
        .referencedVertexCount = static_cast<std::uint32_t>(referencedVertexCount),
        .duplicatedVertexReferences = static_cast<std::uint32_t>(vertexReferenceCount - referencedVertexCount),
    };
}

} // namespace

std::expected<void, MeshError> ValidateMesh(std::span<Float3 const> positions,
                                            std::span<GlobalIndex const> indices) noexcept
{
    if (positions.empty())
    {
        return std::unexpected(MeshError::EmptyPositions);
    }
    if (indices.empty())
    {
        return std::unexpected(MeshError::EmptyIndices);
    }
    if (indices.size() % 3U != 0U)
    {
        return std::unexpected(MeshError::IndexCountNotTriangleList);
    }
    for (Float3 const &position : positions)
    {
        if (!IsFinite(position))
        {
            return std::unexpected(MeshError::NonFinitePosition);
        }
    }
    std::size_t const positionCount = positions.size();
    for (GlobalIndex const index : indices)
    {
        if (static_cast<std::size_t>(index) >= positionCount)
        {
            return std::unexpected(MeshError::IndexOutOfRange);
        }
    }
    for (std::size_t triangle = 0U; triangle < indices.size(); triangle += 3U)
    {
        GlobalIndex const i0 = indices[triangle];
        GlobalIndex const i1 = indices[triangle + 1U];
        GlobalIndex const i2 = indices[triangle + 2U];
        if (i0 == i1 || i1 == i2 || i0 == i2)
        {
            return std::unexpected(MeshError::DegenerateTriangle);
        }
        if (TriangleIsDegenerate(ToVec3(positions[i0]), ToVec3(positions[i1]), ToVec3(positions[i2])))
        {
            return std::unexpected(MeshError::DegenerateTriangle);
        }
    }
    return {};
}

std::expected<void, MeshError> ValidateLimits(MeshletLimits const limits) noexcept
{
    if (limits.maxVertices < 3U)
    {
        return std::unexpected(MeshError::MaxVerticesTooSmall);
    }
    if (limits.maxVertices > kMaxMeshletVertices)
    {
        return std::unexpected(MeshError::MaxVerticesTooLarge);
    }
    if (limits.maxPrimitives < 1U)
    {
        return std::unexpected(MeshError::MaxPrimitivesTooSmall);
    }
    if (limits.maxPrimitives > kMaxMeshletPrimitives)
    {
        return std::unexpected(MeshError::MaxPrimitivesTooLarge);
    }
    return {};
}

std::expected<MeshletBuild, MeshError> BuildMeshlets(std::span<Float3 const> positions,
                                                     std::span<GlobalIndex const> indices, MeshletLimits const limits)
{
    if (auto const limitsValid = ValidateLimits(limits); !limitsValid)
    {
        return std::unexpected(limitsValid.error());
    }
    if (auto const meshValid = ValidateMesh(positions, indices); !meshValid)
    {
        return std::unexpected(meshValid.error());
    }
    if (indices.size() / 3U > std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected(MeshError::ArithmeticOverflow);
    }

    MeshletBuild build{};
    Meshlet current{};
    std::unordered_map<GlobalIndex, LocalIndex> localOf{};

    auto const finishCurrent = [&]()
    {
        if (!current.primitives.empty())
        {
            build.meshlets.push_back(std::move(current));
        }
        current = Meshlet{};
        localOf.clear();
    };

    auto const requiredNewVertices = [&](GlobalIndex const i0, GlobalIndex const i1, GlobalIndex const i2)
    {
        std::uint32_t required = 0U;
        if (!localOf.contains(i0))
        {
            ++required;
        }
        if (i1 != i0 && !localOf.contains(i1))
        {
            ++required;
        }
        if (i2 != i0 && i2 != i1 && !localOf.contains(i2))
        {
            ++required;
        }
        return required;
    };

    auto const appendVertex = [&](GlobalIndex const global) -> LocalIndex
    {
        if (auto const existing = localOf.find(global); existing != localOf.end())
        {
            return existing->second;
        }
        auto const local = static_cast<LocalIndex>(current.vertexRemap.size());
        current.vertexRemap.push_back(global);
        localOf.emplace(global, local);
        return local;
    };

    for (std::size_t triangle = 0U; triangle < indices.size(); triangle += 3U)
    {
        GlobalIndex const i0 = indices[triangle];
        GlobalIndex const i1 = indices[triangle + 1U];
        GlobalIndex const i2 = indices[triangle + 2U];

        std::uint32_t const newVertices = requiredNewVertices(i0, i1, i2);
        std::uint32_t const currentVertices = static_cast<std::uint32_t>(current.vertexRemap.size());
        std::uint32_t const currentPrimitives = static_cast<std::uint32_t>(current.primitives.size());
        bool const verticesFit = newVertices <= limits.maxVertices - currentVertices;
        bool const primitivesFit = currentPrimitives < limits.maxPrimitives;
        if (!current.primitives.empty() && !(verticesFit && primitivesFit))
        {
            finishCurrent();
        }

        current.primitives.push_back({appendVertex(i0), appendVertex(i1), appendVertex(i2)});
    }
    finishCurrent();

    auto const statistics = ComputeStatistics(build.meshlets, positions.size());
    if (!statistics)
    {
        return std::unexpected(statistics.error());
    }
    build.statistics = *statistics;
    return build;
}

std::expected<std::vector<GlobalIndex>, MeshError> ReconstructIndices(MeshletBuild const &build)
{
    std::vector<GlobalIndex> indices{};
    for (Meshlet const &meshlet : build.meshlets)
    {
        std::size_t const remapSize = meshlet.vertexRemap.size();
        for (PrimitiveTriangle const &primitive : meshlet.primitives)
        {
            if (static_cast<std::size_t>(primitive.a) >= remapSize ||
                static_cast<std::size_t>(primitive.b) >= remapSize ||
                static_cast<std::size_t>(primitive.c) >= remapSize)
            {
                return std::unexpected(MeshError::LocalIndexOutOfRange);
            }
            indices.push_back(meshlet.vertexRemap[primitive.a]);
            indices.push_back(meshlet.vertexRemap[primitive.b]);
            indices.push_back(meshlet.vertexRemap[primitive.c]);
        }
    }
    return indices;
}

std::expected<void, MeshError> ValidateMeshletBuild(MeshletBuild const &build, std::span<Float3 const> positions,
                                                    std::span<GlobalIndex const> indices, MeshletLimits const limits)
{
    if (auto const limitsValid = ValidateLimits(limits); !limitsValid)
    {
        return std::unexpected(limitsValid.error());
    }
    if (auto const meshValid = ValidateMesh(positions, indices); !meshValid)
    {
        return std::unexpected(meshValid.error());
    }
    if (build.meshlets.empty())
    {
        return std::unexpected(MeshError::EmptyMeshletSet);
    }

    std::size_t const positionCount = positions.size();
    for (Meshlet const &meshlet : build.meshlets)
    {
        if (meshlet.primitives.empty() || meshlet.vertexRemap.empty())
        {
            return std::unexpected(MeshError::EmptyMeshlet);
        }
        if (meshlet.vertexRemap.size() > limits.maxVertices)
        {
            return std::unexpected(MeshError::VertexLimitExceeded);
        }
        if (meshlet.primitives.size() > limits.maxPrimitives)
        {
            return std::unexpected(MeshError::PrimitiveLimitExceeded);
        }

        std::unordered_set<GlobalIndex> seen{};
        seen.reserve(meshlet.vertexRemap.size());
        for (GlobalIndex const global : meshlet.vertexRemap)
        {
            if (static_cast<std::size_t>(global) >= positionCount)
            {
                return std::unexpected(MeshError::GlobalIndexOutOfRange);
            }
            if (!seen.insert(global).second)
            {
                return std::unexpected(MeshError::DuplicateGlobalVertex);
            }
        }

        std::size_t const remapSize = meshlet.vertexRemap.size();
        for (PrimitiveTriangle const &primitive : meshlet.primitives)
        {
            if (static_cast<std::size_t>(primitive.a) >= remapSize ||
                static_cast<std::size_t>(primitive.b) >= remapSize ||
                static_cast<std::size_t>(primitive.c) >= remapSize)
            {
                return std::unexpected(MeshError::LocalIndexOutOfRange);
            }
        }
    }

    auto const reconstructed = ReconstructIndices(build);
    if (!reconstructed)
    {
        return std::unexpected(reconstructed.error());
    }
    if (reconstructed->size() != indices.size())
    {
        return std::unexpected(MeshError::ReconstructionCountMismatch);
    }
    for (std::size_t index = 0U; index < indices.size(); ++index)
    {
        if ((*reconstructed)[index] != indices[index])
        {
            return std::unexpected(MeshError::ReconstructionIndexMismatch);
        }
    }
    auto const statistics = ComputeStatistics(build.meshlets, positions.size());
    if (!statistics)
    {
        return std::unexpected(statistics.error());
    }
    if (*statistics != build.statistics)
    {
        return std::unexpected(MeshError::StatisticsMismatch);
    }
    return {};
}

std::expected<BoundingSphere, MeshError> ComputeCentroidBoundingSphere(Meshlet const &meshlet,
                                                                       std::span<Float3 const> positions)
{
    if (meshlet.vertexRemap.empty())
    {
        return std::unexpected(MeshError::EmptyMeshlet);
    }

    std::size_t const positionCount = positions.size();
    Vec3 centroid{};
    for (GlobalIndex const global : meshlet.vertexRemap)
    {
        if (static_cast<std::size_t>(global) >= positionCount)
        {
            return std::unexpected(MeshError::GlobalIndexOutOfRange);
        }
        Float3 const &position = positions[global];
        if (!IsFinite(position))
        {
            return std::unexpected(MeshError::NonFinitePosition);
        }
        Vec3 const point = ToVec3(position);
        centroid.x += point.x;
        centroid.y += point.y;
        centroid.z += point.z;
    }

    double const count = static_cast<double>(meshlet.vertexRemap.size());
    centroid = {centroid.x / count, centroid.y / count, centroid.z / count};
    Float3 const center{static_cast<float>(centroid.x), static_cast<float>(centroid.y), static_cast<float>(centroid.z)};
    Vec3 const centerExact = ToVec3(center);

    double maxDistanceSquared = 0.0;
    for (GlobalIndex const global : meshlet.vertexRemap)
    {
        Vec3 const offset = Subtract(ToVec3(positions[global]), centerExact);
        maxDistanceSquared = std::max(maxDistanceSquared, LengthSquared(offset));
    }

    float radius = static_cast<float>(std::sqrt(maxDistanceSquared));
    if (static_cast<double>(radius) * static_cast<double>(radius) < maxDistanceSquared)
    {
        radius = std::nextafter(radius, std::numeric_limits<float>::infinity());
    }
    return BoundingSphere{.center = center, .radius = radius};
}

std::expected<NormalCone, MeshError> ComputeNormalCone(Meshlet const &meshlet, std::span<Float3 const> positions)
{
    if (meshlet.primitives.empty())
    {
        return std::unexpected(MeshError::EmptyMeshlet);
    }

    std::size_t const positionCount = positions.size();
    std::size_t const remapSize = meshlet.vertexRemap.size();

    NormalCone const disabled{};
    std::vector<Vec3> unitNormals{};
    unitNormals.reserve(meshlet.primitives.size());
    Vec3 accumulated{};

    for (PrimitiveTriangle const &primitive : meshlet.primitives)
    {
        if (static_cast<std::size_t>(primitive.a) >= remapSize || static_cast<std::size_t>(primitive.b) >= remapSize ||
            static_cast<std::size_t>(primitive.c) >= remapSize)
        {
            return std::unexpected(MeshError::LocalIndexOutOfRange);
        }
        GlobalIndex const g0 = meshlet.vertexRemap[primitive.a];
        GlobalIndex const g1 = meshlet.vertexRemap[primitive.b];
        GlobalIndex const g2 = meshlet.vertexRemap[primitive.c];
        if (static_cast<std::size_t>(g0) >= positionCount || static_cast<std::size_t>(g1) >= positionCount ||
            static_cast<std::size_t>(g2) >= positionCount)
        {
            return std::unexpected(MeshError::GlobalIndexOutOfRange);
        }
        Float3 const &p0 = positions[g0];
        Float3 const &p1 = positions[g1];
        Float3 const &p2 = positions[g2];
        if (!IsFinite(p0) || !IsFinite(p1) || !IsFinite(p2))
        {
            return std::unexpected(MeshError::NonFinitePosition);
        }

        Vec3 const normal = Cross(Subtract(ToVec3(p1), ToVec3(p0)), Subtract(ToVec3(p2), ToVec3(p0)));
        double const lengthSquared = LengthSquared(normal);
        if (!(lengthSquared > 0.0) || !std::isfinite(lengthSquared))
        {
            // A degenerate face cannot yield a trustworthy orientation, so refuse
            // to emit a cone rather than risk culling visible geometry.
            return disabled;
        }
        double const inverseLength = 1.0 / std::sqrt(lengthSquared);
        Vec3 const unitNormal{normal.x * inverseLength, normal.y * inverseLength, normal.z * inverseLength};
        unitNormals.push_back(unitNormal);
        accumulated.x += unitNormal.x;
        accumulated.y += unitNormal.y;
        accumulated.z += unitNormal.z;
    }

    double const accumulatedLength = std::sqrt(LengthSquared(accumulated));
    constexpr double kAxisEpsilon = 1e-6;
    if (accumulatedLength <= kAxisEpsilon)
    {
        // Opposing normals cancel: no axis can bound them, so the cone is disabled.
        return disabled;
    }

    Vec3 const axis{accumulated.x / accumulatedLength, accumulated.y / accumulatedLength,
                    accumulated.z / accumulatedLength};
    double minimumDot = 1.0;
    for (Vec3 const &unitNormal : unitNormals)
    {
        double const clamped = std::clamp(Dot(unitNormal, axis), -1.0, 1.0);
        minimumDot = std::min(minimumDot, clamped);
    }
    if (minimumDot <= 0.0)
    {
        // The normals span at least a hemisphere; a half-angle of 90 degrees or
        // more is never safe for backface culling.
        return disabled;
    }

    Float3 const storedAxis{static_cast<float>(axis.x), static_cast<float>(axis.y), static_cast<float>(axis.z)};
    Vec3 const quantizedAxis = ToVec3(storedAxis);
    double const quantizedAxisLength = std::sqrt(LengthSquared(quantizedAxis));
    if (!(quantizedAxisLength > kAxisEpsilon))
    {
        return disabled;
    }

    Vec3 const normalizedQuantizedAxis{quantizedAxis.x / quantizedAxisLength, quantizedAxis.y / quantizedAxisLength,
                                       quantizedAxis.z / quantizedAxisLength};
    minimumDot = 1.0;
    for (Vec3 const &unitNormal : unitNormals)
    {
        minimumDot = std::min(minimumDot, std::clamp(Dot(unitNormal, normalizedQuantizedAxis), -1.0, 1.0));
    }
    if (minimumDot <= 0.0)
    {
        return disabled;
    }

    float cosHalfAngle = static_cast<float>(minimumDot);
    if (static_cast<double>(cosHalfAngle) > minimumDot)
    {
        cosHalfAngle = std::nextafter(cosHalfAngle, -std::numeric_limits<float>::infinity());
    }
    return NormalCone{.axis = storedAxis, .cosHalfAngle = cosHalfAngle, .valid = true};
}

} // namespace ch22::meshlets
