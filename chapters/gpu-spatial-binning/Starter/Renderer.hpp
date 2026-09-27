#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/shader_compiler.hpp>

#include <optional>

namespace ch35::spatial::starter
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
    [[nodiscard]] auto CreateShaders() -> lgp::framework::Status;
    [[nodiscard]] auto CreatePipeline() -> lgp::framework::Status;
    [[nodiscard]] auto CreateResources() -> lgp::framework::Status;
    [[nodiscard]] auto ActiveConfiguration() const -> gpu::LabConfiguration;

    lgp::framework::DeviceResources *resources_{};
    std::optional<gpu::LabConfiguration> headless_{};
    gpu::FrameReadback reference_{};
    std::vector<gpu::BufferResource> drawSlots_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRoot_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
};
} // namespace ch35::spatial::starter
