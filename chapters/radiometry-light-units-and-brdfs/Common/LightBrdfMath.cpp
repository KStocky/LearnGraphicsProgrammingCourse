#include "LightBrdfMath.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ch05::lighting
{
namespace
{

inline constexpr float kPi = 3.14159265358979323846F;
inline constexpr float kDirectionLengthTolerance = 1.0e-4F;

[[nodiscard]] bool IsFinite(Float3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool IsFinite(LinearRgb value) noexcept
{
    return std::isfinite(value.r) && std::isfinite(value.g) && std::isfinite(value.b);
}

[[nodiscard]] bool IsInRange(LinearRgb value, float minimum, float maximum) noexcept
{
    return IsFinite(value) && value.r >= minimum && value.r <= maximum && value.g >= minimum && value.g <= maximum &&
           value.b >= minimum && value.b <= maximum;
}

[[nodiscard]] bool IsNonNegative(LinearRgb value) noexcept
{
    return IsFinite(value) && value.r >= 0.0F && value.g >= 0.0F && value.b >= 0.0F;
}

[[nodiscard]] Float3 ToFloat3(UnitDirection3 value) noexcept
{
    return {value.x, value.y, value.z};
}

[[nodiscard]] std::expected<void, LightingError> ValidateDirection(UnitDirection3 value) noexcept
{
    Float3 const direction = ToFloat3(value);
    if (!IsFinite(direction))
    {
        return std::unexpected(LightingError::NonFiniteVector);
    }

    float const lengthSquared = (direction.x * direction.x) + (direction.y * direction.y) + (direction.z * direction.z);
    if (std::abs(lengthSquared - 1.0F) > kDirectionLengthTolerance)
    {
        return std::unexpected(LightingError::DirectionNotNormalized);
    }
    return {};
}

[[nodiscard]] float Dot(UnitDirection3 first, UnitDirection3 second) noexcept
{
    return (first.x * second.x) + (first.y * second.y) + (first.z * second.z);
}

[[nodiscard]] LinearRgb Add(LinearRgb first, LinearRgb second) noexcept
{
    return {first.r + second.r, first.g + second.g, first.b + second.b};
}

[[nodiscard]] LinearRgb Multiply(LinearRgb first, LinearRgb second) noexcept
{
    return {first.r * second.r, first.g * second.g, first.b * second.b};
}

[[nodiscard]] LinearRgb Multiply(LinearRgb value, float scalar) noexcept
{
    return {value.r * scalar, value.g * scalar, value.b * scalar};
}

[[nodiscard]] LinearRgb Lerp(LinearRgb first, LinearRgb second, float amount) noexcept
{
    return {
        first.r + ((second.r - first.r) * amount),
        first.g + ((second.g - first.g) * amount),
        first.b + ((second.b - first.b) * amount),
    };
}

[[nodiscard]] float SmithGgxG1(float nDotDirection, float roughness) noexcept
{
    float const cosine = std::clamp(nDotDirection, 0.0F, 1.0F);
    if (cosine <= 0.0F)
    {
        return 0.0F;
    }

    float const effectiveRoughness = std::max(roughness, kMinimumRoughness);
    float const alpha = effectiveRoughness * effectiveRoughness;
    float const alphaSquared = alpha * alpha;
    float const root = std::sqrt(alphaSquared + ((1.0F - alphaSquared) * cosine * cosine));
    return (2.0F * cosine) / (cosine + root);
}

[[nodiscard]] BrdfDiagnosticTerms EmptyTerms(float nDotL, float nDotV, MaterialParameters const &material) noexcept
{
    BrdfDiagnosticTerms terms{};
    terms.nDotL = nDotL;
    terms.nDotV = nDotV;
    terms.effectiveRoughness = std::max(material.roughness, kMinimumRoughness);
    terms.f0 = DielectricMetalF0(material);
    return terms;
}

} // namespace

std::expected<UnitDirection3, LightingError> Normalize(Float3 value) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(LightingError::NonFiniteVector);
    }

    double const lengthSquared = (static_cast<double>(value.x) * static_cast<double>(value.x)) +
                                 (static_cast<double>(value.y) * static_cast<double>(value.y)) +
                                 (static_cast<double>(value.z) * static_cast<double>(value.z));
    if (lengthSquared <= 0.0)
    {
        return std::unexpected(LightingError::ZeroLengthDirection);
    }

    double const inverseLength = 1.0 / std::sqrt(lengthSquared);
    return UnitDirection3{
        static_cast<float>(static_cast<double>(value.x) * inverseLength),
        static_cast<float>(static_cast<double>(value.y) * inverseLength),
        static_cast<float>(static_cast<double>(value.z) * inverseLength),
    };
}

float ClampedDot(UnitDirection3 first, UnitDirection3 second) noexcept
{
    return std::clamp(Dot(first, second), 0.0F, 1.0F);
}

