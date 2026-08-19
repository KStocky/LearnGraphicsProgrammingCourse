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

#include <imgui.h>
#include <imgui_impl_dx12.h>

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

namespace ch12::gbuffer::solution
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
inline constexpr UINT kSrvCount = 6U;
inline constexpr UINT kPerSlotShaderVisibleDescriptors = 7U;

enum RootParameter : UINT
{
    FrameRootConstants = 0U,
    ObjectRootConstants = 1U,
    GBufferSrvTable = 2U,
};

enum class AttachmentIndex : UINT
{
    BaseColorMetalness = 0U,
    OctahedralNormal,
    Roughness,
    DeviceDepth,
    Motion,
    Identity,
    Count,
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
    std::array<ComPtr<ID3D12Resource>, static_cast<std::size_t>(AttachmentIndex::Count)> textures{};
    lgp::framework::DescriptorAllocation rtvs{};
    lgp::framework::DescriptorAllocation dsv{};
    lgp::framework::DescriptorAllocation srvs{};
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

[[nodiscard]] SceneState MakeSceneState(bool headless, std::optional<HeadlessTestConfiguration> const &configuration,
                                        DepthConvention uiDepthConvention, gpu::DebugView uiDebugView)
{
    SceneState state{};
    state.depthConvention = uiDepthConvention;
    state.debugView = uiDebugView;
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

[[nodiscard]] DXGI_FORMAT AttachmentResourceFormat(AttachmentIndex index) noexcept
{
    switch (index)
    {
    case AttachmentIndex::BaseColorMetalness:
        return gpu::kBaseColorMetalnessResourceFormat;
    case AttachmentIndex::OctahedralNormal:
        return gpu::kOctahedralNormalFormat;
    case AttachmentIndex::Roughness:
        return gpu::kRoughnessFormat;
    case AttachmentIndex::DeviceDepth:
        return gpu::kDepthResourceFormat;
    case AttachmentIndex::Motion:
        return gpu::kMotionFormat;
    case AttachmentIndex::Identity:
        return gpu::kIdentityFormat;
    case AttachmentIndex::Count:
        break;
    }
    return DXGI_FORMAT_UNKNOWN;
}

[[nodiscard]] UINT ColorAttachmentCount() noexcept
{
    return 5U;
}

[[nodiscard]] UINT TextureIndex(AttachmentIndex attachment) noexcept
{
    return static_cast<UINT>(attachment);
}

[[nodiscard]] char const *DebugViewComboItems() noexcept
{
    return "Final shading\0Base color\0Metalness\0Decoded normal\0Roughness\0Device depth\0Motion\0Identity\0";
}

[[nodiscard]] char const *DepthModeItems() noexcept
{
    return "Forward Z\0Reversed Z\0";
}

} // namespace

class Renderer::Impl final
{
  public:
    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader geometryVertexShader_{};
    lgp::framework::CompiledShader geometryPixelShader_{};
    lgp::framework::CompiledShader fullscreenVertexShader_{};
    lgp::framework::CompiledShader deferredPixelShader_{};
    ComPtr<ID3D12RootSignature> rootSignature_{};
    ComPtr<ID3D12PipelineState> geometryForwardPipeline_{};
    ComPtr<ID3D12PipelineState> geometryReversedPipeline_{};
    ComPtr<ID3D12PipelineState> deferredPipeline_{};
    MeshBuffers sphere_{};
    MeshBuffers ground_{};
    MeshBuffers quad_{};
    lgp::framework::DescriptorHeap dsvHeap_{};
    lgp::framework::DescriptorAllocation imguiFontDescriptor_{};
    std::vector<FrameSlotResources> frameSlots_{};
    SceneState sceneState_{};
    std::optional<HeadlessTestConfiguration> headlessConfiguration_{};
    DepthConvention uiDepthConvention_{DepthConvention::Forward};
    gpu::DebugView uiDebugView_{gpu::DebugView::Final};
    UINT lastRenderedFrameSlot_{0U};
    bool headless_{false};
    bool imguiInitialized_{false};
    bool imguiFrameBegun_{false};

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context);
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateMeshBuffers(CpuMesh const &mesh, std::wstring_view name,
                                                           MeshBuffers &buffers);
    [[nodiscard]] lgp::framework::Status CreateMeshBuffers(GeometryMesh const &mesh, std::wstring_view name,
                                                           MeshBuffers &buffers);
    [[nodiscard]] lgp::framework::Status CreateSizeDependentResources(lgp::framework::Extent2D drawableSize);
    [[nodiscard]] lgp::framework::Status InitializeImGui();
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context);
    [[nodiscard]] lgp::framework::Status BuildUi(lgp::framework::UpdateContext const &context);
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext);
    [[nodiscard]] std::expected<ReadbackOutputs, lgp::framework::Error> ReadBackOutputs() const;
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

    auto geometryVs = CompileShader(compiler, options, L"GeometryVS", L"vs_6_0");
    if (!geometryVs)
    {
        return std::unexpected(std::move(geometryVs.error()));
    }
    geometryVertexShader_ = std::move(*geometryVs);

    auto geometryPs = CompileShader(compiler, options, L"GeometryPS", L"ps_6_0");
    if (!geometryPs)
    {
        return std::unexpected(std::move(geometryPs.error()));
    }
    geometryPixelShader_ = std::move(*geometryPs);

    auto fullscreenVs = CompileShader(compiler, options, L"FullscreenVS", L"vs_6_0");
    if (!fullscreenVs)
    {
        return std::unexpected(std::move(fullscreenVs.error()));
    }
    fullscreenVertexShader_ = std::move(*fullscreenVs);

    auto deferredPs = CompileShader(compiler, options, L"DeferredPS", L"ps_6_0");
    if (!deferredPs)
    {
        return std::unexpected(std::move(deferredPs.error()));
    }
    deferredPixelShader_ = std::move(*deferredPs);
    return {};
}

