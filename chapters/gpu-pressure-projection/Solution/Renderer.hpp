#pragma once

#include "../Common/GpuSupport.hpp"

#include <array>
#include <optional>
#include <vector>

namespace ch37::projection::solution
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
    void ConfigureHeadlessTest(gpu::Configuration const &config);
    [[nodiscard]] auto ReadBackOutputs() -> std::expected<gpu::Readback, lgp::framework::Error>;

  private:
    struct Slot final
    {
        std::array<gpu::Buffer, 2> inputs{};
        std::array<gpu::Buffer, 9> work{};
        std::array<gpu::Buffer, 4> readback{};
        bool used{};
    };
    [[nodiscard]] auto Active() const -> gpu::Configuration;
    lgp::framework::DeviceResources *resources_{};
    std::optional<gpu::Configuration> headless_{};
    std::vector<Slot> slots_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_{}, graphicsRoot_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 17> pipelines_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphics_{};
    std::uint32_t lastSlot_{};
    bool rendered_{};
    bool headlessCli_{};
};
} // namespace ch37::projection::solution
