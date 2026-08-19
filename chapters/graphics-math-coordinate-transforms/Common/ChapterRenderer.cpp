#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ChapterRenderer.hpp"

#include "ChapterGeometry.hpp"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <wrl/client.h>

#include <DirectXMath.h>
#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lgp/framework/buffer.hpp>
#include <lgp/framework/shader_compiler.hpp>

namespace ch01::graphics_math
{
namespace
{

using Microsoft::WRL::ComPtr;

struct alignas(16) LinePassConstants final
{
    XMFLOAT4X4 model{};
    XMFLOAT4X4 viewProjection{};
    XMFLOAT4 colorTint{};
};

static_assert(sizeof(LinePassConstants) % 16U == 0U);

struct StaticMeshBuffers final
{
    lgp::framework::Buffer vertexBuffer{};
    lgp::framework::Buffer indexBuffer{};
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW indexBufferView{};
    UINT indexCount{};
};

inline constexpr UINT kMaxDrawsPerFrame = 24U;
inline constexpr float kAxisScale = 1.6F;
inline constexpr float kCameraAxisScale = 0.9F;
inline constexpr float kSelectedPointScale = 0.08F;

[[nodiscard]] DirectX::XMMATRIX LoadMatrix(XMFLOAT4X4 const &matrix) noexcept
{
    return DirectX::XMLoadFloat4x4(&matrix);
}

[[nodiscard]] XMFLOAT4X4 StoreMatrixValue(DirectX::XMMATRIX matrix) noexcept
{
    XMFLOAT4X4 storedMatrix{};
    DirectX::XMStoreFloat4x4(&storedMatrix, matrix);
    return storedMatrix;
}

[[nodiscard]] DirectX::XMVECTOR LoadFloat3(XMFLOAT3 const &value) noexcept
{
    return DirectX::XMVectorSet(value.x, value.y, value.z, 0.0F);
}

[[nodiscard]] DirectX::XMVECTOR LoadPoint(XMFLOAT3 const &value) noexcept
{
    return DirectX::XMVectorSet(value.x, value.y, value.z, 1.0F);
}

[[nodiscard]] DirectX::XMMATRIX BuildBasisTransform(DirectX::XMVECTOR right, DirectX::XMVECTOR up,
                                                    DirectX::XMVECTOR forward, DirectX::XMVECTOR translation) noexcept
{
    DirectX::XMMATRIX matrix = DirectX::XMMatrixIdentity();
    matrix.r[0] = right;
    matrix.r[1] = up;
    matrix.r[2] = forward;
    matrix.r[3] = DirectX::XMVectorSetW(translation, 1.0F);
    return matrix;
}

[[nodiscard]] DirectX::XMMATRIX BuildCameraBasisTransform(ChapterDerivedScene const &scene, float scale) noexcept
{
    return BuildBasisTransform(DirectX::XMVectorScale(LoadFloat3(scene.cameraRight), scale),
                               DirectX::XMVectorScale(LoadFloat3(scene.cameraUp), scale),
                               DirectX::XMVectorScale(LoadFloat3(scene.cameraForward), scale),
                               LoadPoint(scene.cameraPosition));
}

[[nodiscard]] DirectX::XMMATRIX BuildTranslationScaleTransform(XMFLOAT3 position, float uniformScale) noexcept
{
    return DirectX::XMMatrixScaling(uniformScale, uniformScale, uniformScale) *
           DirectX::XMMatrixTranslation(position.x, position.y, position.z);
}

[[nodiscard]] DirectX::XMMATRIX BuildRayTransform(XMFLOAT3 origin, XMFLOAT3 direction) noexcept
{
    DirectX::XMVECTOR const directionVector = LoadFloat3(direction);
    float const length = DirectX::XMVectorGetX(DirectX::XMVector3Length(directionVector));
    if (length <= 1.0e-5F)
    {
        return DirectX::XMMatrixIdentity();
    }

    DirectX::XMVECTOR const forward = DirectX::XMVector3Normalize(directionVector);
    DirectX::XMVECTOR referenceUp = LoadFloat3(kWorldUp);
    if (std::fabs(DirectX::XMVectorGetX(DirectX::XMVector3Dot(referenceUp, forward))) > 0.98F)
    {
        referenceUp = DirectX::XMVectorSet(1.0F, 0.0F, 0.0F, 0.0F);
    }

    DirectX::XMVECTOR right = DirectX::XMVector3Cross(referenceUp, forward);
    right = DirectX::XMVector3Normalize(right);
    DirectX::XMVECTOR const up = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(forward, right));

