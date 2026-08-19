#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include <lgp/framework/pix.hpp>

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace ch10::pass_scheduling::solution
{
namespace
{

inline constexpr UINT kDescriptorsPerSlot = 6U;
inline constexpr UINT kRtvsPerSlot = 2U;
inline constexpr UINT kComputeScratchSrv = 0U;
inline constexpr UINT kComputeFinalSrv = 1U;
inline constexpr UINT kGraphicsScratchSrv = 2U;
inline constexpr UINT kGraphicsFinalSrv = 3U;
inline constexpr UINT kComputeScratchUav = 4U;
inline constexpr UINT kComputeFinalUav = 5U;
inline constexpr UINT kComputeQueriesPerSlot = 2U;
inline constexpr UINT kDirectQueriesPerSlot = 4U;
inline constexpr std::uint64_t kComputeReadbackBytes = 2U * sizeof(std::uint64_t);
inline constexpr std::uint64_t kDirectReadbackBytes = 4U * sizeof(std::uint64_t);
inline constexpr std::size_t kMaxFrameSlotUsageHistory = 120U;

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "MultiQueueSchedulingLab.hlsl";
}

[[nodiscard]] std::size_t TextureIndex(gpu::LabTextureIndex index) noexcept
{
    return static_cast<std::size_t>(index);
}

[[nodiscard]] ScheduledPass const *FindPass(SchedulePlan const &plan, ch08::frame_graph::PassId passId) noexcept
{
    auto const pass = std::ranges::find(plan.passes, passId, &ScheduledPass::passId);
    return pass == plan.passes.end() ? nullptr : &*pass;
}

[[nodiscard]] double NormalizeTimestamp(std::uint64_t timestamp, std::uint64_t gpuCalibration,
                                        std::uint64_t cpuCalibration, std::uint64_t queueFrequency,
                                        std::uint64_t qpcFrequency) noexcept
{
    long double const cpuSeconds = static_cast<long double>(cpuCalibration) / static_cast<long double>(qpcFrequency);
    long double gpuDelta = 0.0L;
    if (timestamp >= gpuCalibration)
    {
        gpuDelta = static_cast<long double>(timestamp - gpuCalibration);
    }
    else
    {
        gpuDelta = -static_cast<long double>(gpuCalibration - timestamp);
    }
    return static_cast<double>(cpuSeconds + (gpuDelta / static_cast<long double>(queueFrequency)));
}

void DrawIntervalBar(double start, double end, double spanStart, double spanEnd)
{
    double const span = spanEnd - spanStart;
    if (span <= 0.0 || end <= start)
    {
        return;
    }
    float const availableWidth = (std::max)(ImGui::GetContentRegionAvail().x, 1.0F);
    float const startFraction = static_cast<float>((start - spanStart) / span);
    float const durationFraction = static_cast<float>((end - start) / span);
    float const barWidth = (std::max)(availableWidth * durationFraction, 1.0F);
    float const baseX = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(baseX + (availableWidth * startFraction));
    ImGui::ProgressBar(1.0F, {barWidth, 8.0F}, "");
}

[[nodiscard]] std::string_view TraceLabel(gpu::QueueExecutionTraceKind kind) noexcept
{
    switch (kind)
    {
    case gpu::QueueExecutionTraceKind::ComputeBranchRecorded:
        return "Compute branch recorded";
    case gpu::QueueExecutionTraceKind::IndependentGraphicsBranchRecorded:
        return "Independent graphics branch recorded";
    case gpu::QueueExecutionTraceKind::CompositeRecordedForFrameworkSubmission:
        return "Composite recorded for framework submission";
    case gpu::QueueExecutionTraceKind::ComputeExecute:
        return "Compute queue executes branch";
    case gpu::QueueExecutionTraceKind::ComputeSignal:
        return "Compute queue signals fence";
    case gpu::QueueExecutionTraceKind::IndependentDirectExecute:
        return "Direct queue executes independent branch";
    case gpu::QueueExecutionTraceKind::DirectWaitBeforeComposite:
        return "Direct queue waits before composite";
    }
    return "Unknown execution trace record";
}

[[nodiscard]] std::string_view IntervalLabel(gpu::GpuIntervalKind kind) noexcept
{
    switch (kind)
    {
    case gpu::GpuIntervalKind::ComputeBranch:
        return "Compute branch";
    case gpu::GpuIntervalKind::IndependentGraphicsBranch:
        return "Independent graphics branch";
    case gpu::GpuIntervalKind::Composite:
        return "Composite";
    }
    return "Unknown GPU interval";
}

} // namespace

UINT Renderer::DescriptorIndex(UINT frameSlot, UINT withinSlot) const noexcept
{
    return (frameSlot * kDescriptorsPerSlot) + withinSlot;
}

UINT Renderer::RtvIndex(UINT frameSlot, UINT withinSlot) const noexcept
{
    return (frameSlot * kRtvsPerSlot) + withinSlot;
}

UINT Renderer::ComputeQueryIndex(UINT frameSlot, UINT withinSlot) const noexcept
{
    return (frameSlot * kComputeQueriesPerSlot) + withinSlot;
}

UINT Renderer::DirectQueryIndex(UINT frameSlot, UINT withinSlot) const noexcept
{
    return (frameSlot * kDirectQueriesPerSlot) + withinSlot;
}

