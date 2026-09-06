# Chapter 23 material-layering convention

This chapter's CPU contract (`MaterialLayering.hpp` / `.cpp`) is a deterministic,
dependency-light mathematical core for a layered material: an anisotropic
metal/dielectric base lobe under a dielectric clearcoat, plus additive emission.
The runnable D3D12/HLSL lab mirrors these contracts term for term where practical,
so the shapes and names are deliberately explicit. Its generated sphere and probe
geometry use canonical right-handed tangent frames.

## Types communicate units and domains

- `LinearRgb` is scene-linear reflectance/throughput. Albedo and F0 inputs are
  validated to `[0, 1]`; BRDF lobe values are non-negative but may exceed 1 per
  steradian.
- `RadianceRgb` is emitted radiance: additive, non-negative, unbounded, and a
  distinct type so emission is never folded into reflectance or the
  energy-conservation checks.

## Shading frame and anisotropy convention

`ShadingFrame` is a right-handed orthonormal basis with
`cross(tangent, bitangent) == normal`, tangent = local +X, bitangent = local +Y,
normal = local +Z. `MakeShadingFrame` builds one by Gram-Schmidt;
`ValidateShadingFrame` checks orthonormality and right-handedness and rejects
left-handed or degenerate bases. Build frames through `MakeShadingFrame` when
importing Chapter 6 tangent data: a mirrored-UV tangent with `w = -1` must be
canonicalized rather than copied directly into this right-handed frame.

`MapAnisotropicRoughness` maps perceptual roughness and anisotropy to
`(alpha_t, alpha_b)`:

- `alpha = clamp(roughness, floor, 1)^2` (the Disney/UE roughness-squared alpha).
- `alpha_t = alpha * sqrt((1 + k*a) / (1 - k*a))`, `alpha_b = alpha^2 / alpha_t`
  with `k = 0.8`.
- Sign convention: **positive anisotropy makes the tangent axis rougher**
  (`alpha_t > alpha_b`); negative makes the bitangent rougher; zero is isotropic.
- Analytically, the product `alpha_t * alpha_b == alpha^2`
  (roughness-product intent), the aspect ratio is bounded (<= 9:1 here), and
  negating anisotropy swaps the two axes. Floating-point evaluation introduces
  ordinary rounding.

This is **one reasonable convention, not a uniquely standard one.** Disney/UE use
`aspect = sqrt(1 - 0.9*aniso)` for a one-sided `aniso in [0, 1]`; the mapping here
is chosen for two-sided sign-swap symmetry so the equivalence contracts are
clean. Both mappings preserve the roughness product analytically.

## Microfacet model

- Anisotropic Trowbridge-Reitz (GGX) normal distribution.
- Height-correlated Smith masking-shadowing (Heitz 2014), named
  `SmithGgxG2HeightCorrelated`, chosen over the separable `G1(v)*G1(l)` form.
- Schlick Fresnel, Lambert diffuse.
- Roughness of exactly zero is a legal input; it is clamped to a small floor
  **inside evaluation only** so lobes stay finite. Invalid inputs (out of range or
  non-finite) are rejected with a typed `MaterialError`, never silently clamped.
  `NormalizeMaterial` is the separate, deliberate UI-clamping helper.

## Layered composition is an approximation

The coat/base composition models one coat reflection plus two macro-surface
transmissions:

```
reflected = coatReflection(once)
          + (1 - w*Fcoat(N.L)) * (1 - w*Fcoat(N.V)) * baseBRDF
```

The coat uses a dielectric F0 of 0.04 and its own roughness. Adding the coat
reflection once and attenuating the base by the two transmissions is what avoids
double-counting reflected energy. This is **not** an exact layered light-transport
solution: it ignores internal inter-reflection between coat and base,
refraction/parallax, and lateral transport, and it does not conserve energy
exactly. The Schlick/Smith building blocks are themselves approximations and are
not claimed to be exact.

## Energy is checked by integration, not asserted

`IntegrateDirectionalHemisphericalReflectance` measures the cosine-weighted BRDF
integral per RGB channel with a fixed deterministic stratified quadrature, for
arbitrary view directions. Emission is excluded. Single-scattering GGX loses
energy at high roughness, so passive materials integrate to at most one within a
loose, documented tolerance rather than to an exact analytic value. The tests
exercise representative and adversarial materials (very smooth, very rough, full
metal, full coat, strong anisotropy, bright emitters) instead of asserting
exactness. The fixed midpoint quadrature can under-resolve a near-delta lobe, so
the diagnostic is a regression bound for the tested sample layout rather than a
proof for an ideal mirror.
