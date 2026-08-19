#include "VisibilityBufferContracts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ch15::visibility_buffer
{
namespace
{

inline constexpr double kBarycentricSumTolerance = 1.0e-5;
inline constexpr double kInsideTriangleTolerance = 64.0 * static_cast<double>(std::numeric_limits<float>::epsilon());
inline constexpr double kMinimumTriangleConditioning =
    32.0 * static_cast<double>(std::numeric_limits<float>::epsilon());

struct Double2 final
{
    double x{};
    double y{};
};

struct Double3 final
{
    double x{};
    double y{};
    double z{};
};

struct ScreenBarycentricEvaluation final
{
    std::array<double, 3U> values{};
    std::array<double, 3U> ddx{};
    std::array<double, 3U> ddy{};
};

struct ProjectedTriangle final
{
    std::array<Double2, 3U> positions{};
};

[[nodiscard]] bool IsFinite(Float2 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] bool IsFinite(Float3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool IsFinite(Float4 value) noexcept
{
    return IsFinite(Float3{value.x, value.y, value.z}) && std::isfinite(value.w);
}

[[nodiscard]] bool IsFinite(Double2 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] bool IsFinite(Double3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] Double2 Subtract(Double2 left, Double2 right) noexcept
{
    return {left.x - right.x, left.y - right.y};
}

[[nodiscard]] Double3 Subtract(Double3 left, Double3 right) noexcept
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] Double3 Multiply(Double3 value, double scalar) noexcept
{
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] double Dot(Double2 left, Double2 right) noexcept
{
    return (left.x * right.x) + (left.y * right.y);
}

[[nodiscard]] double Dot(Double3 left, Double3 right) noexcept
{
    return (left.x * right.x) + (left.y * right.y) + (left.z * right.z);
}

[[nodiscard]] double Cross(Double2 left, Double2 right) noexcept
{
    return (left.x * right.y) - (left.y * right.x);
}

[[nodiscard]] Double3 Cross(Double3 left, Double3 right) noexcept
{
    return {
        (left.y * right.z) - (left.z * right.y),
        (left.z * right.x) - (left.x * right.z),
        (left.x * right.y) - (left.y * right.x),
    };
}

[[nodiscard]] Double3 ToDouble3(Float3 value) noexcept
{
    return {static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z)};
}

[[nodiscard]] std::expected<Float2, ContractError> ToFloat2(Double2 value) noexcept
{
    Float2 const result{static_cast<float>(value.x), static_cast<float>(value.y)};
    if (!IsFinite(result))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return result;
}

[[nodiscard]] std::expected<Float3, ContractError> ToFloat3(Double3 value) noexcept
{
    Float3 const result{static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
    if (!IsFinite(result))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return result;
}

[[nodiscard]] std::expected<Double3, ContractError> Normalize(Double3 value) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    double const lengthSquared = Dot(value, value);
    if (!std::isfinite(lengthSquared))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    if (lengthSquared <= static_cast<double>(std::numeric_limits<float>::epsilon()) *
                             static_cast<double>(std::numeric_limits<float>::epsilon()))
    {
        return std::unexpected(ContractError::InvalidSurfaceFrame);
    }
    return Multiply(value, 1.0 / std::sqrt(lengthSquared));
}

[[nodiscard]] bool CheckedAdd(std::uint32_t first, std::uint32_t second, std::uint32_t &result) noexcept
{
    if (first > std::numeric_limits<std::uint32_t>::max() - second)
    {
        return false;
    }
    result = first + second;
    return true;
}

[[nodiscard]] bool CheckedMultiply(std::uint32_t first, std::uint32_t second, std::uint32_t &result) noexcept
{
    if (second != 0U && first > std::numeric_limits<std::uint32_t>::max() / second)
    {
        return false;
    }
    result = first * second;
    return true;
}

[[nodiscard]] std::expected<void, ContractError> ValidateExtent(RenderExtent extent) noexcept
{
    if (extent.width == 0U || extent.height == 0U || extent.width > kMaximumRenderDimensionForFloatPixelCenters ||
        extent.height > kMaximumRenderDimensionForFloatPixelCenters)
    {
        return std::unexpected(ContractError::InvalidExtent);
    }
    return {};
}

[[nodiscard]] ContractError MapSurfaceFrameError(ch06::surface_frames::SurfaceFrameError error) noexcept
{
    if (error == ch06::surface_frames::SurfaceFrameError::NonFiniteInput)
    {
        return ContractError::NonFinite;
    }
    if (error == ch06::surface_frames::SurfaceFrameError::ArithmeticOverflow)
    {
        return ContractError::ArithmeticOverflow;
    }
    return ContractError::InvalidSurfaceFrame;
}

[[nodiscard]] std::expected<std::array<double, 3U>, ContractError> CanonicalizeBarycentrics(
    BarycentricCoordinates barycentrics) noexcept
{
    if (!std::isfinite(barycentrics.first) || !std::isfinite(barycentrics.second) || !std::isfinite(barycentrics.third))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (barycentrics.first < 0.0F || barycentrics.first > 1.0F || barycentrics.second < 0.0F ||
        barycentrics.second > 1.0F || barycentrics.third < 0.0F || barycentrics.third > 1.0F)
    {
        return std::unexpected(ContractError::InvalidBarycentrics);
    }

    double const sum = static_cast<double>(barycentrics.first) + static_cast<double>(barycentrics.second) +
                       static_cast<double>(barycentrics.third);
    if (!std::isfinite(sum) || sum <= 0.0 || std::abs(sum - 1.0) > kBarycentricSumTolerance)
    {
        return std::unexpected(ContractError::InvalidBarycentrics);
    }

    std::array<double, 3U> result{
        static_cast<double>(barycentrics.first) / sum,
        static_cast<double>(barycentrics.second) / sum,
        0.0,
    };
    result[2] = 1.0 - result[0] - result[1];
    if (result[2] < 0.0 && result[2] >= -kBarycentricSumTolerance)
    {
        result[2] = 0.0;
        double const firstTwoSum = result[0] + result[1];
        result[0] /= firstTwoSum;
        result[1] = 1.0 - result[0];
    }
    if (result[2] < 0.0)
    {
        return std::unexpected(ContractError::InvalidBarycentrics);
    }
    return result;
}

[[nodiscard]] std::expected<BarycentricCoordinates, ContractError> ToBarycentricCoordinates(
    std::array<double, 3U> values) noexcept
{
    for (double const value : values)
    {
        if (!std::isfinite(value) || value < 0.0 || value > 1.0)
        {
            return std::unexpected(ContractError::ArithmeticOverflow);
        }
    }

    float first = static_cast<float>(values[0]);
    float second = static_cast<float>(values[1]);
    if (!std::isfinite(first) || !std::isfinite(second))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    if (first + second > 1.0F)
    {
        if (values[0] >= values[1])
        {
            first = 1.0F - second;
        }
        else
        {
            second = 1.0F - first;
        }
    }
    float const third = 1.0F - first - second;
    if (third < 0.0F || !std::isfinite(third))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return BarycentricCoordinates{first, second, third};
}

[[nodiscard]] std::expected<void, ContractError> ValidateTriangle(TriangleVertices const &triangle) noexcept
{
    for (GeometryVertex const &vertex : triangle.vertices)
    {
        if (!IsFinite(vertex.clipPosition))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (vertex.clipPosition.w <= 0.0F)
        {
            return std::unexpected(ContractError::InvalidClipW);
        }
        float const reciprocalW = 1.0F / vertex.clipPosition.w;
        if (!std::isfinite(reciprocalW) || !std::isfinite(vertex.clipPosition.x * reciprocalW) ||
            !std::isfinite(vertex.clipPosition.y * reciprocalW))
        {
            return std::unexpected(ContractError::InvalidClipW);
        }

        std::expected<void, ch06::surface_frames::SurfaceFrameError> const surfaceResult =
            ch06::surface_frames::ValidateSurfaceVertex(vertex.surface);
        if (!surfaceResult)
        {
            return std::unexpected(MapSurfaceFrameError(surfaceResult.error()));
        }
    }

    float const handedness = triangle.vertices[0].surface.tangent.w;
    if (triangle.vertices[1].surface.tangent.w != handedness || triangle.vertices[2].surface.tangent.w != handedness)
    {
        return std::unexpected(ContractError::InvalidSurfaceFrame);
    }

    Double3 const viewEdgeOne =
        Subtract(ToDouble3(triangle.vertices[1].surface.position), ToDouble3(triangle.vertices[0].surface.position));
    Double3 const viewEdgeTwo =
        Subtract(ToDouble3(triangle.vertices[2].surface.position), ToDouble3(triangle.vertices[0].surface.position));
    Double3 const viewCross = Cross(viewEdgeOne, viewEdgeTwo);
    double const viewAreaSquared = Dot(viewCross, viewCross);
    double const viewScaleSquared = Dot(viewEdgeOne, viewEdgeOne) * Dot(viewEdgeTwo, viewEdgeTwo);
    if (!std::isfinite(viewAreaSquared) || !std::isfinite(viewScaleSquared))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    if (viewAreaSquared <= kMinimumTriangleConditioning * kMinimumTriangleConditioning * viewScaleSquared)
    {
        return std::unexpected(ContractError::DegenerateTriangle);
    }

    std::array<Double2, 3U> ndcPositions{};
    for (std::size_t index = 0U; index < triangle.vertices.size(); ++index)
    {
        GeometryVertex const &vertex = triangle.vertices[index];
        double const inverseW = 1.0 / static_cast<double>(vertex.clipPosition.w);
        ndcPositions[index] = {
            static_cast<double>(vertex.clipPosition.x) * inverseW,
            static_cast<double>(vertex.clipPosition.y) * inverseW,
        };
        if (!IsFinite(ndcPositions[index]))
        {
            return std::unexpected(ContractError::ArithmeticOverflow);
        }
    }

    Double2 const projectedEdgeOne = Subtract(ndcPositions[1], ndcPositions[0]);
    Double2 const projectedEdgeTwo = Subtract(ndcPositions[2], ndcPositions[0]);
    double const projectedArea = Cross(projectedEdgeOne, projectedEdgeTwo);
    double const projectedScaleSquared =
        Dot(projectedEdgeOne, projectedEdgeOne) * Dot(projectedEdgeTwo, projectedEdgeTwo);
    if (!std::isfinite(projectedArea) || !std::isfinite(projectedScaleSquared))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    if ((projectedArea * projectedArea) <=
        kMinimumTriangleConditioning * kMinimumTriangleConditioning * projectedScaleSquared)
    {
        return std::unexpected(ContractError::DegenerateTriangle);
    }
    return {};
}

[[nodiscard]] std::expected<ProjectedTriangle, ContractError> ProjectTriangle(RenderExtent extent,
                                                                              TriangleVertices const &triangle) noexcept
{
    if (std::expected<void, ContractError> const extentResult = ValidateExtent(extent); !extentResult)
    {
        return std::unexpected(extentResult.error());
    }
    if (std::expected<void, ContractError> const triangleResult = ValidateTriangle(triangle); !triangleResult)
    {
        return std::unexpected(triangleResult.error());
    }

    ProjectedTriangle result{};
    for (std::size_t index = 0U; index < triangle.vertices.size(); ++index)
    {
        Float4 const clip = triangle.vertices[index].clipPosition;
        double const inverseW = 1.0 / static_cast<double>(clip.w);
        double const ndcX = static_cast<double>(clip.x) * inverseW;
        double const ndcY = static_cast<double>(clip.y) * inverseW;
        result.positions[index] = {
            ((ndcX * 0.5) + 0.5) * static_cast<double>(extent.width),
            (0.5 - (ndcY * 0.5)) * static_cast<double>(extent.height),
        };
        if (!IsFinite(result.positions[index]))
        {
            return std::unexpected(ContractError::ArithmeticOverflow);
        }
    }
    return result;
}

[[nodiscard]] std::expected<ScreenBarycentricEvaluation, ContractError> EvaluateScreenBarycentrics(
    Float2 samplePosition, RenderExtent extent, TriangleVertices const &triangle) noexcept
{
    if (!IsFinite(samplePosition))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (std::expected<void, ContractError> const extentResult = ValidateExtent(extent); !extentResult)
    {
        return std::unexpected(extentResult.error());
    }
    if (samplePosition.x < 0.0F || samplePosition.y < 0.0F ||
        static_cast<double>(samplePosition.x) >= static_cast<double>(extent.width) ||
        static_cast<double>(samplePosition.y) >= static_cast<double>(extent.height))
    {
        return std::unexpected(ContractError::InvalidSamplePosition);
    }

    std::expected<ProjectedTriangle, ContractError> const projected = ProjectTriangle(extent, triangle);
    if (!projected)
    {
        return std::unexpected(projected.error());
    }

    Double2 const edgeOne = Subtract(projected->positions[1], projected->positions[0]);
    Double2 const edgeTwo = Subtract(projected->positions[2], projected->positions[0]);
    Double2 const fromFirst = Subtract(
        Double2{static_cast<double>(samplePosition.x), static_cast<double>(samplePosition.y)}, projected->positions[0]);
    double const denominator = Cross(edgeOne, edgeTwo);
    if (!std::isfinite(denominator) || denominator == 0.0)
    {
        return std::unexpected(ContractError::DegenerateTriangle);
    }

    double const second = Cross(fromFirst, edgeTwo) / denominator;
    double const third = Cross(edgeOne, fromFirst) / denominator;
    double const first = 1.0 - second - third;
    std::array<double, 3U> values{first, second, third};
    for (double &value : values)
    {
        if (!std::isfinite(value))
        {
            return std::unexpected(ContractError::ArithmeticOverflow);
        }
        if (value < -kInsideTriangleTolerance || value > 1.0 + kInsideTriangleTolerance)
        {
            return std::unexpected(ContractError::OutsideTriangle);
        }
        value = std::clamp(value, 0.0, 1.0);
    }
    double const sum = values[0] + values[1] + values[2];
    values[0] /= sum;
    values[1] /= sum;
    values[2] = 1.0 - values[0] - values[1];

    return ScreenBarycentricEvaluation{
        .values = values,
        .ddx =
            {
                (edgeOne.y - edgeTwo.y) / denominator,
                edgeTwo.y / denominator,
                -edgeOne.y / denominator,
            },
        .ddy =
            {
                (edgeTwo.x - edgeOne.x) / denominator,
                -edgeTwo.x / denominator,
                edgeOne.x / denominator,
            },
    };
}

[[nodiscard]] std::expected<std::array<double, 3U>, ContractError> PerspectiveWeights(
    BarycentricCoordinates screenBarycentrics, TriangleVertices const &triangle) noexcept
{
    std::expected<std::array<double, 3U>, ContractError> const barycentrics =
        CanonicalizeBarycentrics(screenBarycentrics);
    if (!barycentrics)
    {
        return std::unexpected(barycentrics.error());
    }
    if (std::expected<void, ContractError> const triangleResult = ValidateTriangle(triangle); !triangleResult)
    {
        return std::unexpected(triangleResult.error());
    }

    std::array<double, 3U> weighted{};
    double denominator = 0.0;
    for (std::size_t index = 0U; index < weighted.size(); ++index)
    {
        weighted[index] = (*barycentrics)[index] / static_cast<double>(triangle.vertices[index].clipPosition.w);
        denominator += weighted[index];
    }
    if (!std::isfinite(denominator) || denominator <= 0.0)
    {
        return std::unexpected(ContractError::InvalidClipW);
    }
    for (double &weight : weighted)
    {
        weight /= denominator;
        if (!std::isfinite(weight) || weight < 0.0 || weight > 1.0)
        {
            return std::unexpected(ContractError::ArithmeticOverflow);
        }
    }
    weighted[2] = 1.0 - weighted[0] - weighted[1];
    if (weighted[2] < 0.0 && weighted[2] >= -kInsideTriangleTolerance)
    {
        weighted[2] = 0.0;
        weighted[1] = 1.0 - weighted[0];
    }
    if (weighted[2] < 0.0)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return weighted;
}

[[nodiscard]] Double3 WeightedSum3(std::array<double, 3U> const &weights, std::array<Float3, 3U> values) noexcept
{
    Double3 result{};
    for (std::size_t index = 0U; index < weights.size(); ++index)
    {
        result.x += weights[index] * static_cast<double>(values[index].x);
        result.y += weights[index] * static_cast<double>(values[index].y);
        result.z += weights[index] * static_cast<double>(values[index].z);
    }
    return result;
}

[[nodiscard]] Double2 WeightedSum2(std::array<double, 3U> const &weights, std::array<Float2, 3U> values) noexcept
{
    Double2 result{};
    for (std::size_t index = 0U; index < weights.size(); ++index)
    {
        result.x += weights[index] * static_cast<double>(values[index].x);
        result.y += weights[index] * static_cast<double>(values[index].y);
    }
    return result;
}

[[nodiscard]] std::expected<TriangleVertices, ContractError> ResolveDecodedVisibilityTriangle(
    DecodedVisibility const &visibility, std::span<IndexedDrawRange const> draws,
    std::span<std::uint32_t const> indices, std::span<GeometryVertex const> vertices) noexcept
{
    if (visibility.isBackground)
    {
        return std::unexpected(ContractError::BackgroundVisibility);
    }

    std::expected<IndexedTriangle, ContractError> const indexedTriangle =
        CheckedIndexedTriangle(visibility.drawIdentifier, visibility.primitiveIdentifier, draws, indices, vertices);
    if (!indexedTriangle)
    {
        return std::unexpected(indexedTriangle.error());
    }

    return TriangleVertices{
        .vertices =
            {
                vertices[indexedTriangle->vertexIndices[0]],
                vertices[indexedTriangle->vertexIndices[1]],
                vertices[indexedTriangle->vertexIndices[2]],
            },
    };
}

[[nodiscard]] ContractError MapPayloadError(ch12::gbuffer::ContractError error) noexcept
{
    if (error == ch12::gbuffer::ContractError::InvalidExtent)
    {
        return ContractError::InvalidExtent;
    }
    if (error == ch12::gbuffer::ContractError::ArithmeticOverflow)
    {
        return ContractError::ArithmeticOverflow;
    }
    return ContractError::PayloadAccountingFailure;
}

[[nodiscard]] std::expected<LogicalPayloadAccounting, ContractError> MakeLogicalPayloadAccounting(
    std::uint32_t renderWidth, std::uint32_t renderHeight, std::uint32_t surfaceBytesPerPixelExcludingDepth,
    std::uint32_t depthBytesPerPixel, std::span<ch12::gbuffer::AttachmentStorage const> attachments) noexcept
{
    std::expected<std::uint32_t, ContractError> const bytesPerPixel =
        CheckedLogicalBytesPerPixel(surfaceBytesPerPixelExcludingDepth, depthBytesPerPixel);
    if (!bytesPerPixel)
    {
        return std::unexpected(bytesPerPixel.error());
    }

    std::expected<ch12::gbuffer::LogicalGBufferTraffic, ch12::gbuffer::ContractError> const traffic =
        ch12::gbuffer::ComputeLogicalTraffic(renderWidth, renderHeight, attachments);
    if (!traffic)
    {
        return std::unexpected(MapPayloadError(traffic.error()));
    }
    return LogicalPayloadAccounting{
        .logicalSurfaceBytesPerPixelExcludingDepth = surfaceBytesPerPixelExcludingDepth,
        .logicalDepthBytesPerPixel = depthBytesPerPixel,
        .logicalTotalBytesPerPixel = *bytesPerPixel,
        .logicalTraffic = *traffic,
    };
}

[[nodiscard]] std::expected<LogicalPayloadSavings, ContractError> ComputeSavings(
    LogicalPayloadAccounting const &baseline, LogicalPayloadAccounting const &visibility) noexcept
{
    if (baseline.logicalTotalBytesPerPixel < visibility.logicalTotalBytesPerPixel ||
        baseline.logicalTraffic.rasterWriteBytes < visibility.logicalTraffic.rasterWriteBytes ||
        baseline.logicalTraffic.deferredReadBytes < visibility.logicalTraffic.deferredReadBytes ||
        baseline.logicalTraffic.totalPayloadBytes < visibility.logicalTraffic.totalPayloadBytes)
    {
        return std::unexpected(ContractError::PayloadAccountingFailure);
    }
    return LogicalPayloadSavings{
        .logicalBytesPerPixel = baseline.logicalTotalBytesPerPixel - visibility.logicalTotalBytesPerPixel,
        .rasterWriteBytes = baseline.logicalTraffic.rasterWriteBytes - visibility.logicalTraffic.rasterWriteBytes,
        .materialReadBytes = baseline.logicalTraffic.deferredReadBytes - visibility.logicalTraffic.deferredReadBytes,
        .totalPayloadBytes = baseline.logicalTraffic.totalPayloadBytes - visibility.logicalTraffic.totalPayloadBytes,
    };
}

} // namespace