lgp::framework::Status Renderer::Impl::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kSrvCount;
    range.BaseShaderRegister = 0U;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[FrameRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[FrameRootConstants].Constants.ShaderRegister = 0U;
    parameters[FrameRootConstants].Constants.Num32BitValues = sizeof(FrameConstants) / sizeof(std::uint32_t);
    parameters[FrameRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ObjectRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[ObjectRootConstants].Constants.ShaderRegister = 1U;
    parameters[ObjectRootConstants].Constants.Num32BitValues = sizeof(ObjectConstants) / sizeof(std::uint32_t);
    parameters[ObjectRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[GBufferSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[GBufferSrvTable].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[GBufferSrvTable].DescriptorTable.pDescriptorRanges = &range;
    parameters[GBufferSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
                                             "Failed to create the Chapter 12 Solution root signature."));
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
    for (UINT index = 0U; index < ColorAttachmentCount(); ++index)
    {
        blend.RenderTarget[index].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryDescription{};
    geometryDescription.pRootSignature = rootSignature_.Get();
    geometryDescription.VS = geometryVertexShader_.Bytecode();
    geometryDescription.PS = geometryPixelShader_.Bytecode();
    geometryDescription.BlendState = blend;
    geometryDescription.SampleMask = UINT_MAX;
    geometryDescription.RasterizerState = rasterizer;
    geometryDescription.DepthStencilState = depth;
    geometryDescription.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    geometryDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    geometryDescription.NumRenderTargets = ColorAttachmentCount();
    geometryDescription.RTVFormats[0] = gpu::kBaseColorMetalnessViewFormat;
    geometryDescription.RTVFormats[1] = gpu::kOctahedralNormalFormat;
    geometryDescription.RTVFormats[2] = gpu::kRoughnessFormat;
    geometryDescription.RTVFormats[3] = gpu::kMotionFormat;
    geometryDescription.RTVFormats[4] = gpu::kIdentityFormat;
    geometryDescription.DSVFormat = gpu::kDepthDsvFormat;
    geometryDescription.SampleDesc.Count = 1U;

    geometryDescription.DepthStencilState.DepthFunc = DepthFunc(DepthConvention::Forward);
    HRESULT const forwardResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &geometryDescription, IID_PPV_ARGS(geometryForwardPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(forwardResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", forwardResult,
                                             "Failed to create the Chapter 12 Solution forward geometry pipeline."));
    }

    geometryDescription.DepthStencilState.DepthFunc = DepthFunc(DepthConvention::Reversed);
    HRESULT const reversedResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &geometryDescription, IID_PPV_ARGS(geometryReversedPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(reversedResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", reversedResult,
                                             "Failed to create the Chapter 12 Solution reversed geometry pipeline."));
    }

    D3D12_BLEND_DESC fullscreenBlend{};
    fullscreenBlend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC fullscreenDepth{};
    fullscreenDepth.DepthEnable = FALSE;
    fullscreenDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    fullscreenDepth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC deferredDescription{};
    deferredDescription.pRootSignature = rootSignature_.Get();
    deferredDescription.VS = fullscreenVertexShader_.Bytecode();
    deferredDescription.PS = deferredPixelShader_.Bytecode();
    deferredDescription.BlendState = fullscreenBlend;
    deferredDescription.SampleMask = UINT_MAX;
    deferredDescription.RasterizerState = rasterizer;
    deferredDescription.DepthStencilState = fullscreenDepth;
    deferredDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    deferredDescription.NumRenderTargets = 1U;
    deferredDescription.RTVFormats[0] = deviceResources_->back_buffer_format();
    deferredDescription.SampleDesc.Count = 1U;
    HRESULT const deferredResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &deferredDescription, IID_PPV_ARGS(deferredPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(deferredResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", deferredResult,
                                             "Failed to create the Chapter 12 Solution deferred pipeline."));
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
        for (auto &texture : slot.textures)
        {
            texture.Reset();
        }
        if (slot.rtvs)
        {
            deviceResources_->rtv_heap().Free(slot.rtvs);
        }
        if (slot.srvs)
        {
            deviceResources_->shader_visible_cbv_srv_uav_heap().Free(slot.srvs);
        }
        if (slot.dsv)
        {
            dsvHeap_.Free(slot.dsv);
        }
        slot.rtvs = {};
        slot.srvs = {};
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
        auto rtvResult = deviceResources_->rtv_heap().Allocate(ColorAttachmentCount());
        if (!rtvResult)
        {
            return std::unexpected(std::move(rtvResult.error()));
        }
        frameSlots_[frameSlot].rtvs = *rtvResult;

        auto dsvResult = dsvHeap_.Allocate(1U);
        if (!dsvResult)
        {
            return std::unexpected(std::move(dsvResult.error()));
        }
        frameSlots_[frameSlot].dsv = *dsvResult;

        auto srvResult = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kPerSlotShaderVisibleDescriptors);
        if (!srvResult)
        {
            return std::unexpected(std::move(srvResult.error()));
        }
        frameSlots_[frameSlot].srvs = *srvResult;

        for (UINT index = 0U; index < TextureIndex(AttachmentIndex::Count); ++index)
        {
            AttachmentIndex const attachment = static_cast<AttachmentIndex>(index);
            DXGI_FORMAT const resourceFormat = AttachmentResourceFormat(attachment);
            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (attachment == AttachmentIndex::DeviceDepth)
            {
                flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            }
            else if (attachment == AttachmentIndex::Identity)
            {
                flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
            D3D12_RESOURCE_DESC1 const description = gpu::MakeTextureDescription(drawableSize, resourceFormat, flags);

            HRESULT const createResult = deviceResources_->device()->CreateCommittedResource3(
                &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0U, nullptr,
                IID_PPV_ARGS(frameSlots_[frameSlot].textures[index].ReleaseAndGetAddressOf()));
            if (FAILED(createResult))
            {
                return std::unexpected(
                    lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", createResult,
                                                     "Failed to create a Chapter 12 Solution per-frame attachment."));
            }
        }

        D3D12_RENDER_TARGET_VIEW_DESC baseRtv{};
        baseRtv.Format = gpu::kBaseColorMetalnessViewFormat;
        baseRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateRenderTargetView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::BaseColorMetalness)].Get(), &baseRtv,
            frameSlots_[frameSlot].rtvs.CpuHandle(0U));

        D3D12_RENDER_TARGET_VIEW_DESC normalRtv{};
        normalRtv.Format = gpu::kOctahedralNormalFormat;
        normalRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateRenderTargetView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::OctahedralNormal)].Get(), &normalRtv,
            frameSlots_[frameSlot].rtvs.CpuHandle(1U));

        D3D12_RENDER_TARGET_VIEW_DESC roughnessRtv{};
        roughnessRtv.Format = gpu::kRoughnessFormat;
        roughnessRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateRenderTargetView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::Roughness)].Get(), &roughnessRtv,
            frameSlots_[frameSlot].rtvs.CpuHandle(2U));

        D3D12_RENDER_TARGET_VIEW_DESC motionRtv{};
        motionRtv.Format = gpu::kMotionFormat;
        motionRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateRenderTargetView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::Motion)].Get(), &motionRtv,
            frameSlots_[frameSlot].rtvs.CpuHandle(3U));

        D3D12_RENDER_TARGET_VIEW_DESC identityRtv{};
        identityRtv.Format = gpu::kIdentityFormat;
        identityRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateRenderTargetView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::Identity)].Get(), &identityRtv,
            frameSlots_[frameSlot].rtvs.CpuHandle(4U));

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = gpu::kDepthDsvFormat;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateDepthStencilView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::DeviceDepth)].Get(), &dsv,
            frameSlots_[frameSlot].dsv.cpuHandle);

        D3D12_SHADER_RESOURCE_VIEW_DESC baseSrv{};
        baseSrv.Format = gpu::kBaseColorMetalnessViewFormat;
        baseSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        baseSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        baseSrv.Texture2D.MipLevels = 1U;
        deviceResources_->device()->CreateShaderResourceView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::BaseColorMetalness)].Get(), &baseSrv,
            frameSlots_[frameSlot].srvs.CpuHandle(0U));

        D3D12_SHADER_RESOURCE_VIEW_DESC normalSrv = baseSrv;
        normalSrv.Format = gpu::kOctahedralNormalFormat;
        deviceResources_->device()->CreateShaderResourceView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::OctahedralNormal)].Get(), &normalSrv,
            frameSlots_[frameSlot].srvs.CpuHandle(1U));

        D3D12_SHADER_RESOURCE_VIEW_DESC roughnessSrv = baseSrv;
        roughnessSrv.Format = gpu::kRoughnessFormat;
        deviceResources_->device()->CreateShaderResourceView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::Roughness)].Get(), &roughnessSrv,
            frameSlots_[frameSlot].srvs.CpuHandle(2U));

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = baseSrv;
        depthSrv.Format = gpu::kDepthSrvFormat;
        deviceResources_->device()->CreateShaderResourceView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::DeviceDepth)].Get(), &depthSrv,
            frameSlots_[frameSlot].srvs.CpuHandle(3U));

        D3D12_SHADER_RESOURCE_VIEW_DESC motionSrv = baseSrv;
        motionSrv.Format = gpu::kMotionFormat;
        deviceResources_->device()->CreateShaderResourceView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::Motion)].Get(), &motionSrv,
            frameSlots_[frameSlot].srvs.CpuHandle(4U));

        D3D12_SHADER_RESOURCE_VIEW_DESC identitySrv = baseSrv;
        identitySrv.Format = gpu::kIdentityFormat;
        deviceResources_->device()->CreateShaderResourceView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::Identity)].Get(), &identitySrv,
            frameSlots_[frameSlot].srvs.CpuHandle(5U));

        D3D12_UNORDERED_ACCESS_VIEW_DESC identityUav{};
        identityUav.Format = gpu::kIdentityFormat;
        identityUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateUnorderedAccessView(
            frameSlots_[frameSlot].textures[TextureIndex(AttachmentIndex::Identity)].Get(), nullptr, &identityUav,
            frameSlots_[frameSlot].srvs.CpuHandle(6U));
    }
    return {};
}

