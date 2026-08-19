#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include "../Common/ChapterGeometry.hpp"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <wrl/client.h>

#include <DirectXMath.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

namespace ch06::surface_frames::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
inline constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
inline constexpr DXGI_FORMAT kTextureFormat = DXGI_FORMAT_R8G8B8A8_TYPELESS;
inline constexpr std::uint32_t kTextureSize = 128U;

enum RootParameter : UINT
{
    LightingRootConstants = 0U,
    ObjectRootConstants = 1U,
    OutputRootConstants = 2U,
    MaterialTextures = 3U,
    HdrTexture = 4U,
};

struct LightingConstants final
{
    DirectX::XMFLOAT4X4 viewProjection{};
    DirectX::XMFLOAT3 cameraPosition{};
    std::uint32_t visualization{};
    DirectX::XMFLOAT3 lightPosition{};
    float intensity{};
    DirectX::XMFLOAT3 directionToLight{};
    std::uint32_t lightType{};
    DirectX::XMFLOAT3 lightColor{1.0F, 1.0F, 1.0F};
    std::uint32_t materialFlags{7U};
    DirectX::XMFLOAT3 overrideBaseColor{0.72F, 0.18F, 0.08F};
    float overrideRoughness{0.45F};
    float overrideMetalness{};
    float normalStrength{1.0F};
    std::uint32_t invertNormalGreen{};
    std::uint32_t overrideNormalSample{};
    std::uint32_t padding{};
    DirectX::XMFLOAT3 overrideNormal{0.5F, 0.5F, 1.0F};
    std::uint32_t padding2{};
};

struct ObjectConstants final
{
    DirectX::XMFLOAT3 translation{};
    float uvScale{1.0F};
    DirectX::XMFLOAT3 baseColor{};
    float roughness{};
    float metalness{};
    DirectX::XMFLOAT3 dielectricF0{0.04F, 0.04F, 0.04F};
    std::uint32_t objectId{};
};

struct OutputConstants final
{
    float exposure{-4.0F};
    std::uint32_t applyHdrDisplayTransform{1U};
    DirectX::XMFLOAT2 padding{};
};

struct MeshBuffers final
{
    lgp::framework::Buffer vertices{};
    lgp::framework::Buffer indices{};
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    UINT indexCount{};
};

struct TexturePixels final
{
    std::vector<std::uint8_t> baseColor;
    std::vector<std::uint8_t> packedMaterial;
    std::vector<std::uint8_t> normal;
};

