#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include "../Common/GpuLabSupport.hpp"

#include <ChapterGeometry.hpp>
#include <SurfaceFrame.hpp>

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lgp/framework/buffer.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

namespace ch12::gbuffer::starter
{
namespace
{

using ch06::surface_frames::GenerateGroundPlane;
using ch06::surface_frames::GenerateUvSphere;
using ch06::surface_frames::GeometryMesh;
using ch06::surface_frames::SurfaceVertex;
using Microsoft::WRL::ComPtr;

inline constexpr float kVerticalFov = 0.78F;
inline constexpr float kNearPlane = 0.1F;
inline constexpr float kFarPlane = 100.0F;

enum RootParameter : UINT
{
    FrameRootConstants = 0U,
    ObjectRootConstants = 1U,
};

struct FrameConstants final
{
    DirectX::XMFLOAT4X4 viewMatrix{};
    DirectX::XMFLOAT4X4 viewProjection{};
    DirectX::XMFLOAT4 projectionData{};
    DirectX::XMFLOAT4 lightDirectionIntensity{};
    DirectX::XMUINT4 options{};
};

struct ObjectConstants final
{
    DirectX::XMFLOAT3 translation{};
    float roughness{};
    DirectX::XMFLOAT3 baseColor{};
    float metalness{};
    DirectX::XMUINT4 metadata{};
};

struct MeshBuffers final
{
    lgp::framework::Buffer vertices{};
    lgp::framework::Buffer indices{};
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    UINT indexCount{};
};

struct CpuMesh final
{
    std::vector<SurfaceVertex> vertices{};
    std::vector<std::uint32_t> indices{};
};

struct SceneObject final
{
    MeshBuffers const *mesh{};
    ObjectConstants constants{};
};

struct SceneState final
{
    gpu::SceneMode scene{gpu::SceneMode::Full};
    DepthConvention depthConvention{DepthConvention::Forward};
    gpu::DebugView debugView{gpu::DebugView::Final};
    DirectX::XMFLOAT3 eye{};
    DirectX::XMFLOAT3 target{};
    DirectX::XMFLOAT3 directionToLightWorld{-0.45F, 0.82F, -0.35F};
    float lightIntensity{40.0F};
};

struct FrameSlotResources final
{
    ComPtr<ID3D12Resource> depth{};
    lgp::framework::DescriptorAllocation dsv{};
};

static_assert((sizeof(FrameConstants) % sizeof(std::uint32_t)) == 0U);
static_assert((sizeof(ObjectConstants) % sizeof(std::uint32_t)) == 0U);

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "GBufferDeferredLab.hlsl";
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] DirectX::XMFLOAT3 Normalize(DirectX::XMFLOAT3 direction) noexcept
{
    float const lengthSquared = (direction.x * direction.x) + (direction.y * direction.y) + (direction.z * direction.z);
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-8F)
    {
        return {0.0F, 0.0F, -1.0F};
    }
    DirectX::XMVECTOR const value = DirectX::XMLoadFloat3(&direction);
    DirectX::XMFLOAT3 result{};
    DirectX::XMStoreFloat3(&result, DirectX::XMVector3Normalize(value));
    return result;
}