lgp::framework::Status Renderer::Impl::InitializeImGui()
{
    auto descriptorResult = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(1U);
    if (!descriptorResult)
    {
        return std::unexpected(std::move(descriptorResult.error()));
    }
    imguiFontDescriptor_ = *descriptorResult;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().BackendPlatformName = "LGP.ManualInput";

    ImGui_ImplDX12_InitInfo info{};
    info.Device = deviceResources_->device();
    info.CommandQueue = deviceResources_->graphics_queue();
    info.NumFramesInFlight = static_cast<int>(deviceResources_->back_buffer_count());
    info.RTVFormat = deviceResources_->back_buffer_format();
    info.DSVFormat = gpu::kDepthDsvFormat;
    info.SrvDescriptorHeap = deviceResources_->shader_visible_cbv_srv_uav_heap().Get();
    info.LegacySingleSrvCpuDescriptor = imguiFontDescriptor_.cpuHandle;
    info.LegacySingleSrvGpuDescriptor = imguiFontDescriptor_.gpuHandle;
    if (!ImGui_ImplDX12_Init(&info))
    {
        return std::unexpected(
            lgp::framework::MakeError("ImGui_ImplDX12_Init", "Failed to initialize the Chapter 12 diagnostics UI."));
    }
    imguiInitialized_ = true;
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
            lgp::framework::MakeError("GenerateGeometry", "Failed to build the Chapter 12 Solution geometry."));
    }

    auto dsvHeapResult =
        lgp::framework::CreateDescriptorHeap(*deviceResources_->device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                             deviceResources_->back_buffer_count(), false, L"Ch12 Solution DSV heap");
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
    if (auto status = CreateMeshBuffers(*sphereResult, L"Ch12 Solution Sphere", sphere_); !status)
    {
        return status;
    }
    if (auto status = CreateMeshBuffers(*groundResult, L"Ch12 Solution Ground", ground_); !status)
    {
        return status;
    }
    if (auto status = CreateMeshBuffers(MakeFrontQuad(12.0F, 12.0F, 6.0F), L"Ch12 Solution Front Quad", quad_); !status)
    {
        return status;
    }
    if (auto status = CreateSizeDependentResources(context.drawableSize); !status)
    {
        return status;
    }
    if (!headless_)
    {
        return InitializeImGui();
    }
    return {};
}

