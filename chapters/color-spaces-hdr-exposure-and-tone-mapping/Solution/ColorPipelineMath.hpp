#pragma once

#include "../Common/ChapterTypes.hpp"

namespace ch04::color_pipeline::solution
{

[[nodiscard]] bool IsFiniteNonNegative(float value) noexcept;
[[nodiscard]] bool IsFiniteNonNegative(Color3 color) noexcept;

[[nodiscard]] float ClampDisplayOutput(float value) noexcept;
[[nodiscard]] Color3 ClampDisplayOutput(Color3 color) noexcept;

// Exact IEC 61966-2-1 sRGB electro-optical transfer function, not an arbitrary gamma power.
[[nodiscard]] float SrgbEotfDecode(float encodedSrgb) noexcept;
[[nodiscard]] Color3 SrgbEotfDecode(Color3 encodedSrgb) noexcept;

// Exact IEC 61966-2-1 sRGB opto-electronic transfer function, not an arbitrary gamma power.
[[nodiscard]] float SrgbOetfEncode(float linearLight) noexcept;
[[nodiscard]] Color3 SrgbOetfEncode(Color3 linearLight) noexcept;

[[nodiscard]] Color3 BlendLinearLightSrgb(Color3 firstEncodedSrgb, Color3 secondEncodedSrgb, float amount) noexcept;
[[nodiscard]] Color3 BlendEncodedSrgbDiagnostic(Color3 firstEncodedSrgb, Color3 secondEncodedSrgb,
                                                float amount) noexcept;

[[nodiscard]] float RelativeLuminanceRec709(Color3 linearLight) noexcept;
[[nodiscard]] float ExposureMultiplier(float exposureValue) noexcept;
[[nodiscard]] Color3 ApplyExposure(Color3 linearHdr, float exposureValue) noexcept;

[[nodiscard]] Color3 ReinhardToneMap(Color3 linearHdr) noexcept;

// Stephen Hill ACES-fitted approximation: RGB input/output matrices with the RRT+ODT fit in between.
[[nodiscard]] Color3 AcesFittedToneMap(Color3 linearHdr) noexcept;

// Linear display pipeline order: validate inputs separately, exposure, tone map, sRGB encode,
// then clamp exactly once at the display [0, 1] boundary.
[[nodiscard]] Color3 LinearToDisplaySrgb(Color3 linearHdr, float exposureValue, ToneMapper toneMapper) noexcept;

} // namespace ch04::color_pipeline::solution
