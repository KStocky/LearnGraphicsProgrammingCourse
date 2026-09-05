#include "Renderer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>

namespace ch21::gpu_driven::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorCount = 4U;

struct DispatchConstants final
{
    std::uint32_t instanceCount{};
    std::uint32_t capacity{};
    std::uint32_t viewportHeight{gpu::kViewportHeight};
    float projectionScale{gpu::kProjectionScale};
    float nearPlane{gpu::kNearPlane};
    std::array<std::uint32_t, 3U> reserved{};
};

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "GpuDrivenLab.hlsl";
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    return blob == nullptr ? std::string{}
                           : std::string{static_cast<char const *>(blob->GetBufferPointer()), blob->GetBufferSize()};
}

[[nodiscard]] lgp::framework::Status SerializeAndCreate(ID3D12Device10 &device,
                                                        D3D12_ROOT_SIGNATURE_DESC const &description,
                                                        ComPtr<ID3D12RootSignature> &rootSignature)
{
    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};
    HRESULT const serialize =
        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, serialized.ReleaseAndGetAddressOf(),
                                    errors.ReleaseAndGetAddressOf());
    if (FAILED(serialize))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serialize, BlobText(errors.Get())));
    }
    HRESULT const create = device.CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                      IID_PPV_ARGS(rootSignature.ReleaseAndGetAddressOf()));
    if (FAILED(create))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", create,
                                                                "Failed to create a Chapter 21 root signature."));
    }
    return {};
}

