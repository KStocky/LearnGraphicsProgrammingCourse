#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include "../Common/ChapterGeometry.hpp"

#include <wrl/client.h>

#include <DirectXMath.h>
#include <d3d12.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <lgp/framework/buffer.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

namespace ch07::shadows::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
inline constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

enum RootParameter : UINT
{
    LightingRootConstants = 0U,
    ObjectRootConstants = 1U,
    SceneTextures = 2U,
};

struct LightingConstants final
{
    DirectX::XMFLOAT4X4 viewProjection{};
    DirectX::XMFLOAT4X4 lightViewProjection{};
    DirectX::XMFLOAT3 cameraPosition{};
    float exposure{-4.0F};
    DirectX::XMFLOAT3 directionToLight{};
    float intensity{1200.0F};
    float receiverDepthBias{0.00035F};
    float receiverNormalOffsetWorld{0.02F};
    float configuredConstantBiasEstimateDepth{};
    float configuredSlopeBias{1.5F};
    float configuredBiasClamp{0.01F};
    std::uint32_t visualization{};
};

struct ObjectConstants final
{
    DirectX::XMFLOAT4X4 world{};
    std::uint32_t objectId{};
};

struct MeshBuffers final
{
    lgp::framework::Buffer vertices{};
    lgp::framework::Buffer indices{};
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    UINT indexCount{};
};

static_assert(sizeof(LightingConstants) % sizeof(std::uint32_t) == 0U);
static_assert(sizeof(ObjectConstants) % sizeof(std::uint32_t) == 0U);

[[nodiscard]] std::string D3D12BlobToUtf8(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] std::filesystem::path ResolveShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "DirectionalBaseline.hlsl";
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
lgp::framework::TextureBarrierState constexpr kDepthWriteState{
    D3D12_BARRIER_SYNC_DEPTH_STENCIL,
    D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
    D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
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

[[nodiscard]] DirectX::XMFLOAT3 NormalizeDirection(DirectX::XMFLOAT3 direction) noexcept
{
    float const lengthSquared = (direction.x * direction.x) + (direction.y * direction.y) + (direction.z * direction.z);
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-8F)
    {
        return {-0.45F, 0.82F, -0.35F};
    }
    DirectX::XMVECTOR const value = DirectX::XMLoadFloat3(&direction);
    DirectX::XMVECTOR const normalized = DirectX::XMVector3Normalize(value);
    DirectX::XMFLOAT3 result{};
    DirectX::XMStoreFloat3(&result, normalized);
    return result;
}

[[nodiscard]] ObjectConstants MakeObject(DirectX::XMMATRIX const &world, std::uint32_t objectId) noexcept
{
    ObjectConstants object{};
    DirectX::XMStoreFloat4x4(&object.world, world);
    object.objectId = objectId;
    return object;
}

struct SceneObject final
{
    MeshBuffers const *mesh{};
    ObjectConstants constants{};
};

} // namespace

