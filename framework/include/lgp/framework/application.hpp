#pragma once

#include <cstdint>
#include <string>

#include <lgp/framework/command_line.hpp>
#include <lgp/framework/device_resources.hpp>
#include <lgp/framework/error.hpp>
#include <lgp/framework/input.hpp>

namespace lgp::framework
{

class ApplicationControl final
{
  public:
    void RequestExit() noexcept
    {
        exitRequested_ = true;
    }
    [[nodiscard]] bool ExitRequested() const noexcept
    {
        return exitRequested_;
    }

  private:
    bool exitRequested_{false};
};

struct ApplicationConfiguration final
{
    std::wstring title{L"Learn Graphics Programming"};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    std::uint32_t backBufferCount{2};
    std::uint32_t shaderVisibleDescriptorCount{1024};
    std::uint32_t rtvDescriptorCount{32};
    DXGI_FORMAT backBufferFormat{DXGI_FORMAT_R8G8B8A8_UNORM};
    bool enableDebugLayer{true};
    double deterministicDeltaSeconds{1.0 / 60.0};
};

struct ApplicationInitContext final
{
    DeviceResources &deviceResources;
    CommandLineOptions const &commandLine;
    ApplicationControl &application;
    HWND windowHandle{};
    Extent2D drawableSize{};
};

struct UpdateContext final
{
    DeviceResources &deviceResources;
    CommandLineOptions const &commandLine;
    ApplicationControl &application;
    InputState const &input;
    Extent2D drawableSize{};
    std::uint64_t frameIndex{};
    double elapsedSeconds{};
    double deltaSeconds{};
    bool deterministic{};
};

class IChapterRenderer
{
  public:
    virtual ~IChapterRenderer() = default;

    [[nodiscard]] virtual Status Initialize(ApplicationInitContext const &context) = 0;
    [[nodiscard]] virtual Status OnResize(DeviceResources &deviceResources, Extent2D drawableSize) = 0;
    [[nodiscard]] virtual Status Update(UpdateContext const &context) = 0;
    [[nodiscard]] virtual Status BuildUi(UpdateContext const &context)
    {
        (void)context;
        return {};
    }
    [[nodiscard]] virtual Status Render(FrameContext const &frameContext) = 0;
    virtual void Shutdown(DeviceResources &deviceResources) noexcept = 0;
};

[[nodiscard]] Result<int> RunApplication(ApplicationConfiguration const &configuration, IChapterRenderer &renderer);
[[nodiscard]] Result<int> RunApplication(ApplicationConfiguration const &configuration, int argc,
                                         wchar_t const *const *argv, IChapterRenderer &renderer);
[[nodiscard]] Result<int> RunApplication(ApplicationConfiguration const &configuration,
                                         CommandLineOptions const &options, IChapterRenderer &renderer);

} // namespace lgp::framework
