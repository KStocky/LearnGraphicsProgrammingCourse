#include <lgp/framework/timer.hpp>

namespace lgp::framework
{

FrameTimer::FrameTimer(double deterministicDeltaSeconds) noexcept
    : deterministicDeltaSeconds_{deterministicDeltaSeconds > 0.0 ? deterministicDeltaSeconds : (1.0 / 60.0)}
{
    Reset();
}

void FrameTimer::Reset() noexcept
{
    lastTick_ = clock::now();
    totalSeconds_ = 0.0;
    frameIndex_ = 0;
    initialized_ = false;
}

FrameTiming FrameTimer::Tick(bool deterministic) noexcept
{
    double deltaSeconds = 0.0;

    if (deterministic)
    {
        deltaSeconds = deterministicDeltaSeconds_;
    }
    else
    {
        clock::time_point const currentTick = clock::now();
        if (initialized_)
        {
            deltaSeconds = std::chrono::duration<double>(currentTick - lastTick_).count();
        }

        lastTick_ = currentTick;
        initialized_ = true;
    }

    totalSeconds_ += deltaSeconds;

    FrameTiming timing{};
    timing.frameIndex = frameIndex_;
    timing.elapsedSeconds = totalSeconds_;
    timing.deltaSeconds = deltaSeconds;
    timing.deterministic = deterministic;

    ++frameIndex_;
    return timing;
}

double FrameTimer::deterministic_delta_seconds() const noexcept
{
    return deterministicDeltaSeconds_;
}

void FrameTimer::set_deterministic_delta_seconds(double deltaSeconds) noexcept
{
    if (deltaSeconds > 0.0)
    {
        deterministicDeltaSeconds_ = deltaSeconds;
    }
}

} // namespace lgp::framework