class Renderer::Impl final
{
  public:
    lgp::framework::DeviceResources *deviceResources{};
    lgp::framework::CompiledShader lightingVertexShader{};
    lgp::framework::CompiledShader lightingPixelShader{};
    lgp::framework::CompiledShader shadowVertexShader{};
    lgp::framework::CompiledShader fullscreenVertexShader{};
    lgp::framework::CompiledShader displayPixelShader{};
    ComPtr<ID3D12RootSignature> rootSignature{};
    ComPtr<ID3D12PipelineState> lightingPipeline{};
    ComPtr<ID3D12PipelineState> shadowPipeline{};
    ComPtr<ID3D12PipelineState> displayPipeline{};
    MeshBuffers sphere{};
    MeshBuffers plane{};
    MeshBuffers box{};
    lgp::framework::DescriptorHeap dsvHeap{};
    lgp::framework::DescriptorAllocation depthView{};
    lgp::framework::DescriptorAllocation shadowDepthView{};
    lgp::framework::DescriptorAllocation hdrRtv{};
    lgp::framework::DescriptorAllocation textureSrvs{};
    lgp::framework::DescriptorAllocation imguiFontDescriptor{};
    ComPtr<ID3D12Resource> depthTarget{};
    ComPtr<ID3D12Resource> hdrTarget{};
    ComPtr<ID3D12Resource> shadowTarget{};
    LightingConstants lighting{};
    DirectX::XMFLOAT3 lightDirection{-0.45F, 0.82F, -0.35F};
    DirectX::XMFLOAT3 lightTarget{0.0F, 0.0F, 0.0F};
    OrthographicExtents orthographicExtents{-10.0F, 10.0F, -8.0F, 8.0F};
    DepthRange lightDepthRange{0.1F, 40.0F};
    float lightDistance{18.0F};
    std::uint32_t shadowResolution{1024U};
    std::int32_t rasterConstantBias{1200};
    float rasterSlopeBias{1.5F};
    float rasterBiasClamp{0.01F};
    bool shadowEnabled{true};
    ShadowVisualization visualization{ShadowVisualization::Final};
    float orbitAzimuth{};
    float orbitElevation{0.34F};
    float orbitRadius{20.0F};
    bool headless{};
    bool imguiInitialized{};
    bool imguiFrameBegun{};
    std::optional<HeadlessTestConfiguration> headlessTest{};

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context);
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelineStates();
    [[nodiscard]] lgp::framework::Status CreateShadowPipeline();
    [[nodiscard]] lgp::framework::Status CreateMeshBuffers(GeometryMesh const &mesh, std::wstring_view name,
                                                           MeshBuffers &buffers);
    [[nodiscard]] lgp::framework::Status CreateSizeDependentTargets(lgp::framework::Extent2D size);
    [[nodiscard]] lgp::framework::Status CreateShadowTarget();
    [[nodiscard]] lgp::framework::Status InitializeImGui();
    [[nodiscard]] lgp::framework::Status BuildUi(lgp::framework::UpdateContext const &context);
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context);
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext);
    void DrawMesh(ID3D12GraphicsCommandList &commandList, MeshBuffers const &mesh,
                  ObjectConstants const &object) const noexcept;
    [[nodiscard]] std::array<SceneObject, 8U> SceneObjects() const noexcept;
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

    auto compile = [&compiler,
                    &options](wchar_t const *entryPoint,
                              wchar_t const *profile) -> lgp::framework::Result<lgp::framework::CompiledShader>
    {
        options.entryPoint = entryPoint;
        options.targetProfile = profile;
        options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
        return compiler.Compile(options);
    };

    auto lightingVs = compile(L"LightingVS", L"vs_6_0");
    if (!lightingVs)
    {
        return std::unexpected(std::move(lightingVs.error()));
    }
    lightingVertexShader = std::move(lightingVs.value());
    auto shadowVs = compile(L"ShadowVS", L"vs_6_0");
    if (!shadowVs)
    {
        return std::unexpected(std::move(shadowVs.error()));
    }
    shadowVertexShader = std::move(shadowVs.value());
    auto lightingPs = compile(L"LightingPS", L"ps_6_0");
    if (!lightingPs)
    {
        return std::unexpected(std::move(lightingPs.error()));
    }
    lightingPixelShader = std::move(lightingPs.value());
    auto fullscreenVs = compile(L"FullscreenVS", L"vs_6_0");
    if (!fullscreenVs)
    {
        return std::unexpected(std::move(fullscreenVs.error()));
    }
    fullscreenVertexShader = std::move(fullscreenVs.value());
    auto displayPs = compile(L"DisplayPS", L"ps_6_0");
    if (!displayPs)
    {
        return std::unexpected(std::move(displayPs.error()));
    }
    displayPixelShader = std::move(displayPs.value());
    return {};
}

lgp::framework::Status Renderer::Impl::CreateRootSignature()
{
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[LightingRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[LightingRootConstants].Constants.ShaderRegister = 0U;
    parameters[LightingRootConstants].Constants.Num32BitValues = sizeof(LightingConstants) / sizeof(std::uint32_t);
    parameters[LightingRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ObjectRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[ObjectRootConstants].Constants.ShaderRegister = 1U;
    parameters[ObjectRootConstants].Constants.Num32BitValues = sizeof(ObjectConstants) / sizeof(std::uint32_t);
    parameters[ObjectRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2U;
    range.BaseShaderRegister = 0U;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    parameters[SceneTextures].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[SceneTextures].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[SceneTextures].DescriptorTable.pDescriptorRanges = &range;
    parameters[SceneTextures].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[3]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].ShaderRegister = 1U;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].ShaderRegister = 2U;
    samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[2].MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
    description.pParameters = parameters;
    description.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    description.pStaticSamplers = samplers;
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
                                                                "Failed to create the direct-light root signature."));
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
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    D3D12_INPUT_ELEMENT_DESC const inputLayout[]{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature.Get();
    description.VS = lightingVertexShader.Bytecode();
    description.PS = lightingPixelShader.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = kHdrFormat;
    description.DSVFormat = kDepthFormat;
    description.SampleDesc.Count = 1U;
    HRESULT const lightingResult = deviceResources->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(lightingPipeline.ReleaseAndGetAddressOf()));
    if (FAILED(lightingResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device::CreateGraphicsPipelineState", lightingResult, "Failed to create the lighting pipeline."));
    }

    description.VS = fullscreenVertexShader.Bytecode();
    description.PS = displayPixelShader.Bytecode();
    description.InputLayout = {};
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RTVFormats[0] = deviceResources->back_buffer_format();
    description.DSVFormat = DXGI_FORMAT_UNKNOWN;
    HRESULT const displayResult = deviceResources->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(displayPipeline.ReleaseAndGetAddressOf()));
    if (FAILED(displayResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device::CreateGraphicsPipelineState", displayResult, "Failed to create the display pipeline."));
    }
    return {};
}