std::expected<QuantizedBarycentrics, ContractError> QuantizeBarycentrics(BarycentricCoordinates barycentrics) noexcept
{
    std::expected<std::array<double, 3U>, ContractError> const canonical = CanonicalizeBarycentrics(barycentrics);
    if (!canonical)
    {
        return std::unexpected(canonical.error());
    }

    double const scale = static_cast<double>(kMaximumStoredBarycentricComponent);
    double const scaledFirst = (*canonical)[0] * scale;
    double const scaledSecond = (*canonical)[1] * scale;
    std::uint32_t first = static_cast<std::uint32_t>(std::llround(scaledFirst));
    std::uint32_t second = static_cast<std::uint32_t>(std::llround(scaledSecond));
    std::uint64_t sum = static_cast<std::uint64_t>(first) + static_cast<std::uint64_t>(second);
    if (sum > kMaximumStoredBarycentricComponent)
    {
        if (sum != static_cast<std::uint64_t>(kMaximumStoredBarycentricComponent) + 1U)
        {
            return std::unexpected(ContractError::ArithmeticOverflow);
        }

        double const firstError = static_cast<double>(first) - scaledFirst;
        double const secondError = static_cast<double>(second) - scaledSecond;
        if (first > 0U && (second == 0U || firstError >= secondError))
        {
            --first;
        }
        else if (second > 0U)
        {
            --second;
        }
        else
        {
            return std::unexpected(ContractError::ArithmeticOverflow);
        }
        sum = static_cast<std::uint64_t>(first) + static_cast<std::uint64_t>(second);
    }
    if (sum > kMaximumStoredBarycentricComponent)
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return QuantizedBarycentrics{first, second};
}