lgp::framework::Status Renderer::Impl::Update(lgp::framework::UpdateContext const &)
{
    sceneState_ = MakeSceneState(headless_, headlessConfiguration_, uiDepthConvention_, uiDebugView_);
    sceneState_.directionToLightWorld = Normalize(sceneState_.directionToLightWorld);
    return {};
}

lgp::framework::Status Renderer::Impl::BuildUi(lgp::framework::UpdateContext const &context)
{
    if (headless_ || !imguiInitialized_)
    {
        return {};
    }

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(context.drawableSize.width), static_cast<float>(context.drawableSize.height)};
    io.DeltaTime = static_cast<float>(std::max(context.deltaSeconds, 1.0 / 240.0));
    io.AddMousePosEvent(static_cast<float>(context.input.mouse.x), static_cast<float>(context.input.mouse.y));
    io.AddMouseButtonEvent(0, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Left));
    io.AddMouseButtonEvent(1, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Right));
    io.AddMouseWheelEvent(0.0F, context.input.mouse.wheelDelta);

    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    imguiFrameBegun_ = true;

    ImGui::Begin("Chapter 12 G-buffer diagnostics");
    int depthMode = static_cast<int>(uiDepthConvention_);
    ImGui::Combo("Depth mode", &depthMode, DepthModeItems());
    uiDepthConvention_ = depthMode == 0 ? DepthConvention::Forward : DepthConvention::Reversed;

    int debugView = static_cast<int>(uiDebugView_);
    ImGui::Combo("Debug view", &debugView, DebugViewComboItems());
    uiDebugView_ = static_cast<gpu::DebugView>(debugView);

    std::array<AttachmentStorage, 4U> const coreAttachments{
        AttachmentStorage{AttachmentSemantic::BaseColorMetalness, 4U},
        AttachmentStorage{AttachmentSemantic::OctahedralNormal, 4U},
        AttachmentStorage{AttachmentSemantic::Roughness, 1U},
        AttachmentStorage{AttachmentSemantic::DeviceDepth, 4U},
    };
    std::array<AttachmentStorage, 6U> const fullAttachments{
        AttachmentStorage{AttachmentSemantic::BaseColorMetalness, 4U},
        AttachmentStorage{AttachmentSemantic::OctahedralNormal, 4U},
        AttachmentStorage{AttachmentSemantic::Roughness, 1U},
        AttachmentStorage{AttachmentSemantic::DeviceDepth, 4U},
        AttachmentStorage{AttachmentSemantic::Motion, 4U},
        AttachmentStorage{AttachmentSemantic::Identity, 4U},
    };
    auto const coreTraffic =
        ComputeLogicalTraffic(context.drawableSize.width, context.drawableSize.height, coreAttachments);
    auto const fullTraffic =
        ComputeLogicalTraffic(context.drawableSize.width, context.drawableSize.height, fullAttachments);

    ImGui::SeparatorText("Attachments");
    ImGui::BulletText("BaseColor+Metalness: R8G8B8A8_TYPELESS, RTV/SRV = R8G8B8A8_UNORM_SRGB");
    ImGui::BulletText("Oct Normal: R16G16_UNORM");
    ImGui::BulletText("Roughness: R8_UNORM");
    ImGui::BulletText("Depth: R32_TYPELESS with D32_FLOAT DSV and R32_FLOAT SRV");
    ImGui::BulletText("Motion: R16G16_FLOAT");
    ImGui::BulletText("Identity: R32_UINT (0 reserved for invalid/background)");

    ImGui::SeparatorText("Nominal payload accounting");
    ImGui::TextWrapped("These numbers are logical chapter payload bytes per pixel, not hardware bandwidth claims.");
    ImGui::Text("Core G-buffer bytes/pixel: 13");
    ImGui::Text("With motion + identity bytes/pixel: 21");
    if (coreTraffic)
    {
        ImGui::Text("Core resident bytes: %llu", static_cast<unsigned long long>(coreTraffic->residentBytes));
    }
    if (fullTraffic)
    {
        ImGui::Text("Full resident bytes: %llu", static_cast<unsigned long long>(fullTraffic->residentBytes));
        ImGui::Text("Full raster write bytes: %llu", static_cast<unsigned long long>(fullTraffic->rasterWriteBytes));
        ImGui::Text("Full deferred read bytes: %llu", static_cast<unsigned long long>(fullTraffic->deferredReadBytes));
        ImGui::Text("Full total payload bytes: %llu", static_cast<unsigned long long>(fullTraffic->totalPayloadBytes));
    }

    ImGui::SeparatorText("Notes");
    ImGui::TextWrapped("Deferred reads use Texture.Load exact-pixel fetches. Motion and identity stay visible for the "
                       "Chapter 11 connection, but this lab does not accumulate temporal history.");
    ImGui::End();
    return {};
}

