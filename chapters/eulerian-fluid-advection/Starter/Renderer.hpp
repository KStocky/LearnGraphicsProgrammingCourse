#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <optional>

namespace ch36::fluid::starter
{
class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    [[nodiscard]] auto Initialize(lgp::framework::ApplicationInitContext const &) -> lgp::framework::Status override;
    [[nodiscard]] auto OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D)
        -> lgp::framework::Status override;
    [[nodiscard]] auto Update(lgp::framework::UpdateContext const &) -> lgp::framework::Status override;
    [[nodiscard]] auto Render(lgp::framework::FrameContext const &) -> lgp::framework::Status override;
    void Shutdown(lgp::framework::DeviceResources &) noexcept override;
    void ConfigureHeadlessTest(gpu::LabConfiguration const &configuration);
    [[nodiscard]] auto ReadBackOutputs() const -> std::expected<gpu::FrameReadback, lgp::framework::Error>;

  private:
    [[nodiscard]] auto Active() const -> gpu::LabConfiguration;
    lgp::framework::DeviceResources *resources_{};
    std::optional<gpu::LabConfiguration> headless_{};
    std::vector<gpu::Buffer> drawSlots_{};
    Field baseline_{};
    bool rendered_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_{};
};
} // namespace ch36::fluid::starter
