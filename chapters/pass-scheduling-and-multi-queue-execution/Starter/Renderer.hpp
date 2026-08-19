#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>

#include <wrl/client.h>

#include <array>
#include <optional>
#include <vector>

namespace ch10::pass_scheduling::starter
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

    [[nodiscard]] gpu::LabGraph const *CompiledGraph() const noexcept;
    [[nodiscard]] SchedulePlan const *SerialSchedule() const noexcept;
    [[nodiscard]] gpu::PhysicalTextureByteSizes const *PhysicalByteSizes() const noexcept;
    [[nodiscard]] std::vector<gpu::QueueExecutionTraceRecord> const &LastExecutionTrace() const noexcept;

  private:
    struct FrameSlotTextures final
    {
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, gpu::kLabTextureCount> textures{};
    };

    [[nodiscard]] lgp::framework::Status CreateDeviceObjects();
    [[nodiscard]] lgp::framework::Status CreateSizeDependentResources(lgp::framework::Extent2D size);
    void ReleaseSizeDependentResources() noexcept;
    void DrawTo(ID3D12GraphicsCommandList7 &commandList, ID3D12PipelineState &pipeline,
                D3D12_CPU_DESCRIPTOR_HANDLE renderTarget) const;
    void Dispatch(ID3D12GraphicsCommandList7 &commandList, ID3D12PipelineState &pipeline,
                  lgp::framework::Extent2D size) const;

    [[nodiscard]] UINT DescriptorIndex(UINT frameSlot, UINT withinSlot) const noexcept;
    [[nodiscard]] UINT RtvIndex(UINT frameSlot, UINT withinSlot) const noexcept;

    lgp::framework::DeviceResources *deviceResources_{};
    gpu::LabShaders shaders_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_{};
    gpu::LabPipelines pipelines_{};
    lgp::framework::DescriptorAllocation textureDescriptors_{};
    lgp::framework::DescriptorAllocation textureRtvs_{};
    std::vector<FrameSlotTextures> frameSlots_{};
    std::optional<gpu::LabGraph> graph_{};
    std::optional<gpu::PhysicalTextureByteSizes> physicalByteSizes_{};
    std::optional<SchedulePlan> serialSchedule_{};
    std::vector<gpu::QueueExecutionTraceRecord> emptyExecutionTrace_{};
};

} // namespace ch10::pass_scheduling::starter
