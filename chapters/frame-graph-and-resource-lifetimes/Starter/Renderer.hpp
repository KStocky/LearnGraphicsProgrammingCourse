#pragma once

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

namespace ch08::frame_graph::starter
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
    [[nodiscard]] lgp::framework::Status CreateHdrIntermediate(lgp::framework::Extent2D size);

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader fullscreenVertexShader_{};
    lgp::framework::CompiledShader hdrPixelShader_{};
    lgp::framework::CompiledShader displayPixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> hdrPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displayPipeline_{};
    lgp::framework::DescriptorAllocation hdrRtv_{};
    lgp::framework::DescriptorAllocation hdrSrv_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> hdrIntermediate_{};
};

} // namespace ch08::frame_graph::starter