std::expected<BarycentricCoordinates, ContractError> DecodeBarycentrics(QuantizedBarycentrics barycentrics) noexcept
{
    if (barycentrics.first > kMaximumStoredBarycentricComponent ||
        barycentrics.second > kMaximumStoredBarycentricComponent)
    {
        return std::unexpected(ContractError::MalformedVisibilityRecord);
    }
    std::uint64_t const sum =
        static_cast<std::uint64_t>(barycentrics.first) + static_cast<std::uint64_t>(barycentrics.second);
    if (sum > kMaximumStoredBarycentricComponent)
    {
        return std::unexpected(ContractError::MalformedVisibilityRecord);
    }

    double const inverseMaximum = 1.0 / static_cast<double>(kMaximumStoredBarycentricComponent);
    std::array<double, 3U> const values{
        static_cast<double>(barycentrics.first) * inverseMaximum,
        static_cast<double>(barycentrics.second) * inverseMaximum,
        static_cast<double>(kMaximumStoredBarycentricComponent - static_cast<std::uint32_t>(sum)) * inverseMaximum,
    };
    return ToBarycentricCoordinates(values);
}

std::expected<VisibilityRecord, ContractError> PackVisibility(std::uint32_t oneBasedDrawIdentifier,
                                                              std::uint32_t primitiveIdentifier,
                                                              BarycentricCoordinates screenBarycentrics) noexcept
{
    if (oneBasedDrawIdentifier == 0U)
    {
        return std::unexpected(ContractError::InvalidDrawIdentifier);
    }
    if (oneBasedDrawIdentifier > kMaximumDrawIdentifier || primitiveIdentifier > kMaximumPrimitiveIdentifier)
    {
        return std::unexpected(ContractError::UnrepresentableIdentifier);
    }

    std::expected<QuantizedBarycentrics, ContractError> const quantized = QuantizeBarycentrics(screenBarycentrics);
    if (!quantized)
    {
        return std::unexpected(quantized.error());
    }

    std::uint32_t const primitiveLowMask = (1U << kPrimitiveIdentifierLowBitCount) - 1U;
    std::uint32_t const primitiveLow = primitiveIdentifier & primitiveLowMask;
    std::uint32_t const primitiveHigh = primitiveIdentifier >> kPrimitiveIdentifierLowBitCount;
    return VisibilityRecord{
        .drawAndPrimitiveLowWord =
            (oneBasedDrawIdentifier << kDrawIdentifierShift) | (primitiveLow << kPrimitiveIdentifierLowShift),
        .primitiveHighAndBarycentricWord = (primitiveHigh << kPrimitiveIdentifierHighShift) |
                                           (quantized->first << kFirstBarycentricShift) |
                                           (quantized->second << kSecondBarycentricShift),
    };
}

