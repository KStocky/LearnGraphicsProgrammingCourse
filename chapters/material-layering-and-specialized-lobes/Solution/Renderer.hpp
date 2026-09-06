#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/application.hpp>
#include <lgp/framework/buffer.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <cstdint>
#include <expected>
#include <vector>

namespace ch23::material_layering::solution
{

// Solution: an inspectable material-lobe lab. It keeps the Starter's byte-identical
// isotropic baseline (the Baseline preset) and extends it with anisotropy, a
// dielectric clearcoat, additive emission, and diagnostic contribution views. A
// probe-card geometry renders an analytic surface for CPU/GPU parity and anisotropy
// orientation checks. Headless behavior is fully driven by ConfigureHeadlessTest and
// stays deterministic; windowed keyboard controls only switch preset, view,
// geometry, and exposure.
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
    [[nodiscard]] std::expected<gpu::TextureReadback, lgp::framework::Error> ReadBackHdr();
    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;

  private:
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelineStates();
    [[nodiscard]] lgp::framework::Status CreateMeshBuffers();
    [[nodiscard]] lgp::framework::Status CreateConstantBuffers();
    [[nodiscard]] lgp::framework::Status CreateSizeDependentTargets(lgp::framework::Extent2D size);
    [[nodiscard]] gpu::CameraMatrices ActiveCamera(lgp::framework::Extent2D size) const noexcept;
    void ApplyInteractiveControls(lgp::framework::UpdateContext const &context);

    lgp::framework::DeviceResources *deviceResources_{};
    bool headless_{};

    lgp::framework::CompiledShader sphereVertexShader_{};
    lgp::framework::CompiledShader probeVertexShader_{};
    lgp::framework::CompiledShader lightingPixelShader_{};
    lgp::framework::CompiledShader fullscreenVertexShader_{};
    lgp::framework::CompiledShader displayPixelShader_{};

    Microsoft::WRL::ComPtr<ID3D12RootSignature> lightingRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> displayRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> spherePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> probePipeline_{};
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
    lgp::framework::Extent2D hdrSize_{};

    gpu::LabConfiguration headlessConfiguration_{gpu::BaselineConfiguration()};
    bool hasHeadlessConfiguration_{};

    gpu::ScenePreset interactivePreset_{gpu::ScenePreset::Baseline};
    gpu::OutputView interactiveView_{gpu::OutputView::Final};
    gpu::SceneGeometry interactiveGeometry_{gpu::SceneGeometry::Sphere};
    float interactiveExposure_{0.0F};
    float orbitAzimuth_{0.0F};
    float orbitElevation_{0.25F};
    float orbitRadius_{3.1F};
};

} // namespace ch23::material_layering::solution
