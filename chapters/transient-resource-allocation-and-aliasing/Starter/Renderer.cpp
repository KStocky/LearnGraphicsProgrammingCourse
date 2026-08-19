#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include "../Common/GpuLabSupport.hpp"

#include <d3d12.h>

#include <array>
#include <filesystem>
#include <utility>

namespace ch09::transient_aliasing::starter
{
namespace
{

using gpu::MakeTextureBarrier;
using gpu::RenderTargetState;
using gpu::ShaderResourceState;
using gpu::UndefinedState;

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "TransientAliasingLab.hlsl";
}

void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, D3D12_TEXTURE_BARRIER *barriers, UINT barrierCount)
{
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = barrierCount;
    group.pTextureBarriers = barriers;
    commandList.Barrier(1U, &group);
}

} // namespace

lgp::framework::Status Renderer::CreateShaders()
{
    return gpu::CreateLabShaders(ShaderPath(), fullscreenVertexShader_, analyticPixelShader_, copyPixelShader_,
                                 accentPixelShader_, compositePixelShader_);
}

lgp::framework::Status Renderer::CreateRootSignature()
{
    return gpu::CreateLabRootSignature(*deviceResources_->device(), rootSignature_);
}

lgp::framework::Status Renderer::CreatePipelineStates()
{
    return gpu::CreateLabPipelineStates(*deviceResources_->device(), deviceResources_->back_buffer_format(),
                                        *rootSignature_.Get(), fullscreenVertexShader_, analyticPixelShader_,
                                        copyPixelShader_, accentPixelShader_, compositePixelShader_, analyticPipeline_,
                                        copyPipeline_, accentPipeline_, compositePipeline_);
}

lgp::framework::Status Renderer::CreateTransientTextures(lgp::framework::Extent2D size)
{
    for (auto &texture : transientTextures_)
    {
        texture.Reset();
    }
    if (size.empty())
    {
        return {};
    }

    if (!transientRtvs_)
    {
        auto result = deviceResources_->rtv_heap().Allocate(3U);
        if (!result)
        {
            return std::unexpected(std::move(result.error()));
        }
        transientRtvs_ = *result;
    }
    if (!transientSrvs_)
    {
        auto result = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(3U);
        if (!result)
        {
            return std::unexpected(std::move(result.error()));
        }
        transientSrvs_ = *result;
    }

    D3D12_RESOURCE_DESC1 const description = gpu::TextureDescription(size);
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = gpu::kTransientFormat;
    clearValue.Color[3] = 1.0F;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (UINT textureIndex = 0U; textureIndex < transientTextures_.size(); ++textureIndex)
    {
        HRESULT const result = deviceResources_->device()->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_UNDEFINED, &clearValue, nullptr, 0U,
            nullptr, IID_PPV_ARGS(transientTextures_[textureIndex].ReleaseAndGetAddressOf()));
        if (FAILED(result))
        {
            return std::unexpected(
                lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                 "Failed to create a committed Chapter 9 Starter transient texture."));
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = gpu::kTransientFormat;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateRenderTargetView(transientTextures_[textureIndex].Get(), &rtv,
                                                           transientRtvs_.CpuHandle(textureIndex));

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = gpu::kTransientFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1U;
        deviceResources_->device()->CreateShaderResourceView(transientTextures_[textureIndex].Get(), &srv,
                                                             transientSrvs_.CpuHandle(textureIndex));
    }
    return {};
}

