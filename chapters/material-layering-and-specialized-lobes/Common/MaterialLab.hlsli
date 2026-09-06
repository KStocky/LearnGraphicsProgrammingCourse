#ifndef CH23_MATERIAL_LAB_HLSLI
#define CH23_MATERIAL_LAB_HLSLI

// Chapter 23 - Material Layering and Specialized BRDF Lobes (shared HLSL core).
//
// This include mirrors the CPU contract in Common/MaterialLayering.cpp term for
// term so the runnable lab and the Catch2 contract stay legible against each
// other. The Starter uses only the isotropic baseline (ShadeBaselineRadiance);
// the Solution adds the anisotropic layered path, clearcoat, and emission.
//
// Skeptical-physics notes carried over from the CPU contract:
//   * The coat/base composition is one coat reflection plus two macro-surface
//     transmissions. It ignores internal inter-reflection, refraction/parallax,
//     and lateral transport, and does not conserve energy exactly.
//   * Single-scattering GGX loses energy at high roughness by design.
//   * The (roughness, anisotropy) -> (alpha_t, alpha_b) mapping is a deliberate
//     convention (k = 0.8, product preserving, sign-swap symmetric), not a
//     uniquely standard one.

// ---------------------------------------------------------------------------
// Scene, light, and material constants. The field order and 16-byte packing
// match ch23::material_layering::gpu::LabConstants exactly; the C++ side asserts
// every offset so the two never drift.
// ---------------------------------------------------------------------------

struct LabConstants
{
    row_major float4x4 viewProjection;

    float3 cameraPosition;
    uint sceneGeometry; // 0 = sphere, 1 = probe card.

    float3 directionToLight;
    float lightIntensity;

    float3 lightColor;
    uint outputView; // 0 final, 1 base, 2 coat, 3 emission, 4 attenuation.

    float3 baseColor;
    float metallic;

    float3 baseDielectricF0;
    float perceptualRoughness;

    float3 emissiveColor;
    float anisotropy;

    float clearcoatWeight;
    float clearcoatRoughness;
    float emissiveIntensity;
    uint preset; // 0 baseline, 1 anisotropic, 2 clearcoat, 3 emissive.

    float3 probeNormal;
    float probePad0;

    float3 probeTangent;
    float probePad1;

    float3 probeViewDirection;
    float probePad2;

    float3 probeLightDirection;
    float probePad3;
};

ConstantBuffer<LabConstants> gLab : register(b0);

struct DisplayConstants
{
    float exposure;
    uint outputView;
    float2 displayPad;
};

// The display constants live in register space 1 so this shared include can
// declare both the lighting (b0, space0) and display (b0, space1) cbuffers
// without a binding conflict; each entry point only references one of them.
ConstantBuffer<DisplayConstants> gDisplay : register(b0, space1);
Texture2D<float4> gHdrInput : register(t0);
SamplerState gLinearClampSampler : register(s0);

// Scene geometry identifiers shared with the C++ SceneGeometry enum.
static const uint SceneGeometrySphere = 0u;
static const uint SceneGeometryProbe = 1u;

// Material presets shared with the C++ ScenePreset enum.
static const uint PresetBaseline = 0u;
static const uint PresetAnisotropic = 1u;
static const uint PresetClearcoat = 2u;
static const uint PresetEmissive = 3u;

// Diagnostic output views shared with the C++ OutputView enum.
static const uint OutputViewFinal = 0u;
static const uint OutputViewBase = 1u;
static const uint OutputViewCoat = 2u;
static const uint OutputViewEmission = 3u;
static const uint OutputViewAttenuation = 4u;

static const float MaterialPi = 3.14159265358979323846f;
static const float MaterialInversePi = 1.0f / MaterialPi;

// Numerical safeguards from the CPU contract.
static const float MinimumPerceptualRoughness = 0.045f;
static const float AnisotropyAspectStrength = 0.8f;
static const float ClearcoatF0 = 0.04f;

struct MeshVertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT; // xyz = tangent; generated chapter geometry stores canonical w = +1.
};