lgp::framework::Status Renderer::Impl::CreateShadowPipeline()
{
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = 0U;
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthBias = rasterConstantBias;
    rasterizer.DepthBiasClamp = rasterBiasClamp;
    rasterizer.SlopeScaledDepthBias = rasterSlopeBias;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    D3D12_INPUT_ELEMENT_DESC const inputLayout[]{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature.Get();
    description.VS = shadowVertexShader.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.InputLayout = {inputLayout, static_cast<UINT>(std::size(inputLayout))};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 0U;
    description.DSVFormat = kDepthFormat;
    description.SampleDesc.Count = 1U;
    ComPtr<ID3D12PipelineState> pipeline;
    HRESULT const result =
        deviceResources->device()->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(pipeline.GetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create the shadow depth pipeline."));
    }
    shadowPipeline = std::move(pipeline);
    return {};
}

lgp::framework::Status Renderer::Impl::CreateMeshBuffers(GeometryMesh const &mesh, std::wstring_view name,
                                                         MeshBuffers &buffers)
{
    auto vertexResult = lgp::framework::CreateUploadBuffer(
        *deviceResources->device(), mesh.vertices.size() * sizeof(GeometryVertex), std::wstring{name} + L" vertices");
    if (!vertexResult)
    {
        return std::unexpected(std::move(vertexResult.error()));
    }
    buffers.vertices = std::move(vertexResult.value());
    if (auto status = lgp::framework::WriteBuffer(
            buffers.vertices, std::span<GeometryVertex const>{mesh.vertices.data(), mesh.vertices.size()});
        !status)
    {
        return status;
    }
    auto indexResult = lgp::framework::CreateUploadBuffer(
        *deviceResources->device(), mesh.indices.size() * sizeof(std::uint32_t), std::wstring{name} + L" indices");
    if (!indexResult)
    {
        return std::unexpected(std::move(indexResult.error()));
    }
    buffers.indices = std::move(indexResult.value());
    if (auto status = lgp::framework::WriteBuffer(
            buffers.indices, std::span<std::uint32_t const>{mesh.indices.data(), mesh.indices.size()});
        !status)
    {
        return status;
    }
    buffers.vertexView.BufferLocation = buffers.vertices.gpu_virtual_address();
    buffers.vertexView.SizeInBytes = static_cast<UINT>(mesh.vertices.size() * sizeof(GeometryVertex));
    buffers.vertexView.StrideInBytes = sizeof(GeometryVertex);
    buffers.indexView.BufferLocation = buffers.indices.gpu_virtual_address();
    buffers.indexView.SizeInBytes = static_cast<UINT>(mesh.indices.size() * sizeof(std::uint32_t));
    buffers.indexView.Format = DXGI_FORMAT_R32_UINT;
    buffers.indexCount = static_cast<UINT>(mesh.indices.size());
    return {};
}

lgp::framework::Status Renderer::Impl::CreateSizeDependentTargets(lgp::framework::Extent2D size)
{
    depthTarget.Reset();
    hdrTarget.Reset();
    if (size.empty())
    {
        return {};
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC1 texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = size.width;
    texture.Height = size.height;
    texture.DepthOrArraySize = 1U;
    texture.MipLevels = 1U;
    texture.SampleDesc.Count = 1U;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_CLEAR_VALUE hdrClear{};
    hdrClear.Format = kHdrFormat;
    hdrClear.Color[3] = 1.0F;
    texture.Format = kHdrFormat;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    HRESULT const hdrResult = deviceResources->device()->CreateCommittedResource3(
        &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &hdrClear, nullptr, 0U, nullptr,
        IID_PPV_ARGS(hdrTarget.ReleaseAndGetAddressOf()));
    if (FAILED(hdrResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", hdrResult,
                                                                "Failed to create the lighting HDR target."));
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
    deviceResources->device()->CreateShaderResourceView(hdrTarget.Get(), &srv, textureSrvs.CpuHandle(0U));

    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = kDepthFormat;
    depthClear.DepthStencil.Depth = 1.0F;
    texture.Format = kDepthFormat;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    HRESULT const depthResult = deviceResources->device()->CreateCommittedResource3(
        &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, &depthClear, nullptr, 0U,
        nullptr, IID_PPV_ARGS(depthTarget.ReleaseAndGetAddressOf()));
    if (FAILED(depthResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", depthResult,
                                                                "Failed to create the lighting depth target."));
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = kDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    deviceResources->device()->CreateDepthStencilView(depthTarget.Get(), &dsv, depthView.cpuHandle);
    return {};
}

lgp::framework::Status Renderer::Impl::CreateShadowTarget()
{
    shadowTarget.Reset();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC1 texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = shadowResolution;
    texture.Height = shadowResolution;
    texture.DepthOrArraySize = 1U;
    texture.MipLevels = 1U;
    texture.Format = DXGI_FORMAT_R32_TYPELESS;
    texture.SampleDesc.Count = 1U;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0F;
    HRESULT const result = deviceResources->device()->CreateCommittedResource3(
        &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &clear, nullptr, 0U, nullptr,
        IID_PPV_ARGS(shadowTarget.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                                "Failed to create the typeless shadow map."));
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    deviceResources->device()->CreateDepthStencilView(shadowTarget.Get(), &dsv, shadowDepthView.cpuHandle);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1U;
    deviceResources->device()->CreateShaderResourceView(shadowTarget.Get(), &srv, textureSrvs.CpuHandle(1U));
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
        return std::unexpected(lgp::framework::MakeError("ImGui_ImplDX12_Init", "Failed to initialize the shadow UI."));
    }
    imguiInitialized = true;
    return {};
}

lgp::framework::Status Renderer::Impl::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources = &context.deviceResources;
    headless = context.commandLine.headless;
    auto sphereResult = GenerateSphere(0.72F, 24U, 48U);
    auto planeResult = GeneratePlane(22.0F, 18.0F, 1U, 1U);
    auto boxResult = GenerateBox({1.0F, 1.0F, 1.0F});
    if (!sphereResult || !planeResult || !boxResult)
    {
        return std::unexpected(lgp::framework::MakeError("GenerateGeometry", "Could not create chapter geometry."));
    }
    auto dsvResult = lgp::framework::CreateDescriptorHeap(*deviceResources->device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                                          2U, false, L"Ch07 depth descriptors");
    if (!dsvResult)
    {
        return std::unexpected(std::move(dsvResult.error()));
    }
    dsvHeap = std::move(dsvResult.value());
    auto depthAllocation = dsvHeap.Allocate(2U);
    if (!depthAllocation)
    {
        return std::unexpected(std::move(depthAllocation.error()));
    }
    depthView = depthAllocation.value();
    shadowDepthView = depthView;
    shadowDepthView.cpuHandle = depthView.CpuHandle(1U);
    shadowDepthView.offset += 1U;
    shadowDepthView.count = 1U;
    auto rtvAllocation = deviceResources->rtv_heap().Allocate(1U);
    if (!rtvAllocation)
    {
        return std::unexpected(std::move(rtvAllocation.error()));
    }
    hdrRtv = rtvAllocation.value();
    auto srvAllocation = deviceResources->shader_visible_cbv_srv_uav_heap().Allocate(2U);
    if (!srvAllocation)
    {
        return std::unexpected(std::move(srvAllocation.error()));
    }
    textureSrvs = srvAllocation.value();

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
    if (auto status = CreateShadowPipeline(); !status)
    {
        return status;
    }
    if (auto status = CreateMeshBuffers(*sphereResult, L"Ch07 sphere", sphere); !status)
    {
        return status;
    }
    if (auto status = CreateMeshBuffers(*planeResult, L"Ch07 plane", plane); !status)
    {
        return status;
    }
    if (auto status = CreateMeshBuffers(*boxResult, L"Ch07 box", box); !status)
    {
        return status;
    }
    if (auto status = CreateSizeDependentTargets(context.drawableSize); !status)
    {
        return status;
    }
    if (auto status = CreateShadowTarget(); !status)
    {
        return status;
    }
    if (!headless)
    {
        if (auto status = InitializeImGui(); !status)
        {
            return status;
        }
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
    io.DisplaySize = {static_cast<float>(context.drawableSize.width), static_cast<float>(context.drawableSize.height)};
    io.DeltaTime = static_cast<float>(std::max(context.deltaSeconds, 1.0 / 240.0));
    io.AddMousePosEvent(static_cast<float>(context.input.mouse.x), static_cast<float>(context.input.mouse.y));
    io.AddMouseButtonEvent(0, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Left));
    io.AddMouseButtonEvent(1, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Right));
    io.AddMouseWheelEvent(0.0F, context.input.mouse.wheelDelta);
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    imguiFrameBegun = true;

    ImGui::Begin("Directional hard-shadow map");
    ImGui::Checkbox("Shadow enabled", &shadowEnabled);
    int resolutionIndex = 2;
    if (shadowResolution == 512U)
    {
        resolutionIndex = 0;
    }
    else if (shadowResolution == 1024U)
    {
        resolutionIndex = 1;
    }
    if (ImGui::Combo("Resolution", &resolutionIndex,
                     "512 x 512\0"
                     "1024 x 1024\0"
                     "2048 x 2048\0"))
    {
        shadowResolution = std::array<std::uint32_t, 3U>{512U, 1024U, 2048U}[static_cast<std::size_t>(resolutionIndex)];
    }
    ImGui::SliderFloat3("Direction to light", &lightDirection.x, -1.0F, 1.0F);
    ImGui::SliderFloat3("Light target (world m)", &lightTarget.x, -10.0F, 10.0F, "%.2f m");
    ImGui::SliderFloat("Light distance (world m)", &lightDistance, 1.0F, 50.0F, "%.2f m");
    ImGui::SliderFloat("Ortho left (light m)", &orthographicExtents.left, -30.0F, -0.1F, "%.2f");
    ImGui::SliderFloat("Ortho right (light m)", &orthographicExtents.right, 0.1F, 30.0F, "%.2f");
    ImGui::SliderFloat("Ortho bottom (light m)", &orthographicExtents.bottom, -30.0F, -0.1F, "%.2f");
    ImGui::SliderFloat("Ortho top (light m)", &orthographicExtents.top, 0.1F, 30.0F, "%.2f");
    ImGui::SliderFloat("Near plane (light m)", &lightDepthRange.nearPlane, 0.01F, 20.0F, "%.3f");
    ImGui::SliderFloat("Far plane (light m)", &lightDepthRange.farPlane, 1.0F, 100.0F, "%.2f");
    ImGui::SeparatorText("Hardware raster bias controls (shadow-map write)");
    ImGui::SliderInt("DepthBias (integer units)", &rasterConstantBias, -10000, 10000);
    ImGui::SliderFloat("SlopeScaledDepthBias", &rasterSlopeBias, -8.0F, 8.0F, "%.4f");
    ImGui::SliderFloat("DepthBiasClamp", &rasterBiasClamp, -0.05F, 0.05F, "%.6f");
    ImGui::TextWrapped("The configured-bias estimate view is educational, not a reconstruction of D32_FLOAT raster "
                       "bias. It uses DepthBias * 2^-23 as a fixed upper-magnitude scale for normalized D32 values "
                       "and a world-normal slope proxy; the hardware uses each shadow primitive's depth exponent and "
                       "maximum raster depth slope, which the camera pass cannot recover.");
    ImGui::SeparatorText("Receiver bias (camera-pass compare)");
    ImGui::SliderFloat("Exact depth subtraction", &lighting.receiverDepthBias, -0.01F, 0.01F, "%.9f");
    ImGui::SliderFloat("Normal offset (world m)", &lighting.receiverNormalOffsetWorld, -0.2F, 0.2F, "%.4f m");
    ImGui::SliderFloat("Illuminance", &lighting.intensity, 0.0F, 10000.0F, "%.0f lux");
    ImGui::SliderFloat("Exposure", &lighting.exposure, -10.0F, 4.0F, "%.2f EV");
    int view = static_cast<int>(visualization);
    ImGui::Combo(
        "View", &view,
        "Final\0Visibility\0Shadow depth (point texel)\0Receiver depth\0Stored minus receiver\0Configured bias "
        "estimate (educational)\0Shadow UV\0Frustum coverage\0Normal-slope proxy\0Finite validation\0Object "
        "ID\0Point-sample agreement\0Comparison relation\0World texel size\0");
    visualization = static_cast<ShadowVisualization>(view);
    ImGui::TextWrapped("Right-drag orbits the camera; the wheel dollies. The exact receiver comparison is "
                       "receiverDepth - %.9g. Positive normal offset moves the receiver along its world normal.",
                       lighting.receiverDepthBias);
    ImGui::End();
    return {};
}

