#include "SurfaceFrame.hpp"

#include <cmath>
#include <cstddef>
#include <limits>

namespace ch06::surface_frames
{
namespace
{

inline constexpr double kMinimumConditioning = static_cast<double>(std::numeric_limits<float>::epsilon());
inline constexpr double kMinimumConditioningSquared = kMinimumConditioning * kMinimumConditioning;
inline constexpr double kUnitTolerance = 1.0e-4;
inline constexpr double kOrthogonalTolerance = 1.0e-4;

struct Double3 final
{
    double x{};
    double y{};
    double z{};
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

[[nodiscard]] bool IsUnitInterval(float value) noexcept
{
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] bool IsUnitInterval(Float3 value) noexcept
{
    return IsUnitInterval(value.x) && IsUnitInterval(value.y) && IsUnitInterval(value.z);
}

[[nodiscard]] Double3 ToDouble3(Float3 value) noexcept
{
    return {static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z)};
}

[[nodiscard]] Double3 Add(Double3 first, Double3 second) noexcept
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

[[nodiscard]] Double3 Subtract(Double3 first, Double3 second) noexcept
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

[[nodiscard]] Double3 Multiply(Double3 value, double scalar) noexcept
{
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] double Dot(Double3 first, Double3 second) noexcept
{
    return (first.x * second.x) + (first.y * second.y) + (first.z * second.z);
}

[[nodiscard]] Double3 Cross(Double3 first, Double3 second) noexcept
{
    return {
        (first.y * second.z) - (first.z * second.y),
        (first.z * second.x) - (first.x * second.z),
        (first.x * second.y) - (first.y * second.x),
    };
}

[[nodiscard]] bool IsFinite(Double3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] std::expected<Double3, SurfaceFrameError> Normalize(Double3 value,
                                                                  SurfaceFrameError degenerateError) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(SurfaceFrameError::ArithmeticOverflow);
    }

    double const lengthSquared = Dot(value, value);
    if (!std::isfinite(lengthSquared))
    {
        return std::unexpected(SurfaceFrameError::ArithmeticOverflow);
    }
    if (lengthSquared == 0.0)
    {
        return std::unexpected(degenerateError);
    }

    return Multiply(value, 1.0 / std::sqrt(lengthSquared));
}

[[nodiscard]] std::expected<Float3, SurfaceFrameError> ToFloat3(Double3 value) noexcept
{
    Float3 const result{
        static_cast<float>(value.x),
        static_cast<float>(value.y),
        static_cast<float>(value.z),
    };
    if (!IsFinite(result))
    {
        return std::unexpected(SurfaceFrameError::ArithmeticOverflow);
    }
    return result;
}

[[nodiscard]] std::expected<void, SurfaceFrameError> ValidateUnitDirection(Float3 value,
                                                                           SurfaceFrameError degenerateError) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(SurfaceFrameError::NonFiniteInput);
    }
    double const lengthSquared = Dot(ToDouble3(value), ToDouble3(value));
    if (lengthSquared == 0.0)
    {
        return std::unexpected(degenerateError);
    }
    if (std::abs(lengthSquared - 1.0) > kUnitTolerance)
    {
        return std::unexpected(degenerateError);
    }
    return {};
}

} // namespace

std::expected<void, SurfaceFrameError> ValidateMaterialTexture(MaterialTextureDeclaration declaration) noexcept
{
    switch (declaration.semantic)
    {
    case MaterialTextureSemantic::BaseColor:
    case MaterialTextureSemantic::Roughness:
    case MaterialTextureSemantic::Metalness:
    case MaterialTextureSemantic::TangentSpaceNormal:
        break;
    default:
        return std::unexpected(SurfaceFrameError::InvalidSemantic);
    }
    if (declaration.transferFunction != RequiredTransferFunction(declaration.semantic))
    {
        return std::unexpected(SurfaceFrameError::IncorrectTransferFunction);
    }
    return {};
}

std::expected<void, SurfaceFrameError> ValidateSample(BaseColorSrgbSample sample) noexcept
{
    if (!IsFinite(sample.encodedSrgb))
    {
        return std::unexpected(SurfaceFrameError::NonFiniteInput);
    }
    if (!IsUnitInterval(sample.encodedSrgb))
    {
        return std::unexpected(SurfaceFrameError::ValueOutOfRange);
    }
    return {};
}

std::expected<void, SurfaceFrameError> ValidateSample(RoughnessLinearSample sample) noexcept
{
    if (!std::isfinite(sample.value))
    {
        return std::unexpected(SurfaceFrameError::NonFiniteInput);
    }
    if (!IsUnitInterval(sample.value))
    {
        return std::unexpected(SurfaceFrameError::ValueOutOfRange);
    }
    return {};
}

