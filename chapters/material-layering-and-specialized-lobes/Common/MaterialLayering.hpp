#pragma once

// Chapter 23 - Material Layering and Specialized BRDF Lobes (CPU contract core).
//
// This header defines the deterministic, dependency-light mathematical contract
// for a layered material model: an anisotropic metal/dielectric base lobe under a
// dielectric clearcoat, plus additive emission. The types and functions are kept
// pedagogically explicit so a later HLSL lab can mirror them term for term.
//
// Skeptical-physics notes that apply throughout:
//   * The clearcoat/base composition is an artist-friendly *approximation*. It
//     models a single coat reflection plus two macro-surface transmissions and
//     deliberately ignores internal inter-reflection between coat and base,
//     refraction/parallax, and lateral light transport. It is not an exact
//     layered light-transport solution and is not claimed to conserve energy
//     exactly.
//   * Single-scattering GGX (the microfacet model used here) loses energy at high
//     roughness; directional-hemispherical reflectance therefore trends below one
//     for rough materials. That is expected, not a bug.
//   * The perceptual-roughness/anisotropy -> (alpha_t, alpha_b) mapping is one of
//     several reasonable conventions, not a uniquely standard one.

#include <cstdint>
#include <expected>

namespace ch23::material_layering
{

// ---------------------------------------------------------------------------
// Scalar/vector/color types. Names encode the intended unit and domain so that
// reflectance (a bounded throughput) is never silently confused with emitted
// radiance (an unbounded additive quantity).
// ---------------------------------------------------------------------------

// A generic Cartesian 3-vector with no normalization guarantee.
struct Float3 final
{
    float x{};
    float y{};
    float z{};

    [[nodiscard]] constexpr bool operator==(Float3 const &) const noexcept = default;
};

// A direction that public entry points validate to be finite and unit length.
// Fields are public so the type is trivial to share with shader-facing code; the
// evaluation contracts still re-validate any caller-constructed value.
struct UnitVector3 final
{
    float x{};
    float y{};
    float z{};

    [[nodiscard]] constexpr bool operator==(UnitVector3 const &) const noexcept = default;
};

// Scene-linear RGB used for *reflectance-domain* quantities: albedo and F0 inputs
// are validated to [0, 1]; BRDF lobe values and throughputs are non-negative but
// may exceed 1 per steradian. This is a three-channel approximation, not spectral.
struct LinearRgb final
{
    float r{};
    float g{};
    float b{};

    [[nodiscard]] constexpr bool operator==(LinearRgb const &) const noexcept = default;
};

// Emitted radiance in scene-linear RGB. This is additive light leaving the
// surface and is deliberately a distinct type from LinearRgb so emission is never
// folded into reflectance or the energy-conservation checks. Non-negative and
// unbounded above.
struct RadianceRgb final
{
    float r{};
    float g{};
    float b{};

    [[nodiscard]] constexpr bool operator==(RadianceRgb const &) const noexcept = default;
};

// A right-handed orthonormal shading basis. The convention is fixed:
//   tangent = local +X, bitangent = local +Y, normal = local +Z,
//   cross(tangent, bitangent) == normal  (right-handed).
// Tangent-space anisotropy is expressed against this basis: alpha_t is the
// roughness along tangent, alpha_b the roughness along bitangent.
struct ShadingFrame final
{
    UnitVector3 tangent{1.0F, 0.0F, 0.0F};
    UnitVector3 bitangent{0.0F, 1.0F, 0.0F};
    UnitVector3 normal{0.0F, 0.0F, 1.0F};

    [[nodiscard]] constexpr bool operator==(ShadingFrame const &) const noexcept = default;
};

// ---------------------------------------------------------------------------
// Material parameters. Emission is stored as a bounded color plus a non-negative
// intensity so the two roles (which channels vs how bright) validate separately;
// EmittedRadiance() combines them into a RadianceRgb.
// ---------------------------------------------------------------------------

struct LayeredMaterial final
{
    // Base layer.
    LinearRgb baseColor{0.5F, 0.5F, 0.5F};           // [0, 1] albedo / metal tint.
    float metallic{};                                // [0, 1] dielectric<->metal blend.
    float perceptualRoughness{0.5F};                 // [0, 1] artist roughness of the base.
    float anisotropy{};                              // [-1, 1] tangent/bitangent roughness bias.
    LinearRgb baseDielectricF0{0.04F, 0.04F, 0.04F}; // [0, 1] normal-incidence dielectric reflectance.