lgp::framework::Status Renderer::CreateQueueObjects()
{
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    HRESULT const queueResult = deviceResources_->device()->CreateCommandQueue(
        &queueDescription, IID_PPV_ARGS(computeQueue_.ReleaseAndGetAddressOf()));
    if (FAILED(queueResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandQueue", queueResult,
                                                                "Failed to create the Chapter 10 compute queue."));
    }
    HRESULT const fenceResult = deviceResources_->device()->CreateFence(
        0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(computeFence_.ReleaseAndGetAddressOf()));
    if (FAILED(fenceResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateFence", fenceResult,
                                                                "Failed to create the Chapter 10 compute fence."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateFrameCommandObjects()
{
    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlotResources &slot : frameSlots_)
    {
        HRESULT const computeAllocatorResult = deviceResources_->device()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(slot.computeAllocator.ReleaseAndGetAddressOf()));
        if (FAILED(computeAllocatorResult))
        {
            return std::unexpected(
                lgp::framework::MakeHResultError("ID3D12Device::CreateCommandAllocator", computeAllocatorResult,
                                                 "Failed to create a per-slot Chapter 10 compute allocator."));
        }
        HRESULT const computeListResult = deviceResources_->device()->CreateCommandList(
            0U, D3D12_COMMAND_LIST_TYPE_COMPUTE, slot.computeAllocator.Get(), nullptr,
            IID_PPV_ARGS(slot.computeList.ReleaseAndGetAddressOf()));
        if (FAILED(computeListResult))
        {
            return std::unexpected(
                lgp::framework::MakeHResultError("ID3D12Device::CreateCommandList", computeListResult,
                                                 "Failed to create a per-slot Chapter 10 compute list."));
        }
        if (FAILED(slot.computeList->Close()))
        {
            return std::unexpected(lgp::framework::MakeError("ID3D12GraphicsCommandList::Close",
                                                             "Failed to close the initial Chapter 10 compute list."));
        }

        HRESULT const directAllocatorResult = deviceResources_->device()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.directAllocator.ReleaseAndGetAddressOf()));
        if (FAILED(directAllocatorResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError(
                "ID3D12Device::CreateCommandAllocator", directAllocatorResult,
                "Failed to create a per-slot Chapter 10 independent direct allocator."));
        }
        HRESULT const directListResult = deviceResources_->device()->CreateCommandList(
            0U, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.directAllocator.Get(), nullptr,
            IID_PPV_ARGS(slot.directList.ReleaseAndGetAddressOf()));
        if (FAILED(directListResult))
        {
            return std::unexpected(
                lgp::framework::MakeHResultError("ID3D12Device::CreateCommandList", directListResult,
                                                 "Failed to create a per-slot Chapter 10 independent direct list."));
        }
        if (FAILED(slot.directList->Close()))
        {
            return std::unexpected(lgp::framework::MakeError(
                "ID3D12GraphicsCommandList::Close", "Failed to close the initial Chapter 10 independent direct list."));
        }
    }
    return {};
}

lgp::framework::Status Renderer::CreateTimestampObjects()
{
    UINT const frameSlotCount = deviceResources_->back_buffer_count();
    D3D12_QUERY_HEAP_DESC computeHeapDescription{};
    computeHeapDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    computeHeapDescription.Count = frameSlotCount * kComputeQueriesPerSlot;
    HRESULT const computeHeapResult = deviceResources_->device()->CreateQueryHeap(
        &computeHeapDescription, IID_PPV_ARGS(computeTimestampHeap_.ReleaseAndGetAddressOf()));
    if (FAILED(computeHeapResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateQueryHeap", computeHeapResult,
                                             "Failed to create the Chapter 10 compute timestamp heap."));
    }
    D3D12_QUERY_HEAP_DESC directHeapDescription{};
    directHeapDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    directHeapDescription.Count = frameSlotCount * kDirectQueriesPerSlot;
    HRESULT const directHeapResult = deviceResources_->device()->CreateQueryHeap(
        &directHeapDescription, IID_PPV_ARGS(directTimestampHeap_.ReleaseAndGetAddressOf()));
    if (FAILED(directHeapResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateQueryHeap", directHeapResult,
                                             "Failed to create the Chapter 10 direct timestamp heap."));
    }

    for (UINT frameSlot = 0U; frameSlot < frameSlotCount; ++frameSlot)
    {
        auto computeReadback = lgp::framework::CreateCommittedBuffer(
            *deviceResources_->device(),
            {kComputeReadbackBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
             L"Ch10 Compute Timestamp Readback"});
        if (!computeReadback)
        {
            return std::unexpected(std::move(computeReadback.error()));
        }
        frameSlots_[frameSlot].computeTimestampReadback = std::move(*computeReadback);

        auto directReadback = lgp::framework::CreateCommittedBuffer(
            *deviceResources_->device(),
            {kDirectReadbackBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
             L"Ch10 Direct Timestamp Readback"});
        if (!directReadback)
        {
            return std::unexpected(std::move(directReadback.error()));
        }
        frameSlots_[frameSlot].directTimestampReadback = std::move(*directReadback);
    }

    return {};
}

lgp::framework::Status Renderer::CreateDeviceObjects()
{
    if (auto status = gpu::CreateLabShaders(ShaderPath(), shaders_); !status)
    {
        return status;
    }
    if (auto status = gpu::CreateLabRootSignature(*deviceResources_->device(), rootSignature_); !status)
    {
        return status;
    }
    if (auto status = gpu::CreateLabPipelines(*deviceResources_->device(), deviceResources_->back_buffer_format(),
                                              *rootSignature_.Get(), shaders_, pipelines_);
        !status)
    {
        return status;
    }
    if (auto status = CreateQueueObjects(); !status)
    {
        return status;
    }
    if (auto status = CreateFrameCommandObjects(); !status)
    {
        return status;
    }
    return CreateTimestampObjects();
}

void Renderer::ReleaseSizeDependentResources() noexcept
{
    for (FrameSlotResources &slot : frameSlots_)
    {
        for (auto &texture : slot.textures)
        {
            texture.Reset();
        }
        slot.submissionCalibration = {};
        slot.timingPending = false;
    }
    graph_.reset();
    physicalByteSizes_.reset();
    schedules_.reset();
    lastExecutionTrace_.clear();
    lastProducerBarrierTrace_.clear();
    lastGpuTimingSample_.reset();
}