[[nodiscard]] CpuMesh MakeFrontQuad(float width, float height, float z)
{
    float const halfWidth = width * 0.5F;
    float const halfHeight = height * 0.5F;
    CpuMesh mesh{};
    mesh.vertices = {
        {{-halfWidth, halfHeight, z}, {0.0F, 0.0F, -1.0F}, {0.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{halfWidth, halfHeight, z}, {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{-halfWidth, -halfHeight, z}, {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
        {{halfWidth, -halfHeight, z}, {0.0F, 0.0F, -1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}},
    };
    mesh.indices = {0U, 1U, 2U, 1U, 3U, 2U};
    return mesh;
}

[[nodiscard]] SceneState MakeSceneState(bool headless, std::optional<HeadlessTestConfiguration> const &configuration)
{
    SceneState state{};
    if (!headless || !configuration.has_value())
    {
        state.eye = {6.0F, 4.4F, -8.2F};
        state.target = {0.0F, 1.0F, 0.6F};
        return state;
    }

    state.scene = configuration->scene;
    state.depthConvention = configuration->depthConvention;
    state.debugView = configuration->debugView;
    switch (configuration->scene)
    {
    case gpu::SceneMode::Full:
        state.eye = {6.0F, 4.4F, -8.2F};
        state.target = {0.0F, 1.0F, 0.6F};
        state.directionToLightWorld = {-0.45F, 0.82F, -0.35F};
        state.lightIntensity = 40.0F;
        break;
    case gpu::SceneMode::FrontQuad:
    case gpu::SceneMode::SharedEdgeQuad:
    case gpu::SceneMode::ClearOnly:
        state.eye = {0.0F, 0.0F, 0.0F};
        state.target = {0.0F, 0.0F, 1.0F};
        state.directionToLightWorld = {0.0F, 0.0F, -1.0F};
        state.lightIntensity = 18.0F;
        break;
    }
    return state;
}

[[nodiscard]] std::expected<lgp::framework::CompiledShader, lgp::framework::Error> CompileShader(
    lgp::framework::ShaderCompiler &compiler, lgp::framework::ShaderCompileOptions &options, wchar_t const *entryPoint,
    wchar_t const *profile)
{
    options.entryPoint = entryPoint;
    options.targetProfile = profile;
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    return compiler.Compile(options);
}

[[nodiscard]] DirectX::XMMATRIX MakeProjectionMatrix(float verticalFieldOfViewRadians, float aspectRatio,
                                                     DeviceDepthCoefficients coefficients) noexcept
{
    float const tanHalfFov = std::tan(verticalFieldOfViewRadians * 0.5F);
    float const xScale = 1.0F / (tanHalfFov * aspectRatio);
    float const yScale = 1.0F / tanHalfFov;
    return DirectX::XMMATRIX(xScale, 0.0F, 0.0F, 0.0F, 0.0F, yScale, 0.0F, 0.0F, 0.0F, 0.0F, coefficients.additive,
                             1.0F, 0.0F, 0.0F, coefficients.reciprocal, 0.0F);
}

[[nodiscard]] std::uint32_t DepthModeFlag(DepthConvention convention) noexcept
{
    return convention == DepthConvention::Forward ? 0U : 1U;
}

[[nodiscard]] D3D12_COMPARISON_FUNC DepthFunc(DepthConvention convention) noexcept
{
    return convention == DepthConvention::Forward ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                                  : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
}

} // namespace

class Renderer::Impl final
{
  public:
    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    ComPtr<ID3D12RootSignature> rootSignature_{};
    ComPtr<ID3D12PipelineState> forwardPipeline_{};
    ComPtr<ID3D12PipelineState> reversedPipeline_{};
    MeshBuffers sphere_{};
    MeshBuffers ground_{};
    MeshBuffers quad_{};
    lgp::framework::DescriptorHeap dsvHeap_{};
    std::vector<FrameSlotResources> frameSlots_{};
    SceneState sceneState_{};
    std::optional<HeadlessTestConfiguration> headlessConfiguration_{};
    bool headless_{false};

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context);
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateMeshBuffers(CpuMesh const &mesh, std::wstring_view name,
                                                           MeshBuffers &buffers);
    [[nodiscard]] lgp::framework::Status CreateMeshBuffers(GeometryMesh const &mesh, std::wstring_view name,
                                                           MeshBuffers &buffers);
    [[nodiscard]] lgp::framework::Status CreateSizeDependentResources(lgp::framework::Extent2D drawableSize);
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context);
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext);
    void ReleaseSizeDependentResources() noexcept;
    void Shutdown() noexcept;
};

lgp::framework::Status Renderer::Impl::CreateShaders()
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

    auto vertex = CompileShader(compiler, options, L"ForwardVS", L"vs_6_0");
    if (!vertex)
    {
        return std::unexpected(std::move(vertex.error()));
    }
    vertexShader_ = std::move(*vertex);

    auto pixel = CompileShader(compiler, options, L"ForwardPS", L"ps_6_0");
    if (!pixel)
    {
        return std::unexpected(std::move(pixel.error()));
    }
    pixelShader_ = std::move(*pixel);
    return {};
}

