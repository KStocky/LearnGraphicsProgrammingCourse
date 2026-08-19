#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/buffer.hpp>
#include <lgp/framework/descriptors.hpp>

#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ch10::pass_scheduling::solution
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
    [[nodiscard]] SchedulePlan const *AsyncSchedule() const noexcept;
    [[nodiscard]] gpu::PhysicalTextureByteSizes const *PhysicalByteSizes() const noexcept;
    [[nodiscard]] std::vector<gpu::QueueExecutionTraceRecord> const &LastExecutionTrace() const noexcept;
    [[nodiscard]] std::vector<gpu::ExecutedTextureBarrier> const &LastProducerBarrierTrace() const noexcept;
    [[nodiscard]] std::optional<gpu::GpuTimingSample> const &LastGpuTimingSample() const noexcept;
    [[nodiscard]] std::vector<gpu::FrameSlotUsageRecord> const &FrameSlotUsageHistory() const noexcept;

  private:
    struct FrameSlotResources final
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> computeAllocator{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> computeList{};
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> directAllocator{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> directList{};
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, gpu::kLabTextureCount> textures{};
        lgp::framework::Buffer computeTimestampReadback{};
        lgp::framework::Buffer directTimestampReadback{};
        std::uint64_t submittedFrameIndex{};
        gpu::GpuSubmissionCalibration submissionCalibration{};
        bool timingPending{};
    };

    [[nodiscard]] lgp::framework::Status CreateDeviceObjects();
    [[nodiscard]] lgp::framework::Status CreateQueueObjects();
    [[nodiscard]] lgp::framework::Status CreateFrameCommandObjects();
    [[nodiscard]] lgp::framework::Status CreateTimestampObjects();
    [[nodiscard]] lgp::framework::Status CreateSizeDependentResources(lgp::framework::Extent2D size);
    [[nodiscard]] lgp::framework::Status InitializeImGui();
    [[nodiscard]] lgp::framework::Status ValidateConcreteAsyncPlan() const;
    void CaptureSubmissionCalibration(std::uint64_t frameIndex, FrameSlotResources &slot) noexcept;
    [[nodiscard]] lgp::framework::Status ReadCompletedTimingSample(UINT frameSlot);
    [[nodiscard]] lgp::framework::Status RecordComputeBranch(lgp::framework::FrameContext const &frameContext,
                                                             FrameSlotResources &slot);
    [[nodiscard]] lgp::framework::Status RecordIndependentGraphicsBranch(
        lgp::framework::FrameContext const &frameContext, FrameSlotResources &slot);
    [[nodiscard]] lgp::framework::Status RecordComposite(lgp::framework::FrameContext const &frameContext,
                                                         FrameSlotResources &slot);
    void ReleaseSizeDependentResources() noexcept;
    void BuildDiagnosticsWindow() const;
    void DrawTo(ID3D12GraphicsCommandList7 &commandList, ID3D12PipelineState &pipeline,
                D3D12_CPU_DESCRIPTOR_HANDLE renderTarget) const;
    void Dispatch(ID3D12GraphicsCommandList7 &commandList, ID3D12PipelineState &pipeline,
                  lgp::framework::Extent2D size) const;
    void DrainComputeQueueForShutdown() noexcept;

    [[nodiscard]] UINT DescriptorIndex(UINT frameSlot, UINT withinSlot) const noexcept;
    [[nodiscard]] UINT RtvIndex(UINT frameSlot, UINT withinSlot) const noexcept;
    [[nodiscard]] UINT ComputeQueryIndex(UINT frameSlot, UINT withinSlot) const noexcept;
    [[nodiscard]] UINT DirectQueryIndex(UINT frameSlot, UINT withinSlot) const noexcept;

    lgp::framework::DeviceResources *deviceResources_{};
    gpu::LabShaders shaders_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_{};
    gpu::LabPipelines pipelines_{};
    lgp::framework::DescriptorAllocation textureDescriptors_{};
    lgp::framework::DescriptorAllocation textureRtvs_{};
    lgp::framework::DescriptorAllocation imguiFontDescriptor_{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> computeQueue_{};
    Microsoft::WRL::ComPtr<ID3D12Fence> computeFence_{};
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> computeTimestampHeap_{};
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> directTimestampHeap_{};
    std::vector<FrameSlotResources> frameSlots_{};
    std::optional<gpu::LabGraph> graph_{};
    std::optional<gpu::PhysicalTextureByteSizes> physicalByteSizes_{};
    std::optional<gpu::ScheduleComparison> schedules_{};
    std::vector<gpu::QueueExecutionTraceRecord> lastExecutionTrace_{};
    std::vector<gpu::ExecutedTextureBarrier> lastProducerBarrierTrace_{};
    std::optional<gpu::GpuTimingSample> lastGpuTimingSample_{};
    std::vector<gpu::FrameSlotUsageRecord> frameSlotUsageHistory_{};
    std::uint64_t nextComputeFenceValue_{1U};
    std::uint64_t lastSignaledComputeFenceValue_{};
    bool headless_{};
    bool imguiInitialized_{};
    bool imguiFrameBegun_{};
};

} // namespace ch10::pass_scheduling::solution