static_assert(sizeof(LightingConstants) % sizeof(std::uint32_t) == 0U);
static_assert(sizeof(ObjectConstants) % sizeof(std::uint32_t) == 0U);
static_assert(sizeof(OutputConstants) % sizeof(std::uint32_t) == 0U);

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
    return std::filesystem::path{__FILE__}.parent_path() / "SurfaceBaseline.hlsl";
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
lgp::framework::TextureBarrierState constexpr kCopyDestinationState{
    D3D12_BARRIER_SYNC_COPY,
    D3D12_BARRIER_ACCESS_COPY_DEST,
    D3D12_BARRIER_LAYOUT_COPY_DEST,
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

[[nodiscard]] std::uint8_t EncodeUnit(float value) noexcept
{
    return static_cast<std::uint8_t>(std::clamp(std::lround(value * 255.0F), 0L, 255L));
}

[[nodiscard]] bool UsesHdrDisplayTransform(std::uint32_t visualization) noexcept
{
    switch (static_cast<HeadlessVisualization>(visualization))
    {
    case HeadlessVisualization::Uv:
    case HeadlessVisualization::GeometricNormal:
    case HeadlessVisualization::InterpolatedNormal:
    case HeadlessVisualization::MappedNormal:
    case HeadlessVisualization::Tangent:
    case HeadlessVisualization::Bitangent:
    case HeadlessVisualization::Handedness:
    case HeadlessVisualization::BaseColor:
    case HeadlessVisualization::Roughness:
    case HeadlessVisualization::Metalness:
    case HeadlessVisualization::ObjectId:
    case HeadlessVisualization::FiniteValidation:
        return false;
    case HeadlessVisualization::Final:
    case HeadlessVisualization::Diffuse:
    case HeadlessVisualization::Specular:
    default:
        return true;
    }
}

[[nodiscard]] TexturePixels GenerateMaterialTextures()
{
    std::size_t const byteCount = static_cast<std::size_t>(kTextureSize) * kTextureSize * 4U;
    TexturePixels pixels{
        std::vector<std::uint8_t>(byteCount),
        std::vector<std::uint8_t>(byteCount),
        std::vector<std::uint8_t>(byteCount),
    };
    for (std::uint32_t y = 0U; y < kTextureSize; ++y)
    {
        for (std::uint32_t x = 0U; x < kTextureSize; ++x)
        {
            float const u = (static_cast<float>(x) + 0.5F) / static_cast<float>(kTextureSize);
            float const v = (static_cast<float>(y) + 0.5F) / static_cast<float>(kTextureSize);
            bool const checker = (((x / 16U) + (y / 16U)) & 1U) != 0U;
            bool const seamBand = x < 4U || x >= kTextureSize - 4U || std::abs(static_cast<int>(x) - 64) < 3;
            bool const arrow =
                std::abs(v - 0.5F) < (0.035F + 0.22F * std::max(0.0F, u - 0.62F)) && u > 0.14F && u < 0.92F;
            DirectX::XMFLOAT3 color =
                checker ? DirectX::XMFLOAT3{0.78F, 0.16F, 0.055F} : DirectX::XMFLOAT3{0.045F, 0.28F, 0.78F};
            if (arrow)
            {
                color = {0.95F, 0.82F, 0.08F};
            }
            if (seamBand)
            {
                color = {0.05F, 0.9F, 0.28F};
            }
            std::size_t const offset = (static_cast<std::size_t>(y) * kTextureSize + x) * 4U;
            pixels.baseColor[offset + 0U] = EncodeUnit(color.x);
            pixels.baseColor[offset + 1U] = EncodeUnit(color.y);
            pixels.baseColor[offset + 2U] = EncodeUnit(color.z);
            pixels.baseColor[offset + 3U] = 255U;

            float const roughness = 0.08F + (0.84F * u);
            float metalness = 0.5F;
            if (v < 0.48F)
            {
                metalness = 0.0F;
            }
            else if (v > 0.52F)
            {
                metalness = 1.0F;
            }
            pixels.packedMaterial[offset + 0U] = EncodeUnit(roughness);
            pixels.packedMaterial[offset + 1U] = EncodeUnit(metalness);
            pixels.packedMaterial[offset + 2U] = checker ? 255U : 0U;
            pixels.packedMaterial[offset + 3U] = 255U;

            float nx = 0.58F * std::sin(u * 6.28318530718F);
            float ny = 0.52F * std::cos(v * 12.56637061436F);
            if (u > 0.46F && u < 0.54F)
            {
                nx = u < 0.5F ? -0.72F : 0.72F;
            }
            float const nz = std::sqrt(std::max(1.0F - (nx * nx) - (ny * ny), 0.02F));
            float const inverseLength = 1.0F / std::sqrt((nx * nx) + (ny * ny) + (nz * nz));
            pixels.normal[offset + 0U] = EncodeUnit((nx * inverseLength * 0.5F) + 0.5F);
            pixels.normal[offset + 1U] = EncodeUnit((ny * inverseLength * 0.5F) + 0.5F);
            pixels.normal[offset + 2U] = EncodeUnit((nz * inverseLength * 0.5F) + 0.5F);
            pixels.normal[offset + 3U] = 255U;
        }
    }
    return pixels;
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
    lgp::framework::DescriptorHeap dsvHeap{};
    lgp::framework::DescriptorAllocation depthView{};
    lgp::framework::DescriptorAllocation hdrRtv{};
    lgp::framework::DescriptorAllocation hdrSrv{};
    lgp::framework::DescriptorAllocation materialSrvs{};
    lgp::framework::DescriptorAllocation imguiFontDescriptor{};
    ComPtr<ID3D12Resource> depthTarget{};
    ComPtr<ID3D12Resource> hdrTarget{};
    std::array<ComPtr<ID3D12Resource>, 3U> materialTextures{};
    LightingConstants lighting{};
    OutputConstants output{};
    DirectX::XMFLOAT3 lightDirection{-0.45F, 0.82F, -0.35F};
    float directionalLux{1200.0F};
    float pointCandela{1800.0F};
    float lightDistance{7.0F};
    float orbitAzimuth{};
    float orbitElevation{0.28F};
    float orbitRadius{15.5F};
    bool useBaseColorTexture{true};
    bool usePackedMaterialTexture{true};
    bool useNormalTexture{true};
    bool invertNormalGreen{};
    bool sampleBaseColorAsLinear{};
    bool materialOverride{};
    bool headless{};
    bool imguiInitialized{};
    bool imguiFrameBegun{};
    std::optional<HeadlessTestConfiguration> headlessTest{};

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context);
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelineStates();
    [[nodiscard]] lgp::framework::Status CreateMeshBuffers(GeometryMesh const &mesh, std::wstring_view name,
                                                           MeshBuffers &buffers);
    [[nodiscard]] lgp::framework::Status CreateMaterialTextures();
    [[nodiscard]] lgp::framework::Status CreateSizeDependentTargets(lgp::framework::Extent2D size);
    [[nodiscard]] lgp::framework::Status InitializeImGui();
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context);
    [[nodiscard]] lgp::framework::Status BuildUi(lgp::framework::UpdateContext const &context);
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
    D3D12_ROOT_PARAMETER parameters[5]{};
    parameters[LightingRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[LightingRootConstants].Constants.ShaderRegister = 0U;
    parameters[LightingRootConstants].Constants.Num32BitValues = sizeof(LightingConstants) / sizeof(std::uint32_t);
    parameters[LightingRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[ObjectRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[ObjectRootConstants].Constants.ShaderRegister = 1U;
    parameters[ObjectRootConstants].Constants.Num32BitValues = sizeof(ObjectConstants) / sizeof(std::uint32_t);
    parameters[ObjectRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[OutputRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[OutputRootConstants].Constants.ShaderRegister = 2U;
    parameters[OutputRootConstants].Constants.Num32BitValues = sizeof(OutputConstants) / sizeof(std::uint32_t);
    parameters[OutputRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE materialRange{};
    materialRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    materialRange.NumDescriptors = 4U;
    materialRange.BaseShaderRegister = 0U;
    materialRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    parameters[MaterialTextures].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[MaterialTextures].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[MaterialTextures].DescriptorTable.pDescriptorRanges = &materialRange;
    parameters[MaterialTextures].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE hdrRange{};
    hdrRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    hdrRange.NumDescriptors = 1U;
    hdrRange.BaseShaderRegister = 4U;
    hdrRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    parameters[HdrTexture].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[HdrTexture].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[HdrTexture].DescriptorTable.pDescriptorRanges = &hdrRange;
    parameters[HdrTexture].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    std::array<D3D12_STATIC_SAMPLER_DESC, 2U> samplers{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1] = samplers[0];
    samplers[1].ShaderRegister = 1U;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
    description.pParameters = parameters;
    description.NumStaticSamplers = static_cast<UINT>(samplers.size());
    description.pStaticSamplers = samplers.data();
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
        {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 24U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TANGENT", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 32U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
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
        *deviceResources->device(), mesh.vertices.size() * sizeof(SurfaceVertex), std::wstring{name} + L" vertices");
    if (!vertexResult)
    {
        return std::unexpected(std::move(vertexResult.error()));
    }
    buffers.vertices = std::move(vertexResult.value());
    if (auto status = lgp::framework::WriteBuffer(
            buffers.vertices, std::span<SurfaceVertex const>{mesh.vertices.data(), mesh.vertices.size()});
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
    buffers.vertexView.SizeInBytes = static_cast<UINT>(mesh.vertices.size() * sizeof(SurfaceVertex));
    buffers.vertexView.StrideInBytes = sizeof(SurfaceVertex);
    buffers.indexView.BufferLocation = buffers.indices.gpu_virtual_address();
    buffers.indexView.SizeInBytes = static_cast<UINT>(mesh.indices.size() * sizeof(std::uint32_t));
    buffers.indexView.Format = DXGI_FORMAT_R32_UINT;
    buffers.indexCount = static_cast<UINT>(mesh.indices.size());
    return {};
}

lgp::framework::Status Renderer::Impl::CreateMaterialTextures()
{
    TexturePixels const pixels = GenerateMaterialTextures();
    std::array<std::vector<std::uint8_t> const *, 3U> const sources{
        &pixels.baseColor,
        &pixels.packedMaterial,
        &pixels.normal,
    };
    std::array<wchar_t const *, 3U> const names{
        L"Ch06 generated base color",
        L"Ch06 generated roughness metalness",
        L"Ch06 generated tangent-space normal",
    };
    std::array<lgp::framework::Buffer, 3U> uploads{};
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 3U> footprints{};

    ID3D12Device10 *const device = deviceResources->device();
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC1 textureDescription{};
    textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDescription.Width = kTextureSize;
    textureDescription.Height = kTextureSize;
    textureDescription.DepthOrArraySize = 1U;
    textureDescription.MipLevels = 1U;
    textureDescription.Format = kTextureFormat;
    textureDescription.SampleDesc.Count = 1U;
    textureDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    for (std::size_t textureIndex = 0U; textureIndex < materialTextures.size(); ++textureIndex)
    {
        HRESULT const textureResult = device->CreateCommittedResource3(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDescription, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr,
            0U, nullptr, IID_PPV_ARGS(materialTextures[textureIndex].ReleaseAndGetAddressOf()));
        if (FAILED(textureResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3",
                                                                    textureResult,
                                                                    "Failed to create a generated material texture."));
        }
        materialTextures[textureIndex]->SetName(names[textureIndex]);
        UINT rowCount{};
        UINT64 rowSize{};
        UINT64 uploadSize{};
        device->GetCopyableFootprints1(&textureDescription, 0U, 1U, 0U, &footprints[textureIndex], &rowCount, &rowSize,
                                       &uploadSize);
        auto uploadResult = lgp::framework::CreateUploadBuffer(*device, uploadSize, L"Ch06 material texture upload");
        if (!uploadResult)
        {
            return std::unexpected(std::move(uploadResult.error()));
        }
        uploads[textureIndex] = std::move(uploadResult.value());
        std::size_t const sourceRowBytes = static_cast<std::size_t>(kTextureSize) * 4U;
        for (std::uint32_t row = 0U; row < kTextureSize; ++row)
        {
            std::byte *const destination =
                uploads[textureIndex].mapped_data() + footprints[textureIndex].Offset +
                (static_cast<std::size_t>(row) * footprints[textureIndex].Footprint.RowPitch);
            std::uint8_t const *const source =
                sources[textureIndex]->data() + (static_cast<std::size_t>(row) * sourceRowBytes);
            std::memcpy(destination, source, sourceRowBytes);
        }
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    HRESULT const allocatorResult = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                   IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf()));
    if (FAILED(allocatorResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandAllocator", allocatorResult,
                                                                "Failed to create the material upload allocator."));
    }
    ComPtr<ID3D12GraphicsCommandList7> commandList;
    HRESULT const listResult = device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                         IID_PPV_ARGS(commandList.ReleaseAndGetAddressOf()));
    if (FAILED(listResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandList", listResult,
                                                                "Failed to create the material upload command list."));
    }
    for (std::size_t textureIndex = 0U; textureIndex < materialTextures.size(); ++textureIndex)
    {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = materialTextures[textureIndex].Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = uploads[textureIndex].resource();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprints[textureIndex];
        commandList->CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);
        lgp::framework::TransitionTexture(*commandList.Get(), *materialTextures[textureIndex].Get(),
                                          kCopyDestinationState, kShaderResourceState);
    }
    HRESULT const closeResult = commandList->Close();
    if (FAILED(closeResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12GraphicsCommandList::Close", closeResult,
                                                                "Failed to close the material upload command list."));
    }
    ID3D12CommandList *const lists[]{commandList.Get()};
    deviceResources->graphics_queue()->ExecuteCommandLists(1U, lists);
    if (auto idleStatus = deviceResources->WaitForGpuIdle(); !idleStatus)
    {
        return idleStatus;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1U;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    device->CreateShaderResourceView(materialTextures[0].Get(), &srv, materialSrvs.CpuHandle(0U));
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateShaderResourceView(materialTextures[1].Get(), &srv, materialSrvs.CpuHandle(1U));
    device->CreateShaderResourceView(materialTextures[2].Get(), &srv, materialSrvs.CpuHandle(2U));
    device->CreateShaderResourceView(materialTextures[0].Get(), &srv, materialSrvs.CpuHandle(3U));
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
        return std::unexpected(lgp::framework::MakeError("ImGui_ImplDX12_Init", "Failed to initialize the lab UI."));
    }
    imguiInitialized = true;
    return {};
}

