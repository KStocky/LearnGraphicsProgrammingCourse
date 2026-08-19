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

namespace ch07::shadows::starter
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
    HdrTexture = 2U,
};

struct LightingConstants final
{
    DirectX::XMFLOAT4X4 viewProjection{};
    DirectX::XMFLOAT3 cameraPosition{};
    float exposure{-4.0F};
    DirectX::XMFLOAT3 directionToLight{};
    float intensity{1200.0F};
};

struct ObjectConstants final
{
    DirectX::XMFLOAT4X4 world{};
    std::array<DirectX::XMFLOAT4, 3U> normalColumns{};
    DirectX::XMFLOAT3 baseColor{};
    float roughness{};
    float metalness{};
    DirectX::XMFLOAT3 dielectricF0{0.04F, 0.04F, 0.04F};
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

[[nodiscard]] ObjectConstants MakeObject(DirectX::XMMATRIX const &world, DirectX::XMFLOAT3 baseColor, float roughness,
                                         float metalness) noexcept
{
    ObjectConstants object{};
    DirectX::XMStoreFloat4x4(&object.world, world);
    DirectX::XMVECTOR determinant{};
    DirectX::XMMATRIX const normalWorld = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(&determinant, world));
    DirectX::XMFLOAT4X4 normalMatrix{};
    DirectX::XMStoreFloat4x4(&normalMatrix, normalWorld);
    object.normalColumns = {
        DirectX::XMFLOAT4{normalMatrix._11, normalMatrix._21, normalMatrix._31, 0.0F},
        DirectX::XMFLOAT4{normalMatrix._12, normalMatrix._22, normalMatrix._32, 0.0F},
        DirectX::XMFLOAT4{normalMatrix._13, normalMatrix._23, normalMatrix._33, 0.0F},
    };
    object.baseColor = baseColor;
    object.roughness = roughness;
    object.metalness = metalness;
    object.dielectricF0 = {0.04F, 0.04F, 0.04F};
    return object;
}

} // namespace

class Renderer::Impl final
{
  public:
    lgp::framework::DeviceResources *deviceResources{};
    lgp::framework::CompiledShader lightingVertexShader{};
    lgp::framework::CompiledShader lightingPixelShader{};
    lgp::framework::CompiledShader fullscreenVertexShader{};
    lgp::framework::CompiledShader displayPixelShader{};
    ComPtr<ID3D12RootSignature> rootSignature{};
    ComPtr<ID3D12PipelineState> lightingPipeline{};
    ComPtr<ID3D12PipelineState> displayPipeline{};
    MeshBuffers sphere{};
    MeshBuffers plane{};
    MeshBuffers box{};
    lgp::framework::DescriptorHeap dsvHeap{};
    lgp::framework::DescriptorAllocation depthView{};
    lgp::framework::DescriptorAllocation hdrRtv{};
    lgp::framework::DescriptorAllocation hdrSrv{};
    ComPtr<ID3D12Resource> depthTarget{};
    ComPtr<ID3D12Resource> hdrTarget{};
    LightingConstants lighting{};
    DirectX::XMFLOAT3 lightDirection{-0.45F, 0.82F, -0.35F};
    float orbitAzimuth{};
    float orbitElevation{0.34F};
    float orbitRadius{20.0F};
    bool headless{};
    std::optional<HeadlessTestConfiguration> headlessTest{};

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context);
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelineStates();
    [[nodiscard]] lgp::framework::Status CreateMeshBuffers(GeometryMesh const &mesh, std::wstring_view name,
                                                           MeshBuffers &buffers);
    [[nodiscard]] lgp::framework::Status CreateSizeDependentTargets(lgp::framework::Extent2D size);
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context);
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext);
    void DrawMesh(ID3D12GraphicsCommandList &commandList, MeshBuffers const &mesh,
                  ObjectConstants const &object) const noexcept;
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
    range.NumDescriptors = 1U;
    range.BaseShaderRegister = 0U;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    parameters[HdrTexture].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[HdrTexture].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[HdrTexture].DescriptorTable.pDescriptorRanges = &range;
    parameters[HdrTexture].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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
    deviceResources->device()->CreateShaderResourceView(hdrTarget.Get(), &srv, hdrSrv.cpuHandle);

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
                                                          1U, false, L"Ch07 depth descriptors");
    if (!dsvResult)
    {
        return std::unexpected(std::move(dsvResult.error()));
    }
    dsvHeap = std::move(dsvResult.value());
    auto depthAllocation = dsvHeap.Allocate(1U);
    if (!depthAllocation)
    {
        return std::unexpected(std::move(depthAllocation.error()));
    }
    depthView = depthAllocation.value();
    auto rtvAllocation = deviceResources->rtv_heap().Allocate(1U);
    if (!rtvAllocation)
    {
        return std::unexpected(std::move(rtvAllocation.error()));
    }
    hdrRtv = rtvAllocation.value();
    auto srvAllocation = deviceResources->shader_visible_cbv_srv_uav_heap().Allocate(1U);
    if (!srvAllocation)
    {
        return std::unexpected(std::move(srvAllocation.error()));
    }
    hdrSrv = srvAllocation.value();

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
    return {};
}

