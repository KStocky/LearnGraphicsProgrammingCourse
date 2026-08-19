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
#include <cstdio>
#include <filesystem>
#include <string>

namespace ch13::work_distribution::starter
{
namespace
{

using Microsoft::WRL::ComPtr;

enum RootParameter : UINT
{
    CandidateRootConstant = 0U,
    CandidateSrvTable = 1U,
};

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "WorkDistributionLab.hlsl";
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
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
    options.sourcePath = ShaderPath();
    options.includeDirectories = {options.sourcePath.parent_path()};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif

    if (auto status = gpu::CompileShader(compiler, options, L"IndirectVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    return gpu::CompileShader(compiler, options, L"ColorPS", L"ps_6_0", pixelShader_);
}

lgp::framework::Status Renderer::CreateGraphicsRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1U;
    srvRange.BaseShaderRegister = 0U;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[CandidateRootConstant].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[CandidateRootConstant].Constants.ShaderRegister = 0U;
    parameters[CandidateRootConstant].Constants.Num32BitValues = 1U;
    parameters[CandidateRootConstant].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[CandidateSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[CandidateSrvTable].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[CandidateSrvTable].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[CandidateSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
    description.pParameters = parameters;
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
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult, BlobText(errors.Get())));
    }

    HRESULT const createResult =
        deviceResources_->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(graphicsRootSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                             "Failed to create the Chapter 13 Starter root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateGraphicsPipeline()
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
    description.pRootSignature = graphicsRootSignature_.Get();
    description.VS = vertexShader_.Bytecode();
    description.PS = pixelShader_.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = deviceResources_->back_buffer_format();
    description.SampleDesc.Count = 1U;

    HRESULT const createResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(graphicsPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState",
                                                                createResult,
                                                                "Failed to create the Chapter 13 Starter pipeline."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateCandidateResources()
{
    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlotResources &slot : frameSlots_)
    {
        auto candidateBuffer = gpu::CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(gpu::kCandidateCount) * sizeof(gpu::CandidateData),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch13 Starter Candidate Buffer", true);
        if (!candidateBuffer)
        {
            return std::unexpected(std::move(candidateBuffer.error()));
        }
        slot.candidateBuffer = std::move(*candidateBuffer);

        auto candidateSrv = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(1U);
        if (!candidateSrv)
        {
            return std::unexpected(std::move(candidateSrv.error()));
        }
        slot.candidateSrv = *candidateSrv;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Buffer.FirstElement = 0U;
        srv.Buffer.NumElements = gpu::kCandidateCount;
        srv.Buffer.StructureByteStride = sizeof(gpu::CandidateData);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        deviceResources_->device()->CreateShaderResourceView(slot.candidateBuffer.Get(), &srv,
                                                             slot.candidateSrv.cpuHandle);
    }
    return {};
}

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    if (headless_ && headlessConfiguration_.has_value())
    {
        return {
            headlessConfiguration_->scene,
            gpu::NormalizeCapacity(headlessConfiguration_->capacity),
            gpu::ExecutionMode::Stable,
        };
    }
    return {};
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreateGraphicsRootSignature(); !status)
    {
        return status;
    }
    if (auto status = CreateGraphicsPipeline(); !status)
    {
        return status;
    }
    return CreateCandidateResources();
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &deviceResources,
                                          lgp::framework::Extent2D drawableSize)
{
    (void)deviceResources;
    (void)drawableSize;
    return {};
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &context)
{
    (void)context;
    auto reference = gpu::BuildCpuReference(ActiveConfiguration());
    if (!reference)
    {
        return std::unexpected(std::move(reference.error()));
    }
    currentReference_ = std::move(*reference);
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 13 frame slot is out of range."));
    }

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    auto const writeStatus =
        gpu::WriteBuffer(slot.candidateBuffer, std::span<gpu::CandidateData const>{currentReference_.candidates});
    if (!writeStatus)
    {
        return writeStatus;
    }

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    std::vector<D3D12_TEXTURE_BARRIER> barriers{
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::FrameStartState(frameContext),
                                gpu::RenderTargetState()),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);

    float const clearColor[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    commandList.SetPipelineState(graphicsPipeline_.Get());

    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.SetGraphicsRootDescriptorTable(CandidateSrvTable, slot.candidateSrv.gpuHandle);

    for (IndirectDrawCommand const &command : currentReference_.indirectCommands)
    {
        commandList.SetGraphicsRoot32BitConstant(CandidateRootConstant, command.candidateIndex, 0U);
        commandList.DrawInstanced(command.draw.vertexCountPerInstance, command.draw.instanceCount,
                                  command.draw.startVertexLocation, command.draw.startInstanceLocation);
    }

    barriers = {
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(), gpu::FrameEndState(frameContext)),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    for (FrameSlotResources &slot : frameSlots_)
    {
        if (slot.candidateSrv)
        {
            deviceResources.shader_visible_cbv_srv_uav_heap().Free(slot.candidateSrv);
            slot.candidateSrv = {};
        }
    }
    frameSlots_.clear();
    graphicsPipeline_.Reset();
    graphicsRootSignature_.Reset();
    vertexShader_ = {};
    pixelShader_ = {};
    deviceResources_ = nullptr;
}

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

gpu::CpuReference const &Renderer::LastReference() const noexcept
{
    return currentReference_;
}

} // namespace ch13::work_distribution::starter
