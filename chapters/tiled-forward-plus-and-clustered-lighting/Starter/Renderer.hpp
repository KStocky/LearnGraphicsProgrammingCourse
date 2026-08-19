#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/buffer.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <optional>
#include <vector>

namespace ch14::clustered_lighting::starter
{

struct HeadlessTestConfiguration final
{
    gpu::ScenePreset scene{gpu::ScenePreset::Diagnostic};
    gpu::LightPreset lights{gpu::LightPreset::Diagnostic};
    DepthConvention depthConvention{DepthConvention::Forward};
    std::uint32_t lightCount{gpu::kMaximumLightCount};
};

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
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
        lgp::framework::DescriptorAllocation dsv{};
        lgp::framework::DescriptorAllocation lightSrv{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateGeometry();
    [[nodiscard]] lgp::framework::Status CreateSizeDependentResources(lgp::framework::Extent2D size);
    void ReleaseSizeDependentResources() noexcept;
    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> forwardPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> reversedPipeline_{};
    MeshBuffers quad_{};
    lgp::framework::DescriptorHeap dsvHeap_{};
    std::vector<FrameSlotResources> frameSlots_{};
    std::vector<gpu::SceneObject> currentScene_{};
    std::vector<gpu::PointLightData> currentLights_{};
    std::optional<HeadlessTestConfiguration> headlessConfiguration_{};
    bool headless_{false};
};

} // namespace ch14::clustered_lighting::starter
