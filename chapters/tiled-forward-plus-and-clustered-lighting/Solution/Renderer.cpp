#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include <Windows.h>

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

namespace ch14::clustered_lighting::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorsPerSlot = 10U;

enum GraphicsRootParameter : UINT
{
    GraphicsFrameRootConstants = 0U,
    GraphicsObjectRootConstants = 1U,
    GraphicsSrvTable = 2U,
};

enum ComputeRootParameter : UINT
{
    ComputeFrameRootConstants = 0U,
    ComputeLightSrvTable = 1U,
    ComputeDepthSrvTable = 2U,
    ComputeUavTable = 3U,
};

enum DescriptorIndex : UINT
{
    LightSrv = 0U,
    CellSrv = 1U,
    IndexSrv = 2U,
    CountSrv = 3U,
    StatisticsSrv = 4U,
    DepthSrv = 5U,
    CountUav = 6U,
    CellUav = 7U,
    IndexUav = 8U,
    StatisticsUav = 9U,
};

struct FrameConstants final
{
    DirectX::XMFLOAT4X4 projection{};
    DirectX::XMFLOAT4 projectionData{};
    DirectX::XMFLOAT4 slicing{};
    DirectX::XMUINT4 dimensions{};
    DirectX::XMUINT4 counts{};
    DirectX::XMUINT4 options{};
};

struct GpuStatistics final
{
    std::uint32_t attemptedCount{};
    std::uint32_t emittedCount{};
    std::uint32_t overflowCount{};
    std::uint32_t cellCount{};
};

static_assert(sizeof(FrameConstants) == 144U);
static_assert(sizeof(GpuStatistics) == 16U);

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "ClusteredLightingLab.hlsl";
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] DirectX::XMMATRIX MakeProjectionMatrix(PerspectiveProjection projection,
                                                     ch12::gbuffer::DeviceDepthCoefficients coefficients) noexcept
{
    float const tangentHalfFov = std::tan(projection.verticalFieldOfViewRadians * 0.5F);
    float const xScale = 1.0F / (tangentHalfFov * projection.aspectRatio);
    float const yScale = 1.0F / tangentHalfFov;
    return DirectX::XMMATRIX(xScale, 0.0F, 0.0F, 0.0F, 0.0F, yScale, 0.0F, 0.0F, 0.0F, 0.0F, coefficients.additive,
                             1.0F, 0.0F, 0.0F, coefficients.reciprocal, 0.0F);
}

[[nodiscard]] D3D12_COMPARISON_FUNC DepthFunction(DepthConvention convention) noexcept
{
    return convention == DepthConvention::Forward ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                                  : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
}

[[nodiscard]] std::uint32_t DepthModeFlag(DepthConvention convention) noexcept
{
    return convention == DepthConvention::Forward ? 0U : 1U;
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
                                             std::string{"Failed to create the Chapter 14 "} + label + " pipeline."));
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

    if (auto status = gpu::CompileShader(compiler, options, L"DepthVS", L"vs_6_0", depthVertexShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"ForwardVS", L"vs_6_0", forwardVertexShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"ForwardPS", L"ps_6_0", forwardPixelShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"ResetListsCS", L"cs_6_0", resetShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"CountLightsCS", L"cs_6_0", countShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"PrefixCellsCS", L"cs_6_0", prefixShader_); !status)
    {
        return status;
    }
    return gpu::CompileShader(compiler, options, L"FillLightsCS", L"cs_6_0", fillShader_);
}

