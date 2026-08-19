#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

namespace ch14::clustered_lighting::starter
{
namespace
{

using Microsoft::WRL::ComPtr;

enum RootParameter : UINT
{
    FrameRootConstants = 0U,
    ObjectRootConstants = 1U,
    LightSrvTable = 2U,
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

static_assert(sizeof(FrameConstants) == 144U);

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

    if (auto status = gpu::CompileShader(compiler, options, L"ForwardVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    return gpu::CompileShader(compiler, options, L"ForwardPS", L"ps_6_0", pixelShader_);
}

lgp::framework::Status Renderer::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE lightRange{};
    lightRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    lightRange.NumDescriptors = 1U;
    lightRange.BaseShaderRegister = 0U;
    lightRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[FrameRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[FrameRootConstants].Constants.ShaderRegister = 0U;
    parameters[FrameRootConstants].Constants.Num32BitValues = sizeof(FrameConstants) / sizeof(std::uint32_t);
    parameters[FrameRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ObjectRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[ObjectRootConstants].Constants.ShaderRegister = 1U;
    parameters[ObjectRootConstants].Constants.Num32BitValues = sizeof(gpu::ObjectData) / sizeof(std::uint32_t);
    parameters[ObjectRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[LightSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[LightSrvTable].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[LightSrvTable].DescriptorTable.pDescriptorRanges = &lightRange;
    parameters[LightSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
                                                        IID_PPV_ARGS(rootSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                             "Failed to create the Chapter 14 Starter root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreatePipelines()
{
    D3D12_INPUT_ELEMENT_DESC const inputLayout[]{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    };
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature_.Get();
    description.VS = vertexShader_.Bytecode();
    description.PS = pixelShader_.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = deviceResources_->back_buffer_format();
    description.DSVFormat = gpu::kDepthDsvFormat;
    description.SampleDesc.Count = 1U;

    description.DepthStencilState.DepthFunc = DepthFunction(DepthConvention::Forward);
    HRESULT const forwardResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(forwardPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(forwardResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", forwardResult,
                                             "Failed to create the Chapter 14 Starter forward-depth pipeline."));
    }

    description.DepthStencilState.DepthFunc = DepthFunction(DepthConvention::Reversed);
    HRESULT const reversedResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(reversedPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(reversedResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", reversedResult,
                                             "Failed to create the Chapter 14 Starter reversed-depth pipeline."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateGeometry()
{
    auto const vertices = gpu::BuildUnitQuadVertices();
    auto const indices = gpu::BuildUnitQuadIndices();
    auto vertexBuffer = lgp::framework::CreateUploadBuffer(*deviceResources_->device(), sizeof(vertices),
                                                           L"Ch14 Starter quad vertices");
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
        lgp::framework::CreateUploadBuffer(*deviceResources_->device(), sizeof(indices), L"Ch14 Starter quad indices");
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
        if (slot.dsv)
        {
            dsvHeap_.Free(slot.dsv);
        }
        if (slot.lightSrv)
        {
            deviceResources_->shader_visible_cbv_srv_uav_heap().Free(slot.lightSrv);
        }
        slot.dsv = {};
        slot.lightSrv = {};
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

    frameSlots_.resize(deviceResources_->back_buffer_count());
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (FrameSlotResources &slot : frameSlots_)
    {
        auto dsv = dsvHeap_.Allocate(1U);
        if (!dsv)
        {
            return std::unexpected(std::move(dsv.error()));
        }
        slot.dsv = *dsv;
        auto lightSrv = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(1U);
        if (!lightSrv)
        {
            return std::unexpected(std::move(lightSrv.error()));
        }
        slot.lightSrv = *lightSrv;

        D3D12_RESOURCE_DESC1 const description =
            gpu::MakeTextureDescription(size, gpu::kDepthResourceFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        HRESULT const depthResult = deviceResources_->device()->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0U, nullptr,
            IID_PPV_ARGS(slot.depth.ReleaseAndGetAddressOf()));
        if (FAILED(depthResult))
        {
            return std::unexpected(
                lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", depthResult,
                                                 "Failed to create a Chapter 14 Starter depth resource."));
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDescription{};
        dsvDescription.Format = gpu::kDepthDsvFormat;
        dsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateDepthStencilView(slot.depth.Get(), &dsvDescription, slot.dsv.cpuHandle);

        auto lights =
            gpu::CreateBuffer(*deviceResources_->device(),
                              static_cast<std::uint64_t>(gpu::kMaximumLightCount) * sizeof(gpu::PointLightData),
                              D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch14 Starter per-slot lights", true);
        if (!lights)
        {
            return std::unexpected(std::move(lights.error()));
        }
        slot.lights = std::move(*lights);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Buffer.NumElements = gpu::kMaximumLightCount;
        srv.Buffer.StructureByteStride = sizeof(gpu::PointLightData);
        deviceResources_->device()->CreateShaderResourceView(slot.lights.Get(), &srv, slot.lightSrv.cpuHandle);
    }
    return {};
}

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    gpu::LabConfiguration configuration{};
    configuration.mode = gpu::LightingMode::BruteForce;
    if (headless_ && headlessConfiguration_.has_value())
    {
        configuration.scene = headlessConfiguration_->scene;
        configuration.lights = headlessConfiguration_->lights;
        configuration.depthConvention = headlessConfiguration_->depthConvention;
        configuration.lightCount = gpu::NormalizeLightCount(headlessConfiguration_->lightCount);
    }
    return configuration;
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    auto dsvHeap =
        lgp::framework::CreateDescriptorHeap(*deviceResources_->device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                             deviceResources_->back_buffer_count(), false, L"Ch14 Starter DSV heap");
    if (!dsvHeap)
    {
        return std::unexpected(std::move(dsvHeap.error()));
    }
    dsvHeap_ = std::move(*dsvHeap);
    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreateRootSignature(); !status)
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
    (void)context;
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
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 14 Starter frame is invalid."));
    }

    gpu::LabConfiguration const configuration = ActiveConfiguration();
    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    if (auto status = gpu::WriteBuffer(slot.lights, std::span<gpu::PointLightData const>{currentLights_}); !status)
    {
        return status;
    }

    PerspectiveProjection const projection =
        gpu::MakeProjection(frameContext.drawableSize, configuration.depthConvention);
    auto const coefficients = MakeDeviceDepthCoefficients(projection);
    if (!coefficients)
    {
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 14 Starter projection is invalid."));
    }
    auto const tiles = MakeTileGrid(frameContext.drawableSize.width, frameContext.drawableSize.height, gpu::kTileWidth,
                                    gpu::kTileHeight);
    if (!tiles)
    {
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 14 Starter tile grid is invalid."));
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
    constants.slicing = {gpu::kNearPlane, gpu::kFarPlane, static_cast<float>(gpu::kClusterSliceCount), 0.0F};
    constants.dimensions = {
        frameContext.drawableSize.width,
        frameContext.drawableSize.height,
        tiles->tileCountX,
        tiles->tileCountY,
    };
    constants.counts = {static_cast<std::uint32_t>(currentLights_.size()), tiles->tileCount, 0U, 0U};
    constants.options = {
        static_cast<std::uint32_t>(gpu::LightingMode::BruteForce),
        DepthModeFlag(configuration.depthConvention),
        static_cast<std::uint32_t>(gpu::DebugView::Final),
        0U,
    };

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    std::vector<D3D12_TEXTURE_BARRIER> barriers{
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::FrameStartState(frameContext),
                                gpu::RenderTargetState()),
        gpu::MakeTextureBarrier(*slot.depth.Get(), gpu::CommonState(), gpu::DepthWriteState()),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);

    float const clearColor[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    commandList.ClearDepthStencilView(slot.dsv.cpuHandle, D3D12_CLEAR_FLAG_DEPTH,
                                      DepthClearValue(configuration.depthConvention), 0U, 0U, nullptr);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, &slot.dsv.cpuHandle);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.IASetVertexBuffers(0U, 1U, &quad_.vertexView);
    commandList.IASetIndexBuffer(&quad_.indexView);
    commandList.SetGraphicsRootSignature(rootSignature_.Get());
    commandList.SetGraphicsRoot32BitConstants(FrameRootConstants, sizeof(constants) / sizeof(std::uint32_t), &constants,
                                              0U);
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.SetGraphicsRootDescriptorTable(LightSrvTable, slot.lightSrv.gpuHandle);
    commandList.SetPipelineState(configuration.depthConvention == DepthConvention::Forward ? forwardPipeline_.Get()
                                                                                           : reversedPipeline_.Get());
    for (gpu::SceneObject const &object : currentScene_)
    {
        commandList.SetGraphicsRoot32BitConstants(ObjectRootConstants, sizeof(gpu::ObjectData) / sizeof(std::uint32_t),
                                                  &object.data, 0U);
        commandList.DrawIndexedInstanced(quad_.indexCount, 1U, 0U, 0, 0U);
    }

    barriers = {
        gpu::MakeTextureBarrier(*slot.depth.Get(), gpu::DepthWriteState(), gpu::CommonState()),
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(), gpu::FrameEndState(frameContext)),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    ReleaseSizeDependentResources();
    quad_ = {};
    reversedPipeline_.Reset();
    forwardPipeline_.Reset();
    rootSignature_.Reset();
    vertexShader_ = {};
    pixelShader_ = {};
    dsvHeap_ = {};
    deviceResources_ = nullptr;
    (void)deviceResources;
}

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

} // namespace ch14::clustered_lighting::starter