lgp::framework::Status Renderer::CreateSizeDependentResources(lgp::framework::Extent2D size)
{
    ReleaseSizeDependentResources();
    if (size.empty())
    {
        return {};
    }

    UINT const frameSlotCount = deviceResources_->back_buffer_count();
    if (!textureDescriptors_)
    {
        auto descriptors =
            deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(frameSlotCount * kDescriptorsPerSlot);
        if (!descriptors)
        {
            return std::unexpected(std::move(descriptors.error()));
        }
        textureDescriptors_ = *descriptors;
    }
    if (!textureRtvs_)
    {
        auto rtvs = deviceResources_->rtv_heap().Allocate(frameSlotCount * kRtvsPerSlot);
        if (!rtvs)
        {
            return std::unexpected(std::move(rtvs.error()));
        }
        textureRtvs_ = *rtvs;
    }

    auto graphResult = gpu::CompileLabGraph(headless_);
    if (!graphResult)
    {
        return std::unexpected(std::move(graphResult.error()));
    }
    auto sizesResult = gpu::QueryPhysicalTextureByteSizes(*deviceResources_->device(), size);
    if (!sizesResult)
    {
        return std::unexpected(std::move(sizesResult.error()));
    }
    auto schedulesResult = gpu::CompileScheduleComparison(*graphResult, *sizesResult);
    if (!schedulesResult)
    {
        return std::unexpected(std::move(schedulesResult.error()));
    }

    D3D12_RESOURCE_DESC1 const computeDescription = gpu::ComputeTextureDescription(size);
    D3D12_RESOURCE_DESC1 const graphicsDescription = gpu::GraphicsTextureDescription(size);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = gpu::kTransientFormat;
    clearValue.Color[3] = 1.0F;
    for (UINT frameSlot = 0U; frameSlot < frameSlotCount; ++frameSlot)
    {
        FrameSlotResources &slot = frameSlots_[frameSlot];
        for (std::size_t textureIndex = 0U; textureIndex < gpu::kLabTextureCount; ++textureIndex)
        {
            bool const computeTexture = textureIndex <= TextureIndex(gpu::LabTextureIndex::ComputeFinal);
            D3D12_RESOURCE_DESC1 const &description = computeTexture ? computeDescription : graphicsDescription;
            D3D12_CLEAR_VALUE const *optimizedClearValue = computeTexture ? nullptr : &clearValue;
            HRESULT const result = deviceResources_->device()->CreateCommittedResource3(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_UNDEFINED, optimizedClearValue, nullptr,
                0U, nullptr, IID_PPV_ARGS(slot.textures[textureIndex].ReleaseAndGetAddressOf()));
            if (FAILED(result))
            {
                return std::unexpected(
                    lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                     "Failed to create a per-frame Chapter 10 Solution texture."));
            }
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = gpu::kTransientFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1U;
        std::array<std::pair<gpu::LabTextureIndex, UINT>, 4U> const srvs{{
            {gpu::LabTextureIndex::ComputeScratch, kComputeScratchSrv},
            {gpu::LabTextureIndex::ComputeFinal, kComputeFinalSrv},
            {gpu::LabTextureIndex::GraphicsScratch, kGraphicsScratchSrv},
            {gpu::LabTextureIndex::GraphicsFinal, kGraphicsFinalSrv},
        }};
        for (auto const &[texture, descriptor] : srvs)
        {
            deviceResources_->device()->CreateShaderResourceView(
                slot.textures[TextureIndex(texture)].Get(), &srv,
                textureDescriptors_.CpuHandle(DescriptorIndex(frameSlot, descriptor)));
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = gpu::kTransientFormat;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateUnorderedAccessView(
            slot.textures[TextureIndex(gpu::LabTextureIndex::ComputeScratch)].Get(), nullptr, &uav,
            textureDescriptors_.CpuHandle(DescriptorIndex(frameSlot, kComputeScratchUav)));
        deviceResources_->device()->CreateUnorderedAccessView(
            slot.textures[TextureIndex(gpu::LabTextureIndex::ComputeFinal)].Get(), nullptr, &uav,
            textureDescriptors_.CpuHandle(DescriptorIndex(frameSlot, kComputeFinalUav)));

        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = gpu::kTransientFormat;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateRenderTargetView(
            slot.textures[TextureIndex(gpu::LabTextureIndex::GraphicsScratch)].Get(), &rtv,
            textureRtvs_.CpuHandle(RtvIndex(frameSlot, 0U)));
        deviceResources_->device()->CreateRenderTargetView(
            slot.textures[TextureIndex(gpu::LabTextureIndex::GraphicsFinal)].Get(), &rtv,
            textureRtvs_.CpuHandle(RtvIndex(frameSlot, 1U)));
    }

    graph_ = std::move(*graphResult);
    physicalByteSizes_ = *sizesResult;
    schedules_ = std::move(*schedulesResult);
    return ValidateConcreteAsyncPlan();
}

lgp::framework::Status Renderer::InitializeImGui()
{
    auto descriptor = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(1U);
    if (!descriptor)
    {
        return std::unexpected(std::move(descriptor.error()));
    }
    imguiFontDescriptor_ = *descriptor;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().BackendPlatformName = "LGP.ManualInput";
    ImGui_ImplDX12_InitInfo info{};
    info.Device = deviceResources_->device();
    info.CommandQueue = deviceResources_->graphics_queue();
    info.NumFramesInFlight = static_cast<int>(deviceResources_->back_buffer_count());
    info.RTVFormat = deviceResources_->back_buffer_format();
    info.SrvDescriptorHeap = deviceResources_->shader_visible_cbv_srv_uav_heap().Get();
    info.LegacySingleSrvCpuDescriptor = imguiFontDescriptor_.cpuHandle;
    info.LegacySingleSrvGpuDescriptor = imguiFontDescriptor_.gpuHandle;
    if (!ImGui_ImplDX12_Init(&info))
    {
        return std::unexpected(
            lgp::framework::MakeError("ImGui_ImplDX12_Init", "Failed to initialize Chapter 10 diagnostics."));
    }
    imguiInitialized_ = true;
    return {};
}

lgp::framework::Status Renderer::ValidateConcreteAsyncPlan() const
{
    if (!graph_ || !schedules_)
    {
        return std::unexpected(
            lgp::framework::MakeError("Renderer::ValidateConcreteAsyncPlan", "The async plan is unavailable."));
    }
    SchedulePlan const &plan = schedules_->async;
    std::array<std::tuple<ch08::frame_graph::PassId, QueueKind, std::uint32_t>, 5U> const expected{{
        {graph_->computeGenerate, QueueKind::Compute, 0U},
        {graph_->computeCollapse, QueueKind::Compute, 1U},
        {graph_->graphicsGeometry, QueueKind::Direct, 0U},
        {graph_->graphicsResolve, QueueKind::Direct, 1U},
        {graph_->composite, QueueKind::Direct, 2U},
    }};
    for (auto const &[passId, queue, queuePosition] : expected)
    {
        ScheduledPass const *pass = FindPass(plan, passId);
        if (pass == nullptr || pass->queue != queue || pass->queuePosition != queuePosition)
        {
            return std::unexpected(lgp::framework::MakeError("Renderer::ValidateConcreteAsyncPlan",
                                                             "A concrete pass queue or queue position does not match "
                                                             "the runtime recording order."));
        }
    }
    if (plan.crossQueueFenceDependencies.size() != 1U)
    {
        return std::unexpected(lgp::framework::MakeError("Renderer::ValidateConcreteAsyncPlan",
                                                         "The async plan must have exactly one fence dependency."));
    }
    CrossQueueFenceDependency const &fence = plan.crossQueueFenceDependencies.front();
    bool const reasonMatches = fence.reasons.size() == 1U && fence.reasons.front().resourceId == graph_->computeFinal;
    if (fence.producerPassId != graph_->computeCollapse || fence.consumerPassId != graph_->composite ||
        fence.producerQueue != QueueKind::Compute || fence.consumerQueue != QueueKind::Direct ||
        fence.signalAfterProducerQueuePosition != 1U || fence.waitBeforeConsumerQueuePosition != 2U || !reasonMatches)
    {
        return std::unexpected(lgp::framework::MakeError(
            "Renderer::ValidateConcreteAsyncPlan",
            "The concrete compute-collapse to composite fence dependency does not match the runtime path."));
    }
    return {};
}

void Renderer::CaptureSubmissionCalibration(std::uint64_t frameIndex, FrameSlotResources &slot) noexcept
{
    gpu::GpuSubmissionCalibration calibration{};
    calibration.frameIndex = frameIndex;
    LARGE_INTEGER qpcFrequency{};
    if (QueryPerformanceFrequency(&qpcFrequency) != FALSE)
    {
        calibration.qpcFrequency = static_cast<std::uint64_t>(qpcFrequency.QuadPart);
    }

    HRESULT const computeFrequencyResult = computeQueue_->GetTimestampFrequency(&calibration.compute.frequency);
    HRESULT const computeClockResult =
        computeQueue_->GetClockCalibration(&calibration.compute.gpuTimestamp, &calibration.compute.cpuQpc);
    calibration.compute.valid = SUCCEEDED(computeFrequencyResult) && SUCCEEDED(computeClockResult) &&
                                calibration.compute.frequency != 0U && calibration.qpcFrequency != 0U;

    HRESULT const directFrequencyResult =
        deviceResources_->graphics_queue()->GetTimestampFrequency(&calibration.direct.frequency);
    HRESULT const directClockResult = deviceResources_->graphics_queue()->GetClockCalibration(
        &calibration.direct.gpuTimestamp, &calibration.direct.cpuQpc);
    calibration.direct.valid = SUCCEEDED(directFrequencyResult) && SUCCEEDED(directClockResult) &&
                               calibration.direct.frequency != 0U && calibration.qpcFrequency != 0U;
    calibration.sharedCalibrationValid = calibration.compute.valid && calibration.direct.valid;
    slot.submissionCalibration = calibration;
}

void Renderer::DrawTo(ID3D12GraphicsCommandList7 &commandList, ID3D12PipelineState &pipeline,
                      D3D12_CPU_DESCRIPTOR_HANDLE renderTarget) const
{
    commandList.OMSetRenderTargets(1U, &renderTarget, FALSE, nullptr);
    commandList.SetPipelineState(&pipeline);
    commandList.DrawInstanced(3U, 1U, 0U, 0U);
}

void Renderer::Dispatch(ID3D12GraphicsCommandList7 &commandList, ID3D12PipelineState &pipeline,
                        lgp::framework::Extent2D size) const
{
    commandList.SetPipelineState(&pipeline);
    commandList.Dispatch((size.width + 7U) / 8U, (size.height + 7U) / 8U, 1U);
}

lgp::framework::Status Renderer::RecordComputeBranch(lgp::framework::FrameContext const &frameContext,
                                                     FrameSlotResources &slot)
{
    HRESULT const allocatorReset = slot.computeAllocator->Reset();
    if (FAILED(allocatorReset))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12CommandAllocator::Reset", allocatorReset,
                                                                "Failed to reset a Chapter 10 compute allocator."));
    }
    HRESULT const listReset = slot.computeList->Reset(slot.computeAllocator.Get(), nullptr);
    if (FAILED(listReset))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12GraphicsCommandList::Reset", listReset,
                                                                "Failed to reset a Chapter 10 compute list."));
    }
    ID3D12GraphicsCommandList7 &commandList = *slot.computeList.Get();
    {
        lgp::framework::PixEventScope pixEvent{commandList, lgp::framework::PixColor(80U, 220U, 220U),
                                               L"Ch10 Compute Branch"};
        ID3D12DescriptorHeap *const descriptorHeaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
        commandList.SetDescriptorHeaps(1U, descriptorHeaps);
        commandList.SetComputeRootSignature(rootSignature_.Get());
        UINT const queryStart = ComputeQueryIndex(frameContext.frameSlot, 0U);
        commandList.EndQuery(computeTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryStart);

        ID3D12Resource &computeScratch = *slot.textures[TextureIndex(gpu::LabTextureIndex::ComputeScratch)].Get();
        ID3D12Resource &computeFinal = *slot.textures[TextureIndex(gpu::LabTextureIndex::ComputeFinal)].Get();
        std::vector<D3D12_TEXTURE_BARRIER> barriers{
            gpu::MakeTextureBarrier(computeScratch, gpu::UndefinedState(), gpu::ComputeUnorderedAccessState(),
                                    D3D12_TEXTURE_BARRIER_FLAG_DISCARD),
        };
        gpu::SubmitTextureBarriers(commandList, barriers);
        commandList.SetComputeRootDescriptorTable(
            2U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, kComputeScratchUav)));
        Dispatch(commandList, *pipelines_.computeGenerate.Get(), frameContext.drawableSize);

        barriers = {
            gpu::MakeTextureBarrier(computeScratch, gpu::ComputeUnorderedAccessState(),
                                    gpu::ComputeShaderResourceState()),
            gpu::MakeTextureBarrier(computeFinal, gpu::UndefinedState(), gpu::ComputeUnorderedAccessState(),
                                    D3D12_TEXTURE_BARRIER_FLAG_DISCARD),
        };
        gpu::SubmitTextureBarriers(commandList, barriers);
        commandList.SetComputeRootDescriptorTable(
            0U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, kComputeScratchSrv)));
        commandList.SetComputeRootDescriptorTable(
            2U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, kComputeFinalUav)));
        Dispatch(commandList, *pipelines_.computeCollapse.Get(), frameContext.drawableSize);

        lgp::framework::TextureBarrierState const producedState = gpu::ComputeProducedSharedShaderResourceState();
        barriers = {
            gpu::MakeTextureBarrier(computeScratch, gpu::ComputeShaderResourceState(), gpu::UndefinedState()),
            gpu::MakeTextureBarrier(computeFinal, gpu::ComputeUnorderedAccessState(), producedState),
        };
        gpu::SubmitTextureBarriers(commandList, barriers);
        lastProducerBarrierTrace_ = {
            {graph_->computeScratch, gpu::ComputeShaderResourceState(), gpu::UndefinedState(),
             D3D12_TEXTURE_BARRIER_FLAG_NONE},
            {graph_->computeFinal, gpu::ComputeUnorderedAccessState(), producedState, D3D12_TEXTURE_BARRIER_FLAG_NONE},
        };

        commandList.EndQuery(computeTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryStart + 1U);
        commandList.ResolveQueryData(computeTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryStart, 2U,
                                     slot.computeTimestampReadback.resource(), 0U);
    }
    HRESULT const closeResult = commandList.Close();
    if (FAILED(closeResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12GraphicsCommandList::Close", closeResult,
                                                                "Failed to close the Chapter 10 compute list."));
    }
    lastExecutionTrace_.push_back(
        {gpu::QueueExecutionTraceKind::ComputeBranchRecorded, QueueKind::Compute, std::nullopt});
    return {};
}

