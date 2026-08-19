#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <wrl/client.h>

#include <DirectXMath.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

#include <lgp/framework/buffer.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

namespace ch02::rasterization::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
inline constexpr std::size_t kCullModeCount = 3U;
inline constexpr std::size_t kWindingCount = 2U;
inline constexpr std::size_t kDepthModeCount = 6U;
inline constexpr std::size_t kPipelineCount = kCullModeCount * kWindingCount * kDepthModeCount;

enum class Scene : int
{
    Overlap = 0,
    Perspective,
    Clipping,
};

enum class Interpolation : int
{
    PerspectiveCorrect = 0,
    Affine,
};

enum class Visualization : int
{
    Attribute = 0,
    Depth,
};

enum class DepthMode : int
{
    Disabled = 0,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Always,
};

struct TriangleVertex final
{
    DirectX::XMFLOAT4 position{};
    DirectX::XMFLOAT3 attribute{};
};

struct Controls final
{
    Scene scene{Scene::Overlap};
    Interpolation interpolation{Interpolation::PerspectiveCorrect};
    Visualization visualization{Visualization::Attribute};
    D3D12_CULL_MODE cullMode{D3D12_CULL_MODE_BACK};
    DepthMode depthMode{DepthMode::Less};
    bool frontCounterClockwise{false};
    bool reverseDrawOrder{false};
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
    return std::filesystem::path{__FILE__}.parent_path() / "Rasterization.hlsl";
}

[[nodiscard]] constexpr std::size_t PipelineIndex(D3D12_CULL_MODE cullMode, bool frontCounterClockwise,
                                                  DepthMode depthMode) noexcept
{
    std::size_t const cullIndex = static_cast<std::size_t>(cullMode - D3D12_CULL_MODE_NONE);
    std::size_t const windingIndex = frontCounterClockwise ? 1U : 0U;
    return ((cullIndex * kWindingCount) + windingIndex) * kDepthModeCount + static_cast<std::size_t>(depthMode);
}

[[nodiscard]] constexpr D3D12_COMPARISON_FUNC DepthFunction(DepthMode mode) noexcept
{
    switch (mode)
    {
    case DepthMode::Less:
        return D3D12_COMPARISON_FUNC_LESS;
    case DepthMode::LessEqual:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case DepthMode::Greater:
        return D3D12_COMPARISON_FUNC_GREATER;
    case DepthMode::GreaterEqual:
        return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case DepthMode::Always:
    case DepthMode::Disabled:
        return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

[[nodiscard]] constexpr float ClearDepth(DepthMode mode) noexcept
{
    return mode == DepthMode::Greater || mode == DepthMode::GreaterEqual ? 0.0F : 1.0F;
}

[[nodiscard]] constexpr UINT SceneFirstVertex(Scene scene) noexcept
{
    switch (scene)
    {
    case Scene::Overlap:
        return 0U;
    case Scene::Perspective:
        return 6U;
    case Scene::Clipping:
        return 9U;
    }
    return 0U;
}

[[nodiscard]] constexpr UINT SceneVertexCount(Scene scene) noexcept
{
    return scene == Scene::Overlap ? 6U : 3U;
}

[[nodiscard]] std::array<TriangleVertex, 12U> MakeVertices()
{
    return {{
        {{-0.72F, -0.62F, 0.72F, 1.0F}, {1.0F, 0.12F, 0.08F}},
        {{0.00F, 0.78F, 0.72F, 1.0F}, {0.08F, 1.0F, 0.14F}},
        {{0.72F, -0.62F, 0.72F, 1.0F}, {0.10F, 0.18F, 1.0F}},
        {{-0.48F, -0.42F, 0.28F, 1.0F}, {1.0F, 0.78F, 0.10F}},
        {{0.16F, 0.58F, 0.28F, 1.0F}, {0.10F, 1.0F, 0.82F}},
        {{0.62F, -0.38F, 0.28F, 1.0F}, {0.90F, 0.12F, 1.0F}},
        {{-0.44F, -0.36F, 0.33F, 0.55F}, {1.0F, 0.0F, 0.0F}},
        {{0.00F, 1.20F, 0.96F, 1.60F}, {0.0F, 1.0F, 0.0F}},
        {{0.60F, -0.48F, 0.48F, 0.80F}, {0.0F, 0.0F, 1.0F}},
        {{-0.70F, -0.70F, 0.45F, 1.0F}, {1.0F, 0.20F, 0.10F}},
        {{0.20F, 0.78F, 0.45F, 1.0F}, {0.10F, 1.0F, 0.20F}},
        {{1.55F, -0.35F, 0.45F, 1.0F}, {0.15F, 0.25F, 1.0F}},
    }};
}

} // namespace

class Renderer::Impl final
{
  public:
    lgp::framework::DeviceResources *deviceResources{};
    lgp::framework::CompiledShader vertexShader{};
    lgp::framework::CompiledShader pixelShader{};
    ComPtr<ID3D12RootSignature> rootSignature{};
    std::array<ComPtr<ID3D12PipelineState>, kPipelineCount> pipelineStates{};
    lgp::framework::Buffer vertexBuffer{};
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
    lgp::framework::DescriptorHeap dsvHeap{};
    lgp::framework::DescriptorAllocation depthView{};
    ComPtr<ID3D12Resource> depthTarget{};
    lgp::framework::DescriptorAllocation imguiFontDescriptor{};
    Controls controls{};
    bool headless{};
    bool imguiInitialized{};
    bool imguiFrameBegun{};

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context);
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelineStates();
    [[nodiscard]] lgp::framework::Status CreateVertexBuffer();
    [[nodiscard]] lgp::framework::Status CreateDepthTarget(lgp::framework::Extent2D size);
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

    options.entryPoint = L"VSMain";
    options.targetProfile = L"vs_6_0";
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto vertexResult = compiler.Compile(options);
    if (!vertexResult)
    {
        return std::unexpected(std::move(vertexResult.error()));
    }
    vertexShader = std::move(vertexResult.value());

    options.entryPoint = L"PSMain";
    options.targetProfile = L"ps_6_0";
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto pixelResult = compiler.Compile(options);
    if (!pixelResult)
    {
        return std::unexpected(std::move(pixelResult.error()));
    }
    pixelShader = std::move(pixelResult.value());
    return {};
}

