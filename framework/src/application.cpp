#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <shellapi.h>
#include <windowsx.h>

#include <lgp/framework/application.hpp>
#include <lgp/framework/timer.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace lgp::framework
{
namespace
{

inline constexpr wchar_t kWindowClassName[] = L"LgpFrameworkWindowClass";

class Window final
{
  public:
    static Result<std::unique_ptr<Window>> Create(std::wstring_view title, Extent2D clientSize);

    ~Window()
    {
        if (handle_ != nullptr)
        {
            ::DestroyWindow(handle_);
            handle_ = nullptr;
        }
    }

    Window(Window const &) = delete;
    Window &operator=(Window const &) = delete;

    void Show() const noexcept
    {
        ::ShowWindow(handle_, SW_SHOWDEFAULT);
        ::UpdateWindow(handle_);
    }

    [[nodiscard]] bool PumpMessages(int &exitCode) noexcept
    {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != FALSE)
        {
            if (message.message == WM_QUIT)
            {
                exitCode = static_cast<int>(message.wParam);
                return false;
            }

            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        return true;
    }

    [[nodiscard]] HWND handle() const noexcept
    {
        return handle_;
    }
    [[nodiscard]] InputState &input() noexcept
    {
        return input_;
    }
    [[nodiscard]] InputState const &input() const noexcept
    {
        return input_;
    }
    [[nodiscard]] bool minimized() const noexcept
    {
        return minimized_;
    }
    [[nodiscard]] bool HasPendingResize() const noexcept
    {
        return pendingResize_;
    }

    [[nodiscard]] Extent2D ConsumePendingResize() noexcept
    {
        pendingResize_ = false;
        return clientSize_;
    }

  private:
    Window() = default;

    static Status RegisterWindowClass();
    static LRESULT CALLBACK WindowProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam);

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;
    void UpdateMousePosition(std::int32_t x, std::int32_t y) noexcept;
    void SetMouseButton(MouseButton button, bool pressed) noexcept;
    void ReleaseMouseCaptureIfNeeded() noexcept;

    HWND handle_{};
    InputState input_{};
    Extent2D clientSize_{};
    bool minimized_{false};
    bool pendingResize_{false};
    bool hasMousePosition_{false};
};

struct RendererShutdownGuard final
{
    IChapterRenderer &renderer;
    DeviceResources *deviceResources;
    bool initialized{false};

    ~RendererShutdownGuard()
    {
        if (initialized && deviceResources != nullptr)
        {
            (void)deviceResources->WaitForGpuIdle();
            renderer.Shutdown(*deviceResources);
        }
    }
};

[[nodiscard]] Status EnsureValidConfiguration(ApplicationConfiguration const &configuration)
{
    if (configuration.width == 0U || configuration.height == 0U)
    {
        return std::unexpected(MakeError("RunApplication", "Window width and height must be greater than zero."));
    }

    if (configuration.backBufferCount == 0U)
    {
        return std::unexpected(MakeError("RunApplication", "At least one frame context is required."));
    }

    if (configuration.shaderVisibleDescriptorCount == 0U)
    {
        return std::unexpected(
            MakeError("RunApplication", "The shader-visible descriptor heap must contain at least one descriptor."));
    }

    if (configuration.rtvDescriptorCount == 0U)
    {
        return std::unexpected(
            MakeError("RunApplication", "The RTV descriptor heap must contain at least one descriptor."));
    }

    if (configuration.deterministicDeltaSeconds <= 0.0)
    {
        return std::unexpected(MakeError("RunApplication", "The deterministic delta time must be greater than zero."));
    }

    return {};
}

Status Window::RegisterWindowClass()
{
    HINSTANCE const moduleHandle = ::GetModuleHandleW(nullptr);
    if (moduleHandle == nullptr)
    {
        return std::unexpected(
            MakeLastError("GetModuleHandleW", "Failed to get the module handle for Win32 window creation."));
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &Window::WindowProc;
    windowClass.hInstance = moduleHandle;
    windowClass.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = kWindowClassName;

    ATOM const result = ::RegisterClassExW(&windowClass);
    if (result == 0U)
    {
        DWORD const win32Error = ::GetLastError();
        if (win32Error != ERROR_CLASS_ALREADY_EXISTS)
        {
            return std::unexpected(
                MakeWin32Error("RegisterClassExW", win32Error, "Failed to register the Win32 framework window class."));
        }
    }

    return {};
}

Result<std::unique_ptr<Window>> Window::Create(std::wstring_view title, Extent2D clientSize)
{
    auto const registration = RegisterWindowClass();
    if (!registration)
    {
        return std::unexpected(std::move(registration.error()));
    }

    RECT windowRect{
        0,
        0,
        static_cast<LONG>(clientSize.width),
        static_cast<LONG>(clientSize.height),
    };

    if (::AdjustWindowRectEx(&windowRect, WS_OVERLAPPEDWINDOW, FALSE, 0U) == FALSE)
    {
        return std::unexpected(MakeLastError(
            "AdjustWindowRectEx", "Failed to calculate the Win32 outer window size for the requested client area."));
    }

    std::unique_ptr<Window> window{new Window()};
    std::wstring const windowTitle{title};

    HINSTANCE const moduleHandle = ::GetModuleHandleW(nullptr);
    if (moduleHandle == nullptr)
    {
        return std::unexpected(
            MakeLastError("GetModuleHandleW", "Failed to get the module handle for window creation."));
    }

    window->handle_ =
        ::CreateWindowExW(0U, kWindowClassName, windowTitle.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                          windowRect.right - windowRect.left, windowRect.bottom - windowRect.top, nullptr, nullptr,
                          moduleHandle, window.get());

    if (window->handle_ == nullptr)
    {
        return std::unexpected(MakeLastError("CreateWindowExW", "Failed to create the framework window."));
    }

    RECT clientRect{};
    if (::GetClientRect(window->handle_, &clientRect) != FALSE)
    {
        window->clientSize_.width = static_cast<std::uint32_t>(clientRect.right - clientRect.left);
        window->clientSize_.height = static_cast<std::uint32_t>(clientRect.bottom - clientRect.top);
    }
    else
    {
        window->clientSize_ = clientSize;
    }

    return window;
}

LRESULT CALLBACK Window::WindowProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        auto *createStruct = reinterpret_cast<CREATESTRUCTW *>(lParam);
        auto *window = static_cast<Window *>(createStruct->lpCreateParams);
        ::SetWindowLongPtrW(windowHandle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->handle_ = windowHandle;
    }

    auto *window = reinterpret_cast<Window *>(::GetWindowLongPtrW(windowHandle, GWLP_USERDATA));
    if (window != nullptr)
    {
        return window->HandleMessage(message, wParam, lParam);
    }

    return ::DefWindowProcW(windowHandle, message, wParam, lParam);
}

LRESULT Window::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
    switch (message)
    {
    case WM_CLOSE:
        input_.closeRequested = true;
        ::DestroyWindow(handle_);
        return 0;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;

    case WM_NCDESTROY:
    {
        HWND const windowHandle = handle_;
        ::SetWindowLongPtrW(handle_, GWLP_USERDATA, 0);
        handle_ = nullptr;
        return ::DefWindowProcW(windowHandle, message, wParam, lParam);
    }

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
        {
            minimized_ = true;
            clientSize_ = {};
            pendingResize_ = false;
            return 0;
        }

        minimized_ = false;
        clientSize_.width = static_cast<std::uint32_t>(LOWORD(lParam));
        clientSize_.height = static_cast<std::uint32_t>(HIWORD(lParam));
        pendingResize_ = true;
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        std::size_t const keyIndex = static_cast<std::size_t>(wParam);
        if (keyIndex < input_.keysDown.size())
        {
            if (!input_.keysDown[keyIndex])
            {
                input_.keysPressed[keyIndex] = true;
            }
            input_.keysDown[keyIndex] = true;
        }
        return 0;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        std::size_t const keyIndex = static_cast<std::size_t>(wParam);
        if (keyIndex < input_.keysDown.size())
        {
            if (input_.keysDown[keyIndex])
            {
                input_.keysReleased[keyIndex] = true;
            }
            input_.keysDown[keyIndex] = false;
        }
        return 0;
    }

    case WM_MOUSEMOVE:
        UpdateMousePosition(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_LBUTTONDOWN:
        SetMouseButton(MouseButton::Left, true);
        ::SetCapture(handle_);
        return 0;

    case WM_LBUTTONUP:
        SetMouseButton(MouseButton::Left, false);
        ReleaseMouseCaptureIfNeeded();
        return 0;

    case WM_RBUTTONDOWN:
        SetMouseButton(MouseButton::Right, true);
        ::SetCapture(handle_);
        return 0;

    case WM_RBUTTONUP:
        SetMouseButton(MouseButton::Right, false);
        ReleaseMouseCaptureIfNeeded();
        return 0;

    case WM_MBUTTONDOWN:
        SetMouseButton(MouseButton::Middle, true);
        ::SetCapture(handle_);
        return 0;

    case WM_MBUTTONUP:
        SetMouseButton(MouseButton::Middle, false);
        ReleaseMouseCaptureIfNeeded();
        return 0;

    case WM_XBUTTONDOWN:
    {
        UINT const button = GET_XBUTTON_WPARAM(wParam);
        if (button == XBUTTON1)
        {
            SetMouseButton(MouseButton::X1, true);
        }
        else if (button == XBUTTON2)
        {
            SetMouseButton(MouseButton::X2, true);
        }
        ::SetCapture(handle_);
        return TRUE;
    }

    case WM_XBUTTONUP:
    {
        UINT const button = GET_XBUTTON_WPARAM(wParam);
        if (button == XBUTTON1)
        {
            SetMouseButton(MouseButton::X1, false);
        }
        else if (button == XBUTTON2)
        {
            SetMouseButton(MouseButton::X2, false);
        }
        ReleaseMouseCaptureIfNeeded();
        return TRUE;
    }

    case WM_MOUSEWHEEL:
        input_.mouse.wheelDelta += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
        return 0;

    case WM_KILLFOCUS:
        input_.ResetAll();
        hasMousePosition_ = false;
        return 0;

    default:
        return ::DefWindowProcW(handle_, message, wParam, lParam);
    }
}

