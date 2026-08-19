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
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>

namespace ch11::reprojection::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kSrvCount = 11U;
inline constexpr UINT kUavCount = static_cast<UINT>(Renderer::TextureIndex::Count);
inline constexpr UINT kDescriptorsPerSlot = kSrvCount + kUavCount;

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
                                             std::string{"Failed to create "} + label + " for Chapter 11 Solution."));
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
                                             "Failed to create the Chapter 11 Solution composite pipeline."));
    }
    return {};
}

[[nodiscard]] DXGI_FORMAT TextureFormat(Renderer::TextureIndex texture) noexcept
{
    switch (texture)
    {
    case Renderer::TextureIndex::CurrentColor:
    case Renderer::TextureIndex::ReprojectedHistoryColor:
        return gpu::kColorFormat;
    case Renderer::TextureIndex::CurrentDepth:
        return gpu::kDepthFormat;
    case Renderer::TextureIndex::CurrentIdentity:
    case Renderer::TextureIndex::RejectionReasons:
        return gpu::kIdentityFormat;
    case Renderer::TextureIndex::MotionClipDepth:
        return gpu::kMotionFormat;
    case Renderer::TextureIndex::PreviousHistoryUv:
        return gpu::kUvFormat;
    case Renderer::TextureIndex::ExposureScale:
        return gpu::kScalarFormat;
    case Renderer::TextureIndex::Count:
        break;
    }
    return DXGI_FORMAT_UNKNOWN;
}

[[nodiscard]] UINT TextureCountU32() noexcept
{
    return static_cast<UINT>(Renderer::TextureIndex::Count);
}

} // namespace

void Renderer::SetScenario(gpu::Scenario scenario) noexcept
{
    scenario_ = scenario;
}

void Renderer::RequestHistoryReset() noexcept
{
    resetRequested_ = true;
}

void Renderer::EnsureDistinctHistoryIndices() noexcept
{
    historyReadIndex_ &= 1U;
    historyWriteIndex_ &= 1U;
    if (historyReadIndex_ == historyWriteIndex_)
    {
        historyWriteIndex_ = 1U - historyReadIndex_;
    }
}

UINT Renderer::HistoryReadIndex() const noexcept
{
    return historyReadIndex_;
}

UINT Renderer::HistoryWriteIndex() const noexcept
{
    return historyWriteIndex_;
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::FrameConstantAddress(UINT frameSlot) const noexcept
{
    if (frameSlot >= frameSlots_.size())
    {
        return 0U;
    }
    return frameSlots_[frameSlot].frameConstants.gpu_virtual_address();
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
    options.includeDirectories = {options.sourcePath.parent_path()};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif

    if (auto status = Compile(compiler, options, L"GenerateCurrentFrameCS", L"cs_6_0", generateShader_); !status)
    {
        return status;
    }
    if (auto status = Compile(compiler, options, L"ValidateReprojectionCS", L"cs_6_0", validateShader_); !status)
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
    srvRange.NumDescriptors = kSrvCount;
    srvRange.BaseShaderRegister = 0U;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = kUavCount;
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

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderRegister = 0U;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;

    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].ShaderRegister = 1U;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
    description.pParameters = parameters;
    description.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    description.pStaticSamplers = samplers;
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
                                             "Failed to create the Chapter 11 Solution root signature."));
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
    if (auto status = CreateComputePipeline(*deviceResources_->device(), *rootSignature_.Get(), validateShader_,
                                            "ValidateReprojectionCS", validatePipeline_);
        !status)
    {
        return status;
    }
    return CreateGraphicsPipeline(*deviceResources_->device(), *rootSignature_.Get(),
                                  deviceResources_->back_buffer_format(), fullscreenVertexShader_,
                                  compositePixelShader_, compositePipeline_);
}

UINT Renderer::DescriptorIndex(UINT frameSlot, UINT descriptorIndex, bool uav) const noexcept
{
    return (frameSlot * kDescriptorsPerSlot) + (uav ? kSrvCount : 0U) + descriptorIndex;
}