    // Clearcoat layer (an isotropic dielectric coat aligned with the base normal).
    float clearcoatWeight{};         // [0, 1] presence of the coat.
    float clearcoatRoughness{0.05F}; // [0, 1] artist roughness of the coat.

    // Emission (excluded from reflectance / energy accounting).
    LinearRgb emissiveColor{}; // [0, 1] emitted color.
    float emissiveIntensity{}; // >= 0 radiance scale.

    [[nodiscard]] constexpr bool operator==(LayeredMaterial const &) const noexcept = default;
};

// ---------------------------------------------------------------------------
// Errors. Public evaluation/validation contracts fail loudly with a typed error
// rather than silently clamping an invalid caller input into a plausible result.
// ---------------------------------------------------------------------------

enum class MaterialError : std::uint8_t
{
    NonFiniteInput = 0,
    BaseColorOutOfRange,
    MetallicOutOfRange,
    RoughnessOutOfRange,
    AnisotropyOutOfRange,
    DielectricF0OutOfRange,
    ClearcoatWeightOutOfRange,
    ClearcoatRoughnessOutOfRange,
    EmissiveColorOutOfRange,
    EmissiveIntensityOutOfRange,
    ZeroLengthDirection,
    DirectionNotNormalized,
    DegenerateFrame,
    FrameNotRightHanded,
    InvalidQuadrature,
    ArithmeticOverflow,
};

// ---------------------------------------------------------------------------
// Derived roughness. alpha is the GGX roughness parameter (alpha = roughness^2,
// the Disney/UE convention). Analytically, the anisotropic split preserves
// alpha_t * alpha_b == alpha^2 and negating anisotropy swaps the two axes;
// floating-point evaluation introduces ordinary rounding.
// ---------------------------------------------------------------------------

struct AnisotropicRoughness final
{
    float alphaTangent{};   // Strictly positive GGX roughness along the tangent.
    float alphaBitangent{}; // Strictly positive GGX roughness along the bitangent.

    [[nodiscard]] constexpr bool operator==(AnisotropicRoughness const &) const noexcept = default;
};

// ---------------------------------------------------------------------------
// Diagnostic result. EvaluateLayeredBrdf returns the full breakdown, not only a
// final RGB, so a learner (and the tests) can inspect where energy goes.
// ---------------------------------------------------------------------------

struct LayeredBrdfResult final
{
    // Shared microfacet geometry (clamped cosines, all in [0, 1]).
    float nDotV{};
    float nDotL{};
    float nDotH{};

    // Base layer breakdown (before coat attenuation).
    AnisotropicRoughness baseRoughness{};
    float baseNormalDistribution{}; // Anisotropic GGX D for the base.
    float baseMaskingShadowing{};   // Height-correlated Smith G2 for the base.
    LinearRgb baseFresnel{};        // Schlick Fresnel at the base half-angle.
    LinearRgb baseDiffuse{};        // Lambert diffuse * dielectric transmission weight.
    LinearRgb baseSpecular{};       // Cook-Torrance specular lobe.
    LinearRgb baseBrdf{};           // baseDiffuse + baseSpecular.

    // Clearcoat breakdown.
    float coatAlpha{};              // Isotropic GGX roughness for the coat.
    float coatFresnel{};            // Schlick Fresnel at the coat half-angle (dielectric F0).
    float coatNormalDistribution{}; // Isotropic GGX D for the coat.
    float coatMaskingShadowing{};   // Height-correlated Smith G2 for the coat.
    LinearRgb coatSpecular{};       // Coat reflection lobe, already scaled by weight (added once).