std::expected<void, SurfaceFrameError> ValidateSample(MetalnessLinearSample sample) noexcept
{
    if (!std::isfinite(sample.value))
    {
        return std::unexpected(SurfaceFrameError::NonFiniteInput);
    }
    if (!IsUnitInterval(sample.value))
    {
        return std::unexpected(SurfaceFrameError::ValueOutOfRange);
    }
    return {};
}

std::expected<void, SurfaceFrameError> ValidateSample(TangentNormalLinearSample sample) noexcept
{
    if (!IsFinite(sample.encoded))
    {
        return std::unexpected(SurfaceFrameError::NonFiniteInput);
    }
    if (!IsUnitInterval(sample.encoded))
    {
        return std::unexpected(SurfaceFrameError::ValueOutOfRange);
    }
    return {};
}

std::expected<void, SurfaceFrameError> ValidateTangentInputVertex(TangentInputVertex vertex) noexcept
{
    if (!IsFinite(vertex.position) || !IsFinite(vertex.normal) || !IsFinite(vertex.textureCoordinates))
    {
        return std::unexpected(SurfaceFrameError::NonFiniteInput);
    }
    return ValidateUnitDirection(vertex.normal, SurfaceFrameError::DegenerateNormal);
}

std::expected<void, SurfaceFrameError> ValidateSurfaceVertex(SurfaceVertex vertex) noexcept
{
    TangentInputVertex const input{vertex.position, vertex.normal, vertex.textureCoordinates};
    if (std::expected<void, SurfaceFrameError> const inputResult = ValidateTangentInputVertex(input); !inputResult)
    {
        return inputResult;
    }
    if (!IsFinite(vertex.tangent))
    {
        return std::unexpected(SurfaceFrameError::NonFiniteInput);
    }
    if (vertex.tangent.w != -1.0F && vertex.tangent.w != 1.0F)
    {
        return std::unexpected(SurfaceFrameError::InvalidHandedness);
    }
    Float3 const tangent{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z};
    if (std::expected<void, SurfaceFrameError> const tangentResult =
            ValidateUnitDirection(tangent, SurfaceFrameError::DegenerateTangent);
        !tangentResult)
    {
        return tangentResult;
    }
    if (std::abs(Dot(ToDouble3(vertex.normal), ToDouble3(tangent))) > kOrthogonalTolerance)
    {
        return std::unexpected(SurfaceFrameError::DegenerateTangent);
    }
    return {};
}

std::expected<TriangleTangentFrame, SurfaceFrameError> DeriveTriangleTangent(TangentInputVertex first,
                                                                             TangentInputVertex second,
                                                                             TangentInputVertex third) noexcept
{
    for (TangentInputVertex const vertex : {first, second, third})
    {
        if (!IsFinite(vertex.position) || !IsFinite(vertex.textureCoordinates))
        {
            return std::unexpected(SurfaceFrameError::NonFiniteInput);
        }
    }

    Double3 const edgeOne = Subtract(ToDouble3(second.position), ToDouble3(first.position));
    Double3 const edgeTwo = Subtract(ToDouble3(third.position), ToDouble3(first.position));
    Double3 const geometricNormal = Cross(edgeOne, edgeTwo);
    double const edgeOneLengthSquared = Dot(edgeOne, edgeOne);
    double const edgeTwoLengthSquared = Dot(edgeTwo, edgeTwo);
    double const geometricAreaSquared = Dot(geometricNormal, geometricNormal);
    double const geometricScaleSquared = edgeOneLengthSquared * edgeTwoLengthSquared;
    if (!std::isfinite(geometricAreaSquared) || !std::isfinite(geometricScaleSquared))
    {
        return std::unexpected(SurfaceFrameError::ArithmeticOverflow);
    }
    if (geometricAreaSquared <= kMinimumConditioningSquared * geometricScaleSquared)
    {
        return std::unexpected(SurfaceFrameError::DegenerateGeometry);
    }

    double const deltaUOne =
        static_cast<double>(second.textureCoordinates.x) - static_cast<double>(first.textureCoordinates.x);
    double const deltaVOne =
        static_cast<double>(second.textureCoordinates.y) - static_cast<double>(first.textureCoordinates.y);
    double const deltaUTwo =
        static_cast<double>(third.textureCoordinates.x) - static_cast<double>(first.textureCoordinates.x);
    double const deltaVTwo =
        static_cast<double>(third.textureCoordinates.y) - static_cast<double>(first.textureCoordinates.y);
    double const determinant = (deltaUOne * deltaVTwo) - (deltaUTwo * deltaVOne);
    double const uvEdgeOneLengthSquared = (deltaUOne * deltaUOne) + (deltaVOne * deltaVOne);
    double const uvEdgeTwoLengthSquared = (deltaUTwo * deltaUTwo) + (deltaVTwo * deltaVTwo);
    double const uvScaleSquared = uvEdgeOneLengthSquared * uvEdgeTwoLengthSquared;
    double const determinantSquared = determinant * determinant;
    if (!std::isfinite(determinantSquared) || !std::isfinite(uvScaleSquared))
    {
        return std::unexpected(SurfaceFrameError::ArithmeticOverflow);
    }
    if (determinantSquared <= kMinimumConditioningSquared * uvScaleSquared)
    {
        return std::unexpected(SurfaceFrameError::DegenerateUv);
    }

    double const inverseDeterminant = 1.0 / determinant;
    Double3 const tangent =
        Multiply(Subtract(Multiply(edgeOne, deltaVTwo), Multiply(edgeTwo, deltaVOne)), inverseDeterminant);
    Double3 const bitangent =
        Multiply(Subtract(Multiply(edgeTwo, deltaUOne), Multiply(edgeOne, deltaUTwo)), inverseDeterminant);
    std::expected<Float3, SurfaceFrameError> const floatTangent = ToFloat3(tangent);
    std::expected<Float3, SurfaceFrameError> const floatBitangent = ToFloat3(bitangent);
    if (!floatTangent)
    {
        return std::unexpected(floatTangent.error());
    }
    if (!floatBitangent)
    {
        return std::unexpected(floatBitangent.error());
    }
    return TriangleTangentFrame{*floatTangent, *floatBitangent};
}