    return BuildBasisTransform(right, up, DirectX::XMVectorScale(forward, length), LoadPoint(origin));
}

[[nodiscard]] XMFLOAT4 MakeColor(float red, float green, float blue, float alpha = 1.0F) noexcept
{
    return {red, green, blue, alpha};
}

[[nodiscard]] std::filesystem::path ResolveRepositoryRoot()
{
    return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path().parent_path();
}

[[nodiscard]] std::filesystem::path ResolveFrameworkShaderDirectory()
{
    return ResolveRepositoryRoot() / "framework" / "shaders";
}

[[nodiscard]] lgp::framework::Result<StaticMeshBuffers> CreateStaticMeshBuffers(ID3D12Device &device,
                                                                                IndexedLineMesh mesh,
                                                                                std::wstring_view debugName)
{
    std::uint64_t const vertexBufferSize = static_cast<std::uint64_t>(mesh.vertices.size_bytes());
    std::uint64_t const indexBufferSize = static_cast<std::uint64_t>(mesh.indices.size_bytes());

    auto vertexBuffer =
        lgp::framework::CreateUploadBuffer(device, vertexBufferSize, std::wstring{debugName} + L" Vertex Buffer");
    if (!vertexBuffer)
    {
        return std::unexpected(std::move(vertexBuffer.error()));
    }

    auto indexBuffer =
        lgp::framework::CreateUploadBuffer(device, indexBufferSize, std::wstring{debugName} + L" Index Buffer");
    if (!indexBuffer)
    {
        return std::unexpected(std::move(indexBuffer.error()));
    }

    auto const writeVertexStatus = lgp::framework::WriteBuffer(vertexBuffer.value(), mesh.vertices);
    if (!writeVertexStatus)
    {
        return std::unexpected(std::move(writeVertexStatus.error()));
    }

    auto const writeIndexStatus = lgp::framework::WriteBuffer(indexBuffer.value(), mesh.indices);
    if (!writeIndexStatus)
    {
        return std::unexpected(std::move(writeIndexStatus.error()));
    }

    StaticMeshBuffers buffers{};
    buffers.vertexBuffer = std::move(vertexBuffer.value());
    buffers.indexBuffer = std::move(indexBuffer.value());
    buffers.vertexBufferView.BufferLocation = buffers.vertexBuffer.gpu_virtual_address();
    buffers.vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);
    buffers.vertexBufferView.StrideInBytes = sizeof(LineVertex);
    buffers.indexBufferView.BufferLocation = buffers.indexBuffer.gpu_virtual_address();
    buffers.indexBufferView.SizeInBytes = static_cast<UINT>(indexBufferSize);
    buffers.indexBufferView.Format = DXGI_FORMAT_R16_UINT;
    buffers.indexCount = static_cast<UINT>(mesh.indices.size());
    return buffers;
}

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

void DrawPassPreview(char const *label, XMFLOAT2 correctScreen, XMFLOAT2 wrongScreen, bool showWrong,
                     lgp::framework::Extent2D viewport)
{
    ImGui::TextUnformatted(label);
    ImVec2 const previewSize{260.0F, 160.0F};
    ImVec2 const previewOrigin = ImGui::GetCursorScreenPos();
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    ImVec2 const previewMax{previewOrigin.x + previewSize.x, previewOrigin.y + previewSize.y};

    drawList->AddRectFilled(previewOrigin, previewMax, IM_COL32(18, 22, 30, 255));
    drawList->AddRect(previewOrigin, previewMax, IM_COL32(96, 110, 135, 255));

    auto const toPreview = [&](XMFLOAT2 point)
    {
        float const width = viewport.width == 0U ? 1.0F : static_cast<float>(viewport.width);
        float const height = viewport.height == 0U ? 1.0F : static_cast<float>(viewport.height);
        return ImVec2{
            previewOrigin.x + (point.x / width) * previewSize.x,
            previewOrigin.y + (point.y / height) * previewSize.y,
        };
    };

    ImVec2 const correctPoint = toPreview(correctScreen);
    drawList->AddCircleFilled(correctPoint, 4.5F, IM_COL32(80, 220, 110, 255));
    drawList->AddText({correctPoint.x + 8.0F, correctPoint.y - 8.0F}, IM_COL32(200, 255, 210, 255), "correct");

    if (showWrong)
    {
        ImVec2 const wrongPoint = toPreview(wrongScreen);
        drawList->AddCircleFilled(wrongPoint, 4.5F, IM_COL32(255, 130, 110, 255));
        drawList->AddText({wrongPoint.x + 8.0F, wrongPoint.y + 2.0F}, IM_COL32(255, 215, 200, 255), "no divide");
    }

    ImGui::Dummy(previewSize);
}

template <typename T> [[nodiscard]] std::span<T const> SpanOfSingle(T const &value) noexcept
{
    return std::span<T const>(&value, 1U);
}

} // namespace

class ChapterRenderer::Impl final
{
  public:
    explicit Impl(std::unique_ptr<IChapterLogic> inputLogic) : logic{std::move(inputLogic)}
    {
        if (logic != nullptr)
        {
            logic->InitializeControls(controls);
        }
    }

