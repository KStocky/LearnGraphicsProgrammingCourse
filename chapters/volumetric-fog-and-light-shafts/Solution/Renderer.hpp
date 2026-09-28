#pragma once
#include "../Common/GpuSupport.hpp"
#include <optional>
namespace ch39::fog::solution
{
class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &) override;
    void Shutdown(lgp::framework::DeviceResources &) noexcept override;
    void ConfigureHeadlessTest(gpu::Configuration const &config)
    {
        test_ = config;
    }
    [[nodiscard]] std::expected<gpu::Readback, lgp::framework::Error> ReadBackOutputs();

  private:
    lgp::framework::DeviceResources *resources_{};
    gpu::SyntheticVolume volume_{};
    std::optional<gpu::Configuration> test_{};
    bool headless_{};
};
} // namespace ch39::fog::solution
