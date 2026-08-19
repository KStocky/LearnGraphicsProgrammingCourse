#pragma once

#include "ChapterTypes.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace ch01::graphics_math
{

struct LineVertex final
{
    XMFLOAT3 position{};
    XMFLOAT4 color{};
};

struct IndexedLineMesh final
{
    std::span<LineVertex const> vertices;
    std::span<std::uint16_t const> indices;
};

inline constexpr XMFLOAT3 kReferenceNormalObject{0.5F, 1.0F, 0.35F};
inline constexpr XMFLOAT3 kWorldUp{0.0F, 1.0F, 0.0F};

[[nodiscard]] std::span<XMFLOAT3 const> ObjectVertices() noexcept;
[[nodiscard]] IndexedLineMesh ObjectMesh() noexcept;
[[nodiscard]] IndexedLineMesh AxisMesh() noexcept;
[[nodiscard]] IndexedLineMesh ClipFrustumMesh() noexcept;
[[nodiscard]] IndexedLineMesh CrossMesh() noexcept;
[[nodiscard]] IndexedLineMesh RayMesh() noexcept;

} // namespace ch01::graphics_math