lgp::framework::Status Renderer::Impl::CreateRootSignature()
{
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[FrameRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[FrameRootConstants].Constants.ShaderRegister = 0U;
    parameters[FrameRootConstants].Constants.Num32BitValues = sizeof(FrameConstants) / sizeof(std::uint32_t);
    parameters[FrameRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ObjectRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[ObjectRootConstants].Constants.ShaderRegister = 1U;
    parameters[ObjectRootConstants].Constants.Num32BitValues = sizeof(ObjectConstants) / sizeof(std::uint32_t);
    parameters[ObjectRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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
                                             "Failed to create the Chapter 12 Starter root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::Impl::CreatePipelines()
{
    D3D12_INPUT_ELEMENT_DESC const inputLayout[]{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 24U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TANGENT", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 32U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
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

    description.DepthStencilState.DepthFunc = DepthFunc(DepthConvention::Forward);
    HRESULT const forwardResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(forwardPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(forwardResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", forwardResult,
                                             "Failed to create the Chapter 12 Starter forward-depth pipeline."));
    }

    description.DepthStencilState.DepthFunc = DepthFunc(DepthConvention::Reversed);
    HRESULT const reversedResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(reversedPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(reversedResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", reversedResult,
                                             "Failed to create the Chapter 12 Starter reversed-depth pipeline."));
    }

    return {};
}

lgp::framework::Status Renderer::Impl::CreateMeshBuffers(CpuMesh const &mesh, std::wstring_view const name,
                                                         MeshBuffers &buffers)
{
    auto vertexResult = lgp::framework::CreateUploadBuffer(
        *deviceResources_->device(), mesh.vertices.size() * sizeof(SurfaceVertex), std::wstring{name} + L" vertices");
    if (!vertexResult)
    {
        return std::unexpected(std::move(vertexResult.error()));
    }
    buffers.vertices = std::move(*vertexResult);
    if (auto status = lgp::framework::WriteBuffer(
            buffers.vertices, std::span<SurfaceVertex const>{mesh.vertices.data(), mesh.vertices.size()});
        !status)
    {
        return status;
    }

    auto indexResult = lgp::framework::CreateUploadBuffer(
        *deviceResources_->device(), mesh.indices.size() * sizeof(std::uint32_t), std::wstring{name} + L" indices");
    if (!indexResult)
    {
        return std::unexpected(std::move(indexResult.error()));
    }
    buffers.indices = std::move(*indexResult);
    if (auto status = lgp::framework::WriteBuffer(
            buffers.indices, std::span<std::uint32_t const>{mesh.indices.data(), mesh.indices.size()});
        !status)
    {
        return status;
    }

    buffers.vertexView.BufferLocation = buffers.vertices.gpu_virtual_address();
    buffers.vertexView.SizeInBytes = static_cast<UINT>(mesh.vertices.size() * sizeof(SurfaceVertex));
    buffers.vertexView.StrideInBytes = sizeof(SurfaceVertex);
    buffers.indexView.BufferLocation = buffers.indices.gpu_virtual_address();
    buffers.indexView.SizeInBytes = static_cast<UINT>(mesh.indices.size() * sizeof(std::uint32_t));
    buffers.indexView.Format = DXGI_FORMAT_R32_UINT;
    buffers.indexCount = static_cast<UINT>(mesh.indices.size());
    return {};
}

lgp::framework::Status Renderer::Impl::CreateMeshBuffers(GeometryMesh const &mesh, std::wstring_view const name,
                                                         MeshBuffers &buffers)
{
    CpuMesh converted{};
    converted.vertices = mesh.vertices;
    converted.indices = mesh.indices;
    return CreateMeshBuffers(converted, name, buffers);
}

void Renderer::Impl::ReleaseSizeDependentResources() noexcept
{
    for (FrameSlotResources &slot : frameSlots_)
    {
        slot.depth.Reset();
        if (slot.dsv)
        {
            dsvHeap_.Free(slot.dsv);
        }
        slot.dsv = {};
    }
    frameSlots_.clear();
}

lgp::framework::Status Renderer::Impl::CreateSizeDependentResources(lgp::framework::Extent2D const drawableSize)
{
    ReleaseSizeDependentResources();
    if (drawableSize.empty())
    {
        return {};
    }

    frameSlots_.resize(deviceResources_->back_buffer_count());
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (UINT frameSlot = 0U; frameSlot < deviceResources_->back_buffer_count(); ++frameSlot)
    {
        auto dsvResult = dsvHeap_.Allocate(1U);
        if (!dsvResult)
        {
            return std::unexpected(std::move(dsvResult.error()));
        }
        frameSlots_[frameSlot].dsv = *dsvResult;

        D3D12_RESOURCE_DESC1 const description =
            gpu::MakeTextureDescription(drawableSize, gpu::kDepthDsvFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        HRESULT const createResult = deviceResources_->device()->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0U, nullptr,
            IID_PPV_ARGS(frameSlots_[frameSlot].depth.ReleaseAndGetAddressOf()));
        if (FAILED(createResult))
        {
            return std::unexpected(
                lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", createResult,
                                                 "Failed to create a Chapter 12 Starter depth resource."));
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = gpu::kDepthDsvFormat;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateDepthStencilView(frameSlots_[frameSlot].depth.Get(), &dsv,
                                                           frameSlots_[frameSlot].dsv.cpuHandle);
    }

    return {};
}

lgp::framework::Status Renderer::Impl::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;

    auto sphereResult = GenerateUvSphere(0.95F, 24U, 48U);
    auto groundResult = GenerateGroundPlane(18.0F, 14.0F);
    if (!sphereResult || !groundResult)
    {
        return std::unexpected(
            lgp::framework::MakeError("GenerateGeometry", "Failed to build the Chapter 12 Starter geometry."));
    }

    auto dsvHeapResult =
        lgp::framework::CreateDescriptorHeap(*deviceResources_->device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                             deviceResources_->back_buffer_count(), false, L"Ch12 Starter DSV heap");
    if (!dsvHeapResult)
    {
        return std::unexpected(std::move(dsvHeapResult.error()));
    }
    dsvHeap_ = std::move(*dsvHeapResult);

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
    if (auto status = CreateMeshBuffers(*sphereResult, L"Ch12 Starter Sphere", sphere_); !status)
    {
        return status;
    }
    if (auto status = CreateMeshBuffers(*groundResult, L"Ch12 Starter Ground", ground_); !status)
    {
        return status;
    }
    if (auto status = CreateMeshBuffers(MakeFrontQuad(12.0F, 12.0F, 6.0F), L"Ch12 Starter Front Quad", quad_); !status)
    {
        return status;
    }
    return CreateSizeDependentResources(context.drawableSize);
}

lgp::framework::Status Renderer::Impl::Update(lgp::framework::UpdateContext const &context)
{
    sceneState_ = MakeSceneState(headless_, headlessConfiguration_);
    sceneState_.directionToLightWorld = Normalize(sceneState_.directionToLightWorld);
    if (!headless_ && !headlessConfiguration_.has_value())
    {
        sceneState_.depthConvention = DepthConvention::Forward;
        sceneState_.debugView = gpu::DebugView::Final;
    }
    if (context.drawableSize.empty())
    {
        return {};
    }
    return {};
}

lgp::framework::Status Renderer::Impl::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr ||
        frameContext.frameSlot >= frameSlots_.size())
    {
        return {};
    }

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.SetGraphicsRootSignature(rootSignature_.Get());

    float const aspectRatio = frameContext.drawableSize.height == 0U
                                  ? 1.0F
                                  : static_cast<float>(frameContext.drawableSize.width) /
                                        static_cast<float>(frameContext.drawableSize.height);
    PerspectiveProjection const projection{
        .verticalFieldOfViewRadians = kVerticalFov,
        .aspectRatio = aspectRatio,
        .nearPlane = kNearPlane,
        .farPlane = kFarPlane,
        .depthConvention = sceneState_.depthConvention,
    };
    auto const coefficients = MakeDeviceDepthCoefficients(projection);
    if (!coefficients)
    {
        return std::unexpected(lgp::framework::MakeError("MakeDeviceDepthCoefficients",
                                                         "Invalid Chapter 12 Starter projection constants."));
    }

    DirectX::XMVECTOR const eye = DirectX::XMLoadFloat3(&sceneState_.eye);
    DirectX::XMVECTOR const target = DirectX::XMLoadFloat3(&sceneState_.target);
    DirectX::XMMATRIX const view = DirectX::XMMatrixLookAtLH(eye, target, DirectX::XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F));
    DirectX::XMMATRIX const projectionMatrix = MakeProjectionMatrix(kVerticalFov, aspectRatio, *coefficients);
    DirectX::XMMATRIX const viewProjection = view * projectionMatrix;

    DirectX::XMFLOAT3 directionToLightView{};
    DirectX::XMStoreFloat3(&directionToLightView,
                           DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(
                               DirectX::XMLoadFloat3(&sceneState_.directionToLightWorld), view)));

    FrameConstants frameConstants{};
    DirectX::XMStoreFloat4x4(&frameConstants.viewMatrix, view);
    DirectX::XMStoreFloat4x4(&frameConstants.viewProjection, viewProjection);
    frameConstants.projectionData = {
        std::tan(kVerticalFov * 0.5F) * aspectRatio,
        std::tan(kVerticalFov * 0.5F),
        coefficients->additive,
        coefficients->reciprocal,
    };
    frameConstants.lightDirectionIntensity = {
        directionToLightView.x,
        directionToLightView.y,
        directionToLightView.z,
        sceneState_.lightIntensity,
    };
    frameConstants.options = {
        static_cast<std::uint32_t>(sceneState_.debugView),
        DepthModeFlag(sceneState_.depthConvention),
        frameContext.drawableSize.width,
        frameContext.drawableSize.height,
    };

    std::vector<SceneObject> objects{};
    switch (sceneState_.scene)
    {
    case gpu::SceneMode::Full:
        objects.push_back({&ground_, {{0.0F, 0.0F, 0.0F}, 0.78F, {0.28F, 0.30F, 0.34F}, 0.02F, {1U, 0U, 0U, 0U}}});
        objects.push_back({&sphere_, {{-1.8F, 1.0F, 0.2F}, 0.18F, {0.82F, 0.22F, 0.10F}, 0.05F, {2U, 0U, 0U, 0U}}});
        objects.push_back({&sphere_, {{1.7F, 1.15F, 1.3F}, 0.58F, {0.84F, 0.80F, 0.76F}, 1.0F, {3U, 0U, 0U, 0U}}});
        break;
    case gpu::SceneMode::FrontQuad:
        objects.push_back({&quad_, {{0.0F, 0.0F, 0.0F}, 0.36F, {0.20F, 0.62F, 0.88F}, 0.35F, {17U, 0U, 0U, 0U}}});
        break;
    case gpu::SceneMode::SharedEdgeQuad:
        objects.push_back({&quad_, {{0.0F, 0.0F, -2.0F}, 0.22F, {0.78F, 0.44F, 0.12F}, 0.08F, {9U, 0U, 0U, 0U}}});
        break;
    case gpu::SceneMode::ClearOnly:
        break;
    }

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    std::vector<D3D12_TEXTURE_BARRIER> barriers{
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::FrameStartState(frameContext),
                                gpu::RenderTargetState()),
        gpu::MakeTextureBarrier(*slot.depth.Get(), gpu::CommonState(), gpu::DepthWriteState(),
                                D3D12_TEXTURE_BARRIER_FLAG_DISCARD),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);

    float const clearColor[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, &slot.dsv.cpuHandle);
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    commandList.ClearDepthStencilView(slot.dsv.cpuHandle, D3D12_CLEAR_FLAG_DEPTH,
                                      DepthClearValue(sceneState_.depthConvention), 0U, 0U, nullptr);

    commandList.SetPipelineState(sceneState_.depthConvention == DepthConvention::Forward ? forwardPipeline_.Get()
                                                                                         : reversedPipeline_.Get());
    commandList.SetGraphicsRoot32BitConstants(FrameRootConstants, sizeof(FrameConstants) / sizeof(std::uint32_t),
                                              &frameConstants, 0U);

    for (SceneObject const &object : objects)
    {
        commandList.SetGraphicsRoot32BitConstants(ObjectRootConstants, sizeof(ObjectConstants) / sizeof(std::uint32_t),
                                                  &object.constants, 0U);
        commandList.IASetVertexBuffers(0U, 1U, &object.mesh->vertexView);
        commandList.IASetIndexBuffer(&object.mesh->indexView);
        commandList.DrawIndexedInstanced(object.mesh->indexCount, 1U, 0U, 0, 0U);
    }

    barriers = {
        gpu::MakeTextureBarrier(*slot.depth.Get(), gpu::DepthWriteState(), gpu::CommonState()),
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(), gpu::FrameEndState(frameContext)),
    };
    gpu::SubmitTextureBarriers(commandList, barriers);
    return {};
}

void Renderer::Impl::Shutdown() noexcept
{
    ReleaseSizeDependentResources();
    dsvHeap_ = {};
    quad_ = {};
    ground_ = {};
    sphere_ = {};
    reversedPipeline_.Reset();
    forwardPipeline_.Reset();
    rootSignature_.Reset();
    deviceResources_ = nullptr;
}

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer &&) noexcept = default;
Renderer &Renderer::operator=(Renderer &&) noexcept = default;

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    return impl_->Initialize(context);
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &,
                                          lgp::framework::Extent2D const drawableSize)
{
    return impl_->CreateSizeDependentResources(drawableSize);
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &context)
{
    return impl_->Update(context);
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    return impl_->Render(frameContext);
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    impl_->Shutdown();
}

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    impl_->headlessConfiguration_ = configuration;
}

} // namespace ch12::gbuffer::starter
