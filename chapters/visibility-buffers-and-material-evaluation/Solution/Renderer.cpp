#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

namespace ch15::visibility_buffer::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorsPerSlot = 11U;

enum GraphicsRootParameter : UINT
{
    GraphicsFrameConstants = 0U,
    GraphicsDrawConstants = 1U,
};

enum ComputeRootParameter : UINT
{
    ComputeFrameConstants = 0U,
    ComputeInputTable = 1U,
    ComputeLightTable = 2U,
    ComputeUavTable = 3U,
};

enum DescriptorIndex : UINT
{
    VisibilitySrv = 0U,
    DepthSrv = 1U,
    VertexSrv = 2U,
    IndexSrv = 3U,
    DrawSrv = 4U,
    MaterialSrv = 5U,
    LightSrv = 6U,
    CellSrv = 7U,
    LightIndexSrv = 8U,
    OutputUav = 9U,
    DiagnosticsUav = 10U,
};

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "VisibilityBufferLab.hlsl";
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] D3D12_STATIC_SAMPLER_DESC MaterialSampler() noexcept
{
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0F;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0U;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return sampler;
}

[[nodiscard]] lgp::framework::Status SerializeRootSignature(ID3D12Device10 &device,
                                                            D3D12_ROOT_SIGNATURE_DESC const &description,
                                                            char const *label,
                                                            ComPtr<ID3D12RootSignature> &rootSignature)
{
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
        device.CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                   IID_PPV_ARGS(rootSignature.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device::CreateRootSignature", createResult,
            std::string{"Failed to create the Chapter 15 Solution "} + label + " root signature."));
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
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device::CreateComputePipelineState", result,
            "Failed to create the Chapter 15 Solution compute material-evaluation pipeline."));
    }
    return {};
}

[[nodiscard]] lgp::framework::Status CreateTexture(ID3D12Device10 &device, lgp::framework::Extent2D size,
                                                   DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, char const *label,
                                                   ComPtr<ID3D12Resource> &texture)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC1 const description = gpu::MakeTextureDescription(size, format, flags);
    HRESULT const result =
        device.CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_COMMON, nullptr,
                                        nullptr, 0U, nullptr, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device10::CreateCommittedResource3", result,
            std::string{"Failed to create the Chapter 15 Solution "} + label + " texture."));
    }
    return {};
}

} // namespace

lgp::framework::Status Renderer::ValidateRequiredFeatures()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
    HRESULT const optionsResult =
        deviceResources_->device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options));
    if (FAILED(optionsResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CheckFeatureSupport", optionsResult,
                                             "Failed to query the Chapter 15 barycentrics capability."));
    }
    if (!options.BarycentricsSupported)
    {
        return std::unexpected(
            lgp::framework::MakeError("ID3D12Device::CheckFeatureSupport",
                                      "Chapter 15 Solution requires native pixel-shader SV_Barycentrics support."));
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_1};
    HRESULT const shaderModelResult =
        deviceResources_->device()->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));
    if (FAILED(shaderModelResult) || shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_1)
    {
        return std::unexpected(lgp::framework::MakeError("ID3D12Device::CheckFeatureSupport",
                                                         "Chapter 15 Solution requires Shader Model 6.1 or newer."));
    }
    return {};
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
    options.includeDirectories = {options.sourcePath.parent_path(), gpu::SharedShaderPath().parent_path()};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif
    if (auto status = gpu::CompileShader(compiler, options, L"RasterVS", L"vs_6_1", rasterVertexShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"VisibilityPS", L"ps_6_1", rasterPixelShader_); !status)
    {
        return status;
    }
    return gpu::CompileShader(compiler, options, L"ShadeCS", L"cs_6_1", shadeShader_);
}

