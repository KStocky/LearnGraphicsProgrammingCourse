#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace lgp::framework
{

enum class MouseButton : std::size_t
{
    Left = 0,
    Right,
    Middle,
    X1,
    X2,
    Count,
};

inline constexpr std::size_t kMouseButtonCount = static_cast<std::size_t>(MouseButton::Count);

struct MouseState final
{
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t deltaX{};
    std::int32_t deltaY{};
    float wheelDelta{};
    std::array<bool, kMouseButtonCount> buttonsDown{};
    std::array<bool, kMouseButtonCount> buttonsPressed{};
    std::array<bool, kMouseButtonCount> buttonsReleased{};

    [[nodiscard]] bool IsButtonDown(MouseButton button) const noexcept
    {
        return buttonsDown[static_cast<std::size_t>(button)];
    }

    [[nodiscard]] bool WasButtonPressed(MouseButton button) const noexcept
    {
        return buttonsPressed[static_cast<std::size_t>(button)];
    }

    [[nodiscard]] bool WasButtonReleased(MouseButton button) const noexcept
    {
        return buttonsReleased[static_cast<std::size_t>(button)];
    }

    void ResetTransient() noexcept
    {
        deltaX = 0;
        deltaY = 0;
        wheelDelta = 0.0F;
        buttonsPressed.fill(false);
        buttonsReleased.fill(false);
    }

    void ResetAll() noexcept
    {
        x = 0;
        y = 0;
        buttonsDown.fill(false);
        ResetTransient();
    }
};

struct InputState final
{
    std::array<bool, 256> keysDown{};
    std::array<bool, 256> keysPressed{};
    std::array<bool, 256> keysReleased{};
    MouseState mouse{};
    bool closeRequested{false};

    [[nodiscard]] bool IsKeyDown(std::uint32_t virtualKey) const noexcept
    {
        return virtualKey < keysDown.size() ? keysDown[virtualKey] : false;
    }

    [[nodiscard]] bool WasKeyPressed(std::uint32_t virtualKey) const noexcept
    {
        return virtualKey < keysPressed.size() ? keysPressed[virtualKey] : false;
    }

    [[nodiscard]] bool WasKeyReleased(std::uint32_t virtualKey) const noexcept
    {
        return virtualKey < keysReleased.size() ? keysReleased[virtualKey] : false;
    }

    void ResetTransient() noexcept
    {
        keysPressed.fill(false);
        keysReleased.fill(false);
        mouse.ResetTransient();
    }

    void ResetAll() noexcept
    {
        keysDown.fill(false);
        keysPressed.fill(false);
        keysReleased.fill(false);
        mouse.ResetAll();
        closeRequested = false;
    }
};

} // namespace lgp::framework
