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
#include <cstring>
#include <filesystem>
#include <string>

namespace ch13::work_distribution::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorsPerSlot = 7U;

enum GraphicsRootParameter : UINT
{
    CandidateRootConstant = 0U,
    CandidateSrvTable = 1U,
};

enum ComputeRootParameter : UINT
{
    DispatchRootConstants = 0U,
    ComputeSrvTable = 1U,
    ComputeUavTable = 2U,
};

enum DescriptorIndex : UINT
{
    CandidateSrv = 0U,
    FlagsSrv = 1U,
    FlagsUav = 2U,
    EmittedIndicesUav = 3U,
    IndirectCommandsUav = 4U,
    IndirectCountUav = 5U,
    StatisticsUav = 6U,
};

struct DispatchConstants final
{
    std::uint32_t capacity{};
    std::uint32_t candidateCount{gpu::kCandidateCount};
    std::uint32_t vertexCountPerQuad{gpu::kVertexCountPerQuad};
    std::uint32_t reserved{};
};

static_assert(sizeof(DispatchConstants) == 16U);

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

[[nodiscard]] lgp::framework::Status CreateComputePipeline(ID3D12Device10 &device, ID3D12RootSignature &rootSignature,
                                                           lgp::framework::CompiledShader const &shader,
                                                           char const *label, ComPtr<ID3D12PipelineState> &pipeline)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &rootSignature;
    description.CS = shader.Bytecode();

    HRESULT const result =
        device.CreateComputePipelineState(&description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState", result,
                                             std::string{"Failed to create the Chapter 13 "} + label + " pipeline."));
    }
    return {};
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

    if (auto status = gpu::CompileShader(compiler, options, L"ResetCS", L"cs_6_0", resetShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"ClassifyCS", L"cs_6_0", classifyShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"StableCompactCS", L"cs_6_0", stableShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"AtomicAppendCS", L"cs_6_0", atomicShader_); !status)
    {
        return status;
    }
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
                                             "Failed to create the Chapter 13 graphics root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateComputeRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 2U;
    srvRange.BaseShaderRegister = 0U;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 5U;
    uavRange.BaseShaderRegister = 0U;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[DispatchRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[DispatchRootConstants].Constants.ShaderRegister = 0U;
    parameters[DispatchRootConstants].Constants.Num32BitValues = sizeof(DispatchConstants) / sizeof(std::uint32_t);
    parameters[DispatchRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ComputeSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[ComputeSrvTable].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[ComputeSrvTable].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[ComputeSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ComputeUavTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[ComputeUavTable].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[ComputeUavTable].DescriptorTable.pDescriptorRanges = &uavRange;
    parameters[ComputeUavTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
    description.pParameters = parameters;

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
                                                        IID_PPV_ARGS(computeRootSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                             "Failed to create the Chapter 13 compute root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreatePipelines()
{
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), resetShader_,
                                            "reset", resetPipeline_);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), classifyShader_,
                                            "classification", classifyPipeline_);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), stableShader_,
                                            "stable compaction", stablePipeline_);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), atomicShader_,
                                            "atomic append", atomicPipeline_);
        !status)
    {
        return status;
    }

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
                                                                "Failed to create the Chapter 13 graphics pipeline."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateCommandSignature()
{
    D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arguments[0].Constant.RootParameterIndex = CandidateRootConstant;
    arguments[0].Constant.DestOffsetIn32BitValues = 0U;
    arguments[0].Constant.Num32BitValuesToSet = 1U;
    arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    D3D12_COMMAND_SIGNATURE_DESC description{};
    description.ByteStride = kIndirectDrawCommandStride;
    description.NumArgumentDescs = static_cast<UINT>(std::size(arguments));
    description.pArgumentDescs = arguments;

    IndirectExecutionLayout const layout{
        .byteStride = kIndirectDrawCommandStride,
        .argumentBufferOffset = 0U,
        .argumentBufferBytes = static_cast<std::uint64_t>(gpu::kCandidateCount) * kIndirectDrawCommandStride,
        .maximumCommandCount = gpu::kCandidateCount,
        .countBufferOffset = 0U,
        .countBufferBytes = sizeof(std::uint32_t),
    };
    if (auto validation = ValidateIndirectExecutionLayout(layout); !validation)
    {
        return std::unexpected(
            lgp::framework::MakeError("CreateCommandSignature", "The Chapter 13 indirect layout validation failed."));
    }

    HRESULT const createResult = deviceResources_->device()->CreateCommandSignature(
        &description, graphicsRootSignature_.Get(), IID_PPV_ARGS(commandSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandSignature", createResult,
                                                                "Failed to create the Chapter 13 command signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateCandidateResources()
{
    return {};
}

lgp::framework::Status Renderer::CreateFrameSlotResources()
{
    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlotResources &slot : frameSlots_)
    {
        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorsPerSlot);
        if (!descriptors)
        {
            return std::unexpected(std::move(descriptors.error()));
        }
        slot.descriptors = *descriptors;

        auto flags = gpu::CreateBuffer(*deviceResources_->device(), gpu::kCandidateCount * sizeof(std::uint32_t),
                                       D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                       L"Ch13 Flags Buffer");
        auto candidateBuffer = gpu::CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(gpu::kCandidateCount) * sizeof(gpu::CandidateData),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch13 Solution Candidate Buffer", true);
        auto indices = gpu::CreateBuffer(*deviceResources_->device(), gpu::kCandidateCount * sizeof(std::uint32_t),
                                         D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                         L"Ch13 Emitted Indices Buffer");
        auto commands = gpu::CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(gpu::kCandidateCount) * kIndirectDrawCommandStride,
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch13 Indirect Commands Buffer");
        auto count = gpu::CreateBuffer(*deviceResources_->device(), sizeof(std::uint32_t), D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch13 Indirect Count Buffer");
        auto statistics =
            gpu::CreateBuffer(*deviceResources_->device(), 4U * sizeof(std::uint32_t), D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch13 Statistics Buffer");
        auto indicesReadback =
            gpu::CreateBuffer(*deviceResources_->device(), gpu::kCandidateCount * sizeof(std::uint32_t),
                              D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, L"Ch13 Indices Readback", true);
        auto commandsReadback = gpu::CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(gpu::kCandidateCount) * kIndirectDrawCommandStride,
            D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, L"Ch13 Commands Readback", true);
        auto countReadback =
            gpu::CreateBuffer(*deviceResources_->device(), sizeof(std::uint32_t), D3D12_HEAP_TYPE_READBACK,
                              D3D12_RESOURCE_FLAG_NONE, L"Ch13 Count Readback", true);
        auto statisticsReadback =
            gpu::CreateBuffer(*deviceResources_->device(), 4U * sizeof(std::uint32_t), D3D12_HEAP_TYPE_READBACK,
                              D3D12_RESOURCE_FLAG_NONE, L"Ch13 Statistics Readback", true);

        if (!flags)
        {
            return std::unexpected(std::move(flags.error()));
        }
        if (!candidateBuffer)
        {
            return std::unexpected(std::move(candidateBuffer.error()));
        }
        if (!indices)
        {
            return std::unexpected(std::move(indices.error()));
        }
        if (!commands)
        {
            return std::unexpected(std::move(commands.error()));
        }
        if (!count)
        {
            return std::unexpected(std::move(count.error()));
        }
        if (!statistics)
        {
            return std::unexpected(std::move(statistics.error()));
        }
        if (!indicesReadback)
        {
            return std::unexpected(std::move(indicesReadback.error()));
        }
        if (!commandsReadback)
        {
            return std::unexpected(std::move(commandsReadback.error()));
        }
        if (!countReadback)
        {
            return std::unexpected(std::move(countReadback.error()));
        }
        if (!statisticsReadback)
        {
            return std::unexpected(std::move(statisticsReadback.error()));
        }

        slot.candidateBuffer = std::move(*candidateBuffer);
        slot.flags = std::move(*flags);
        slot.emittedIndices = std::move(*indices);
        slot.indirectCommands = std::move(*commands);
        slot.indirectCount = std::move(*count);
        slot.statistics = std::move(*statistics);
        slot.emittedIndicesReadback = std::move(*indicesReadback);
        slot.indirectCommandsReadback = std::move(*commandsReadback);
        slot.indirectCountReadback = std::move(*countReadback);
        slot.statisticsReadback = std::move(*statisticsReadback);

        D3D12_SHADER_RESOURCE_VIEW_DESC candidateSrv{};
        candidateSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        candidateSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        candidateSrv.Format = DXGI_FORMAT_UNKNOWN;
        candidateSrv.Buffer.FirstElement = 0U;
        candidateSrv.Buffer.NumElements = gpu::kCandidateCount;
        candidateSrv.Buffer.StructureByteStride = sizeof(gpu::CandidateData);
        deviceResources_->device()->CreateShaderResourceView(slot.candidateBuffer.Get(), &candidateSrv,
                                                             slot.descriptors.CpuHandle(CandidateSrv));

        D3D12_SHADER_RESOURCE_VIEW_DESC flagsSrv{};
        flagsSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        flagsSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        flagsSrv.Format = DXGI_FORMAT_UNKNOWN;
        flagsSrv.Buffer.FirstElement = 0U;
        flagsSrv.Buffer.NumElements = gpu::kCandidateCount;
        flagsSrv.Buffer.StructureByteStride = sizeof(std::uint32_t);
        deviceResources_->device()->CreateShaderResourceView(slot.flags.Get(), &flagsSrv,
                                                             slot.descriptors.CpuHandle(FlagsSrv));

        D3D12_UNORDERED_ACCESS_VIEW_DESC structuredUav{};
        structuredUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        structuredUav.Format = DXGI_FORMAT_UNKNOWN;
        structuredUav.Buffer.FirstElement = 0U;
        structuredUav.Buffer.NumElements = gpu::kCandidateCount;
        structuredUav.Buffer.StructureByteStride = sizeof(std::uint32_t);
        deviceResources_->device()->CreateUnorderedAccessView(slot.flags.Get(), nullptr, &structuredUav,
                                                              slot.descriptors.CpuHandle(FlagsUav));
        deviceResources_->device()->CreateUnorderedAccessView(slot.emittedIndices.Get(), nullptr, &structuredUav,
                                                              slot.descriptors.CpuHandle(EmittedIndicesUav));

        D3D12_UNORDERED_ACCESS_VIEW_DESC commandsUav{};
        commandsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        commandsUav.Format = DXGI_FORMAT_R32_TYPELESS;
        commandsUav.Buffer.FirstElement = 0U;
        commandsUav.Buffer.NumElements =
            static_cast<UINT>(slot.indirectCommands.size_in_bytes() / sizeof(std::uint32_t));
        commandsUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        deviceResources_->device()->CreateUnorderedAccessView(slot.indirectCommands.Get(), nullptr, &commandsUav,
                                                              slot.descriptors.CpuHandle(IndirectCommandsUav));

        D3D12_UNORDERED_ACCESS_VIEW_DESC countUav{};
        countUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        countUav.Format = DXGI_FORMAT_UNKNOWN;
        countUav.Buffer.FirstElement = 0U;
        countUav.Buffer.NumElements = 1U;
        countUav.Buffer.StructureByteStride = sizeof(std::uint32_t);
        deviceResources_->device()->CreateUnorderedAccessView(slot.indirectCount.Get(), nullptr, &countUav,
                                                              slot.descriptors.CpuHandle(IndirectCountUav));

        D3D12_UNORDERED_ACCESS_VIEW_DESC statisticsUav{};
        statisticsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        statisticsUav.Format = DXGI_FORMAT_UNKNOWN;
        statisticsUav.Buffer.FirstElement = 0U;
        statisticsUav.Buffer.NumElements = 4U;
        statisticsUav.Buffer.StructureByteStride = sizeof(std::uint32_t);
        deviceResources_->device()->CreateUnorderedAccessView(slot.statistics.Get(), nullptr, &statisticsUav,
                                                              slot.descriptors.CpuHandle(StatisticsUav));
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
            headlessConfiguration_->mode,
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
    if (auto status = CreateComputeRootSignature(); !status)
    {
        return status;
    }
    if (auto status = CreatePipelines(); !status)
    {
        return status;
    }
    if (auto status = CreateCommandSignature(); !status)
    {
        return status;
    }
    if (auto status = CreateCandidateResources(); !status)
    {
        return status;
    }
    return CreateFrameSlotResources();
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

    gpu::LabConfiguration const configuration = ActiveConfiguration();
    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    auto const writeStatus =
        gpu::WriteBuffer(slot.candidateBuffer, std::span<gpu::CandidateData const>{currentReference_.candidates});
    if (!writeStatus)
    {
        return writeStatus;
    }
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);

    std::vector<D3D12_BUFFER_BARRIER> bufferBarriers{};
    gpu::BufferBarrierState const writableBefore =
        slot.writableStateInitialized ? gpu::ComputeUnorderedAccessState() : gpu::NoAccessState();
    bufferBarriers.push_back(
        gpu::MakeBufferBarrier(*slot.flags.Get(), writableBefore, gpu::ComputeUnorderedAccessState()));
    bufferBarriers.push_back(
        gpu::MakeBufferBarrier(*slot.emittedIndices.Get(), writableBefore, gpu::ComputeUnorderedAccessState()));
    bufferBarriers.push_back(
        gpu::MakeBufferBarrier(*slot.indirectCommands.Get(), writableBefore, gpu::ComputeUnorderedAccessState()));
    bufferBarriers.push_back(
        gpu::MakeBufferBarrier(*slot.indirectCount.Get(), writableBefore, gpu::ComputeUnorderedAccessState()));
    bufferBarriers.push_back(
        gpu::MakeBufferBarrier(*slot.statistics.Get(), writableBefore, gpu::ComputeUnorderedAccessState()));
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);

    DispatchConstants const dispatchConstants{configuration.capacity, gpu::kCandidateCount, gpu::kVertexCountPerQuad,
                                              0U};

    commandList.SetComputeRootSignature(computeRootSignature_.Get());
    commandList.SetComputeRoot32BitConstants(DispatchRootConstants, sizeof(DispatchConstants) / sizeof(std::uint32_t),
                                             &dispatchConstants, 0U);
    commandList.SetComputeRootDescriptorTable(ComputeSrvTable, slot.descriptors.GpuHandle(CandidateSrv));
    commandList.SetComputeRootDescriptorTable(ComputeUavTable, slot.descriptors.GpuHandle(FlagsUav));

    commandList.SetPipelineState(resetPipeline_.Get());
    commandList.Dispatch(1U, 1U, 1U);

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.flags.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.emittedIndices.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.indirectCommands.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.indirectCount.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.statistics.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);

    commandList.SetPipelineState(classifyPipeline_.Get());
    commandList.Dispatch(1U, 1U, 1U);

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.flags.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeShaderResourceState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);

    commandList.SetPipelineState((configuration.mode == gpu::ExecutionMode::Stable) ? stablePipeline_.Get()
                                                                                    : atomicPipeline_.Get());
    commandList.Dispatch(1U, 1U, 1U);

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.indirectCommands.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ExecuteIndirectState()),
        gpu::MakeBufferBarrier(*slot.indirectCount.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ExecuteIndirectState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);

    std::vector<D3D12_TEXTURE_BARRIER> textureBarriers{
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::FrameStartState(frameContext),
                                gpu::RenderTargetState()),
    };
    gpu::SubmitTextureBarriers(commandList, textureBarriers);

    float const clearColor[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    commandList.SetGraphicsRootDescriptorTable(CandidateSrvTable, slot.descriptors.GpuHandle(CandidateSrv));
    commandList.SetPipelineState(graphicsPipeline_.Get());
    commandList.ExecuteIndirect(commandSignature_.Get(), configuration.capacity, slot.indirectCommands.Get(), 0U,
                                slot.indirectCount.Get(), 0U);

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.emittedIndices.Get(), gpu::ComputeUnorderedAccessState(), gpu::CopySourceState()),
        gpu::MakeBufferBarrier(*slot.indirectCommands.Get(), gpu::ExecuteIndirectState(), gpu::CopySourceState()),
        gpu::MakeBufferBarrier(*slot.indirectCount.Get(), gpu::ExecuteIndirectState(), gpu::CopySourceState()),
        gpu::MakeBufferBarrier(*slot.statistics.Get(), gpu::ComputeUnorderedAccessState(), gpu::CopySourceState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);

    commandList.CopyBufferRegion(slot.emittedIndicesReadback.Get(), 0U, slot.emittedIndices.Get(), 0U,
                                 slot.emittedIndices.size_in_bytes());
    commandList.CopyBufferRegion(slot.indirectCommandsReadback.Get(), 0U, slot.indirectCommands.Get(), 0U,
                                 slot.indirectCommands.size_in_bytes());
    commandList.CopyBufferRegion(slot.indirectCountReadback.Get(), 0U, slot.indirectCount.Get(), 0U,
                                 slot.indirectCount.size_in_bytes());
    commandList.CopyBufferRegion(slot.statisticsReadback.Get(), 0U, slot.statistics.Get(), 0U,
                                 slot.statistics.size_in_bytes());

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.flags.Get(), gpu::ComputeShaderResourceState(),
                               gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.emittedIndices.Get(), gpu::CopySourceState(), gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.indirectCommands.Get(), gpu::CopySourceState(),
                               gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.indirectCount.Get(), gpu::CopySourceState(), gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.statistics.Get(), gpu::CopySourceState(), gpu::ComputeUnorderedAccessState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);

    textureBarriers = {
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(), gpu::FrameEndState(frameContext)),
    };
    gpu::SubmitTextureBarriers(commandList, textureBarriers);
    slot.writableStateInitialized = true;
    slot.writableStateInitialized = true;
    lastRenderedFrameSlot_ = frameContext.frameSlot;
    return {};
}

