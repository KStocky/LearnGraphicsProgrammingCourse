#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/buffer.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <expected>
#include <optional>
#include <vector>

namespace ch14::clustered_lighting::solution
{

struct HeadlessTestConfiguration final
{
    gpu::LightingMode mode{gpu::LightingMode::Clustered};
    gpu::ScenePreset scene{gpu::ScenePreset::Diagnostic};
    gpu::LightPreset lights{gpu::LightPreset::Diagnostic};
    DepthConvention depthConvention{DepthConvention::Forward};
    gpu::DebugView debugView{gpu::DebugView::Final};
    std::uint32_t lightCount{gpu::kMaximumLightCount};
    std::uint32_t capacity{gpu::kMaximumLightIndexCapacity};
};

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    struct ReadbackOutputs final
    {
        BoundedLightLists lists{};
        std::vector<std::uint32_t> attemptedCounts{};
        gpu::TextureReadback depth{};
        std::vector<gpu::PointLightData> lights{};
        PerspectiveProjection projection{};
        gpu::LabConfiguration configuration{};
        std::uint32_t cellCount{};
        bool unusedStorageIntact{};
    };

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
        lgp::framework::Buffer vertices{};
        lgp::framework::Buffer indices{};
        D3D12_VERTEX_BUFFER_VIEW vertexView{};
        D3D12_INDEX_BUFFER_VIEW indexView{};
        UINT indexCount{};
    };

    struct FrameSlotResources final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> depth{};
        gpu::BufferResource lights{};
        gpu::BufferResource counts{};
        gpu::BufferResource cells{};
        gpu::BufferResource indices{};
        gpu::BufferResource statistics{};
        gpu::BufferResource countsReadback{};
        gpu::BufferResource cellsReadback{};
        gpu::BufferResource indicesReadback{};
        gpu::BufferResource statisticsReadback{};
        lgp::framework::DescriptorAllocation dsvs{};
        lgp::framework::DescriptorAllocation descriptors{};
        std::uint32_t allocatedCellCount{};
        bool writableStateInitialized{false};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateGraphicsRootSignature();
    [[nodiscard]] lgp::framework::Status CreateComputeRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateGeometry();
    [[nodiscard]] lgp::framework::Status CreateSizeDependentResources(lgp::framework::Extent2D size);
    void ReleaseSizeDependentResources() noexcept;
    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader depthVertexShader_{};
    lgp::framework::CompiledShader forwardVertexShader_{};
    lgp::framework::CompiledShader forwardPixelShader_{};
    lgp::framework::CompiledShader resetShader_{};
    lgp::framework::CompiledShader countShader_{};
    lgp::framework::CompiledShader prefixShader_{};
    lgp::framework::CompiledShader fillShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> depthForwardPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> depthReversedPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> shadeForwardPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> shadeReversedPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> resetPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> countPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> prefixPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> fillPipeline_{};
    MeshBuffers quad_{};
    lgp::framework::DescriptorHeap dsvHeap_{};
    std::vector<FrameSlotResources> frameSlots_{};
    std::vector<gpu::SceneObject> currentScene_{};
    std::vector<gpu::PointLightData> currentLights_{};
    std::optional<HeadlessTestConfiguration> headlessConfiguration_{};
    gpu::LightingMode interactiveMode_{gpu::LightingMode::Clustered};
    gpu::DebugView interactiveDebugView_{gpu::DebugView::Final};
    std::uint32_t lastRenderedFrameSlot_{};
    std::uint32_t lastRenderedCellCount_{};
    PerspectiveProjection lastProjection_{};
    gpu::LabConfiguration lastConfiguration_{};
    bool headless_{false};
};

} // namespace ch14::clustered_lighting::solution
