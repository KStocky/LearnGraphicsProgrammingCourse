#include "MaterialLayering.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ch23::material_layering
{
namespace
{

inline constexpr float kPi = 3.14159265358979323846F;
inline constexpr float kInversePi = 1.0F / kPi;
inline constexpr double kPiDouble = 3.141592653589793238462643383279502884;

// Frames are supplied as float data but validated in double so orthonormality and
// handedness tolerances are not dominated by float rounding of the dot products.
inline constexpr double kUnitTolerance = 2.0e-3;
inline constexpr double kOrthogonalTolerance = 2.0e-3;
inline constexpr double kRightHandedThreshold = 0.99;

// Guards the deterministic quadrature against absurd sample counts (which would
// only waste time, never improve the teaching point) and integer overflow.
inline constexpr std::uint32_t kMaximumStrata = 4096U;

struct Double3 final
{
    double x{};
    double y{};
    double z{};
};

[[nodiscard]] bool IsFinite(Float3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool IsFinite(UnitVector3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool IsFinite(LinearRgb value) noexcept
{
    return std::isfinite(value.r) && std::isfinite(value.g) && std::isfinite(value.b);
}

[[nodiscard]] bool IsFinite(RadianceRgb value) noexcept
{
    return std::isfinite(value.r) && std::isfinite(value.g) && std::isfinite(value.b);
}

[[nodiscard]] bool IsFiniteNonNegative(LinearRgb value) noexcept
{
    return IsFinite(value) && value.r >= 0.0F && value.g >= 0.0F && value.b >= 0.0F;
}

[[nodiscard]] bool InUnitInterval(float value) noexcept
{
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] bool InUnitInterval(LinearRgb value) noexcept
{
    return InUnitInterval(value.r) && InUnitInterval(value.g) && InUnitInterval(value.b);
}

[[nodiscard]] bool InClosedRange(float value, float minimum, float maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

[[nodiscard]] float ClampUnit(float value, float fallback) noexcept
{
    if (!std::isfinite(value))
    {
        return fallback;
    }
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] LinearRgb Add(LinearRgb first, LinearRgb second) noexcept
{
    return {first.r + second.r, first.g + second.g, first.b + second.b};
}

[[nodiscard]] LinearRgb Multiply(LinearRgb first, LinearRgb second) noexcept
{
    return {first.r * second.r, first.g * second.g, first.b * second.b};
}

[[nodiscard]] LinearRgb Scale(LinearRgb value, float scalar) noexcept
{
    return {value.r * scalar, value.g * scalar, value.b * scalar};
}

[[nodiscard]] Double3 ToDouble3(Float3 value) noexcept
{
    return {static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z)};
}

[[nodiscard]] Double3 ToDouble3(UnitVector3 value) noexcept
{
    return {static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z)};
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

[[nodiscard]] UnitVector3 ToUnitVector3(Double3 value) noexcept
{
    return {static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
}

[[nodiscard]] std::expected<void, MaterialError> ValidateUnitVector(UnitVector3 value) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(MaterialError::NonFiniteInput);
    }

    double const lengthSquared = Dot(ToDouble3(value), ToDouble3(value));
    if (lengthSquared <= 0.0)
    {
        return std::unexpected(MaterialError::ZeroLengthDirection);
    }
    if (std::abs(lengthSquared - 1.0) > kUnitTolerance)
    {
        return std::unexpected(MaterialError::DirectionNotNormalized);
    }
    return {};
}

// Tangent-space evaluation core shared by the public single-sample entry point and
// the hemisphere integrator. Both localView and localLight are expected to be
// (near) unit vectors in the frame's tangent space; the caller owns validation.
[[nodiscard]] LayeredBrdfResult EvaluateLayeredBrdfLocal(LayeredMaterial const &material, Float3 localView,
                                                         Float3 localLight) noexcept;

} // namespace

std::expected<void, MaterialError> ValidateMaterial(LayeredMaterial const &material) noexcept
{
    if (!InUnitInterval(material.baseColor))
    {
        return std::unexpected(MaterialError::BaseColorOutOfRange);
    }
    if (!InClosedRange(material.metallic, 0.0F, 1.0F))
    {
        return std::unexpected(MaterialError::MetallicOutOfRange);
    }
    if (!InClosedRange(material.perceptualRoughness, 0.0F, 1.0F))
    {
        return std::unexpected(MaterialError::RoughnessOutOfRange);
    }
    if (!InClosedRange(material.anisotropy, -1.0F, 1.0F))
    {
        return std::unexpected(MaterialError::AnisotropyOutOfRange);
    }
    if (!InUnitInterval(material.baseDielectricF0))
    {
        return std::unexpected(MaterialError::DielectricF0OutOfRange);
    }
    if (!InClosedRange(material.clearcoatWeight, 0.0F, 1.0F))
    {
        return std::unexpected(MaterialError::ClearcoatWeightOutOfRange);
    }
    if (!InClosedRange(material.clearcoatRoughness, 0.0F, 1.0F))
    {
        return std::unexpected(MaterialError::ClearcoatRoughnessOutOfRange);
    }
    if (!InUnitInterval(material.emissiveColor))
    {
        return std::unexpected(MaterialError::EmissiveColorOutOfRange);
    }
    if (!std::isfinite(material.emissiveIntensity) || material.emissiveIntensity < 0.0F)
    {
        return std::unexpected(MaterialError::EmissiveIntensityOutOfRange);
    }
    return {};
}

LayeredMaterial NormalizeMaterial(LayeredMaterial const &material) noexcept
{
    LayeredMaterial normalized{};
    normalized.baseColor = {
        ClampUnit(material.baseColor.r, 0.5F),
        ClampUnit(material.baseColor.g, 0.5F),
        ClampUnit(material.baseColor.b, 0.5F),
    };
    normalized.metallic = ClampUnit(material.metallic, 0.0F);
    normalized.perceptualRoughness = ClampUnit(material.perceptualRoughness, 0.5F);
    normalized.anisotropy = std::isfinite(material.anisotropy) ? std::clamp(material.anisotropy, -1.0F, 1.0F) : 0.0F;
    normalized.baseDielectricF0 = {
        ClampUnit(material.baseDielectricF0.r, kClearcoatF0),
        ClampUnit(material.baseDielectricF0.g, kClearcoatF0),
        ClampUnit(material.baseDielectricF0.b, kClearcoatF0),
    };
    normalized.clearcoatWeight = ClampUnit(material.clearcoatWeight, 0.0F);
    normalized.clearcoatRoughness = ClampUnit(material.clearcoatRoughness, 0.05F);
    normalized.emissiveColor = {
        ClampUnit(material.emissiveColor.r, 0.0F),
        ClampUnit(material.emissiveColor.g, 0.0F),
        ClampUnit(material.emissiveColor.b, 0.0F),
    };
    normalized.emissiveIntensity = (std::isfinite(material.emissiveIntensity) && material.emissiveIntensity > 0.0F)
                                       ? material.emissiveIntensity
                                       : 0.0F;
    return normalized;
}

std::expected<UnitVector3, MaterialError> Normalize(Float3 value) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(MaterialError::NonFiniteInput);
    }

    Double3 const wide = ToDouble3(value);
    double const lengthSquared = Dot(wide, wide);
    if (lengthSquared <= 0.0)
    {
        return std::unexpected(MaterialError::ZeroLengthDirection);
    }

    double const inverseLength = 1.0 / std::sqrt(lengthSquared);
    return ToUnitVector3({wide.x * inverseLength, wide.y * inverseLength, wide.z * inverseLength});
}