lgp::framework::Status Renderer::Impl::Update(lgp::framework::UpdateContext const &context)
{
    std::uint32_t const previousResolution = shadowResolution;
    std::int32_t const previousConstantBias = rasterConstantBias;
    float const previousSlopeBias = rasterSlopeBias;
    float const previousBiasClamp = rasterBiasClamp;
    if (auto status = BuildUi(context); !status)
    {
        return status;
    }
    if (!headless && context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Right))
    {
        orbitAzimuth += static_cast<float>(context.input.mouse.deltaX) * 0.006F;
        orbitElevation =
            std::clamp(orbitElevation - (static_cast<float>(context.input.mouse.deltaY) * 0.006F), -0.15F, 1.15F);
    }
    if (!headless)
    {
        orbitRadius = std::clamp(orbitRadius - context.input.mouse.wheelDelta, 10.0F, 28.0F);
    }
    float const horizontalRadius = orbitRadius * std::cos(orbitElevation);
    DirectX::XMFLOAT3 const cameraPosition{
        horizontalRadius * std::sin(orbitAzimuth),
        1.4F + (orbitRadius * std::sin(orbitElevation)),
        -horizontalRadius * std::cos(orbitAzimuth),
    };
    DirectX::XMVECTOR const eye = DirectX::XMLoadFloat3(&cameraPosition);
    DirectX::XMVECTOR const target = DirectX::XMVectorSet(0.5F, 1.3F, 0.8F, 1.0F);
    DirectX::XMMATRIX const view = DirectX::XMMatrixLookAtLH(eye, target, DirectX::XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F));
    float const aspect = context.drawableSize.height == 0U ? 1.0F
                                                           : static_cast<float>(context.drawableSize.width) /
                                                                 static_cast<float>(context.drawableSize.height);
    DirectX::XMMATRIX const projection = DirectX::XMMatrixPerspectiveFovLH(0.78F, aspect, 0.1F, 100.0F);
    DirectX::XMStoreFloat4x4(&lighting.viewProjection, view * projection);
    lighting.cameraPosition = cameraPosition;
    lighting.directionToLight = NormalizeDirection(lightDirection);
    if (headlessTest)
    {
        HeadlessTestConfiguration const &test = *headlessTest;
        lighting.directionToLight =
            NormalizeDirection({test.directionToLight.x, test.directionToLight.y, test.directionToLight.z});
        lighting.intensity = std::max(test.illuminanceLux, 0.0F);
        lighting.exposure = test.exposure;
        shadowResolution = std::clamp(test.shadowResolution, 128U, 4096U);
        orthographicExtents = test.orthographicExtents;
        lightTarget = {test.lightTarget.x, test.lightTarget.y, test.lightTarget.z};
        lightDistance = std::max(test.lightDistance, 0.01F);
        lightDepthRange = test.depthRange;
        rasterConstantBias = test.rasterConstantBias;
        rasterSlopeBias = test.rasterSlopeBias;
        rasterBiasClamp = test.rasterBiasClamp;
        lighting.receiverDepthBias = test.receiverDepthBias;
        lighting.receiverNormalOffsetWorld = test.receiverNormalOffsetWorld;
        shadowEnabled = test.shadowEnabled;
        visualization = test.visualization;
    }
    lightDepthRange.nearPlane = std::max(lightDepthRange.nearPlane, 0.001F);
    lightDepthRange.farPlane = std::max(lightDepthRange.farPlane, lightDepthRange.nearPlane + 0.001F);
    orthographicExtents.right = std::max(orthographicExtents.right, orthographicExtents.left + 0.001F);
    orthographicExtents.top = std::max(orthographicExtents.top, orthographicExtents.bottom + 0.001F);
    DirectX::XMFLOAT3 const direction = lighting.directionToLight;
    DirectX::XMVECTOR const lightTargetVector = DirectX::XMLoadFloat3(&lightTarget);
    DirectX::XMVECTOR const directionVector = DirectX::XMLoadFloat3(&direction);
    DirectX::XMVECTOR const lightPosition =
        DirectX::XMVectorAdd(lightTargetVector, DirectX::XMVectorScale(directionVector, lightDistance));
    DirectX::XMVECTOR up = std::fabs(direction.y) > 0.98F ? DirectX::XMVectorSet(1.0F, 0.0F, 0.0F, 0.0F)
                                                          : DirectX::XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F);
    DirectX::XMMATRIX const lightView = DirectX::XMMatrixLookAtLH(lightPosition, lightTargetVector, up);
    DirectX::XMMATRIX const lightProjection = DirectX::XMMatrixOrthographicOffCenterLH(
        orthographicExtents.left, orthographicExtents.right, orthographicExtents.bottom, orthographicExtents.top,
        lightDepthRange.nearPlane, lightDepthRange.farPlane);
    DirectX::XMStoreFloat4x4(&lighting.lightViewProjection, lightView * lightProjection);
    lighting.configuredConstantBiasEstimateDepth = static_cast<float>(rasterConstantBias) * std::exp2(-23.0F);
    lighting.configuredSlopeBias = rasterSlopeBias;
    lighting.configuredBiasClamp = rasterBiasClamp;
    if (visualization == ShadowVisualization::WorldTexelSize)
    {
        lighting.configuredConstantBiasEstimateDepth =
            (orthographicExtents.right - orthographicExtents.left) / static_cast<float>(shadowResolution);
        lighting.configuredSlopeBias =
            (orthographicExtents.top - orthographicExtents.bottom) / static_cast<float>(shadowResolution);
    }
    lighting.visualization =
        static_cast<std::uint32_t>(visualization) | (shadowEnabled ? 0U : (std::uint32_t{1U} << 31U));

    bool const recreateShadow = shadowResolution != previousResolution;
    bool const recreatePipeline = rasterConstantBias != previousConstantBias || rasterSlopeBias != previousSlopeBias ||
                                  rasterBiasClamp != previousBiasClamp;
    if (recreateShadow || recreatePipeline)
    {
        if (auto status = deviceResources->WaitForGpuIdle(); !status)
        {
            return status;
        }
        if (recreateShadow)
        {
            if (auto status = CreateShadowTarget(); !status)
            {
                return status;
            }
        }
        if (recreatePipeline)
        {
            if (auto status = CreateShadowPipeline(); !status)
            {
                return status;
            }
        }
    }
    return {};
}

