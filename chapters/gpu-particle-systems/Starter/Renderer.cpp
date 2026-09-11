#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>

namespace ch34::particles::starter
{
namespace
{

using Microsoft::WRL::ComPtr;

enum GraphicsRootParameter : UINT
{
    DrawConstantsParam = 0U,
    DrawSrvTable = 1U,
};

struct DrawConstants final
{
    std::uint32_t drawStateSlot{};
    float viewScale{};
    float quadHalfExtent{};
    std::uint32_t pad{};
};

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "ParticleLab.hlsl";
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

    if (auto status = gpu::CompileShader(compiler, options, L"ParticleVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    return gpu::CompileShader(compiler, options, L"ParticlePS", L"ps_6_0", pixelShader_);
}

lgp::framework::Status Renderer::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 3U;
    srvRange.BaseShaderRegister = 0U;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[DrawConstantsParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[DrawConstantsParam].Constants.ShaderRegister = 1U;
    parameters[DrawConstantsParam].Constants.Num32BitValues = sizeof(DrawConstants) / sizeof(std::uint32_t);
    parameters[DrawConstantsParam].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[DrawSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[DrawSrvTable].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[DrawSrvTable].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[DrawSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

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
                                             "Failed to create the Chapter 34 Starter root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreatePipeline()
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
                                                                "Failed to create the Chapter 34 Starter pipeline."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateResources()
{
    ID3D12Device10 &device = *deviceResources_->device();
    std::uint64_t const stateBytes = static_cast<std::uint64_t>(gpu::kMaximumLabCapacity) * sizeof(gpu::GpuParticle);
    std::uint64_t const listBytes = static_cast<std::uint64_t>(gpu::kMaximumLabCapacity + 1U) * sizeof(std::uint32_t);

    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlotResources &slot : frameSlots_)
    {
        auto stateA = gpu::CreateBuffer(device, stateBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                        L"Ch34 Starter State A", true);
        auto stateB = gpu::CreateBuffer(device, stateBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                        L"Ch34 Starter State B", true);
        auto slots = gpu::CreateBuffer(device, listBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                       L"Ch34 Starter Slots", true);
        if (!stateA)
        {
            return std::unexpected(std::move(stateA.error()));
        }
        if (!stateB)
        {
            return std::unexpected(std::move(stateB.error()));
        }
        if (!slots)
        {
            return std::unexpected(std::move(slots.error()));
        }
        slot.stateA = std::move(*stateA);
        slot.stateB = std::move(*stateB);
        slot.slots = std::move(*slots);

        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(3U);
        if (!descriptors)
        {
            return std::unexpected(std::move(descriptors.error()));
        }
        slot.descriptors = *descriptors;

        auto structuredSrv =
            [&device](gpu::BufferResource &buffer, UINT numElements, UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE handle)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.Buffer.FirstElement = 0U;
            srv.Buffer.NumElements = numElements;
            srv.Buffer.StructureByteStride = stride;
            device.CreateShaderResourceView(buffer.Get(), &srv, handle);
        };
        structuredSrv(slot.stateA, gpu::kMaximumLabCapacity, sizeof(gpu::GpuParticle), slot.descriptors.CpuHandle(0U));
        structuredSrv(slot.stateB, gpu::kMaximumLabCapacity, sizeof(gpu::GpuParticle), slot.descriptors.CpuHandle(1U));
        structuredSrv(slot.slots, gpu::kMaximumLabCapacity + 1U, sizeof(std::uint32_t), slot.descriptors.CpuHandle(2U));
    }
    return {};
}

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    if (headlessConfiguration_.has_value())
    {
        return *headlessConfiguration_;
    }
    return gpu::DefaultLabConfiguration();
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreateRootSignature(); !status)
    {
        return status;
    }
    if (auto status = CreatePipeline(); !status)
    {
        return status;
    }
    return CreateResources();
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
    auto reference = gpu::BuildReferenceReadback(ActiveConfiguration());
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
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 34 frame slot is out of range."));
    }

    gpu::LabConfiguration const configuration = ActiveConfiguration();
    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];

    // The honest baseline: the CPU reference already advanced the system and compacted the live set. Upload the
    // final state and the compacted slot list and draw them directly, one instanced quad per live particle.
    if (auto status = gpu::WriteBuffer(slot.stateA, std::span<gpu::GpuParticle const>{currentReference_.state});
        !status)
    {
        return status;
    }

    std::vector<std::uint32_t> slotList(static_cast<std::size_t>(gpu::kMaximumLabCapacity) + 1U, kInvalidParticleId);
    for (std::size_t index = 0U; index < currentReference_.compactedSlots.size(); ++index)
    {
        slotList[index] = currentReference_.compactedSlots[index];
    }
    if (auto status = gpu::WriteBuffer(slot.slots, std::span<std::uint32_t const>{slotList}); !status)
    {
        return status;
    }

    std::uint32_t const instanceCount = currentReference_.indirectArguments.instanceCount;

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

    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.SetGraphicsRootDescriptorTable(DrawSrvTable, slot.descriptors.GpuHandle(0U));

    DrawConstants drawConstants{};
    drawConstants.drawStateSlot = 0U;
    drawConstants.viewScale = configuration.viewScale;
    drawConstants.quadHalfExtent = configuration.quadHalfExtent;
    commandList.SetGraphicsRoot32BitConstants(DrawConstantsParam, sizeof(DrawConstants) / sizeof(std::uint32_t),
                                              &drawConstants, 0U);
    commandList.SetPipelineState(graphicsPipeline_.Get());
    if (instanceCount > 0U)
    {
        commandList.DrawInstanced(gpu::kVertexCountPerParticle, instanceCount, 0U, 0U);
    }

    barriers = {
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(), gpu::FrameEndState(frameContext)),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);
    return {};
}

std::expected<gpu::FrameReadback, lgp::framework::Error> Renderer::ReadBackOutputs()
{
    // The Starter runs none of the GPU compute stages: its evidence is the CPU reference it drew. The stage
    // partition says so honestly, which is the contrast the Solution's full pipeline is measured against.
    gpu::FrameReadback readback = currentReference_;
    StagePartition partition{};
    auto record = [&partition](FrameStage stage, bool submitted)
    {
        if (submitted)
        {
            partition.submittedMask |= StageBit(stage);
        }
        else
        {
            partition.skippedMask |= StageBit(stage);
        }
    };
    record(FrameStage::Reset, false);
    record(FrameStage::Emission, false);
    record(FrameStage::Simulation, false);
    record(FrameStage::Compaction, false);
    record(FrameStage::IndirectArguments, false);
    record(FrameStage::Render, readback.emittedCount > 0U);
    record(FrameStage::Readback, true);
    partition.recordedMask = partition.submittedMask | partition.skippedMask | partition.unavailableMask;
    readback.stagePartition = partition;
    readback.indirectArguments = {};
    readback.executedCommandCount = 0U;
    readback.guardsIntact = false;
    readback.barriersModelled = false;
    return readback;
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    for (FrameSlotResources &slot : frameSlots_)
    {
        if (slot.descriptors)
        {
            deviceResources.shader_visible_cbv_srv_uav_heap().Free(slot.descriptors);
            slot.descriptors = {};
        }
    }
    frameSlots_.clear();
    graphicsPipeline_.Reset();
    graphicsRootSignature_.Reset();
    vertexShader_ = {};
    pixelShader_ = {};
    deviceResources_ = nullptr;
}

void Renderer::ConfigureHeadlessTest(gpu::LabConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

} // namespace ch34::particles::starter