lgp::framework::Status Renderer::CreateGraphicsRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 3U;
    srvRange.BaseShaderRegister = 0U;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[GraphicsFrameRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[GraphicsFrameRootConstants].Constants.ShaderRegister = 0U;
    parameters[GraphicsFrameRootConstants].Constants.Num32BitValues = sizeof(FrameConstants) / sizeof(std::uint32_t);
    parameters[GraphicsFrameRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[GraphicsObjectRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[GraphicsObjectRootConstants].Constants.ShaderRegister = 1U;
    parameters[GraphicsObjectRootConstants].Constants.Num32BitValues = sizeof(gpu::ObjectData) / sizeof(std::uint32_t);
    parameters[GraphicsObjectRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[GraphicsSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[GraphicsSrvTable].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[GraphicsSrvTable].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[GraphicsSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
                                             "Failed to create the Chapter 14 graphics root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateComputeRootSignature()
{
    D3D12_DESCRIPTOR_RANGE lightRange{};
    lightRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    lightRange.NumDescriptors = 1U;
    lightRange.BaseShaderRegister = 0U;
    lightRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE depthRange{};
    depthRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depthRange.NumDescriptors = 1U;
    depthRange.BaseShaderRegister = 5U;
    depthRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 4U;
    uavRange.BaseShaderRegister = 0U;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[ComputeFrameRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[ComputeFrameRootConstants].Constants.ShaderRegister = 0U;
    parameters[ComputeFrameRootConstants].Constants.Num32BitValues = sizeof(FrameConstants) / sizeof(std::uint32_t);
    parameters[ComputeFrameRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ComputeLightSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[ComputeLightSrvTable].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[ComputeLightSrvTable].DescriptorTable.pDescriptorRanges = &lightRange;
    parameters[ComputeLightSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ComputeDepthSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[ComputeDepthSrvTable].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[ComputeDepthSrvTable].DescriptorTable.pDescriptorRanges = &depthRange;
    parameters[ComputeDepthSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
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
                                             "Failed to create the Chapter 14 compute root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreatePipelines()
{
    D3D12_INPUT_ELEMENT_DESC const inputLayout[]{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    };
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_BLEND_DESC depthBlend{};
    D3D12_DEPTH_STENCIL_DESC depthState{};
    depthState.DepthEnable = TRUE;
    depthState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC depthDescription{};
    depthDescription.pRootSignature = graphicsRootSignature_.Get();
    depthDescription.VS = depthVertexShader_.Bytecode();
    depthDescription.BlendState = depthBlend;
    depthDescription.SampleMask = UINT_MAX;
    depthDescription.RasterizerState = rasterizer;
    depthDescription.DepthStencilState = depthState;
    depthDescription.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    depthDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    depthDescription.DSVFormat = gpu::kDepthDsvFormat;
    depthDescription.SampleDesc.Count = 1U;

    depthDescription.DepthStencilState.DepthFunc = DepthFunction(DepthConvention::Forward);
    HRESULT const depthForwardResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &depthDescription, IID_PPV_ARGS(depthForwardPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(depthForwardResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", depthForwardResult,
                                             "Failed to create the Chapter 14 forward depth-prepass pipeline."));
    }
    depthDescription.DepthStencilState.DepthFunc = DepthFunction(DepthConvention::Reversed);
    HRESULT const depthReversedResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &depthDescription, IID_PPV_ARGS(depthReversedPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(depthReversedResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", depthReversedResult,
                                             "Failed to create the Chapter 14 reversed depth-prepass pipeline."));
    }

    D3D12_BLEND_DESC shadeBlend{};
    shadeBlend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_DEPTH_STENCIL_DESC shadeDepth{};
    shadeDepth.DepthEnable = TRUE;
    shadeDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadeDescription{};
    shadeDescription.pRootSignature = graphicsRootSignature_.Get();
    shadeDescription.VS = forwardVertexShader_.Bytecode();
    shadeDescription.PS = forwardPixelShader_.Bytecode();
    shadeDescription.BlendState = shadeBlend;
    shadeDescription.SampleMask = UINT_MAX;
    shadeDescription.RasterizerState = rasterizer;
    shadeDescription.DepthStencilState = shadeDepth;
    shadeDescription.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    shadeDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    shadeDescription.NumRenderTargets = 1U;
    shadeDescription.RTVFormats[0] = deviceResources_->back_buffer_format();
    shadeDescription.DSVFormat = gpu::kDepthDsvFormat;
    shadeDescription.SampleDesc.Count = 1U;

    shadeDescription.DepthStencilState.DepthFunc = DepthFunction(DepthConvention::Forward);
    HRESULT const shadeForwardResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &shadeDescription, IID_PPV_ARGS(shadeForwardPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(shadeForwardResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", shadeForwardResult,
                                             "Failed to create the Chapter 14 forward shading pipeline."));
    }
    shadeDescription.DepthStencilState.DepthFunc = DepthFunction(DepthConvention::Reversed);
    HRESULT const shadeReversedResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &shadeDescription, IID_PPV_ARGS(shadeReversedPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(shadeReversedResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", shadeReversedResult,
                                             "Failed to create the Chapter 14 reversed shading pipeline."));
    }

    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), resetShader_,
                                            "list reset and canary", resetPipeline_);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), countShader_,
                                            "cell count", countPipeline_);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), prefixShader_,
                                            "stable cell-major prefix", prefixPipeline_);
        !status)
    {
        return status;
    }
    return CreateComputePipeline(*deviceResources_->device(), *computeRootSignature_.Get(), fillShader_,
                                 "bounded cell fill", fillPipeline_);
}