lgp::framework::Status Renderer::RecordIndependentGraphicsBranch(lgp::framework::FrameContext const &frameContext,
                                                                 FrameSlotResources &slot)
{
    HRESULT const allocatorReset = slot.directAllocator->Reset();
    if (FAILED(allocatorReset))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12CommandAllocator::Reset", allocatorReset,
                                             "Failed to reset a Chapter 10 independent direct allocator."));
    }
    HRESULT const listReset = slot.directList->Reset(slot.directAllocator.Get(), nullptr);
    if (FAILED(listReset))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12GraphicsCommandList::Reset", listReset, "Failed to reset a Chapter 10 independent direct list."));
    }
    ID3D12GraphicsCommandList7 &commandList = *slot.directList.Get();
    {
        lgp::framework::PixEventScope pixEvent{commandList, lgp::framework::PixColor(255U, 180U, 80U),
                                               L"Ch10 Independent Graphics"};
        ID3D12DescriptorHeap *const descriptorHeaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
        commandList.SetDescriptorHeaps(1U, descriptorHeaps);
        commandList.SetGraphicsRootSignature(rootSignature_.Get());
        commandList.RSSetViewports(1U, &frameContext.viewport);
        commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
        commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT const queryStart = DirectQueryIndex(frameContext.frameSlot, 0U);
        commandList.EndQuery(directTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryStart);

        ID3D12Resource &graphicsScratch = *slot.textures[TextureIndex(gpu::LabTextureIndex::GraphicsScratch)].Get();
        ID3D12Resource &graphicsFinal = *slot.textures[TextureIndex(gpu::LabTextureIndex::GraphicsFinal)].Get();
        std::vector<D3D12_TEXTURE_BARRIER> barriers{
            gpu::MakeTextureBarrier(graphicsScratch, gpu::UndefinedState(), gpu::RenderTargetState(),
                                    D3D12_TEXTURE_BARRIER_FLAG_DISCARD),
        };
        gpu::SubmitTextureBarriers(commandList, barriers);
        DrawTo(commandList, *pipelines_.graphicsGeometry.Get(),
               textureRtvs_.CpuHandle(RtvIndex(frameContext.frameSlot, 0U)));

        barriers = {
            gpu::MakeTextureBarrier(graphicsScratch, gpu::RenderTargetState(), gpu::PixelShaderResourceState()),
            gpu::MakeTextureBarrier(graphicsFinal, gpu::UndefinedState(), gpu::RenderTargetState(),
                                    D3D12_TEXTURE_BARRIER_FLAG_DISCARD),
        };
        gpu::SubmitTextureBarriers(commandList, barriers);
        commandList.SetGraphicsRootDescriptorTable(
            0U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, kGraphicsScratchSrv)));
        DrawTo(commandList, *pipelines_.graphicsResolve.Get(),
               textureRtvs_.CpuHandle(RtvIndex(frameContext.frameSlot, 1U)));

        barriers = {
            gpu::MakeTextureBarrier(graphicsScratch, gpu::PixelShaderResourceState(), gpu::UndefinedState()),
            gpu::MakeTextureBarrier(graphicsFinal, gpu::RenderTargetState(), gpu::PixelShaderResourceState()),
        };
        gpu::SubmitTextureBarriers(commandList, barriers);
        commandList.EndQuery(directTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryStart + 1U);
        commandList.ResolveQueryData(directTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryStart, 2U,
                                     slot.directTimestampReadback.resource(), 0U);
    }
    HRESULT const closeResult = commandList.Close();
    if (FAILED(closeResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12GraphicsCommandList::Close", closeResult,
                                             "Failed to close the Chapter 10 independent direct list."));
    }
    lastExecutionTrace_.push_back(
        {gpu::QueueExecutionTraceKind::IndependentGraphicsBranchRecorded, QueueKind::Direct, std::nullopt});
    return {};
}