std::expected<ShadingFrame, MaterialError> MakeShadingFrame(Float3 normal, Float3 tangentHint) noexcept
{
    if (!IsFinite(normal) || !IsFinite(tangentHint))
    {
        return std::unexpected(MaterialError::NonFiniteInput);
    }

    Double3 const wideNormal = ToDouble3(normal);
    double const normalLengthSquared = Dot(wideNormal, wideNormal);
    if (normalLengthSquared <= 0.0)
    {
        return std::unexpected(MaterialError::DegenerateFrame);
    }
    Double3 const unitNormal = {
        wideNormal.x / std::sqrt(normalLengthSquared),
        wideNormal.y / std::sqrt(normalLengthSquared),
        wideNormal.z / std::sqrt(normalLengthSquared),
    };

    Double3 const wideHint = ToDouble3(tangentHint);
    double const projection = Dot(wideHint, unitNormal);
    Double3 const projectedTangent = {
        wideHint.x - (unitNormal.x * projection),
        wideHint.y - (unitNormal.y * projection),
        wideHint.z - (unitNormal.z * projection),
    };
    double const tangentLengthSquared = Dot(projectedTangent, projectedTangent);
    if (tangentLengthSquared <= 0.0)
    {
        return std::unexpected(MaterialError::DegenerateFrame);
    }
    double const inverseTangentLength = 1.0 / std::sqrt(tangentLengthSquared);
    Double3 const unitTangent = {
        projectedTangent.x * inverseTangentLength,
        projectedTangent.y * inverseTangentLength,
        projectedTangent.z * inverseTangentLength,
    };

    // Right-handed: bitangent = normal x tangent, so cross(tangent, bitangent) == normal.
    Double3 const unitBitangent = Cross(unitNormal, unitTangent);

    ShadingFrame const frame{
        ToUnitVector3(unitTangent),
        ToUnitVector3(unitBitangent),
        ToUnitVector3(unitNormal),
    };
    if (!IsFinite(frame.tangent) || !IsFinite(frame.bitangent) || !IsFinite(frame.normal))
    {
        return std::unexpected(MaterialError::ArithmeticOverflow);
    }
    return frame;
}