void Renderer::DrawTo(ID3D12GraphicsCommandList7 &commandList, ID3D12PipelineState &pipeline,
                      D3D12_CPU_DESCRIPTOR_HANDLE renderTarget) const
{
    commandList.OMSetRenderTargets(1U, &renderTarget, FALSE, nullptr);
    commandList.SetPipelineState(&pipeline);
    commandList.DrawInstanced(3U, 1U, 0U, 0U);
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreateRootSignature(); !status)
    {
        return status;
    }
    if (auto status = CreatePipelineStates(); !status)
    {
        return status;
    }
    return CreateTransientTextures(context.drawableSize);
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D drawableSize)
{
    return CreateTransientTextures(drawableSize);
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &)
{
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr ||
        transientTextures_[0] == nullptr || transientTextures_[1] == nullptr || transientTextures_[2] == nullptr)
    {
        return {};
    }

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.SetGraphicsRootSignature(rootSignature_.Get());

    std::array<D3D12_TEXTURE_BARRIER, 1U> analyticBarriers{
        MakeTextureBarrier(*transientTextures_[0].Get(), UndefinedState(), RenderTargetState(),
                           D3D12_TEXTURE_BARRIER_FLAG_DISCARD),
    };
    SubmitTextureBarriers(commandList, analyticBarriers.data(), static_cast<UINT>(analyticBarriers.size()));
    DrawTo(commandList, *analyticPipeline_.Get(), transientRtvs_.CpuHandle(0U));

    std::array<D3D12_TEXTURE_BARRIER, 2U> copyBarriers{
        MakeTextureBarrier(*transientTextures_[0].Get(), RenderTargetState(), ShaderResourceState()),
        MakeTextureBarrier(*transientTextures_[1].Get(), UndefinedState(), RenderTargetState(),
                           D3D12_TEXTURE_BARRIER_FLAG_DISCARD),
    };
    SubmitTextureBarriers(commandList, copyBarriers.data(), static_cast<UINT>(copyBarriers.size()));
    ID3D12DescriptorHeap *const descriptorHeaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, descriptorHeaps);
    commandList.SetGraphicsRootDescriptorTable(0U, transientSrvs_.GpuHandle(0U));
    DrawTo(commandList, *copyPipeline_.Get(), transientRtvs_.CpuHandle(1U));

    std::array<D3D12_TEXTURE_BARRIER, 2U> accentBoundaryBarriers{
        MakeTextureBarrier(
            *transientTextures_[0].Get(), ShaderResourceState(),
            {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_UNDEFINED}),
        MakeTextureBarrier(
            *transientTextures_[2].Get(),
            {D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_UNDEFINED},
            RenderTargetState(), D3D12_TEXTURE_BARRIER_FLAG_DISCARD),
    };
    SubmitTextureBarriers(commandList, accentBoundaryBarriers.data(), static_cast<UINT>(accentBoundaryBarriers.size()));
    DrawTo(commandList, *accentPipeline_.Get(), transientRtvs_.CpuHandle(2U));

    std::array<D3D12_TEXTURE_BARRIER, 3U> compositeBarriers{
        MakeTextureBarrier(*transientTextures_[1].Get(), RenderTargetState(), ShaderResourceState()),
        MakeTextureBarrier(*transientTextures_[2].Get(), RenderTargetState(), ShaderResourceState()),
        MakeTextureBarrier(*frameContext.renderTarget, gpu::FrameStartState(frameContext.headless),
                           RenderTargetState()),
    };
    SubmitTextureBarriers(commandList, compositeBarriers.data(), static_cast<UINT>(compositeBarriers.size()));
    commandList.SetDescriptorHeaps(1U, descriptorHeaps);
    commandList.SetGraphicsRootDescriptorTable(0U, transientSrvs_.GpuHandle(1U));
    DrawTo(commandList, *compositePipeline_.Get(), frameContext.renderTargetView);

    std::array<D3D12_TEXTURE_BARRIER, 3U> boundaryBarriers{
        MakeTextureBarrier(*transientTextures_[1].Get(), ShaderResourceState(), UndefinedState()),
        MakeTextureBarrier(*transientTextures_[2].Get(), ShaderResourceState(), UndefinedState()),
        MakeTextureBarrier(*frameContext.renderTarget, RenderTargetState(), gpu::FrameEndState(frameContext.headless)),
    };
    SubmitTextureBarriers(commandList, boundaryBarriers.data(), static_cast<UINT>(boundaryBarriers.size()));
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    for (auto &texture : transientTextures_)
    {
        texture.Reset();
    }
    if (deviceResources_ != nullptr && transientRtvs_)
    {
        deviceResources_->rtv_heap().Free(transientRtvs_);
        transientRtvs_ = {};
    }
    if (deviceResources_ != nullptr && transientSrvs_)
    {
        deviceResources_->shader_visible_cbv_srv_uav_heap().Free(transientSrvs_);
        transientSrvs_ = {};
    }
    compositePipeline_.Reset();
    accentPipeline_.Reset();
    copyPipeline_.Reset();
    analyticPipeline_.Reset();
    rootSignature_.Reset();
    deviceResources_ = nullptr;
}

} // namespace ch09::transient_aliasing::starter
