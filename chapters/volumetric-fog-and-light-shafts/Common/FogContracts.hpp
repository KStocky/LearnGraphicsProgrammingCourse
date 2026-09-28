#pragma once
#include <cstdint>
#include <expected>
#include <span>

namespace ch39::fog
{
inline constexpr std::uint32_t kWidth = 32, kHeight = 32, kSlices = 16;
enum class Error : std::uint8_t
{
    InvalidMedium,
    InvalidDistance,
    InvalidPhase
};
struct Medium final
{
    float extinction{0.35F};
    float albedo{0.8F};
    float anisotropy{0.3F};
};
struct Segment final
{
    float length{};
    float density{1.0F};
    float light{1.0F};
    float visibility{1.0F};
};
struct Integral final
{
    float radiance{};
    float transmittance{1.0F};
};
struct HistorySample final
{
    Integral integral{};
    float depth{};
    float signature{};
    bool valid{};
};
[[nodiscard]] std::expected<float, Error> PhaseHG(float cosine, float anisotropy) noexcept;
[[nodiscard]] std::expected<Integral, Error> Integrate(Medium medium, std::span<Segment const> segments,
                                                       Integral initial = {}) noexcept;
[[nodiscard]] bool AcceptHistory(HistorySample previous, float currentDepth, float currentSignature,
                                 float depthTolerance = 0.05F, float historyShiftX = 0.F) noexcept;
[[nodiscard]] float SyntheticDensity(float x, float y, float z) noexcept;
[[nodiscard]] float SyntheticVisibility(float x, float y, float z) noexcept;
[[nodiscard]] float HashJitter(std::uint32_t x, std::uint32_t y, std::uint32_t frame) noexcept;
} // namespace ch39::fog
