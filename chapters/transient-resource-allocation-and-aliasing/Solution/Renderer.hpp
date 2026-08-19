#pragma once

#include "../Common/TransientTextureAllocation.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <array>
#include <optional>
#include <vector>

namespace ch09::transient_aliasing::solution
{

struct ExecutedAliasBarrierRecord final
{
    ch08::frame_graph::TextureResourceId resourceId{};
    lgp::framework::TextureBarrierState before{};
    lgp::framework::TextureBarrierState after{};
    D3D12_TEXTURE_BARRIER_FLAGS flags{D3D12_TEXTURE_BARRIER_FLAG_NONE};
};

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    [[nodiscard]] ch08::frame_graph::CompiledPassGraph const *CompiledGraph() const noexcept;
    [[nodiscard]] transient_allocation::TransientTextureAllocationPlan const *AllocationPlan() const noexcept;
    [[nodiscard]] std::vector<ExecutedAliasBarrierRecord> const &LastAliasHandoffTrace() const noexcept;

  private:
    struct GraphExecutionPlan final
    {
        ch08::frame_graph::CompiledPassGraph graph{};
        ch08::frame_graph::TextureResourceId textureA{};
        ch08::frame_graph::TextureResourceId textureB{};
        ch08::frame_graph::TextureResourceId textureC{};
        ch08::frame_graph::TextureResourceId frameTarget{};
        ch08::frame_graph::PassId analyticPass{};
        ch08::frame_graph::PassId copyPass{};
        ch08::frame_graph::PassId accentPass{};
        ch08::frame_graph::PassId compositePass{};
        ch08::frame_graph::PassId boundaryPass{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelineStates();
    [[nodiscard]] lgp::framework::Status CreateTransientTextures(lgp::framework::Extent2D size);
    [[nodiscard]] lgp::framework::Status InitializeImGui();
    [[nodiscard]] std::expected<GraphExecutionPlan, lgp::framework::Error> CompileGraph() const;
    [[nodiscard]] lgp::framework::Status ApplyBarrierGroup(ID3D12GraphicsCommandList7 &commandList,
                                                           ch08::frame_graph::TextureBarrierGroup const &group,
                                                           std::vector<ID3D12Resource *> const &resources) const;
    [[nodiscard]] lgp::framework::Status ExecuteAliasHandoff(ID3D12GraphicsCommandList7 &commandList);
    void DrawTo(ID3D12GraphicsCommandList7 &commandList, ID3D12PipelineState &pipeline,
                D3D12_CPU_DESCRIPTOR_HANDLE renderTarget) const;
    void BuildDiagnosticsWindow() const;
    void ExecuteComposite(lgp::framework::FrameContext const &frameContext);

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
    lgp::framework::DescriptorAllocation imguiFontDescriptor_{};
    Microsoft::WRL::ComPtr<ID3D12Heap> transientHeap_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 3U> transientTextures_{};
    std::optional<GraphExecutionPlan> graphPlan_{};
    std::optional<transient_allocation::TransientTextureAllocationPlan> allocationPlan_{};
    std::vector<ExecutedAliasBarrierRecord> lastAliasHandoffTrace_{};
    bool headless_{};
    bool imguiInitialized_{};
    bool imguiFrameBegun_{};
};

} // namespace ch09::transient_aliasing::solution