std::expected<void, MaterialError> ValidateShadingFrame(ShadingFrame const &frame) noexcept
{
    if (!IsFinite(frame.tangent) || !IsFinite(frame.bitangent) || !IsFinite(frame.normal))
    {
        return std::unexpected(MaterialError::NonFiniteInput);
    }

    Double3 const tangent = ToDouble3(frame.tangent);
    Double3 const bitangent = ToDouble3(frame.bitangent);
    Double3 const normal = ToDouble3(frame.normal);

    for (Double3 const axis : {tangent, bitangent, normal})
    {
        if (std::abs(Dot(axis, axis) - 1.0) > kUnitTolerance)
        {
            return std::unexpected(MaterialError::DegenerateFrame);
        }
    }

    if (std::abs(Dot(tangent, bitangent)) > kOrthogonalTolerance ||
        std::abs(Dot(tangent, normal)) > kOrthogonalTolerance ||
        std::abs(Dot(bitangent, normal)) > kOrthogonalTolerance)
    {
        return std::unexpected(MaterialError::DegenerateFrame);
    }

    double const handedness = Dot(Cross(tangent, bitangent), normal);
    if (handedness < kRightHandedThreshold)
    {
        return std::unexpected(MaterialError::FrameNotRightHanded);
    }
    return {};
}

Float3 ToTangentSpace(ShadingFrame const &frame, Float3 worldVector) noexcept
{
    Double3 const wide = ToDouble3(worldVector);
    return {
        static_cast<float>(Dot(ToDouble3(frame.tangent), wide)),
        static_cast<float>(Dot(ToDouble3(frame.bitangent), wide)),
        static_cast<float>(Dot(ToDouble3(frame.normal), wide)),
    };
}

float PerceptualRoughnessToAlpha(float perceptualRoughness) noexcept
{
    float const clamped = std::clamp(perceptualRoughness, kMinimumPerceptualRoughness, 1.0F);
    return clamped * clamped;
}

AnisotropicRoughness MapAnisotropicRoughness(float perceptualRoughness, float anisotropy) noexcept
{
    float const alpha = PerceptualRoughnessToAlpha(perceptualRoughness);
    float const clampedAnisotropy = std::clamp(anisotropy, -1.0F, 1.0F);

    // Product-preserving, sign-swap-symmetric split around the isotropic alpha:
    //   alpha_t = alpha * sqrt((1 + k*a) / (1 - k*a)),  alpha_b = alpha^2 / alpha_t.
    // Analytically alpha_t * alpha_b == alpha^2, and negating a swaps the axes.
    float const biased = kAnisotropyAspectStrength * clampedAnisotropy;
    float const ratio = (1.0F + biased) / (1.0F - biased);
    float const factor = std::sqrt(ratio);
    float const alphaTangent = alpha * factor;
    float const alphaBitangent = alpha / factor;
    return {alphaTangent, alphaBitangent};
}

float AnisotropicGgxNdf(Float3 localHalf, AnisotropicRoughness roughness) noexcept
{
    float const cosTheta = localHalf.z;
    if (cosTheta <= 0.0F)
    {
        return 0.0F;
    }

    float const alphaTangent = std::max(roughness.alphaTangent, std::numeric_limits<float>::min());
    float const alphaBitangent = std::max(roughness.alphaBitangent, std::numeric_limits<float>::min());
    float const tangentTerm = localHalf.x / alphaTangent;
    float const bitangentTerm = localHalf.y / alphaBitangent;
    float const inner = (tangentTerm * tangentTerm) + (bitangentTerm * bitangentTerm) + (cosTheta * cosTheta);
    float const denominator = kPi * alphaTangent * alphaBitangent * inner * inner;
    if (denominator <= 0.0F || !std::isfinite(denominator))
    {
        return 0.0F;
    }
    return 1.0F / denominator;
}

