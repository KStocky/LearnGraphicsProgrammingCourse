#include "ColorPipelineMath.hpp"

#include <algorithm>
#include <cmath>

namespace ch04::color_pipeline::starter
{

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

float SrgbEotfDecode(float) noexcept
{
    return 0.0F;
}

Color3 SrgbEotfDecode(Color3) noexcept
{
    return {};
}

float SrgbOetfEncode(float) noexcept
{
    return 0.0F;
}

Color3 SrgbOetfEncode(Color3) noexcept
{
    return {};
}

Color3 BlendLinearLightSrgb(Color3, Color3, float) noexcept
{
    return {};
}

Color3 BlendEncodedSrgbDiagnostic(Color3 firstEncodedSrgb, Color3, float) noexcept
{
    return firstEncodedSrgb;
}

float RelativeLuminanceRec709(Color3) noexcept
{
    return 0.0F;
}

float ExposureMultiplier(float) noexcept
{
    return 1.0F;
}

Color3 ApplyExposure(Color3 linearHdr, float) noexcept
{
    return linearHdr;
}

Color3 ReinhardToneMap(Color3) noexcept
{
    return {};
}

Color3 AcesFittedToneMap(Color3) noexcept
{
    return {};
}

Color3 LinearToDisplaySrgb(Color3 linearHdr, float, ToneMapper) noexcept
{
    return ClampDisplayOutput(linearHdr);
}

} // namespace ch04::color_pipeline::starter
