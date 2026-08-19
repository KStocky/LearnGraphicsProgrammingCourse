#pragma once

#include "ChapterTypes.hpp"

#include <expected>

namespace ch05::lighting
{

inline constexpr float kMinimumRoughness = 0.045F;

[[nodiscard]] std::expected<UnitDirection3, LightingError> Normalize(Float3 value) noexcept;
[[nodiscard]] float ClampedDot(UnitDirection3 first, UnitDirection3 second) noexcept;

[[nodiscard]] std::expected<void, LightingError> ValidateMaterial(MaterialParameters const &material) noexcept;
[[nodiscard]] std::expected<void, LightingError> ValidateDirectionalLight(DirectionalLight const &light) noexcept;
[[nodiscard]] std::expected<void, LightingError> ValidatePointLight(PointLight const &light) noexcept;

// The directional control is illuminance on a surface perpendicular to the
// light direction. Tilting the surface applies max(N dot L, 0).
[[nodiscard]] std::expected<IncidentLight, LightingError> EvaluateDirectionalLight(
    DirectionalLight const &light, UnitDirection3 surfaceNormal) noexcept;

// Point-light illuminance is I / r^2 at normal incidence, where I is luminous
// intensity in candela. Tilting the surface additionally applies max(N dot L, 0).
[[nodiscard]] std::expected<IncidentLight, LightingError> EvaluatePointLight(PointLight const &light,
                                                                             Float3 surfacePosition,
                                                                             UnitDirection3 surfaceNormal) noexcept;

[[nodiscard]] LinearRgb LambertDiffuse(LinearRgb baseColor) noexcept;
[[nodiscard]] LinearRgb SchlickFresnel(LinearRgb f0, float vDotH) noexcept;
[[nodiscard]] float GgxTrowbridgeReitzNdf(float nDotH, float roughness) noexcept;
[[nodiscard]] float SmithGgxMaskingShadowing(float nDotV, float nDotL, float roughness) noexcept;
[[nodiscard]] LinearRgb CookTorranceSpecular(LinearRgb fresnel, float normalDistribution, float maskingShadowing,
                                             float nDotV, float nDotL) noexcept;
[[nodiscard]] LinearRgb DielectricMetalF0(MaterialParameters const &material) noexcept;
[[nodiscard]] LinearRgb DiffuseEnergyWeight(LinearRgb fresnel, float metalness) noexcept;

[[nodiscard]] std::expected<BrdfDiagnosticTerms, LightingError> EvaluateDirectLightBrdf(
    MaterialParameters const &material, UnitDirection3 surfaceNormal, UnitDirection3 directionToView,
    UnitDirection3 directionToLight) noexcept;

} // namespace ch05::lighting