float SmithGgxLambda(Float3 localDirection, AnisotropicRoughness roughness) noexcept
{
    float const cosTheta = localDirection.z;
    if (cosTheta <= 0.0F)
    {
        return 0.0F;
    }

    float const tangentTerm = roughness.alphaTangent * localDirection.x;
    float const bitangentTerm = roughness.alphaBitangent * localDirection.y;
    float const projectedRoughnessSquared = (tangentTerm * tangentTerm) + (bitangentTerm * bitangentTerm);
    float const tangentSquared = projectedRoughnessSquared / (cosTheta * cosTheta);
    return 0.5F * (-1.0F + std::sqrt(1.0F + tangentSquared));
}

float SmithGgxG1(Float3 localDirection, AnisotropicRoughness roughness) noexcept
{
    if (localDirection.z <= 0.0F)
    {
        return 0.0F;
    }
    return 1.0F / (1.0F + SmithGgxLambda(localDirection, roughness));
}

float SmithGgxG2HeightCorrelated(Float3 localView, Float3 localLight, AnisotropicRoughness roughness) noexcept
{
    if (localView.z <= 0.0F || localLight.z <= 0.0F)
    {
        return 0.0F;
    }
    float const lambdaView = SmithGgxLambda(localView, roughness);
    float const lambdaLight = SmithGgxLambda(localLight, roughness);
    return 1.0F / (1.0F + lambdaView + lambdaLight);
}

float SchlickFresnel(float f0, float cosTheta) noexcept
{
    float const clampedCosine = std::clamp(cosTheta, 0.0F, 1.0F);
    float const oneMinusCosine = 1.0F - clampedCosine;
    float const squared = oneMinusCosine * oneMinusCosine;
    float const quintic = squared * squared * oneMinusCosine;
    return f0 + ((1.0F - f0) * quintic);
}

LinearRgb SchlickFresnel(LinearRgb f0, float cosTheta) noexcept
{
    return {
        SchlickFresnel(f0.r, cosTheta),
        SchlickFresnel(f0.g, cosTheta),
        SchlickFresnel(f0.b, cosTheta),
    };
}

LinearRgb LambertDiffuse(LinearRgb baseColor) noexcept
{
    return Scale(baseColor, kInversePi);
}

LinearRgb BaseF0(LayeredMaterial const &material) noexcept
{
    float const metallic = std::clamp(material.metallic, 0.0F, 1.0F);
    return {
        material.baseDielectricF0.r + ((material.baseColor.r - material.baseDielectricF0.r) * metallic),
        material.baseDielectricF0.g + ((material.baseColor.g - material.baseDielectricF0.g) * metallic),
        material.baseDielectricF0.b + ((material.baseColor.b - material.baseDielectricF0.b) * metallic),
    };
}

RadianceRgb EmittedRadiance(LayeredMaterial const &material) noexcept
{
    float const intensity = std::max(material.emissiveIntensity, 0.0F);
    return {
        material.emissiveColor.r * intensity,
        material.emissiveColor.g * intensity,
        material.emissiveColor.b * intensity,
    };
}