std::expected<DecodedVisibility, ContractError> UnpackVisibility(VisibilityRecord record) noexcept
{
    if (IsBackground(record))
    {
        return DecodedVisibility{.isBackground = true};
    }

    std::uint32_t const drawMask = (1U << kDrawIdentifierBitCount) - 1U;
    std::uint32_t const primitiveLowMask = (1U << kPrimitiveIdentifierLowBitCount) - 1U;
    std::uint32_t const primitiveHighMask = (1U << kPrimitiveIdentifierHighBitCount) - 1U;
    std::uint32_t const barycentricMask = (1U << kStoredBarycentricComponentBitCount) - 1U;

    std::uint32_t const drawIdentifier = (record.drawAndPrimitiveLowWord >> kDrawIdentifierShift) & drawMask;
    if (drawIdentifier == 0U)
    {
        return std::unexpected(ContractError::MalformedVisibilityRecord);
    }

    std::uint32_t const primitiveLow =
        (record.drawAndPrimitiveLowWord >> kPrimitiveIdentifierLowShift) & primitiveLowMask;
    std::uint32_t const primitiveHigh =
        (record.primitiveHighAndBarycentricWord >> kPrimitiveIdentifierHighShift) & primitiveHighMask;
    std::uint32_t const primitiveIdentifier = primitiveLow | (primitiveHigh << kPrimitiveIdentifierLowBitCount);
    QuantizedBarycentrics const quantized{
        .first = (record.primitiveHighAndBarycentricWord >> kFirstBarycentricShift) & barycentricMask,
        .second = (record.primitiveHighAndBarycentricWord >> kSecondBarycentricShift) & barycentricMask,
    };
    std::expected<BarycentricCoordinates, ContractError> const barycentrics = DecodeBarycentrics(quantized);
    if (!barycentrics)
    {
        return std::unexpected(barycentrics.error());
    }
    return DecodedVisibility{
        .isBackground = false,
        .drawIdentifier = drawIdentifier,
        .primitiveIdentifier = primitiveIdentifier,
        .screenBarycentrics = *barycentrics,
    };
}