lgp::framework::Status Renderer::Impl::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr ||
        frameContext.frameSlot >= frameSlots_.size())
    {
        if (imguiFrameBegun_)
        {
            ImGui::EndFrame();
            imguiFrameBegun_ = false;
        }
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
                                                         "Invalid Chapter 12 Solution projection constants."));
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
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);

    std::vector<D3D12_TEXTURE_BARRIER> barriers{};
    barriers.push_back(gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::FrameStartState(frameContext),
                                               gpu::RenderTargetState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::BaseColorMetalness)].Get(),
                                               gpu::CommonState(), gpu::RenderTargetState(),
                                               D3D12_TEXTURE_BARRIER_FLAG_DISCARD));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::OctahedralNormal)].Get(),
                                               gpu::CommonState(), gpu::RenderTargetState(),
                                               D3D12_TEXTURE_BARRIER_FLAG_DISCARD));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::Roughness)].Get(),
                                               gpu::CommonState(), gpu::RenderTargetState(),
                                               D3D12_TEXTURE_BARRIER_FLAG_DISCARD));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::Motion)].Get(),
                                               gpu::CommonState(), gpu::RenderTargetState(),
                                               D3D12_TEXTURE_BARRIER_FLAG_DISCARD));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::Identity)].Get(),
                                               gpu::CommonState(), gpu::UnorderedAccessState(),
                                               D3D12_TEXTURE_BARRIER_FLAG_DISCARD));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::DeviceDepth)].Get(),
                                               gpu::CommonState(), gpu::DepthWriteState(),
                                               D3D12_TEXTURE_BARRIER_FLAG_DISCARD));
    gpu::SubmitTextureBarriers(commandList, barriers);

    float const displayClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    float const baseClear[]{0.0F, 0.0F, 0.0F, 0.0F};
    float const normalClear[]{0.5F, 0.5F, 0.0F, 0.0F};
    float const roughnessClear[]{1.0F, 0.0F, 0.0F, 0.0F};
    float const motionClear[]{0.0F, 0.0F, 0.0F, 0.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, displayClear, 0U, nullptr);
    commandList.ClearRenderTargetView(slot.rtvs.CpuHandle(0U), baseClear, 0U, nullptr);
    commandList.ClearRenderTargetView(slot.rtvs.CpuHandle(1U), normalClear, 0U, nullptr);
    commandList.ClearRenderTargetView(slot.rtvs.CpuHandle(2U), roughnessClear, 0U, nullptr);
    commandList.ClearRenderTargetView(slot.rtvs.CpuHandle(3U), motionClear, 0U, nullptr);
    UINT const identityClear[]{0U, 0U, 0U, 0U};
    commandList.ClearUnorderedAccessViewUint(slot.srvs.GpuHandle(6U), slot.srvs.CpuHandle(6U),
                                             slot.textures[TextureIndex(AttachmentIndex::Identity)].Get(),
                                             identityClear, 0U, nullptr);
    commandList.ClearDepthStencilView(slot.dsv.cpuHandle, D3D12_CLEAR_FLAG_DEPTH,
                                      DepthClearValue(sceneState_.depthConvention), 0U, 0U, nullptr);

    commandList.SetGraphicsRoot32BitConstants(FrameRootConstants, sizeof(FrameConstants) / sizeof(std::uint32_t),
                                              &frameConstants, 0U);
    barriers.clear();
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::Identity)].Get(),
                                               gpu::UnorderedAccessState(), gpu::RenderTargetState()));
    gpu::SubmitTextureBarriers(commandList, barriers);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[5U]{
        slot.rtvs.CpuHandle(0U), slot.rtvs.CpuHandle(1U), slot.rtvs.CpuHandle(2U),
        slot.rtvs.CpuHandle(3U), slot.rtvs.CpuHandle(4U),
    };
    commandList.OMSetRenderTargets(ColorAttachmentCount(), rtvs, FALSE, &slot.dsv.cpuHandle);
    commandList.SetPipelineState(sceneState_.depthConvention == DepthConvention::Forward
                                     ? geometryForwardPipeline_.Get()
                                     : geometryReversedPipeline_.Get());
    for (SceneObject const &object : objects)
    {
        commandList.SetGraphicsRoot32BitConstants(ObjectRootConstants, sizeof(ObjectConstants) / sizeof(std::uint32_t),
                                                  &object.constants, 0U);
        commandList.IASetVertexBuffers(0U, 1U, &object.mesh->vertexView);
        commandList.IASetIndexBuffer(&object.mesh->indexView);
        commandList.DrawIndexedInstanced(object.mesh->indexCount, 1U, 0U, 0, 0U);
    }

    barriers.clear();
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::BaseColorMetalness)].Get(),
                                               gpu::RenderTargetState(), gpu::DirectQueueShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::OctahedralNormal)].Get(),
                                               gpu::RenderTargetState(), gpu::DirectQueueShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::Roughness)].Get(),
                                               gpu::RenderTargetState(), gpu::DirectQueueShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::Motion)].Get(),
                                               gpu::RenderTargetState(), gpu::DirectQueueShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::Identity)].Get(),
                                               gpu::RenderTargetState(), gpu::DirectQueueShaderResourceState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::DeviceDepth)].Get(),
                                               gpu::DepthWriteState(), gpu::DirectQueueShaderResourceState()));
    gpu::SubmitTextureBarriers(commandList, barriers);

    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.SetPipelineState(deferredPipeline_.Get());
    commandList.SetGraphicsRootDescriptorTable(GBufferSrvTable, slot.srvs.gpuHandle);
    commandList.DrawInstanced(3U, 1U, 0U, 0U);

    if (imguiFrameBegun_)
    {
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), &commandList);
        imguiFrameBegun_ = false;
    }

    barriers.clear();
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::BaseColorMetalness)].Get(),
                                               gpu::DirectQueueShaderResourceState(), gpu::CommonState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::OctahedralNormal)].Get(),
                                               gpu::DirectQueueShaderResourceState(), gpu::CommonState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::Roughness)].Get(),
                                               gpu::DirectQueueShaderResourceState(), gpu::CommonState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::Motion)].Get(),
                                               gpu::DirectQueueShaderResourceState(), gpu::CommonState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::Identity)].Get(),
                                               gpu::DirectQueueShaderResourceState(), gpu::CommonState()));
    barriers.push_back(gpu::MakeTextureBarrier(*slot.textures[TextureIndex(AttachmentIndex::DeviceDepth)].Get(),
                                               gpu::DirectQueueShaderResourceState(), gpu::CommonState()));
    barriers.push_back(gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(),
                                               gpu::FrameEndState(frameContext)));
    gpu::SubmitTextureBarriers(commandList, barriers);
    lastRenderedFrameSlot_ = frameContext.frameSlot;
    return {};
}