lgp::framework::Status Renderer::RecordComposite(lgp::framework::FrameContext const &frameContext,
                                                 FrameSlotResources &slot)
{
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    lgp::framework::PixEventScope pixEvent{commandList, lgp::framework::PixColor(180U, 100U, 255U), L"Ch10 Composite"};
    ID3D12DescriptorHeap *const descriptorHeaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, descriptorHeaps);
    commandList.SetGraphicsRootSignature(rootSignature_.Get());
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT const queryStart = DirectQueryIndex(frameContext.frameSlot, 2U);
    commandList.EndQuery(directTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryStart);

    std::vector<D3D12_TEXTURE_BARRIER> barriers{
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::FrameStartState(frameContext.headless),
                                gpu::RenderTargetState()),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);
    commandList.SetGraphicsRootDescriptorTable(
        0U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, kComputeFinalSrv)));
    commandList.SetGraphicsRootDescriptorTable(
        1U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, kGraphicsFinalSrv)));
    DrawTo(commandList, *pipelines_.composite.Get(), frameContext.renderTargetView);

    if (imguiFrameBegun_)
    {
        commandList.SetDescriptorHeaps(1U, descriptorHeaps);
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), &commandList);
        imguiFrameBegun_ = false;
    }

    ID3D12Resource &computeFinal = *slot.textures[TextureIndex(gpu::LabTextureIndex::ComputeFinal)].Get();
    ID3D12Resource &graphicsFinal = *slot.textures[TextureIndex(gpu::LabTextureIndex::GraphicsFinal)].Get();
    barriers = {
        gpu::MakeTextureBarrier(computeFinal, gpu::PixelShaderResourceState(), gpu::UndefinedState()),
        gpu::MakeTextureBarrier(graphicsFinal, gpu::PixelShaderResourceState(), gpu::UndefinedState()),
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(),
                                gpu::FrameEndState(frameContext.headless)),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);
    commandList.EndQuery(directTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryStart + 1U);
    commandList.ResolveQueryData(directTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryStart, 2U,
                                 slot.directTimestampReadback.resource(), 2U * sizeof(std::uint64_t));
    lastExecutionTrace_.push_back(
        {gpu::QueueExecutionTraceKind::CompositeRecordedForFrameworkSubmission, QueueKind::Direct, std::nullopt});
    return {};
}