std::expected<IndexedTriangle, ContractError> CheckedIndexedTriangle(std::uint32_t oneBasedDrawIdentifier,
                                                                     std::uint32_t primitiveIdentifier,
                                                                     std::span<IndexedDrawRange const> draws,
                                                                     std::span<std::uint32_t const> indices,
                                                                     std::span<GeometryVertex const> vertices) noexcept
{
    if (oneBasedDrawIdentifier == 0U)
    {
        return std::unexpected(ContractError::InvalidDrawIdentifier);
    }
    if (oneBasedDrawIdentifier > kMaximumDrawIdentifier || primitiveIdentifier > kMaximumPrimitiveIdentifier)
    {
        return std::unexpected(ContractError::UnrepresentableIdentifier);
    }
    std::size_t const drawIndex = static_cast<std::size_t>(oneBasedDrawIdentifier - 1U);
    if (drawIndex >= draws.size())
    {
        return std::unexpected(ContractError::InvalidDrawIdentifier);
    }

    IndexedDrawRange const draw = draws[drawIndex];
    if (draw.vertexCount == 0U || draw.indexCount == 0U || (draw.indexCount % 3U) != 0U)
    {
        return std::unexpected(ContractError::MalformedDrawRange);
    }

    std::uint32_t vertexEnd{};
    std::uint32_t indexEnd{};
    if (!CheckedAdd(draw.vertexOffset, draw.vertexCount, vertexEnd) ||
        !CheckedAdd(draw.indexOffset, draw.indexCount, indexEnd))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    if (static_cast<std::size_t>(vertexEnd) > vertices.size() || static_cast<std::size_t>(indexEnd) > indices.size())
    {
        return std::unexpected(ContractError::MalformedDrawRange);
    }

    std::uint32_t primitiveIndexOffset{};
    if (!CheckedMultiply(primitiveIdentifier, 3U, primitiveIndexOffset))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    if (primitiveIndexOffset >= draw.indexCount)
    {
        return std::unexpected(ContractError::InvalidPrimitiveIdentifier);
    }

    std::uint32_t triangleIndexOffset{};
    if (!CheckedAdd(draw.indexOffset, primitiveIndexOffset, triangleIndexOffset))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }

    IndexedTriangle result{};
    for (std::uint32_t corner = 0U; corner < result.vertexIndices.size(); ++corner)
    {
        std::uint32_t absoluteIndexOffset{};
        if (!CheckedAdd(triangleIndexOffset, corner, absoluteIndexOffset))
        {
            return std::unexpected(ContractError::ArithmeticOverflow);
        }
        std::uint32_t const localVertexIndex = indices[absoluteIndexOffset];
        if (localVertexIndex >= draw.vertexCount)
        {
            return std::unexpected(ContractError::InvalidIndex);
        }
        if (!CheckedAdd(draw.vertexOffset, localVertexIndex, result.vertexIndices[corner]))
        {
            return std::unexpected(ContractError::ArithmeticOverflow);
        }
        if (static_cast<std::size_t>(result.vertexIndices[corner]) >= vertices.size())
        {
            return std::unexpected(ContractError::InvalidIndex);
        }
    }
    return result;
}

