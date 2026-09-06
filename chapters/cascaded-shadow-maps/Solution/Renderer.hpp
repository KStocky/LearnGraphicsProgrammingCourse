#pragma once

#include "../Common/CascadedShadowLab.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/application.hpp>
#include <lgp/framework/buffer.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <vector>

namespace ch24::cascaded_shadows::solution
{

// Solution: a four-cascade directional shadow lab. Each of four depth-only passes
// writes one slice of a Texture2DArray whose orthographic light view is fit to one
// slice of the camera frustum; the lighting pass selects a cascade by view depth,
// cross-fades across an upper-edge blend band, scales the bias per cascade, and
// does a hardware comparison lookup. The Starter's byte-identical single-map path
// is recovered by forcing one cascade. Windowed controls only toggle the debug
// view, stabilization, blending, and shadows; headless runs are deterministic.
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
    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;
    [[nodiscard]] std::expected<gpu::DepthReadback, lgp::framework::Error> ReadBackShadowSlice(std::uint32_t cascade);

  private:
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelineStates();
    [[nodiscard]] lgp::framework::Status CreateSceneBuffers();
    [[nodiscard]] lgp::framework::Status CreateConstantBuffers();
    [[nodiscard]] lgp::framework::Status CreateShadowArray();
    [[nodiscard]] lgp::framework::Status CreateSizeDependentTargets(lgp::framework::Extent2D size);
    void ApplyInteractiveControls(lgp::framework::UpdateContext const &context);

    lgp::framework::DeviceResources *deviceResources_{};
    bool headless_{};

    lgp::framework::CompiledShader shadowVertexShader_{};
    lgp::framework::CompiledShader sceneVertexShader_{};
    lgp::framework::CompiledShader lightingPixelShader_{};

    Microsoft::WRL::ComPtr<ID3D12RootSignature> shadowRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> lightingRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> shadowPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> lightingPipeline_{};

    lgp::framework::Buffer sceneVertices_{};
    lgp::framework::Buffer sceneIndices_{};
    D3D12_VERTEX_BUFFER_VIEW sceneVertexView_{};
    D3D12_INDEX_BUFFER_VIEW sceneIndexView_{};
    UINT sceneIndexCount_{};

    std::vector<lgp::framework::Buffer> labConstantBuffers_{};
    std::vector<lgp::framework::Buffer> shadowConstantBuffers_{};
    UINT labConstantStride_{};
    UINT shadowConstantStride_{};

    lgp::framework::DescriptorHeap dsvHeap_{};
    lgp::framework::DescriptorAllocation sceneDepthView_{};
    std::array<lgp::framework::DescriptorAllocation, gpu::kMaxCascades> shadowSliceViews_{};
    lgp::framework::DescriptorAllocation shadowShaderResourceView_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> shadowArray_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> sceneDepthTarget_{};

    gpu::LabConfiguration headlessConfiguration_{gpu::BaselineConfiguration()};
    bool hasHeadlessConfiguration_{};

    gpu::DebugView interactiveDebugView_{gpu::DebugView::Shaded};
    bool interactiveStabilization_{true};
    bool interactiveBlend_{true};
    bool interactiveShadows_{true};
};

} // namespace ch24::cascaded_shadows::solution