void Renderer::DestroyFrameSlotResources(lgp::framework::DeviceResources &deviceResources) noexcept
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
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    DestroyFrameSlotResources(deviceResources);
    commandSignature_.Reset();
    graphicsPipeline_.Reset();
    atomicPipeline_.Reset();
    stablePipeline_.Reset();
    classifyPipeline_.Reset();
    resetPipeline_.Reset();
    computeRootSignature_.Reset();
    graphicsRootSignature_.Reset();
    resetShader_ = {};
    classifyShader_ = {};
    stableShader_ = {};
    atomicShader_ = {};
    vertexShader_ = {};
    pixelShader_ = {};
    deviceResources_ = nullptr;
}

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

std::expected<Renderer::ReadbackOutputs, lgp::framework::Error> Renderer::ReadBackOutputs()
{
    if (frameSlots_.empty())
    {
        return std::unexpected(lgp::framework::MakeError("ReadBackOutputs", "No Chapter 13 frame has been rendered."));
    }
    auto const idle = deviceResources_->WaitForGpuIdle();
    if (!idle)
    {
        return std::unexpected(std::move(idle.error()));
    }

    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    ReadbackOutputs outputs{};
    outputs.mode = ActiveConfiguration().mode;

    auto const *statisticsValues = reinterpret_cast<std::uint32_t const *>(slot.statisticsReadback.mapped_data());
    auto const *countValue = reinterpret_cast<std::uint32_t const *>(slot.indirectCountReadback.mapped_data());
    if (statisticsValues == nullptr || countValue == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "The Chapter 13 readback buffers are not mapped."));
    }

    outputs.statistics = {
        statisticsValues[0],
        statisticsValues[1],
        statisticsValues[2],
        statisticsValues[3],
    };
    outputs.gpuCount = countValue[0];
    outputs.executionCount = ResolveExecutionCount(ActiveConfiguration().capacity, outputs.gpuCount);

    auto const *indices = reinterpret_cast<std::uint32_t const *>(slot.emittedIndicesReadback.mapped_data());
    outputs.emittedCandidateIndices.assign(indices, indices + outputs.statistics.emittedCount);

    outputs.indirectCommands.resize(outputs.statistics.emittedCount);
    std::memcpy(outputs.indirectCommands.data(), slot.indirectCommandsReadback.mapped_data(),
                static_cast<std::size_t>(outputs.statistics.emittedCount) * sizeof(IndirectDrawCommand));
    return outputs;
}

} // namespace ch13::work_distribution::solution