lgp::framework::Status Renderer::CreateGeometry()
{
    auto const vertices = gpu::BuildUnitQuadVertices();
    auto const indices = gpu::BuildUnitQuadIndices();
    auto vertexBuffer = lgp::framework::CreateUploadBuffer(*deviceResources_->device(), sizeof(vertices),
                                                           L"Ch14 Solution quad vertices");
    if (!vertexBuffer)
    {
        return std::unexpected(std::move(vertexBuffer.error()));
    }
    quad_.vertices = std::move(*vertexBuffer);
    if (auto status = lgp::framework::WriteBuffer(quad_.vertices, std::span<gpu::Vertex const>{vertices}); !status)
    {
        return status;
    }
    auto indexBuffer =
        lgp::framework::CreateUploadBuffer(*deviceResources_->device(), sizeof(indices), L"Ch14 Solution quad indices");
    if (!indexBuffer)
    {
        return std::unexpected(std::move(indexBuffer.error()));
    }
    quad_.indices = std::move(*indexBuffer);
    if (auto status = lgp::framework::WriteBuffer(quad_.indices, std::span<std::uint32_t const>{indices}); !status)
    {
        return status;
    }
    quad_.vertexView.BufferLocation = quad_.vertices.gpu_virtual_address();
    quad_.vertexView.SizeInBytes = static_cast<UINT>(sizeof(vertices));
    quad_.vertexView.StrideInBytes = sizeof(gpu::Vertex);
    quad_.indexView.BufferLocation = quad_.indices.gpu_virtual_address();
    quad_.indexView.SizeInBytes = static_cast<UINT>(sizeof(indices));
    quad_.indexView.Format = DXGI_FORMAT_R32_UINT;
    quad_.indexCount = static_cast<UINT>(indices.size());
    return {};
}