    std::unique_ptr<IChapterLogic> logic{};
    lgp::framework::DeviceResources *deviceResources{};
    lgp::framework::CompiledShader vertexShader{};
    lgp::framework::CompiledShader pixelShader{};
    ComPtr<ID3D12RootSignature> rootSignature{};
    ComPtr<ID3D12PipelineState> pipelineState{};
    StaticMeshBuffers objectMesh{};
    StaticMeshBuffers axisMesh{};
    StaticMeshBuffers frustumMesh{};
    StaticMeshBuffers crossMesh{};
    StaticMeshBuffers rayMesh{};
    lgp::framework::Buffer constantBuffer{};
    UINT constantBufferStride{};
    UINT constantBufferBytesPerFrame{};
    ChapterControls controls{};
    ChapterDerivedScene scene{};
    lgp::framework::Extent2D latestViewport{};
    double latestElapsedSeconds{};
    bool sceneValid{};
    bool headless{};
    bool imguiInitialized{};
    bool imguiFrameBegun{};
    lgp::framework::DescriptorAllocation imguiFontDescriptor{};

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context);
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &resources,
                                                  lgp::framework::Extent2D drawableSize);
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context);
    [[nodiscard]] lgp::framework::Status BuildUi(lgp::framework::UpdateContext const &context);
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext);
    void Shutdown() noexcept;

  private:
    [[nodiscard]] lgp::framework::Status RebuildScene();
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelineState();
    [[nodiscard]] lgp::framework::Status CreateMeshBuffers();
    [[nodiscard]] lgp::framework::Status CreateConstantBuffer();
    [[nodiscard]] lgp::framework::Status InitializeImGui();
    void BeginImGuiFrame(lgp::framework::UpdateContext const &context);
    void DrawUiControls(bool &changed);
    static void DrawMatrix(char const *label, XMFLOAT4X4 const &matrix);
    static void DrawFloat4(char const *label, XMFLOAT4 const &value);
    static void DrawFloat3(char const *label, XMFLOAT3 const &value);
    static void DrawFloat2(char const *label, XMFLOAT2 const &value);
    [[nodiscard]] lgp::framework::Result<D3D12_GPU_VIRTUAL_ADDRESS> WriteDrawConstants(
        lgp::framework::FrameContext const &frameContext, UINT drawIndex, LinePassConstants const &constants);
    [[nodiscard]] lgp::framework::Status DrawMesh(lgp::framework::FrameContext const &frameContext,
                                                  StaticMeshBuffers const &mesh, DirectX::XMMATRIX modelMatrix,
                                                  XMFLOAT4 colorTint, UINT &drawIndex);
};

lgp::framework::Status ChapterRenderer::Impl::CreateShaders()
{
    auto compiler = lgp::framework::ShaderCompiler::Create();
    if (!compiler)
    {
        return std::unexpected(std::move(compiler.error()));
    }
    lgp::framework::ShaderCompiler shaderCompiler = std::move(compiler.value());

    std::filesystem::path const shaderPath = logic->ShaderPath();
    std::filesystem::path const frameworkShaderDirectory = ResolveFrameworkShaderDirectory();

    lgp::framework::ShaderCompileOptions shaderOptions{};
    shaderOptions.sourcePath = shaderPath;
    shaderOptions.includeDirectories = {shaderPath.parent_path(), frameworkShaderDirectory};
#ifdef _DEBUG
    shaderOptions.enableDebugInformation = true;
    shaderOptions.optimize = false;
#endif

    lgp::framework::ShaderCompileOptions vertexOptions = shaderOptions;
    vertexOptions.entryPoint = L"VSMain";
    vertexOptions.targetProfile = L"vs_6_0";
    vertexOptions.additionalArguments = {L"-E", vertexOptions.entryPoint, L"-T", vertexOptions.targetProfile};
    auto vertexCompile = shaderCompiler.Compile(vertexOptions);
    if (!vertexCompile)
    {
        return std::unexpected(std::move(vertexCompile.error()));
    }
    vertexShader = std::move(vertexCompile.value());

    lgp::framework::ShaderCompileOptions pixelOptions = shaderOptions;
    pixelOptions.entryPoint = L"PSMain";
    pixelOptions.targetProfile = L"ps_6_0";
    pixelOptions.additionalArguments = {L"-E", pixelOptions.entryPoint, L"-T", pixelOptions.targetProfile};
    auto pixelCompile = shaderCompiler.Compile(pixelOptions);
    if (!pixelCompile)
    {
        return std::unexpected(std::move(pixelCompile.error()));
    }
    pixelShader = std::move(pixelCompile.value());

    return {};
}

lgp::framework::Status ChapterRenderer::Impl::CreateRootSignature()
{
    D3D12_ROOT_PARAMETER rootParameter{};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rootParameter.Descriptor.ShaderRegister = 0U;
    rootParameter.Descriptor.RegisterSpace = 0U;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.NumParameters = 1U;
    rootSignatureDesc.pParameters = &rootParameter;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSignature;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT const serializeResult = ::D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                                  serializedRootSignature.ReleaseAndGetAddressOf(),
                                                                  errorBlob.ReleaseAndGetAddressOf());

    if (FAILED(serializeResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult,
                                                                D3D12BlobToUtf8(errorBlob.Get())));
    }

    HRESULT const createResult = deviceResources->device()->CreateRootSignature(
        0U, serializedRootSignature->GetBufferPointer(), serializedRootSignature->GetBufferSize(),
        IID_PPV_ARGS(rootSignature.ReleaseAndGetAddressOf()));

    if (FAILED(createResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                             "Failed to create the chapter line-rendering root signature."));
    }

    return {};
}

