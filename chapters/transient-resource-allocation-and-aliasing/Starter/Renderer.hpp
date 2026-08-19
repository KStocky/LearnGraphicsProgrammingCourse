#pragma once

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <array>

namespace ch09::transient_aliasing::starter
{

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

  private:
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelineStates();
    [[nodiscard]] lgp::framework::Status CreateTransientTextures(lgp::framework::Extent2D size);
    void DrawTo(ID3D12GraphicsCommandList7 &commandList, ID3D12PipelineState &pipeline,
                D3D12_CPU_DESCRIPTOR_HANDLE renderTarget) const;

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader fullscreenVertexShader_{};
    lgp::framework::CompiledShader analyticPixelShader_{};
    lgp::framework::CompiledShader copyPixelShader_{};
    lgp::framework::CompiledShader accentPixelShader_{};
    lgp::framework::CompiledShader compositePixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> analyticPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> copyPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> accentPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> compositePipeline_{};
    lgp::framework::DescriptorAllocation transientRtvs_{};
    lgp::framework::DescriptorAllocation transientSrvs_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 3U> transientTextures_{};
};

} // namespace ch09::transient_aliasing::starter
