#include "ColorPipelineMath.hpp"

#include <algorithm>
#include <cmath>

namespace ch04::color_pipeline::solution
{
namespace
{

inline constexpr float kSrgbDecodeThreshold = 0.04045F;
inline constexpr float kSrgbEncodeThreshold = 0.0031308F;
inline constexpr float kSrgbLinearScale = 12.92F;
inline constexpr float kSrgbA = 0.055F;
inline constexpr float kSrgbGamma = 2.4F;
inline constexpr float kRec709Red = 0.2126F;
inline constexpr float kRec709Green = 0.7152F;
inline constexpr float kRec709Blue = 0.0722F;

[[nodiscard]] float Lerp(float first, float second, float amount) noexcept
{
    return first + ((second - first) * amount);
}

[[nodiscard]] Color3 Lerp(Color3 first, Color3 second, float amount) noexcept
{
    return {
        Lerp(first.r, second.r, amount),
        Lerp(first.g, second.g, amount),
        Lerp(first.b, second.b, amount),
    };
}

[[nodiscard]] Color3 Mul(Color3 color, float scalar) noexcept
{
    return {
        color.r * scalar,
        color.g * scalar,
        color.b * scalar,
    };
}

[[nodiscard]] Color3 Mul3x3(Color3 color, float const (&matrix)[3][3]) noexcept
{
    return {
        (color.r * matrix[0][0]) + (color.g * matrix[0][1]) + (color.b * matrix[0][2]),
        (color.r * matrix[1][0]) + (color.g * matrix[1][1]) + (color.b * matrix[1][2]),
        (color.r * matrix[2][0]) + (color.g * matrix[2][1]) + (color.b * matrix[2][2]),
    };
}

[[nodiscard]] Color3 RrtAndOdtFit(Color3 color) noexcept
{
    auto const fit = [](float value) noexcept
    {
        float const numerator = (value * (value + 0.0245786F)) - 0.000090537F;
        float const denominator = (value * ((0.983729F * value) + 0.4329510F)) + 0.238081F;
        return numerator / denominator;
    };

    return {
        fit(color.r),
        fit(color.g),
        fit(color.b),
    };
}

} // namespace

bool IsFiniteNonNegative(float value) noexcept
{
    return std::isfinite(value) && value >= 0.0F;
}

bool IsFiniteNonNegative(Color3 color) noexcept
{
    return IsFiniteNonNegative(color.r) && IsFiniteNonNegative(color.g) && IsFiniteNonNegative(color.b);
}

float ClampDisplayOutput(float value) noexcept
{
    if (std::isnan(value))
    {
        return 0.0F;
    }
    return std::clamp(value, 0.0F, 1.0F);
}

Color3 ClampDisplayOutput(Color3 color) noexcept
{
    return {
        ClampDisplayOutput(color.r),
        ClampDisplayOutput(color.g),
        ClampDisplayOutput(color.b),
    };
}

float SrgbEotfDecode(float encodedSrgb) noexcept
{
    if (encodedSrgb <= kSrgbDecodeThreshold)
    {
        return encodedSrgb / kSrgbLinearScale;
    }
    return std::pow((encodedSrgb + kSrgbA) / (1.0F + kSrgbA), kSrgbGamma);
}

Color3 SrgbEotfDecode(Color3 encodedSrgb) noexcept
{
    return {
        SrgbEotfDecode(encodedSrgb.r),
        SrgbEotfDecode(encodedSrgb.g),
        SrgbEotfDecode(encodedSrgb.b),
    };
}

float SrgbOetfEncode(float linearLight) noexcept
{
    if (linearLight <= kSrgbEncodeThreshold)
    {
        return linearLight * kSrgbLinearScale;
    }
    return ((1.0F + kSrgbA) * std::pow(linearLight, 1.0F / kSrgbGamma)) - kSrgbA;
}

Color3 SrgbOetfEncode(Color3 linearLight) noexcept
{
    return {
        SrgbOetfEncode(linearLight.r),
        SrgbOetfEncode(linearLight.g),
        SrgbOetfEncode(linearLight.b),
    };
}

Color3 BlendLinearLightSrgb(Color3 firstEncodedSrgb, Color3 secondEncodedSrgb, float amount) noexcept
{
    float const clampedAmount = std::clamp(amount, 0.0F, 1.0F);
    Color3 const firstLinear = SrgbEotfDecode(firstEncodedSrgb);
    Color3 const secondLinear = SrgbEotfDecode(secondEncodedSrgb);
    return SrgbOetfEncode(Lerp(firstLinear, secondLinear, clampedAmount));
}

Color3 BlendEncodedSrgbDiagnostic(Color3 firstEncodedSrgb, Color3 secondEncodedSrgb, float amount) noexcept
{
    return Lerp(firstEncodedSrgb, secondEncodedSrgb, std::clamp(amount, 0.0F, 1.0F));
}

float RelativeLuminanceRec709(Color3 linearLight) noexcept
{
    return (linearLight.r * kRec709Red) + (linearLight.g * kRec709Green) + (linearLight.b * kRec709Blue);
}

float ExposureMultiplier(float exposureValue) noexcept
{
    return std::exp2(exposureValue);
}

Color3 ApplyExposure(Color3 linearHdr, float exposureValue) noexcept
{
    return Mul(linearHdr, ExposureMultiplier(exposureValue));
}

Color3 ReinhardToneMap(Color3 linearHdr) noexcept
{
    auto const reinhard = [](float value) noexcept { return value / (1.0F + value); };

    return {
        reinhard(linearHdr.r),
        reinhard(linearHdr.g),
        reinhard(linearHdr.b),
    };
}

Color3 AcesFittedToneMap(Color3 linearHdr) noexcept
{
    static constexpr float acesInputMatrix[3][3]{
        {0.59719F, 0.35458F, 0.04823F},
        {0.07600F, 0.90834F, 0.01566F},
        {0.02840F, 0.13383F, 0.83777F},
    };
    static constexpr float acesOutputMatrix[3][3]{
        {1.60475F, -0.53108F, -0.07367F},
        {-0.10208F, 1.10813F, -0.00605F},
        {-0.00327F, -0.07276F, 1.07602F},
    };

    return Mul3x3(RrtAndOdtFit(Mul3x3(linearHdr, acesInputMatrix)), acesOutputMatrix);
}

Color3 LinearToDisplaySrgb(Color3 linearHdr, float exposureValue, ToneMapper toneMapper) noexcept
{
    Color3 const exposed = ApplyExposure(linearHdr, exposureValue);
    Color3 toneMapped = exposed;

    switch (toneMapper)
    {
    case ToneMapper::None:
        toneMapped = exposed;
        break;
    case ToneMapper::Reinhard:
        toneMapped = ReinhardToneMap(exposed);
        break;
    case ToneMapper::AcesFitted:
        toneMapped = AcesFittedToneMap(exposed);
        break;
    }

    return ClampDisplayOutput(SrgbOetfEncode(toneMapped));
}

} // namespace ch04::color_pipeline::solution
