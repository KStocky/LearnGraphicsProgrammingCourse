#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include "../Common/GpuLabSupport.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>

namespace ch11::reprojection::starter
{
namespace
{

using Microsoft::WRL::ComPtr;
using TextureIndex = Renderer::TextureIndex;

struct alignas(16) SurfaceConstants final
{
    gpu::Float4 currentRectUv{};
    gpu::Float4 currentColor{};
    gpu::Uint4 metadata{};
    gpu::Float4 depths{};
    std::array<gpu::Float4, 4U> currentClipRows{};
    std::array<gpu::Float4, 4U> previousClipRows{};
};

struct alignas(16) FrameConstants final
{
    gpu::Uint4 header0{};
    gpu::Float4 currentJitterExposure{};
    gpu::Float4 previousJitterExposure{};
    gpu::Float4 depthSettings{};
    std::array<SurfaceConstants, gpu::kMaxSurfaces> surfaces{};
};

static_assert((sizeof(FrameConstants) % 16U) == 0U);

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "ReprojectionLab.hlsl";
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }

    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] lgp::framework::Status Compile(lgp::framework::ShaderCompiler &compiler,
                                             lgp::framework::ShaderCompileOptions &options, wchar_t const *entryPoint,
                                             wchar_t const *profile, lgp::framework::CompiledShader &shader)
{
    options.entryPoint = entryPoint;
    options.targetProfile = profile;
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto result = compiler.Compile(options);
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }

    shader = std::move(*result);
    return {};
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
                                             std::string{"Failed to create "} + label + " for Chapter 11 Starter."));
    }
    return {};
}

[[nodiscard]] lgp::framework::Status CreateGraphicsPipeline(ID3D12Device10 &device, ID3D12RootSignature &rootSignature,
                                                            DXGI_FORMAT format,
                                                            lgp::framework::CompiledShader const &vertexShader,
                                                            lgp::framework::CompiledShader const &pixelShader,
                                                            ComPtr<ID3D12PipelineState> &pipeline)
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
    description.pRootSignature = &rootSignature;
    description.VS = vertexShader.Bytecode();
    description.PS = pixelShader.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1U;

    HRESULT const result =
        device.CreateGraphicsPipelineState(&description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                             "Failed to create the Chapter 11 Starter composite pipeline."));
    }
    return {};
}

[[nodiscard]] std::uint32_t TextureCountU32() noexcept
{
    return static_cast<std::uint32_t>(TextureIndex::Count);
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

    if (auto status = Compile(compiler, options, L"GenerateCurrentFrameCS", L"cs_6_0", generateShader_); !status)
    {
        return status;
    }
    if (auto status = Compile(compiler, options, L"BaselineNoHistoryCS", L"cs_6_0", baselineShader_); !status)
    {
        return status;
    }
    if (auto status = Compile(compiler, options, L"FullscreenVS", L"vs_6_0", fullscreenVertexShader_); !status)
    {
        return status;
    }
    return Compile(compiler, options, L"CompositePS", L"ps_6_0", compositePixelShader_);
}

lgp::framework::Status Renderer::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = TextureCountU32();
    srvRange.BaseShaderRegister = 0U;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = TextureCountU32();
    uavRange.BaseShaderRegister = 0U;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0U;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[2].DescriptorTable.pDescriptorRanges = &uavRange;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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
                                             "Failed to create the Chapter 11 Starter root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreatePipelines()
{
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *rootSignature_.Get(), generateShader_,
                                            "GenerateCurrentFrameCS", generatePipeline_);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *rootSignature_.Get(), baselineShader_,
                                            "BaselineNoHistoryCS", baselinePipeline_);
        !status)
    {
        return status;
    }
    return CreateGraphicsPipeline(*deviceResources_->device(), *rootSignature_.Get(),
                                  deviceResources_->back_buffer_format(), fullscreenVertexShader_,
                                  compositePixelShader_, compositePipeline_);
}