lgp::framework::Status ChapterRenderer::Impl::CreatePipelineState()
{
    D3D12_INPUT_ELEMENT_DESC const inputLayout[]{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"COLOR", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 12U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    };

    D3D12_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthClipEnable = TRUE;
    rasterizerDesc.MultisampleEnable = FALSE;
    rasterizerDesc.AntialiasedLineEnable = TRUE;
    rasterizerDesc.ForcedSampleCount = 0U;
    rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blendDesc{};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC renderTargetBlendDesc{};
    renderTargetBlendDesc.BlendEnable = FALSE;
    renderTargetBlendDesc.LogicOpEnable = FALSE;
    renderTargetBlendDesc.SrcBlend = D3D12_BLEND_ONE;
    renderTargetBlendDesc.DestBlend = D3D12_BLEND_ZERO;
    renderTargetBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    renderTargetBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    renderTargetBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    renderTargetBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    renderTargetBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    renderTargetBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blendDesc.RenderTarget[0] = renderTargetBlendDesc;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
    depthStencilDesc.DepthEnable = FALSE;
    depthStencilDesc.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineStateDesc{};
    pipelineStateDesc.pRootSignature = rootSignature.Get();
    pipelineStateDesc.VS = vertexShader.Bytecode();
    pipelineStateDesc.PS = pixelShader.Bytecode();
    pipelineStateDesc.BlendState = blendDesc;
    pipelineStateDesc.SampleMask = UINT_MAX;
    pipelineStateDesc.RasterizerState = rasterizerDesc;
    pipelineStateDesc.DepthStencilState = depthStencilDesc;
    pipelineStateDesc.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    pipelineStateDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    pipelineStateDesc.NumRenderTargets = 1U;
    pipelineStateDesc.RTVFormats[0] = deviceResources->back_buffer_format();
    pipelineStateDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pipelineStateDesc.SampleDesc.Count = 1U;

    HRESULT const createResult = deviceResources->device()->CreateGraphicsPipelineState(
        &pipelineStateDesc, IID_PPV_ARGS(pipelineState.ReleaseAndGetAddressOf()));

    if (FAILED(createResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", createResult,
                                             "Failed to create the chapter line-rendering pipeline state."));
    }

    return {};
}

lgp::framework::Status ChapterRenderer::Impl::CreateMeshBuffers()
{
    auto objectMeshBuffers = CreateStaticMeshBuffers(*deviceResources->device(), ObjectMesh(), L"Ch01 Object");
    if (!objectMeshBuffers)
    {
        return std::unexpected(std::move(objectMeshBuffers.error()));
    }
    objectMesh = std::move(objectMeshBuffers.value());

    auto axisMeshBuffers = CreateStaticMeshBuffers(*deviceResources->device(), AxisMesh(), L"Ch01 Axis");
    if (!axisMeshBuffers)
    {
        return std::unexpected(std::move(axisMeshBuffers.error()));
    }
    axisMesh = std::move(axisMeshBuffers.value());

    auto frustumMeshBuffers = CreateStaticMeshBuffers(*deviceResources->device(), ClipFrustumMesh(), L"Ch01 Frustum");
    if (!frustumMeshBuffers)
    {
        return std::unexpected(std::move(frustumMeshBuffers.error()));
    }
    frustumMesh = std::move(frustumMeshBuffers.value());

    auto crossMeshBuffers = CreateStaticMeshBuffers(*deviceResources->device(), CrossMesh(), L"Ch01 Cross");
    if (!crossMeshBuffers)
    {
        return std::unexpected(std::move(crossMeshBuffers.error()));
    }
    crossMesh = std::move(crossMeshBuffers.value());

    auto rayMeshBuffers = CreateStaticMeshBuffers(*deviceResources->device(), RayMesh(), L"Ch01 Ray");
    if (!rayMeshBuffers)
    {
        return std::unexpected(std::move(rayMeshBuffers.error()));
    }
    rayMesh = std::move(rayMeshBuffers.value());

    return {};
}

lgp::framework::Status ChapterRenderer::Impl::CreateConstantBuffer()
{
    constantBufferStride =
        lgp::framework::AlignConstantBufferSize(static_cast<std::uint32_t>(sizeof(LinePassConstants)));
    constantBufferBytesPerFrame = constantBufferStride * kMaxDrawsPerFrame;

    std::uint64_t const totalBufferBytes =
        static_cast<std::uint64_t>(constantBufferBytesPerFrame) * deviceResources->back_buffer_count();

    auto constantBufferResult =
        lgp::framework::CreateUploadBuffer(*deviceResources->device(), totalBufferBytes, L"Ch01 Line Pass Constants");
    if (!constantBufferResult)
    {
        return std::unexpected(std::move(constantBufferResult.error()));
    }

    constantBuffer = std::move(constantBufferResult.value());
    return {};
}

lgp::framework::Status ChapterRenderer::Impl::InitializeImGui()
{
    auto descriptorAllocation = deviceResources->shader_visible_cbv_srv_uav_heap().Allocate(1U);
    if (!descriptorAllocation)
    {
        return std::unexpected(std::move(descriptorAllocation.error()));
    }
    imguiFontDescriptor = descriptorAllocation.value();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendPlatformName = "LGP.ManualInput";

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = deviceResources->device();
    initInfo.CommandQueue = deviceResources->graphics_queue();
    initInfo.NumFramesInFlight = static_cast<int>(deviceResources->back_buffer_count());
    initInfo.RTVFormat = deviceResources->back_buffer_format();
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = deviceResources->shader_visible_cbv_srv_uav_heap().Get();
    initInfo.LegacySingleSrvCpuDescriptor = imguiFontDescriptor.cpuHandle;
    initInfo.LegacySingleSrvGpuDescriptor = imguiFontDescriptor.gpuHandle;

    if (!ImGui_ImplDX12_Init(&initInfo))
    {
        return std::unexpected(lgp::framework::MakeError("ImGui_ImplDX12_Init",
                                                         "Failed to initialize Dear ImGui's D3D12 renderer backend."));
    }

    imguiInitialized = true;
    return {};
}