struct SurfaceVertex
{
    float4 clipPosition : SV_Position;
    float3 worldPosition : POSITION;
    float3 worldNormal : NORMAL;
    float4 worldTangent : TANGENT;
};

struct FullscreenVertex
{
    float4 clipPosition : SV_Position;
    float2 uv : TEXCOORD0;
};

// A right-handed orthonormal shading basis: cross(tangent, bitangent) == normal.
struct ShadingBasis
{
    float3 tangent;
    float3 bitangent;
    float3 normal;
};

// The reflected/emitted breakdown mirrors ch23::material_layering::LayeredBrdfResult
// closely enough to drive every diagnostic view and the CPU parity probes.
struct LayeredResponse
{
    float3 baseBrdf;
    float3 coatSpecular;
    float coatTransmission;
    float3 attenuatedBase;
    float3 reflectedBrdf;
    float3 emittedRadiance;
};

// ---------------------------------------------------------------------------
// Roughness mapping. PerceptualRoughnessToAlpha clamps to the floor then squares
// (Disney/UE alpha = roughness^2). MapAnisotropicRoughness is product preserving
// and sign-swap symmetric with k = 0.8, matching MaterialLayering.cpp.
// ---------------------------------------------------------------------------

float PerceptualRoughnessToAlpha(float perceptualRoughness)
{
    float clamped = clamp(perceptualRoughness, MinimumPerceptualRoughness, 1.0f);
    return clamped * clamped;
}

float2 MapAnisotropicRoughness(float perceptualRoughness, float anisotropy)
{
    float alpha = PerceptualRoughnessToAlpha(perceptualRoughness);
    float clampedAnisotropy = clamp(anisotropy, -1.0f, 1.0f);
    float biased = AnisotropyAspectStrength * clampedAnisotropy;
    float ratio = (1.0f + biased) / (1.0f - biased);
    float factor = sqrt(ratio);
    return float2(alpha * factor, alpha / factor); // (alpha_t, alpha_b).
}

// ---------------------------------------------------------------------------
// Microfacet building blocks evaluated in tangent space so the anisotropy axes
// are unambiguous. All return finite, non-negative values for valid input.
// ---------------------------------------------------------------------------

float AnisotropicGgxNdf(float3 localHalf, float2 alpha)
{
    float cosTheta = localHalf.z;
    if (cosTheta <= 0.0f)
    {
        return 0.0f;
    }
    float alphaTangent = max(alpha.x, 1.0e-30f);
    float alphaBitangent = max(alpha.y, 1.0e-30f);
    float tangentTerm = localHalf.x / alphaTangent;
    float bitangentTerm = localHalf.y / alphaBitangent;
    float inner = (tangentTerm * tangentTerm) + (bitangentTerm * bitangentTerm) + (cosTheta * cosTheta);
    float denominator = MaterialPi * alphaTangent * alphaBitangent * inner * inner;
    if (denominator <= 0.0f)
    {
        return 0.0f;
    }
    return 1.0f / denominator;
}

float SmithGgxLambda(float3 localDirection, float2 alpha)
{
    float cosTheta = localDirection.z;
    if (cosTheta <= 0.0f)
    {
        return 0.0f;
    }
    float tangentTerm = alpha.x * localDirection.x;
    float bitangentTerm = alpha.y * localDirection.y;
    float projectedRoughnessSquared = (tangentTerm * tangentTerm) + (bitangentTerm * bitangentTerm);
    float tangentSquared = projectedRoughnessSquared / (cosTheta * cosTheta);
    return 0.5f * (-1.0f + sqrt(1.0f + tangentSquared));
}

// Height-correlated Smith masking-shadowing (Heitz 2014), chosen over the
// separable G1(v) * G1(l) form and used by both the baseline and layered paths.
float SmithGgxG2HeightCorrelated(float3 localView, float3 localLight, float2 alpha)
{
    if (localView.z <= 0.0f || localLight.z <= 0.0f)
    {
        return 0.0f;
    }
    float lambdaView = SmithGgxLambda(localView, alpha);
    float lambdaLight = SmithGgxLambda(localLight, alpha);
    return 1.0f / (1.0f + lambdaView + lambdaLight);
}