    // Energy budget. The coat transmits incident light to the base and transmits
    // the base response back out: two transmissions multiply into coatTransmission.
    float coatTransmissionIn{};  // 1 - weight * F_coat(N.L).
    float coatTransmissionOut{}; // 1 - weight * F_coat(N.V).
    float coatTransmission{};    // coatTransmissionIn * coatTransmissionOut, in [0, 1].
    LinearRgb attenuatedBase{};  // coatTransmission * baseBrdf (base escaping through the coat).

    // Total reflected BRDF (coat reflection + attenuated base). Excludes emission.
    LinearRgb reflectedBrdf{};

    // Emitted radiance, tracked separately from reflectance.
    RadianceRgb emittedRadiance{};

    // Local sanity flag (NOT an energy-conservation proof; see the header notes and
    // IntegrateDirectionalHemisphericalReflectance for the actual energy check):
    // true iff the coat split stayed within [0, 1] and every reflected term is
    // finite and non-negative.
    bool energyBudgetRespected{};

    [[nodiscard]] constexpr bool operator==(LayeredBrdfResult const &) const noexcept = default;
};

// Fixed, deterministic stratified quadrature layout for hemisphere integration.
struct HemisphereQuadrature final
{
    std::uint32_t polarStrata{};     // Strata in cos(theta) over [0, 1].
    std::uint32_t azimuthalStrata{}; // Strata in phi over [0, 2*pi).

