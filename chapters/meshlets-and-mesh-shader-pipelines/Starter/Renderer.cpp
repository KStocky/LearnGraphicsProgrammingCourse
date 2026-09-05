#include "Renderer.hpp"

#include <array>
#include <filesystem>
#include <string>

namespace ch22::meshlets::starter
{
namespace
{

using Microsoft::WRL::ComPtr;

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "MeshletLab.hlsl";
}

[[nodiscard]] std::filesystem::path CommonShaderDirectory()
{
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "Common";
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    return blob == nullptr ? std::string{}
                           : std::string{static_cast<char const *>(blob->GetBufferPointer()), blob->GetBufferSize()};
}

[[nodiscard]] lgp::framework::Status CreateRootSignature(ID3D12Device10 &device,
                                                         ComPtr<ID3D12RootSignature> &rootSignature)
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1U;
    range.BaseShaderRegister = 0U;

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1U;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 1U;
    description.pParameters = &parameter;
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
            "ID3D12Device::CreateRootSignature", create, "Failed to create the Chapter 22 Starter root signature."));
    }
    return {};
}

} // namespace

lgp::framework::Status Renderer::CreateDeviceObjects()
{
    auto scene = gpu::BuildGpuScene();
    if (!scene)
    {
        return std::unexpected(std::move(scene.error()));
    }
    scene_ = std::move(*scene);

    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    lgp::framework::ShaderCompiler compiler = std::move(*compilerResult);
    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = ShaderPath();
    options.includeDirectories = {CommonShaderDirectory()};
    if (auto status = gpu::CompileShader(compiler, options, L"ClassicVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"ScenePS", L"ps_6_0", pixelShader_); !status)
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
                                             "Failed to create the Chapter 22 Starter graphics pipeline."));
    }

    std::uint64_t const vertexBytes = scene_.classicVertices.size() * sizeof(gpu::SceneVertex);
    std::uint64_t const indexBytes = scene_.classicIndices.size() * sizeof(std::uint32_t);
    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlot &slot : frameSlots_)
    {
        auto vertices = gpu::CreateBuffer(*deviceResources_->device(), vertexBytes, D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_FLAG_NONE, L"Ch22 Starter Vertices", true);
        auto indices = gpu::CreateBuffer(*deviceResources_->device(), indexBytes, D3D12_HEAP_TYPE_UPLOAD,
                                         D3D12_RESOURCE_FLAG_NONE, L"Ch22 Starter Indices", true);
        auto descriptor = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(1U);
        if (!vertices || !indices || !descriptor)
        {
            return std::unexpected(
                lgp::framework::MakeError("CreateDeviceObjects", "Failed to allocate Chapter 22 Starter resources."));
        }
        slot.vertices = std::move(*vertices);
        slot.indices = std::move(*indices);
        slot.descriptor = *descriptor;
        if (auto status = gpu::WriteBuffer(slot.indices, std::span<std::uint32_t const>{scene_.classicIndices});
            !status)
        {
            return status;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = static_cast<UINT>(scene_.classicVertices.size());
        srv.Buffer.StructureByteStride = sizeof(gpu::SceneVertex);
        deviceResources_->device()->CreateShaderResourceView(slot.vertices.Get(), &srv, slot.descriptor.cpuHandle);
    }
    return {};
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
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
    auto scene = gpu::BuildGpuScene();
    if (!scene)
    {
        return std::unexpected(std::move(scene.error()));
    }
    scene_ = std::move(*scene);
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    FrameSlot &slot = frameSlots_.at(frameContext.frameSlot);
    if (auto status = gpu::WriteBuffer(slot.vertices, std::span<gpu::SceneVertex const>{scene_.classicVertices});
        !status)
    {
        return status;
    }
    if (auto status = gpu::WriteBuffer(slot.indices, std::span<std::uint32_t const>{scene_.classicIndices}); !status)
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
    commandList.SetGraphicsRootDescriptorTable(0U, slot.descriptor.gpuHandle);
    commandList.DrawIndexedInstanced(static_cast<UINT>(scene_.classicIndices.size()), 1U, 0U, 0, 0U);

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
    (void)configuration;
}

gpu::GpuScene const &Renderer::LastScene() const noexcept
{
    return scene_;
}

} // namespace ch22::meshlets::starter