lgp::framework::Status Renderer::ReadCompletedTimingSample(UINT frameSlot)
{
    FrameSlotResources &slot = frameSlots_[frameSlot];
    if (!slot.timingPending)
    {
        return {};
    }
    std::array<std::uint64_t, 2U> computeTimestamps{};
    std::array<std::uint64_t, 4U> directTimestamps{};
    D3D12_RANGE const computeReadRange{0U, static_cast<SIZE_T>(kComputeReadbackBytes)};
    void *computeData = nullptr;
    HRESULT const computeMapResult = slot.computeTimestampReadback.resource()->Map(0U, &computeReadRange, &computeData);
    if (FAILED(computeMapResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", computeMapResult,
                                                                "Failed to map Chapter 10 compute timestamps."));
    }
    std::memcpy(computeTimestamps.data(), computeData, kComputeReadbackBytes);
    D3D12_RANGE const noWrites{0U, 0U};
    slot.computeTimestampReadback.resource()->Unmap(0U, &noWrites);

    D3D12_RANGE const directReadRange{0U, static_cast<SIZE_T>(kDirectReadbackBytes)};
    void *directData = nullptr;
    HRESULT const directMapResult = slot.directTimestampReadback.resource()->Map(0U, &directReadRange, &directData);
    if (FAILED(directMapResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", directMapResult,
                                                                "Failed to map Chapter 10 direct timestamps."));
    }
    std::memcpy(directTimestamps.data(), directData, kDirectReadbackBytes);
    slot.directTimestampReadback.resource()->Unmap(0U, &noWrites);

    gpu::GpuTimingSample sample{};
    sample.frameIndex = slot.submittedFrameIndex;
    sample.submissionCalibration = slot.submissionCalibration;
    sample.sharedCalibrationValid = slot.submissionCalibration.sharedCalibrationValid;
    sample.intervals = {
        gpu::GpuTimestampInterval{gpu::GpuIntervalKind::ComputeBranch, QueueKind::Compute, computeTimestamps[0],
                                  computeTimestamps[1]},
        gpu::GpuTimestampInterval{gpu::GpuIntervalKind::IndependentGraphicsBranch, QueueKind::Direct,
                                  directTimestamps[0], directTimestamps[1]},
        gpu::GpuTimestampInterval{gpu::GpuIntervalKind::Composite, QueueKind::Direct, directTimestamps[2],
                                  directTimestamps[3]},
    };
    for (gpu::GpuTimestampInterval &interval : sample.intervals)
    {
        gpu::GpuQueueCalibration const &calibration = interval.queue == QueueKind::Compute
                                                          ? sample.submissionCalibration.compute
                                                          : sample.submissionCalibration.direct;
        if (calibration.frequency != 0U && interval.endTimestamp >= interval.startTimestamp)
        {
            interval.durationSeconds = static_cast<double>(interval.endTimestamp - interval.startTimestamp) /
                                       static_cast<double>(calibration.frequency);
        }
        if (sample.sharedCalibrationValid)
        {
            interval.normalizedStartSeconds =
                NormalizeTimestamp(interval.startTimestamp, calibration.gpuTimestamp, calibration.cpuQpc,
                                   calibration.frequency, sample.submissionCalibration.qpcFrequency);
            interval.normalizedEndSeconds =
                NormalizeTimestamp(interval.endTimestamp, calibration.gpuTimestamp, calibration.cpuQpc,
                                   calibration.frequency, sample.submissionCalibration.qpcFrequency);
        }
    }
    lastGpuTimingSample_ = sample;
    slot.timingPending = false;
    return {};
}