std::expected<std::vector<Float4>, SurfaceFrameError> BuildMeshTangents(std::span<TangentInputVertex const> vertices,
                                                                        std::span<std::uint32_t const> indices)
{
    if (indices.empty() || (indices.size() % 3U) != 0U)
    {
        return std::unexpected(SurfaceFrameError::InvalidIndexCount);
    }
    for (TangentInputVertex const vertex : vertices)
    {
        if (std::expected<void, SurfaceFrameError> const result = ValidateTangentInputVertex(vertex); !result)
        {
            return std::unexpected(result.error());
        }
    }

    std::vector<Double3> accumulatedTangents(vertices.size());
    std::vector<Double3> accumulatedBitangents(vertices.size());
    std::vector<std::int8_t> accumulatedHandedness(vertices.size());
    for (std::size_t index = 0U; index < indices.size(); index += 3U)
    {
        std::uint32_t const firstIndex = indices[index];
        std::uint32_t const secondIndex = indices[index + 1U];
        std::uint32_t const thirdIndex = indices[index + 2U];
        if (firstIndex >= vertices.size() || secondIndex >= vertices.size() || thirdIndex >= vertices.size())
        {
            return std::unexpected(SurfaceFrameError::InvalidIndex);
        }

        std::expected<TriangleTangentFrame, SurfaceFrameError> const triangle =
            DeriveTriangleTangent(vertices[firstIndex], vertices[secondIndex], vertices[thirdIndex]);
        if (!triangle)
        {
            return std::unexpected(triangle.error());
        }

        for (std::uint32_t const vertexIndex : {firstIndex, secondIndex, thirdIndex})
        {
            double const orientation = Dot(Cross(ToDouble3(vertices[vertexIndex].normal), ToDouble3(triangle->tangent)),
                                           ToDouble3(triangle->bitangent));
            if (!std::isfinite(orientation))
            {
                return std::unexpected(SurfaceFrameError::ArithmeticOverflow);
            }
            if (orientation == 0.0)
            {
                return std::unexpected(SurfaceFrameError::DegenerateTangent);
            }

            std::int8_t const handedness = orientation < 0.0 ? -1 : 1;
            if (accumulatedHandedness[vertexIndex] != 0 && accumulatedHandedness[vertexIndex] != handedness)
            {
                return std::unexpected(SurfaceFrameError::MixedUvHandedness);
            }
            accumulatedHandedness[vertexIndex] = handedness;
            accumulatedTangents[vertexIndex] = Add(accumulatedTangents[vertexIndex], ToDouble3(triangle->tangent));
            accumulatedBitangents[vertexIndex] =
                Add(accumulatedBitangents[vertexIndex], ToDouble3(triangle->bitangent));
        }
    }

    std::vector<Float4> tangents;
    tangents.reserve(vertices.size());
    for (std::size_t vertexIndex = 0U; vertexIndex < vertices.size(); ++vertexIndex)
    {
        std::expected<Double3, SurfaceFrameError> const normal =
            Normalize(ToDouble3(vertices[vertexIndex].normal), SurfaceFrameError::DegenerateNormal);
        if (!normal)
        {
            return std::unexpected(normal.error());
        }

        Double3 const projectedTangent = Subtract(accumulatedTangents[vertexIndex],
                                                  Multiply(*normal, Dot(*normal, accumulatedTangents[vertexIndex])));
        std::expected<Double3, SurfaceFrameError> const tangent =
            Normalize(projectedTangent, SurfaceFrameError::DegenerateTangent);
        if (!tangent)
        {
            return std::unexpected(tangent.error());
        }

        double const orientation = Dot(Cross(*normal, *tangent), accumulatedBitangents[vertexIndex]);
        if (!std::isfinite(orientation))
        {
            return std::unexpected(SurfaceFrameError::ArithmeticOverflow);
        }
        if (orientation == 0.0)
        {
            return std::unexpected(SurfaceFrameError::DegenerateTangent);
        }

        std::expected<Float3, SurfaceFrameError> const floatTangent = ToFloat3(*tangent);
        if (!floatTangent)
        {
            return std::unexpected(floatTangent.error());
        }
        tangents.push_back(Float4{floatTangent->x, floatTangent->y, floatTangent->z, orientation < 0.0 ? -1.0F : 1.0F});
    }
    return tangents;
}