void Window::UpdateMousePosition(std::int32_t x, std::int32_t y) noexcept
{
    if (hasMousePosition_)
    {
        input_.mouse.deltaX += x - input_.mouse.x;
        input_.mouse.deltaY += y - input_.mouse.y;
    }
    else
    {
        hasMousePosition_ = true;
    }

    input_.mouse.x = x;
    input_.mouse.y = y;
}

void Window::SetMouseButton(MouseButton button, bool pressed) noexcept
{
    std::size_t const index = static_cast<std::size_t>(button);
    if (index >= input_.mouse.buttonsDown.size())
    {
        return;
    }

    if (pressed)
    {
        if (!input_.mouse.buttonsDown[index])
        {
            input_.mouse.buttonsPressed[index] = true;
        }
        input_.mouse.buttonsDown[index] = true;
    }
    else
    {
        if (input_.mouse.buttonsDown[index])
        {
            input_.mouse.buttonsReleased[index] = true;
        }
        input_.mouse.buttonsDown[index] = false;
    }
}

void Window::ReleaseMouseCaptureIfNeeded() noexcept
{
    for (bool const buttonState : input_.mouse.buttonsDown)
    {
        if (buttonState)
        {
            return;
        }
    }

    if (::GetCapture() == handle_)
    {
        ::ReleaseCapture();
    }
}

} // namespace