lgp::framework::Status ChapterRenderer::Impl::RebuildScene()
{
    auto sceneResult = logic->BuildScene(controls, latestViewport, latestElapsedSeconds);
    if (!sceneResult)
    {
        sceneValid = false;
        return std::unexpected(std::move(sceneResult.error()));
    }

    scene = std::move(sceneResult.value());
    sceneValid = true;
    return {};
}

lgp::framework::Status ChapterRenderer::Impl::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    if (logic == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("ChapterRenderer::Initialize", "No chapter logic implementation was provided."));
    }

    deviceResources = &context.deviceResources;
    latestViewport = context.drawableSize;
    latestElapsedSeconds = 0.0;
    headless = context.commandLine.headless;

    auto const shaderStatus = CreateShaders();
    if (!shaderStatus)
    {
        return std::unexpected(std::move(shaderStatus.error()));
    }

    auto const rootSignatureStatus = CreateRootSignature();
    if (!rootSignatureStatus)
    {
        return std::unexpected(std::move(rootSignatureStatus.error()));
    }

    auto const pipelineStateStatus = CreatePipelineState();
    if (!pipelineStateStatus)
    {
        return std::unexpected(std::move(pipelineStateStatus.error()));
    }

    auto const meshBuffersStatus = CreateMeshBuffers();
    if (!meshBuffersStatus)
    {
        return std::unexpected(std::move(meshBuffersStatus.error()));
    }

    auto const constantBufferStatus = CreateConstantBuffer();
    if (!constantBufferStatus)
    {
        return std::unexpected(std::move(constantBufferStatus.error()));
    }

    auto const sceneStatus = RebuildScene();
    if (!sceneStatus)
    {
        return std::unexpected(std::move(sceneStatus.error()));
    }

    if (!headless)
    {
        auto const imguiStatus = InitializeImGui();
        if (!imguiStatus)
        {
            return std::unexpected(std::move(imguiStatus.error()));
        }
    }

    return {};
}

lgp::framework::Status ChapterRenderer::Impl::OnResize(lgp::framework::DeviceResources &,
                                                       lgp::framework::Extent2D drawableSize)
{
    latestViewport = drawableSize;
    return RebuildScene();
}

lgp::framework::Status ChapterRenderer::Impl::Update(lgp::framework::UpdateContext const &context)
{
    latestElapsedSeconds = context.elapsedSeconds;
    latestViewport = context.drawableSize;

    if (controls.animateSlerp)
    {
        controls.slerpT = 0.5F + 0.5F * std::sin(static_cast<float>(context.elapsedSeconds) *
                                                 controls.slerpCyclesPerSecond * 2.0F * kPi);
    }

    return RebuildScene();
}

void ChapterRenderer::Impl::BeginImGuiFrame(lgp::framework::UpdateContext const &context)
{
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {
        static_cast<float>(context.drawableSize.width),
        static_cast<float>(context.drawableSize.height),
    };
    io.DeltaTime = static_cast<float>(std::max(context.deltaSeconds, 1.0 / 240.0));

    io.AddMousePosEvent(static_cast<float>(context.input.mouse.x), static_cast<float>(context.input.mouse.y));
    io.AddMouseButtonEvent(0, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Left));
    io.AddMouseButtonEvent(1, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Right));
    io.AddMouseButtonEvent(2, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Middle));
    io.AddMouseWheelEvent(0.0F, context.input.mouse.wheelDelta);
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    imguiFrameBegun = true;
}

void ChapterRenderer::Impl::DrawMatrix(char const *label, XMFLOAT4X4 const &matrix)
{
    if (ImGui::TreeNode(label))
    {
        for (int row = 0; row < 4; ++row)
        {
            ImGui::Text("% .3f  % .3f  % .3f  % .3f", matrix.m[row][0], matrix.m[row][1], matrix.m[row][2],
                        matrix.m[row][3]);
        }
        ImGui::TreePop();
    }
}

void ChapterRenderer::Impl::DrawFloat4(char const *label, XMFLOAT4 const &value)
{
    ImGui::Text("%s: (% .3f, % .3f, % .3f, % .3f)", label, value.x, value.y, value.z, value.w);
}

void ChapterRenderer::Impl::DrawFloat3(char const *label, XMFLOAT3 const &value)
{
    ImGui::Text("%s: (% .3f, % .3f, % .3f)", label, value.x, value.y, value.z);
}

void ChapterRenderer::Impl::DrawFloat2(char const *label, XMFLOAT2 const &value)
{
    ImGui::Text("%s: (% .3f, % .3f)", label, value.x, value.y);
}

