#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/application.hpp>
#include <lgp/framework/buffer.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <cstdint>
#include <vector>

namespace ch23::material_layering::starter
{

// Starter: a deterministic isotropic metal/dielectric baseline. It renders one
// tangent-framed sphere under a single directional light into a linear HDR target
// and tone maps it to the swap-chain surface. The shading is the isotropic base
// lobe (height-correlated Smith) shared with the Solution's Baseline preset, so
// their baseline output is byte identical. There is no clearcoat, emission, or
// anisotropy here; those arrive in the Solution.
class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    using HeadlessTestConfiguration = gpu::LabConfiguration;

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept;

  private:
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelineStates();
    [[nodiscard]] lgp::framework::Status CreateMeshBuffers();
    [[nodiscard]] lgp::framework::Status CreateConstantBuffers();
    [[nodiscard]] lgp::framework::Status CreateSizeDependentTargets(lgp::framework::Extent2D size);
    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;
    [[nodiscard]] gpu::CameraMatrices ActiveCamera(lgp::framework::Extent2D size) const noexcept;

    lgp::framework::DeviceResources *deviceResources_{};
    bool headless_{};

    lgp::framework::CompiledShader sphereVertexShader_{};
    lgp::framework::CompiledShader baselinePixelShader_{};
    lgp::framework::CompiledShader fullscreenVertexShader_{};
    lgp::framework::CompiledShader displayPixelShader_{};

    Microsoft::WRL::ComPtr<ID3D12RootSignature> lightingRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> displayRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> spherePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displayPipeline_{};

    lgp::framework::Buffer sphereVertices_{};
    lgp::framework::Buffer sphereIndices_{};
    D3D12_VERTEX_BUFFER_VIEW sphereVertexView_{};
    D3D12_INDEX_BUFFER_VIEW sphereIndexView_{};
    UINT sphereIndexCount_{};

    std::vector<lgp::framework::Buffer> constantBuffers_{};
    UINT constantBufferStride_{};

    lgp::framework::DescriptorHeap dsvHeap_{};
    lgp::framework::DescriptorAllocation depthView_{};
    lgp::framework::DescriptorAllocation hdrRenderTargetView_{};
    lgp::framework::DescriptorAllocation hdrShaderResourceView_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> hdrTarget_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> depthTarget_{};

    gpu::LabConfiguration headlessConfiguration_{gpu::BaselineConfiguration()};
    bool hasHeadlessConfiguration_{};

    float orbitAzimuth_{0.0F};
    float orbitElevation_{0.25F};
    float orbitRadius_{3.1F};
};

} // namespace ch23::material_layering::starter
