#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <vector>

namespace ch22::meshlets::starter
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
    [[nodiscard]] gpu::GpuScene const &LastScene() const noexcept;

  private:
    struct FrameSlot final
    {
        gpu::BufferResource vertices{};
        gpu::BufferResource indices{};
        lgp::framework::DescriptorAllocation descriptor{};
    };

    [[nodiscard]] lgp::framework::Status CreateDeviceObjects();

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_{};
    std::vector<FrameSlot> frameSlots_{};
    gpu::GpuScene scene_{};
};

} // namespace ch22::meshlets::starter
