#pragma once

#include "../Common/ChapterTypes.hpp"

#include <array>
#include <optional>
#include <vector>

namespace ch02::rasterization::solution
{

[[nodiscard]] float SignedTwiceArea(ScreenTriangle const &triangle) noexcept;
[[nodiscard]] Winding ClassifyWinding(float signedTwiceArea, float epsilon = 1.0e-6F) noexcept;
[[nodiscard]] EdgeEquation BuildEdgeEquation(XMFLOAT2 start, XMFLOAT2 end) noexcept;
[[nodiscard]] bool IsCoveredByEdge(EdgeEquation const &edge, XMFLOAT2 sample, float epsilon = 1.0e-6F) noexcept;
[[nodiscard]] bool IsCovered(ScreenTriangle const &triangle, XMFLOAT2 sample, float epsilon = 1.0e-6F) noexcept;
[[nodiscard]] std::optional<BarycentricCoordinates> ComputeBarycentrics(ScreenTriangle const &triangle, XMFLOAT2 sample,
                                                                        float epsilon = 1.0e-6F) noexcept;
[[nodiscard]] XMFLOAT3 InterpolateAffine(std::array<XMFLOAT3, kTriangleVertexCount> const &attributes,
                                         BarycentricCoordinates barycentrics) noexcept;
[[nodiscard]] std::optional<XMFLOAT3> InterpolatePerspectiveCorrect(
    std::array<XMFLOAT3, kTriangleVertexCount> const &attributes, std::array<float, kTriangleVertexCount> const &clipW,
    BarycentricCoordinates barycentrics, float epsilon = 1.0e-6F) noexcept;
[[nodiscard]] float ClipPlaneDistance(XMFLOAT4 position, ClipPlane plane) noexcept;
[[nodiscard]] std::vector<ClipVertex> ClipPolygonAgainstPlane(std::vector<ClipVertex> const &polygon, ClipPlane plane,
                                                              float epsilon = 1.0e-6F);
[[nodiscard]] std::optional<float> NormalizedDepth(XMFLOAT4 clipPosition, float epsilon = 1.0e-6F) noexcept;
[[nodiscard]] bool DepthPasses(float incomingDepth, float storedDepth, DepthComparison comparison) noexcept;

} // namespace ch02::rasterization::solution
