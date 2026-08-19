#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/shader_compiler.hpp>

namespace ch08::frame_graph::starter
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

enum RootParameter : UINT
{
    HdrTexture = 0U,
};

[[nodiscard]] std::string D3D12BlobToUtf8(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }

    return {
        static_cast<char const *>(blob->GetBufferPointer()),
        static_cast<std::size_t>(blob->GetBufferSize()),
    };
}

[[nodiscard]] std::filesystem::path ResolveShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "PassGraphLab.hlsl";
}

[[nodiscard]] lgp::framework::TextureBarrierState ShaderResourceState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_PIXEL_SHADING,
        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
        D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
    };
}

[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_RENDER_TARGET,
        D3D12_BARRIER_ACCESS_RENDER_TARGET,
        D3D12_BARRIER_LAYOUT_RENDER_TARGET,
    };
}

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frameContext.renderTargetInitialLayout};
}

[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept
{
    return {
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT,
    };
}

[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(ID3D12Resource &resource,
                                                       lgp::framework::TextureBarrierState before,
                                                       lgp::framework::TextureBarrierState after) noexcept
{
    D3D12_TEXTURE_BARRIER barrier{};
    barrier.SyncBefore = before.sync;
    barrier.SyncAfter = after.sync;
    barrier.AccessBefore = before.access;
    barrier.AccessAfter = after.access;
    barrier.LayoutBefore = before.layout;
    barrier.LayoutAfter = after.layout;
    barrier.pResource = &resource;
    barrier.Subresources.IndexOrFirstMipLevel = UINT32_MAX;
    return barrier;
}

} // namespace

lgp::framework::Status Renderer::CreateShaders()
{
    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    lgp::framework::ShaderCompiler compiler = std::move(*compilerResult);

    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = ResolveShaderPath();
    options.includeDirectories = {options.sourcePath.parent_path()};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif

    options.entryPoint = L"FullscreenVS";
    options.targetProfile = L"vs_6_0";
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto vertexResult = compiler.Compile(options);
    if (!vertexResult)
    {
        return std::unexpected(std::move(vertexResult.error()));
    }
    fullscreenVertexShader_ = std::move(*vertexResult);

    options.entryPoint = L"HdrAnalyticPS";
    options.targetProfile = L"ps_6_0";
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto hdrResult = compiler.Compile(options);
    if (!hdrResult)
    {
        return std::unexpected(std::move(hdrResult.error()));
    }
    hdrPixelShader_ = std::move(*hdrResult);

    options.entryPoint = L"DisplayPS";
    options.targetProfile = L"ps_6_0";
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto displayResult = compiler.Compile(options);
    if (!displayResult)
    {
        return std::unexpected(std::move(displayResult.error()));
    }
    displayPixelShader_ = std::move(*displayResult);
    return {};
}

lgp::framework::Status Renderer::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE descriptorRange{};
    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange.NumDescriptors = 1U;
    descriptorRange.BaseShaderRegister = 0U;
    descriptorRange.RegisterSpace = 0U;
    descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1U;
    parameter.DescriptorTable.pDescriptorRanges = &descriptorRange;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0U;
    sampler.RegisterSpace = 0U;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 1U;
    description.pParameters = &parameter;
    description.NumStaticSamplers = 1U;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};
    HRESULT const serializeResult =
        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, serialized.ReleaseAndGetAddressOf(),
                                    errors.ReleaseAndGetAddressOf());
    if (FAILED(serializeResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult,
                                                                D3D12BlobToUtf8(errors.Get())));
    }

    HRESULT const createResult =
        deviceResources_->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(rootSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                                                "Failed to create the Chapter 8 root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreatePipelineStates()
{
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature_.Get();
    description.VS = fullscreenVertexShader_.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.SampleDesc.Count = 1U;

    description.PS = hdrPixelShader_.Bytecode();
    description.RTVFormats[0] = kHdrFormat;
    HRESULT const hdrResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(hdrPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(hdrResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", hdrResult,
                                             "Failed to create the Chapter 8 analytic HDR pipeline state."));
    }

    description.PS = displayPixelShader_.Bytecode();
    description.RTVFormats[0] = deviceResources_->back_buffer_format();
    HRESULT const displayResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(displayPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(displayResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", displayResult,
                                             "Failed to create the Chapter 8 display pipeline state."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateHdrIntermediate(lgp::framework::Extent2D size)
{
    hdrIntermediate_.Reset();
    if (size.empty())
    {
        return {};
    }

    if (!hdrRtv_)
    {
        auto rtvResult = deviceResources_->rtv_heap().Allocate(1U);
        if (!rtvResult)
        {
            return std::unexpected(std::move(rtvResult.error()));
        }
        hdrRtv_ = *rtvResult;
    }

    if (!hdrSrv_)
    {
        auto srvResult = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(1U);
        if (!srvResult)
        {
            return std::unexpected(std::move(srvResult.error()));
        }
        hdrSrv_ = *srvResult;
    }

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = kHdrFormat;
    clearValue.Color[3] = 1.0F;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = size.width;
    description.Height = size.height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = kHdrFormat;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    HRESULT const createResult = deviceResources_->device()->CreateCommittedResource3(
        &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &clearValue, nullptr, 0U,
        nullptr, IID_PPV_ARGS(hdrIntermediate_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3",
                                                                createResult,
                                                                "Failed to create the Chapter 8 HDR intermediate."));
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = kHdrFormat;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    deviceResources_->device()->CreateRenderTargetView(hdrIntermediate_.Get(), &rtv, hdrRtv_.cpuHandle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kHdrFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1U;
    deviceResources_->device()->CreateShaderResourceView(hdrIntermediate_.Get(), &srv, hdrSrv_.cpuHandle);
    return {};
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
    return CreateHdrIntermediate(context.drawableSize);
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D drawableSize)
{
    return CreateHdrIntermediate(drawableSize);
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &)
{
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr || hdrIntermediate_ == nullptr)
    {
        return {};
    }

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.SetGraphicsRootSignature(rootSignature_.Get());

    std::array<D3D12_TEXTURE_BARRIER, 1U> analyticBarriers{
        MakeTextureBarrier(*hdrIntermediate_.Get(), ShaderResourceState(), RenderTargetState()),
    };
    D3D12_BARRIER_GROUP analyticGroup{};
    analyticGroup.Type = D3D12_BARRIER_TYPE_TEXTURE;
    analyticGroup.NumBarriers = static_cast<UINT>(analyticBarriers.size());
    analyticGroup.pTextureBarriers = analyticBarriers.data();
    commandList.Barrier(1U, &analyticGroup);

    float const hdrClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.OMSetRenderTargets(1U, &hdrRtv_.cpuHandle, FALSE, nullptr);
    commandList.ClearRenderTargetView(hdrRtv_.cpuHandle, hdrClear, 0U, nullptr);
    commandList.SetPipelineState(hdrPipeline_.Get());
    commandList.DrawInstanced(3U, 1U, 0U, 0U);

    std::array<D3D12_TEXTURE_BARRIER, 2U> displayBarriers{
        MakeTextureBarrier(*hdrIntermediate_.Get(), RenderTargetState(), ShaderResourceState()),
        MakeTextureBarrier(*frameContext.renderTarget, FrameStartState(frameContext), RenderTargetState()),
    };
    D3D12_BARRIER_GROUP displayGroup{};
    displayGroup.Type = D3D12_BARRIER_TYPE_TEXTURE;
    displayGroup.NumBarriers = static_cast<UINT>(displayBarriers.size());
    displayGroup.pTextureBarriers = displayBarriers.data();
    commandList.Barrier(1U, &displayGroup);

    float const frameClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.ClearRenderTargetView(frameContext.renderTargetView, frameClear, 0U, nullptr);
    ID3D12DescriptorHeap *const descriptorHeaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, descriptorHeaps);
    commandList.SetPipelineState(displayPipeline_.Get());
    commandList.SetGraphicsRootDescriptorTable(HdrTexture, hdrSrv_.gpuHandle);
    commandList.DrawInstanced(3U, 1U, 0U, 0U);

    std::array<D3D12_TEXTURE_BARRIER, 1U> frameBoundaryBarriers{
        MakeTextureBarrier(*frameContext.renderTarget, RenderTargetState(), FrameEndState(frameContext)),
    };
    D3D12_BARRIER_GROUP frameBoundaryGroup{};
    frameBoundaryGroup.Type = D3D12_BARRIER_TYPE_TEXTURE;
    frameBoundaryGroup.NumBarriers = static_cast<UINT>(frameBoundaryBarriers.size());
    frameBoundaryGroup.pTextureBarriers = frameBoundaryBarriers.data();
    commandList.Barrier(1U, &frameBoundaryGroup);
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    hdrIntermediate_.Reset();
    if (deviceResources_ != nullptr && hdrRtv_)
    {
        deviceResources_->rtv_heap().Free(hdrRtv_);
        hdrRtv_ = {};
    }
    if (deviceResources_ != nullptr && hdrSrv_)
    {
        deviceResources_->shader_visible_cbv_srv_uav_heap().Free(hdrSrv_);
        hdrSrv_ = {};
    }
    displayPipeline_.Reset();
    hdrPipeline_.Reset();
    rootSignature_.Reset();
    deviceResources_ = nullptr;
}

} // namespace ch08::frame_graph::starter