namespace
{

LayeredBrdfResult EvaluateLayeredBrdfLocal(LayeredMaterial const &material, Float3 localView,
                                           Float3 localLight) noexcept
{
    LayeredBrdfResult result{};
    result.baseRoughness = MapAnisotropicRoughness(material.perceptualRoughness, material.anisotropy);
    result.coatAlpha = PerceptualRoughnessToAlpha(material.clearcoatRoughness);
    result.emittedRadiance = EmittedRadiance(material);

    float const nDotV = std::clamp(localView.z, 0.0F, 1.0F);
    float const nDotL = std::clamp(localLight.z, 0.0F, 1.0F);
    result.nDotV = nDotV;
    result.nDotL = nDotL;

    float const weight = std::clamp(material.clearcoatWeight, 0.0F, 1.0F);
    result.coatTransmissionIn = 1.0F - (weight * SchlickFresnel(kClearcoatF0, nDotL));
    result.coatTransmissionOut = 1.0F - (weight * SchlickFresnel(kClearcoatF0, nDotV));
    result.coatTransmission = result.coatTransmissionIn * result.coatTransmissionOut;

    // No visible reflection geometry: leave every lobe at zero but keep the budget
    // fields populated and finite.
    if (nDotV <= 0.0F || nDotL <= 0.0F)
    {
        result.energyBudgetRespected = true;
        return result;
    }

    LinearRgb const baseF0 = BaseF0(material);

    Float3 const unnormalizedHalf{
        localView.x + localLight.x,
        localView.y + localLight.y,
        localView.z + localLight.z,
    };
    std::expected<UnitVector3, MaterialError> const half = Normalize(unnormalizedHalf);
    bool const hasHalf = half.has_value();
    Float3 const localHalf = hasHalf ? Float3{half->x, half->y, half->z} : Float3{0.0F, 0.0F, 1.0F};
    float const nDotH = hasHalf ? std::clamp(localHalf.z, 0.0F, 1.0F) : 0.0F;
    float const vDotH =
        hasHalf ? std::clamp((localView.x * localHalf.x) + (localView.y * localHalf.y) + (localView.z * localHalf.z),
                             0.0F, 1.0F)
                : 0.0F;
    result.nDotH = nDotH;

    float const specularDenominator = 4.0F * nDotV * nDotL;

    // Base layer.
    result.baseFresnel = SchlickFresnel(baseF0, vDotH);
    result.baseNormalDistribution = hasHalf ? AnisotropicGgxNdf(localHalf, result.baseRoughness) : 0.0F;
    result.baseMaskingShadowing = SmithGgxG2HeightCorrelated(localView, localLight, result.baseRoughness);
    float const baseSpecularScale = (result.baseNormalDistribution * result.baseMaskingShadowing) / specularDenominator;
    result.baseSpecular = Scale(result.baseFresnel, baseSpecularScale);

    // Base diffuse: dielectric substrate only, with the light transmitted through
    // the dielectric interface on entry (N.L) and exit (N.V). Metals have none.
    LinearRgb const fresnelView = SchlickFresnel(baseF0, nDotV);
    LinearRgb const fresnelLight = SchlickFresnel(baseF0, nDotL);
    float const dielectricFraction = 1.0F - std::clamp(material.metallic, 0.0F, 1.0F);
    LinearRgb const diffuseWeight{
        (1.0F - fresnelView.r) * (1.0F - fresnelLight.r) * dielectricFraction,
        (1.0F - fresnelView.g) * (1.0F - fresnelLight.g) * dielectricFraction,
        (1.0F - fresnelView.b) * (1.0F - fresnelLight.b) * dielectricFraction,
    };
    result.baseDiffuse = Multiply(LambertDiffuse(material.baseColor), diffuseWeight);
    result.baseBrdf = Add(result.baseDiffuse, result.baseSpecular);

    // Clearcoat: an isotropic dielectric lobe evaluated against the same normal.
    AnisotropicRoughness const coatRoughness{result.coatAlpha, result.coatAlpha};
    result.coatFresnel = SchlickFresnel(kClearcoatF0, vDotH);
    result.coatNormalDistribution = hasHalf ? AnisotropicGgxNdf(localHalf, coatRoughness) : 0.0F;
    result.coatMaskingShadowing = SmithGgxG2HeightCorrelated(localView, localLight, coatRoughness);
    float const coatSpecularScale =
        (weight * result.coatFresnel * result.coatNormalDistribution * result.coatMaskingShadowing) /
        specularDenominator;
    result.coatSpecular = {coatSpecularScale, coatSpecularScale, coatSpecularScale};

    // Compose: coat reflection is added once; the base is attenuated by the two
    // coat transmissions (into the base along L, back out along V).
    result.attenuatedBase = Scale(result.baseBrdf, result.coatTransmission);
    result.reflectedBrdf = Add(result.coatSpecular, result.attenuatedBase);

    result.energyBudgetRespected = result.coatTransmission >= 0.0F && result.coatTransmission <= 1.0F &&
                                   IsFiniteNonNegative(result.reflectedBrdf) &&
                                   IsFiniteNonNegative(result.coatSpecular) && IsFiniteNonNegative(result.baseBrdf);
    return result;
}

} // namespace