std::expected<Float3, SurfaceFrameError> ReconstructBitangent(Float3 normal, Float4 tangent) noexcept
{
    SurfaceVertex const vertex{{}, normal, {}, tangent};
    if (std::expected<void, SurfaceFrameError> const result = ValidateSurfaceVertex(vertex); !result)
    {
        return std::unexpected(result.error());
    }
    Double3 const reconstructed =
        Multiply(Cross(ToDouble3(normal), Double3{tangent.x, tangent.y, tangent.z}), static_cast<double>(tangent.w));
    return ToFloat3(reconstructed);
}

std::expected<Float3, SurfaceFrameError> DecodeTangentSpaceNormal(TangentNormalLinearSample sample,
                                                                  GreenChannelConvention greenConvention,
                                                                  float strength) noexcept
{
    if (std::expected<void, SurfaceFrameError> const sampleResult = ValidateSample(sample); !sampleResult)
    {
        return std::unexpected(sampleResult.error());
    }
    if (!std::isfinite(strength))
    {
        return std::unexpected(SurfaceFrameError::NonFiniteInput);
    }
    if (strength < 0.0F || strength > 1.0F)
    {
        return std::unexpected(SurfaceFrameError::InvalidStrength);
    }

    double const decodedX = (static_cast<double>(sample.encoded.x) * 2.0) - 1.0;
    double decodedY = (static_cast<double>(sample.encoded.y) * 2.0) - 1.0;
    double const decodedZ = (static_cast<double>(sample.encoded.z) * 2.0) - 1.0;
    switch (greenConvention)
    {
    case GreenChannelConvention::PositiveY:
        break;
    case GreenChannelConvention::InvertedY:
        decodedY = -decodedY;
        break;
    default:
        return std::unexpected(SurfaceFrameError::InvalidGreenConvention);
    }

    Double3 const strengthened{
        decodedX * static_cast<double>(strength),
        decodedY * static_cast<double>(strength),
        1.0 + ((decodedZ - 1.0) * static_cast<double>(strength)),
    };
    std::expected<Double3, SurfaceFrameError> const normalized =
        Normalize(strengthened, SurfaceFrameError::DegenerateNormal);
    if (!normalized)
    {
        return std::unexpected(normalized.error());
    }
    return ToFloat3(*normalized);
}

std::expected<Float3, SurfaceFrameError> TransformTangentSpaceNormal(Float3 tangentSpaceNormal, Float3 surfaceNormal,
                                                                     Float4 tangent) noexcept
{
    if (std::expected<void, SurfaceFrameError> const normalResult =
            ValidateUnitDirection(tangentSpaceNormal, SurfaceFrameError::DegenerateNormal);
        !normalResult)
    {
        return std::unexpected(normalResult.error());
    }
    std::expected<Float3, SurfaceFrameError> const bitangent = ReconstructBitangent(surfaceNormal, tangent);
    if (!bitangent)
    {
        return std::unexpected(bitangent.error());
    }

    Double3 const transformed =
        Add(Add(Multiply(Double3{tangent.x, tangent.y, tangent.z}, static_cast<double>(tangentSpaceNormal.x)),
                Multiply(ToDouble3(*bitangent), static_cast<double>(tangentSpaceNormal.y))),
            Multiply(ToDouble3(surfaceNormal), static_cast<double>(tangentSpaceNormal.z)));
    std::expected<Double3, SurfaceFrameError> const normalized =
        Normalize(transformed, SurfaceFrameError::DegenerateNormal);
    if (!normalized)
    {
        return std::unexpected(normalized.error());
    }
    return ToFloat3(*normalized);
}

} // namespace ch06::surface_frames