std::expected<TriangleVertices, ContractError> ResolveVisibilityTriangle(
    VisibilityRecord record, std::span<IndexedDrawRange const> draws, std::span<std::uint32_t const> indices,
    std::span<GeometryVertex const> vertices) noexcept
{
    std::expected<DecodedVisibility, ContractError> const visibility = UnpackVisibility(record);
    if (!visibility)
    {
        return std::unexpected(visibility.error());
    }
    return ResolveDecodedVisibilityTriangle(*visibility, draws, indices, vertices);
}

std::expected<Float2, ContractError> PixelCenter(std::uint32_t pixelX, std::uint32_t pixelY,
                                                 RenderExtent extent) noexcept
{
    if (std::expected<void, ContractError> const extentResult = ValidateExtent(extent); !extentResult)
    {
        return std::unexpected(extentResult.error());
    }
    if (pixelX >= extent.width || pixelY >= extent.height)
    {
        return std::unexpected(ContractError::InvalidSamplePosition);
    }
    return Float2{static_cast<float>(pixelX) + 0.5F, static_cast<float>(pixelY) + 0.5F};
}

std::expected<BarycentricCoordinates, ContractError> ComputeScreenBarycentrics(
    Float2 samplePosition, RenderExtent extent, TriangleVertices const &triangle) noexcept
{
    std::expected<ScreenBarycentricEvaluation, ContractError> const evaluation =
        EvaluateScreenBarycentrics(samplePosition, extent, triangle);
    if (!evaluation)
    {
        return std::unexpected(evaluation.error());
    }
    return ToBarycentricCoordinates(evaluation->values);
}

std::expected<BarycentricCoordinates, ContractError> ComputePerspectiveCorrectWeights(
    BarycentricCoordinates screenBarycentrics, TriangleVertices const &triangle) noexcept
{
    std::expected<std::array<double, 3U>, ContractError> const weights =
        PerspectiveWeights(screenBarycentrics, triangle);
    if (!weights)
    {
        return std::unexpected(weights.error());
    }
    return ToBarycentricCoordinates(*weights);
}

std::expected<Float2, ContractError> InterpolateTextureCoordinates(BarycentricCoordinates screenBarycentrics,
                                                                   TriangleVertices const &triangle) noexcept
{
    std::expected<std::array<double, 3U>, ContractError> const weights =
        PerspectiveWeights(screenBarycentrics, triangle);
    if (!weights)
    {
        return std::unexpected(weights.error());
    }

    std::array<Float2, 3U> const textureCoordinates{
        triangle.vertices[0].surface.textureCoordinates,
        triangle.vertices[1].surface.textureCoordinates,
        triangle.vertices[2].surface.textureCoordinates,
    };
    return ToFloat2(WeightedSum2(*weights, textureCoordinates));
}

std::expected<ReconstructedSurface, ContractError> ReconstructSurface(BarycentricCoordinates screenBarycentrics,
                                                                      TriangleVertices const &triangle) noexcept
{
    std::expected<std::array<double, 3U>, ContractError> const weights =
        PerspectiveWeights(screenBarycentrics, triangle);
    if (!weights)
    {
        return std::unexpected(weights.error());
    }

    std::array<Float3, 3U> const positions{
        triangle.vertices[0].surface.position,
        triangle.vertices[1].surface.position,
        triangle.vertices[2].surface.position,
    };
    std::array<Float2, 3U> const textureCoordinates{
        triangle.vertices[0].surface.textureCoordinates,
        triangle.vertices[1].surface.textureCoordinates,
        triangle.vertices[2].surface.textureCoordinates,
    };
    std::array<Float3, 3U> const normals{
        triangle.vertices[0].surface.normal,
        triangle.vertices[1].surface.normal,
        triangle.vertices[2].surface.normal,
    };
    std::array<Float3, 3U> const tangents{
        Float3{
            triangle.vertices[0].surface.tangent.x,
            triangle.vertices[0].surface.tangent.y,
            triangle.vertices[0].surface.tangent.z,
        },
        Float3{
            triangle.vertices[1].surface.tangent.x,
            triangle.vertices[1].surface.tangent.y,
            triangle.vertices[1].surface.tangent.z,
        },
        Float3{
            triangle.vertices[2].surface.tangent.x,
            triangle.vertices[2].surface.tangent.y,
            triangle.vertices[2].surface.tangent.z,
        },
    };

    std::expected<Float3, ContractError> const position = ToFloat3(WeightedSum3(*weights, positions));
    std::expected<Float2, ContractError> const uv = ToFloat2(WeightedSum2(*weights, textureCoordinates));
    std::expected<Double3, ContractError> const normal = Normalize(WeightedSum3(*weights, normals));
    if (!position)
    {
        return std::unexpected(position.error());
    }
    if (!uv)
    {
        return std::unexpected(uv.error());
    }
    if (!normal)
    {
        return std::unexpected(normal.error());
    }

    Double3 const interpolatedTangent = WeightedSum3(*weights, tangents);
    Double3 const projectedTangent =
        Subtract(interpolatedTangent, Multiply(*normal, Dot(*normal, interpolatedTangent)));
    std::expected<Double3, ContractError> const tangent = Normalize(projectedTangent);
    if (!tangent)
    {
        return std::unexpected(tangent.error());
    }

    std::expected<Float3, ContractError> const floatNormal = ToFloat3(*normal);
    std::expected<Float3, ContractError> const floatTangent = ToFloat3(*tangent);
    if (!floatNormal)
    {
        return std::unexpected(floatNormal.error());
    }
    if (!floatTangent)
    {
        return std::unexpected(floatTangent.error());
    }

    Float4 const tangentWithHandedness{
        floatTangent->x,
        floatTangent->y,
        floatTangent->z,
        triangle.vertices[0].surface.tangent.w,
    };
    std::expected<Float3, ch06::surface_frames::SurfaceFrameError> const bitangent =
        ch06::surface_frames::ReconstructBitangent(*floatNormal, tangentWithHandedness);
    if (!bitangent)
    {
        return std::unexpected(MapSurfaceFrameError(bitangent.error()));
    }

    return ReconstructedSurface{
        .viewPosition = *position,
        .textureCoordinates = *uv,
        .normal = *floatNormal,
        .tangent = tangentWithHandedness,
        .bitangent = *bitangent,
    };
}