void ChapterRenderer::Impl::DrawUiControls(bool &changed)
{
    ImGui::Begin(logic->IsStarterVariant() ? "Starter Diagnostics" : "Solution Diagnostics");

    ImGui::Text("Variant: %s", logic->IsStarterVariant() ? "Starter" : "Solution");
    ImGui::TextWrapped("%s", scene.implementationNote.c_str());
    ImGui::SeparatorText("Object Transform");

    if (!scene.capabilities.transformComposition)
    {
        ImGui::TextDisabled("TODO: compose translation, rotation, and scale in Starter/MathTransforms.cpp.");
    }

    ImGui::BeginDisabled(!scene.capabilities.transformComposition);
    changed |= ImGui::SliderFloat3("Translation (m)", &controls.translation.x, -3.0F, 3.0F);
    changed |= ImGui::SliderFloat3("Scale", &controls.scale.x, 0.25F, 3.0F);
    changed |= ImGui::SliderFloat3("Euler angles (deg)", &controls.eulerDegrees.x, -180.0F, 180.0F);
    changed |= ImGui::Checkbox("Show wrong-order overlay", &controls.showWrongOrder);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Camera");
    if (!scene.capabilities.cameraBasis)
    {
        ImGui::TextDisabled("TODO: rebuild the camera basis from yaw, pitch, and target orbit controls.");
    }

    ImGui::BeginDisabled(!scene.capabilities.cameraBasis);
    changed |= ImGui::SliderFloat("Camera yaw (deg)", &controls.cameraYawDegrees, -180.0F, 180.0F);
    changed |= ImGui::SliderFloat("Camera pitch (deg)", &controls.cameraPitchDegrees, -89.0F, 89.0F);
    changed |= ImGui::SliderFloat("Camera distance", &controls.cameraDistance, 1.5F, 12.0F);
    changed |= ImGui::SliderFloat3("Camera target", &controls.cameraTarget.x, -2.0F, 2.0F);
    changed |= ImGui::Checkbox("Show camera basis", &controls.showCameraAxes);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Projection");
    if (!scene.capabilities.projectionModes)
    {
        ImGui::TextDisabled("TODO: add orthographic and perspective projection selection.");
    }

    int projectionMode = static_cast<int>(controls.projectionMode);
    ImGui::BeginDisabled(!scene.capabilities.projectionModes);
    changed |= ImGui::Combo("Projection mode", &projectionMode, "Perspective\0Orthographic\0");
    controls.projectionMode = static_cast<ProjectionMode>(projectionMode);
    changed |= ImGui::SliderFloat("Vertical FOV (deg)", &controls.verticalFieldOfViewDegrees, 20.0F, 120.0F);
    changed |= ImGui::SliderFloat("Orthographic height", &controls.orthographicHeight, 1.0F, 12.0F);
    changed |= ImGui::SliderFloat("Near plane", &controls.nearPlane, 0.05F, 5.0F, "%.2f");
    changed |= ImGui::SliderFloat("Far plane", &controls.farPlane, 2.0F, 40.0F, "%.1f");
    changed |= ImGui::Checkbox("Show frustum overlay", &controls.showFrustum);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Orientation");
    if (!scene.capabilities.quaternionOrientation)
    {
        ImGui::TextDisabled("TODO: compare Euler and quaternion orientation in the learner-owned files.");
    }

    int orientationMode = static_cast<int>(controls.orientationMode);
    ImGui::BeginDisabled(!scene.capabilities.quaternionOrientation);
    changed |= ImGui::Combo("Orientation mode", &orientationMode, "Euler\0Quaternion\0Slerp\0");
    controls.orientationMode = static_cast<OrientationMode>(orientationMode);
    changed |= ImGui::SliderFloat3("Quaternion source (deg)", &controls.quaternionDegrees.x, -180.0F, 180.0F);
    changed |= ImGui::Checkbox("Show comparison orientation", &controls.showComparisonOrientation);
    ImGui::EndDisabled();

    if (scene.capabilities.quaternionSlerp)
    {
        changed |= ImGui::SliderFloat3("Slerp start (deg)", &controls.slerpStartDegrees.x, -180.0F, 180.0F);
        changed |= ImGui::SliderFloat3("Slerp end (deg)", &controls.slerpEndDegrees.x, -180.0F, 180.0F);
        changed |= ImGui::SliderFloat("Slerp t", &controls.slerpT, 0.0F, 1.0F);
        changed |= ImGui::Checkbox("Animate slerp", &controls.animateSlerp);
        changed |= ImGui::SliderFloat("Slerp cycles/sec", &controls.slerpCyclesPerSecond, 0.02F, 0.75F, "%.2f");
    }
    else
    {
        ImGui::TextDisabled("TODO: animate quaternion slerp after wiring the Solution math.");
    }

    ImGui::SeparatorText("Diagnostics");
    changed |=
        ImGui::SliderInt("Selected vertex", &controls.selectedVertex, 0, static_cast<int>(kObjectVertexCount - 1U));
    controls.selectedVertex = ClampVertexIndex(controls.selectedVertex);

    int inspectionSpace = static_cast<int>(controls.inspectionSpace);
    changed |= ImGui::Combo("Inspection space", &inspectionSpace, "Object\0World\0View\0Clip\0NDC\0Screen\0");
    controls.inspectionSpace = static_cast<InspectionSpace>(inspectionSpace);

    changed |= ImGui::Checkbox("Show object axes", &controls.showObjectAxes);
    changed |= ImGui::Checkbox("Show world axes", &controls.showWorldAxes);

    ImGui::BeginDisabled(!scene.capabilities.normalMatrix);
    changed |= ImGui::Checkbox("Show normal vectors", &controls.showNormalVectors);
    changed |= ImGui::Checkbox("Show incorrect normal", &controls.demonstrateIncorrectNormal);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!scene.capabilities.homogeneousDivide);
    changed |= ImGui::Checkbox("Omit perspective divide preview", &controls.omitPerspectiveDivide);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Selected Vertex Values");
    DrawFloat4("Object", scene.inspection.object);
    DrawFloat4("World", scene.inspection.world);
    DrawFloat4("View", scene.inspection.view);
    DrawFloat4("Clip", scene.inspection.clip);
    DrawFloat3("NDC", scene.inspection.ndc);
    DrawFloat2("Screen", scene.inspection.screen);
    if (controls.omitPerspectiveDivide)
    {
        DrawFloat2("Screen without divide", scene.inspection.screenWithoutDivide);
    }

    DrawPassPreview("Screen-space preview", scene.inspection.screen, scene.inspection.screenWithoutDivide,
                    controls.omitPerspectiveDivide && scene.capabilities.homogeneousDivide &&
                        scene.capabilities.viewportMapping,
                    latestViewport);

    ImGui::SeparatorText("Camera Basis");
    DrawFloat3("Position", scene.cameraPosition);
    DrawFloat3("Right", scene.cameraRight);
    DrawFloat3("Up", scene.cameraUp);
    DrawFloat3("Forward", scene.cameraForward);

    ImGui::SeparatorText("Matrices");
    DrawMatrix("Model", scene.model);
    DrawMatrix("View", scene.view);
    DrawMatrix("Projection", scene.projection);
    DrawMatrix("ViewProjection", scene.viewProjection);
    if (scene.hasWrongOrderModel)
    {
        DrawMatrix("Wrong-order model", scene.wrongOrderModel);
    }
    if (scene.hasComparisonModel)
    {
        DrawMatrix("Comparison model", scene.comparisonModel);
    }
    if (scene.capabilities.normalMatrix)
    {
        DrawMatrix("Normal matrix", scene.normalMatrix);
    }

    ImGui::End();
}

