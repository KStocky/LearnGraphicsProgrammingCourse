#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "GpuLabSupport.hpp"

#include <d3d12.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace ch09::transient_aliasing::gpu
{
namespace
{

[[nodiscard]] std::string D3D12BlobToUtf8(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] lgp::framework::Status CompileShader(lgp::framework::ShaderCompiler &compiler,
                                                   lgp::framework::ShaderCompileOptions &options,
                                                   wchar_t const *entryPoint, wchar_t const *targetProfile,
                                                   lgp::framework::CompiledShader &shader)
{
    options.entryPoint = entryPoint;
    options.targetProfile = targetProfile;
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto result = compiler.Compile(options);
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    shader = std::move(*result);
    return {};
}

[[nodiscard]] lgp::framework::Status CreatePipeline(ID3D12Device10 &device,
                                                    D3D12_GRAPHICS_PIPELINE_STATE_DESC &description,
                                                    lgp::framework::CompiledShader const &pixelShader,
                                                    DXGI_FORMAT format, std::string_view label,
                                                    Microsoft::WRL::ComPtr<ID3D12PipelineState> &pipeline)
{
    description.PS = pixelShader.Bytecode();
    description.RTVFormats[0] = format;
    HRESULT const result =
        device.CreateGraphicsPipelineState(&description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create the Chapter 9 " + std::string{label} +
                                                                    " pipeline state."));
    }
    return {};
}

} // namespace

lgp::framework::TextureBarrierState UndefinedState() noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_UNDEFINED};
}

lgp::framework::TextureBarrierState ShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
            D3D12_BARRIER_LAYOUT_SHADER_RESOURCE};
}

lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
}

lgp::framework::TextureBarrierState FrameStartState(bool headless) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
            headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT};
}

lgp::framework::TextureBarrierState FrameEndState(bool headless) noexcept
{
    return FrameStartState(headless);
}

D3D12_RESOURCE_DESC1 TextureDescription(lgp::framework::Extent2D size) noexcept
{
    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = size.width;
    description.Height = size.height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = kTransientFormat;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return description;
}

D3D12_TEXTURE_BARRIER MakeTextureBarrier(ID3D12Resource &resource, lgp::framework::TextureBarrierState before,
                                         lgp::framework::TextureBarrierState after,
                                         D3D12_TEXTURE_BARRIER_FLAGS flags) noexcept
{
    D3D12_TEXTURE_BARRIER barrier{};
    barrier.SyncBefore = before.sync;
    barrier.SyncAfter = after.sync;
    barrier.AccessBefore = before.access;
    barrier.AccessAfter = after.access;
    barrier.LayoutBefore = before.layout;
    barrier.LayoutAfter = after.layout;
    barrier.pResource = &resource;
    barrier.Subresources.IndexOrFirstMipLevel = UINT32_MAX;
    barrier.Flags = flags;
    return barrier;
}

lgp::framework::Status CreateLabShaders(std::filesystem::path const &path,
                                        lgp::framework::CompiledShader &fullscreenVertexShader,
                                        lgp::framework::CompiledShader &analyticPixelShader,
                                        lgp::framework::CompiledShader &copyPixelShader,
                                        lgp::framework::CompiledShader &accentPixelShader,
                                        lgp::framework::CompiledShader &compositePixelShader)
{
    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    lgp::framework::ShaderCompiler compiler = std::move(*compilerResult);
    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = path;
    options.includeDirectories = {path.parent_path()};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif

    if (auto status = CompileShader(compiler, options, L"FullscreenVS", L"vs_6_0", fullscreenVertexShader); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"AnalyticPS", L"ps_6_0", analyticPixelShader); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"CopyPS", L"ps_6_0", copyPixelShader); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"AccentMaskPS", L"ps_6_0", accentPixelShader); !status)
    {
        return status;
    }
    return CompileShader(compiler, options, L"CompositePS", L"ps_6_0", compositePixelShader);
}

lgp::framework::Status CreateLabRootSignature(ID3D12Device10 &device,
                                              Microsoft::WRL::ComPtr<ID3D12RootSignature> &rootSignature)
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2U;
    range.BaseShaderRegister = 0U;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1U;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0U;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 1U;
    description.pParameters = &parameter;
    description.NumStaticSamplers = 1U;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized{};
    Microsoft::WRL::ComPtr<ID3DBlob> errors{};
    HRESULT const serializeResult =
        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, serialized.ReleaseAndGetAddressOf(),
                                    errors.ReleaseAndGetAddressOf());
    if (FAILED(serializeResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult,
                                                                D3D12BlobToUtf8(errors.Get())));
    }

    HRESULT const createResult =
        device.CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                   IID_PPV_ARGS(rootSignature.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                                                "Failed to create the Chapter 9 root signature."));
    }
    return {};
}

lgp::framework::Status CreateLabPipelineStates(ID3D12Device10 &device, DXGI_FORMAT frameFormat,
                                               ID3D12RootSignature &rootSignature,
                                               lgp::framework::CompiledShader const &fullscreenVertexShader,
                                               lgp::framework::CompiledShader const &analyticPixelShader,
                                               lgp::framework::CompiledShader const &copyPixelShader,
                                               lgp::framework::CompiledShader const &accentPixelShader,
                                               lgp::framework::CompiledShader const &compositePixelShader,
                                               Microsoft::WRL::ComPtr<ID3D12PipelineState> &analyticPipeline,
                                               Microsoft::WRL::ComPtr<ID3D12PipelineState> &copyPipeline,
                                               Microsoft::WRL::ComPtr<ID3D12PipelineState> &accentPipeline,
                                               Microsoft::WRL::ComPtr<ID3D12PipelineState> &compositePipeline)
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
    description.VS = fullscreenVertexShader.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.SampleDesc.Count = 1U;

    if (auto status =
            CreatePipeline(device, description, analyticPixelShader, kTransientFormat, "analytic", analyticPipeline);
        !status)
    {
        return status;
    }
    if (auto status = CreatePipeline(device, description, copyPixelShader, kTransientFormat, "copy", copyPipeline);
        !status)
    {
        return status;
    }
    if (auto status =
            CreatePipeline(device, description, accentPixelShader, kTransientFormat, "accent", accentPipeline);
        !status)
    {
        return status;
    }
    return CreatePipeline(device, description, compositePixelShader, frameFormat, "composite", compositePipeline);
}

} // namespace ch09::transient_aliasing::gpu