Result<int> RunApplication(ApplicationConfiguration const &configuration, IChapterRenderer &renderer)
{
    int argumentCount = 0;
    LPWSTR *const arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argumentCount);
    if (arguments == nullptr)
    {
        return std::unexpected(MakeLastError("CommandLineToArgvW", "Failed to read the current process command line."));
    }

    auto const parsedOptions = ParseCommandLine(argumentCount, const_cast<wchar_t const *const *>(arguments));
    ::LocalFree(arguments);

    if (!parsedOptions)
    {
        return std::unexpected(std::move(parsedOptions.error()));
    }

    return RunApplication(configuration, parsedOptions.value(), renderer);
}

Result<int> RunApplication(ApplicationConfiguration const &configuration, int argc, wchar_t const *const *argv,
                           IChapterRenderer &renderer)
{
    auto const parsedOptions = ParseCommandLine(argc, argv);
    if (!parsedOptions)
    {
        return std::unexpected(std::move(parsedOptions.error()));
    }

    return RunApplication(configuration, parsedOptions.value(), renderer);
}

Result<int> RunApplication(ApplicationConfiguration const &configuration, CommandLineOptions const &options,
                           IChapterRenderer &renderer)
{
    auto const configurationStatus = EnsureValidConfiguration(configuration);
    if (!configurationStatus)
    {
        return std::unexpected(std::move(configurationStatus.error()));
    }

    if (options.showHelp)
    {
        std::fputws(UsageText().c_str(), stdout);
        return 0;
    }

    std::unique_ptr<Window> window;
    if (!options.headless)
    {
        auto windowResult = Window::Create(configuration.title, {configuration.width, configuration.height});
        if (!windowResult)
        {
            return std::unexpected(std::move(windowResult.error()));
        }

        window = std::move(windowResult.value());
    }

    DeviceResourcesConfiguration deviceConfiguration{};
    deviceConfiguration.windowHandle = window != nullptr ? window->handle() : nullptr;
    deviceConfiguration.width = configuration.width;
    deviceConfiguration.height = configuration.height;
    deviceConfiguration.backBufferCount = configuration.backBufferCount;
    deviceConfiguration.shaderVisibleDescriptorCount = configuration.shaderVisibleDescriptorCount;
    deviceConfiguration.rtvDescriptorCount = configuration.rtvDescriptorCount;
    deviceConfiguration.backBufferFormat = configuration.backBufferFormat;
    deviceConfiguration.enableDebugLayer = configuration.enableDebugLayer;
    deviceConfiguration.useWarpAdapter = options.useWarpAdapter;
    deviceConfiguration.headless = options.headless;

    auto deviceResourcesResult = DeviceResources::Create(deviceConfiguration);
    if (!deviceResourcesResult)
    {
        return std::unexpected(std::move(deviceResourcesResult.error()));
    }

    std::unique_ptr<DeviceResources> deviceResources = std::move(deviceResourcesResult.value());

    ApplicationControl applicationControl;
    RendererShutdownGuard shutdownGuard{renderer, deviceResources.get(), false};

    ApplicationInitContext initContext{
        *deviceResources,
        options,
        applicationControl,
        window != nullptr ? window->handle() : nullptr,
        deviceResources->drawable_size(),
    };

    auto const initializeStatus = renderer.Initialize(initContext);
    if (!initializeStatus)
    {
        return std::unexpected(std::move(initializeStatus.error()));
    }

    shutdownGuard.initialized = true;

    auto const resizeStatus = renderer.OnResize(*deviceResources, deviceResources->drawable_size());
    if (!resizeStatus)
    {
        return std::unexpected(std::move(resizeStatus.error()));
    }

    if (window != nullptr)
    {
        window->Show();
    }

    FrameTimer timer{configuration.deterministicDeltaSeconds};
    bool const deterministic = options.headless || options.maxFrameCount.has_value();

    int exitCode = 0;
    InputState const emptyInput{};

    while (!applicationControl.ExitRequested())
    {
        if (window != nullptr)
        {
            if (!window->PumpMessages(exitCode))
            {
                return exitCode;
            }

            if (window->HasPendingResize())
            {
                Extent2D const newSize = window->ConsumePendingResize();
                if (!newSize.empty())
                {
                    auto const deviceResize = deviceResources->Resize(newSize.width, newSize.height);
                    if (!deviceResize)
                    {
                        return std::unexpected(std::move(deviceResize.error()));
                    }

                    auto const rendererResize = renderer.OnResize(*deviceResources, newSize);
                    if (!rendererResize)
                    {
                        return std::unexpected(std::move(rendererResize.error()));
                    }
                }
            }

            if (window->minimized() && !options.maxFrameCount.has_value())
            {
                ::WaitMessage();
                continue;
            }
        }

        FrameTiming const timing = timer.Tick(deterministic);
        InputState const &input = window != nullptr ? static_cast<InputState const &>(window->input()) : emptyInput;

        UpdateContext updateContext{
            *deviceResources,
            options,
            applicationControl,
            input,
            deviceResources->drawable_size(),
            timing.frameIndex,
            timing.elapsedSeconds,
            timing.deltaSeconds,
            timing.deterministic,
        };

        auto const updateStatus = renderer.Update(updateContext);
        if (!updateStatus)
        {
            return std::unexpected(std::move(updateStatus.error()));
        }

        auto const uiStatus = renderer.BuildUi(updateContext);
        if (!uiStatus)
        {
            return std::unexpected(std::move(uiStatus.error()));
        }

        auto frameContext = deviceResources->BeginFrame(timing.frameIndex);
        if (!frameContext)
        {
            return std::unexpected(std::move(frameContext.error()));
        }

        auto const renderStatus = renderer.Render(frameContext.value());
        if (!renderStatus)
        {
            return std::unexpected(std::move(renderStatus.error()));
        }

        auto const endFrameStatus = deviceResources->EndFrame(frameContext.value());
        if (!endFrameStatus)
        {
            return std::unexpected(std::move(endFrameStatus.error()));
        }

        if (window != nullptr)
        {
            window->input().ResetTransient();
        }

        if (options.maxFrameCount.has_value() && ((timing.frameIndex + 1U) >= options.maxFrameCount.value()))
        {
            applicationControl.RequestExit();
        }
    }

    return exitCode;
}

} // namespace lgp::framework