std::expected<ReconstructedSurface, ContractError> ReconstructSurfaceAtSample(Float2 samplePosition,
                                                                              RenderExtent extent,
                                                                              TriangleVertices const &triangle) noexcept
{
    std::expected<BarycentricCoordinates, ContractError> const barycentrics =
        ComputeScreenBarycentrics(samplePosition, extent, triangle);
    if (!barycentrics)
    {
        return std::unexpected(barycentrics.error());
    }
    return ReconstructSurface(*barycentrics, triangle);
}

std::expected<ReconstructedSurface, ContractError> ReconstructSurfaceFromVisibility(
    VisibilityRecord record, std::span<IndexedDrawRange const> draws, std::span<std::uint32_t const> indices,
    std::span<GeometryVertex const> vertices) noexcept
{
    std::expected<DecodedVisibility, ContractError> const visibility = UnpackVisibility(record);
    if (!visibility)
    {
        return std::unexpected(visibility.error());
    }
    if (visibility->isBackground)
    {
        return std::unexpected(ContractError::BackgroundVisibility);
    }

    std::expected<TriangleVertices, ContractError> const triangle =
        ResolveDecodedVisibilityTriangle(*visibility, draws, indices, vertices);
    if (!triangle)
    {
        return std::unexpected(triangle.error());
    }
    return ReconstructSurface(visibility->screenBarycentrics, *triangle);
}

std::expected<TextureGradients, ContractError> ComputeAnalyticTextureGradients(
    Float2 samplePosition, RenderExtent extent, TriangleVertices const &triangle) noexcept
{
    std::expected<ScreenBarycentricEvaluation, ContractError> const evaluation =
        EvaluateScreenBarycentrics(samplePosition, extent, triangle);
    if (!evaluation)
    {
        return std::unexpected(evaluation.error());
    }

    double denominator = 0.0;
    double denominatorDdx = 0.0;
    double denominatorDdy = 0.0;
    Double2 numerator{};
    Double2 numeratorDdx{};
    Double2 numeratorDdy{};
    for (std::size_t index = 0U; index < triangle.vertices.size(); ++index)
    {
        double const reciprocalW = 1.0 / static_cast<double>(triangle.vertices[index].clipPosition.w);
        double const weightedBarycentric = evaluation->values[index] * reciprocalW;
        double const weightedDdx = evaluation->ddx[index] * reciprocalW;
        double const weightedDdy = evaluation->ddy[index] * reciprocalW;
        Float2 const uv = triangle.vertices[index].surface.textureCoordinates;

        denominator += weightedBarycentric;
        denominatorDdx += weightedDdx;
        denominatorDdy += weightedDdy;
        numerator.x += weightedBarycentric * static_cast<double>(uv.x);
        numerator.y += weightedBarycentric * static_cast<double>(uv.y);
        numeratorDdx.x += weightedDdx * static_cast<double>(uv.x);
        numeratorDdx.y += weightedDdx * static_cast<double>(uv.y);
        numeratorDdy.x += weightedDdy * static_cast<double>(uv.x);
        numeratorDdy.y += weightedDdy * static_cast<double>(uv.y);
    }

    if (!std::isfinite(denominator) || denominator <= 0.0)
    {
        return std::unexpected(ContractError::InvalidClipW);
    }
    double const denominatorSquared = denominator * denominator;
    Double2 const ddx{
        ((numeratorDdx.x * denominator) - (numerator.x * denominatorDdx)) / denominatorSquared,
        ((numeratorDdx.y * denominator) - (numerator.y * denominatorDdx)) / denominatorSquared,
    };
    Double2 const ddy{
        ((numeratorDdy.x * denominator) - (numerator.x * denominatorDdy)) / denominatorSquared,
        ((numeratorDdy.y * denominator) - (numerator.y * denominatorDdy)) / denominatorSquared,
    };
    std::expected<Float2, ContractError> const floatDdx = ToFloat2(ddx);
    std::expected<Float2, ContractError> const floatDdy = ToFloat2(ddy);
    if (!floatDdx)
    {
        return std::unexpected(floatDdx.error());
    }
    if (!floatDdy)
    {
        return std::unexpected(floatDdy.error());
    }
    return TextureGradients{*floatDdx, *floatDdy};
}

