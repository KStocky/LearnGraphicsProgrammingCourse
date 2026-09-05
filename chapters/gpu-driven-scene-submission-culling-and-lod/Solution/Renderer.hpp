#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <expected>
#include <optional>
#include <vector>

namespace ch21::gpu_driven::solution
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept;
    [[nodiscard]] std::expected<gpu::ReadbackEvidence, lgp::framework::Error> ReadBackEvidence();
    [[nodiscard]] gpu::CpuReference const &LastReference() const noexcept;

  private:
    struct FrameSlot final
    {
        gpu::BufferResource instances{};
        gpu::BufferResource templates{};
        gpu::BufferResource indices{};
        gpu::BufferResource commands{};
        gpu::BufferResource count{};
        gpu::BufferResource commandsReadback{};
        gpu::BufferResource countReadback{};
        lgp::framework::DescriptorAllocation descriptors{};
        bool writableStateInitialized{};
    };

    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateCommandSignature();
    [[nodiscard]] lgp::framework::Status CreateFrameSlots();

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader resetShader_{};
    lgp::framework::CompiledShader cullShader_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> resetPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> cullPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> commandSignature_{};
    std::vector<FrameSlot> frameSlots_{};
    gpu::CpuReference reference_{};
    std::optional<HeadlessTestConfiguration> headlessConfiguration_{};
    UINT lastFrameSlot_{};
    bool headless_{};
};

} // namespace ch21::gpu_driven::solution
