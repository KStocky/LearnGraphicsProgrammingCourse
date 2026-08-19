#include "ChapterGeometry.hpp"

namespace ch01::graphics_math
{
namespace
{

constexpr std::array<XMFLOAT3, kObjectVertexCount> kObjectPositions{
    XMFLOAT3{-0.75F, -0.75F, -0.75F}, XMFLOAT3{-0.75F, -0.75F, 0.75F}, XMFLOAT3{-0.75F, 0.75F, -0.75F},
    XMFLOAT3{-0.75F, 0.75F, 0.75F},   XMFLOAT3{0.75F, -0.75F, -0.75F}, XMFLOAT3{0.75F, -0.75F, 0.75F},
    XMFLOAT3{0.75F, 0.75F, -0.75F},   XMFLOAT3{0.75F, 0.75F, 0.75F},
};

constexpr std::array<LineVertex, kObjectVertexCount> kObjectLineVertices{
    LineVertex{kObjectPositions[0], XMFLOAT4{0.55F, 0.80F, 1.00F, 1.00F}},
    LineVertex{kObjectPositions[1], XMFLOAT4{0.55F, 0.80F, 1.00F, 1.00F}},
    LineVertex{kObjectPositions[2], XMFLOAT4{0.55F, 0.80F, 1.00F, 1.00F}},
    LineVertex{kObjectPositions[3], XMFLOAT4{0.55F, 0.80F, 1.00F, 1.00F}},
    LineVertex{kObjectPositions[4], XMFLOAT4{0.55F, 0.80F, 1.00F, 1.00F}},
    LineVertex{kObjectPositions[5], XMFLOAT4{0.55F, 0.80F, 1.00F, 1.00F}},
    LineVertex{kObjectPositions[6], XMFLOAT4{0.55F, 0.80F, 1.00F, 1.00F}},
    LineVertex{kObjectPositions[7], XMFLOAT4{0.55F, 0.80F, 1.00F, 1.00F}},
};

constexpr std::array<std::uint16_t, 24U> kObjectLineIndices{
    0U, 1U, 1U, 3U, 3U, 2U, 2U, 0U, 4U, 5U, 5U, 7U, 7U, 6U, 6U, 4U, 0U, 4U, 1U, 5U, 2U, 6U, 3U, 7U,
};

constexpr std::array<LineVertex, 6U> kAxisVertices{
    LineVertex{XMFLOAT3{0.0F, 0.0F, 0.0F}, XMFLOAT4{1.0F, 0.2F, 0.2F, 1.0F}},
    LineVertex{XMFLOAT3{1.0F, 0.0F, 0.0F}, XMFLOAT4{1.0F, 0.2F, 0.2F, 1.0F}},
    LineVertex{XMFLOAT3{0.0F, 0.0F, 0.0F}, XMFLOAT4{0.2F, 1.0F, 0.2F, 1.0F}},
    LineVertex{XMFLOAT3{0.0F, 1.0F, 0.0F}, XMFLOAT4{0.2F, 1.0F, 0.2F, 1.0F}},
    LineVertex{XMFLOAT3{0.0F, 0.0F, 0.0F}, XMFLOAT4{0.2F, 0.5F, 1.0F, 1.0F}},
    LineVertex{XMFLOAT3{0.0F, 0.0F, 1.0F}, XMFLOAT4{0.2F, 0.5F, 1.0F, 1.0F}},
};

constexpr std::array<std::uint16_t, 6U> kAxisIndices{0U, 1U, 2U, 3U, 4U, 5U};

constexpr std::array<LineVertex, 8U> kClipFrustumVertices{
    LineVertex{XMFLOAT3{-1.0F, -1.0F, 0.0F}, XMFLOAT4{1.0F, 0.95F, 0.2F, 1.0F}},
    LineVertex{XMFLOAT3{-1.0F, 1.0F, 0.0F}, XMFLOAT4{1.0F, 0.95F, 0.2F, 1.0F}},
    LineVertex{XMFLOAT3{1.0F, -1.0F, 0.0F}, XMFLOAT4{1.0F, 0.95F, 0.2F, 1.0F}},
    LineVertex{XMFLOAT3{1.0F, 1.0F, 0.0F}, XMFLOAT4{1.0F, 0.95F, 0.2F, 1.0F}},
    LineVertex{XMFLOAT3{-1.0F, -1.0F, 1.0F}, XMFLOAT4{1.0F, 0.95F, 0.2F, 1.0F}},
    LineVertex{XMFLOAT3{-1.0F, 1.0F, 1.0F}, XMFLOAT4{1.0F, 0.95F, 0.2F, 1.0F}},
    LineVertex{XMFLOAT3{1.0F, -1.0F, 1.0F}, XMFLOAT4{1.0F, 0.95F, 0.2F, 1.0F}},
    LineVertex{XMFLOAT3{1.0F, 1.0F, 1.0F}, XMFLOAT4{1.0F, 0.95F, 0.2F, 1.0F}},
};

constexpr std::array<std::uint16_t, 24U> kClipFrustumIndices{
    0U, 1U, 1U, 3U, 3U, 2U, 2U, 0U, 4U, 5U, 5U, 7U, 7U, 6U, 6U, 4U, 0U, 4U, 1U, 5U, 2U, 6U, 3U, 7U,
};

constexpr std::array<LineVertex, 6U> kCrossVertices{
    LineVertex{XMFLOAT3{-1.0F, 0.0F, 0.0F}, XMFLOAT4{1.0F, 1.0F, 1.0F, 1.0F}},
    LineVertex{XMFLOAT3{1.0F, 0.0F, 0.0F}, XMFLOAT4{1.0F, 1.0F, 1.0F, 1.0F}},
    LineVertex{XMFLOAT3{0.0F, -1.0F, 0.0F}, XMFLOAT4{1.0F, 1.0F, 1.0F, 1.0F}},
    LineVertex{XMFLOAT3{0.0F, 1.0F, 0.0F}, XMFLOAT4{1.0F, 1.0F, 1.0F, 1.0F}},
    LineVertex{XMFLOAT3{0.0F, 0.0F, -1.0F}, XMFLOAT4{1.0F, 1.0F, 1.0F, 1.0F}},
    LineVertex{XMFLOAT3{0.0F, 0.0F, 1.0F}, XMFLOAT4{1.0F, 1.0F, 1.0F, 1.0F}},
};

constexpr std::array<std::uint16_t, 6U> kCrossIndices{0U, 1U, 2U, 3U, 4U, 5U};

constexpr std::array<LineVertex, 2U> kRayVertices{
    LineVertex{XMFLOAT3{0.0F, 0.0F, 0.0F}, XMFLOAT4{1.0F, 1.0F, 1.0F, 1.0F}},
    LineVertex{XMFLOAT3{0.0F, 0.0F, 1.0F}, XMFLOAT4{1.0F, 1.0F, 1.0F, 1.0F}},
};

constexpr std::array<std::uint16_t, 2U> kRayIndices{0U, 1U};

} // namespace

std::span<XMFLOAT3 const> ObjectVertices() noexcept
{
    return kObjectPositions;
}

IndexedLineMesh ObjectMesh() noexcept
{
    return {kObjectLineVertices, kObjectLineIndices};
}

IndexedLineMesh AxisMesh() noexcept
{
    return {kAxisVertices, kAxisIndices};
}

IndexedLineMesh ClipFrustumMesh() noexcept
{
    return {kClipFrustumVertices, kClipFrustumIndices};
}

IndexedLineMesh CrossMesh() noexcept
{
    return {kCrossVertices, kCrossIndices};
}

IndexedLineMesh RayMesh() noexcept
{
    return {kRayVertices, kRayIndices};
}

} // namespace ch01::graphics_math