void Renderer::ReleaseSizeDependentResources() noexcept
{
    for (FrameSlotResources &slot : frameSlots_)
    {
        slot.depth.Reset();
        if (slot.dsvs)
        {
            dsvHeap_.Free(slot.dsvs);
        }
        if (slot.descriptors)
        {
            deviceResources_->shader_visible_cbv_srv_uav_heap().Free(slot.descriptors);
        }
        slot.dsvs = {};
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
    auto const allocatedCellCount = gpu::CellCountForMode(gpu::LightingMode::Clustered, size);
    if (!allocatedCellCount)
    {
        return std::unexpected(allocatedCellCount.error());
    }

    frameSlots_.resize(deviceResources_->back_buffer_count());
    D3D12_HEAP_PROPERTIES textureHeap{};
    textureHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (FrameSlotResources &slot : frameSlots_)
    {
        slot.allocatedCellCount = *allocatedCellCount;
        auto dsvs = dsvHeap_.Allocate(2U);
        if (!dsvs)
        {
            return std::unexpected(std::move(dsvs.error()));
        }
        slot.dsvs = *dsvs;
        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorsPerSlot);
        if (!descriptors)
        {
            return std::unexpected(std::move(descriptors.error()));
        }
        slot.descriptors = *descriptors;

        D3D12_RESOURCE_DESC1 const depthDescription =
            gpu::MakeTextureDescription(size, gpu::kDepthResourceFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        HRESULT const depthResult = deviceResources_->device()->CreateCommittedResource3(
            &textureHeap, D3D12_HEAP_FLAG_NONE, &depthDescription, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0U,
            nullptr, IID_PPV_ARGS(slot.depth.ReleaseAndGetAddressOf()));
        if (FAILED(depthResult))
        {
            return std::unexpected(
                lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", depthResult,
                                                 "Failed to create a Chapter 14 Solution depth resource."));
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC writeDsv{};
        writeDsv.Format = gpu::kDepthDsvFormat;
        writeDsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateDepthStencilView(slot.depth.Get(), &writeDsv, slot.dsvs.CpuHandle(0U));
        D3D12_DEPTH_STENCIL_VIEW_DESC readDsv = writeDsv;
        readDsv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        deviceResources_->device()->CreateDepthStencilView(slot.depth.Get(), &readDsv, slot.dsvs.CpuHandle(1U));

        auto lights =
            gpu::CreateBuffer(*deviceResources_->device(),
                              static_cast<std::uint64_t>(gpu::kMaximumLightCount) * sizeof(gpu::PointLightData),
                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch14 Solution per-slot lights", true);
        auto counts = gpu::CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(*allocatedCellCount) * sizeof(std::uint32_t),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch14 per-cell attempted counts");
        auto cells = gpu::CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(*allocatedCellCount) * sizeof(CellLightRange),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch14 stable cell ranges");
        auto indices = gpu::CreateBuffer(
            *deviceResources_->device(),
            static_cast<std::uint64_t>(gpu::kMaximumLightIndexCapacity) * sizeof(std::uint32_t),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch14 bounded light indices");
        auto statistics = gpu::CreateBuffer(*deviceResources_->device(), sizeof(GpuStatistics), D3D12_HEAP_TYPE_DEFAULT,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch14 list statistics");
        auto countsReadback = gpu::CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(*allocatedCellCount) * sizeof(std::uint32_t),
            D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, L"Ch14 attempted-count readback", true);
        auto cellsReadback = gpu::CreateBuffer(
            *deviceResources_->device(), static_cast<std::uint64_t>(*allocatedCellCount) * sizeof(CellLightRange),
            D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, L"Ch14 cell-range readback", true);
        auto indicesReadback =
            gpu::CreateBuffer(*deviceResources_->device(),
                              static_cast<std::uint64_t>(gpu::kMaximumLightIndexCapacity) * sizeof(std::uint32_t),
                              D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, L"Ch14 light-index readback", true);
        auto statisticsReadback =
            gpu::CreateBuffer(*deviceResources_->device(), sizeof(GpuStatistics), D3D12_HEAP_TYPE_READBACK,
                              D3D12_RESOURCE_FLAG_NONE, L"Ch14 statistics readback", true);
        if (!lights)
        {
            return std::unexpected(std::move(lights.error()));
        }
        if (!counts)
        {
            return std::unexpected(std::move(counts.error()));
        }
        if (!cells)
        {
            return std::unexpected(std::move(cells.error()));
        }
        if (!indices)
        {
            return std::unexpected(std::move(indices.error()));
        }
        if (!statistics)
        {
            return std::unexpected(std::move(statistics.error()));
        }
        if (!countsReadback)
        {
            return std::unexpected(std::move(countsReadback.error()));
        }
        if (!cellsReadback)
        {
            return std::unexpected(std::move(cellsReadback.error()));
        }
        if (!indicesReadback)
        {
            return std::unexpected(std::move(indicesReadback.error()));
        }
        if (!statisticsReadback)
        {
            return std::unexpected(std::move(statisticsReadback.error()));
        }
        slot.lights = std::move(*lights);
        slot.counts = std::move(*counts);
        slot.cells = std::move(*cells);
        slot.indices = std::move(*indices);
        slot.statistics = std::move(*statistics);
        slot.countsReadback = std::move(*countsReadback);
        slot.cellsReadback = std::move(*cellsReadback);
        slot.indicesReadback = std::move(*indicesReadback);
        slot.statisticsReadback = std::move(*statisticsReadback);

        D3D12_SHADER_RESOURCE_VIEW_DESC structuredSrv{};
        structuredSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        structuredSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        structuredSrv.Format = DXGI_FORMAT_UNKNOWN;
        structuredSrv.Buffer.NumElements = gpu::kMaximumLightCount;
        structuredSrv.Buffer.StructureByteStride = sizeof(gpu::PointLightData);
        deviceResources_->device()->CreateShaderResourceView(slot.lights.Get(), &structuredSrv,
                                                             slot.descriptors.CpuHandle(LightSrv));
        structuredSrv.Buffer.NumElements = *allocatedCellCount;
        structuredSrv.Buffer.StructureByteStride = sizeof(CellLightRange);
        deviceResources_->device()->CreateShaderResourceView(slot.cells.Get(), &structuredSrv,
                                                             slot.descriptors.CpuHandle(CellSrv));
        structuredSrv.Buffer.NumElements = gpu::kMaximumLightIndexCapacity;
        structuredSrv.Buffer.StructureByteStride = sizeof(std::uint32_t);
        deviceResources_->device()->CreateShaderResourceView(slot.indices.Get(), &structuredSrv,
                                                             slot.descriptors.CpuHandle(IndexSrv));
        structuredSrv.Buffer.NumElements = *allocatedCellCount;
        deviceResources_->device()->CreateShaderResourceView(slot.counts.Get(), &structuredSrv,
                                                             slot.descriptors.CpuHandle(CountSrv));
        structuredSrv.Buffer.NumElements = 4U;
        deviceResources_->device()->CreateShaderResourceView(slot.statistics.Get(), &structuredSrv,
                                                             slot.descriptors.CpuHandle(StatisticsSrv));

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
        depthSrv.Format = gpu::kDepthSrvFormat;
        depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrv.Texture2D.MipLevels = 1U;
        deviceResources_->device()->CreateShaderResourceView(slot.depth.Get(), &depthSrv,
                                                             slot.descriptors.CpuHandle(DepthSrv));

        D3D12_UNORDERED_ACCESS_VIEW_DESC structuredUav{};
        structuredUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        structuredUav.Format = DXGI_FORMAT_UNKNOWN;
        structuredUav.Buffer.NumElements = *allocatedCellCount;
        structuredUav.Buffer.StructureByteStride = sizeof(std::uint32_t);
        deviceResources_->device()->CreateUnorderedAccessView(slot.counts.Get(), nullptr, &structuredUav,
                                                              slot.descriptors.CpuHandle(CountUav));
        structuredUav.Buffer.StructureByteStride = sizeof(CellLightRange);
        deviceResources_->device()->CreateUnorderedAccessView(slot.cells.Get(), nullptr, &structuredUav,
                                                              slot.descriptors.CpuHandle(CellUav));
        structuredUav.Buffer.NumElements = gpu::kMaximumLightIndexCapacity;
        structuredUav.Buffer.StructureByteStride = sizeof(std::uint32_t);
        deviceResources_->device()->CreateUnorderedAccessView(slot.indices.Get(), nullptr, &structuredUav,
                                                              slot.descriptors.CpuHandle(IndexUav));
        structuredUav.Buffer.NumElements = 4U;
        deviceResources_->device()->CreateUnorderedAccessView(slot.statistics.Get(), nullptr, &structuredUav,
                                                              slot.descriptors.CpuHandle(StatisticsUav));
    }
    return {};
}

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    gpu::LabConfiguration configuration{};
    configuration.mode = interactiveMode_;
    configuration.debugView = interactiveDebugView_;
    if (headless_ && headlessConfiguration_.has_value())
    {
        configuration.mode = headlessConfiguration_->mode;
        configuration.scene = headlessConfiguration_->scene;
        configuration.lights = headlessConfiguration_->lights;
        configuration.depthConvention = headlessConfiguration_->depthConvention;
        configuration.debugView = headlessConfiguration_->debugView;
        configuration.lightCount = gpu::NormalizeLightCount(headlessConfiguration_->lightCount);
        configuration.capacity = gpu::NormalizeCapacity(headlessConfiguration_->capacity);
    }
    return configuration;
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    if (auto const slicing = ValidateLogDepthSlicing(gpu::MakeSlicing()); !slicing)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLogDepthSlicing", "The Chapter 14 logarithmic slices are invalid."));
    }
    auto dsvHeap = lgp::framework::CreateDescriptorHeap(*deviceResources_->device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                                        deviceResources_->back_buffer_count() * 2U, false,
                                                        L"Ch14 Solution DSV heap");
    if (!dsvHeap)
    {
        return std::unexpected(std::move(dsvHeap.error()));
    }
    dsvHeap_ = std::move(*dsvHeap);
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
    return CreateGeometry();
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
            interactiveMode_ = gpu::LightingMode::BruteForce;
        }
        if (context.input.WasKeyPressed('2'))
        {
            interactiveMode_ = gpu::LightingMode::Tiled;
        }
        if (context.input.WasKeyPressed('3'))
        {
            interactiveMode_ = gpu::LightingMode::Clustered;
        }
        if (context.input.WasKeyPressed('O'))
        {
            interactiveDebugView_ = gpu::DebugView::Occupancy;
        }
        if (context.input.WasKeyPressed('V'))
        {
            interactiveDebugView_ = gpu::DebugView::Overflow;
        }
        if (context.input.WasKeyPressed('F'))
        {
            interactiveDebugView_ = gpu::DebugView::Final;
        }
    }

    gpu::LabConfiguration const configuration = ActiveConfiguration();
    currentScene_ = gpu::BuildScene(configuration.scene);
    currentLights_ = gpu::BuildLights(configuration.lights, configuration.lightCount);
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr ||
        frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 14 Solution frame is invalid."));
    }

    gpu::LabConfiguration configuration = ActiveConfiguration();
    configuration.lightCount = static_cast<std::uint32_t>(currentLights_.size());
    configuration.capacity = gpu::NormalizeCapacity(configuration.capacity);
    gpu::LightingMode const listMode =
        configuration.mode == gpu::LightingMode::Clustered ? gpu::LightingMode::Clustered : gpu::LightingMode::Tiled;
    auto const cellCount = gpu::CellCountForMode(listMode, frameContext.drawableSize);
    auto const tiles = MakeTileGrid(frameContext.drawableSize.width, frameContext.drawableSize.height, gpu::kTileWidth,
                                    gpu::kTileHeight);
    if (!cellCount || !tiles)
    {
        return std::unexpected(
            lgp::framework::MakeError("Render", "The Chapter 14 Solution grid configuration is invalid."));
    }

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    if (*cellCount > slot.allocatedCellCount)
    {
        return std::unexpected(
            lgp::framework::MakeError("Render", "The Chapter 14 cell count exceeds the per-slot allocation."));
    }
    if (auto status = gpu::WriteBuffer(slot.lights, std::span<gpu::PointLightData const>{currentLights_}); !status)
    {
        return status;
    }

    PerspectiveProjection const projection =
        gpu::MakeProjection(frameContext.drawableSize, configuration.depthConvention);
    auto const coefficients = MakeDeviceDepthCoefficients(projection);
    if (!coefficients)
    {
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 14 Solution projection is invalid."));
    }

    FrameConstants constants{};
    DirectX::XMStoreFloat4x4(&constants.projection, MakeProjectionMatrix(projection, *coefficients));
    float const tangentHalfFov = std::tan(projection.verticalFieldOfViewRadians * 0.5F);
    constants.projectionData = {
        tangentHalfFov * projection.aspectRatio,
        tangentHalfFov,
        coefficients->additive,
        coefficients->reciprocal,
    };
    constants.slicing = {
        gpu::kNearPlane,
        gpu::kFarPlane,
        static_cast<float>(gpu::kClusterSliceCount),
        0.0F,
    };
    constants.dimensions = {
        frameContext.drawableSize.width,
        frameContext.drawableSize.height,
        tiles->tileCountX,
        tiles->tileCountY,
    };
    constants.counts = {
        configuration.lightCount,
        tiles->tileCount,
        *cellCount,
        configuration.capacity,
    };
    constants.options = {
        static_cast<std::uint32_t>(configuration.mode),
        DepthModeFlag(configuration.depthConvention),
        static_cast<std::uint32_t>(configuration.debugView),
        slot.allocatedCellCount,
    };

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.IASetVertexBuffers(0U, 1U, &quad_.vertexView);
    commandList.IASetIndexBuffer(&quad_.indexView);

    std::vector<D3D12_TEXTURE_BARRIER> textureBarriers{
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::FrameStartState(frameContext),
                                gpu::RenderTargetState()),
        gpu::MakeTextureBarrier(*slot.depth.Get(), gpu::CommonState(), gpu::DepthWriteState()),
    };
    gpu::SubmitTextureBarriers(commandList, textureBarriers);

    gpu::BufferBarrierState const writableBefore =
        slot.writableStateInitialized ? gpu::ComputeUnorderedAccessState() : gpu::NoAccessState();
    std::vector<D3D12_BUFFER_BARRIER> bufferBarriers{
        gpu::MakeBufferBarrier(*slot.counts.Get(), writableBefore, gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.cells.Get(), writableBefore, gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.indices.Get(), writableBefore, gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.statistics.Get(), writableBefore, gpu::ComputeUnorderedAccessState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);

    float const clearColor[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    commandList.ClearDepthStencilView(slot.dsvs.CpuHandle(0U), D3D12_CLEAR_FLAG_DEPTH,
                                      DepthClearValue(configuration.depthConvention), 0U, 0U, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE const depthWriteDsv = slot.dsvs.CpuHandle(0U);
    commandList.OMSetRenderTargets(0U, nullptr, FALSE, &depthWriteDsv);
    commandList.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    commandList.SetGraphicsRoot32BitConstants(GraphicsFrameRootConstants, sizeof(constants) / sizeof(std::uint32_t),
                                              &constants, 0U);
    commandList.SetPipelineState(configuration.depthConvention == DepthConvention::Forward
                                     ? depthForwardPipeline_.Get()
                                     : depthReversedPipeline_.Get());
    for (gpu::SceneObject const &object : currentScene_)
    {
        commandList.SetGraphicsRoot32BitConstants(GraphicsObjectRootConstants,
                                                  sizeof(gpu::ObjectData) / sizeof(std::uint32_t), &object.data, 0U);
        commandList.DrawIndexedInstanced(quad_.indexCount, 1U, 0U, 0, 0U);
    }

    textureBarriers = {
        gpu::MakeTextureBarrier(*slot.depth.Get(), gpu::DepthWriteState(), gpu::ComputeShaderResourceState()),
    };
    gpu::SubmitTextureBarriers(commandList, textureBarriers);

    commandList.SetComputeRootSignature(computeRootSignature_.Get());
    commandList.SetComputeRoot32BitConstants(ComputeFrameRootConstants, sizeof(constants) / sizeof(std::uint32_t),
                                             &constants, 0U);
    commandList.SetComputeRootDescriptorTable(ComputeLightSrvTable, slot.descriptors.GpuHandle(LightSrv));
    commandList.SetComputeRootDescriptorTable(ComputeDepthSrvTable, slot.descriptors.GpuHandle(DepthSrv));
    commandList.SetComputeRootDescriptorTable(ComputeUavTable, slot.descriptors.GpuHandle(CountUav));
    commandList.SetPipelineState(resetPipeline_.Get());
    commandList.Dispatch((std::max(slot.allocatedCellCount, gpu::kMaximumLightIndexCapacity) + 63U) / 64U, 1U, 1U);

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.counts.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.cells.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.indices.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.statistics.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);
    commandList.SetPipelineState(countPipeline_.Get());
    commandList.Dispatch((*cellCount + 63U) / 64U, 1U, 1U);

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.counts.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);
    commandList.SetPipelineState(prefixPipeline_.Get());
    commandList.Dispatch(1U, 1U, 1U);

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.cells.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::ComputeUnorderedAccessState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);
    commandList.SetPipelineState(fillPipeline_.Get());
    commandList.Dispatch((*cellCount + 63U) / 64U, 1U, 1U);

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.cells.Get(), gpu::ComputeUnorderedAccessState(), gpu::PixelShaderResourceState()),
        gpu::MakeBufferBarrier(*slot.indices.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::PixelShaderResourceState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);
    textureBarriers = {
        gpu::MakeTextureBarrier(*slot.depth.Get(), gpu::ComputeShaderResourceState(), gpu::DepthReadState()),
    };
    gpu::SubmitTextureBarriers(commandList, textureBarriers);

    D3D12_CPU_DESCRIPTOR_HANDLE const depthReadDsv = slot.dsvs.CpuHandle(1U);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, &depthReadDsv);
    commandList.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    commandList.SetGraphicsRoot32BitConstants(GraphicsFrameRootConstants, sizeof(constants) / sizeof(std::uint32_t),
                                              &constants, 0U);
    commandList.SetGraphicsRootDescriptorTable(GraphicsSrvTable, slot.descriptors.GpuHandle(LightSrv));
    commandList.SetPipelineState(configuration.depthConvention == DepthConvention::Forward
                                     ? shadeForwardPipeline_.Get()
                                     : shadeReversedPipeline_.Get());
    for (gpu::SceneObject const &object : currentScene_)
    {
        commandList.SetGraphicsRoot32BitConstants(GraphicsObjectRootConstants,
                                                  sizeof(gpu::ObjectData) / sizeof(std::uint32_t), &object.data, 0U);
        commandList.DrawIndexedInstanced(quad_.indexCount, 1U, 0U, 0, 0U);
    }

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.counts.Get(), gpu::ComputeUnorderedAccessState(), gpu::CopySourceBufferState()),
        gpu::MakeBufferBarrier(*slot.cells.Get(), gpu::PixelShaderResourceState(), gpu::CopySourceBufferState()),
        gpu::MakeBufferBarrier(*slot.indices.Get(), gpu::PixelShaderResourceState(), gpu::CopySourceBufferState()),
        gpu::MakeBufferBarrier(*slot.statistics.Get(), gpu::ComputeUnorderedAccessState(),
                               gpu::CopySourceBufferState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);
    commandList.CopyBufferRegion(slot.countsReadback.Get(), 0U, slot.counts.Get(), 0U, slot.counts.size_in_bytes());
    commandList.CopyBufferRegion(slot.cellsReadback.Get(), 0U, slot.cells.Get(), 0U, slot.cells.size_in_bytes());
    commandList.CopyBufferRegion(slot.indicesReadback.Get(), 0U, slot.indices.Get(), 0U, slot.indices.size_in_bytes());
    commandList.CopyBufferRegion(slot.statisticsReadback.Get(), 0U, slot.statistics.Get(), 0U,
                                 slot.statistics.size_in_bytes());

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.counts.Get(), gpu::CopySourceBufferState(), gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.cells.Get(), gpu::CopySourceBufferState(), gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.indices.Get(), gpu::CopySourceBufferState(), gpu::ComputeUnorderedAccessState()),
        gpu::MakeBufferBarrier(*slot.statistics.Get(), gpu::CopySourceBufferState(),
                               gpu::ComputeUnorderedAccessState()),
    };
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);
    textureBarriers = {
        gpu::MakeTextureBarrier(*slot.depth.Get(), gpu::DepthReadState(), gpu::CommonState()),
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(), gpu::FrameEndState(frameContext)),
    };
    gpu::SubmitTextureBarriers(commandList, textureBarriers);

    slot.writableStateInitialized = true;
    lastRenderedFrameSlot_ = frameContext.frameSlot;
    lastRenderedCellCount_ = *cellCount;
    lastProjection_ = projection;
    lastConfiguration_ = configuration;
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    ReleaseSizeDependentResources();
    quad_ = {};
    fillPipeline_.Reset();
    prefixPipeline_.Reset();
    countPipeline_.Reset();
    resetPipeline_.Reset();
    shadeReversedPipeline_.Reset();
    shadeForwardPipeline_.Reset();
    depthReversedPipeline_.Reset();
    depthForwardPipeline_.Reset();
    computeRootSignature_.Reset();
    graphicsRootSignature_.Reset();
    depthVertexShader_ = {};
    forwardVertexShader_ = {};
    forwardPixelShader_ = {};
    resetShader_ = {};
    countShader_ = {};
    prefixShader_ = {};
    fillShader_ = {};
    dsvHeap_ = {};
    deviceResources_ = nullptr;
    (void)deviceResources;
}

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

