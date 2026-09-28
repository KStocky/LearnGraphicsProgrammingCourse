#pragma once

#include "../Common/GpuSupport.hpp"

#include <optional>
#include <vector>

namespace ch38::soft_shadows::solution
{
class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &) override;
    void Shutdown(lgp::framework::DeviceResources &) noexcept override;
    void ConfigureHeadlessTest(gpu::Configuration const &);
    [[nodiscard]] std::expected<gpu::Readback, lgp::framework::Error> ReadBackOutputs();

  private:
    struct Slot final
    {
        gpu::Buffer depth{}, visibility{}, readback{};
        bool used{};
    };
    lgp::framework::DeviceResources *resources_{};
    std::optional<gpu::Configuration> testConfig_{};
    gpu::Configuration interactive_{gpu::Technique::Area};
    std::vector<Slot> slots_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRoot_{}, graphicsRoot_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> generate_{}, shade_{}, graphics_{};
    gpu::Configuration lastConfig_{};
    std::uint32_t lastSlot_{};
    bool rendered_{};
    bool headlessCli_{};
};
} // namespace ch38::soft_shadows::solution
