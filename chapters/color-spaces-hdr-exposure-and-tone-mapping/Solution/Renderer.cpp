#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

namespace ch04::color_pipeline::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

enum RootParameter : UINT
{
    OutputConstants = 0U,
    HdrTexture = 1U,
};

struct OutputRootConstants final
{
    float exposureValue{0.0F};
    std::uint32_t toneMapper{static_cast<std::uint32_t>(ToneMapper::AcesFitted)};
    std::uint32_t bypassSrgbEncode{0U};
};

[[nodiscard]] std::string D3D12BlobToUtf8(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {
        static_cast<char const *>(blob->GetBufferPointer()),
        static_cast<std::size_t>(blob->GetBufferSize()),
    };
}

[[nodiscard]] std::filesystem::path ResolveShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "ColorPipeline.hlsl";
}

lgp::framework::TextureBarrierState constexpr kRenderTargetState{
    D3D12_BARRIER_SYNC_RENDER_TARGET,
    D3D12_BARRIER_ACCESS_RENDER_TARGET,
    D3D12_BARRIER_LAYOUT_RENDER_TARGET,
};
lgp::framework::TextureBarrierState constexpr kShaderResourceState{
    D3D12_BARRIER_SYNC_PIXEL_SHADING,
    D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
    D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
};

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frameContext.renderTargetInitialLayout};
}

[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept
{
    return {
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT,
    };
}

} // namespace

class Renderer::Impl final
{
  public:
    lgp::framework::DeviceResources *deviceResources{};
    lgp::framework::CompiledShader fullscreenVertexShader{};
    lgp::framework::CompiledShader hdrPixelShader{};
    lgp::framework::CompiledShader outputPixelShader{};
    ComPtr<ID3D12RootSignature> rootSignature{};
    ComPtr<ID3D12PipelineState> hdrPipeline{};
    ComPtr<ID3D12PipelineState> outputPipeline{};
    lgp::framework::DescriptorAllocation hdrRtv{};
    lgp::framework::DescriptorAllocation hdrSrv{};
    ComPtr<ID3D12Resource> hdrTarget{};
    lgp::framework::DescriptorAllocation imguiFontDescriptor{};
    OutputRootConstants outputConstants{};
    bool headless{};
    bool imguiInitialized{};
    bool imguiFrameBegun{};

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context);
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelineStates();
    [[nodiscard]] lgp::framework::Status CreateHdrTarget(lgp::framework::Extent2D size);
    [[nodiscard]] lgp::framework::Status InitializeImGui();
    [[nodiscard]] lgp::framework::Status BuildUi(lgp::framework::UpdateContext const &context);
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext);
    void Shutdown() noexcept;
};

lgp::framework::Status Renderer::Impl::CreateShaders()
{
    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    auto compiler = std::move(compilerResult.value());

    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = ResolveShaderPath();
    options.includeDirectories = {options.sourcePath.parent_path()};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif

    options.entryPoint = L"FullscreenVS";
    options.targetProfile = L"vs_6_0";
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto vertexResult = compiler.Compile(options);
    if (!vertexResult)
    {
        return std::unexpected(std::move(vertexResult.error()));
    }
    fullscreenVertexShader = std::move(vertexResult.value());

    options.entryPoint = L"HdrAnalyticPS";
    options.targetProfile = L"ps_6_0";
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto hdrResult = compiler.Compile(options);
    if (!hdrResult)
    {
        return std::unexpected(std::move(hdrResult.error()));
    }
    hdrPixelShader = std::move(hdrResult.value());

    options.entryPoint = L"OutputPS";
    options.targetProfile = L"ps_6_0";
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto outputResult = compiler.Compile(options);
    if (!outputResult)
    {
        return std::unexpected(std::move(outputResult.error()));
    }
    outputPixelShader = std::move(outputResult.value());
    return {};
}

lgp::framework::Status Renderer::Impl::CreateRootSignature()
{
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[OutputConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[OutputConstants].Constants.ShaderRegister = 0U;
    parameters[OutputConstants].Constants.RegisterSpace = 0U;
    parameters[OutputConstants].Constants.Num32BitValues = 3U;
    parameters[OutputConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1U;
    srvRange.BaseShaderRegister = 0U;
    srvRange.RegisterSpace = 0U;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    parameters[HdrTexture].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[HdrTexture].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[HdrTexture].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[HdrTexture].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0U;
    sampler.RegisterSpace = 0U;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
    description.pParameters = parameters;
    description.NumStaticSamplers = 1U;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT const serializeResult =
        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, serialized.ReleaseAndGetAddressOf(),
                                    errors.ReleaseAndGetAddressOf());
    if (FAILED(serializeResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult,
                                                                D3D12BlobToUtf8(errors.Get())));
    }

    HRESULT const createResult =
        deviceResources->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                       IID_PPV_ARGS(rootSignature.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                                                "Failed to create the color pipeline root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::Impl::CreatePipelineStates()
{
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
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
    description.pRootSignature = rootSignature.Get();
    description.VS = fullscreenVertexShader.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.SampleDesc.Count = 1U;

    description.PS = hdrPixelShader.Bytecode();
    description.RTVFormats[0] = kHdrFormat;
    HRESULT const hdrResult = deviceResources->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(hdrPipeline.ReleaseAndGetAddressOf()));
    if (FAILED(hdrResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", hdrResult,
                                             "Failed to create the HDR analytic pass pipeline state."));
    }

    description.PS = outputPixelShader.Bytecode();
    description.RTVFormats[0] = deviceResources->back_buffer_format();
    HRESULT const outputResult = deviceResources->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(outputPipeline.ReleaseAndGetAddressOf()));
    if (FAILED(outputResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", outputResult,
                                             "Failed to create the tone-mapped output pass pipeline state."));
    }
    return {};
}