lgp::framework::Status Renderer::Impl::Update(lgp::framework::UpdateContext const &context)
{
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
    lighting.intensity = 1200.0F;
    if (headlessTest)
    {
        lighting.directionToLight = NormalizeDirection(
            {headlessTest->directionToLight.x, headlessTest->directionToLight.y, headlessTest->directionToLight.z});
        lighting.intensity = headlessTest->illuminanceLux;
        lighting.exposure = headlessTest->exposure;
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

lgp::framework::Status Renderer::Impl::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr || hdrTarget == nullptr ||
        depthTarget == nullptr)
    {
        return {};
    }
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.SetGraphicsRootSignature(rootSignature.Get());

    lgp::framework::TransitionTexture(commandList, *hdrTarget.Get(), kShaderResourceState, kRenderTargetState);
    float const hdrClear[]{0.012F, 0.018F, 0.028F, 1.0F};
    commandList.OMSetRenderTargets(1U, &hdrRtv.cpuHandle, FALSE, &depthView.cpuHandle);
    commandList.ClearRenderTargetView(hdrRtv.cpuHandle, hdrClear, 0U, nullptr);
    commandList.ClearDepthStencilView(depthView.cpuHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0U, 0U, nullptr);
    commandList.SetPipelineState(lightingPipeline.Get());
    commandList.SetGraphicsRoot32BitConstants(LightingRootConstants, sizeof(LightingConstants) / sizeof(std::uint32_t),
                                              &lighting, 0U);

    ObjectConstants object = MakeObject(DirectX::XMMatrixIdentity(), {0.32F, 0.34F, 0.38F}, 0.72F, 0.0F);
    DrawMesh(commandList, plane, object);

    object = MakeObject(DirectX::XMMatrixScaling(2.2F, 1.5F, 2.0F) * DirectX::XMMatrixTranslation(-3.2F, 0.75F, 1.8F),
                        {0.72F, 0.12F, 0.055F}, 0.38F, 0.0F);
    DrawMesh(commandList, box, object);

    object = MakeObject(DirectX::XMMatrixTranslation(-0.5F, 0.72F, 1.2F), {0.95F, 0.64F, 0.18F}, 0.22F, 1.0F);
    DrawMesh(commandList, sphere, object);

    object = MakeObject(DirectX::XMMatrixTranslation(2.1F, 0.72F, -1.1F), {0.08F, 0.32F, 0.82F}, 0.68F, 0.0F);
    DrawMesh(commandList, sphere, object);

    object = MakeObject(DirectX::XMMatrixScaling(0.16F, 3.2F, 2.4F) * DirectX::XMMatrixTranslation(1.1F, 1.6F, 2.8F),
                        {0.16F, 0.68F, 0.24F}, 0.5F, 0.0F);
    DrawMesh(commandList, box, object);

    object = MakeObject(DirectX::XMMatrixScaling(3.6F, 0.24F, 1.4F) * DirectX::XMMatrixRotationZ(0.42F) *
                            DirectX::XMMatrixTranslation(-1.0F, 0.9F, -2.4F),
                        {0.62F, 0.18F, 0.72F}, 0.58F, 0.0F);
    DrawMesh(commandList, box, object);

    object = MakeObject(DirectX::XMMatrixScaling(2.8F, 0.12F, 2.2F) * DirectX::XMMatrixRotationX(0.16F) *
                            DirectX::XMMatrixTranslation(3.4F, 0.34F, 3.7F),
                        {0.72F, 0.65F, 0.18F}, 0.84F, 0.0F);
    DrawMesh(commandList, box, object);

    object = MakeObject(DirectX::XMMatrixScaling(1.0F, 1.6F, 1.0F) * DirectX::XMMatrixTranslation(7.7F, 0.8F, 0.4F),
                        {0.08F, 0.62F, 0.68F}, 0.42F, 0.0F);
    DrawMesh(commandList, box, object);

    lgp::framework::TransitionTexture(commandList, *hdrTarget.Get(), kRenderTargetState, kShaderResourceState);

    lgp::framework::TransitionTexture(commandList, *frameContext.renderTarget, FrameStartState(frameContext),
                                      kRenderTargetState);
    float const displayClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.ClearRenderTargetView(frameContext.renderTargetView, displayClear, 0U, nullptr);
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.SetPipelineState(displayPipeline.Get());
    commandList.SetGraphicsRoot32BitConstants(LightingRootConstants, sizeof(LightingConstants) / sizeof(std::uint32_t),
                                              &lighting, 0U);
    commandList.SetGraphicsRootDescriptorTable(HdrTexture, hdrSrv.gpuHandle);
    commandList.DrawInstanced(3U, 1U, 0U, 0U);
    lgp::framework::TransitionTexture(commandList, *frameContext.renderTarget, kRenderTargetState,
                                      FrameEndState(frameContext));
    return {};
}

void Renderer::Impl::Shutdown() noexcept
{
    depthTarget.Reset();
    hdrTarget.Reset();
    if (depthView)
    {
        dsvHeap.Free(depthView);
    }
    if (deviceResources != nullptr && hdrRtv)
    {
        deviceResources->rtv_heap().Free(hdrRtv);
    }
    if (deviceResources != nullptr && hdrSrv)
    {
        deviceResources->shader_visible_cbv_srv_uav_heap().Free(hdrSrv);
    }
    depthView = {};
    hdrRtv = {};
    hdrSrv = {};
    dsvHeap = {};
    sphere = {};
    plane = {};
    box = {};
    displayPipeline.Reset();
    lightingPipeline.Reset();
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

} // namespace ch07::shadows::starter