lgp::framework::Status ChapterRenderer::Impl::BuildUi(lgp::framework::UpdateContext const &context)
{
    if (headless || !imguiInitialized || !sceneValid)
    {
        return {};
    }

    BeginImGuiFrame(context);

    bool controlsChanged = false;
    DrawUiControls(controlsChanged);

    if (controlsChanged)
    {
        auto const rebuildStatus = RebuildScene();
        if (!rebuildStatus)
        {
            return std::unexpected(std::move(rebuildStatus.error()));
        }
    }

    return {};
}

lgp::framework::Result<D3D12_GPU_VIRTUAL_ADDRESS> ChapterRenderer::Impl::WriteDrawConstants(
    lgp::framework::FrameContext const &frameContext, UINT drawIndex, LinePassConstants const &constants)
{
    if (drawIndex >= kMaxDrawsPerFrame)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ChapterRenderer::WriteDrawConstants", "The chapter draw count exceeded the constant-buffer budget."));
    }

    std::uint64_t const offset = static_cast<std::uint64_t>(frameContext.frameSlot) * constantBufferBytesPerFrame +
                                 static_cast<std::uint64_t>(drawIndex) * constantBufferStride;

    auto const writeStatus =
        lgp::framework::WriteBuffer(constantBuffer, std::as_bytes(SpanOfSingle(constants)), offset);
    if (!writeStatus)
    {
        return std::unexpected(std::move(writeStatus.error()));
    }

    return constantBuffer.gpu_virtual_address() + offset;
}

lgp::framework::Status ChapterRenderer::Impl::DrawMesh(lgp::framework::FrameContext const &frameContext,
                                                       StaticMeshBuffers const &mesh, DirectX::XMMATRIX modelMatrix,
                                                       XMFLOAT4 colorTint, UINT &drawIndex)
{
    LinePassConstants constants{};
    constants.model = StoreMatrixValue(modelMatrix);
    constants.viewProjection = scene.viewProjection;
    constants.colorTint = colorTint;

    auto constantBufferAddress = WriteDrawConstants(frameContext, drawIndex, constants);
    if (!constantBufferAddress)
    {
        return std::unexpected(std::move(constantBufferAddress.error()));
    }

    ID3D12GraphicsCommandList *const commandList = frameContext.commandList;
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    commandList->IASetVertexBuffers(0U, 1U, &mesh.vertexBufferView);
    commandList->IASetIndexBuffer(&mesh.indexBufferView);
    commandList->SetGraphicsRootConstantBufferView(0U, constantBufferAddress.value());
    commandList->DrawIndexedInstanced(mesh.indexCount, 1U, 0U, 0, 0U);

    ++drawIndex;
    return {};
}