lgp::framework::Status Renderer::Impl::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources = &context.deviceResources;
    headless = context.commandLine.headless;
    auto sphereResult = GenerateUvSphere(0.72F, 24U, 48U);
    auto planeResult = GenerateGroundPlane(18.0F, 14.0F);
    if (!sphereResult || !planeResult)
    {
        return std::unexpected(lgp::framework::MakeError("GenerateGeometry", "Could not create chapter geometry."));
    }
    auto dsvResult = lgp::framework::CreateDescriptorHeap(*deviceResources->device(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                                          1U, false, L"Ch06 depth descriptors");
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
    auto materialAllocation = deviceResources->shader_visible_cbv_srv_uav_heap().Allocate(4U);
    if (!materialAllocation)
    {
        return std::unexpected(std::move(materialAllocation.error()));
    }
    materialSrvs = materialAllocation.value();

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
    if (auto status = CreateMeshBuffers(*sphereResult, L"Ch06 sphere", sphere); !status)
    {
        return status;
    }
    if (auto status = CreateMeshBuffers(*planeResult, L"Ch06 plane", plane); !status)
    {
        return status;
    }
    if (auto status = CreateMaterialTextures(); !status)
    {
        return status;
    }
    if (auto status = CreateSizeDependentTargets(context.drawableSize); !status)
    {
        return status;
    }
    if (!headless)
    {
        return InitializeImGui();
    }
    return {};
}

