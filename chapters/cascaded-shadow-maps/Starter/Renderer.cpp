#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

namespace ch24::cascaded_shadows::starter
{
namespace
{

using Microsoft::WRL::ComPtr;

enum ShadowRootParameter : UINT
{
    ShadowPassConstantsSlot = 0U,
};

enum LightingRootParameter : UINT
{
    LightingConstantsSlot = 0U,
    LightingShadowTableSlot = 1U,
};

[[nodiscard]] std::string BlobToUtf8(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] std::filesystem::path ResolveShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "CascadedShadowLab.hlsl";
}

[[nodiscard]] D3D12_INPUT_ELEMENT_DESC const *SceneInputLayout(UINT &count) noexcept
{
    static D3D12_INPUT_ELEMENT_DESC const layout[]{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    };
    count = static_cast<UINT>(std::size(layout));
    return layout;
}

} // namespace

lgp::framework::Status Renderer::CreateShaders()
{
    std::expected<lgp::framework::ShaderCompiler, lgp::framework::Error> compilerResult =
        lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    lgp::framework::ShaderCompiler const compiler = std::move(*compilerResult);

    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = ResolveShaderPath();
    options.includeDirectories = {options.sourcePath.parent_path()};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif

    if (auto status = gpu::CompileShaderEntry(compiler, options, L"ShadowVS", L"vs_6_0", shadowVertexShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShaderEntry(compiler, options, L"SceneVS", L"vs_6_0", sceneVertexShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShaderEntry(compiler, options, L"LightingPS", L"ps_6_0", lightingPixelShader_);
        !status)
    {
        return status;
    }
    return {};
}

lgp::framework::Status Renderer::CreateRootSignatures()
{
    D3D12_ROOT_PARAMETER shadowParameters[1]{};
    shadowParameters[ShadowPassConstantsSlot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    shadowParameters[ShadowPassConstantsSlot].Descriptor.ShaderRegister = 1U;
    shadowParameters[ShadowPassConstantsSlot].Descriptor.RegisterSpace = 0U;
    shadowParameters[ShadowPassConstantsSlot].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC shadowDescription{};
    shadowDescription.NumParameters = static_cast<UINT>(std::size(shadowParameters));
    shadowDescription.pParameters = shadowParameters;
    shadowDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                              D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                              D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                              D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
                              D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (HRESULT const result =
            D3D12SerializeRootSignature(&shadowDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                        serialized.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
        FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", result, BlobToUtf8(errors.Get())));
    }
    if (HRESULT const result = deviceResources_->device()->CreateRootSignature(
            0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(shadowRootSignature_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", result,
                                                                "Failed to create the shadow root signature."));
    }

    D3D12_DESCRIPTOR_RANGE shadowRange{};
    shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors = 1U;
    shadowRange.BaseShaderRegister = 0U;
    shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER lightingParameters[2]{};
    lightingParameters[LightingConstantsSlot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    lightingParameters[LightingConstantsSlot].Descriptor.ShaderRegister = 0U;
    lightingParameters[LightingConstantsSlot].Descriptor.RegisterSpace = 0U;
    lightingParameters[LightingConstantsSlot].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    lightingParameters[LightingShadowTableSlot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    lightingParameters[LightingShadowTableSlot].DescriptorTable.NumDescriptorRanges = 1U;
    lightingParameters[LightingShadowTableSlot].DescriptorTable.pDescriptorRanges = &shadowRange;
    lightingParameters[LightingShadowTableSlot].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC comparisonSampler{};
    comparisonSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    comparisonSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    comparisonSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    comparisonSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    comparisonSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    comparisonSampler.ShaderRegister = 0U;
    comparisonSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    comparisonSampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC lightingDescription{};
    lightingDescription.NumParameters = static_cast<UINT>(std::size(lightingParameters));
    lightingDescription.pParameters = lightingParameters;
    lightingDescription.NumStaticSamplers = 1U;
    lightingDescription.pStaticSamplers = &comparisonSampler;
    lightingDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    if (HRESULT const result =
            D3D12SerializeRootSignature(&lightingDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                        serialized.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
        FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", result, BlobToUtf8(errors.Get())));
    }
    if (HRESULT const result = deviceResources_->device()->CreateRootSignature(
            0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(lightingRootSignature_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", result,
                                                                "Failed to create the lighting root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreatePipelineStates()
{
    UINT layoutCount = 0U;
    D3D12_INPUT_ELEMENT_DESC const *layout = SceneInputLayout(layoutCount);

    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowDescription{};
    shadowDescription.pRootSignature = shadowRootSignature_.Get();
    shadowDescription.VS = shadowVertexShader_.Bytecode();
    shadowDescription.SampleMask = UINT_MAX;
    shadowDescription.RasterizerState = rasterizer;
    shadowDescription.DepthStencilState = depth;
    shadowDescription.InputLayout = {layout, layoutCount};
    shadowDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    shadowDescription.NumRenderTargets = 0U;
    shadowDescription.DSVFormat = gpu::kShadowDepthFormat;
    shadowDescription.SampleDesc.Count = 1U;
    if (HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
            &shadowDescription, IID_PPV_ARGS(shadowPipeline_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create the shadow depth pipeline."));
    }

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lightingDescription{};
    lightingDescription.pRootSignature = lightingRootSignature_.Get();
    lightingDescription.VS = sceneVertexShader_.Bytecode();
    lightingDescription.PS = lightingPixelShader_.Bytecode();
    lightingDescription.BlendState = blend;
    lightingDescription.SampleMask = UINT_MAX;
    lightingDescription.RasterizerState = rasterizer;
    lightingDescription.DepthStencilState = depth;
    lightingDescription.InputLayout = {layout, layoutCount};
    lightingDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    lightingDescription.NumRenderTargets = 1U;
    lightingDescription.RTVFormats[0] = deviceResources_->back_buffer_format();
    lightingDescription.DSVFormat = gpu::kSceneDepthFormat;
    lightingDescription.SampleDesc.Count = 1U;
    if (HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
            &lightingDescription, IID_PPV_ARGS(lightingPipeline_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create the lighting pipeline."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateSceneBuffers()
{
    gpu::MeshGeometry const scene = gpu::BuildScene();

    std::uint64_t const vertexBytes = scene.vertices.size() * sizeof(gpu::MeshVertex);
    std::expected<lgp::framework::Buffer, lgp::framework::Error> vertexBuffer =
        lgp::framework::CreateUploadBuffer(*deviceResources_->device(), vertexBytes, L"Ch24 scene vertices");
    if (!vertexBuffer)
    {
        return std::unexpected(std::move(vertexBuffer.error()));
    }
    sceneVertices_ = std::move(*vertexBuffer);
    if (auto status = lgp::framework::WriteBuffer(
            sceneVertices_, std::span<gpu::MeshVertex const>{scene.vertices.data(), scene.vertices.size()});
        !status)
    {
        return status;
    }

    std::uint64_t const indexBytes = scene.indices.size() * sizeof(std::uint32_t);
    std::expected<lgp::framework::Buffer, lgp::framework::Error> indexBuffer =
        lgp::framework::CreateUploadBuffer(*deviceResources_->device(), indexBytes, L"Ch24 scene indices");
    if (!indexBuffer)
    {
        return std::unexpected(std::move(indexBuffer.error()));
    }
    sceneIndices_ = std::move(*indexBuffer);
    if (auto status = lgp::framework::WriteBuffer(
            sceneIndices_, std::span<std::uint32_t const>{scene.indices.data(), scene.indices.size()});
        !status)
    {
        return status;
    }

    sceneVertexView_.BufferLocation = sceneVertices_.gpu_virtual_address();
    sceneVertexView_.SizeInBytes = static_cast<UINT>(vertexBytes);
    sceneVertexView_.StrideInBytes = sizeof(gpu::MeshVertex);
    sceneIndexView_.BufferLocation = sceneIndices_.gpu_virtual_address();
    sceneIndexView_.SizeInBytes = static_cast<UINT>(indexBytes);
    sceneIndexView_.Format = DXGI_FORMAT_R32_UINT;
    sceneIndexCount_ = static_cast<UINT>(scene.indices.size());
    return {};
}

lgp::framework::Status Renderer::CreateConstantBuffers()
{
    labConstantStride_ = lgp::framework::AlignConstantBufferSize(sizeof(gpu::LabConstants));
    shadowConstantStride_ = lgp::framework::AlignConstantBufferSize(sizeof(gpu::ShadowPassConstants));
    UINT const slots = std::max<UINT>(deviceResources_->back_buffer_count(), 1U);

    labConstantBuffers_.clear();
    shadowConstantBuffers_.clear();
    labConstantBuffers_.reserve(slots);
    shadowConstantBuffers_.reserve(slots);
    for (UINT slot = 0U; slot < slots; ++slot)
    {
        std::expected<lgp::framework::Buffer, lgp::framework::Error> labBuffer =
            lgp::framework::CreateUploadBuffer(*deviceResources_->device(), labConstantStride_, L"Ch24 lab constants");
        if (!labBuffer)
        {
            return std::unexpected(std::move(labBuffer.error()));
        }
        labConstantBuffers_.push_back(std::move(*labBuffer));

        std::expected<lgp::framework::Buffer, lgp::framework::Error> shadowBuffer = lgp::framework::CreateUploadBuffer(
            *deviceResources_->device(), shadowConstantStride_, L"Ch24 shadow pass constants");
        if (!shadowBuffer)
        {
            return std::unexpected(std::move(shadowBuffer.error()));
        }
        shadowConstantBuffers_.push_back(std::move(*shadowBuffer));
    }
    return {};
}

lgp::framework::Status Renderer::CreateShadowTarget()
{
    shadowTarget_.Reset();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC1 texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = gpu::kShadowResolution;
    texture.Height = gpu::kShadowResolution;
    texture.DepthOrArraySize = 1U;
    texture.MipLevels = 1U;
    texture.Format = gpu::kShadowTypelessFormat;
    texture.SampleDesc.Count = 1U;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = gpu::kShadowDepthFormat;
    clear.DepthStencil.Depth = 1.0F;
    if (HRESULT const result = deviceResources_->device()->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &clear, nullptr, 0U, nullptr,
            IID_PPV_ARGS(shadowTarget_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                                "Failed to create the shadow map."));
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = gpu::kShadowDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    deviceResources_->device()->CreateDepthStencilView(shadowTarget_.Get(), &dsv, shadowDepthView_.cpuHandle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = gpu::kShadowResourceFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1U;
    deviceResources_->device()->CreateShaderResourceView(shadowTarget_.Get(), &srv,
                                                         shadowShaderResourceView_.cpuHandle);
    return {};
}

lgp::framework::Status Renderer::CreateSizeDependentTargets(lgp::framework::Extent2D size)
{
    sceneDepthTarget_.Reset();
    if (size.empty())
    {
        return {};
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC1 texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = size.width;
    texture.Height = size.height;
    texture.DepthOrArraySize = 1U;
    texture.MipLevels = 1U;
    texture.SampleDesc.Count = 1U;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture.Format = gpu::kSceneDepthFormat;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = gpu::kSceneDepthFormat;
    depthClear.DepthStencil.Depth = 1.0F;
    if (HRESULT const result = deviceResources_->device()->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, &depthClear, nullptr, 0U,
            nullptr, IID_PPV_ARGS(sceneDepthTarget_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                                "Failed to create the scene depth target."));
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = gpu::kSceneDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    deviceResources_->device()->CreateDepthStencilView(sceneDepthTarget_.Get(), &dsv, sceneDepthView_.cpuHandle);
    return {};
}

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    // The Starter always renders a single directional shadow map, so it forces one
    // cascade regardless of the configured count. Headless callers may still change
    // debug view, light, and camera offset.
    gpu::LabConfiguration configuration =
        hasHeadlessConfiguration_ ? headlessConfiguration_ : gpu::BaselineConfiguration();
    configuration.cascadeCountOverride = 1U;
    return configuration;
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;

    std::expected<lgp::framework::DescriptorHeap, lgp::framework::Error> dsvHeap = lgp::framework::CreateDescriptorHeap(
        *deviceResources_->device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2U, false, L"Ch24 starter DSV heap");
    if (!dsvHeap)
    {
        return std::unexpected(std::move(dsvHeap.error()));
    }
    dsvHeap_ = std::move(*dsvHeap);
    std::expected<lgp::framework::DescriptorAllocation, lgp::framework::Error> sceneDepthView = dsvHeap_.Allocate(1U);
    if (!sceneDepthView)
    {
        return std::unexpected(std::move(sceneDepthView.error()));
    }
    sceneDepthView_ = *sceneDepthView;
    std::expected<lgp::framework::DescriptorAllocation, lgp::framework::Error> shadowDepthView = dsvHeap_.Allocate(1U);
    if (!shadowDepthView)
    {
        return std::unexpected(std::move(shadowDepthView.error()));
    }
    shadowDepthView_ = *shadowDepthView;

    std::expected<lgp::framework::DescriptorAllocation, lgp::framework::Error> shadowSrv =
        deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(1U);
    if (!shadowSrv)
    {
        return std::unexpected(std::move(shadowSrv.error()));
    }
    shadowShaderResourceView_ = *shadowSrv;

    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreateRootSignatures(); !status)
    {
        return status;
    }
    if (auto status = CreatePipelineStates(); !status)
    {
        return status;
    }
    if (auto status = CreateSceneBuffers(); !status)
    {
        return status;
    }
    if (auto status = CreateConstantBuffers(); !status)
    {
        return status;
    }
    if (auto status = CreateShadowTarget(); !status)
    {
        return status;
    }
    return CreateSizeDependentTargets(context.drawableSize);
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D drawableSize)
{
    return CreateSizeDependentTargets(drawableSize);
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &)
{
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr || shadowTarget_ == nullptr ||
        sceneDepthTarget_ == nullptr || labConstantBuffers_.empty())
    {
        return {};
    }

    gpu::LabConfiguration const configuration = ActiveConfiguration();
    gpu::CameraFrame const camera = gpu::ComputeCamera(frameContext.drawableSize, configuration.cameraWorldOffset);
    std::expected<gpu::FrameData, CascadeError> frameData = gpu::BuildFrameData(configuration, camera);
    if (!frameData)
    {
        return std::unexpected(lgp::framework::MakeError("BuildFrameData", "The cascade configuration was invalid."));
    }

    std::size_t const slot = frameContext.frameSlot % labConstantBuffers_.size();
    if (auto status = lgp::framework::WriteBuffer(labConstantBuffers_[slot],
                                                  std::span<gpu::LabConstants const>{&frameData->lab, 1U});
        !status)
    {
        return status;
    }
    if (auto status =
            lgp::framework::WriteBuffer(shadowConstantBuffers_[slot],
                                        std::span<gpu::ShadowPassConstants const>{frameData->shadowPasses.data(), 1U});
        !status)
    {
        return status;
    }

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.IASetVertexBuffers(0U, 1U, &sceneVertexView_);
    commandList.IASetIndexBuffer(&sceneIndexView_);

    // --- Depth pass: fill the single shadow map from the light's point of view. ---
    lgp::framework::TransitionTexture(commandList, *shadowTarget_.Get(), gpu::ShaderResourceState(),
                                      gpu::DepthWriteState());
    D3D12_VIEWPORT const shadowViewport{
        0.0F, 0.0F, static_cast<float>(gpu::kShadowResolution), static_cast<float>(gpu::kShadowResolution), 0.0F, 1.0F};
    D3D12_RECT const shadowScissor{0, 0, static_cast<LONG>(gpu::kShadowResolution),
                                   static_cast<LONG>(gpu::kShadowResolution)};
    commandList.RSSetViewports(1U, &shadowViewport);
    commandList.RSSetScissorRects(1U, &shadowScissor);
    commandList.OMSetRenderTargets(0U, nullptr, FALSE, &shadowDepthView_.cpuHandle);
    commandList.ClearDepthStencilView(shadowDepthView_.cpuHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0U, 0U, nullptr);
    commandList.SetGraphicsRootSignature(shadowRootSignature_.Get());
    commandList.SetGraphicsRootConstantBufferView(ShadowPassConstantsSlot,
                                                  shadowConstantBuffers_[slot].gpu_virtual_address());
    commandList.SetPipelineState(shadowPipeline_.Get());
    commandList.DrawIndexedInstanced(sceneIndexCount_, 1U, 0U, 0, 0U);

    lgp::framework::TransitionTexture(commandList, *shadowTarget_.Get(), gpu::DepthWriteState(),
                                      gpu::ShaderResourceState());

    // --- Lighting pass: shade the scene and sample the shadow map. ---
    lgp::framework::TransitionTexture(commandList, *frameContext.renderTarget, gpu::FrameStartState(frameContext),
                                      gpu::RenderTargetState());
    float const clearColor[]{0.05F, 0.06F, 0.09F, 1.0F};
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, &sceneDepthView_.cpuHandle);
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    commandList.ClearDepthStencilView(sceneDepthView_.cpuHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0U, 0U, nullptr);
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.SetGraphicsRootSignature(lightingRootSignature_.Get());
    commandList.SetGraphicsRootConstantBufferView(LightingConstantsSlot,
                                                  labConstantBuffers_[slot].gpu_virtual_address());
    commandList.SetGraphicsRootDescriptorTable(LightingShadowTableSlot, shadowShaderResourceView_.gpuHandle);
    commandList.SetPipelineState(lightingPipeline_.Get());
    commandList.DrawIndexedInstanced(sceneIndexCount_, 1U, 0U, 0, 0U);

    lgp::framework::TransitionTexture(commandList, *frameContext.renderTarget, gpu::RenderTargetState(),
                                      gpu::FrameEndState(frameContext));
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    shadowTarget_.Reset();
    sceneDepthTarget_.Reset();
    if (sceneDepthView_)
    {
        dsvHeap_.Free(sceneDepthView_);
        sceneDepthView_ = {};
    }
    if (shadowDepthView_)
    {
        dsvHeap_.Free(shadowDepthView_);
        shadowDepthView_ = {};
    }
    if (shadowShaderResourceView_)
    {
        deviceResources.shader_visible_cbv_srv_uav_heap().Free(shadowShaderResourceView_);
        shadowShaderResourceView_ = {};
    }
    labConstantBuffers_.clear();
    shadowConstantBuffers_.clear();
    sceneVertices_ = {};
    sceneIndices_ = {};
    dsvHeap_ = {};
    shadowPipeline_.Reset();
    lightingPipeline_.Reset();
    shadowRootSignature_.Reset();
    lightingRootSignature_.Reset();
    deviceResources_ = nullptr;
}

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
    hasHeadlessConfiguration_ = true;
}

std::expected<gpu::DepthReadback, lgp::framework::Error> Renderer::ReadBackShadowMap()
{
    if (deviceResources_ == nullptr || shadowTarget_ == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("Renderer::ReadBackShadowMap", "The shadow map has not been created."));
    }
    return gpu::ReadBackDepthSlice(*deviceResources_, *shadowTarget_.Get(),
                                   {gpu::kShadowResolution, gpu::kShadowResolution}, 0U, 1U);
}

} // namespace ch24::cascaded_shadows::starter
