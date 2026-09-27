#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <optional>

namespace ch36::fluid::solution
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
    [[nodiscard]] auto ReadBackOutputs() -> std::expected<gpu::FrameReadback, lgp::framework::Error>;

  private:
    struct Slot final
    {
        gpu::Buffer scalar{}, u{}, v{}, prediction{}, reverse{}, corrected{}, final{}, readback{};
        gpu::Buffer uIntermediate{}, vIntermediate{}, uFinal{}, vFinal{}, uReadback{}, vReadback{};
        bool used{};
    };
    [[nodiscard]] auto Active() const -> gpu::LabConfiguration;
    [[nodiscard]] auto CreatePipelines() -> lgp::framework::Status;
    [[nodiscard]] auto CreateResources() -> lgp::framework::Status;
    lgp::framework::DeviceResources *resources_{};
    std::optional<gpu::LabConfiguration> headless_{};
    std::vector<Slot> slots_{};
    std::uint32_t lastSlot_{};
    bool rendered_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRoot_{}, graphicsRoot_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> forward_{}, reverse_{}, correct_{}, ghosts_{}, graphics_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> transportU_{}, transportV_{}, velocityGhosts_{};
};
} // namespace ch36::fluid::solution
