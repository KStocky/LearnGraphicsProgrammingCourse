#pragma once

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ch02::rasterization
{

using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;

inline constexpr std::size_t kTriangleVertexCount = 3U;

enum class Winding : std::uint8_t
{
    Degenerate = 0,
    Clockwise,
    CounterClockwise,
};

struct ScreenTriangle final
{
    std::array<XMFLOAT2, kTriangleVertexCount> positions{};
};

struct BarycentricCoordinates final
{
    float vertex0{};
    float vertex1{};
    float vertex2{};

    [[nodiscard]] constexpr float sum() const noexcept
    {
        return vertex0 + vertex1 + vertex2;
    }
};

struct EdgeEquation final
{
    float a{};
    float b{};
    float c{};
    bool topLeft{};

    [[nodiscard]] constexpr float Evaluate(XMFLOAT2 point) const noexcept
    {
        return (a * point.x) + (b * point.y) + c;
    }
};

struct ClipVertex final
{
    XMFLOAT4 position{};
    XMFLOAT3 attribute{};
};

enum class ClipPlane : std::uint8_t
{
    Left = 0,
    Right,
    Bottom,
    Top,
    Near,
    Far,
};

enum class DepthComparison : std::uint8_t
{
    Less = 0,
    LessEqual,
    Greater,
    GreaterEqual,
    Always,
};

struct RasterizationConventions final
{
    bool frontFacesAreClockwise{true};
    XMFLOAT2 pixelCenterOffset{0.5F, 0.5F};
    float minimumDepth{0.0F};
    float maximumDepth{1.0F};
};

inline constexpr RasterizationConventions kConventions{};

} // namespace ch02::rasterization