std::expected<DiscontinuityPolicy, ContractError> DetermineDiscontinuityPolicy(
    VisibilityRecord center, std::span<VisibilityRecord const> neighbors) noexcept
{
    std::expected<DecodedVisibility, ContractError> const centerVisibility = UnpackVisibility(center);
    if (!centerVisibility)
    {
        return std::unexpected(centerVisibility.error());
    }
    if (centerVisibility->isBackground)
    {
        return DiscontinuityPolicy{
            .action = MaterialEvaluationAction::SkipBackground,
            .gradientSource = TextureGradientSource::None,
            .allNeighborIdentifiersMatch = false,
        };
    }

    bool allNeighborsMatch = true;
    for (VisibilityRecord const neighbor : neighbors)
    {
        std::expected<DecodedVisibility, ContractError> const neighborVisibility = UnpackVisibility(neighbor);
        if (!neighborVisibility)
        {
            return std::unexpected(neighborVisibility.error());
        }
        if (neighborVisibility->isBackground ||
            neighborVisibility->drawIdentifier != centerVisibility->drawIdentifier ||
            neighborVisibility->primitiveIdentifier != centerVisibility->primitiveIdentifier)
        {
            allNeighborsMatch = false;
        }
    }

    return DiscontinuityPolicy{
        .action = MaterialEvaluationAction::EvaluateForeground,
        .gradientSource = TextureGradientSource::AnalyticSameTriangle,
        .allNeighborIdentifiersMatch = allNeighborsMatch,
    };
}

std::expected<std::uint32_t, ContractError> CheckedLogicalBytesPerPixel(
    std::uint32_t logicalSurfaceBytesPerPixelExcludingDepth, std::uint32_t logicalDepthBytesPerPixel) noexcept
{
    if (logicalSurfaceBytesPerPixelExcludingDepth == 0U || logicalDepthBytesPerPixel == 0U)
    {
        return std::unexpected(ContractError::InvalidLogicalStorage);
    }

    std::uint32_t total{};
    if (!CheckedAdd(logicalSurfaceBytesPerPixelExcludingDepth, logicalDepthBytesPerPixel, total))
    {
        return std::unexpected(ContractError::ArithmeticOverflow);
    }
    return total;
}

std::expected<LogicalPayloadComparison, ContractError> ComputeLogicalPayloadComparison(
    std::uint32_t renderWidth, std::uint32_t renderHeight) noexcept
{
    std::array<ch12::gbuffer::AttachmentStorage, 2U> const visibilityAttachments{
        ch12::gbuffer::AttachmentStorage{
            ch12::gbuffer::AttachmentSemantic::Identity,
            kVisibilityRecordLogicalBytesPerPixel,
        },
        ch12::gbuffer::AttachmentStorage{
            ch12::gbuffer::AttachmentSemantic::DeviceDepth,
            kExistingDepthLogicalBytesPerPixel,
        },
    };
    std::array<ch12::gbuffer::AttachmentStorage, 4U> const coreAttachments{
        ch12::gbuffer::AttachmentStorage{ch12::gbuffer::AttachmentSemantic::BaseColorMetalness, 4U},
        ch12::gbuffer::AttachmentStorage{ch12::gbuffer::AttachmentSemantic::OctahedralNormal, 4U},
        ch12::gbuffer::AttachmentStorage{ch12::gbuffer::AttachmentSemantic::Roughness, 1U},
        ch12::gbuffer::AttachmentStorage{
            ch12::gbuffer::AttachmentSemantic::DeviceDepth,
            kExistingDepthLogicalBytesPerPixel,
        },
    };
    std::array<ch12::gbuffer::AttachmentStorage, 6U> const extendedAttachments{
        ch12::gbuffer::AttachmentStorage{ch12::gbuffer::AttachmentSemantic::BaseColorMetalness, 4U},
        ch12::gbuffer::AttachmentStorage{ch12::gbuffer::AttachmentSemantic::OctahedralNormal, 4U},
        ch12::gbuffer::AttachmentStorage{ch12::gbuffer::AttachmentSemantic::Roughness, 1U},
        ch12::gbuffer::AttachmentStorage{
            ch12::gbuffer::AttachmentSemantic::DeviceDepth,
            kExistingDepthLogicalBytesPerPixel,
        },
        ch12::gbuffer::AttachmentStorage{ch12::gbuffer::AttachmentSemantic::Motion, 4U},
        ch12::gbuffer::AttachmentStorage{ch12::gbuffer::AttachmentSemantic::Identity, 4U},
    };

    std::expected<LogicalPayloadAccounting, ContractError> const visibility =
        MakeLogicalPayloadAccounting(renderWidth, renderHeight, kVisibilityRecordLogicalBytesPerPixel,
                                     kExistingDepthLogicalBytesPerPixel, visibilityAttachments);
    std::expected<LogicalPayloadAccounting, ContractError> const core =
        MakeLogicalPayloadAccounting(renderWidth, renderHeight, kChapter12CoreSurfaceLogicalBytesPerPixelExcludingDepth,
                                     kExistingDepthLogicalBytesPerPixel, coreAttachments);
    std::expected<LogicalPayloadAccounting, ContractError> const extended = MakeLogicalPayloadAccounting(
        renderWidth, renderHeight, kChapter12ExtendedSurfaceLogicalBytesPerPixelExcludingDepth,
        kExistingDepthLogicalBytesPerPixel, extendedAttachments);
    if (!visibility)
    {
        return std::unexpected(visibility.error());
    }
    if (!core)
    {
        return std::unexpected(core.error());
    }
    if (!extended)
    {
        return std::unexpected(extended.error());
    }

    std::expected<LogicalPayloadSavings, ContractError> const versusCore = ComputeSavings(*core, *visibility);
    std::expected<LogicalPayloadSavings, ContractError> const versusExtended = ComputeSavings(*extended, *visibility);
    if (!versusCore)
    {
        return std::unexpected(versusCore.error());
    }
    if (!versusExtended)
    {
        return std::unexpected(versusExtended.error());
    }
    return LogicalPayloadComparison{
        .visibilityAndDepth = *visibility,
        .chapter12Core = *core,
        .chapter12Extended = *extended,
        .visibilitySavingsVersusCore = *versusCore,
        .visibilitySavingsVersusExtended = *versusExtended,
    };
}

} // namespace ch15::visibility_buffer
