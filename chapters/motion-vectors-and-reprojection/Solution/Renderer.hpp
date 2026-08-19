#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/buffer.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <expected>
#include <vector>

namespace ch11::reprojection::solution
{

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    enum class TextureIndex : std::uint32_t
    {
        CurrentColor = 0U,
        CurrentDepth,
        CurrentIdentity,
        MotionClipDepth,
        PreviousHistoryUv,
        ReprojectedHistoryColor,
        RejectionReasons,
        ExposureScale,
        Count,
    };

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status BuildUi(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void SetScenario(gpu::Scenario scenario) noexcept;
    void RequestHistoryReset() noexcept;
    [[nodiscard]] UINT HistoryReadIndex() const noexcept;
    [[nodiscard]] UINT HistoryWriteIndex() const noexcept;
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS FrameConstantAddress(UINT frameSlot) const noexcept;

    struct ReadbackOutputs final
    {
        gpu::TextureReadback currentDepth{};
        gpu::TextureReadback currentIdentity{};
        gpu::TextureReadback motionClipDepth{};
        gpu::TextureReadback previousHistoryUv{};
        gpu::TextureReadback rejectionReasons{};
        gpu::TextureReadback exposureScale{};
    };

    [[nodiscard]] std::expected<ReadbackOutputs, lgp::framework::Error> ReadBackOutputs() const;

  private:
    struct FrameSlotResources final
    {
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, static_cast<std::size_t>(TextureIndex::Count)> textures{};
        lgp::framework::Buffer frameConstants{};
    };

    struct HistoryResources final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> color{};
        Microsoft::WRL::ComPtr<ID3D12Resource> depth{};
        Microsoft::WRL::ComPtr<ID3D12Resource> identity{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateSizeDependentResources(lgp::framework::Extent2D drawableSize);
    [[nodiscard]] lgp::framework::Status InitializeImGui();
    void ReleaseSizeDependentResources() noexcept;
    void EnsureDistinctHistoryIndices() noexcept;
    void UpdateHistorySrvs(UINT frameSlot, UINT historyIndex);
    [[nodiscard]] UINT DescriptorIndex(UINT frameSlot, UINT descriptorIndex, bool uav) const noexcept;
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS WriteFrameConstants(UINT frameSlot, lgp::framework::Extent2D size,
                                                                bool resetRequested);

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader generateShader_{};
    lgp::framework::CompiledShader validateShader_{};
    lgp::framework::CompiledShader fullscreenVertexShader_{};
    lgp::framework::CompiledShader compositePixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> generatePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> validatePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> compositePipeline_{};
    lgp::framework::DescriptorAllocation textureDescriptors_{};
    lgp::framework::DescriptorAllocation imguiFontDescriptor_{};
    std::vector<FrameSlotResources> frameSlots_{};
    std::array<HistoryResources, 2U> history_{};
    gpu::Scenario scenario_{gpu::Scenario::MotionCurrent};
    Float2 previousJitterUv_{};
    float previousPreExposure_{1.0F};
    UINT historyReadIndex_{0U};
    UINT historyWriteIndex_{1U};
    UINT lastRenderedFrameSlot_{0U};
    lgp::framework::Extent2D lastDrawableSize_{};
    bool hasHistory_{false};
    bool resetRequested_{false};
    bool headless_{false};
    bool imguiInitialized_{false};
    bool imguiFrameBegun_{false};
};

} // namespace ch11::reprojection::solution
