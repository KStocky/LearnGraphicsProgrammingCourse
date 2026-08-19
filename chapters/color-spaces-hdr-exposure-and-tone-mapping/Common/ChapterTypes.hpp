#pragma once

#include <cstdint>

namespace ch04::color_pipeline
{

struct Color3 final
{
    float r{};
    float g{};
    float b{};

    [[nodiscard]] constexpr bool operator==(Color3 const &) const noexcept = default;
};

enum class ToneMapper : std::uint8_t
{
    None = 0,
    Reinhard,
    AcesFitted,
};

} // namespace ch04::color_pipeline
