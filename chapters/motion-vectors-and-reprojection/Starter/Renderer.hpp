#pragma once

#include <lgp/framework/application.hpp>
#include <lgp/framework/buffer.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <vector>

namespace ch11::reprojection::starter
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
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

  private:
    struct FrameSlotResources final
    {
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, static_cast<std::size_t>(TextureIndex::Count)> textures{};
        lgp::framework::Buffer frameConstants{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateSizeDependentResources(lgp::framework::Extent2D drawableSize);
    void ReleaseSizeDependentResources() noexcept;

    [[nodiscard]] UINT DescriptorIndex(UINT frameSlot, UINT textureIndex, bool uav) const noexcept;
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS WriteFrameConstants(UINT frameSlot, lgp::framework::Extent2D size);

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader generateShader_{};
    lgp::framework::CompiledShader baselineShader_{};
    lgp::framework::CompiledShader fullscreenVertexShader_{};
    lgp::framework::CompiledShader compositePixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> generatePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> baselinePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> compositePipeline_{};
    lgp::framework::DescriptorAllocation textureDescriptors_{};
    std::vector<FrameSlotResources> frameSlots_{};
};

} // namespace ch11::reprojection::starter