void Renderer::Impl::DrawMesh(ID3D12GraphicsCommandList &commandList, MeshBuffers const &mesh,
                              ObjectConstants const &object) const noexcept
{
    commandList.SetGraphicsRoot32BitConstants(ObjectRootConstants, sizeof(ObjectConstants) / sizeof(std::uint32_t),
                                              &object, 0U);
    commandList.IASetVertexBuffers(0U, 1U, &mesh.vertexView);
    commandList.IASetIndexBuffer(&mesh.indexView);
    commandList.DrawIndexedInstanced(mesh.indexCount, 1U, 0U, 0, 0U);
}

std::array<SceneObject, 8U> Renderer::Impl::SceneObjects() const noexcept
{
    return {{
        {&plane, MakeObject(DirectX::XMMatrixIdentity(), 1U)},
        {&box,
         MakeObject(DirectX::XMMatrixScaling(2.2F, 1.5F, 2.0F) * DirectX::XMMatrixTranslation(-3.2F, 0.75F, 1.8F), 2U)},
        {&sphere, MakeObject(DirectX::XMMatrixTranslation(-0.5F, 0.72F, 1.2F), 3U)},
        {&sphere, MakeObject(DirectX::XMMatrixTranslation(2.1F, 0.72F, -1.1F), 4U)},
        {&box,
         MakeObject(DirectX::XMMatrixScaling(0.16F, 3.2F, 2.4F) * DirectX::XMMatrixTranslation(1.1F, 1.6F, 2.8F), 5U)},
        {&box, MakeObject(DirectX::XMMatrixScaling(3.6F, 0.24F, 1.4F) * DirectX::XMMatrixRotationZ(0.42F) *
                              DirectX::XMMatrixTranslation(-1.0F, 0.9F, -2.4F),
                          6U)},
        {&box, MakeObject(DirectX::XMMatrixScaling(2.8F, 0.12F, 2.2F) * DirectX::XMMatrixRotationX(0.16F) *
                              DirectX::XMMatrixTranslation(3.4F, 0.34F, 3.7F),
                          7U)},
        {&box,
         MakeObject(DirectX::XMMatrixScaling(1.0F, 1.6F, 1.0F) * DirectX::XMMatrixTranslation(7.7F, 0.8F, 0.4F), 8U)},
    }};
}

