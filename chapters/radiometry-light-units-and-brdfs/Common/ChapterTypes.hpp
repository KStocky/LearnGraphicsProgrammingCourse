#pragma once

#include <cstdint>

namespace ch05::lighting
{

struct LinearRgb final
{
    float r{};
    float g{};
    float b{};

    [[nodiscard]] constexpr bool operator==(LinearRgb const &) const noexcept = default;
};

struct Float3 final
{
    float x{};
    float y{};
    float z{};

    [[nodiscard]] constexpr bool operator==(Float3 const &) const noexcept = default;
};

// A unit-length direction produced by Normalize. Public fields keep the type easy
// to share with later shader-facing chapter code; every public calculation still
// validates that manually constructed values are finite and normalized.
struct UnitDirection3 final
{
    float x{};
    float y{};
    float z{};

    [[nodiscard]] constexpr bool operator==(UnitDirection3 const &) const noexcept = default;
};

// RGB lighting in this chapter is a scene-linear, three-channel approximation,
// not a spectral photometric model. A neutral {1, 1, 1} tint applies the scalar
// lux or candela control equally to all channels. Colored tints scale those
// channels for rendering but do not represent exact human luminous efficiency.
struct DirectionalLight final
{
    UnitDirection3 directionToLight{};
    float normalIlluminanceLux{};
    LinearRgb color{1.0F, 1.0F, 1.0F};
};

struct PointLight final
{
    Float3 position{};
    float luminousIntensityCandela{};
    LinearRgb color{1.0F, 1.0F, 1.0F};
};

struct MaterialParameters final
{
    LinearRgb baseColor{};
    float roughness{0.5F};
    float metalness{};
    LinearRgb dielectricF0{0.04F, 0.04F, 0.04F};
};

enum class LightingError : std::uint8_t
{
    NonFiniteVector = 0,
    ZeroLengthDirection,
    DirectionNotNormalized,
    InvalidBaseColor,
    InvalidRoughness,
    InvalidMetalness,
    InvalidDielectricF0,
    InvalidLightColor,
    InvalidIlluminance,
    InvalidLuminousIntensity,
    CoincidentPointLight,
    ArithmeticOverflow,
};

struct IncidentLight final
{
    UnitDirection3 directionToLight{};
    float illuminanceLux{};
    LinearRgb illuminanceRgb{};
};

struct BrdfDiagnosticTerms final
{
    float nDotL{};
    float nDotV{};
    float nDotH{};
    float vDotH{};
    float effectiveRoughness{};
    LinearRgb f0{};
    LinearRgb fresnel{};
    float normalDistribution{};
    float maskingShadowing{};
    LinearRgb diffuseWeight{};
    LinearRgb diffuseBrdf{};
    LinearRgb specularBrdf{};
    LinearRgb combinedBrdf{};
};

} // namespace ch05::lighting
