#include "RasterizationMath.hpp"

#include <algorithm>
#include <cmath>

namespace ch02::rasterization::solution
{
namespace
{

[[nodiscard]] XMFLOAT3 ScaleAdd(XMFLOAT3 first, float firstScale, XMFLOAT3 second, float secondScale) noexcept
{
    return {
        (first.x * firstScale) + (second.x * secondScale),
        (first.y * firstScale) + (second.y * secondScale),
        (first.z * firstScale) + (second.z * secondScale),
    };
}

[[nodiscard]] ClipVertex Interpolate(ClipVertex const &first, ClipVertex const &second, float amount) noexcept
{
    float const firstWeight = 1.0F - amount;
    return {
        {
            (first.position.x * firstWeight) + (second.position.x * amount),
            (first.position.y * firstWeight) + (second.position.y * amount),
            (first.position.z * firstWeight) + (second.position.z * amount),
            (first.position.w * firstWeight) + (second.position.w * amount),
        },
        ScaleAdd(first.attribute, firstWeight, second.attribute, amount),
    };
}

} // namespace

float SignedTwiceArea(ScreenTriangle const &triangle) noexcept
{
    XMFLOAT2 const edge01{
        triangle.positions[1].x - triangle.positions[0].x,
        triangle.positions[1].y - triangle.positions[0].y,
    };
    XMFLOAT2 const edge02{
        triangle.positions[2].x - triangle.positions[0].x,
        triangle.positions[2].y - triangle.positions[0].y,
    };
    return (edge01.x * edge02.y) - (edge01.y * edge02.x);
}

Winding ClassifyWinding(float signedTwiceArea, float epsilon) noexcept
{
    if (std::fabs(signedTwiceArea) <= epsilon)
    {
        return Winding::Degenerate;
    }
    return signedTwiceArea > 0.0F ? Winding::Clockwise : Winding::CounterClockwise;
}

EdgeEquation BuildEdgeEquation(XMFLOAT2 start, XMFLOAT2 end) noexcept
{
    float const deltaX = end.x - start.x;
    float const deltaY = end.y - start.y;
    return {
        -deltaY,
        deltaX,
        (deltaY * start.x) - (deltaX * start.y),
        deltaY < 0.0F || (deltaY == 0.0F && deltaX > 0.0F),
    };
}

bool IsCoveredByEdge(EdgeEquation const &edge, XMFLOAT2 sample, float epsilon) noexcept
{
    float const value = edge.Evaluate(sample);
    return value > epsilon || (std::fabs(value) <= epsilon && edge.topLeft);
}

bool IsCovered(ScreenTriangle const &triangle, XMFLOAT2 sample, float epsilon) noexcept
{
    if (ClassifyWinding(SignedTwiceArea(triangle), epsilon) != Winding::Clockwise)
    {
        return false;
    }

    return IsCoveredByEdge(BuildEdgeEquation(triangle.positions[0], triangle.positions[1]), sample, epsilon) &&
           IsCoveredByEdge(BuildEdgeEquation(triangle.positions[1], triangle.positions[2]), sample, epsilon) &&
           IsCoveredByEdge(BuildEdgeEquation(triangle.positions[2], triangle.positions[0]), sample, epsilon);
}

std::optional<BarycentricCoordinates> ComputeBarycentrics(ScreenTriangle const &triangle, XMFLOAT2 sample,
                                                          float epsilon) noexcept
{
    float const area = SignedTwiceArea(triangle);
    if (std::fabs(area) <= epsilon)
    {
        return std::nullopt;
    }

    return BarycentricCoordinates{
        BuildEdgeEquation(triangle.positions[1], triangle.positions[2]).Evaluate(sample) / area,
        BuildEdgeEquation(triangle.positions[2], triangle.positions[0]).Evaluate(sample) / area,
        BuildEdgeEquation(triangle.positions[0], triangle.positions[1]).Evaluate(sample) / area,
    };
}

XMFLOAT3 InterpolateAffine(std::array<XMFLOAT3, kTriangleVertexCount> const &attributes,
                           BarycentricCoordinates barycentrics) noexcept
{
    XMFLOAT3 const firstPair = ScaleAdd(attributes[0], barycentrics.vertex0, attributes[1], barycentrics.vertex1);
    return ScaleAdd(firstPair, 1.0F, attributes[2], barycentrics.vertex2);
}

std::optional<XMFLOAT3> InterpolatePerspectiveCorrect(std::array<XMFLOAT3, kTriangleVertexCount> const &attributes,
                                                      std::array<float, kTriangleVertexCount> const &clipW,
                                                      BarycentricCoordinates barycentrics, float epsilon) noexcept
{
    if (std::ranges::any_of(clipW, [epsilon](float value) { return std::fabs(value) <= epsilon; }))
    {
        return std::nullopt;
    }

    std::array<float, kTriangleVertexCount> const weights{
        barycentrics.vertex0 / clipW[0],
        barycentrics.vertex1 / clipW[1],
        barycentrics.vertex2 / clipW[2],
    };
    float const denominator = weights[0] + weights[1] + weights[2];
    if (std::fabs(denominator) <= epsilon)
    {
        return std::nullopt;
    }

    XMFLOAT3 const numerator01 = ScaleAdd(attributes[0], weights[0], attributes[1], weights[1]);
    XMFLOAT3 const numerator = ScaleAdd(numerator01, 1.0F, attributes[2], weights[2]);
    float const reciprocalDenominator = 1.0F / denominator;
    return XMFLOAT3{
        numerator.x * reciprocalDenominator,
        numerator.y * reciprocalDenominator,
        numerator.z * reciprocalDenominator,
    };
}

float ClipPlaneDistance(XMFLOAT4 position, ClipPlane plane) noexcept
{
    switch (plane)
    {
    case ClipPlane::Left:
        return position.x + position.w;
    case ClipPlane::Right:
        return position.w - position.x;
    case ClipPlane::Bottom:
        return position.y + position.w;
    case ClipPlane::Top:
        return position.w - position.y;
    case ClipPlane::Near:
        return position.z;
    case ClipPlane::Far:
        return position.w - position.z;
    }
    return -1.0F;
}

std::vector<ClipVertex> ClipPolygonAgainstPlane(std::vector<ClipVertex> const &polygon, ClipPlane plane, float epsilon)
{
    std::vector<ClipVertex> output;
    if (polygon.empty())
    {
        return output;
    }
    output.reserve(polygon.size() + 1U);

    ClipVertex previous = polygon.back();
    float previousDistance = ClipPlaneDistance(previous.position, plane);
    bool previousInside = previousDistance >= -epsilon;

    for (ClipVertex const &current : polygon)
    {
        float const currentDistance = ClipPlaneDistance(current.position, plane);
        bool const currentInside = currentDistance >= -epsilon;

        if (previousInside != currentInside)
        {
            float const denominator = previousDistance - currentDistance;
            if (std::fabs(denominator) > epsilon)
            {
                output.push_back(Interpolate(previous, current, previousDistance / denominator));
            }
        }
        if (currentInside)
        {
            output.push_back(current);
        }

        previous = current;
        previousDistance = currentDistance;
        previousInside = currentInside;
    }
    return output;
}

std::optional<float> NormalizedDepth(XMFLOAT4 clipPosition, float epsilon) noexcept
{
    if (std::fabs(clipPosition.w) <= epsilon)
    {
        return std::nullopt;
    }
    return clipPosition.z / clipPosition.w;
}

bool DepthPasses(float incomingDepth, float storedDepth, DepthComparison comparison) noexcept
{
    switch (comparison)
    {
    case DepthComparison::Less:
        return incomingDepth < storedDepth;
    case DepthComparison::LessEqual:
        return incomingDepth <= storedDepth;
    case DepthComparison::Greater:
        return incomingDepth > storedDepth;
    case DepthComparison::GreaterEqual:
        return incomingDepth >= storedDepth;
    case DepthComparison::Always:
        return true;
    }
    return false;
}

} // namespace ch02::rasterization::solution