std::expected<void, LightingError> ValidateMaterial(MaterialParameters const &material) noexcept
{
    if (!IsInRange(material.baseColor, 0.0F, 1.0F))
    {
        return std::unexpected(LightingError::InvalidBaseColor);
    }
    if (!std::isfinite(material.roughness) || material.roughness < 0.0F || material.roughness > 1.0F)
    {
        return std::unexpected(LightingError::InvalidRoughness);
    }
    if (!std::isfinite(material.metalness) || material.metalness < 0.0F || material.metalness > 1.0F)
    {
        return std::unexpected(LightingError::InvalidMetalness);
    }
    if (!IsInRange(material.dielectricF0, 0.0F, 1.0F))
    {
        return std::unexpected(LightingError::InvalidDielectricF0);
    }
    return {};
}

std::expected<void, LightingError> ValidateDirectionalLight(DirectionalLight const &light) noexcept
{
    if (std::expected<void, LightingError> const directionResult = ValidateDirection(light.directionToLight);
        !directionResult)
    {
        return directionResult;
    }
    if (!std::isfinite(light.normalIlluminanceLux) || light.normalIlluminanceLux < 0.0F)
    {
        return std::unexpected(LightingError::InvalidIlluminance);
    }
    if (!IsNonNegative(light.color))
    {
        return std::unexpected(LightingError::InvalidLightColor);
    }
    return {};
}

std::expected<void, LightingError> ValidatePointLight(PointLight const &light) noexcept
{
    if (!IsFinite(light.position))
    {
        return std::unexpected(LightingError::NonFiniteVector);
    }
    if (!std::isfinite(light.luminousIntensityCandela) || light.luminousIntensityCandela < 0.0F)
    {
        return std::unexpected(LightingError::InvalidLuminousIntensity);
    }
    if (!IsNonNegative(light.color))
    {
        return std::unexpected(LightingError::InvalidLightColor);
    }
    return {};
}

std::expected<IncidentLight, LightingError> EvaluateDirectionalLight(DirectionalLight const &light,
                                                                     UnitDirection3 surfaceNormal) noexcept
{
    if (std::expected<void, LightingError> const lightResult = ValidateDirectionalLight(light); !lightResult)
    {
        return std::unexpected(lightResult.error());
    }
    if (std::expected<void, LightingError> const normalResult = ValidateDirection(surfaceNormal); !normalResult)
    {
        return std::unexpected(normalResult.error());
    }

    float const illuminanceLux = light.normalIlluminanceLux * ClampedDot(surfaceNormal, light.directionToLight);
    LinearRgb const illuminanceRgb = Multiply(light.color, illuminanceLux);
    if (!IsFinite(illuminanceRgb))
    {
        return std::unexpected(LightingError::ArithmeticOverflow);
    }

    return IncidentLight{
        light.directionToLight,
        illuminanceLux,
        illuminanceRgb,
    };
}

std::expected<IncidentLight, LightingError> EvaluatePointLight(PointLight const &light, Float3 surfacePosition,
                                                               UnitDirection3 surfaceNormal) noexcept
{
    if (std::expected<void, LightingError> const lightResult = ValidatePointLight(light); !lightResult)
    {
        return std::unexpected(lightResult.error());
    }
    if (!IsFinite(surfacePosition))
    {
        return std::unexpected(LightingError::NonFiniteVector);
    }
    if (std::expected<void, LightingError> const normalResult = ValidateDirection(surfaceNormal); !normalResult)
    {
        return std::unexpected(normalResult.error());
    }

    Float3 const offset{
        light.position.x - surfacePosition.x,
        light.position.y - surfacePosition.y,
        light.position.z - surfacePosition.z,
    };
    double const distanceSquared = (static_cast<double>(offset.x) * static_cast<double>(offset.x)) +
                                   (static_cast<double>(offset.y) * static_cast<double>(offset.y)) +
                                   (static_cast<double>(offset.z) * static_cast<double>(offset.z));
    if (distanceSquared <= 0.0)
    {
        return std::unexpected(LightingError::CoincidentPointLight);
    }

    std::expected<UnitDirection3, LightingError> const directionResult = Normalize(offset);
    if (!directionResult)
    {
        return std::unexpected(directionResult.error());
    }

    double const normalIlluminance = static_cast<double>(light.luminousIntensityCandela) / distanceSquared;
    double const surfaceIlluminance =
        normalIlluminance * static_cast<double>(ClampedDot(surfaceNormal, *directionResult));
    if (!std::isfinite(surfaceIlluminance) ||
        surfaceIlluminance > static_cast<double>((std::numeric_limits<float>::max)()))
    {
        return std::unexpected(LightingError::ArithmeticOverflow);
    }

    float const illuminanceLux = static_cast<float>(surfaceIlluminance);
    LinearRgb const illuminanceRgb = Multiply(light.color, illuminanceLux);
    if (!IsFinite(illuminanceRgb))
    {
        return std::unexpected(LightingError::ArithmeticOverflow);
    }

    return IncidentLight{
        *directionResult,
        illuminanceLux,
        illuminanceRgb,
    };
}

LinearRgb LambertDiffuse(LinearRgb baseColor) noexcept
{
    return Multiply(baseColor, 1.0F / kPi);
}

