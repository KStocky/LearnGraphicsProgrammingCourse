#include "Renderer.hpp"

#include <array>
#include <filesystem>
#include <string>

namespace ch21::gpu_driven::starter
{
namespace
{

using Microsoft::WRL::ComPtr;

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "GpuDrivenLab.hlsl";
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), blob->GetBufferSize()};
}

[[nodiscard]] lgp::framework::Status CreateRootSignature(ID3D12Device10 &device,
                                                         ComPtr<ID3D12RootSignature> &rootSignature)
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1U;
    range.BaseShaderRegister = 0U;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0U;
    parameters[0].Constants.Num32BitValues = 3U;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
    description.pParameters = parameters;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
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
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device::CreateRootSignature", create, "Failed to create the Chapter 21 Starter root signature."));
    }
    return {};
}

} // namespace

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    return headless_ && headlessConfiguration_.has_value() ? *headlessConfiguration_ : gpu::LabConfiguration{};
}

lgp::framework::Status Renderer::CreateDeviceObjects()
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
    if (auto status = gpu::CompileShader(compiler, options, L"IndirectVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"ColorPS", L"ps_6_0", pixelShader_); !status)
    {
        return status;
    }
    if (auto status = CreateRootSignature(*deviceResources_->device(), rootSignature_); !status)
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
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = rootSignature_.Get();
    pipelineDescription.VS = vertexShader_.Bytecode();
    pipelineDescription.PS = pixelShader_.Bytecode();
    pipelineDescription.BlendState = blend;
    pipelineDescription.SampleMask = UINT_MAX;
    pipelineDescription.RasterizerState = rasterizer;
    pipelineDescription.DepthStencilState = depth;
    pipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDescription.NumRenderTargets = 1U;
    pipelineDescription.RTVFormats[0] = deviceResources_->back_buffer_format();
    pipelineDescription.SampleDesc.Count = 1U;
    HRESULT const pipelineResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &pipelineDescription, IID_PPV_ARGS(pipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(pipelineResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", pipelineResult,
                                             "Failed to create the Chapter 21 Starter graphics pipeline."));
    }

    std::array<std::uint32_t, 12U> const indices{{0U, 1U, 2U, 2U, 1U, 3U, 0U, 1U, 2U, 0U, 2U, 3U}};
    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlot &slot : frameSlots_)
    {
        auto instances =
            gpu::CreateBuffer(*deviceResources_->device(), gpu::kMaximumInstanceCount * sizeof(gpu::GpuInstance),
                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch21 Starter Instances", true);
        auto indexBuffer = gpu::CreateBuffer(*deviceResources_->device(), sizeof(indices), D3D12_HEAP_TYPE_UPLOAD,
                                             D3D12_RESOURCE_FLAG_NONE, L"Ch21 Starter Indices", true);
        auto descriptor = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(1U);
        if (!instances)
        {
            return std::unexpected(std::move(instances.error()));
        }
        if (!indexBuffer)
        {
            return std::unexpected(std::move(indexBuffer.error()));
        }
        if (!descriptor)
        {
            return std::unexpected(std::move(descriptor.error()));
        }
        slot.instances = std::move(*instances);
        slot.indices = std::move(*indexBuffer);
        slot.descriptor = *descriptor;
        if (auto status =
                gpu::WriteBuffer(slot.indices, std::span<std::uint32_t const>{indices.data(), indices.size()});
            !status)
        {
            return status;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = gpu::kMaximumInstanceCount;
        srv.Buffer.StructureByteStride = sizeof(gpu::GpuInstance);
        deviceResources_->device()->CreateShaderResourceView(slot.instances.Get(), &srv, slot.descriptor.cpuHandle);
    }
    return {};
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    return CreateDeviceObjects();
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
    if (auto status = gpu::WriteBuffer(slot.instances, std::span<gpu::GpuInstance const>{reference_.gpuInstances});
        !status)
    {
        return status;
    }

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    std::vector<D3D12_TEXTURE_BARRIER> barriers{gpu::MakeTextureBarrier(
        *frameContext.renderTarget, gpu::FrameStartState(frameContext), gpu::RenderTargetState())};
    gpu::SubmitTextureBarriers(commandList, barriers);
    float const clear[]{0.01F, 0.015F, 0.025F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clear, 0U, nullptr);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_INDEX_BUFFER_VIEW const indexView{slot.indices.Get()->GetGPUVirtualAddress(),
                                            static_cast<UINT>(slot.indices.size_in_bytes()), DXGI_FORMAT_R32_UINT};
    commandList.IASetIndexBuffer(&indexView);
    commandList.SetGraphicsRootSignature(rootSignature_.Get());
    commandList.SetPipelineState(pipeline_.Get());
    ID3D12DescriptorHeap *heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.SetGraphicsRootDescriptorTable(1U, slot.descriptor.gpuHandle);
    for (IndirectCommand const &command : reference_.commands)
    {
        std::array<std::uint32_t, 3U> const constants{command.stableId, command.lod,
                                                      command.draw.startInstanceLocation};
        commandList.SetGraphicsRoot32BitConstants(0U, static_cast<UINT>(constants.size()), constants.data(), 0U);
        commandList.DrawIndexedInstanced(command.draw.indexCountPerInstance, command.draw.instanceCount,
                                         command.draw.startIndexLocation, command.draw.baseVertexLocation,
                                         command.draw.startInstanceLocation);
    }
    barriers = {gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(),
                                        gpu::FrameEndState(frameContext))};
    gpu::SubmitTextureBarriers(commandList, barriers);
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    for (FrameSlot &slot : frameSlots_)
    {
        if (slot.descriptor)
        {
            deviceResources.shader_visible_cbv_srv_uav_heap().Free(slot.descriptor);
        }
    }
    frameSlots_.clear();
    pipeline_.Reset();
    rootSignature_.Reset();
    deviceResources_ = nullptr;
}

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

gpu::CpuReference const &Renderer::LastReference() const noexcept
{
    return reference_;
}

} // namespace ch21::gpu_driven::starter
