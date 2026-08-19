#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <expected>
#include <optional>
#include <vector>

namespace ch15::visibility_buffer::solution
{

struct HeadlessTestConfiguration final
{
    gpu::ScenePreset scene{gpu::ScenePreset::Diagnostic};
    gpu::DepthConvention depthConvention{gpu::DepthConvention::Forward};
    gpu::DebugView debugView{gpu::DebugView::Final};
    std::uint32_t lightCount{gpu::kMaximumLightCount};
};

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    using ReadbackOutputs = gpu::FrameReadback;

    Renderer() = default;
    Renderer(Renderer &&) noexcept = default;
    Renderer &operator=(Renderer &&) noexcept = default;
    Renderer(Renderer const &) = delete;
    Renderer &operator=(Renderer const &) = delete;
    ~Renderer() override = default;

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept;
    [[nodiscard]] std::expected<ReadbackOutputs, lgp::framework::Error> ReadBackOutputs();

  private:
    struct MeshBuffers final
    {
        gpu::BufferResource vertices{};
        gpu::BufferResource indices{};
        gpu::BufferResource draws{};
        D3D12_VERTEX_BUFFER_VIEW vertexView{};
        D3D12_INDEX_BUFFER_VIEW indexView{};
    };

    struct FrameSlotResources final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> visibility{};
        Microsoft::WRL::ComPtr<ID3D12Resource> depth{};
        Microsoft::WRL::ComPtr<ID3D12Resource> output{};
        gpu::BufferResource lights{};
        gpu::BufferResource cells{};
        gpu::BufferResource lightIndices{};
        gpu::BufferResource diagnostics{};
        gpu::BufferResource diagnosticsReadback{};
        gpu::TextureReadbackBuffer visibilityReadback{};
        gpu::TextureReadbackBuffer depthReadback{};
        lgp::framework::DescriptorAllocation rtv{};
        lgp::framework::DescriptorAllocation dsv{};
        lgp::framework::DescriptorAllocation descriptors{};
        bool diagnosticsInitialized{};
    };

    [[nodiscard]] lgp::framework::Status ValidateRequiredFeatures();
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateGeometry();
    [[nodiscard]] lgp::framework::Status CreateSizeDependentResources(lgp::framework::Extent2D size);
    void ReleaseSizeDependentResources() noexcept;
    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader rasterVertexShader_{};
    lgp::framework::CompiledShader rasterPixelShader_{};
    lgp::framework::CompiledShader shadeShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> rasterForwardPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> rasterReversedPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> shadePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> materialTexture_{};
    MeshBuffers mesh_{};
    gpu::SceneData diagnosticScene_{};
    lgp::framework::DescriptorHeap rtvHeap_{};
    lgp::framework::DescriptorHeap dsvHeap_{};
    std::vector<FrameSlotResources> frameSlots_{};
    std::vector<gpu::PointLightData> currentLights_{};
    std::optional<HeadlessTestConfiguration> headlessConfiguration_{};
    gpu::DebugView interactiveDebugView_{gpu::DebugView::Final};
    std::uint32_t lastRenderedFrameSlot_{};
    std::uint32_t lastRenderedCellCount_{};
    std::uint32_t lastRenderedLightIndexCount_{};
    std::uint32_t lastRenderedPixelCount_{};
    gpu::PerspectiveProjection lastProjection_{};
    gpu::LabConfiguration lastConfiguration_{};
    bool headless_{};
};

} // namespace ch15::visibility_buffer::solution
