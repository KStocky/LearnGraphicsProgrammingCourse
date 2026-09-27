#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/shader_compiler.hpp>

#include <optional>

namespace ch35::spatial::solution
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
    struct FrameSlot final
    {
        gpu::BufferResource input{};
        gpu::BufferResource ids{};
        gpu::BufferResource output{};
        gpu::BufferResource readback{};
        bool initialized{};
    };

    [[nodiscard]] auto CreateShaders() -> lgp::framework::Status;
    [[nodiscard]] auto CreatePipelines() -> lgp::framework::Status;
    [[nodiscard]] auto CreateResources() -> lgp::framework::Status;
    [[nodiscard]] auto ActiveConfiguration() const -> gpu::LabConfiguration;

    lgp::framework::DeviceResources *resources_{};
    std::optional<gpu::LabConfiguration> headless_{};
    std::uint32_t lastFrameSlot_{};
    std::vector<FrameSlot> slots_{};
    lgp::framework::CompiledShader keyShader_{};
    lgp::framework::CompiledShader sortShader_{};
    lgp::framework::CompiledShader rangeShader_{};
    lgp::framework::CompiledShader queryShader_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRoot_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRoot_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> keyPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> sortPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> rangePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> queryPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
};
} // namespace ch35::spatial::solution
