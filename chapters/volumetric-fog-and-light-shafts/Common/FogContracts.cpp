#include "FogContracts.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ch39::fog
{
std::expected<float, Error> PhaseHG(float cosine, float anisotropy) noexcept
{
    if (!std::isfinite(cosine) || !std::isfinite(anisotropy) || cosine < -1 || cosine > 1 || std::abs(anisotropy) >= 1)
    {
        return std::unexpected(Error::InvalidPhase);
    }
    float const denominator = 1 + anisotropy * anisotropy - 2 * anisotropy * cosine;
    return (1 - anisotropy * anisotropy) / (4 * std::numbers::pi_v<float> * denominator * std::sqrt(denominator));
}
std::expected<Integral, Error> Integrate(Medium medium, std::span<Segment const> segments, Integral initial) noexcept
{
    if (!std::isfinite(medium.extinction) || medium.extinction < 0 || !std::isfinite(medium.albedo) ||
        medium.albedo < 0 || medium.albedo > 1 || !std::isfinite(medium.anisotropy) ||
        std::abs(medium.anisotropy) >= 1 || !std::isfinite(initial.radiance) || initial.radiance < 0 ||
        !std::isfinite(initial.transmittance) || initial.transmittance < 0 || initial.transmittance > 1)
    {
        return std::unexpected(Error::InvalidMedium);
    }
    for (auto const &s : segments)
    {
        if (!std::isfinite(s.length) || s.length < 0 || !std::isfinite(s.density) || s.density < 0 ||
            !std::isfinite(s.light) || s.light < 0 || !std::isfinite(s.visibility) || s.visibility < 0 ||
            s.visibility > 1)
        {
            return std::unexpected(Error::InvalidDistance);
        }
        float const tau = medium.extinction * s.density * s.length;
        float const opacity = -std::expm1(-tau);
        initial.radiance += initial.transmittance * opacity * medium.albedo * s.light * s.visibility;
        initial.transmittance *= std::exp(-tau);
        if (!std::isfinite(initial.radiance))
        {
            return std::unexpected(Error::InvalidMedium);
        }
    }
    return initial;
}
bool AcceptHistory(HistorySample previous, float currentDepth, float currentSignature, float depthTolerance,
                   float historyShiftX) noexcept
{
    return previous.valid && std::isfinite(previous.integral.radiance) && previous.integral.radiance >= 0 &&
           std::isfinite(previous.integral.transmittance) && previous.integral.transmittance >= 0 &&
           previous.integral.transmittance <= 1 && std::isfinite(currentDepth) && std::isfinite(previous.depth) &&
           std::isfinite(currentSignature) && std::isfinite(previous.signature) && std::isfinite(depthTolerance) &&
           depthTolerance >= 0 && std::isfinite(historyShiftX) && historyShiftX == 0.F &&
           std::abs(currentDepth - previous.depth) <= depthTolerance && currentSignature == previous.signature;
}
float SyntheticDensity(float x, float y, float z) noexcept
{
    return 0.6F + ((x > 0.2F && x < 0.8F && y > 0.2F && y < 0.8F && z > 2 && z < 6) ? 0.8F : 0.F);
}
float SyntheticVisibility(float x, float y, float z) noexcept
{
    if (z <= 3)
    {
        return 1;
    }
    float const px = 0.5F + (x - 0.5F) * 3 / z;
    float const py = 0.5F + (y - 0.5F) * 3 / z;
    return (px - 0.5F) * (px - 0.5F) + (py - 0.5F) * (py - 0.5F) < 0.11F * 0.11F ? 0.F : 1.F;
}
float HashJitter(std::uint32_t x, std::uint32_t y, std::uint32_t frame) noexcept
{
    auto n = x + y * kWidth + frame * 0x9e3779b9U;
    n ^= n >> 16;
    n *= 0x7feb352dU;
    n ^= n >> 15;
    n *= 0x846ca68bU;
    n ^= n >> 16;
    return static_cast<float>(n & 0xffffU) / 65536.F;
}
} // namespace ch39::fog