std::expected<LayeredBrdfResult, MaterialError> EvaluateLayeredBrdf(LayeredMaterial const &material,
                                                                    ShadingFrame const &frame,
                                                                    UnitVector3 directionToView,
                                                                    UnitVector3 directionToLight) noexcept
{
    if (std::expected<void, MaterialError> const materialResult = ValidateMaterial(material); !materialResult)
    {
        return std::unexpected(materialResult.error());
    }
    if (std::expected<void, MaterialError> const frameResult = ValidateShadingFrame(frame); !frameResult)
    {
        return std::unexpected(frameResult.error());
    }
    if (std::expected<void, MaterialError> const viewResult = ValidateUnitVector(directionToView); !viewResult)
    {
        return std::unexpected(viewResult.error());
    }
    if (std::expected<void, MaterialError> const lightResult = ValidateUnitVector(directionToLight); !lightResult)
    {
        return std::unexpected(lightResult.error());
    }

    Float3 const localView = ToTangentSpace(frame, {directionToView.x, directionToView.y, directionToView.z});
    Float3 const localLight = ToTangentSpace(frame, {directionToLight.x, directionToLight.y, directionToLight.z});
    LayeredBrdfResult const result = EvaluateLayeredBrdfLocal(material, localView, localLight);

    if (!IsFiniteNonNegative(result.reflectedBrdf) || !IsFinite(result.emittedRadiance))
    {
        return std::unexpected(MaterialError::ArithmeticOverflow);
    }
    return result;
}

std::expected<LinearRgb, MaterialError> IntegrateDirectionalHemisphericalReflectance(
    LayeredMaterial const &material, ShadingFrame const &frame, UnitVector3 directionToView,
    HemisphereQuadrature quadrature) noexcept
{
    if (std::expected<void, MaterialError> const materialResult = ValidateMaterial(material); !materialResult)
    {
        return std::unexpected(materialResult.error());
    }
    if (std::expected<void, MaterialError> const frameResult = ValidateShadingFrame(frame); !frameResult)
    {
        return std::unexpected(frameResult.error());
    }
    if (std::expected<void, MaterialError> const viewResult = ValidateUnitVector(directionToView); !viewResult)
    {
        return std::unexpected(viewResult.error());
    }
    if (quadrature.polarStrata == 0U || quadrature.azimuthalStrata == 0U || quadrature.polarStrata > kMaximumStrata ||
        quadrature.azimuthalStrata > kMaximumStrata)
    {
        return std::unexpected(MaterialError::InvalidQuadrature);
    }

    Float3 const localView = ToTangentSpace(frame, {directionToView.x, directionToView.y, directionToView.z});
    if (localView.z <= 0.0F)
    {
        return LinearRgb{0.0F, 0.0F, 0.0F};
    }

    double const cosineStep = 1.0 / static_cast<double>(quadrature.polarStrata);
    double const azimuthStep = (2.0 * kPiDouble) / static_cast<double>(quadrature.azimuthalStrata);

    double integralRed = 0.0;
    double integralGreen = 0.0;
    double integralBlue = 0.0;
    for (std::uint32_t polarIndex = 0U; polarIndex < quadrature.polarStrata; ++polarIndex)
    {
        double const cosTheta = (static_cast<double>(polarIndex) + 0.5) * cosineStep;
        double const sinTheta = std::sqrt(std::max(0.0, 1.0 - (cosTheta * cosTheta)));
        for (std::uint32_t azimuthIndex = 0U; azimuthIndex < quadrature.azimuthalStrata; ++azimuthIndex)
        {
            double const azimuth = (static_cast<double>(azimuthIndex) + 0.5) * azimuthStep;
            Float3 const localLight{
                static_cast<float>(sinTheta * std::cos(azimuth)),
                static_cast<float>(sinTheta * std::sin(azimuth)),
                static_cast<float>(cosTheta),
            };
            LayeredBrdfResult const sample = EvaluateLayeredBrdfLocal(material, localView, localLight);
            double const weight = cosTheta * cosineStep * azimuthStep;
            integralRed += static_cast<double>(sample.reflectedBrdf.r) * weight;
            integralGreen += static_cast<double>(sample.reflectedBrdf.g) * weight;
            integralBlue += static_cast<double>(sample.reflectedBrdf.b) * weight;
        }
    }

    LinearRgb const reflectance{
        static_cast<float>(integralRed),
        static_cast<float>(integralGreen),
        static_cast<float>(integralBlue),
    };
    if (!IsFiniteNonNegative(reflectance))
    {
        return std::unexpected(MaterialError::ArithmeticOverflow);
    }
    return reflectance;
}

} // namespace ch23::material_layering