LinearRgb SchlickFresnel(LinearRgb f0, float vDotH) noexcept
{
    float const oneMinusCosine = 1.0F - std::clamp(vDotH, 0.0F, 1.0F);
    float const factor = oneMinusCosine * oneMinusCosine * oneMinusCosine * oneMinusCosine * oneMinusCosine;
    return {
        f0.r + ((1.0F - f0.r) * factor),
        f0.g + ((1.0F - f0.g) * factor),
        f0.b + ((1.0F - f0.b) * factor),
    };
}

float GgxTrowbridgeReitzNdf(float nDotH, float roughness) noexcept
{
    float const cosine = std::clamp(nDotH, 0.0F, 1.0F);
    float const effectiveRoughness = std::max(roughness, kMinimumRoughness);
    float const alpha = effectiveRoughness * effectiveRoughness;
    float const alphaSquared = alpha * alpha;
    float const denominatorTerm = ((cosine * cosine) * (alphaSquared - 1.0F)) + 1.0F;
    return alphaSquared / (kPi * denominatorTerm * denominatorTerm);
}

float SmithGgxMaskingShadowing(float nDotV, float nDotL, float roughness) noexcept
{
    return SmithGgxG1(nDotV, roughness) * SmithGgxG1(nDotL, roughness);
}

LinearRgb CookTorranceSpecular(LinearRgb fresnel, float normalDistribution, float maskingShadowing, float nDotV,
                               float nDotL) noexcept
{
    float const cosineProduct = std::clamp(nDotV, 0.0F, 1.0F) * std::clamp(nDotL, 0.0F, 1.0F);
    if (cosineProduct <= 0.0F)
    {
        return {};
    }

    float const scale = (normalDistribution * maskingShadowing) / (4.0F * cosineProduct);
    return Multiply(fresnel, scale);
}

LinearRgb DielectricMetalF0(MaterialParameters const &material) noexcept
{
    return Lerp(material.dielectricF0, material.baseColor, material.metalness);
}

LinearRgb DiffuseEnergyWeight(LinearRgb fresnel, float metalness) noexcept
{
    float const dielectricFraction = 1.0F - std::clamp(metalness, 0.0F, 1.0F);
    return {
        (1.0F - fresnel.r) * dielectricFraction,
        (1.0F - fresnel.g) * dielectricFraction,
        (1.0F - fresnel.b) * dielectricFraction,
    };
}

std::expected<BrdfDiagnosticTerms, LightingError> EvaluateDirectLightBrdf(MaterialParameters const &material,
                                                                          UnitDirection3 surfaceNormal,
                                                                          UnitDirection3 directionToView,
                                                                          UnitDirection3 directionToLight) noexcept
{
    if (std::expected<void, LightingError> const materialResult = ValidateMaterial(material); !materialResult)
    {
        return std::unexpected(materialResult.error());
    }
    for (UnitDirection3 const direction : {surfaceNormal, directionToView, directionToLight})
    {
        if (std::expected<void, LightingError> const directionResult = ValidateDirection(direction); !directionResult)
        {
            return std::unexpected(directionResult.error());
        }
    }

    float const nDotL = ClampedDot(surfaceNormal, directionToLight);
    float const nDotV = ClampedDot(surfaceNormal, directionToView);
    if (nDotL <= 0.0F || nDotV <= 0.0F)
    {
        return EmptyTerms(nDotL, nDotV, material);
    }

    Float3 const halfVector{
        directionToView.x + directionToLight.x,
        directionToView.y + directionToLight.y,
        directionToView.z + directionToLight.z,
    };
    std::expected<UnitDirection3, LightingError> const halfResult = Normalize(halfVector);
    if (!halfResult)
    {
        return EmptyTerms(nDotL, nDotV, material);
    }

    BrdfDiagnosticTerms terms = EmptyTerms(nDotL, nDotV, material);
    terms.nDotH = ClampedDot(surfaceNormal, *halfResult);
    terms.vDotH = ClampedDot(directionToView, *halfResult);
    terms.fresnel = SchlickFresnel(terms.f0, terms.vDotH);
    terms.normalDistribution = GgxTrowbridgeReitzNdf(terms.nDotH, material.roughness);
    terms.maskingShadowing = SmithGgxMaskingShadowing(nDotV, nDotL, material.roughness);
    // Diffuse substrate light transmits through the dielectric interface once
    // on entry along L and once on exit along V. Metals have no diffuse substrate.
    LinearRgb const viewTransmission = DiffuseEnergyWeight(SchlickFresnel(terms.f0, nDotV), material.metalness);
    LinearRgb const lightTransmission = DiffuseEnergyWeight(SchlickFresnel(terms.f0, nDotL), 0.0F);
    terms.diffuseWeight = Multiply(viewTransmission, lightTransmission);
    terms.diffuseBrdf = Multiply(LambertDiffuse(material.baseColor), terms.diffuseWeight);
    terms.specularBrdf =
        CookTorranceSpecular(terms.fresnel, terms.normalDistribution, terms.maskingShadowing, nDotV, nDotL);
    terms.combinedBrdf = Add(terms.diffuseBrdf, terms.specularBrdf);
    return terms;
}

} // namespace ch05::lighting