lgp::framework::Status Renderer::Impl::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr || hdrTarget == nullptr ||
        depthTarget == nullptr || shadowTarget == nullptr)
    {
        if (imguiFrameBegun)
        {
            ImGui::EndFrame();
            imguiFrameBegun = false;
        }
        return {};
    }
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.SetGraphicsRootSignature(rootSignature.Get());
    commandList.SetGraphicsRoot32BitConstants(LightingRootConstants, sizeof(LightingConstants) / sizeof(std::uint32_t),
                                              &lighting, 0U);
    std::array<SceneObject, 8U> const objects = SceneObjects();

    lgp::framework::TransitionTexture(commandList, *shadowTarget.Get(), kShaderResourceState, kDepthWriteState);
    D3D12_VIEWPORT const shadowViewport{
        0.0F, 0.0F, static_cast<float>(shadowResolution), static_cast<float>(shadowResolution), 0.0F, 1.0F};
    D3D12_RECT const shadowScissor{0, 0, static_cast<LONG>(shadowResolution), static_cast<LONG>(shadowResolution)};
    commandList.RSSetViewports(1U, &shadowViewport);
    commandList.RSSetScissorRects(1U, &shadowScissor);
    commandList.OMSetRenderTargets(0U, nullptr, FALSE, &shadowDepthView.cpuHandle);
    commandList.ClearDepthStencilView(shadowDepthView.cpuHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0U, 0U, nullptr);
    commandList.SetPipelineState(shadowPipeline.Get());
    for (SceneObject const &object : objects)
    {
        DrawMesh(commandList, *object.mesh, object.constants);
    }
    lgp::framework::TransitionTexture(commandList, *shadowTarget.Get(), kDepthWriteState, kShaderResourceState);

    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    lgp::framework::TransitionTexture(commandList, *hdrTarget.Get(), kShaderResourceState, kRenderTargetState);
    float const hdrClear[]{0.012F, 0.018F, 0.028F, 1.0F};
    commandList.OMSetRenderTargets(1U, &hdrRtv.cpuHandle, FALSE, &depthView.cpuHandle);
    commandList.ClearRenderTargetView(hdrRtv.cpuHandle, hdrClear, 0U, nullptr);
    commandList.ClearDepthStencilView(depthView.cpuHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0U, 0U, nullptr);
    commandList.SetPipelineState(lightingPipeline.Get());
    commandList.SetGraphicsRootDescriptorTable(SceneTextures, textureSrvs.gpuHandle);
    for (SceneObject const &object : objects)
    {
        DrawMesh(commandList, *object.mesh, object.constants);
    }

    lgp::framework::TransitionTexture(commandList, *hdrTarget.Get(), kRenderTargetState, kShaderResourceState);

    lgp::framework::TransitionTexture(commandList, *frameContext.renderTarget, FrameStartState(frameContext),
                                      kRenderTargetState);
    float const displayClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.ClearRenderTargetView(frameContext.renderTargetView, displayClear, 0U, nullptr);
    commandList.SetPipelineState(displayPipeline.Get());
    commandList.SetGraphicsRoot32BitConstants(LightingRootConstants, sizeof(LightingConstants) / sizeof(std::uint32_t),
                                              &lighting, 0U);
    commandList.SetGraphicsRootDescriptorTable(SceneTextures, textureSrvs.gpuHandle);
    commandList.DrawInstanced(3U, 1U, 0U, 0U);
    if (imguiFrameBegun)
    {
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), &commandList);
        imguiFrameBegun = false;
    }
    lgp::framework::TransitionTexture(commandList, *frameContext.renderTarget, kRenderTargetState,
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
    }
    depthTarget.Reset();
    hdrTarget.Reset();
    shadowTarget.Reset();
    if (depthView)
    {
        dsvHeap.Free(depthView);
    }
    if (deviceResources != nullptr && hdrRtv)
    {
        deviceResources->rtv_heap().Free(hdrRtv);
    }
    if (deviceResources != nullptr && textureSrvs)
    {
        deviceResources->shader_visible_cbv_srv_uav_heap().Free(textureSrvs);
    }
    depthView = {};
    shadowDepthView = {};
    hdrRtv = {};
    textureSrvs = {};
    imguiFontDescriptor = {};
    dsvHeap = {};
    sphere = {};
    plane = {};
    box = {};
    displayPipeline.Reset();
    lightingPipeline.Reset();
    shadowPipeline.Reset();
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
    return impl_->CreateSizeDependentTargets(drawableSize);
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
    impl_->headlessTest = configuration;
}

} // namespace ch07::shadows::solution
