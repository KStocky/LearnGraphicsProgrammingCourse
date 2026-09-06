#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include <DirectXMath.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

namespace ch23::material_layering::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

enum LightingRootParameter : UINT
{
    LightingConstantsSlot = 0U,
};

enum DisplayRootParameter : UINT
{
    DisplayConstantsSlot = 0U,
    DisplayHdrTableSlot = 1U,
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
    return std::filesystem::path{__FILE__}.parent_path() / "MaterialLab.hlsl";
}

[[nodiscard]] char const *PresetName(gpu::ScenePreset preset) noexcept
{
    switch (preset)
    {
    case gpu::ScenePreset::Baseline:
        return "Baseline (isotropic)";
    case gpu::ScenePreset::Anisotropic:
        return "Anisotropic";
    case gpu::ScenePreset::Clearcoat:
        return "Clearcoat";
    case gpu::ScenePreset::Emissive:
        return "Emissive";
    }
    return "Unknown";
}

[[nodiscard]] char const *ViewName(gpu::OutputView view) noexcept
{
    switch (view)
    {
    case gpu::OutputView::Final:
        return "Final";
    case gpu::OutputView::Base:
        return "Base (coat-attenuated)";
    case gpu::OutputView::Coat:
        return "Coat reflection";
    case gpu::OutputView::Emission:
        return "Emission";
    case gpu::OutputView::Attenuation:
        return "Coat transmission";
    }
    return "Unknown";
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

    if (auto status = gpu::CompileShaderEntry(compiler, options, L"SphereVS", L"vs_6_0", sphereVertexShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShaderEntry(compiler, options, L"ProbeVS", L"vs_6_0", probeVertexShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShaderEntry(compiler, options, L"LightingPS", L"ps_6_0", lightingPixelShader_);
        !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShaderEntry(compiler, options, L"FullscreenVS", L"vs_6_0", fullscreenVertexShader_);
        !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShaderEntry(compiler, options, L"DisplayPS", L"ps_6_0", displayPixelShader_); !status)
    {
        return status;
    }
    return {};
}

lgp::framework::Status Renderer::CreateRootSignatures()
{
    D3D12_ROOT_PARAMETER lightingParameters[1]{};
    lightingParameters[LightingConstantsSlot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    lightingParameters[LightingConstantsSlot].Descriptor.ShaderRegister = 0U;
    lightingParameters[LightingConstantsSlot].Descriptor.RegisterSpace = 0U;
    lightingParameters[LightingConstantsSlot].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC lightingDescription{};
    lightingDescription.NumParameters = static_cast<UINT>(std::size(lightingParameters));
    lightingDescription.pParameters = lightingParameters;
    lightingDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
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

    D3D12_DESCRIPTOR_RANGE hdrRange{};
    hdrRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    hdrRange.NumDescriptors = 1U;
    hdrRange.BaseShaderRegister = 0U;
    hdrRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER displayParameters[2]{};
    displayParameters[DisplayConstantsSlot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    displayParameters[DisplayConstantsSlot].Constants.ShaderRegister = 0U;
    displayParameters[DisplayConstantsSlot].Constants.RegisterSpace = 1U;
    displayParameters[DisplayConstantsSlot].Constants.Num32BitValues =
        sizeof(gpu::DisplayConstants) / sizeof(std::uint32_t);
    displayParameters[DisplayConstantsSlot].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    displayParameters[DisplayHdrTableSlot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    displayParameters[DisplayHdrTableSlot].DescriptorTable.NumDescriptorRanges = 1U;
    displayParameters[DisplayHdrTableSlot].DescriptorTable.pDescriptorRanges = &hdrRange;
    displayParameters[DisplayHdrTableSlot].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC displayDescription{};
    displayDescription.NumParameters = static_cast<UINT>(std::size(displayParameters));
    displayDescription.pParameters = displayParameters;
    displayDescription.NumStaticSamplers = 1U;
    displayDescription.pStaticSamplers = &sampler;
    displayDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                               D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                               D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                               D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    if (HRESULT const result =
            D3D12SerializeRootSignature(&displayDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                        serialized.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
        FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", result, BlobToUtf8(errors.Get())));
    }
    if (HRESULT const result = deviceResources_->device()->CreateRootSignature(
            0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(displayRootSignature_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", result,
                                                                "Failed to create the display root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreatePipelineStates()
{
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_BACK;
    rasterizer.FrontCounterClockwise = FALSE;
    rasterizer.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_INPUT_ELEMENT_DESC const inputLayout[]{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TANGENT", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 24U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC sphereDescription{};
    sphereDescription.pRootSignature = lightingRootSignature_.Get();
    sphereDescription.VS = sphereVertexShader_.Bytecode();
    sphereDescription.PS = lightingPixelShader_.Bytecode();
    sphereDescription.BlendState = blend;
    sphereDescription.SampleMask = UINT_MAX;
    sphereDescription.RasterizerState = rasterizer;
    sphereDescription.DepthStencilState = depth;
    sphereDescription.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    sphereDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    sphereDescription.NumRenderTargets = 1U;
    sphereDescription.RTVFormats[0] = gpu::kHdrFormat;
    sphereDescription.DSVFormat = gpu::kDepthFormat;
    sphereDescription.SampleDesc.Count = 1U;
    if (HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
            &sphereDescription, IID_PPV_ARGS(spherePipeline_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create the sphere pipeline."));
    }

    // The probe card fills the viewport with one analytic quad. It needs no vertex
    // buffer and no depth, so it uses the same lighting root signature and pixel
    // shader but a vertex-id-only vertex shader.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC probeDescription = sphereDescription;
    probeDescription.VS = probeVertexShader_.Bytecode();
    probeDescription.InputLayout = {};
    probeDescription.DepthStencilState.DepthEnable = FALSE;
    probeDescription.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    probeDescription.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    probeDescription.DSVFormat = DXGI_FORMAT_UNKNOWN;
    if (HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
            &probeDescription, IID_PPV_ARGS(probePipeline_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create the probe pipeline."));
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC displayDescription{};
    displayDescription.pRootSignature = displayRootSignature_.Get();
    displayDescription.VS = fullscreenVertexShader_.Bytecode();
    displayDescription.PS = displayPixelShader_.Bytecode();
    displayDescription.BlendState = blend;
    displayDescription.SampleMask = UINT_MAX;
    displayDescription.RasterizerState = rasterizer;
    displayDescription.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    displayDescription.DepthStencilState.DepthEnable = FALSE;
    displayDescription.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    displayDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    displayDescription.NumRenderTargets = 1U;
    displayDescription.RTVFormats[0] = deviceResources_->back_buffer_format();
    displayDescription.DSVFormat = DXGI_FORMAT_UNKNOWN;
    displayDescription.SampleDesc.Count = 1U;
    if (HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
            &displayDescription, IID_PPV_ARGS(displayPipeline_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create the display pipeline."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateMeshBuffers()
{
    std::expected<gpu::MeshGeometry, MaterialError> sphere = gpu::GenerateSphere(1.0F, 32U, 48U);
    if (!sphere)
    {
        return std::unexpected(lgp::framework::MakeError("GenerateSphere", "Failed to build the chapter sphere."));
    }

    std::uint64_t const vertexBytes = sphere->vertices.size() * sizeof(gpu::MeshVertex);
    std::expected<lgp::framework::Buffer, lgp::framework::Error> vertexBuffer =
        lgp::framework::CreateUploadBuffer(*deviceResources_->device(), vertexBytes, L"Ch23 sphere vertices");
    if (!vertexBuffer)
    {
        return std::unexpected(std::move(vertexBuffer.error()));
    }
    sphereVertices_ = std::move(*vertexBuffer);
    if (auto status = lgp::framework::WriteBuffer(
            sphereVertices_, std::span<gpu::MeshVertex const>{sphere->vertices.data(), sphere->vertices.size()});
        !status)
    {
        return status;
    }

    std::uint64_t const indexBytes = sphere->indices.size() * sizeof(std::uint32_t);
    std::expected<lgp::framework::Buffer, lgp::framework::Error> indexBuffer =
        lgp::framework::CreateUploadBuffer(*deviceResources_->device(), indexBytes, L"Ch23 sphere indices");
    if (!indexBuffer)
    {
        return std::unexpected(std::move(indexBuffer.error()));
    }
    sphereIndices_ = std::move(*indexBuffer);
    if (auto status = lgp::framework::WriteBuffer(
            sphereIndices_, std::span<std::uint32_t const>{sphere->indices.data(), sphere->indices.size()});
        !status)
    {
        return status;
    }

    sphereVertexView_.BufferLocation = sphereVertices_.gpu_virtual_address();
    sphereVertexView_.SizeInBytes = static_cast<UINT>(vertexBytes);
    sphereVertexView_.StrideInBytes = sizeof(gpu::MeshVertex);
    sphereIndexView_.BufferLocation = sphereIndices_.gpu_virtual_address();
    sphereIndexView_.SizeInBytes = static_cast<UINT>(indexBytes);
    sphereIndexView_.Format = DXGI_FORMAT_R32_UINT;
    sphereIndexCount_ = static_cast<UINT>(sphere->indices.size());
    return {};
}

lgp::framework::Status Renderer::CreateConstantBuffers()
{
    constantBufferStride_ = lgp::framework::AlignConstantBufferSize(sizeof(gpu::LabConstants));
    UINT const slots = std::max<UINT>(deviceResources_->back_buffer_count(), 1U);
    constantBuffers_.clear();
    constantBuffers_.reserve(slots);
    for (UINT slot = 0U; slot < slots; ++slot)
    {
        std::expected<lgp::framework::Buffer, lgp::framework::Error> buffer = lgp::framework::CreateUploadBuffer(
            *deviceResources_->device(), constantBufferStride_, L"Ch23 lab constants");
        if (!buffer)
        {
            return std::unexpected(std::move(buffer.error()));
        }
        constantBuffers_.push_back(std::move(*buffer));
    }
    return {};
}

lgp::framework::Status Renderer::CreateSizeDependentTargets(lgp::framework::Extent2D size)
{
    hdrTarget_.Reset();
    depthTarget_.Reset();
    hdrSize_ = {};
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

    D3D12_CLEAR_VALUE hdrClear{};
    hdrClear.Format = gpu::kHdrFormat;
    hdrClear.Color[3] = 1.0F;
    texture.Format = gpu::kHdrFormat;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (HRESULT const result = deviceResources_->device()->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &hdrClear, nullptr, 0U,
            nullptr, IID_PPV_ARGS(hdrTarget_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                                "Failed to create the HDR target."));
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = gpu::kHdrFormat;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    deviceResources_->device()->CreateRenderTargetView(hdrTarget_.Get(), &rtv, hdrRenderTargetView_.cpuHandle);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = gpu::kHdrFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1U;
    deviceResources_->device()->CreateShaderResourceView(hdrTarget_.Get(), &srv, hdrShaderResourceView_.cpuHandle);

    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = gpu::kDepthFormat;
    depthClear.DepthStencil.Depth = 1.0F;
    texture.Format = gpu::kDepthFormat;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (HRESULT const result = deviceResources_->device()->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, &depthClear, nullptr, 0U,
            nullptr, IID_PPV_ARGS(depthTarget_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                                "Failed to create the depth target."));
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = gpu::kDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    deviceResources_->device()->CreateDepthStencilView(depthTarget_.Get(), &dsv, depthView_.cpuHandle);
    hdrSize_ = size;
    return {};
}

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    if (hasHeadlessConfiguration_)
    {
        return headlessConfiguration_;
    }
    gpu::LabConfiguration configuration = gpu::ConfigurationForPreset(interactivePreset_);
    configuration.outputView = interactiveView_;
    configuration.geometry = interactiveGeometry_;
    configuration.exposure = interactiveExposure_;
    return configuration;
}

gpu::CameraMatrices Renderer::ActiveCamera(lgp::framework::Extent2D size) const noexcept
{
    return gpu::ComputeSphereCamera(size, orbitAzimuth_, orbitElevation_, orbitRadius_);
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;

    std::expected<lgp::framework::DescriptorHeap, lgp::framework::Error> dsvHeap = lgp::framework::CreateDescriptorHeap(
        *deviceResources_->device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1U, false, L"Ch23 solution DSV heap");
    if (!dsvHeap)
    {
        return std::unexpected(std::move(dsvHeap.error()));
    }
    dsvHeap_ = std::move(*dsvHeap);
    std::expected<lgp::framework::DescriptorAllocation, lgp::framework::Error> depthView = dsvHeap_.Allocate(1U);
    if (!depthView)
    {
        return std::unexpected(std::move(depthView.error()));
    }
    depthView_ = *depthView;

    std::expected<lgp::framework::DescriptorAllocation, lgp::framework::Error> hdrRtv =
        deviceResources_->rtv_heap().Allocate(1U);
    if (!hdrRtv)
    {
        return std::unexpected(std::move(hdrRtv.error()));
    }
    hdrRenderTargetView_ = *hdrRtv;

    std::expected<lgp::framework::DescriptorAllocation, lgp::framework::Error> hdrSrv =
        deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(1U);
    if (!hdrSrv)
    {
        return std::unexpected(std::move(hdrSrv.error()));
    }
    hdrShaderResourceView_ = *hdrSrv;

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
    if (auto status = CreateMeshBuffers(); !status)
    {
        return status;
    }
    if (auto status = CreateConstantBuffers(); !status)
    {
        return status;
    }
    return CreateSizeDependentTargets(context.drawableSize);
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D drawableSize)
{
    return CreateSizeDependentTargets(drawableSize);
}

void Renderer::ApplyInteractiveControls(lgp::framework::UpdateContext const &context)
{
    lgp::framework::InputState const &input = context.input;
    gpu::ScenePreset const previousPreset = interactivePreset_;
    gpu::OutputView const previousView = interactiveView_;
    gpu::SceneGeometry const previousGeometry = interactiveGeometry_;

    if (input.WasKeyPressed('1'))
    {
        interactivePreset_ = gpu::ScenePreset::Baseline;
    }
    if (input.WasKeyPressed('2'))
    {
        interactivePreset_ = gpu::ScenePreset::Anisotropic;
    }
    if (input.WasKeyPressed('3'))
    {
        interactivePreset_ = gpu::ScenePreset::Clearcoat;
    }
    if (input.WasKeyPressed('4'))
    {
        interactivePreset_ = gpu::ScenePreset::Emissive;
    }
    if (input.WasKeyPressed('V'))
    {
        interactiveView_ = static_cast<gpu::OutputView>((static_cast<std::uint32_t>(interactiveView_) + 1U) % 5U);
    }
    if (input.WasKeyPressed('G'))
    {
        interactiveGeometry_ = interactiveGeometry_ == gpu::SceneGeometry::Sphere ? gpu::SceneGeometry::ProbeCard
                                                                                  : gpu::SceneGeometry::Sphere;
    }
    if (input.WasKeyPressed(VK_OEM_4)) // '['
    {
        interactiveExposure_ = std::clamp(interactiveExposure_ - 0.25F, -8.0F, 8.0F);
    }
    if (input.WasKeyPressed(VK_OEM_6)) // ']'
    {
        interactiveExposure_ = std::clamp(interactiveExposure_ + 0.25F, -8.0F, 8.0F);
    }

    if (input.mouse.IsButtonDown(lgp::framework::MouseButton::Right))
    {
        orbitAzimuth_ += static_cast<float>(input.mouse.deltaX) * 0.006F;
        orbitElevation_ = std::clamp(orbitElevation_ - (static_cast<float>(input.mouse.deltaY) * 0.006F), -1.2F, 1.2F);
    }
    orbitRadius_ = std::clamp(orbitRadius_ - input.mouse.wheelDelta, 1.8F, 8.0F);

    if (interactivePreset_ != previousPreset || interactiveView_ != previousView ||
        interactiveGeometry_ != previousGeometry)
    {
        std::printf("[Ch23] preset=%s  view=%s  geometry=%s\n", PresetName(interactivePreset_),
                    ViewName(interactiveView_),
                    interactiveGeometry_ == gpu::SceneGeometry::Sphere ? "Sphere" : "ProbeCard");
        std::fflush(stdout);
    }
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &context)
{
    if (!headless_)
    {
        ApplyInteractiveControls(context);
    }
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr || hdrTarget_ == nullptr ||
        depthTarget_ == nullptr || constantBuffers_.empty())
    {
        return {};
    }

    gpu::LabConfiguration const configuration = ActiveConfiguration();
    gpu::CameraMatrices const camera = ActiveCamera(frameContext.drawableSize);
    gpu::LabConstants const constants = gpu::MakeLabConstants(configuration, camera);
    gpu::DisplayConstants displayConstants{};
    displayConstants.exposure = configuration.exposure;
    displayConstants.outputView = static_cast<std::uint32_t>(configuration.outputView);

    lgp::framework::Buffer &constantBuffer = constantBuffers_[frameContext.frameSlot % constantBuffers_.size()];
    if (auto status = lgp::framework::WriteBuffer(constantBuffer, std::span<gpu::LabConstants const>{&constants, 1U});
        !status)
    {
        return status;
    }

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);

    lgp::framework::TransitionTexture(commandList, *hdrTarget_.Get(), gpu::ShaderResourceState(),
                                      gpu::RenderTargetState());
    float const hdrClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.OMSetRenderTargets(1U, &hdrRenderTargetView_.cpuHandle, FALSE, &depthView_.cpuHandle);
    commandList.ClearRenderTargetView(hdrRenderTargetView_.cpuHandle, hdrClear, 0U, nullptr);
    commandList.ClearDepthStencilView(depthView_.cpuHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0U, 0U, nullptr);
    commandList.SetGraphicsRootSignature(lightingRootSignature_.Get());
    commandList.SetGraphicsRootConstantBufferView(LightingConstantsSlot, constantBuffer.gpu_virtual_address());

    if (configuration.geometry == gpu::SceneGeometry::ProbeCard)
    {
        commandList.SetPipelineState(probePipeline_.Get());
        commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        commandList.DrawInstanced(4U, 1U, 0U, 0U);
    }
    else
    {
        commandList.SetPipelineState(spherePipeline_.Get());
        commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList.IASetVertexBuffers(0U, 1U, &sphereVertexView_);
        commandList.IASetIndexBuffer(&sphereIndexView_);
        commandList.DrawIndexedInstanced(sphereIndexCount_, 1U, 0U, 0, 0U);
    }

    lgp::framework::TransitionTexture(commandList, *hdrTarget_.Get(), gpu::RenderTargetState(),
                                      gpu::ShaderResourceState());

    lgp::framework::TransitionTexture(commandList, *frameContext.renderTarget, gpu::FrameStartState(frameContext),
                                      gpu::RenderTargetState());
    float const displayClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.ClearRenderTargetView(frameContext.renderTargetView, displayClear, 0U, nullptr);
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.SetGraphicsRootSignature(displayRootSignature_.Get());
    commandList.SetGraphicsRoot32BitConstants(
        DisplayConstantsSlot, sizeof(gpu::DisplayConstants) / sizeof(std::uint32_t), &displayConstants, 0U);
    commandList.SetGraphicsRootDescriptorTable(DisplayHdrTableSlot, hdrShaderResourceView_.gpuHandle);
    commandList.SetPipelineState(displayPipeline_.Get());
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.DrawInstanced(3U, 1U, 0U, 0U);

    lgp::framework::TransitionTexture(commandList, *frameContext.renderTarget, gpu::RenderTargetState(),
                                      gpu::FrameEndState(frameContext));
    return {};
}

std::expected<gpu::TextureReadback, lgp::framework::Error> Renderer::ReadBackHdr()
{
    if (deviceResources_ == nullptr || hdrTarget_ == nullptr || hdrSize_.empty())
    {
        return std::unexpected(
            lgp::framework::MakeError("Renderer::ReadBackHdr", "The HDR target has not been created."));
    }
    return gpu::ReadBackTexture(*deviceResources_, *hdrTarget_.Get(), hdrSize_, gpu::kHdrFormat);
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    hdrTarget_.Reset();
    depthTarget_.Reset();
    if (depthView_)
    {
        dsvHeap_.Free(depthView_);
        depthView_ = {};
    }
    if (hdrRenderTargetView_)
    {
        deviceResources.rtv_heap().Free(hdrRenderTargetView_);
        hdrRenderTargetView_ = {};
    }
    if (hdrShaderResourceView_)
    {
        deviceResources.shader_visible_cbv_srv_uav_heap().Free(hdrShaderResourceView_);
        hdrShaderResourceView_ = {};
    }
    constantBuffers_.clear();
    sphereVertices_ = {};
    sphereIndices_ = {};
    dsvHeap_ = {};
    spherePipeline_.Reset();
    probePipeline_.Reset();
    displayPipeline_.Reset();
    lightingRootSignature_.Reset();
    displayRootSignature_.Reset();
    deviceResources_ = nullptr;
}

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
    hasHeadlessConfiguration_ = true;
}

} // namespace ch23::material_layering::solution
