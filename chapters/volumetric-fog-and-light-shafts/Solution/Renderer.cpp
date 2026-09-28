#include "Renderer.hpp"
#include <cstdio>
#include <exception>
#include <filesystem>
#include <lgp/framework/error.hpp>
namespace ch39::fog::solution
{
lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    resources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    return volume_.Initialize(*resources_, std::filesystem::path{__FILE__}.parent_path() / "FogLab.hlsl");
}
lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D)
{
    return {};
}
lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &)
{
    return gpu::Validate(test_.value_or(gpu::Configuration{}));
}
lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frame)
{
    return volume_.Render(frame, test_.value_or(gpu::Configuration{}));
}
std::expected<gpu::Readback, lgp::framework::Error> Renderer::ReadBackOutputs()
{
    if (!resources_)
    {
        return std::unexpected(lgp::framework::MakeError("Synthetic fog", "Not initialized."));
    }
    return volume_.ReadBack(*resources_);
}
void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    try
    {
        if (headless_)
        {
            auto result = ReadBackOutputs();
            if (result)
            {
                std::printf(
                    "Solution: bounded synthetic froxels / circular blocker shafts / jitter / validated history: "
                    "center %.4f outside %.4f T %.4f accepted %.0f\n",
                    result->Pixel(16, 16, 0), result->Pixel(2, 2, 0), result->Pixel(16, 16, 1),
                    result->Pixel(16, 16, 3));
            }
            else
            {
                std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
            }
        }
    }
    catch (std::exception const &error)
    {
        std::fprintf(stderr, "Synthetic fog Solution headless readback failed: %s\n", error.what());
    }
    volume_.Shutdown();
    resources_ = nullptr;
}
} // namespace ch39::fog::solution