    [[nodiscard]] constexpr bool operator==(HemisphereQuadrature const &) const noexcept = default;
};

// ---------------------------------------------------------------------------
// Constants.
// ---------------------------------------------------------------------------

// Perceptual-roughness floor. Roughness of exactly zero is a legal input; it is
// clamped to this floor *inside evaluation only* so alpha stays strictly positive
// and the lobes remain finite. This is a documented numerical safeguard, not a
// way to launder invalid inputs.
inline constexpr float kMinimumPerceptualRoughness = 0.045F;

// Bounds the anisotropic aspect ratio. With this strength the ratio
// alpha_t / alpha_b stays within [1/9, 9] across anisotropy in [-1, 1].
inline constexpr float kAnisotropyAspectStrength = 0.8F;

// Fixed dielectric normal-incidence reflectance for the clearcoat (~1.5 IOR).
inline constexpr float kClearcoatF0 = 0.04F;

// A reasonable default quadrature for reflectance measurement.
inline constexpr HemisphereQuadrature kDefaultReflectanceQuadrature{64U, 128U};

// ---------------------------------------------------------------------------
// Validation and normalization.
//   * ValidateMaterial rejects out-of-domain or non-finite inputs (no clamping).
//   * NormalizeMaterial deliberately clamps UI-style inputs into the valid domain
//     and replaces non-finite values with safe defaults, so the result always
//     passes ValidateMaterial. Use it for sliders, not for trusting raw data.
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<void, MaterialError> ValidateMaterial(LayeredMaterial const &material) noexcept;
[[nodiscard]] LayeredMaterial NormalizeMaterial(LayeredMaterial const &material) noexcept;

// ---------------------------------------------------------------------------
// Geometry helpers.
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<UnitVector3, MaterialError> Normalize(Float3 value) noexcept;

// Builds a right-handed orthonormal frame from a normal and a tangent hint via
// Gram-Schmidt. Fails if either input is degenerate or the two are parallel.
[[nodiscard]] std::expected<ShadingFrame, MaterialError> MakeShadingFrame(Float3 normal, Float3 tangentHint) noexcept;

// Validates orthonormality and right-handedness (cross(T, B) == N) of a frame.
[[nodiscard]] std::expected<void, MaterialError> ValidateShadingFrame(ShadingFrame const &frame) noexcept;

// Expresses a world-space vector in the frame's tangent space (x=T.v, y=B.v, z=N.v).
[[nodiscard]] Float3 ToTangentSpace(ShadingFrame const &frame, Float3 worldVector) noexcept;

// ---------------------------------------------------------------------------
// Roughness mapping.
// ---------------------------------------------------------------------------

// Maps perceptual roughness to the isotropic GGX alpha (= clampedRoughness^2),
// applying the numerical floor.
[[nodiscard]] float PerceptualRoughnessToAlpha(float perceptualRoughness) noexcept;

// Maps (perceptual roughness, anisotropy) to a strictly positive (alpha_t,
// alpha_b) pair. Sign convention: positive anisotropy makes the tangent axis
// rougher (alpha_t > alpha_b); negative makes the bitangent axis rougher; zero is
// isotropic. See header notes: product-preserving and sign-swap symmetric. This
// is a deliberate convention, not a uniquely standard one.
[[nodiscard]] AnisotropicRoughness MapAnisotropicRoughness(float perceptualRoughness, float anisotropy) noexcept;

// ---------------------------------------------------------------------------
// Microfacet building blocks. Inputs are tangent-space vectors so the anisotropy
// axes are unambiguous. All return finite, non-negative values for valid input.
// ---------------------------------------------------------------------------

// Anisotropic Trowbridge-Reitz (GGX) normal distribution. localHalf is the
// tangent-space microfacet normal; only its upper-hemisphere part contributes.
[[nodiscard]] float AnisotropicGgxNdf(Float3 localHalf, AnisotropicRoughness roughness) noexcept;

// Smith GGX Lambda auxiliary for a tangent-space direction. Returns 0 for
// directions at or below the horizon.
[[nodiscard]] float SmithGgxLambda(Float3 localDirection, AnisotropicRoughness roughness) noexcept;

// Single-direction Smith masking G1 = 1 / (1 + Lambda).
[[nodiscard]] float SmithGgxG1(Float3 localDirection, AnisotropicRoughness roughness) noexcept;

// Height-correlated Smith masking-shadowing G2 = 1 / (1 + Lambda_v + Lambda_l)
// (Heitz 2014). Chosen over the separable G1(v)*G1(l) form and named accordingly.
[[nodiscard]] float SmithGgxG2HeightCorrelated(Float3 localView, Float3 localLight,
                                               AnisotropicRoughness roughness) noexcept;

// Schlick Fresnel. cosTheta is clamped to [0, 1] before evaluation.
[[nodiscard]] float SchlickFresnel(float f0, float cosTheta) noexcept;
[[nodiscard]] LinearRgb SchlickFresnel(LinearRgb f0, float cosTheta) noexcept;

// Lambert diffuse BRDF value (baseColor / pi).
[[nodiscard]] LinearRgb LambertDiffuse(LinearRgb baseColor) noexcept;

// Base-layer normal-incidence reflectance: lerp(dielectric F0, baseColor, metallic).
[[nodiscard]] LinearRgb BaseF0(LayeredMaterial const &material) noexcept;

// Combined emitted radiance (emissiveColor * emissiveIntensity).
[[nodiscard]] RadianceRgb EmittedRadiance(LayeredMaterial const &material) noexcept;

// ---------------------------------------------------------------------------
// Evaluation.
// ---------------------------------------------------------------------------

// Full layered BRDF for a single (view, light) pair. Validates the material,
// frame, and directions, then returns the diagnostic breakdown. Hemisphere checks
// and degenerate half-vectors yield zero lobes rather than non-finite output.
[[nodiscard]] std::expected<LayeredBrdfResult, MaterialError> EvaluateLayeredBrdf(
    LayeredMaterial const &material, ShadingFrame const &frame, UnitVector3 directionToView,
    UnitVector3 directionToLight) noexcept;

// Deterministic stratified measurement of directional-hemispherical reflectance
// (the cosine-weighted BRDF integral) per RGB channel, for an arbitrary view
// direction. Emission is excluded. The sample layout is fixed by the quadrature,
// so results are reproducible. This is the honest energy check: passive materials
// should integrate to <= 1 per channel within a documented tolerance.
[[nodiscard]] std::expected<LinearRgb, MaterialError> IntegrateDirectionalHemisphericalReflectance(
    LayeredMaterial const &material, ShadingFrame const &frame, UnitVector3 directionToView,
    HemisphereQuadrature quadrature) noexcept;

} // namespace ch23::material_layering