void Renderer::BuildDiagnosticsWindow() const
{
    if (!schedules_)
    {
        return;
    }
    ImGui::Begin("Pass schedule comparison");
    ImGui::Text("Abstract makespan: serial %llu, async %llu",
                static_cast<unsigned long long>(schedules_->serial.abstractMakespanTicks),
                static_cast<unsigned long long>(schedules_->async.abstractMakespanTicks));
    if (schedules_->serial.peakTransientBytes && schedules_->async.peakTransientBytes)
    {
        ImGui::Text("Complete peak bytes: serial %llu, async %llu",
                    static_cast<unsigned long long>(*schedules_->serial.peakTransientBytes),
                    static_cast<unsigned long long>(*schedules_->async.peakTransientBytes));
    }
    for (ScheduledPass const &pass : schedules_->async.passes)
    {
        ImGui::Text("%s %s [%llu, %llu)", pass.queue == QueueKind::Compute ? "Compute" : "Direct",
                    pass.educationalName.c_str(), static_cast<unsigned long long>(pass.abstractStartTick),
                    static_cast<unsigned long long>(pass.abstractEndTick));
        DrawIntervalBar(static_cast<double>(pass.abstractStartTick), static_cast<double>(pass.abstractEndTick), 0.0,
                        static_cast<double>(schedules_->async.abstractMakespanTicks));
    }
    if (!schedules_->async.crossQueueFenceDependencies.empty())
    {
        CrossQueueFenceDependency const &dependency = schedules_->async.crossQueueFenceDependencies.front();
        ImGui::Text("Fence: pass %u signals; pass %u waits", dependency.producerPassId.value,
                    dependency.consumerPassId.value);
    }
    ImGui::Separator();
    for (gpu::QueueExecutionTraceRecord const &record : lastExecutionTrace_)
    {
        std::string_view const label = TraceLabel(record.kind);
        ImGui::Text("%.*s | %s queue | fence %llu", static_cast<int>(label.size()), label.data(),
                    record.queue == QueueKind::Compute ? "Compute" : "Direct",
                    static_cast<unsigned long long>(record.fenceValue.value_or(0U)));
    }
    if (lastGpuTimingSample_)
    {
        ImGui::Separator();
        ImGui::Text("GPU frame %llu; shared calibration %s",
                    static_cast<unsigned long long>(lastGpuTimingSample_->frameIndex),
                    lastGpuTimingSample_->sharedCalibrationValid ? "valid" : "unavailable");
        double sharedStart = 0.0;
        double sharedEnd = 0.0;
        if (lastGpuTimingSample_->sharedCalibrationValid)
        {
            sharedStart = *lastGpuTimingSample_->intervals.front().normalizedStartSeconds;
            sharedEnd = *lastGpuTimingSample_->intervals.front().normalizedEndSeconds;
            for (gpu::GpuTimestampInterval const &interval : lastGpuTimingSample_->intervals)
            {
                sharedStart = (std::min)(sharedStart, *interval.normalizedStartSeconds);
                sharedEnd = (std::max)(sharedEnd, *interval.normalizedEndSeconds);
            }
        }
        for (gpu::GpuTimestampInterval const &interval : lastGpuTimingSample_->intervals)
        {
            std::string_view const label = IntervalLabel(interval.kind);
            ImGui::Text("%.*s: %.3f ms", static_cast<int>(label.size()), label.data(),
                        interval.durationSeconds.value_or(0.0) * 1'000.0);
            if (interval.normalizedStartSeconds && interval.normalizedEndSeconds)
            {
                ImGui::Text("  calibrated [%.6f, %.6f] s", *interval.normalizedStartSeconds,
                            *interval.normalizedEndSeconds);
                DrawIntervalBar(*interval.normalizedStartSeconds, *interval.normalizedEndSeconds, sharedStart,
                                sharedEnd);
            }
        }
        if (!lastGpuTimingSample_->sharedCalibrationValid)
        {
            ImGui::TextUnformatted("Durations remain useful; the submission graph alone proves overlap opportunity.");
        }
    }
    ImGui::End();
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    if (auto status = CreateDeviceObjects(); !status)
    {
        return status;
    }
    if (auto status = CreateSizeDependentResources(context.drawableSize); !status)
    {
        return status;
    }
    return headless_ ? lgp::framework::Status{} : InitializeImGui();
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D drawableSize)
{
    return CreateSizeDependentResources(drawableSize);
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &context)
{
    if (headless_ || !imguiInitialized_)
    {
        return {};
    }
    if (imguiFrameBegun_)
    {
        ImGui::EndFrame();
        imguiFrameBegun_ = false;
    }
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(context.drawableSize.width), static_cast<float>(context.drawableSize.height)};
    io.DeltaTime = static_cast<float>((std::max)(context.deltaSeconds, 1.0 / 240.0));
    io.AddMousePosEvent(static_cast<float>(context.input.mouse.x), static_cast<float>(context.input.mouse.y));
    io.AddMouseButtonEvent(0, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Left));
    io.AddMouseButtonEvent(1, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Right));
    io.AddMouseWheelEvent(0.0F, context.input.mouse.wheelDelta);
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    imguiFrameBegun_ = true;
    BuildDiagnosticsWindow();
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr)
    {
        return std::unexpected(lgp::framework::MakeError("Solution::Renderer::Render",
                                                         "The frame context does not provide a direct command list."));
    }
    if (frameContext.renderTarget == nullptr)
    {
        return std::unexpected(lgp::framework::MakeError("Solution::Renderer::Render",
                                                         "The frame context does not provide a render target."));
    }
    if (!graph_ || !schedules_ || !physicalByteSizes_)
    {
        return std::unexpected(lgp::framework::MakeError(
            "Solution::Renderer::Render",
            "Size-dependent graph, schedule, and texture resources are unavailable; resize to a non-empty extent "
            "before rendering."));
    }
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Solution::Renderer::Render",
                                                         "The frame context references an out-of-range frame slot."));
    }
    if (auto status = ValidateConcreteAsyncPlan(); !status)
    {
        return status;
    }
    if (auto status = ReadCompletedTimingSample(frameContext.frameSlot); !status)
    {
        return status;
    }

    lastExecutionTrace_.clear();
    lastProducerBarrierTrace_.clear();
    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    if (auto status = RecordComputeBranch(frameContext, slot); !status)
    {
        return status;
    }
    if (auto status = RecordIndependentGraphicsBranch(frameContext, slot); !status)
    {
        return status;
    }
    if (auto status = RecordComposite(frameContext, slot); !status)
    {
        return status;
    }

    CaptureSubmissionCalibration(frameContext.frameIndex, slot);
    ID3D12CommandList *const computeLists[]{slot.computeList.Get()};
    computeQueue_->ExecuteCommandLists(1U, computeLists);
    lastExecutionTrace_.push_back({gpu::QueueExecutionTraceKind::ComputeExecute, QueueKind::Compute, std::nullopt});

    std::uint64_t const fenceValue = nextComputeFenceValue_++;
    HRESULT const signalResult = computeQueue_->Signal(computeFence_.Get(), fenceValue);
    if (FAILED(signalResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12CommandQueue::Signal", signalResult,
                                                                "Failed to signal the Chapter 10 compute fence."));
    }
    lastSignaledComputeFenceValue_ = fenceValue;
    lastExecutionTrace_.push_back({gpu::QueueExecutionTraceKind::ComputeSignal, QueueKind::Compute, fenceValue});

    ID3D12CommandList *const directLists[]{slot.directList.Get()};
    deviceResources_->graphics_queue()->ExecuteCommandLists(1U, directLists);
    lastExecutionTrace_.push_back(
        {gpu::QueueExecutionTraceKind::IndependentDirectExecute, QueueKind::Direct, std::nullopt});

    HRESULT const waitResult = deviceResources_->graphics_queue()->Wait(computeFence_.Get(), fenceValue);
    if (FAILED(waitResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12CommandQueue::Wait", waitResult, "Failed to enqueue the Chapter 10 direct-queue dependency wait."));
    }
    lastExecutionTrace_.push_back(
        {gpu::QueueExecutionTraceKind::DirectWaitBeforeComposite, QueueKind::Direct, fenceValue});

    slot.submittedFrameIndex = frameContext.frameIndex;
    slot.timingPending = true;
    if (frameSlotUsageHistory_.size() >= kMaxFrameSlotUsageHistory)
    {
        frameSlotUsageHistory_.erase(frameSlotUsageHistory_.begin());
    }
    frameSlotUsageHistory_.push_back(
        {frameContext.frameIndex, frameContext.frameSlot, fenceValue, slot.submissionCalibration.frameIndex});
    return {};
}