std::expected<Renderer::ReadbackOutputs, lgp::framework::Error> Renderer::ReadBackOutputs()
{
    if (frameSlots_.empty() || lastRenderedCellCount_ == 0U)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "No Chapter 14 Solution frame has been rendered."));
    }
    auto const idle = deviceResources_->WaitForGpuIdle();
    if (!idle)
    {
        return std::unexpected(std::move(idle.error()));
    }

    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    auto const *statistics = reinterpret_cast<GpuStatistics const *>(slot.statisticsReadback.mapped_data());
    auto const *attemptedCounts = reinterpret_cast<std::uint32_t const *>(slot.countsReadback.mapped_data());
    auto const *cells = reinterpret_cast<CellLightRange const *>(slot.cellsReadback.mapped_data());
    auto const *indices = reinterpret_cast<std::uint32_t const *>(slot.indicesReadback.mapped_data());
    if (statistics == nullptr || attemptedCounts == nullptr || cells == nullptr || indices == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "A Chapter 14 readback buffer is not mapped."));
    }
    if (statistics->cellCount != lastRenderedCellCount_ || statistics->emittedCount > gpu::kMaximumLightIndexCapacity)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "The Chapter 14 GPU statistics are malformed."));
    }

    ReadbackOutputs outputs{};
    outputs.attemptedCounts.assign(attemptedCounts, attemptedCounts + lastRenderedCellCount_);
    outputs.lists.cells.assign(cells, cells + lastRenderedCellCount_);
    outputs.lists.lightIndices.assign(indices, indices + statistics->emittedCount);
    outputs.lists.statistics = {
        statistics->attemptedCount,
        statistics->emittedCount,
        statistics->overflowCount,
    };
    auto const validation = ValidateBoundedLightLists(outputs.lists, lastConfiguration_.lightCount);
    if (!validation)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "The Chapter 14 GPU light list failed validation."));
    }
    for (std::uint32_t cellIndex = 0U; cellIndex < lastRenderedCellCount_; ++cellIndex)
    {
        if (outputs.attemptedCounts[cellIndex] != outputs.lists.cells[cellIndex].attemptedCount)
        {
            return std::unexpected(
                lgp::framework::MakeError("ReadBackOutputs", "The Chapter 14 count and range buffers disagree."));
        }
    }
    CellLightRange const canaryRange{
        gpu::kBufferCanary,
        gpu::kBufferCanary,
        gpu::kBufferCanary,
        gpu::kBufferCanary,
    };
    for (std::uint32_t cellIndex = lastRenderedCellCount_; cellIndex < slot.allocatedCellCount; ++cellIndex)
    {
        if (attemptedCounts[cellIndex] != gpu::kBufferCanary || cells[cellIndex] != canaryRange)
        {
            return std::unexpected(
                lgp::framework::MakeError("ReadBackOutputs", "The Chapter 14 GPU wrote beyond the active cell range."));
        }
    }
    for (std::uint32_t index = statistics->emittedCount; index < gpu::kMaximumLightIndexCapacity; ++index)
    {
        if (indices[index] != gpu::kBufferCanary)
        {
            return std::unexpected(lgp::framework::MakeError(
                "ReadBackOutputs", "The Chapter 14 GPU wrote beyond the emitted light-index range."));
        }
    }

    auto depth = gpu::ReadBackTexture(*deviceResources_, *slot.depth.Get(), gpu::CommonState());
    if (!depth)
    {
        return std::unexpected(std::move(depth.error()));
    }
    outputs.depth = std::move(*depth);
    outputs.lights = currentLights_;
    outputs.projection = lastProjection_;
    outputs.configuration = lastConfiguration_;
    outputs.cellCount = lastRenderedCellCount_;
    outputs.unusedStorageIntact = true;
    return outputs;
}

} // namespace ch14::clustered_lighting::solution