void Renderer::UpdateHistorySrvs(UINT frameSlot, UINT historyIndex)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC colorSrv{};
    colorSrv.Format = gpu::kColorFormat;
    colorSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    colorSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    colorSrv.Texture2D.MipLevels = 1U;

    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = colorSrv;
    depthSrv.Format = gpu::kDepthFormat;

    D3D12_SHADER_RESOURCE_VIEW_DESC identitySrv = colorSrv;
    identitySrv.Format = gpu::kIdentityFormat;

    deviceResources_->device()->CreateShaderResourceView(
        history_[historyIndex].color.Get(), &colorSrv,
        textureDescriptors_.CpuHandle(DescriptorIndex(frameSlot, 8U, false)));
    deviceResources_->device()->CreateShaderResourceView(
        history_[historyIndex].depth.Get(), &depthSrv,
        textureDescriptors_.CpuHandle(DescriptorIndex(frameSlot, 9U, false)));
    deviceResources_->device()->CreateShaderResourceView(
        history_[historyIndex].identity.Get(), &identitySrv,
        textureDescriptors_.CpuHandle(DescriptorIndex(frameSlot, 10U, false)));
}

D3D12_GPU_VIRTUAL_ADDRESS Renderer::WriteFrameConstants(UINT frameSlot, lgp::framework::Extent2D size,
                                                        bool resetRequested)
{
    gpu::ScenarioState const scenarioState = gpu::MakeScenarioState(scenario_);
    FrameConstants constants{};
    constants.header0 = {size.width, size.height, scenarioState.surfaceCount,
                         (hasHistory_ ? 1U : 0U) | (resetRequested ? 2U : 0U)};
    constants.currentJitterExposure = {scenarioState.currentJitterUv.x, scenarioState.currentJitterUv.y,
                                       scenarioState.currentPreExposure, 0.0F};
    constants.previousJitterExposure = {previousJitterUv_.x, previousJitterUv_.y, previousPreExposure_, 4.0F};
    constants.depthSettings = {0.001F, 0.005F, 1.0F, 0.0F};

    for (std::uint32_t index = 0U; index < scenarioState.surfaceCount; ++index)
    {
        gpu::SurfaceState const &surface = scenarioState.surfaces[index];
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
    lastDrawableSize_ = drawableSize;
    if (drawableSize.empty())
    {
        resetRequested_ = false;
        hasHistory_ = false;
        return {};
    }

    auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(
        deviceResources_->back_buffer_count() * kDescriptorsPerSlot);
    if (!descriptors)
    {
        return std::unexpected(std::move(descriptors.error()));
    }
    textureDescriptors_ = *descriptors;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (UINT frameSlot = 0U; frameSlot < deviceResources_->back_buffer_count(); ++frameSlot)
    {
        auto constantsResult = lgp::framework::CreateUploadBuffer(*deviceResources_->device(), sizeof(FrameConstants),
                                                                  L"Ch11 Solution Frame Constants");
        if (!constantsResult)
        {
            return std::unexpected(std::move(constantsResult.error()));
        }
        frameSlots_[frameSlot].frameConstants = std::move(*constantsResult);

        for (UINT textureIndex = 0U; textureIndex < TextureCountU32(); ++textureIndex)
        {
            DXGI_FORMAT const format = TextureFormat(static_cast<TextureIndex>(textureIndex));
            D3D12_RESOURCE_DESC1 const description =
                gpu::MakeTextureDescription(drawableSize, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            HRESULT const createResult = deviceResources_->device()->CreateCommittedResource3(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0U, nullptr,
                IID_PPV_ARGS(frameSlots_[frameSlot].textures[textureIndex].ReleaseAndGetAddressOf()));
            if (FAILED(createResult))
            {
                return std::unexpected(
                    lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", createResult,
                                                     "Failed to create a Chapter 11 Solution per-frame texture."));
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1U;
            deviceResources_->device()->CreateShaderResourceView(
                frameSlots_[frameSlot].textures[textureIndex].Get(), &srv,
                textureDescriptors_.CpuHandle(DescriptorIndex(frameSlot, textureIndex, false)));

            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = format;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            deviceResources_->device()->CreateUnorderedAccessView(
                frameSlots_[frameSlot].textures[textureIndex].Get(), nullptr, &uav,
                textureDescriptors_.CpuHandle(DescriptorIndex(frameSlot, textureIndex, true)));
        }
    }

    for (auto &historyTextures : history_)
    {
        D3D12_RESOURCE_DESC1 const colorDescription =
            gpu::MakeTextureDescription(drawableSize, gpu::kColorFormat, D3D12_RESOURCE_FLAG_NONE);
        D3D12_RESOURCE_DESC1 const depthDescription =
            gpu::MakeTextureDescription(drawableSize, gpu::kDepthFormat, D3D12_RESOURCE_FLAG_NONE);
        D3D12_RESOURCE_DESC1 const identityDescription =
            gpu::MakeTextureDescription(drawableSize, gpu::kIdentityFormat, D3D12_RESOURCE_FLAG_NONE);

        HRESULT const colorResult = deviceResources_->device()->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &colorDescription, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0U, nullptr,
            IID_PPV_ARGS(historyTextures.color.ReleaseAndGetAddressOf()));
        HRESULT const depthResult = deviceResources_->device()->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &depthDescription, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0U, nullptr,
            IID_PPV_ARGS(historyTextures.depth.ReleaseAndGetAddressOf()));
        HRESULT const identityResult = deviceResources_->device()->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &identityDescription, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0U,
            nullptr, IID_PPV_ARGS(historyTextures.identity.ReleaseAndGetAddressOf()));
        if (FAILED(colorResult) || FAILED(depthResult) || FAILED(identityResult))
        {
            return std::unexpected(lgp::framework::MakeError(
                "CreateSizeDependentResources", "Failed to create a Chapter 11 Solution history texture."));
        }
    }

    resetRequested_ = false;
    hasHistory_ = false;
    historyReadIndex_ = 0U;
    historyWriteIndex_ = 1U;
    EnsureDistinctHistoryIndices();
    return {};
}