[[nodiscard]] lgp::framework::Status CreateComputePipeline(ID3D12Device10 &device, ID3D12RootSignature &rootSignature,
                                                           lgp::framework::CompiledShader const &shader,
                                                           ComPtr<ID3D12PipelineState> &pipeline)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &rootSignature;
    description.CS = shader.Bytecode();
    HRESULT const result =
        device.CreateComputePipelineState(&description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState", result,
                                                                "Failed to create a Chapter 21 compute pipeline."));
    }
    return {};
}

} // namespace

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    gpu::LabConfiguration configuration =
        headless_ && headlessConfiguration_.has_value() ? *headlessConfiguration_ : gpu::LabConfiguration{};
    configuration.capacity = gpu::NormalizeCapacity(configuration.capacity);
    return configuration;
}

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
    if (auto status = gpu::CompileShader(compiler, options, L"ResetCS", L"cs_6_0", resetShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"CullAndBuildCS", L"cs_6_0", cullShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"IndirectVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    return gpu::CompileShader(compiler, options, L"ColorPS", L"ps_6_0", pixelShader_);
}

lgp::framework::Status Renderer::CreateRootSignatures()
{
    D3D12_DESCRIPTOR_RANGE graphicsRange{};
    graphicsRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    graphicsRange.NumDescriptors = 1U;
    D3D12_ROOT_PARAMETER graphicsParameters[2]{};
    graphicsParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[0].Constants.ShaderRegister = 0U;
    graphicsParameters[0].Constants.Num32BitValues = 3U;
    graphicsParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    graphicsParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParameters[1].DescriptorTable.NumDescriptorRanges = 1U;
    graphicsParameters[1].DescriptorTable.pDescriptorRanges = &graphicsRange;
    graphicsParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC graphicsDescription{};
    graphicsDescription.NumParameters = static_cast<UINT>(std::size(graphicsParameters));
    graphicsDescription.pParameters = graphicsParameters;
    graphicsDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    if (auto status = SerializeAndCreate(*deviceResources_->device(), graphicsDescription, graphicsRootSignature_);
        !status)
    {
        return status;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 2U;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 2U;
    D3D12_ROOT_PARAMETER computeParameters[3]{};
    computeParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[0].Constants.ShaderRegister = 0U;
    computeParameters[0].Constants.Num32BitValues = sizeof(DispatchConstants) / sizeof(std::uint32_t);
    computeParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[1].DescriptorTable.NumDescriptorRanges = 1U;
    computeParameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
    computeParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[2].DescriptorTable.NumDescriptorRanges = 1U;
    computeParameters[2].DescriptorTable.pDescriptorRanges = &uavRange;
    D3D12_ROOT_SIGNATURE_DESC computeDescription{};
    computeDescription.NumParameters = static_cast<UINT>(std::size(computeParameters));
    computeDescription.pParameters = computeParameters;
    return SerializeAndCreate(*deviceResources_->device(), computeDescription, computeRootSignature_);
}

lgp::framework::Status Renderer::CreatePipelines()
{
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), resetShader_,
                                            resetPipeline_);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), cullShader_,
                                            cullPipeline_);
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
    HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(graphicsPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create the Chapter 21 graphics pipeline."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateCommandSignature()
{
    D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arguments[0].Constant.RootParameterIndex = 0U;
    arguments[0].Constant.Num32BitValuesToSet = 3U;
    arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC description{};
    description.ByteStride = gpu::kIndirectCommandStride;
    description.NumArgumentDescs = static_cast<UINT>(std::size(arguments));
    description.pArgumentDescs = arguments;
    HRESULT const result = deviceResources_->device()->CreateCommandSignature(
        &description, graphicsRootSignature_.Get(), IID_PPV_ARGS(commandSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandSignature", result,
                                                                "Failed to create the Chapter 21 command signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateFrameSlots()
{
    std::array<std::uint32_t, 12U> const indices{{0U, 1U, 2U, 2U, 1U, 3U, 0U, 1U, 2U, 0U, 2U, 3U}};
    std::uint64_t const commandBytes =
        static_cast<std::uint64_t>(gpu::kMaximumInstanceCount + 1U) * gpu::kIndirectCommandStride;
    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlot &slot : frameSlots_)
    {
        auto instances =
            gpu::CreateBuffer(*deviceResources_->device(), gpu::kMaximumInstanceCount * sizeof(gpu::GpuInstance),
                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch21 Instances", true);
        auto templates =
            gpu::CreateBuffer(*deviceResources_->device(), gpu::kDrawTemplateCount * sizeof(gpu::GpuDrawTemplate),
                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch21 Templates", true);
        auto indexBuffer = gpu::CreateBuffer(*deviceResources_->device(), sizeof(indices), D3D12_HEAP_TYPE_UPLOAD,
                                             D3D12_RESOURCE_FLAG_NONE, L"Ch21 Indices", true);
        auto commands = gpu::CreateBuffer(*deviceResources_->device(), commandBytes, D3D12_HEAP_TYPE_DEFAULT,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch21 Commands");
        auto count = gpu::CreateBuffer(*deviceResources_->device(), sizeof(std::uint32_t), D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch21 Count");
        auto commandsReadback = gpu::CreateBuffer(*deviceResources_->device(), commandBytes, D3D12_HEAP_TYPE_READBACK,
                                                  D3D12_RESOURCE_FLAG_NONE, L"Ch21 Commands Readback", true);
        auto countReadback =
            gpu::CreateBuffer(*deviceResources_->device(), sizeof(std::uint32_t), D3D12_HEAP_TYPE_READBACK,
                              D3D12_RESOURCE_FLAG_NONE, L"Ch21 Count Readback", true);
        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorCount);
        if (!instances || !templates || !indexBuffer || !commands || !count || !commandsReadback || !countReadback ||
            !descriptors)
        {
            return std::unexpected(
                lgp::framework::MakeError("CreateFrameSlots", "Failed to allocate Chapter 21 frame resources."));
        }
        slot.instances = std::move(*instances);
        slot.templates = std::move(*templates);
        slot.indices = std::move(*indexBuffer);
        slot.commands = std::move(*commands);
        slot.count = std::move(*count);
        slot.commandsReadback = std::move(*commandsReadback);
        slot.countReadback = std::move(*countReadback);
        slot.descriptors = *descriptors;
        if (auto status =
                gpu::WriteBuffer(slot.indices, std::span<std::uint32_t const>{indices.data(), indices.size()});
            !status)
        {
            return status;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC instanceSrv{};
        instanceSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        instanceSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        instanceSrv.Buffer.NumElements = gpu::kMaximumInstanceCount;
        instanceSrv.Buffer.StructureByteStride = sizeof(gpu::GpuInstance);
        deviceResources_->device()->CreateShaderResourceView(slot.instances.Get(), &instanceSrv,
                                                             slot.descriptors.CpuHandle(0U));
        D3D12_SHADER_RESOURCE_VIEW_DESC templateSrv = instanceSrv;
        templateSrv.Buffer.NumElements = gpu::kDrawTemplateCount;
        templateSrv.Buffer.StructureByteStride = sizeof(gpu::GpuDrawTemplate);
        deviceResources_->device()->CreateShaderResourceView(slot.templates.Get(), &templateSrv,
                                                             slot.descriptors.CpuHandle(1U));
        D3D12_UNORDERED_ACCESS_VIEW_DESC commandUav{};
        commandUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        commandUav.Format = DXGI_FORMAT_R32_TYPELESS;
        commandUav.Buffer.NumElements = static_cast<UINT>(commandBytes / sizeof(std::uint32_t));
        commandUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        deviceResources_->device()->CreateUnorderedAccessView(slot.commands.Get(), nullptr, &commandUav,
                                                              slot.descriptors.CpuHandle(2U));
        D3D12_UNORDERED_ACCESS_VIEW_DESC countUav{};
        countUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        countUav.Buffer.NumElements = 1U;
        countUav.Buffer.StructureByteStride = sizeof(std::uint32_t);
        deviceResources_->device()->CreateUnorderedAccessView(slot.count.Get(), nullptr, &countUav,
                                                              slot.descriptors.CpuHandle(3U));
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
    if (auto status = CreateRootSignatures(); !status)
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
    return CreateFrameSlots();
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
    reference_ = std::move(*reference);
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    FrameSlot &slot = frameSlots_.at(frameContext.frameSlot);
    std::array<gpu::GpuDrawTemplate, gpu::kDrawTemplateCount> gpuTemplates{};
    for (std::size_t index = 0U; index < gpuTemplates.size(); ++index)
    {
        DrawTemplate const &source = reference_.drawTemplates[index];
        gpuTemplates[index] = {source.indexCount, source.startIndex, source.baseVertex, source.materialIndex};
    }
    if (auto status = gpu::WriteBuffer(slot.instances, std::span<gpu::GpuInstance const>{reference_.gpuInstances});
        !status)
    {
        return status;
    }
    if (auto status = gpu::WriteBuffer(slot.templates,
                                       std::span<gpu::GpuDrawTemplate const>{gpuTemplates.data(), gpuTemplates.size()});
        !status)
    {
        return status;
    }

    gpu::LabConfiguration const configuration = ActiveConfiguration();
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    gpu::BufferBarrierState const before =
        slot.writableStateInitialized ? gpu::ComputeUnorderedAccessState() : gpu::NoAccessState();
    std::vector<D3D12_BUFFER_BARRIER> bufferBarriers{
        gpu::MakeBufferBarrier(*slot.commands.Get(), before, gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.count.Get(), before, gpu::ComputeUnorderedAccessState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);

    DispatchConstants const constants{static_cast<std::uint32_t>(reference_.gpuInstances.size()),
                                      configuration.capacity};
    commandList.SetComputeRootSignature(computeRootSignature_.Get());
    commandList.SetComputeRoot32BitConstants(0U, sizeof(constants) / sizeof(std::uint32_t), &constants, 0U);
    commandList.SetComputeRootDescriptorTable(1U, slot.descriptors.GpuHandle(0U));
    commandList.SetComputeRootDescriptorTable(2U, slot.descriptors.GpuHandle(2U));
    commandList.SetPipelineState(resetPipeline_.Get());
    commandList.Dispatch(1U, 1U, 1U);
    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.commands.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.count.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);
    commandList.SetPipelineState(cullPipeline_.Get());
    commandList.Dispatch(1U, 1U, 1U);
    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.commands.Get(), gpu::ComputeUnorderedAccessState(), gpu::ExecuteIndirectState()),
        gpu::MakeBufferBarrier(*slot.count.Get(), gpu::ComputeUnorderedAccessState(), gpu::ExecuteIndirectState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);

    std::vector<D3D12_TEXTURE_BARRIER> textureBarriers{gpu::MakeTextureBarrier(
        *frameContext.renderTarget, gpu::FrameStartState(frameContext), gpu::RenderTargetState())};
    gpu::SubmitTextureBarriers(commandList, textureBarriers);
    float const clear[]{0.01F, 0.015F, 0.025F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clear, 0U, nullptr);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_INDEX_BUFFER_VIEW const indexView{slot.indices.Get()->GetGPUVirtualAddress(),
                                            static_cast<UINT>(slot.indices.size_in_bytes()), DXGI_FORMAT_R32_UINT};
    commandList.IASetIndexBuffer(&indexView);
    commandList.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    commandList.SetGraphicsRootDescriptorTable(1U, slot.descriptors.GpuHandle(0U));
    commandList.SetPipelineState(graphicsPipeline_.Get());
    commandList.ExecuteIndirect(commandSignature_.Get(), configuration.capacity, slot.commands.Get(), 0U,
                                slot.count.Get(), 0U);

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.commands.Get(), gpu::ExecuteIndirectState(), gpu::CopySourceState()),
        gpu::MakeBufferBarrier(*slot.count.Get(), gpu::ExecuteIndirectState(), gpu::CopySourceState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);
    commandList.CopyBufferRegion(slot.commandsReadback.Get(), 0U, slot.commands.Get(), 0U,
                                 slot.commands.size_in_bytes());
    commandList.CopyBufferRegion(slot.countReadback.Get(), 0U, slot.count.Get(), 0U, slot.count.size_in_bytes());
    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.commands.Get(), gpu::CopySourceState(), gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.count.Get(), gpu::CopySourceState(), gpu::ComputeUnorderedAccessState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);
    textureBarriers = {gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(),
                                               gpu::FrameEndState(frameContext))};
    gpu::SubmitTextureBarriers(commandList, textureBarriers);
    slot.writableStateInitialized = true;
    lastFrameSlot_ = frameContext.frameSlot;
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    for (FrameSlot &slot : frameSlots_)
    {
        if (slot.descriptors)
        {
            deviceResources.shader_visible_cbv_srv_uav_heap().Free(slot.descriptors);
        }
    }
    frameSlots_.clear();
    commandSignature_.Reset();
    graphicsPipeline_.Reset();
    cullPipeline_.Reset();
    resetPipeline_.Reset();
    graphicsRootSignature_.Reset();
    computeRootSignature_.Reset();
    deviceResources_ = nullptr;
}

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

std::expected<gpu::ReadbackEvidence, lgp::framework::Error> Renderer::ReadBackEvidence()
{
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }
    FrameSlot const &slot = frameSlots_.at(lastFrameSlot_);
    auto const *count = reinterpret_cast<std::uint32_t const *>(slot.countReadback.mapped_data());
    auto const *commands = reinterpret_cast<gpu::GpuIndirectCommand const *>(slot.commandsReadback.mapped_data());
    if (count == nullptr || commands == nullptr)
    {
        return std::unexpected(lgp::framework::MakeError("ReadBackEvidence", "Chapter 21 readback is not mapped."));
    }
    gpu::ReadbackEvidence evidence{};
    evidence.visibleCount = count[0];
    evidence.executedCount = std::min(evidence.visibleCount, ActiveConfiguration().capacity);
    evidence.commands.reserve(evidence.executedCount);
    for (std::uint32_t index = 0U; index < evidence.executedCount; ++index)
    {
        evidence.commands.push_back(
            gpu::ToContractCommand(commands[index], reference_.instances, reference_.drawTemplates));
    }
    std::memcpy(evidence.guard.data(), commands + ActiveConfiguration().capacity, sizeof(evidence.guard));
    return evidence;
}

gpu::CpuReference const &Renderer::LastReference() const noexcept
{
    return reference_;
}

} // namespace ch21::gpu_driven::solution