std::expected<Renderer::ReadbackOutputs, lgp::framework::Error> Renderer::Impl::ReadBackOutputs() const
{
    if (frameSlots_.empty())
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "No Chapter 12 Solution frame has been rendered yet."));
    }

    ReadbackOutputs outputs{};
    auto base = gpu::ReadBackTexture(
        *deviceResources_,
        *frameSlots_[lastRenderedFrameSlot_].textures[TextureIndex(AttachmentIndex::BaseColorMetalness)].Get(),
        gpu::CommonState());
    if (!base)
    {
        return std::unexpected(std::move(base.error()));
    }
    outputs.baseColorMetalness = std::move(*base);

    auto normal = gpu::ReadBackTexture(
        *deviceResources_,
        *frameSlots_[lastRenderedFrameSlot_].textures[TextureIndex(AttachmentIndex::OctahedralNormal)].Get(),
        gpu::CommonState());
    if (!normal)
    {
        return std::unexpected(std::move(normal.error()));
    }
    outputs.octahedralNormal = std::move(*normal);

    auto roughness = gpu::ReadBackTexture(
        *deviceResources_,
        *frameSlots_[lastRenderedFrameSlot_].textures[TextureIndex(AttachmentIndex::Roughness)].Get(),
        gpu::CommonState());
    if (!roughness)
    {
        return std::unexpected(std::move(roughness.error()));
    }
    outputs.roughness = std::move(*roughness);

    auto depth = gpu::ReadBackTexture(
        *deviceResources_,
        *frameSlots_[lastRenderedFrameSlot_].textures[TextureIndex(AttachmentIndex::DeviceDepth)].Get(),
        gpu::CommonState());
    if (!depth)
    {
        return std::unexpected(std::move(depth.error()));
    }
    outputs.deviceDepth = std::move(*depth);

    auto motion = gpu::ReadBackTexture(
        *deviceResources_, *frameSlots_[lastRenderedFrameSlot_].textures[TextureIndex(AttachmentIndex::Motion)].Get(),
        gpu::CommonState());
    if (!motion)
    {
        return std::unexpected(std::move(motion.error()));
    }
    outputs.motion = std::move(*motion);

    auto identity = gpu::ReadBackTexture(
        *deviceResources_, *frameSlots_[lastRenderedFrameSlot_].textures[TextureIndex(AttachmentIndex::Identity)].Get(),
        gpu::CommonState());
    if (!identity)
    {
        return std::unexpected(std::move(identity.error()));
    }
    outputs.identity = std::move(*identity);
    return outputs;
}

void Renderer::Impl::Shutdown() noexcept
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
    ReleaseSizeDependentResources();
    if (deviceResources_ != nullptr && imguiFontDescriptor_)
    {
        deviceResources_->shader_visible_cbv_srv_uav_heap().Free(imguiFontDescriptor_);
    }
    imguiFontDescriptor_ = {};
    dsvHeap_ = {};
    quad_ = {};
    ground_ = {};
    sphere_ = {};
    deferredPipeline_.Reset();
    geometryReversedPipeline_.Reset();
    geometryForwardPipeline_.Reset();
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

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    impl_->headlessConfiguration_ = configuration;
}

std::expected<Renderer::ReadbackOutputs, lgp::framework::Error> Renderer::ReadBackOutputs() const
{
    return impl_->ReadBackOutputs();
}

} // namespace ch12::gbuffer::solution
