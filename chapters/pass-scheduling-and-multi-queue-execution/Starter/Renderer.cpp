#include "Renderer.hpp"

#include <lgp/framework/pix.hpp>

#include <array>
#include <filesystem>
#include <utility>

namespace ch10::pass_scheduling::starter
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

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "MultiQueueSchedulingLab.hlsl";
}

[[nodiscard]] std::size_t TextureIndex(gpu::LabTextureIndex index) noexcept
{
    return static_cast<std::size_t>(index);
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
    return gpu::CreateLabPipelines(*deviceResources_->device(), deviceResources_->back_buffer_format(),
                                   *rootSignature_.Get(), shaders_, pipelines_);
}

void Renderer::ReleaseSizeDependentResources() noexcept
{
    frameSlots_.clear();
    graph_.reset();
    physicalByteSizes_.reset();
    serialSchedule_.reset();
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

    auto graphResult = gpu::CompileLabGraph(deviceResources_->headless());
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

    frameSlots_.resize(frameSlotCount);
    D3D12_RESOURCE_DESC1 const computeDescription = gpu::ComputeTextureDescription(size);
    D3D12_RESOURCE_DESC1 const graphicsDescription = gpu::GraphicsTextureDescription(size);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = gpu::kTransientFormat;
    clearValue.Color[3] = 1.0F;

    for (UINT frameSlot = 0U; frameSlot < frameSlotCount; ++frameSlot)
    {
        FrameSlotTextures &slot = frameSlots_[frameSlot];
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
                                                     "Failed to create a per-frame Chapter 10 Starter texture."));
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
    serialSchedule_ = std::move(schedulesResult->serial);
    return {};
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

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    if (auto status = CreateDeviceObjects(); !status)
    {
        return status;
    }
    return CreateSizeDependentResources(context.drawableSize);
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D drawableSize)
{
    return CreateSizeDependentResources(drawableSize);
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &)
{
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr)
    {
        return std::unexpected(lgp::framework::MakeError("Starter::Renderer::Render",
                                                         "The frame context does not provide a direct command list."));
    }
    if (frameContext.renderTarget == nullptr)
    {
        return std::unexpected(lgp::framework::MakeError("Starter::Renderer::Render",
                                                         "The frame context does not provide a render target."));
    }
    if (!graph_ || !serialSchedule_ || !physicalByteSizes_)
    {
        return std::unexpected(lgp::framework::MakeError(
            "Starter::Renderer::Render",
            "Size-dependent graph, schedule, and texture resources are unavailable; resize to a non-empty extent "
            "before rendering."));
    }
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Starter::Renderer::Render",
                                                         "The frame context references an out-of-range frame slot."));
    }

    FrameSlotTextures &slot = frameSlots_[frameContext.frameSlot];
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *const descriptorHeaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, descriptorHeaps);
    commandList.SetComputeRootSignature(rootSignature_.Get());
    commandList.SetGraphicsRootSignature(rootSignature_.Get());
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    lgp::framework::PixEventScope frameEvent{commandList, lgp::framework::PixColor(80U, 160U, 255U),
                                             L"Ch10 Starter Serial"};

    ID3D12Resource &computeScratch = *slot.textures[TextureIndex(gpu::LabTextureIndex::ComputeScratch)].Get();
    ID3D12Resource &computeFinal = *slot.textures[TextureIndex(gpu::LabTextureIndex::ComputeFinal)].Get();
    ID3D12Resource &graphicsScratch = *slot.textures[TextureIndex(gpu::LabTextureIndex::GraphicsScratch)].Get();
    ID3D12Resource &graphicsFinal = *slot.textures[TextureIndex(gpu::LabTextureIndex::GraphicsFinal)].Get();

    std::vector<D3D12_TEXTURE_BARRIER> barriers{
        gpu::MakeTextureBarrier(computeScratch, gpu::UndefinedState(), gpu::ComputeUnorderedAccessState(),
                                D3D12_TEXTURE_BARRIER_FLAG_DISCARD),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);
    commandList.SetComputeRootDescriptorTable(
        2U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, kComputeScratchUav)));
    Dispatch(commandList, *pipelines_.computeGenerate.Get(), frameContext.drawableSize);

    barriers = {
        gpu::MakeTextureBarrier(computeScratch, gpu::ComputeUnorderedAccessState(), gpu::ComputeShaderResourceState()),
        gpu::MakeTextureBarrier(computeFinal, gpu::UndefinedState(), gpu::ComputeUnorderedAccessState(),
                                D3D12_TEXTURE_BARRIER_FLAG_DISCARD),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);
    commandList.SetComputeRootDescriptorTable(
        0U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, kComputeScratchSrv)));
    commandList.SetComputeRootDescriptorTable(
        2U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, kComputeFinalUav)));
    Dispatch(commandList, *pipelines_.computeCollapse.Get(), frameContext.drawableSize);

    lgp::framework::TextureBarrierState const directSharedState = gpu::PixelShaderResourceState();
    barriers = {
        gpu::MakeTextureBarrier(computeScratch, gpu::ComputeShaderResourceState(), gpu::UndefinedState()),
        gpu::MakeTextureBarrier(computeFinal, gpu::ComputeUnorderedAccessState(), directSharedState),
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
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::FrameStartState(frameContext.headless),
                                gpu::RenderTargetState()),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);
    commandList.SetGraphicsRootDescriptorTable(
        0U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, kComputeFinalSrv)));
    commandList.SetGraphicsRootDescriptorTable(
        1U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, kGraphicsFinalSrv)));
    DrawTo(commandList, *pipelines_.composite.Get(), frameContext.renderTargetView);

    barriers = {
        gpu::MakeTextureBarrier(computeFinal, directSharedState, gpu::UndefinedState()),
        gpu::MakeTextureBarrier(graphicsFinal, gpu::PixelShaderResourceState(), gpu::UndefinedState()),
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(),
                                gpu::FrameEndState(frameContext.headless)),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);
    return {};
}

gpu::LabGraph const *Renderer::CompiledGraph() const noexcept
{
    return graph_ ? &*graph_ : nullptr;
}

SchedulePlan const *Renderer::SerialSchedule() const noexcept
{
    return serialSchedule_ ? &*serialSchedule_ : nullptr;
}

gpu::PhysicalTextureByteSizes const *Renderer::PhysicalByteSizes() const noexcept
{
    return physicalByteSizes_ ? &*physicalByteSizes_ : nullptr;
}

std::vector<gpu::QueueExecutionTraceRecord> const &Renderer::LastExecutionTrace() const noexcept
{
    return emptyExecutionTrace_;
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    ReleaseSizeDependentResources();
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
    pipelines_ = {};
    rootSignature_.Reset();
    deviceResources_ = nullptr;
}

} // namespace ch10::pass_scheduling::starter
