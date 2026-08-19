#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace ch06::surface_frames
{

struct Float2 final
{
    float x{};
    float y{};

    [[nodiscard]] constexpr bool operator==(Float2 const &) const noexcept = default;
};

struct Float3 final
{
    float x{};
    float y{};
    float z{};

    [[nodiscard]] constexpr bool operator==(Float3 const &) const noexcept = default;
};

struct Float4 final
{
    float x{};
    float y{};
    float z{};
    float w{};

    [[nodiscard]] constexpr bool operator==(Float4 const &) const noexcept = default;
};

struct TangentInputVertex final
{
    Float3 position{};
    Float3 normal{};
    Float2 textureCoordinates{};
};

struct SurfaceVertex final
{
    Float3 position{};
    Float3 normal{};
    Float2 textureCoordinates{};
    Float4 tangent{};
};

enum class MaterialTextureSemantic : std::uint8_t
{
    BaseColor = 0,
    Roughness,
    Metalness,
    TangentSpaceNormal,
};

enum class TextureTransferFunction : std::uint8_t
{
    Linear = 0,
    Srgb,
};

struct MaterialTextureDeclaration final
{
    MaterialTextureSemantic semantic{};
    TextureTransferFunction transferFunction{};
};

struct BaseColorSrgbSample final
{
    Float3 encodedSrgb{};
};

struct RoughnessLinearSample final
{
    float value{};
};

struct MetalnessLinearSample final
{
    float value{};
};

struct TangentNormalLinearSample final
{
    Float3 encoded{};
};

struct TriangleTangentFrame final
{
    Float3 tangent{};
    Float3 bitangent{};
};

enum class GreenChannelConvention : std::uint8_t
{
    PositiveY = 0,
    InvertedY,
};

enum class SurfaceFrameError : std::uint8_t
{
    NonFiniteInput = 0,
    ValueOutOfRange,
    InvalidSemantic,
    IncorrectTransferFunction,
    DegenerateGeometry,
    DegenerateUv,
    InvalidIndexCount,
    InvalidIndex,
    DegenerateNormal,
    DegenerateTangent,
    MixedUvHandedness,
    InvalidHandedness,
    InvalidGreenConvention,
    InvalidStrength,
    ArithmeticOverflow,
};

[[nodiscard]] constexpr TextureTransferFunction RequiredTransferFunction(MaterialTextureSemantic semantic) noexcept
{
    return semantic == MaterialTextureSemantic::BaseColor ? TextureTransferFunction::Srgb
                                                          : TextureTransferFunction::Linear;
}

[[nodiscard]] std::expected<void, SurfaceFrameError> ValidateMaterialTexture(
    MaterialTextureDeclaration declaration) noexcept;
[[nodiscard]] std::expected<void, SurfaceFrameError> ValidateSample(BaseColorSrgbSample sample) noexcept;
[[nodiscard]] std::expected<void, SurfaceFrameError> ValidateSample(RoughnessLinearSample sample) noexcept;
[[nodiscard]] std::expected<void, SurfaceFrameError> ValidateSample(MetalnessLinearSample sample) noexcept;
[[nodiscard]] std::expected<void, SurfaceFrameError> ValidateSample(TangentNormalLinearSample sample) noexcept;
[[nodiscard]] std::expected<void, SurfaceFrameError> ValidateTangentInputVertex(TangentInputVertex vertex) noexcept;
[[nodiscard]] std::expected<void, SurfaceFrameError> ValidateSurfaceVertex(SurfaceVertex vertex) noexcept;

[[nodiscard]] std::expected<TriangleTangentFrame, SurfaceFrameError> DeriveTriangleTangent(
    TangentInputVertex first, TangentInputVertex second, TangentInputVertex third) noexcept;

// Educational chapter convention, not MikkTSpace. Generated content must use this builder's convention.
// Vertices on mirrored UV seams must be duplicated so each vertex has one UV handedness.
[[nodiscard]] std::expected<std::vector<Float4>, SurfaceFrameError> BuildMeshTangents(
    std::span<TangentInputVertex const> vertices, std::span<std::uint32_t const> indices);

[[nodiscard]] std::expected<Float3, SurfaceFrameError> ReconstructBitangent(Float3 normal, Float4 tangent) noexcept;
[[nodiscard]] std::expected<Float3, SurfaceFrameError> DecodeTangentSpaceNormal(TangentNormalLinearSample sample,
                                                                                GreenChannelConvention greenConvention,
                                                                                float strength) noexcept;
[[nodiscard]] std::expected<Float3, SurfaceFrameError> TransformTangentSpaceNormal(Float3 tangentSpaceNormal,
                                                                                   Float3 surfaceNormal,
                                                                                   Float4 tangent) noexcept;

} // namespace ch06::surface_frames