lgp::framework::Status ChapterRenderer::Impl::Render(lgp::framework::FrameContext const &frameContext)
{
    if (headless || frameContext.renderTarget == nullptr)
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

    float const clearColor[] = {0.08F, 0.10F, 0.13F, 1.0F};
    commandList->OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList->ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    commandList->RSSetViewports(1U, &frameContext.viewport);
    commandList->RSSetScissorRects(1U, &frameContext.scissorRect);

    ID3D12DescriptorHeap *const descriptorHeaps[] = {frameContext.shaderVisibleCbvSrvUavHeap};
    commandList->SetDescriptorHeaps(1U, descriptorHeaps);
    commandList->SetGraphicsRootSignature(rootSignature.Get());
    commandList->SetPipelineState(pipelineState.Get());

    UINT drawIndex = 0U;

    if (controls.showWorldAxes)
    {
        auto const status =
            DrawMesh(frameContext, axisMesh, DirectX::XMMatrixScaling(kAxisScale, kAxisScale, kAxisScale),
                     MakeColor(1.0F, 1.0F, 1.0F), drawIndex);
        if (!status)
        {
            return std::unexpected(std::move(status.error()));
        }
    }

    auto const objectStatus =
        DrawMesh(frameContext, objectMesh, LoadMatrix(scene.model), MakeColor(0.9F, 0.95F, 1.0F), drawIndex);
    if (!objectStatus)
    {
        return std::unexpected(std::move(objectStatus.error()));
    }

    if (controls.showObjectAxes)
    {
        auto const status =
            DrawMesh(frameContext, axisMesh,
                     DirectX::XMMatrixScaling(kAxisScale, kAxisScale, kAxisScale) * LoadMatrix(scene.model),
                     MakeColor(1.0F, 1.0F, 1.0F), drawIndex);
        if (!status)
        {
            return std::unexpected(std::move(status.error()));
        }
    }

    if (scene.hasWrongOrderModel && controls.showWrongOrder)
    {
        auto const status = DrawMesh(frameContext, objectMesh, LoadMatrix(scene.wrongOrderModel),
                                     MakeColor(1.0F, 0.55F, 0.20F, 0.9F), drawIndex);
        if (!status)
        {
            return std::unexpected(std::move(status.error()));
        }
    }

    if (scene.hasComparisonModel && controls.showComparisonOrientation)
    {
        auto const status = DrawMesh(frameContext, objectMesh, LoadMatrix(scene.comparisonModel),
                                     MakeColor(0.35F, 1.0F, 0.7F, 0.9F), drawIndex);
        if (!status)
        {
            return std::unexpected(std::move(status.error()));
        }
    }

    if (controls.showCameraAxes)
    {
        auto const status = DrawMesh(frameContext, axisMesh, BuildCameraBasisTransform(scene, kCameraAxisScale),
                                     MakeColor(1.0F, 1.0F, 1.0F), drawIndex);
        if (!status)
        {
            return std::unexpected(std::move(status.error()));
        }
    }

    if (controls.showFrustum && scene.capabilities.frustumOverlay)
    {
        auto const status = DrawMesh(frameContext, frustumMesh, LoadMatrix(scene.inverseViewProjection),
                                     MakeColor(1.0F, 0.95F, 0.30F), drawIndex);
        if (!status)
        {
            return std::unexpected(std::move(status.error()));
        }
    }

    auto const selectedPointStatus =
        DrawMesh(frameContext, crossMesh, BuildTranslationScaleTransform(scene.normalOriginWorld, kSelectedPointScale),
                 MakeColor(1.0F, 1.0F, 1.0F), drawIndex);
    if (!selectedPointStatus)
    {
        return std::unexpected(std::move(selectedPointStatus.error()));
    }

    if (controls.showNormalVectors && scene.capabilities.normalMatrix)
    {
        auto const correctNormalStatus =
            DrawMesh(frameContext, rayMesh, BuildRayTransform(scene.normalOriginWorld, scene.correctNormalWorld),
                     MakeColor(0.30F, 1.0F, 0.35F), drawIndex);
        if (!correctNormalStatus)
        {
            return std::unexpected(std::move(correctNormalStatus.error()));
        }

        if (controls.demonstrateIncorrectNormal)
        {
            auto const incorrectNormalStatus =
                DrawMesh(frameContext, rayMesh, BuildRayTransform(scene.normalOriginWorld, scene.incorrectNormalWorld),
                         MakeColor(1.0F, 0.30F, 0.75F), drawIndex);
            if (!incorrectNormalStatus)
            {
                return std::unexpected(std::move(incorrectNormalStatus.error()));
            }
        }
    }

    if (imguiFrameBegun)
    {
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
        imguiFrameBegun = false;
    }

    lgp::framework::TextureBarrierState constexpr presentState{
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        D3D12_BARRIER_LAYOUT_PRESENT,
    };
    lgp::framework::TransitionTexture(*commandList, *frameContext.renderTarget, renderTargetState, presentState);

    return {};
}

void ChapterRenderer::Impl::Shutdown() noexcept
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

    rayMesh = {};
    crossMesh = {};
    frustumMesh = {};
    axisMesh = {};
    objectMesh = {};
    constantBuffer = {};
    pipelineState.Reset();
    rootSignature.Reset();
    deviceResources = nullptr;
}

ChapterRenderer::ChapterRenderer(std::unique_ptr<IChapterLogic> logic) : impl_{std::make_unique<Impl>(std::move(logic))}
{
}

ChapterRenderer::~ChapterRenderer() = default;

ChapterRenderer::ChapterRenderer(ChapterRenderer &&) noexcept = default;

ChapterRenderer &ChapterRenderer::operator=(ChapterRenderer &&) noexcept = default;

lgp::framework::Status ChapterRenderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    return impl_->Initialize(context);
}

lgp::framework::Status ChapterRenderer::OnResize(lgp::framework::DeviceResources &deviceResources,
                                                 lgp::framework::Extent2D drawableSize)
{
    return impl_->OnResize(deviceResources, drawableSize);
}

lgp::framework::Status ChapterRenderer::Update(lgp::framework::UpdateContext const &context)
{
    return impl_->Update(context);
}

lgp::framework::Status ChapterRenderer::BuildUi(lgp::framework::UpdateContext const &context)
{
    return impl_->BuildUi(context);
}

lgp::framework::Status ChapterRenderer::Render(lgp::framework::FrameContext const &frameContext)
{
    return impl_->Render(frameContext);
}

void ChapterRenderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    impl_->Shutdown();
}

} // namespace ch01::graphics_math
