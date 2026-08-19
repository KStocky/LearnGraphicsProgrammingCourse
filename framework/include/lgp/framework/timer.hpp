#pragma once

#include <chrono>
#include <cstdint>

namespace lgp::framework
{

struct FrameTiming final
{
    std::uint64_t frameIndex{};
    double elapsedSeconds{};
    double deltaSeconds{};
    bool deterministic{};
};

class FrameTimer final
{
  public:
    explicit FrameTimer(double deterministicDeltaSeconds = 1.0 / 60.0) noexcept;

    void Reset() noexcept;

    [[nodiscard]] FrameTiming Tick(bool deterministic) noexcept;
    [[nodiscard]] double deterministic_delta_seconds() const noexcept;
    void set_deterministic_delta_seconds(double deltaSeconds) noexcept;

  private:
    using clock = std::chrono::steady_clock;

    clock::time_point lastTick_{};
    double totalSeconds_{0.0};
    double deterministicDeltaSeconds_{1.0 / 60.0};
    std::uint64_t frameIndex_{0};
    bool initialized_{false};
};

} // namespace lgp::framework