lgp::framework::Status Renderer::InitializeImGui()
{
    auto descriptor = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(1U);
    if (!descriptor)
    {
        return std::unexpected(std::move(descriptor.error()));
    }
    imguiFontDescriptor_ = *descriptor;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().BackendPlatformName = "LGP.ManualInput";

    ImGui_ImplDX12_InitInfo info{};
    info.Device = deviceResources_->device();
    info.CommandQueue = deviceResources_->graphics_queue();
    info.NumFramesInFlight = static_cast<int>(deviceResources_->back_buffer_count());
    info.RTVFormat = deviceResources_->back_buffer_format();
    info.SrvDescriptorHeap = deviceResources_->shader_visible_cbv_srv_uav_heap().Get();
    info.LegacySingleSrvCpuDescriptor = imguiFontDescriptor_.cpuHandle;
    info.LegacySingleSrvGpuDescriptor = imguiFontDescriptor_.gpuHandle;
    if (!ImGui_ImplDX12_Init(&info))
    {
        return std::unexpected(
            lgp::framework::MakeError("ImGui_ImplDX12_Init", "Failed to initialize Chapter 11 diagnostics."));
    }
    imguiInitialized_ = true;
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
    for (auto &historyTextures : history_)
    {
        historyTextures.color.Reset();
        historyTextures.depth.Reset();
        historyTextures.identity.Reset();
    }
    if (deviceResources_ != nullptr && textureDescriptors_)
    {
        deviceResources_->shader_visible_cbv_srv_uav_heap().Free(textureDescriptors_);
        textureDescriptors_ = {};
    }
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;

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
    if (auto status = CreateSizeDependentResources(context.drawableSize); !status)
    {
        return status;
    }
    return headless_ ? lgp::framework::Status{} : InitializeImGui();
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D drawableSize)
{
    return CreateSizeDependentResources(drawableSize);
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &)
{
    return {};
}

