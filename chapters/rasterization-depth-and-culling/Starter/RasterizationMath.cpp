#include "RasterizationMath.hpp"

namespace ch02::rasterization::starter
{

float SignedTwiceArea(ScreenTriangle const &) noexcept
{
    return 0.0F;
}

Winding ClassifyWinding(float, float) noexcept
{
    return Winding::Degenerate;
}

EdgeEquation BuildEdgeEquation(XMFLOAT2, XMFLOAT2) noexcept
{
    return {};
}

bool IsCoveredByEdge(EdgeEquation const &, XMFLOAT2, float) noexcept
{
    return false;
}

bool IsCovered(ScreenTriangle const &, XMFLOAT2, float) noexcept
{
    return false;
}

std::optional<BarycentricCoordinates> ComputeBarycentrics(ScreenTriangle const &, XMFLOAT2, float) noexcept
{
    return std::nullopt;
}

XMFLOAT3 InterpolateAffine(std::array<XMFLOAT3, kTriangleVertexCount> const &, BarycentricCoordinates) noexcept
{
    return {};
}

std::optional<XMFLOAT3> InterpolatePerspectiveCorrect(std::array<XMFLOAT3, kTriangleVertexCount> const &,
                                                      std::array<float, kTriangleVertexCount> const &,
                                                      BarycentricCoordinates, float) noexcept
{
    return std::nullopt;
}

float ClipPlaneDistance(XMFLOAT4, ClipPlane) noexcept
{
    return -1.0F;
}

std::vector<ClipVertex> ClipPolygonAgainstPlane(std::vector<ClipVertex> const &, ClipPlane, float)
{
    return {};
}

std::optional<float> NormalizedDepth(XMFLOAT4, float) noexcept
{
    return std::nullopt;
}

bool DepthPasses(float, float, DepthComparison) noexcept
{
    return false;
}

} // namespace ch02::rasterization::starter
