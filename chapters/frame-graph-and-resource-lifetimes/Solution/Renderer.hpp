#pragma once

#include "../Common/PassGraph.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <expected>
#include <optional>
#include <vector>

namespace ch08::frame_graph::solution
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

    [[nodiscard]] CompiledPassGraph const *LastCompiledGraph() const noexcept;

  private:
    struct FrameGraphPlan final
    {
        CompiledPassGraph compiledGraph{};
        TextureResourceId hdrResourceId{};
        TextureResourceId frameTargetResourceId{};
        PassId analyticPassId{};
        PassId displayPassId{};
        PassId frameBoundaryPassId{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelineStates();
    [[nodiscard]] lgp::framework::Status CreateHdrIntermediate(lgp::framework::Extent2D size);
    [[nodiscard]] lgp::framework::Status InitializeImGui();
    [[nodiscard]] std::expected<FrameGraphPlan, lgp::framework::Error> CompileFrameGraph(
        lgp::framework::FrameContext const &frameContext) const;
    [[nodiscard]] lgp::framework::Status ApplyBarrierGroup(ID3D12GraphicsCommandList7 &commandList,
                                                           TextureBarrierGroup const &group,
                                                           std::vector<ID3D12Resource *> const &resources) const;
    void BuildDiagnosticsWindow(lgp::framework::UpdateContext const &context) const;
    void ExecuteAnalyticPass(ID3D12GraphicsCommandList7 &commandList) const;
    void ExecuteDisplayPass(lgp::framework::FrameContext const &frameContext);

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader fullscreenVertexShader_{};
    lgp::framework::CompiledShader hdrPixelShader_{};
    lgp::framework::CompiledShader displayPixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> hdrPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displayPipeline_{};
    lgp::framework::DescriptorAllocation hdrRtv_{};
    lgp::framework::DescriptorAllocation hdrSrv_{};
    lgp::framework::DescriptorAllocation imguiFontDescriptor_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> hdrIntermediate_{};
    bool headless_{};
    bool imguiInitialized_{};
    bool imguiFrameBegun_{};
    std::optional<std::uint64_t> lastCompiledFrameIndex_{};
    std::optional<CompiledPassGraph> lastCompiledGraph_{};
};

} // namespace ch08::frame_graph::solution