lgp::framework::Status Renderer::Impl::CreateHdrTarget(lgp::framework::Extent2D size)
{
    hdrTarget.Reset();
    if (size.empty())
    {
        return {};
    }

    if (!hdrRtv)
    {
        auto rtvResult = deviceResources->rtv_heap().Allocate(1U);
        if (!rtvResult)
        {
            return std::unexpected(std::move(rtvResult.error()));
        }
        hdrRtv = rtvResult.value();
    }
    if (!hdrSrv)
    {
        auto srvResult = deviceResources->shader_visible_cbv_srv_uav_heap().Allocate(1U);
        if (!srvResult)
        {
            return std::unexpected(std::move(srvResult.error()));
        }
        hdrSrv = srvResult.value();
    }

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = kHdrFormat;
    clearValue.Color[3] = 1.0F;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC1 resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = size.width;
    resource.Height = size.height;
    resource.DepthOrArraySize = 1U;
    resource.MipLevels = 1U;
    resource.Format = kHdrFormat;
    resource.SampleDesc.Count = 1U;
    resource.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    HRESULT const createResult = deviceResources->device()->CreateCommittedResource3(
        &heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &clearValue, nullptr, 0U, nullptr,
        IID_PPV_ARGS(hdrTarget.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", createResult,
                                             "Failed to create the Chapter 4 HDR intermediate target."));
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = kHdrFormat;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    deviceResources->device()->CreateRenderTargetView(hdrTarget.Get(), &rtv, hdrRtv.cpuHandle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kHdrFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1U;
    deviceResources->device()->CreateShaderResourceView(hdrTarget.Get(), &srv, hdrSrv.cpuHandle);
    return {};
}

lgp::framework::Status Renderer::Impl::InitializeImGui()
{
    auto descriptorResult = deviceResources->shader_visible_cbv_srv_uav_heap().Allocate(1U);
    if (!descriptorResult)
    {
        return std::unexpected(std::move(descriptorResult.error()));
    }
    imguiFontDescriptor = descriptorResult.value();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().BackendPlatformName = "LGP.ManualInput";

    ImGui_ImplDX12_InitInfo info{};
    info.Device = deviceResources->device();
    info.CommandQueue = deviceResources->graphics_queue();
    info.NumFramesInFlight = static_cast<int>(deviceResources->back_buffer_count());
    info.RTVFormat = deviceResources->back_buffer_format();
    info.SrvDescriptorHeap = deviceResources->shader_visible_cbv_srv_uav_heap().Get();
    info.LegacySingleSrvCpuDescriptor = imguiFontDescriptor.cpuHandle;
    info.LegacySingleSrvGpuDescriptor = imguiFontDescriptor.gpuHandle;
    if (!ImGui_ImplDX12_Init(&info))
    {
        return std::unexpected(
            lgp::framework::MakeError("ImGui_ImplDX12_Init", "Failed to initialize the color pipeline UI."));
    }
    imguiInitialized = true;
    return {};
}

lgp::framework::Status Renderer::Impl::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources = &context.deviceResources;
    headless = context.commandLine.headless;

    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreateRootSignature(); !status)
    {
        return status;
    }
    if (auto status = CreatePipelineStates(); !status)
    {
        return status;
    }
    if (auto status = CreateHdrTarget(context.drawableSize); !status)
    {
        return status;
    }
    if (!headless)
    {
        return InitializeImGui();
    }
    return {};
}