gpu::LabGraph const *Renderer::CompiledGraph() const noexcept
{
    return graph_ ? &*graph_ : nullptr;
}

SchedulePlan const *Renderer::SerialSchedule() const noexcept
{
    return schedules_ ? &schedules_->serial : nullptr;
}

SchedulePlan const *Renderer::AsyncSchedule() const noexcept
{
    return schedules_ ? &schedules_->async : nullptr;
}

gpu::PhysicalTextureByteSizes const *Renderer::PhysicalByteSizes() const noexcept
{
    return physicalByteSizes_ ? &*physicalByteSizes_ : nullptr;
}

std::vector<gpu::QueueExecutionTraceRecord> const &Renderer::LastExecutionTrace() const noexcept
{
    return lastExecutionTrace_;
}

std::vector<gpu::ExecutedTextureBarrier> const &Renderer::LastProducerBarrierTrace() const noexcept
{
    return lastProducerBarrierTrace_;
}

std::optional<gpu::GpuTimingSample> const &Renderer::LastGpuTimingSample() const noexcept
{
    return lastGpuTimingSample_;
}

std::vector<gpu::FrameSlotUsageRecord> const &Renderer::FrameSlotUsageHistory() const noexcept
{
    return frameSlotUsageHistory_;
}

void Renderer::DrainComputeQueueForShutdown() noexcept
{
    if (computeQueue_ == nullptr || computeFence_ == nullptr || lastSignaledComputeFenceValue_ == 0U ||
        computeFence_->GetCompletedValue() >= lastSignaledComputeFenceValue_)
    {
        return;
    }
    if (deviceResources_ == nullptr || FAILED(deviceResources_->device()->GetDeviceRemovedReason()))
    {
        return;
    }
    HANDLE const eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (eventHandle == nullptr)
    {
        return;
    }
    if (SUCCEEDED(computeFence_->SetEventOnCompletion(lastSignaledComputeFenceValue_, eventHandle)))
    {
        (void)WaitForSingleObject(eventHandle, 5'000U);
    }
    CloseHandle(eventHandle);
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    if (imguiFrameBegun_)
    {
        ImGui::EndFrame();
        imguiFrameBegun_ = false;
    }
    if (imguiInitialized_)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized_ = false;
    }
    DrainComputeQueueForShutdown();
    ReleaseSizeDependentResources();
    frameSlotUsageHistory_.clear();
    lastGpuTimingSample_.reset();
    if (deviceResources_ != nullptr && imguiFontDescriptor_)
    {
        deviceResources_->shader_visible_cbv_srv_uav_heap().Free(imguiFontDescriptor_);
        imguiFontDescriptor_ = {};
    }
    if (deviceResources_ != nullptr && textureRtvs_)
    {
        deviceResources_->rtv_heap().Free(textureRtvs_);
        textureRtvs_ = {};
    }
    if (deviceResources_ != nullptr && textureDescriptors_)
    {
        deviceResources_->shader_visible_cbv_srv_uav_heap().Free(textureDescriptors_);
        textureDescriptors_ = {};
    }
    directTimestampHeap_.Reset();
    computeTimestampHeap_.Reset();
    frameSlots_.clear();
    computeFence_.Reset();
    computeQueue_.Reset();
    pipelines_ = {};
    rootSignature_.Reset();
    deviceResources_ = nullptr;
}

} // namespace ch10::pass_scheduling::solution