lgp::framework::Status Renderer::CreateRootSignatures()
{
    D3D12_ROOT_PARAMETER graphicsParameters[2]{};
    graphicsParameters[GraphicsFrameConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[GraphicsFrameConstants].Constants.ShaderRegister = 0U;
    graphicsParameters[GraphicsFrameConstants].Constants.Num32BitValues =
        sizeof(gpu::FrameConstants) / sizeof(std::uint32_t);
    graphicsParameters[GraphicsFrameConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    graphicsParameters[GraphicsDrawConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[GraphicsDrawConstants].Constants.ShaderRegister = 1U;
    graphicsParameters[GraphicsDrawConstants].Constants.Num32BitValues =
        sizeof(gpu::RasterDrawConstants) / sizeof(std::uint32_t);
    graphicsParameters[GraphicsDrawConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC graphicsDescription{};
    graphicsDescription.NumParameters = static_cast<UINT>(std::size(graphicsParameters));
    graphicsDescription.pParameters = graphicsParameters;
    graphicsDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    if (auto status = SerializeRootSignature(*deviceResources_->device(), graphicsDescription, "graphics",
                                             graphicsRootSignature_);
        !status)
    {
        return status;
    }

    D3D12_DESCRIPTOR_RANGE inputRange{};
    inputRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    inputRange.NumDescriptors = 6U;
    inputRange.BaseShaderRegister = 0U;
    inputRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE lightRange{};
    lightRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    lightRange.NumDescriptors = 3U;
    lightRange.BaseShaderRegister = 20U;
    lightRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 2U;
    uavRange.BaseShaderRegister = 0U;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER computeParameters[4]{};
    computeParameters[ComputeFrameConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[ComputeFrameConstants].Constants.ShaderRegister = 0U;
    computeParameters[ComputeFrameConstants].Constants.Num32BitValues =
        sizeof(gpu::FrameConstants) / sizeof(std::uint32_t);
    computeParameters[ComputeFrameConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[ComputeInputTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeInputTable].DescriptorTable.NumDescriptorRanges = 1U;
    computeParameters[ComputeInputTable].DescriptorTable.pDescriptorRanges = &inputRange;
    computeParameters[ComputeInputTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[ComputeLightTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeLightTable].DescriptorTable.NumDescriptorRanges = 1U;
    computeParameters[ComputeLightTable].DescriptorTable.pDescriptorRanges = &lightRange;
    computeParameters[ComputeLightTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[ComputeUavTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeUavTable].DescriptorTable.NumDescriptorRanges = 1U;
    computeParameters[ComputeUavTable].DescriptorTable.pDescriptorRanges = &uavRange;
    computeParameters[ComputeUavTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC const sampler = MaterialSampler();
    D3D12_ROOT_SIGNATURE_DESC computeDescription{};
    computeDescription.NumParameters = static_cast<UINT>(std::size(computeParameters));
    computeDescription.pParameters = computeParameters;
    computeDescription.NumStaticSamplers = 1U;
    computeDescription.pStaticSamplers = &sampler;
    return SerializeRootSignature(*deviceResources_->device(), computeDescription, "compute", computeRootSignature_);
}

lgp::framework::Status Renderer::CreatePipelines()
{
    D3D12_INPUT_ELEMENT_DESC const inputLayout[]{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    };
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = graphicsRootSignature_.Get();
    description.VS = rasterVertexShader_.Bytecode();
    description.PS = rasterPixelShader_.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = gpu::kVisibilityFormat;
    description.DSVFormat = gpu::kDepthDsvFormat;
    description.SampleDesc.Count = 1U;

    description.DepthStencilState.DepthFunc = gpu::DepthFunction(gpu::DepthConvention::Forward);
    HRESULT const forwardResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(rasterForwardPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(forwardResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device::CreateGraphicsPipelineState", forwardResult,
            "Failed to create the Chapter 15 Solution forward-depth visibility pipeline."));
    }
    description.DepthStencilState.DepthFunc = gpu::DepthFunction(gpu::DepthConvention::Reversed);
    HRESULT const reversedResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(rasterReversedPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(reversedResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device::CreateGraphicsPipelineState", reversedResult,
            "Failed to create the Chapter 15 Solution reversed-depth visibility pipeline."));
    }
    return CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), shadeShader_,
                                 shadePipeline_);
}

lgp::framework::Status Renderer::CreateGeometry()
{
    diagnosticScene_ = gpu::BuildScene(gpu::ScenePreset::Diagnostic);
    if (auto const validation = gpu::ValidateScene(diagnosticScene_); !validation)
    {
        return std::unexpected(validation.error());
    }
    auto vertices = gpu::CreateBuffer(
        *deviceResources_->device(), static_cast<std::uint64_t>(diagnosticScene_.vertices.size()) * sizeof(gpu::Vertex),
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch15 Solution indexed vertices", true);
    auto indices =
        gpu::CreateBuffer(*deviceResources_->device(),
                          static_cast<std::uint64_t>(diagnosticScene_.indices.size()) * sizeof(std::uint32_t),
                          D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch15 Solution indexed indices", true);
    auto draws = gpu::CreateBuffer(
        *deviceResources_->device(), static_cast<std::uint64_t>(diagnosticScene_.draws.size()) * sizeof(gpu::DrawData),
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch15 Solution checked draw ranges", true);
    if (!vertices)
    {
        return std::unexpected(vertices.error());
    }
    if (!indices)
    {
        return std::unexpected(indices.error());
    }
    if (!draws)
    {
        return std::unexpected(draws.error());
    }
    mesh_.vertices = std::move(*vertices);
    mesh_.indices = std::move(*indices);
    mesh_.draws = std::move(*draws);
    if (auto status = gpu::WriteBuffer(mesh_.vertices, std::span<gpu::Vertex const>{diagnosticScene_.vertices});
        !status)
    {
        return status;
    }
    if (auto status = gpu::WriteBuffer(mesh_.indices, std::span<std::uint32_t const>{diagnosticScene_.indices});
        !status)
    {
        return status;
    }
    if (auto status = gpu::WriteBuffer(mesh_.draws, std::span<gpu::DrawData const>{diagnosticScene_.draws}); !status)
    {
        return status;
    }
    mesh_.vertexView.BufferLocation = mesh_.vertices.Get()->GetGPUVirtualAddress();
    mesh_.vertexView.SizeInBytes = static_cast<UINT>(diagnosticScene_.vertices.size() * sizeof(gpu::Vertex));
    mesh_.vertexView.StrideInBytes = sizeof(gpu::Vertex);
    mesh_.indexView.BufferLocation = mesh_.indices.Get()->GetGPUVirtualAddress();
    mesh_.indexView.SizeInBytes = static_cast<UINT>(diagnosticScene_.indices.size() * sizeof(std::uint32_t));
    mesh_.indexView.Format = DXGI_FORMAT_R32_UINT;
    return {};
}

void Renderer::ReleaseSizeDependentResources() noexcept
{
    for (FrameSlotResources &slot : frameSlots_)
    {
        slot.visibility.Reset();
        slot.depth.Reset();
        slot.output.Reset();
        if (slot.rtv)
        {
            rtvHeap_.Free(slot.rtv);
        }
        if (slot.dsv)
        {
            dsvHeap_.Free(slot.dsv);
        }
        if (slot.descriptors)
        {
            deviceResources_->shader_visible_cbv_srv_uav_heap().Free(slot.descriptors);
        }
        slot.rtv = {};
        slot.dsv = {};
        slot.descriptors = {};
    }
    frameSlots_.clear();
}

lgp::framework::Status Renderer::CreateSizeDependentResources(lgp::framework::Extent2D size)
{
    ReleaseSizeDependentResources();
    if (size.empty())
    {
        return {};
    }
    if (auto const extent = gpu::ValidateExtent(size); !extent)
    {
        return std::unexpected(extent.error());
    }
    std::uint64_t const pixelCount = static_cast<std::uint64_t>(size.width) * size.height;
    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlotResources &slot : frameSlots_)
    {
        auto rtv = rtvHeap_.Allocate();
        auto dsv = dsvHeap_.Allocate();
        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorsPerSlot);
        if (!rtv)
        {
            return std::unexpected(rtv.error());
        }
        if (!dsv)
        {
            return std::unexpected(dsv.error());
        }
        if (!descriptors)
        {
            return std::unexpected(descriptors.error());
        }
        slot.rtv = *rtv;
        slot.dsv = *dsv;
        slot.descriptors = *descriptors;

        if (auto status = CreateTexture(*deviceResources_->device(), size, gpu::kVisibilityFormat,
                                        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, "visibility", slot.visibility);
            !status)
        {
            return status;
        }
        D3D12_RENDER_TARGET_VIEW_DESC visibilityRtv{};
        visibilityRtv.Format = gpu::kVisibilityFormat;
        visibilityRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateRenderTargetView(slot.visibility.Get(), &visibilityRtv, slot.rtv.CpuHandle());
        D3D12_SHADER_RESOURCE_VIEW_DESC visibilitySrv{};
        visibilitySrv.Format = gpu::kVisibilityFormat;
        visibilitySrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        visibilitySrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        visibilitySrv.Texture2D.MipLevels = 1U;
        deviceResources_->device()->CreateShaderResourceView(slot.visibility.Get(), &visibilitySrv,
                                                             slot.descriptors.CpuHandle(VisibilitySrv));

        if (auto status = CreateTexture(*deviceResources_->device(), size, gpu::kDepthResourceFormat,
                                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, "depth", slot.depth);
            !status)
        {
            return status;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDescription{};
        dsvDescription.Format = gpu::kDepthDsvFormat;
        dsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateDepthStencilView(slot.depth.Get(), &dsvDescription, slot.dsv.CpuHandle());
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
        depthSrv.Format = gpu::kDepthSrvFormat;
        depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrv.Texture2D.MipLevels = 1U;
        deviceResources_->device()->CreateShaderResourceView(slot.depth.Get(), &depthSrv,
                                                             slot.descriptors.CpuHandle(DepthSrv));

        if (auto status = CreateTexture(*deviceResources_->device(), size, gpu::kOutputFormat,
                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "compute output", slot.output);
            !status)
        {
            return status;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav{};
        outputUav.Format = gpu::kOutputFormat;
        outputUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateUnorderedAccessView(slot.output.Get(), nullptr, &outputUav,
                                                              slot.descriptors.CpuHandle(OutputUav));

        D3D12_SHADER_RESOURCE_VIEW_DESC structuredSrv{};
        structuredSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        structuredSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        structuredSrv.Format = DXGI_FORMAT_UNKNOWN;
        structuredSrv.Buffer.NumElements = static_cast<UINT>(diagnosticScene_.vertices.size());
        structuredSrv.Buffer.StructureByteStride = sizeof(gpu::Vertex);
        deviceResources_->device()->CreateShaderResourceView(mesh_.vertices.Get(), &structuredSrv,
                                                             slot.descriptors.CpuHandle(VertexSrv));
        structuredSrv.Buffer.NumElements = static_cast<UINT>(diagnosticScene_.indices.size());
        structuredSrv.Buffer.StructureByteStride = sizeof(std::uint32_t);
        deviceResources_->device()->CreateShaderResourceView(mesh_.indices.Get(), &structuredSrv,
                                                             slot.descriptors.CpuHandle(IndexSrv));
        structuredSrv.Buffer.NumElements = static_cast<UINT>(diagnosticScene_.draws.size());
        structuredSrv.Buffer.StructureByteStride = sizeof(gpu::DrawData);
        deviceResources_->device()->CreateShaderResourceView(mesh_.draws.Get(), &structuredSrv,
                                                             slot.descriptors.CpuHandle(DrawSrv));

        D3D12_SHADER_RESOURCE_VIEW_DESC materialSrv{};
        materialSrv.Format = gpu::kMaterialFormat;
        materialSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        materialSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        materialSrv.Texture2D.MipLevels = gpu::kMaterialMipCount;
        deviceResources_->device()->CreateShaderResourceView(materialTexture_.Get(), &materialSrv,
                                                             slot.descriptors.CpuHandle(MaterialSrv));

        auto lights =
            gpu::CreateBuffer(*deviceResources_->device(),
                              static_cast<std::uint64_t>(gpu::kMaximumLightCount) * sizeof(gpu::PointLightData),
                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch15 Solution per-slot lights", true);
        auto cells = gpu::CreateBuffer(
            *deviceResources_->device(),
            static_cast<std::uint64_t>(gpu::kMaximumClusterCount) * sizeof(gpu::CellLightRange), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, L"Ch15 Solution per-slot cluster ranges", true);
        auto lightIndices = gpu::CreateBuffer(
            *deviceResources_->device(),
            static_cast<std::uint64_t>(gpu::kMaximumLightIndexCount) * sizeof(std::uint32_t), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, L"Ch15 Solution per-slot light indices", true);
        auto diagnostics = gpu::CreateBuffer(*deviceResources_->device(), pixelCount * sizeof(gpu::PixelDiagnostics),
                                             D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                             L"Ch15 Solution pixel diagnostics");
        auto diagnosticsReadback = gpu::CreateBuffer(
            *deviceResources_->device(), pixelCount * sizeof(gpu::PixelDiagnostics), D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_FLAG_NONE, L"Ch15 Solution diagnostics readback", true);
        if (!lights)
        {
            return std::unexpected(lights.error());
        }
        if (!cells)
        {
            return std::unexpected(cells.error());
        }
        if (!lightIndices)
        {
            return std::unexpected(lightIndices.error());
        }
        if (!diagnostics)
        {
            return std::unexpected(diagnostics.error());
        }
        if (!diagnosticsReadback)
        {
            return std::unexpected(diagnosticsReadback.error());
        }
        slot.lights = std::move(*lights);
        slot.cells = std::move(*cells);
        slot.lightIndices = std::move(*lightIndices);
        slot.diagnostics = std::move(*diagnostics);
        slot.diagnosticsReadback = std::move(*diagnosticsReadback);

        structuredSrv.Buffer.NumElements = gpu::kMaximumLightCount;
        structuredSrv.Buffer.StructureByteStride = sizeof(gpu::PointLightData);
        deviceResources_->device()->CreateShaderResourceView(slot.lights.Get(), &structuredSrv,
                                                             slot.descriptors.CpuHandle(LightSrv));
        structuredSrv.Buffer.NumElements = gpu::kMaximumClusterCount;
        structuredSrv.Buffer.StructureByteStride = sizeof(gpu::CellLightRange);
        deviceResources_->device()->CreateShaderResourceView(slot.cells.Get(), &structuredSrv,
                                                             slot.descriptors.CpuHandle(CellSrv));
        structuredSrv.Buffer.NumElements = gpu::kMaximumLightIndexCount;
        structuredSrv.Buffer.StructureByteStride = sizeof(std::uint32_t);
        deviceResources_->device()->CreateShaderResourceView(slot.lightIndices.Get(), &structuredSrv,
                                                             slot.descriptors.CpuHandle(LightIndexSrv));

        D3D12_UNORDERED_ACCESS_VIEW_DESC diagnosticsUav{};
        diagnosticsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        diagnosticsUav.Format = DXGI_FORMAT_UNKNOWN;
        diagnosticsUav.Buffer.NumElements = static_cast<UINT>(pixelCount);
        diagnosticsUav.Buffer.StructureByteStride = sizeof(gpu::PixelDiagnostics);
        deviceResources_->device()->CreateUnorderedAccessView(slot.diagnostics.Get(), nullptr, &diagnosticsUav,
                                                              slot.descriptors.CpuHandle(DiagnosticsUav));

        auto visibilityReadback = gpu::CreateTextureReadbackBuffer(
            *deviceResources_->device(), slot.visibility->GetDesc(), L"Ch15 Solution visibility readback");
        auto depthReadback = gpu::CreateTextureReadbackBuffer(*deviceResources_->device(), slot.depth->GetDesc(),
                                                              L"Ch15 Solution depth readback");
        if (!visibilityReadback)
        {
            return std::unexpected(visibilityReadback.error());
        }
        if (!depthReadback)
        {
            return std::unexpected(depthReadback.error());
        }
        slot.visibilityReadback = std::move(*visibilityReadback);
        slot.depthReadback = std::move(*depthReadback);
    }
    return {};
}

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    gpu::LabConfiguration configuration{};
    configuration.debugView = interactiveDebugView_;
    if (headless_ && headlessConfiguration_.has_value())
    {
        configuration.scene = headlessConfiguration_->scene;
        configuration.depthConvention = headlessConfiguration_->depthConvention;
        configuration.debugView = headlessConfiguration_->debugView;
        configuration.lightCount = gpu::NormalizeLightCount(headlessConfiguration_->lightCount);
    }
    return configuration;
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    if (auto status = ValidateRequiredFeatures(); !status)
    {
        return status;
    }
    auto rtvHeap =
        lgp::framework::CreateDescriptorHeap(*deviceResources_->device(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                             deviceResources_->back_buffer_count(), false, L"Ch15 Solution RTV heap");
    auto dsvHeap =
        lgp::framework::CreateDescriptorHeap(*deviceResources_->device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                             deviceResources_->back_buffer_count(), false, L"Ch15 Solution DSV heap");
    if (!rtvHeap)
    {
        return std::unexpected(rtvHeap.error());
    }
    if (!dsvHeap)
    {
        return std::unexpected(dsvHeap.error());
    }
    rtvHeap_ = std::move(*rtvHeap);
    dsvHeap_ = std::move(*dsvHeap);
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
    if (auto status = CreateGeometry(); !status)
    {
        return status;
    }
    auto material = gpu::CreateDiagnosticMaterialTexture(*deviceResources_);
    if (!material)
    {
        return std::unexpected(material.error());
    }
    materialTexture_ = std::move(*material);
    return {};
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &deviceResources,
                                          lgp::framework::Extent2D drawableSize)
{
    (void)deviceResources;
    return CreateSizeDependentResources(drawableSize);
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &context)
{
    if (!headless_)
    {
        if (context.input.WasKeyPressed('1'))
        {
            interactiveDebugView_ = gpu::DebugView::Final;
        }
        if (context.input.WasKeyPressed('2'))
        {
            interactiveDebugView_ = gpu::DebugView::ReconstructedUv;
        }
        if (context.input.WasKeyPressed('3'))
        {
            interactiveDebugView_ = gpu::DebugView::GradientMagnitude;
        }
        if (context.input.WasKeyPressed('4'))
        {
            interactiveDebugView_ = gpu::DebugView::Identifiers;
        }
    }
    currentLights_ = gpu::BuildLights(ActiveConfiguration().lightCount);
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr ||
        frameContext.frameSlot >= frameSlots_.size() || frameContext.renderTargetFormat != gpu::kOutputFormat)
    {
        return std::unexpected(
            lgp::framework::MakeError("Render", "The Chapter 15 Solution frame context is invalid."));
    }
    if (auto const extent = gpu::ValidateExtent(frameContext.drawableSize); !extent)
    {
        return std::unexpected(extent.error());
    }

    gpu::LabConfiguration configuration = ActiveConfiguration();
    configuration.lightCount = static_cast<std::uint32_t>(currentLights_.size());
    gpu::PerspectiveProjection const projection =
        gpu::MakeProjection(frameContext.drawableSize, configuration.depthConvention);
    auto const projectionMatrix = gpu::MakeProjectionMatrix(projection);
    auto const lists = gpu::BuildClusteredLightLists(frameContext.drawableSize, projection, currentLights_);
    auto const tiles = ch14::clustered_lighting::MakeTileGrid(
        frameContext.drawableSize.width, frameContext.drawableSize.height, gpu::kTileWidth, gpu::kTileHeight);
    if (!projectionMatrix)
    {
        return std::unexpected(projectionMatrix.error());
    }
    if (!lists)
    {
        return std::unexpected(lists.error());
    }
    if (!tiles)
    {
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 15 Solution tile grid is invalid."));
    }

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    gpu::ClearMappedBuffer(slot.lights);
    gpu::ClearMappedBuffer(slot.cells);
    gpu::ClearMappedBuffer(slot.lightIndices);
    if (auto status = gpu::WriteBuffer(slot.lights, std::span<gpu::PointLightData const>{currentLights_}); !status)
    {
        return status;
    }
    if (auto status = gpu::WriteBuffer(slot.cells, std::span<gpu::CellLightRange const>{lists->cells}); !status)
    {
        return status;
    }
    if (auto status = gpu::WriteBuffer(slot.lightIndices, std::span<std::uint32_t const>{lists->lightIndices}); !status)
    {
        return status;
    }

    std::uint32_t const activeDrawCount =
        configuration.scene == gpu::ScenePreset::Empty ? 0U : static_cast<std::uint32_t>(diagnosticScene_.draws.size());
    auto const coefficients = ch12::gbuffer::MakeDeviceDepthCoefficients(projection);
    if (!coefficients)
    {
        return std::unexpected(
            lgp::framework::MakeError("Render", "The Chapter 15 Solution depth projection is invalid."));
    }
    float const tangentHalfFov = std::tan(projection.verticalFieldOfViewRadians * 0.5F);
    gpu::FrameConstants constants{};
    constants.projection = *projectionMatrix;
    constants.projectionData = {
        tangentHalfFov * projection.aspectRatio,
        tangentHalfFov,
        coefficients->additive,
        coefficients->reciprocal,
    };
    constants.dimensions = {
        frameContext.drawableSize.width,
        frameContext.drawableSize.height,
        tiles->tileCountX,
        tiles->tileCountY,
    };
    constants.counts = {
        activeDrawCount,
        static_cast<std::uint32_t>(diagnosticScene_.vertices.size()),
        static_cast<std::uint32_t>(diagnosticScene_.indices.size()),
        configuration.lightCount,
    };
    constants.clusters = {
        static_cast<std::uint32_t>(lists->cells.size()),
        static_cast<std::uint32_t>(lists->lightIndices.size()),
        gpu::kClusterSliceCount,
        static_cast<std::uint32_t>(configuration.debugView),
    };
    constants.slicing = {
        gpu::kNearPlane,
        gpu::kFarPlane,
        static_cast<float>(gpu::kClusterSliceCount),
        static_cast<float>(gpu::DepthModeFlag(configuration.depthConvention)),
    };

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.IASetVertexBuffers(0U, 1U, &mesh_.vertexView);
    commandList.IASetIndexBuffer(&mesh_.indexView);

    std::vector<D3D12_TEXTURE_BARRIER> textureBarriers{
        gpu::MakeTextureBarrier(*slot.visibility.Get(), gpu::CommonState(), gpu::RenderTargetState()),
        gpu::MakeTextureBarrier(*slot.depth.Get(), gpu::CommonState(), gpu::DepthWriteState()),
        gpu::MakeTextureBarrier(*slot.output.Get(), gpu::CommonState(), gpu::UnorderedAccessState()),
    };
    gpu::SubmitTextureBarriers(commandList, textureBarriers);
    std::vector<D3D12_BUFFER_BARRIER> bufferBarriers{
        gpu::MakeBufferBarrier(*slot.diagnostics.Get(),
                               slot.diagnosticsInitialized ? gpu::CopySourceBufferState() : gpu::NoAccessState(),
                               gpu::ComputeUnorderedAccessState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);

    float const clearVisibility[]{0.0F, 0.0F, 0.0F, 0.0F};
    commandList.ClearRenderTargetView(slot.rtv.CpuHandle(), clearVisibility, 0U, nullptr);
    commandList.ClearDepthStencilView(slot.dsv.CpuHandle(), D3D12_CLEAR_FLAG_DEPTH,
                                      gpu::DepthClearValue(configuration.depthConvention), 0U, 0U, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE const visibilityRtv = slot.rtv.CpuHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE const dsv = slot.dsv.CpuHandle();
    commandList.OMSetRenderTargets(1U, &visibilityRtv, FALSE, &dsv);
    commandList.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    commandList.SetGraphicsRoot32BitConstants(GraphicsFrameConstants, sizeof(constants) / sizeof(std::uint32_t),
                                              &constants, 0U);
    commandList.SetPipelineState(configuration.depthConvention == gpu::DepthConvention::Forward
                                     ? rasterForwardPipeline_.Get()
                                     : rasterReversedPipeline_.Get());
    for (std::uint32_t drawIndex = 0U; drawIndex < activeDrawCount; ++drawIndex)
    {
        gpu::DrawData const &draw = diagnosticScene_.draws[drawIndex];
        gpu::RasterDrawConstants drawConstants{};
        drawConstants.identifiers = {drawIndex + 1U, 0U, 0U, 0U};
        drawConstants.baseTintAndRoughness = {
            draw.baseTintAndRoughness.x,
            draw.baseTintAndRoughness.y,
            draw.baseTintAndRoughness.z,
            draw.baseTintAndRoughness.w,
        };
        drawConstants.materialParameters = {
            draw.materialParameters.x,
            draw.materialParameters.y,
            draw.materialParameters.z,
            draw.materialParameters.w,
        };
        commandList.SetGraphicsRoot32BitConstants(GraphicsDrawConstants, sizeof(drawConstants) / sizeof(std::uint32_t),
                                                  &drawConstants, 0U);
        commandList.DrawIndexedInstanced(draw.range.indexCount, 1U, draw.range.indexOffset,
                                         static_cast<INT>(draw.range.vertexOffset), 0U);
    }

    textureBarriers = {
        gpu::MakeTextureBarrier(*slot.visibility.Get(), gpu::RenderTargetState(), gpu::ComputeShaderResourceState()),
        gpu::MakeTextureBarrier(*slot.depth.Get(), gpu::DepthWriteState(), gpu::ComputeShaderResourceState()),
    };
    gpu::SubmitTextureBarriers(commandList, textureBarriers);

    commandList.SetComputeRootSignature(computeRootSignature_.Get());
    commandList.SetComputeRoot32BitConstants(ComputeFrameConstants, sizeof(constants) / sizeof(std::uint32_t),
                                             &constants, 0U);
    commandList.SetComputeRootDescriptorTable(ComputeInputTable, slot.descriptors.GpuHandle(VisibilitySrv));
    commandList.SetComputeRootDescriptorTable(ComputeLightTable, slot.descriptors.GpuHandle(LightSrv));
    commandList.SetComputeRootDescriptorTable(ComputeUavTable, slot.descriptors.GpuHandle(OutputUav));
    commandList.SetPipelineState(shadePipeline_.Get());
    commandList.Dispatch((frameContext.drawableSize.width + 7U) / 8U, (frameContext.drawableSize.height + 7U) / 8U, 1U);

    textureBarriers = {
        gpu::MakeTextureBarrier(*slot.visibility.Get(), gpu::ComputeShaderResourceState(),
                                gpu::CopySourceTextureState()),
        gpu::MakeTextureBarrier(*slot.depth.Get(), gpu::ComputeShaderResourceState(), gpu::CopySourceTextureState()),
        gpu::MakeTextureBarrier(*slot.output.Get(), gpu::UnorderedAccessState(), gpu::CopySourceTextureState()),
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::FrameStartState(frameContext),
                                gpu::CopyDestTextureState()),
    };
    gpu::SubmitTextureBarriers(commandList, textureBarriers);
    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.diagnostics.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::CopySourceBufferState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);

    commandList.CopyResource(frameContext.renderTarget, slot.output.Get());
    gpu::CopyTextureToReadback(commandList, *slot.visibility.Get(), slot.visibilityReadback);
    gpu::CopyTextureToReadback(commandList, *slot.depth.Get(), slot.depthReadback);
    std::uint64_t const diagnosticBytes = static_cast<std::uint64_t>(frameContext.drawableSize.width) *
                                          frameContext.drawableSize.height * sizeof(gpu::PixelDiagnostics);
    commandList.CopyBufferRegion(slot.diagnosticsReadback.Get(), 0U, slot.diagnostics.Get(), 0U, diagnosticBytes);

    textureBarriers = {
        gpu::MakeTextureBarrier(*slot.visibility.Get(), gpu::CopySourceTextureState(), gpu::CommonState()),
        gpu::MakeTextureBarrier(*slot.depth.Get(), gpu::CopySourceTextureState(), gpu::CommonState()),
        gpu::MakeTextureBarrier(*slot.output.Get(), gpu::CopySourceTextureState(), gpu::CommonState()),
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::CopyDestTextureState(),
                                gpu::FrameEndState(frameContext)),
    };
    gpu::SubmitTextureBarriers(commandList, textureBarriers);

    slot.diagnosticsInitialized = true;
    lastRenderedFrameSlot_ = frameContext.frameSlot;
    lastRenderedCellCount_ = static_cast<std::uint32_t>(lists->cells.size());
    lastRenderedLightIndexCount_ = static_cast<std::uint32_t>(lists->lightIndices.size());
    lastRenderedPixelCount_ = frameContext.drawableSize.width * frameContext.drawableSize.height;
    lastProjection_ = projection;
    lastConfiguration_ = configuration;
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    ReleaseSizeDependentResources();
    materialTexture_.Reset();
    mesh_ = {};
    diagnosticScene_ = {};
    shadePipeline_.Reset();
    rasterReversedPipeline_.Reset();
    rasterForwardPipeline_.Reset();
    computeRootSignature_.Reset();
    graphicsRootSignature_.Reset();
    rasterVertexShader_ = {};
    rasterPixelShader_ = {};
    shadeShader_ = {};
    dsvHeap_ = {};
    rtvHeap_ = {};
    deviceResources_ = nullptr;
    (void)deviceResources;
}

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

std::expected<Renderer::ReadbackOutputs, lgp::framework::Error> Renderer::ReadBackOutputs()
{
    if (frameSlots_.empty() || lastRenderedPixelCount_ == 0U)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "No Chapter 15 Solution frame has been rendered."));
    }
    auto const idle = deviceResources_->WaitForGpuIdle();
    if (!idle)
    {
        return std::unexpected(idle.error());
    }
    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    auto lists = gpu::ResolveUploadedLightLists(slot.cells, slot.lightIndices, lastRenderedCellCount_,
                                                lastRenderedLightIndexCount_, lastConfiguration_.lightCount);
    auto diagnostics = gpu::ResolveDiagnostics(slot.diagnosticsReadback, lastRenderedPixelCount_);
    auto depth =
        gpu::ResolveTextureReadback(slot.depthReadback, deviceResources_->drawable_size(), gpu::kDepthResourceFormat);
    auto visibility =
        gpu::ResolveTextureReadback(slot.visibilityReadback, deviceResources_->drawable_size(), gpu::kVisibilityFormat);
    if (!lists)
    {
        return std::unexpected(lists.error());
    }
    if (!diagnostics)
    {
        return std::unexpected(diagnostics.error());
    }
    if (!depth)
    {
        return std::unexpected(depth.error());
    }
    if (!visibility)
    {
        return std::unexpected(visibility.error());
    }
    return ReadbackOutputs{
        .configuration = lastConfiguration_,
        .projection = lastProjection_,
        .lists = std::move(*lists),
        .lights = currentLights_,
        .diagnostics = std::move(*diagnostics),
        .depth = std::move(*depth),
        .visibility = std::move(*visibility),
        .frameSlot = lastRenderedFrameSlot_,
    };
}

} // namespace ch15::visibility_buffer::solution