lgp::framework::Status Renderer::Impl::BuildUi(lgp::framework::UpdateContext const &context)
{
    if (headless || !imguiInitialized)
    {
        return {};
    }

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {
        static_cast<float>(context.drawableSize.width),
        static_cast<float>(context.drawableSize.height),
    };
    io.DeltaTime = static_cast<float>(std::max(context.deltaSeconds, 1.0 / 240.0));
    io.AddMousePosEvent(static_cast<float>(context.input.mouse.x), static_cast<float>(context.input.mouse.y));
    io.AddMouseButtonEvent(0, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Left));
    io.AddMouseWheelEvent(0.0F, context.input.mouse.wheelDelta);

    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    imguiFrameBegun = true;

    ImGui::Begin("Color Pipeline");
    ImGui::SliderFloat("Exposure EV", &outputConstants.exposureValue, -8.0F, 8.0F, "%.2f EV");

    int toneMapper = static_cast<int>(outputConstants.toneMapper);
    ImGui::Combo("Tone mapper", &toneMapper, "None\0Reinhard\0ACES-fitted\0");
    outputConstants.toneMapper = static_cast<std::uint32_t>(toneMapper);

    bool bypassSrgb = outputConstants.bypassSrgbEncode != 0U;
    ImGui::Checkbox("Diagnostic: bypass exact sRGB encode", &bypassSrgb);
    outputConstants.bypassSrgbEncode = bypassSrgb ? 1U : 0U;

    ImGui::Separator();
    ImGui::Text("Exposure multiplier: %.3fx", static_cast<double>(std::exp2(outputConstants.exposureValue)));
    ImGui::TextWrapped("Pass 1 renders analytic HDR color into R16G16B16A16_FLOAT. Pass 2 applies exposure, "
                       "tone mapping, then exact sRGB encoding unless the diagnostic bypass is enabled.");
    ImGui::End();
    return {};
}

lgp::framework::Status Renderer::Impl::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr || hdrTarget == nullptr)
    {
        if (imguiFrameBegun)
        {
            ImGui::EndFrame();
            imguiFrameBegun = false;
        }
        return {};
    }

    ID3D12GraphicsCommandList7 *const commandList = frameContext.commandList;
    commandList->RSSetViewports(1U, &frameContext.viewport);
    commandList->RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootSignature(rootSignature.Get());

    lgp::framework::TransitionTexture(*commandList, *hdrTarget.Get(), kShaderResourceState, kRenderTargetState);
    float const hdrClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList->OMSetRenderTargets(1U, &hdrRtv.cpuHandle, FALSE, nullptr);
    commandList->ClearRenderTargetView(hdrRtv.cpuHandle, hdrClear, 0U, nullptr);
    commandList->SetPipelineState(hdrPipeline.Get());
    commandList->DrawInstanced(3U, 1U, 0U, 0U);
    lgp::framework::TransitionTexture(*commandList, *hdrTarget.Get(), kRenderTargetState, kShaderResourceState);

    lgp::framework::TransitionTexture(*commandList, *frameContext.renderTarget, FrameStartState(frameContext),
                                      kRenderTargetState);
    float const backBufferClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList->OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList->ClearRenderTargetView(frameContext.renderTargetView, backBufferClear, 0U, nullptr);

    ID3D12DescriptorHeap *const descriptorHeaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList->SetDescriptorHeaps(1U, descriptorHeaps);
    commandList->SetPipelineState(outputPipeline.Get());
    commandList->SetGraphicsRoot32BitConstants(OutputConstants, 3U, &outputConstants, 0U);
    commandList->SetGraphicsRootDescriptorTable(HdrTexture, hdrSrv.gpuHandle);
    commandList->DrawInstanced(3U, 1U, 0U, 0U);

    if (imguiFrameBegun)
    {
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
        imguiFrameBegun = false;
    }

    lgp::framework::TransitionTexture(*commandList, *frameContext.renderTarget, kRenderTargetState,
                                      FrameEndState(frameContext));
    return {};
}

void Renderer::Impl::Shutdown() noexcept
{
    if (imguiFrameBegun)
    {
        ImGui::EndFrame();
        imguiFrameBegun = false;
    }
    if (imguiInitialized)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
    }
    if (deviceResources != nullptr && imguiFontDescriptor)
    {
        deviceResources->shader_visible_cbv_srv_uav_heap().Free(imguiFontDescriptor);
        imguiFontDescriptor = {};
    }
    hdrTarget.Reset();
    if (deviceResources != nullptr && hdrRtv)
    {
        deviceResources->rtv_heap().Free(hdrRtv);
        hdrRtv = {};
    }
    if (deviceResources != nullptr && hdrSrv)
    {
        deviceResources->shader_visible_cbv_srv_uav_heap().Free(hdrSrv);
        hdrSrv = {};
    }
    outputPipeline.Reset();
    hdrPipeline.Reset();
    rootSignature.Reset();
    deviceResources = nullptr;
}

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}

Renderer::~Renderer() = default;
Renderer::Renderer(Renderer &&) noexcept = default;
Renderer &Renderer::operator=(Renderer &&) noexcept = default;

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    return impl_->Initialize(context);
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D drawableSize)
{
    return impl_->CreateHdrTarget(drawableSize);
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &)
{
    return {};
}

lgp::framework::Status Renderer::BuildUi(lgp::framework::UpdateContext const &context)
{
    return impl_->BuildUi(context);
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    return impl_->Render(frameContext);
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    impl_->Shutdown();
}

} // namespace ch04::color_pipeline::solution