UINT Renderer::DescriptorIndex(UINT frameSlot, UINT textureIndex, bool uav) const noexcept
{
    UINT const perSlot = TextureCountU32() * 2U;
    return (frameSlot * perSlot) + (uav ? TextureCountU32() : 0U) + textureIndex;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::WriteFrameConstants(UINT frameSlot, lgp::framework::Extent2D size)
{
    FrameConstants constants{};
    constants.header0 = {size.width, size.height, gpu::kMaxSurfaces, 0U};
    gpu::ScenarioState const scenario = gpu::MakeScenarioState(gpu::Scenario::Static);
    constants.header0.z = scenario.surfaceCount;
    constants.currentJitterExposure = {scenario.currentJitterUv.x, scenario.currentJitterUv.y,
                                       scenario.currentPreExposure, 0.0F};
    constants.previousJitterExposure = {0.0F, 0.0F, 1.0F, 4.0F};
    constants.depthSettings = {0.001F, 0.005F, 1.0F, 0.0F};

    for (std::uint32_t index = 0U; index < scenario.surfaceCount; ++index)
    {
        gpu::SurfaceState const &surface = scenario.surfaces[index];
        SurfaceConstants &destination = constants.surfaces[index];
        destination.currentRectUv = surface.currentRectUv;
        destination.currentColor = surface.currentColor;
        destination.metadata = {surface.currentIdentity, 0U, 0U, 0U};
        destination.depths = {surface.currentLinearDepth, surface.expectedPreviousLinearDepth, 0.0F, 0.0F};
        destination.currentClipRows = surface.currentClipFromLocal.rows;
        destination.previousClipRows = surface.previousClipFromLocal.rows;
    }

    std::memcpy(frameSlots_[frameSlot].frameConstants.mapped_data(), &constants, sizeof(constants));
    return frameSlots_[frameSlot].frameConstants.gpu_virtual_address();
}

lgp::framework::Status Renderer::CreateSizeDependentResources(lgp::framework::Extent2D drawableSize)
{
    ReleaseSizeDependentResources();
    if (drawableSize.empty())
    {
        return {};
    }

    UINT const frameSlotCount = deviceResources_->back_buffer_count();
    auto descriptors =
        deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(frameSlotCount * TextureCountU32() * 2U);
    if (!descriptors)
    {
        return std::unexpected(std::move(descriptors.error()));
    }
    textureDescriptors_ = *descriptors;
    frameSlots_.resize(frameSlotCount);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (UINT frameSlot = 0U; frameSlot < frameSlotCount; ++frameSlot)
    {
        auto &slot = frameSlots_[frameSlot];
        auto constantsResult = lgp::framework::CreateUploadBuffer(*deviceResources_->device(), sizeof(FrameConstants),
                                                                  L"Ch11 Starter Frame Constants");
        if (!constantsResult)
        {
            return std::unexpected(std::move(constantsResult.error()));
        }
        slot.frameConstants = std::move(*constantsResult);

        std::array<DXGI_FORMAT, static_cast<std::size_t>(TextureIndex::Count)> const formats{
            gpu::kColorFormat, gpu::kDepthFormat, gpu::kIdentityFormat, gpu::kMotionFormat,
            gpu::kUvFormat,    gpu::kColorFormat, gpu::kReasonFormat,   gpu::kScalarFormat,
        };

        for (UINT textureIndex = 0U; textureIndex < TextureCountU32(); ++textureIndex)
        {
            D3D12_RESOURCE_DESC1 const description = gpu::MakeTextureDescription(
                drawableSize, formats[textureIndex], D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            HRESULT const createResult = deviceResources_->device()->CreateCommittedResource3(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0U, nullptr,
                IID_PPV_ARGS(slot.textures[textureIndex].ReleaseAndGetAddressOf()));
            if (FAILED(createResult))
            {
                return std::unexpected(
                    lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", createResult,
                                                     "Failed to create a Chapter 11 Starter texture."));
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = formats[textureIndex];
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1U;
            deviceResources_->device()->CreateShaderResourceView(
                slot.textures[textureIndex].Get(), &srv,
                textureDescriptors_.CpuHandle(DescriptorIndex(frameSlot, textureIndex, false)));

            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = formats[textureIndex];
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            deviceResources_->device()->CreateUnorderedAccessView(
                slot.textures[textureIndex].Get(), nullptr, &uav,
                textureDescriptors_.CpuHandle(DescriptorIndex(frameSlot, textureIndex, true)));
        }
    }

    return {};
}

void Renderer::ReleaseSizeDependentResources() noexcept
{
    for (auto &slot : frameSlots_)
    {
        for (auto &texture : slot.textures)
        {
            texture.Reset();
        }
    }
    frameSlots_.clear();
    if (deviceResources_ != nullptr && textureDescriptors_)
    {
        deviceResources_->shader_visible_cbv_srv_uav_heap().Free(textureDescriptors_);
        textureDescriptors_ = {};
    }
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
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
    return CreateSizeDependentResources(context.drawableSize);
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D drawableSize)
{
    return CreateSizeDependentResources(drawableSize);
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &)
{
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr)
    {
        return std::unexpected(lgp::framework::MakeError("Starter::Renderer::Render",
                                                         "The frame context is missing required D3D12 handles."));
    }
    if (frameContext.drawableSize.empty() || frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Starter::Renderer::Render",
                                                         "Size-dependent Starter resources are unavailable."));
    }

    auto &slot = frameSlots_[frameContext.frameSlot];
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.SetComputeRootSignature(rootSignature_.Get());
    commandList.SetGraphicsRootSignature(rootSignature_.Get());
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_GPU_VIRTUAL_ADDRESS const constantsAddress =
        WriteFrameConstants(frameContext.frameSlot, frameContext.drawableSize);
    commandList.SetComputeRootConstantBufferView(0U, constantsAddress);
    commandList.SetGraphicsRootConstantBufferView(0U, constantsAddress);

    std::vector<D3D12_TEXTURE_BARRIER> barriers{};
    barriers.reserve(TextureCountU32() + 1U);
    lgp::framework::TextureBarrierState const commonState{
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        D3D12_BARRIER_LAYOUT_COMMON,
    };
    for (UINT textureIndex = 0U; textureIndex < TextureCountU32(); ++textureIndex)
    {
        barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[textureIndex].Get(), commonState,
                                                   gpu::ComputeUnorderedAccessState(),
                                                   D3D12_TEXTURE_BARRIER_FLAG_DISCARD));
    }
    gpu::SubmitTextureBarriers(commandList, barriers);

    commandList.SetComputeRootDescriptorTable(
        2U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, 0U, true)));
    commandList.SetPipelineState(generatePipeline_.Get());
    commandList.Dispatch((frameContext.drawableSize.width + gpu::kThreadsX - 1U) / gpu::kThreadsX,
                         (frameContext.drawableSize.height + gpu::kThreadsY - 1U) / gpu::kThreadsY, 1U);

    commandList.SetPipelineState(baselinePipeline_.Get());
    commandList.Dispatch((frameContext.drawableSize.width + gpu::kThreadsX - 1U) / gpu::kThreadsX,
                         (frameContext.drawableSize.height + gpu::kThreadsY - 1U) / gpu::kThreadsY, 1U);

    barriers.clear();
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::CurrentColor)].Get(),
                                               gpu::ComputeUnorderedAccessState(), gpu::PixelShaderResourceState()));
    barriers.push_back(
        gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::ReprojectedHistoryColor)].Get(),
                                gpu::ComputeUnorderedAccessState(), gpu::PixelShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::RejectionReasons)].Get(),
                                               gpu::ComputeUnorderedAccessState(), gpu::PixelShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::ExposureScale)].Get(),
                                               gpu::ComputeUnorderedAccessState(), gpu::PixelShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::FrameStartState(frameContext.headless),
                                               gpu::RenderTargetState()));
    gpu::SubmitTextureBarriers(commandList, barriers);

    float const clearColor[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    commandList.SetGraphicsRootDescriptorTable(
        1U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, 0U, false)));
    commandList.SetPipelineState(compositePipeline_.Get());
    commandList.DrawInstanced(3U, 1U, 0U, 0U);

    barriers.clear();
    for (UINT textureIndex = 0U; textureIndex < TextureCountU32(); ++textureIndex)
    {
        lgp::framework::TextureBarrierState before = gpu::ComputeUnorderedAccessState();
        if (textureIndex == static_cast<UINT>(TextureIndex::CurrentColor) ||
            textureIndex == static_cast<UINT>(TextureIndex::ReprojectedHistoryColor) ||
            textureIndex == static_cast<UINT>(TextureIndex::RejectionReasons) ||
            textureIndex == static_cast<UINT>(TextureIndex::ExposureScale))
        {
            before = gpu::PixelShaderResourceState();
        }
        barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[textureIndex].Get(), before, commonState));
    }
    barriers.push_back(gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(),
                                               gpu::FrameEndState(frameContext.headless)));
    gpu::SubmitTextureBarriers(commandList, barriers);
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    ReleaseSizeDependentResources();
    compositePipeline_.Reset();
    baselinePipeline_.Reset();
    generatePipeline_.Reset();
    rootSignature_.Reset();
}

} // namespace ch11::reprojection::starter