lgp::framework::Status Renderer::BuildUi(lgp::framework::UpdateContext const &context)
{
    if (headless_ || !imguiInitialized_)
    {
        return {};
    }

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(context.drawableSize.width), static_cast<float>(context.drawableSize.height)};
    io.DeltaTime = static_cast<float>((std::max)(context.deltaSeconds, 1.0 / 240.0));
    io.AddMousePosEvent(static_cast<float>(context.input.mouse.x), static_cast<float>(context.input.mouse.y));
    io.AddMouseButtonEvent(0, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Left));
    io.AddMouseWheelEvent(0.0F, context.input.mouse.wheelDelta);

    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    imguiFrameBegun_ = true;

    int scenarioIndex = static_cast<int>(scenario_);
    char const *items =
        "Static\0Jitter A\0Jitter B\0Motion Previous\0Motion Current\0Identity/Depth Previous\0Identity/Depth Current\0"
        "Exposure Previous\0Exposure Valid\0Exposure Excessive\0Previous W Invalid\0";
    ImGui::Begin("Reprojection diagnostics");
    ImGui::Combo("Scenario", &scenarioIndex, items);
    scenario_ = static_cast<gpu::Scenario>(scenarioIndex);
    if (ImGui::Button("Reset history"))
    {
        resetRequested_ = true;
    }
    ImGui::Text("History: %s", hasHistory_ ? "available" : "empty");
    ImGui::Text("Current jitter: (%.5f, %.5f)", gpu::MakeScenarioState(scenario_).currentJitterUv.x,
                gpu::MakeScenarioState(scenario_).currentJitterUv.y);
    ImGui::Text("Previous jitter: (%.5f, %.5f)", previousJitterUv_.x, previousJitterUv_.y);
    ImGui::Text("Current exposure: %.3f", static_cast<double>(gpu::MakeScenarioState(scenario_).currentPreExposure));
    ImGui::Text("Previous exposure: %.3f", static_cast<double>(previousPreExposure_));
    ImGui::TextWrapped("The composite highlights valid reprojection in green. No-history and reset frames tint magenta "
                       "so the missing temporal reservoir stays explicit.");
    ImGui::End();
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr)
    {
        return std::unexpected(lgp::framework::MakeError("Solution::Renderer::Render",
                                                         "The frame context is missing required D3D12 handles."));
    }
    if (frameContext.drawableSize.empty() || frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Solution::Renderer::Render",
                                                         "Size-dependent Solution resources are unavailable."));
    }

    bool const resetThisFrame = resetRequested_;
    gpu::ScenarioState const scenarioState = gpu::MakeScenarioState(scenario_);
    EnsureDistinctHistoryIndices();
    UpdateHistorySrvs(frameContext.frameSlot, historyReadIndex_);

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
        WriteFrameConstants(frameContext.frameSlot, frameContext.drawableSize, resetThisFrame);
    commandList.SetComputeRootConstantBufferView(0U, constantsAddress);
    commandList.SetGraphicsRootConstantBufferView(0U, constantsAddress);
    commandList.SetComputeRootDescriptorTable(
        1U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, 0U, false)));
    commandList.SetGraphicsRootDescriptorTable(
        1U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, 0U, false)));
    commandList.SetComputeRootDescriptorTable(
        2U, textureDescriptors_.GpuHandle(DescriptorIndex(frameContext.frameSlot, 0U, true)));

    lgp::framework::TextureBarrierState const commonState{
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        D3D12_BARRIER_LAYOUT_COMMON,
    };
    std::vector<D3D12_TEXTURE_BARRIER> barriers{};
    barriers.reserve(TextureCountU32() + 4U);
    for (UINT textureIndex = 0U; textureIndex < TextureCountU32(); ++textureIndex)
    {
        barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[textureIndex].Get(), commonState,
                                                   gpu::ComputeUnorderedAccessState(),
                                                   D3D12_TEXTURE_BARRIER_FLAG_DISCARD));
    }
    gpu::SubmitTextureBarriers(commandList, barriers);

    commandList.SetPipelineState(generatePipeline_.Get());
    commandList.Dispatch((frameContext.drawableSize.width + gpu::kThreadsX - 1U) / gpu::kThreadsX,
                         (frameContext.drawableSize.height + gpu::kThreadsY - 1U) / gpu::kThreadsY, 1U);

    barriers.clear();
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::CurrentColor)].Get(),
                                               gpu::ComputeUnorderedAccessState(), gpu::ComputeShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::CurrentDepth)].Get(),
                                               gpu::ComputeUnorderedAccessState(), gpu::ComputeShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::CurrentIdentity)].Get(),
                                               gpu::ComputeUnorderedAccessState(), gpu::ComputeShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::MotionClipDepth)].Get(),
                                               gpu::ComputeUnorderedAccessState(), gpu::ComputeShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*history_[historyReadIndex_].color.Get(), commonState,
                                               gpu::ComputeShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*history_[historyReadIndex_].depth.Get(), commonState,
                                               gpu::ComputeShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*history_[historyReadIndex_].identity.Get(), commonState,
                                               gpu::ComputeShaderResourceState()));
    gpu::SubmitTextureBarriers(commandList, barriers);

    commandList.SetPipelineState(validatePipeline_.Get());
    commandList.Dispatch((frameContext.drawableSize.width + gpu::kThreadsX - 1U) / gpu::kThreadsX,
                         (frameContext.drawableSize.height + gpu::kThreadsY - 1U) / gpu::kThreadsY, 1U);

    barriers.clear();
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::CurrentColor)].Get(),
                                               gpu::ComputeShaderResourceState(), gpu::PixelShaderResourceState()));
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
    commandList.SetPipelineState(compositePipeline_.Get());
    commandList.DrawInstanced(3U, 1U, 0U, 0U);

    if (imguiFrameBegun_)
    {
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), &commandList);
        imguiFrameBegun_ = false;
    }

    barriers.clear();
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::CurrentColor)].Get(),
                                               gpu::PixelShaderResourceState(), gpu::CopySourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::CurrentDepth)].Get(),
                                               gpu::ComputeShaderResourceState(), gpu::CopySourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::CurrentIdentity)].Get(),
                                               gpu::ComputeShaderResourceState(), gpu::CopySourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*history_[historyReadIndex_].color.Get(),
                                               gpu::ComputeShaderResourceState(), commonState));
    barriers.push_back(gpu::MakeTextureBarrier(*history_[historyReadIndex_].depth.Get(),
                                               gpu::ComputeShaderResourceState(), commonState));
    barriers.push_back(gpu::MakeTextureBarrier(*history_[historyReadIndex_].identity.Get(),
                                               gpu::ComputeShaderResourceState(), commonState));
    barriers.push_back(
        gpu::MakeTextureBarrier(*history_[historyWriteIndex_].color.Get(), commonState, gpu::CopyDestState()));
    barriers.push_back(
        gpu::MakeTextureBarrier(*history_[historyWriteIndex_].depth.Get(), commonState, gpu::CopyDestState()));
    barriers.push_back(
        gpu::MakeTextureBarrier(*history_[historyWriteIndex_].identity.Get(), commonState, gpu::CopyDestState()));
    gpu::SubmitTextureBarriers(commandList, barriers);

    commandList.CopyResource(history_[historyWriteIndex_].color.Get(),
                             slot.textures[static_cast<UINT>(TextureIndex::CurrentColor)].Get());
    commandList.CopyResource(history_[historyWriteIndex_].depth.Get(),
                             slot.textures[static_cast<UINT>(TextureIndex::CurrentDepth)].Get());
    commandList.CopyResource(history_[historyWriteIndex_].identity.Get(),
                             slot.textures[static_cast<UINT>(TextureIndex::CurrentIdentity)].Get());

    barriers.clear();
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::CurrentColor)].Get(),
                                               gpu::CopySourceState(), commonState));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::CurrentDepth)].Get(),
                                               gpu::CopySourceState(), commonState));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::CurrentIdentity)].Get(),
                                               gpu::CopySourceState(), commonState));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::MotionClipDepth)].Get(),
                                               gpu::ComputeShaderResourceState(), commonState));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::PreviousHistoryUv)].Get(),
                                               gpu::ComputeUnorderedAccessState(), commonState));
    barriers.push_back(
        gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::ReprojectedHistoryColor)].Get(),
                                gpu::PixelShaderResourceState(), commonState));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::RejectionReasons)].Get(),
                                               gpu::PixelShaderResourceState(), commonState));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[static_cast<UINT>(TextureIndex::ExposureScale)].Get(),
                                               gpu::PixelShaderResourceState(), commonState));
    barriers.push_back(
        gpu::MakeTextureBarrier(*history_[historyWriteIndex_].color.Get(), gpu::CopyDestState(), commonState));
    barriers.push_back(
        gpu::MakeTextureBarrier(*history_[historyWriteIndex_].depth.Get(), gpu::CopyDestState(), commonState));
    barriers.push_back(
        gpu::MakeTextureBarrier(*history_[historyWriteIndex_].identity.Get(), gpu::CopyDestState(), commonState));
    barriers.push_back(gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(),
                                               gpu::FrameEndState(frameContext.headless)));
    gpu::SubmitTextureBarriers(commandList, barriers);

    previousJitterUv_ = scenarioState.currentJitterUv;
    previousPreExposure_ = scenarioState.currentPreExposure;
    hasHistory_ = true;
    historyReadIndex_ = historyWriteIndex_;
    historyWriteIndex_ = 1U - historyWriteIndex_;
    EnsureDistinctHistoryIndices();
    lastRenderedFrameSlot_ = frameContext.frameSlot;
    resetRequested_ = false;
    return {};
}