float SchlickFresnelScalar(float f0, float cosTheta)
{
    float clampedCosine = saturate(cosTheta);
    float oneMinus = 1.0f - clampedCosine;
    float squared = oneMinus * oneMinus;
    float quintic = squared * squared * oneMinus;
    return f0 + ((1.0f - f0) * quintic);
}

float3 SchlickFresnelRgb(float3 f0, float cosTheta)
{
    float clampedCosine = saturate(cosTheta);
    float oneMinus = 1.0f - clampedCosine;
    float squared = oneMinus * oneMinus;
    float quintic = squared * squared * oneMinus;
    return f0 + ((1.0f - f0) * quintic);
}

float3 BaseF0(float3 baseColor, float3 dielectricF0, float metallic)
{
    return lerp(dielectricF0, baseColor, saturate(metallic));
}

// ---------------------------------------------------------------------------
// Shading basis. ProbeCard supplies an explicit tangent hint; the sphere derives
// its tangent from the interpolated vertex tangent. Both Gram-Schmidt the hint
// against the normal and take bitangent = cross(normal, tangent), matching
// ch23::material_layering::MakeShadingFrame.
// ---------------------------------------------------------------------------

ShadingBasis MakeShadingBasis(float3 normal, float3 tangentHint)
{
    ShadingBasis basis;
    basis.normal = normalize(normal);
    float3 projected = tangentHint - (basis.normal * dot(tangentHint, basis.normal));
    float projectedLengthSquared = dot(projected, projected);
    if (projectedLengthSquared <= 1.0e-12f)
    {
        // Degenerate tangent hint: fall back to an arbitrary stable perpendicular.
        float3 fallback = abs(basis.normal.x) < 0.9f ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
        projected = fallback - (basis.normal * dot(fallback, basis.normal));
    }
    basis.tangent = normalize(projected);
    // Canonicalize to the right-handed frame required by the CPU contract.
    basis.bitangent = cross(basis.normal, basis.tangent);
    return basis;
}

float3 ToTangentSpace(ShadingBasis basis, float3 worldVector)
{
    return float3(dot(basis.tangent, worldVector), dot(basis.bitangent, worldVector), dot(basis.normal, worldVector));
}

// ---------------------------------------------------------------------------
// Base lobe (anisotropic, tangent space). Shared by the layered path. Returns
// the diffuse + specular base BRDF for the supplied tangent-space directions.
// ---------------------------------------------------------------------------

