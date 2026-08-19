#pragma once

#include "../Common/ChapterTypes.hpp"

namespace ch04::color_pipeline::starter
{

[[nodiscard]] bool IsFiniteNonNegative(float value) noexcept;
[[nodiscard]] bool IsFiniteNonNegative(Color3 color) noexcept;

[[nodiscard]] float ClampDisplayOutput(float value) noexcept;
[[nodiscard]] Color3 ClampDisplayOutput(Color3 color) noexcept;

[[nodiscard]] float SrgbEotfDecode(float encodedSrgb) noexcept;
[[nodiscard]] Color3 SrgbEotfDecode(Color3 encodedSrgb) noexcept;
[[nodiscard]] float SrgbOetfEncode(float linearLight) noexcept;
[[nodiscard]] Color3 SrgbOetfEncode(Color3 linearLight) noexcept;

[[nodiscard]] Color3 BlendLinearLightSrgb(Color3 firstEncodedSrgb, Color3 secondEncodedSrgb, float amount) noexcept;
[[nodiscard]] Color3 BlendEncodedSrgbDiagnostic(Color3 firstEncodedSrgb, Color3 secondEncodedSrgb,
                                                float amount) noexcept;

[[nodiscard]] float RelativeLuminanceRec709(Color3 linearLight) noexcept;
[[nodiscard]] float ExposureMultiplier(float exposureValue) noexcept;
[[nodiscard]] Color3 ApplyExposure(Color3 linearHdr, float exposureValue) noexcept;
[[nodiscard]] Color3 ReinhardToneMap(Color3 linearHdr) noexcept;
[[nodiscard]] Color3 AcesFittedToneMap(Color3 linearHdr) noexcept;
[[nodiscard]] Color3 LinearToDisplaySrgb(Color3 linearHdr, float exposureValue, ToneMapper toneMapper) noexcept;

} // namespace ch04::color_pipeline::starter