std::expected<Renderer::ReadbackOutputs, lgp::framework::Error> Renderer::ReadBackOutputs() const
{
    if (frameSlots_.empty() || lastDrawableSize_.empty())
    {
        return std::unexpected(
            lgp::framework::MakeError("Renderer::ReadBackOutputs", "No Solution frame has been rendered yet."));
    }

    ReadbackOutputs outputs{};
    auto currentDepth = gpu::ReadBackTexture(
        *deviceResources_,
        *frameSlots_[lastRenderedFrameSlot_].textures[static_cast<UINT>(TextureIndex::CurrentDepth)].Get(),
        gpu::FrameStartState(true));
    if (!currentDepth)
    {
        return std::unexpected(std::move(currentDepth.error()));
    }
    outputs.currentDepth = std::move(*currentDepth);

    auto currentIdentity = gpu::ReadBackTexture(
        *deviceResources_,
        *frameSlots_[lastRenderedFrameSlot_].textures[static_cast<UINT>(TextureIndex::CurrentIdentity)].Get(),
        gpu::FrameStartState(true));
    if (!currentIdentity)
    {
        return std::unexpected(std::move(currentIdentity.error()));
    }
    outputs.currentIdentity = std::move(*currentIdentity);

    auto motionClipDepth = gpu::ReadBackTexture(
        *deviceResources_,
        *frameSlots_[lastRenderedFrameSlot_].textures[static_cast<UINT>(TextureIndex::MotionClipDepth)].Get(),
        gpu::FrameStartState(true));
    if (!motionClipDepth)
    {
        return std::unexpected(std::move(motionClipDepth.error()));
    }
    outputs.motionClipDepth = std::move(*motionClipDepth);

    auto previousHistoryUv = gpu::ReadBackTexture(
        *deviceResources_,
        *frameSlots_[lastRenderedFrameSlot_].textures[static_cast<UINT>(TextureIndex::PreviousHistoryUv)].Get(),
        gpu::FrameStartState(true));
    if (!previousHistoryUv)
    {
        return std::unexpected(std::move(previousHistoryUv.error()));
    }
    outputs.previousHistoryUv = std::move(*previousHistoryUv);

    auto rejectionReasons = gpu::ReadBackTexture(
        *deviceResources_,
        *frameSlots_[lastRenderedFrameSlot_].textures[static_cast<UINT>(TextureIndex::RejectionReasons)].Get(),
        gpu::FrameStartState(true));
    if (!rejectionReasons)
    {
        return std::unexpected(std::move(rejectionReasons.error()));
    }
    outputs.rejectionReasons = std::move(*rejectionReasons);

    auto exposureScale = gpu::ReadBackTexture(
        *deviceResources_,
        *frameSlots_[lastRenderedFrameSlot_].textures[static_cast<UINT>(TextureIndex::ExposureScale)].Get(),
        gpu::FrameStartState(true));
    if (!exposureScale)
    {
        return std::unexpected(std::move(exposureScale.error()));
    }
    outputs.exposureScale = std::move(*exposureScale);
    return outputs;
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    if (imguiFrameBegun_)
    {
        ImGui::EndFrame();
        imguiFrameBegun_ = false;
    }
    if (imguiInitialized_)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized_ = false;
    }
    if (deviceResources_ != nullptr && imguiFontDescriptor_)
    {
        deviceResources_->shader_visible_cbv_srv_uav_heap().Free(imguiFontDescriptor_);
        imguiFontDescriptor_ = {};
    }

    ReleaseSizeDependentResources();
    compositePipeline_.Reset();
    validatePipeline_.Reset();
    generatePipeline_.Reset();
    rootSignature_.Reset();
}

} // namespace ch11::reprojection::solution