lgp::framework::Status Renderer::Impl::Update(lgp::framework::UpdateContext const &context)
{
    if (headless && headlessTest.has_value())
    {
        HeadlessTestConfiguration const &test = *headlessTest;
        orbitAzimuth = test.cameraAzimuth;
        orbitElevation = std::clamp(test.cameraElevation, -0.15F, 1.15F);
        orbitRadius = std::clamp(test.cameraDistance, 8.0F, 24.0F);
    }
    if (!headless && context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Right))
    {
        orbitAzimuth += static_cast<float>(context.input.mouse.deltaX) * 0.006F;
        orbitElevation =
            std::clamp(orbitElevation - (static_cast<float>(context.input.mouse.deltaY) * 0.006F), -0.15F, 1.15F);
    }
    if (!headless)
    {
        orbitRadius = std::clamp(orbitRadius - context.input.mouse.wheelDelta, 8.0F, 24.0F);
    }
    float const horizontalRadius = orbitRadius * std::cos(orbitElevation);
    DirectX::XMFLOAT3 const cameraPosition{
        horizontalRadius * std::sin(orbitAzimuth),
        1.4F + (orbitRadius * std::sin(orbitElevation)),
        -horizontalRadius * std::cos(orbitAzimuth),
    };
    DirectX::XMVECTOR const eye = DirectX::XMLoadFloat3(&cameraPosition);
    DirectX::XMVECTOR const target = DirectX::XMVectorSet(0.0F, 1.15F, 0.3F, 1.0F);
    DirectX::XMMATRIX const view = DirectX::XMMatrixLookAtLH(eye, target, DirectX::XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F));
    float const aspect = context.drawableSize.height == 0U ? 1.0F
                                                           : static_cast<float>(context.drawableSize.width) /
                                                                 static_cast<float>(context.drawableSize.height);
    DirectX::XMMATRIX const projection = DirectX::XMMatrixPerspectiveFovLH(0.78F, aspect, 0.1F, 100.0F);
    DirectX::XMStoreFloat4x4(&lighting.viewProjection, view * projection);
    lighting.cameraPosition = cameraPosition;
    lighting.directionToLight = NormalizeDirection(lightDirection);
    lighting.intensity = lighting.lightType == 0U ? directionalLux : pointCandela;
    lighting.lightPosition = {
        lighting.directionToLight.x * lightDistance,
        1.2F + (lighting.directionToLight.y * lightDistance),
        lighting.directionToLight.z * lightDistance,
    };
    lighting.materialFlags = (useBaseColorTexture ? 1U : 0U) | (usePackedMaterialTexture ? 2U : 0U) |
                             (useNormalTexture ? 4U : 0U) | (sampleBaseColorAsLinear ? 8U : 0U) |
                             (materialOverride ? 16U : 0U);
    lighting.invertNormalGreen = invertNormalGreen ? 1U : 0U;
    lighting.normalStrength = std::clamp(lighting.normalStrength, 0.0F, 1.0F);
    if (headless && headlessTest.has_value())
    {
        HeadlessTestConfiguration const &test = *headlessTest;
        lighting.visualization = static_cast<std::uint32_t>(test.visualization);
        lighting.materialFlags = (test.useBaseColorTexture ? 1U : 0U) | (test.usePackedMaterialTexture ? 2U : 0U) |
                                 (test.useNormalTexture ? 4U : 0U) | (test.sampleBaseColorAsLinear ? 8U : 0U) |
                                 (test.overrideMaterial ? 16U : 0U);
        lighting.invertNormalGreen = test.invertNormalGreen ? 1U : 0U;
        lighting.overrideNormalSample = test.overrideNormalSample ? 1U : 0U;
        lighting.normalStrength = std::clamp(test.normalStrength, 0.0F, 1.0F);
        lighting.overrideNormal = {
            std::clamp(test.normalSampleR, 0.0F, 1.0F),
            std::clamp(test.normalSampleG, 0.0F, 1.0F),
            std::clamp(test.normalSampleB, 0.0F, 1.0F),
        };
        lighting.overrideBaseColor = {
            std::clamp(test.baseColorR, 0.0F, 1.0F),
            std::clamp(test.baseColorG, 0.0F, 1.0F),
            std::clamp(test.baseColorB, 0.0F, 1.0F),
        };
        lighting.overrideRoughness = std::clamp(test.roughness, 0.0F, 1.0F);
        lighting.overrideMetalness = std::clamp(test.metalness, 0.0F, 1.0F);
        lighting.directionToLight =
            NormalizeDirection({test.directionToLightX, test.directionToLightY, test.directionToLightZ});
        lighting.intensity = std::max(test.lightIntensity, 0.0F);
        output.exposure = test.exposure;
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

    ImGui::Begin("Surface frames and material textures");
    ImGui::Checkbox("Base-color texture", &useBaseColorTexture);
    ImGui::Checkbox("Roughness / metalness texture", &usePackedMaterialTexture);
    ImGui::Checkbox("Normal texture", &useNormalTexture);
    ImGui::SliderFloat("Normal strength", &lighting.normalStrength, 0.0F, 1.0F, "%.2f");
    ImGui::Checkbox("Invert normal green channel", &invertNormalGreen);
    ImGui::Checkbox("Interpret base-color bytes as linear", &sampleBaseColorAsLinear);
    ImGui::Separator();
    ImGui::Checkbox("Override sampled material", &materialOverride);
    ImGui::ColorEdit3("Override base color", &lighting.overrideBaseColor.x);
    ImGui::SliderFloat("Override roughness", &lighting.overrideRoughness, 0.0F, 1.0F);
    ImGui::SliderFloat("Override metalness", &lighting.overrideMetalness, 0.0F, 1.0F);
    ImGui::Separator();
    int lightType = static_cast<int>(lighting.lightType);
    ImGui::Combo("Light type", &lightType, "Directional\0Point\0");
    lighting.lightType = static_cast<std::uint32_t>(lightType);
    if (lighting.lightType == 0U)
    {
        ImGui::SliderFloat("Normal illuminance", &directionalLux, 0.0F, 10000.0F, "%.0f lux");
    }
    else
    {
        ImGui::SliderFloat("Luminous intensity", &pointCandela, 0.0F, 20000.0F, "%.0f cd");
        ImGui::SliderFloat("Light distance", &lightDistance, 2.0F, 15.0F, "%.2f m");
    }
    ImGui::SliderFloat3("Direction to light", &lightDirection.x, -1.0F, 1.0F);
    ImGui::ColorEdit3("Light tint", &lighting.lightColor.x);
    ImGui::SliderFloat("Exposure", &output.exposure, -10.0F, 4.0F, "%.2f EV");
    ImGui::SliderAngle("Camera azimuth", &orbitAzimuth, -180.0F, 180.0F);
    ImGui::SliderAngle("Camera elevation", &orbitElevation, -8.0F, 66.0F);
    ImGui::SliderFloat("Camera distance", &orbitRadius, 8.0F, 24.0F, "%.2f m");
    int visualization = static_cast<int>(lighting.visualization);
    ImGui::Combo("Debug output", &visualization,
                 "Final shading\0UV (R=U, G=V)\0Geometric normal (-1..1 encoded 0..1)\0Interpolated normal "
                 "(-1..1 encoded 0..1)\0Mapped normal (-1..1 encoded 0..1)\0Tangent (-1..1 encoded "
                 "0..1)\0Bitangent (-1..1 encoded 0..1)\0Handedness (red=-1, blue=+1)\0Base color (linear "
                 "0..1)\0Roughness (0..1)\0Metalness (0..1)\0Diffuse HDR\0Specular HDR\0");
    lighting.visualization = static_cast<std::uint32_t>(visualization);
    ImGui::TextWrapped("The plane and every sphere contain mirrored UV islands. Green seam bands and the handedness "
                       "view make the split explicit; correct tangent.w handling keeps mapped bumps continuous.");
    ImGui::TextWrapped("Right-drag orbits the camera; the wheel dollies.");
    ImGui::End();
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
    commandList.SetGraphicsRootDescriptorTable(MaterialTextures, materialSrvs.gpuHandle);

    ObjectConstants object{};
    object.translation = {0.0F, 0.0F, 0.5F};
    object.uvScale = 2.0F;
    object.baseColor = {0.32F, 0.34F, 0.38F};
    object.roughness = 0.62F;
    object.metalness = 0.0F;
    object.dielectricF0 = {0.04F, 0.04F, 0.04F};
    object.objectId = 1U;
    HeadlessScene const scene = headless && headlessTest.has_value() ? headlessTest->scene : HeadlessScene::Full;
    if (scene == HeadlessScene::Full || scene == HeadlessScene::GroundOnly)
    {
        DrawMesh(commandList, plane, object);
    }
    object.translation = {-1.9F, 0.75F, 0.5F};
    object.uvScale = 1.0F;
    object.baseColor = {0.72F, 0.12F, 0.055F};
    object.roughness = 0.28F;
    object.metalness = 0.0F;
    object.objectId = 2U;
    if (scene == HeadlessScene::Full)
    {
        DrawMesh(commandList, sphere, object);
    }

    object.translation = {0.0F, 0.75F, 0.5F};
    object.baseColor = {0.95F, 0.64F, 0.18F};
    object.roughness = 0.22F;
    object.metalness = 1.0F;
    object.objectId = 3U;
    if (scene == HeadlessScene::Full || scene == HeadlessScene::CenterSphereOnly)
    {
        DrawMesh(commandList, sphere, object);
    }

    object.translation = {1.9F, 0.75F, 0.5F};
    object.baseColor = {0.08F, 0.32F, 0.82F};
    object.roughness = 0.68F;
    object.metalness = 0.0F;
    object.objectId = 4U;
    if (scene == HeadlessScene::Full)
    {
        DrawMesh(commandList, sphere, object);
    }

    lgp::framework::TransitionTexture(commandList, *hdrTarget.Get(), kRenderTargetState, kShaderResourceState);

    lgp::framework::TransitionTexture(commandList, *frameContext.renderTarget, FrameStartState(frameContext),
                                      kRenderTargetState);
    float const displayClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.ClearRenderTargetView(frameContext.renderTargetView, displayClear, 0U, nullptr);
    commandList.SetPipelineState(displayPipeline.Get());
    output.applyHdrDisplayTransform = UsesHdrDisplayTransform(lighting.visualization) ? 1U : 0U;
    commandList.SetGraphicsRoot32BitConstants(OutputRootConstants, sizeof(OutputConstants) / sizeof(std::uint32_t),
                                              &output, 0U);
    commandList.SetGraphicsRootDescriptorTable(HdrTexture, hdrSrv.gpuHandle);
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
    for (ComPtr<ID3D12Resource> &texture : materialTextures)
    {
        texture.Reset();
    }
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
    if (deviceResources != nullptr && materialSrvs)
    {
        deviceResources->shader_visible_cbv_srv_uav_heap().Free(materialSrvs);
    }
    depthView = {};
    hdrRtv = {};
    hdrSrv = {};
    materialSrvs = {};
    imguiFontDescriptor = {};
    dsvHeap = {};
    sphere = {};
    plane = {};
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
    impl_->headlessTest = configuration;
}

} // namespace ch06::surface_frames::solution