float3 EvaluateBaseLobe(float3 localView, float3 localLight, float2 alpha, float3 baseColor, float3 dielectricF0,
                        float metallic)
{
    float nDotV = saturate(localView.z);
    float nDotL = saturate(localLight.z);
    if (nDotV <= 0.0f || nDotL <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 unnormalizedHalf = localView + localLight;
    float halfLengthSquared = dot(unnormalizedHalf, unnormalizedHalf);
    bool hasHalf = halfLengthSquared > 1.0e-12f;
    float3 localHalf = hasHalf ? (unnormalizedHalf * rsqrt(halfLengthSquared)) : float3(0.0f, 0.0f, 1.0f);
    float vDotH = hasHalf ? saturate(dot(localView, localHalf)) : 0.0f;

    float3 f0 = BaseF0(baseColor, dielectricF0, metallic);
    float3 fresnel = SchlickFresnelRgb(f0, vDotH);
    float distribution = hasHalf ? AnisotropicGgxNdf(localHalf, alpha) : 0.0f;
    float masking = SmithGgxG2HeightCorrelated(localView, localLight, alpha);
    float specularScale = (distribution * masking) / (4.0f * nDotV * nDotL);
    float3 specular = fresnel * specularScale;

    float3 fresnelView = SchlickFresnelRgb(f0, nDotV);
    float3 fresnelLight = SchlickFresnelRgb(f0, nDotL);
    float dielectricFraction = 1.0f - saturate(metallic);
    float3 diffuseWeight = (1.0f - fresnelView) * (1.0f - fresnelLight) * dielectricFraction;
    float3 diffuse = (baseColor * MaterialInversePi) * diffuseWeight;

    return diffuse + specular;
}

// ---------------------------------------------------------------------------
// Layered response. One coat reflection plus two macro transmissions attenuating
// the base; emission tracked separately. Mirrors EvaluateLayeredBrdfLocal.
// ---------------------------------------------------------------------------

LayeredResponse EvaluateLayered(ShadingBasis basis, float3 worldView, float3 worldLight)
{
    LayeredResponse response;
    response.baseBrdf = float3(0.0f, 0.0f, 0.0f);
    response.coatSpecular = float3(0.0f, 0.0f, 0.0f);
    response.attenuatedBase = float3(0.0f, 0.0f, 0.0f);
    response.reflectedBrdf = float3(0.0f, 0.0f, 0.0f);
    response.emittedRadiance = gLab.emissiveColor * max(gLab.emissiveIntensity, 0.0f);

    float3 localView = ToTangentSpace(basis, worldView);
    float3 localLight = ToTangentSpace(basis, worldLight);
    float nDotV = saturate(localView.z);
    float nDotL = saturate(localLight.z);

    float weight = saturate(gLab.clearcoatWeight);
    float coatTransmissionIn = 1.0f - (weight * SchlickFresnelScalar(ClearcoatF0, nDotL));
    float coatTransmissionOut = 1.0f - (weight * SchlickFresnelScalar(ClearcoatF0, nDotV));
    response.coatTransmission = coatTransmissionIn * coatTransmissionOut;

    if (nDotV <= 0.0f || nDotL <= 0.0f)
    {
        return response;
    }

    float2 baseAlpha = MapAnisotropicRoughness(gLab.perceptualRoughness, gLab.anisotropy);
    response.baseBrdf =
        EvaluateBaseLobe(localView, localLight, baseAlpha, gLab.baseColor, gLab.baseDielectricF0, gLab.metallic);

    float3 unnormalizedHalf = localView + localLight;
    float halfLengthSquared = dot(unnormalizedHalf, unnormalizedHalf);
    bool hasHalf = halfLengthSquared > 1.0e-12f;
    float3 localHalf = hasHalf ? (unnormalizedHalf * rsqrt(halfLengthSquared)) : float3(0.0f, 0.0f, 1.0f);
    float vDotH = hasHalf ? saturate(dot(localView, localHalf)) : 0.0f;

    float coatAlphaScalar = PerceptualRoughnessToAlpha(gLab.clearcoatRoughness);
    float2 coatAlpha = float2(coatAlphaScalar, coatAlphaScalar);
    float coatFresnel = SchlickFresnelScalar(ClearcoatF0, vDotH);
    float coatDistribution = hasHalf ? AnisotropicGgxNdf(localHalf, coatAlpha) : 0.0f;
    float coatMasking = SmithGgxG2HeightCorrelated(localView, localLight, coatAlpha);
    float coatSpecularScale = (weight * coatFresnel * coatDistribution * coatMasking) / (4.0f * nDotV * nDotL);
    response.coatSpecular = float3(coatSpecularScale, coatSpecularScale, coatSpecularScale);

    response.attenuatedBase = response.baseBrdf * response.coatTransmission;
    response.reflectedBrdf = response.coatSpecular + response.attenuatedBase;
    return response;
}

// ---------------------------------------------------------------------------
// Baseline radiance (isotropic, pedagogically simpler). The Starter and the
// Solution's Baseline preset both call this so their baseline output is byte
// identical. Algebraically it is the layered base lobe at anisotropy 0 with no
// coat and no emission.
// ---------------------------------------------------------------------------

float3 ShadeBaselineRadiance(float3 normal, float3 tangentHint, float3 worldView, float3 worldLight)
{
    ShadingBasis basis = MakeShadingBasis(normal, tangentHint);
    float3 localView = ToTangentSpace(basis, worldView);
    float3 localLight = ToTangentSpace(basis, worldLight);
    float nDotL = saturate(localLight.z);

    float alpha = PerceptualRoughnessToAlpha(gLab.perceptualRoughness);
    float3 brdf =
        EvaluateBaseLobe(localView, localLight, float2(alpha, alpha), gLab.baseColor, gLab.baseDielectricF0, gLab.metallic);
    float3 irradiance = gLab.lightColor * gLab.lightIntensity * nDotL;
    return brdf * irradiance;
}

// Selects the diagnostic view. The Final view is exactly base + coat + emission
// so the readback tests can prove the contribution split sums to the final.
float3 SelectLayeredView(LayeredResponse response, float nDotL, uint outputView)
{
    float3 irradiance = gLab.lightColor * gLab.lightIntensity * nDotL;
    float3 baseRadiance = irradiance * response.attenuatedBase;
    float3 coatRadiance = irradiance * response.coatSpecular;
    float3 emissionRadiance = response.emittedRadiance;
    float3 finalRadiance = baseRadiance + coatRadiance + emissionRadiance;

    if (outputView == OutputViewBase)
    {
        return baseRadiance;
    }
    if (outputView == OutputViewCoat)
    {
        return coatRadiance;
    }
    if (outputView == OutputViewEmission)
    {
        return emissionRadiance;
    }
    if (outputView == OutputViewAttenuation)
    {
        return float3(response.coatTransmission, response.coatTransmission, response.coatTransmission);
    }
    return finalRadiance;
}

// ---------------------------------------------------------------------------
// Vertex transforms.
// ---------------------------------------------------------------------------

SurfaceVertex MaterialLabVertex(MeshVertex input)
{
    SurfaceVertex output;
    float4 worldPosition = float4(input.position, 1.0f);
    output.clipPosition = mul(worldPosition, gLab.viewProjection);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = input.normal;
    output.worldTangent = input.tangent;
    return output;
}

// The probe card fills the viewport with one quad (triangle strip of 4 vertices)
// so every pixel evaluates the same analytic surface. Shading inputs come from
// the probe constants, not the interpolated geometry, which keeps CPU/GPU parity
// independent of rasterization position.
SurfaceVertex ProbeCardVertex(uint vertexId)
{
    float2 corners[4] = {
        float2(-1.0f, -1.0f),
        float2(-1.0f, 1.0f),
        float2(1.0f, -1.0f),
        float2(1.0f, 1.0f),
    };
    SurfaceVertex output;
    output.clipPosition = float4(corners[vertexId], 0.0f, 1.0f);
    output.worldPosition = float3(0.0f, 0.0f, 0.0f);
    output.worldNormal = gLab.probeNormal;
    output.worldTangent = float4(gLab.probeTangent, 1.0f);
    return output;
}

FullscreenVertex FullscreenTriangleVertex(uint vertexId)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    FullscreenVertex output;
    output.uv = uv;
    output.clipPosition = float4((uv * float2(2.0f, -2.0f)) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

// ---------------------------------------------------------------------------
// Tone mapping for the display pass. Radiance views go through ACES + exposure +
// sRGB; the attenuation view is a [0, 1] diagnostic shown without tone mapping so
// the energy budget stays readable.
// ---------------------------------------------------------------------------

float3 AcesFit(float3 color)
{
    float3 numerator = color * ((2.51f * color) + 0.03f);
    float3 denominator = (color * ((2.43f * color) + 0.59f)) + 0.14f;
    return saturate(numerator / denominator);
}

float SrgbEncode(float value)
{
    return value <= 0.0031308f ? value * 12.92f : (1.055f * pow(value, 1.0f / 2.4f)) - 0.055f;
}

float3 SrgbEncodeRgb(float3 color)
{
    return float3(SrgbEncode(color.r), SrgbEncode(color.g), SrgbEncode(color.b));
}

float3 DisplayColor(float3 hdr, float exposure, uint outputView)
{
    if (outputView == OutputViewAttenuation)
    {
        return saturate(SrgbEncodeRgb(saturate(hdr)));
    }
    float3 mapped = AcesFit(max(hdr * exp2(exposure), 0.0f));
    return saturate(SrgbEncodeRgb(mapped));
}

#endif // CH23_MATERIAL_LAB_HLSLI