lgp::framework::Status Renderer::Impl::CreateRootSignature()
{
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.ShaderRegister = 0U;
    parameter.Constants.RegisterSpace = 0U;
    parameter.Constants.Num32BitValues = 2U;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 1U;
    description.pParameters = &parameter;
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
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                             "Failed to create the rasterization lab root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::Impl::CreatePipelineStates()
{
    D3D12_INPUT_ELEMENT_DESC const inputLayout[]{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"ATTRIBUTE", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 16U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    };

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    for (D3D12_CULL_MODE cullMode : {
             D3D12_CULL_MODE_NONE,
             D3D12_CULL_MODE_FRONT,
             D3D12_CULL_MODE_BACK,
         })
    {
        for (bool const frontCounterClockwise : {false, true})
        {
            for (int depthValue = 0; depthValue < static_cast<int>(kDepthModeCount); ++depthValue)
            {
                auto const depthMode = static_cast<DepthMode>(depthValue);

                D3D12_RASTERIZER_DESC rasterizer{};
                rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
                rasterizer.CullMode = cullMode;
                rasterizer.FrontCounterClockwise = frontCounterClockwise;
                rasterizer.DepthClipEnable = TRUE;

                D3D12_DEPTH_STENCIL_DESC depth{};
                depth.DepthEnable = depthMode != DepthMode::Disabled;
                depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                depth.DepthFunc = DepthFunction(depthMode);

                D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
                description.pRootSignature = rootSignature.Get();
                description.VS = vertexShader.Bytecode();
                description.PS = pixelShader.Bytecode();
                description.BlendState = blend;
                description.SampleMask = UINT_MAX;
                description.RasterizerState = rasterizer;
                description.DepthStencilState = depth;
                description.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
                description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                description.NumRenderTargets = 1U;
                description.RTVFormats[0] = deviceResources->back_buffer_format();
                description.DSVFormat = kDepthFormat;
                description.SampleDesc.Count = 1U;

                auto &pipelineState = pipelineStates[PipelineIndex(cullMode, frontCounterClockwise, depthMode)];
                HRESULT const result = deviceResources->device()->CreateGraphicsPipelineState(
                    &description, IID_PPV_ARGS(pipelineState.ReleaseAndGetAddressOf()));
                if (FAILED(result))
                {
                    return std::unexpected(lgp::framework::MakeHResultError(
                        "ID3D12Device::CreateGraphicsPipelineState", result,
                        "Failed to create a rasterization lab pipeline-state variant."));
                }
            }
        }
    }
    return {};
}

lgp::framework::Status Renderer::Impl::CreateVertexBuffer()
{
    auto const vertices = MakeVertices();
    auto bufferResult =
        lgp::framework::CreateUploadBuffer(*deviceResources->device(), sizeof(vertices), L"Ch02 Triangle Vertices");
    if (!bufferResult)
    {
        return std::unexpected(std::move(bufferResult.error()));
    }
    vertexBuffer = std::move(bufferResult.value());

    auto const writeStatus = lgp::framework::WriteBuffer(vertexBuffer, std::span<TriangleVertex const>{vertices});
    if (!writeStatus)
    {
        return std::unexpected(std::move(writeStatus.error()));
    }

    vertexBufferView.BufferLocation = vertexBuffer.gpu_virtual_address();
    vertexBufferView.SizeInBytes = static_cast<UINT>(sizeof(vertices));
    vertexBufferView.StrideInBytes = sizeof(TriangleVertex);
    return {};
}

lgp::framework::Status Renderer::Impl::CreateDepthTarget(lgp::framework::Extent2D size)
{
    depthTarget.Reset();
    if (size.empty())
    {
        return {};
    }

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = kDepthFormat;
    clearValue.DepthStencil.Depth = 1.0F;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = size.width;
    description.Height = size.height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = kDepthFormat;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    HRESULT const result = deviceResources->device()->CreateCommittedResource3(
        &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, &clearValue, nullptr, 0U,
        nullptr, IID_PPV_ARGS(depthTarget.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                                "Failed to create the chapter depth target."));
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = kDepthFormat;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    deviceResources->device()->CreateDepthStencilView(depthTarget.Get(), &view, depthView.cpuHandle);
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
    info.DSVFormat = kDepthFormat;
    info.SrvDescriptorHeap = deviceResources->shader_visible_cbv_srv_uav_heap().Get();
    info.LegacySingleSrvCpuDescriptor = imguiFontDescriptor.cpuHandle;
    info.LegacySingleSrvGpuDescriptor = imguiFontDescriptor.gpuHandle;
    if (!ImGui_ImplDX12_Init(&info))
    {
        return std::unexpected(
            lgp::framework::MakeError("ImGui_ImplDX12_Init", "Failed to initialize the rasterization lab UI."));
    }
    imguiInitialized = true;
    return {};
}

lgp::framework::Status Renderer::Impl::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources = &context.deviceResources;
    headless = context.commandLine.headless;

    auto heapResult = lgp::framework::CreateDescriptorHeap(*deviceResources->device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                                           1U, false, L"Ch02 Depth Descriptors");
    if (!heapResult)
    {
        return std::unexpected(std::move(heapResult.error()));
    }
    dsvHeap = std::move(heapResult.value());
    auto viewResult = dsvHeap.Allocate(1U);
    if (!viewResult)
    {
        return std::unexpected(std::move(viewResult.error()));
    }
    depthView = viewResult.value();

    auto shaderStatus = CreateShaders();
    if (!shaderStatus)
    {
        return shaderStatus;
    }
    auto rootStatus = CreateRootSignature();
    if (!rootStatus)
    {
        return rootStatus;
    }
    auto pipelineStatus = CreatePipelineStates();
    if (!pipelineStatus)
    {
        return pipelineStatus;
    }
    auto vertexStatus = CreateVertexBuffer();
    if (!vertexStatus)
    {
        return vertexStatus;
    }
    auto depthStatus = CreateDepthTarget(context.drawableSize);
    if (!depthStatus)
    {
        return depthStatus;
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

    ImGui::Begin("Rasterization Lab");
    int scene = static_cast<int>(controls.scene);
    ImGui::Combo("Scene", &scene, "Overlapping triangles\0Perspective interpolation\0Clip-plane crossing\0");
    controls.scene = static_cast<Scene>(scene);

    int interpolation = static_cast<int>(controls.interpolation);
    ImGui::Combo("Interpolation", &interpolation, "Perspective-correct\0Affine (noperspective)\0");
    controls.interpolation = static_cast<Interpolation>(interpolation);

    int visualization = static_cast<int>(controls.visualization);
    ImGui::Combo("Visualization", &visualization, "Vertex attributes\0Depth\0");
    controls.visualization = static_cast<Visualization>(visualization);

    int cullMode = static_cast<int>(controls.cullMode) - static_cast<int>(D3D12_CULL_MODE_NONE);
    ImGui::Combo("Cull mode", &cullMode, "None\0Front\0Back\0");
    controls.cullMode = static_cast<D3D12_CULL_MODE>(cullMode + static_cast<int>(D3D12_CULL_MODE_NONE));

    ImGui::Checkbox("Front faces are counter-clockwise", &controls.frontCounterClockwise);

    int depthMode = static_cast<int>(controls.depthMode);
    ImGui::Combo("Depth test", &depthMode, "Disabled\0Less\0Less equal\0Greater\0Greater equal\0Always\0");
    controls.depthMode = static_cast<DepthMode>(depthMode);
    ImGui::Checkbox("Reverse draw order", &controls.reverseDrawOrder);

    ImGui::Separator();
    ImGui::TextWrapped("The RGB corners form barycentric basis attributes. Compare interpolation modes in the "
                       "perspective scene, or switch depth off and reverse draw order to expose ordering.");
    ImGui::End();
    return {};
}

lgp::framework::Status Renderer::Impl::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.renderTarget == nullptr || depthTarget == nullptr)
    {
        if (imguiFrameBegun)
        {
            ImGui::EndFrame();
            imguiFrameBegun = false;
        }
        return {};
    }

    ID3D12GraphicsCommandList7 *const commandList = frameContext.commandList;
    lgp::framework::TextureBarrierState const initialState{
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        frameContext.renderTargetInitialLayout,
    };
    lgp::framework::TextureBarrierState constexpr renderTargetState{
        D3D12_BARRIER_SYNC_RENDER_TARGET,
        D3D12_BARRIER_ACCESS_RENDER_TARGET,
        D3D12_BARRIER_LAYOUT_RENDER_TARGET,
    };
    lgp::framework::TransitionTexture(*commandList, *frameContext.renderTarget, initialState, renderTargetState);

    float const clearColor[]{0.035F, 0.055F, 0.085F, 1.0F};
    commandList->OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, &depthView.cpuHandle);
    commandList->ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    commandList->ClearDepthStencilView(depthView.cpuHandle, D3D12_CLEAR_FLAG_DEPTH, ClearDepth(controls.depthMode), 0U,
                                       0U, nullptr);
    commandList->RSSetViewports(1U, &frameContext.viewport);
    commandList->RSSetScissorRects(1U, &frameContext.scissorRect);

    ID3D12DescriptorHeap *const descriptorHeaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList->SetDescriptorHeaps(1U, descriptorHeaps);
    commandList->SetGraphicsRootSignature(rootSignature.Get());
    commandList->SetPipelineState(
        pipelineStates[PipelineIndex(controls.cullMode, controls.frontCounterClockwise, controls.depthMode)].Get());
    std::uint32_t const displayConstants[]{
        static_cast<std::uint32_t>(controls.interpolation),
        static_cast<std::uint32_t>(controls.visualization),
    };
    commandList->SetGraphicsRoot32BitConstants(0U, 2U, displayConstants, 0U);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0U, 1U, &vertexBufferView);

    UINT const firstVertex = SceneFirstVertex(controls.scene);
    UINT const vertexCount = SceneVertexCount(controls.scene);
    if (controls.scene == Scene::Overlap && controls.reverseDrawOrder)
    {
        commandList->DrawInstanced(3U, 1U, firstVertex + 3U, 0U);
        commandList->DrawInstanced(3U, 1U, firstVertex, 0U);
    }
    else
    {
        commandList->DrawInstanced(vertexCount, 1U, firstVertex, 0U);
    }

    if (imguiFrameBegun)
    {
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
        imguiFrameBegun = false;
    }

    lgp::framework::TextureBarrierState const endState{
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT,
    };
    lgp::framework::TransitionTexture(*commandList, *frameContext.renderTarget, renderTargetState, endState);
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
    }
    depthTarget.Reset();
    if (depthView)
    {
        dsvHeap.Free(depthView);
    }
    depthView = {};
    dsvHeap = {};
    vertexBuffer = {};
    for (auto &pipelineState : pipelineStates)
    {
        pipelineState.Reset();
    }
    rootSignature.Reset();
    deviceResources = nullptr;
}

Renderer::Renderer() : impl_{std::make_unique<Impl>()} {}

Renderer::~Renderer() = default;
Renderer::Renderer(Renderer &&) noexcept = default;
Renderer &Renderer::operator=(Renderer &&) noexcept = default;

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    return impl_->Initialize(context);
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D drawableSize)
{
    return impl_->CreateDepthTarget(drawableSize);
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

} // namespace ch02::rasterization::solution
