#include "SoftShadowContracts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ch38::soft_shadows
{
namespace
{

[[nodiscard]] bool Unit(float value) noexcept
{
    return value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] std::expected<bool, SoftShadowError> ValidateReceiver(Receiver receiver) noexcept
{
    if (!std::isfinite(receiver.uv.x) || !std::isfinite(receiver.uv.y) || !std::isfinite(receiver.depth) ||
        !std::isfinite(receiver.bias))
    {
        return std::unexpected(SoftShadowError::NonFiniteValue);
    }
    if (receiver.bias < 0.0F || receiver.bias > 1.0F)
    {
        return std::unexpected(SoftShadowError::InvalidDepth);
    }
    return Unit(receiver.uv.x) && Unit(receiver.uv.y) && Unit(receiver.depth);
}

[[nodiscard]] std::expected<void, SoftShadowError> ValidateMap(ShadowMap map) noexcept
{
    if (map.width == 0U || map.height == 0U ||
        static_cast<std::size_t>(map.width) > (std::numeric_limits<std::size_t>::max)() / map.height ||
        map.depths.size() != static_cast<std::size_t>(map.width) * map.height)
    {
        return std::unexpected(SoftShadowError::InvalidMap);
    }
    for (float depth : map.depths)
    {
        if (!std::isfinite(depth))
        {
            return std::unexpected(SoftShadowError::NonFiniteValue);
        }
        if (!Unit(depth))
        {
            return std::unexpected(SoftShadowError::InvalidDepth);
        }
    }
    return {};
}

[[nodiscard]] std::int64_t Texel(float uv, std::uint32_t extent) noexcept
{
    return static_cast<std::int64_t>(
        std::min(std::floor(static_cast<double>(uv) * extent), static_cast<double>(extent - 1U)));
}

[[nodiscard]] float Sample(ShadowMap map, std::int64_t x, std::int64_t y) noexcept
{
    if (x < 0 || y < 0 || x >= map.width || y >= map.height)
    {
        return 1.0F;
    }
    return map.depths[static_cast<std::size_t>(y) * map.width + static_cast<std::size_t>(x)];
}

[[nodiscard]] float FilterValidated(ShadowMap map, Receiver receiver, std::uint32_t radius) noexcept
{
    std::uint32_t lit = 0U;
    auto const x = Texel(receiver.uv.x, map.width);
    auto const y = Texel(receiver.uv.y, map.height);
    auto const comparisonDepth = receiver.depth - receiver.bias;
    auto const signedRadius = static_cast<std::int64_t>(radius);
    for (auto dy = -signedRadius; dy <= signedRadius; ++dy)
    {
        for (auto dx = -signedRadius; dx <= signedRadius; ++dx)
        {
            lit += comparisonDepth <= Sample(map, x + dx, y + dy) ? 1U : 0U;
        }
    }
    auto const side = 2U * radius + 1U;
    return static_cast<float>(lit) / static_cast<float>(side * side);
}

[[nodiscard]] std::uint32_t CappedRadius(double desired, std::uint32_t maximum) noexcept
{
    if (desired >= maximum)
    {
        return maximum;
    }
    return static_cast<std::uint32_t>(std::ceil(desired));
}

[[nodiscard]] std::uint64_t NextRandom(std::uint64_t &state) noexcept
{
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] double Uniform(std::uint64_t &state) noexcept
{
    return static_cast<double>(NextRandom(state) >> 11U) * 0x1.0p-53;
}

} // namespace

std::expected<Comparison, SoftShadowError> CompareDepth(Receiver receiver, float storedDepth) noexcept
{
    auto const valid = ValidateReceiver(receiver);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (!std::isfinite(storedDepth))
    {
        return std::unexpected(SoftShadowError::NonFiniteValue);
    }
    if (!Unit(storedDepth))
    {
        return std::unexpected(SoftShadowError::InvalidDepth);
    }
    if (!*valid)
    {
        return Comparison{true, false};
    }
    return Comparison{receiver.depth - receiver.bias <= storedDepth, true};
}

std::expected<float, SoftShadowError> PercentageCloserFilter(ShadowMap map, Receiver receiver,
                                                             std::uint32_t radiusTexels) noexcept
{
    if (radiusTexels > 32U)
    {
        return std::unexpected(SoftShadowError::InvalidRadius);
    }
    auto const valid = ValidateReceiver(receiver);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    auto const mapValid = ValidateMap(map);
    if (!mapValid)
    {
        return std::unexpected(mapValid.error());
    }
    return *valid ? FilterValidated(map, receiver, radiusTexels) : 1.0F;
}

std::expected<PcssResult, SoftShadowError> EstimatePcss(ShadowMap map, Receiver receiver,
                                                        PcssParameters parameters) noexcept
{
    if (!std::isfinite(parameters.lightRadiusTexels) || !std::isfinite(parameters.lightNearDepth))
    {
        return std::unexpected(SoftShadowError::NonFiniteValue);
    }
    if (parameters.lightRadiusTexels < 0.0F || !Unit(parameters.lightNearDepth) || parameters.lightNearDepth == 0.0F ||
        parameters.maxRadiusTexels == 0U || parameters.maxRadiusTexels > 32U)
    {
        return std::unexpected(SoftShadowError::InvalidGeometry);
    }
    auto const valid = ValidateReceiver(receiver);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    auto const mapValid = ValidateMap(map);
    if (!mapValid)
    {
        return std::unexpected(mapValid.error());
    }
    if (!*valid)
    {
        return PcssResult{};
    }

    PcssResult result{};
    result.compared = true;
    if (receiver.depth <= parameters.lightNearDepth)
    {
        result.visibility = FilterValidated(map, receiver, 0U);
        return result;
    }
    auto const search = static_cast<double>(parameters.lightRadiusTexels) *
                        (receiver.depth - static_cast<double>(parameters.lightNearDepth)) / receiver.depth;
    result.searchRadiusTexels = CappedRadius(search, parameters.maxRadiusTexels);
    auto const x = Texel(receiver.uv.x, map.width);
    auto const y = Texel(receiver.uv.y, map.height);
    auto const comparisonDepth = receiver.depth - receiver.bias;
    double depthSum = 0.0;
    auto const radius = static_cast<std::int64_t>(result.searchRadiusTexels);
    for (auto dy = -radius; dy <= radius; ++dy)
    {
        for (auto dx = -radius; dx <= radius; ++dx)
        {
            auto const depth = Sample(map, x + dx, y + dy);
            if (depth < comparisonDepth)
            {
                depthSum += depth;
                ++result.blockerCount;
            }
        }
    }
    if (result.blockerCount == 0U)
    {
        return result;
    }
    auto const average = depthSum / result.blockerCount;
    result.averageBlockerDepth = static_cast<float>(average);
    result.degenerateBlockerDepth = average == 0.0;
    auto const penumbra = result.degenerateBlockerDepth ? std::numeric_limits<double>::infinity()
                                                        : static_cast<double>(parameters.lightRadiusTexels) *
                                                              (receiver.depth - average) / average;
    result.penumbraClamped = penumbra > parameters.maxRadiusTexels;
    result.filterRadiusTexels = CappedRadius(penumbra, parameters.maxRadiusTexels);
    result.visibility = FilterValidated(map, receiver, result.filterRadiusTexels);
    return result;
}

std::expected<VisibilityEstimate, SoftShadowError> SampleAreaLightVisibility(std::span<DiskBlocker const> blockers,
                                                                             std::uint32_t sampleCount,
                                                                             std::uint64_t seed, float alpha) noexcept
{
    if (!std::isfinite(alpha))
    {
        return std::unexpected(SoftShadowError::NonFiniteValue);
    }
    if (alpha <= 0.0F || alpha >= 1.0F)
    {
        return std::unexpected(SoftShadowError::InvalidConfidence);
    }
    if (sampleCount == 0U || sampleCount > kMaxAreaLightSamples)
    {
        return std::unexpected(SoftShadowError::InvalidSampleCount);
    }
    for (auto const &blocker : blockers)
    {
        if (!std::isfinite(blocker.center.x) || !std::isfinite(blocker.center.y) || !std::isfinite(blocker.radius))
        {
            return std::unexpected(SoftShadowError::NonFiniteValue);
        }
        if (blocker.radius < 0.0F)
        {
            return std::unexpected(SoftShadowError::InvalidGeometry);
        }
    }

    std::uint32_t visible = 0U;
    for (std::uint32_t index = 0U; index < sampleCount; ++index)
    {
        auto const distance = std::sqrt(Uniform(seed));
        auto const angle = 2.0 * std::numbers::pi * Uniform(seed);
        auto const x = distance * std::cos(angle);
        auto const y = distance * std::sin(angle);
        bool blocked = false;
        for (auto const &blocker : blockers)
        {
            auto const dx = x - blocker.center.x;
            auto const dy = y - blocker.center.y;
            auto const radius = static_cast<double>(blocker.radius);
            if (dx * dx + dy * dy <= radius * radius)
            {
                blocked = true;
                break;
            }
        }
        visible += blocked ? 0U : 1U;
    }
    auto const mean = static_cast<double>(visible) / sampleCount;
    auto const variance = sampleCount > 1U ? mean * (1.0 - mean) * sampleCount / (sampleCount - 1.0) : 0.0;
    auto const bound = std::sqrt(std::log(2.0 / alpha) / (2.0 * sampleCount));
    return VisibilityEstimate{static_cast<float>(mean),
                              static_cast<float>(variance),
                              static_cast<float>(std::sqrt(variance / sampleCount)),
                              static_cast<float>(bound),
                              sampleCount,
                              visible};
}

} // namespace ch38::soft_shadows
