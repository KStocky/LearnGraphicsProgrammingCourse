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
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <lgp/framework/buffer.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

namespace ch03::texture::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr std::uint32_t kTextureSize = 64U;
inline constexpr DXGI_FORMAT kTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

struct Vertex final
{
    DirectX::XMFLOAT4 position{};
    DirectX::XMFLOAT2 uv{};
};

struct MipImage final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba8{};
};

[[nodiscard]] std::filesystem::path ResolveShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "TextureSampling.hlsl";
}

[[nodiscard]] std::array<float, 3U> MakeTint(std::uint32_t level) noexcept
{
    constexpr std::array<std::array<float, 3U>, 7U> kTints{{
        {1.00F, 1.00F, 1.00F},
        {1.00F, 0.70F, 0.70F},
        {0.70F, 1.00F, 0.70F},
        {0.70F, 0.80F, 1.00F},
        {1.00F, 0.85F, 0.45F},
        {0.85F, 0.55F, 1.00F},
        {0.45F, 1.00F, 1.00F},
    }};
    return kTints[(std::min)(level, static_cast<std::uint32_t>(kTints.size() - 1U))];
}

[[nodiscard]] MipImage MakeBaseTexture()
{
    MipImage image{kTextureSize, kTextureSize, std::vector<std::uint8_t>(kTextureSize * kTextureSize * 4U)};
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            bool const checker = (((x / 4U) + (y / 4U)) & 1U) != 0U;
            bool const stripe = ((x ^ (y * 3U)) & 8U) != 0U;
            std::size_t const offset = (static_cast<std::size_t>(y) * image.width + x) * 4U;
            image.rgba8[offset + 0U] = checker ? 235U : 24U;
            image.rgba8[offset + 1U] = stripe ? 220U : 38U;
            image.rgba8[offset + 2U] = checker == stripe ? 245U : 32U;
            image.rgba8[offset + 3U] = 255U;
        }
    }
    return image;
}

[[nodiscard]] std::vector<MipImage> BuildMipChain()
{
    std::vector<MipImage> mips;
    mips.push_back(MakeBaseTexture());

    for (std::uint32_t level = 1U; mips.back().width > 1U || mips.back().height > 1U; ++level)
    {
        MipImage const &previous = mips.back();
        MipImage next{
            (std::max)(1U, previous.width / 2U), (std::max)(1U, previous.height / 2U),
            std::vector<std::uint8_t>((std::max)(1U, previous.width / 2U) * (std::max)(1U, previous.height / 2U) * 4U)};
        auto const tint = MakeTint(level);

        for (std::uint32_t y = 0; y < next.height; ++y)
        {
            for (std::uint32_t x = 0; x < next.width; ++x)
            {
                std::array<std::uint32_t, 4U> sum{};
                for (std::uint32_t oy = 0; oy < 2U; ++oy)
                {
                    for (std::uint32_t ox = 0; ox < 2U; ++ox)
                    {
                        std::uint32_t const sx = (std::min)(previous.width - 1U, (x * 2U) + ox);
                        std::uint32_t const sy = (std::min)(previous.height - 1U, (y * 2U) + oy);
                        std::size_t const source = (static_cast<std::size_t>(sy) * previous.width + sx) * 4U;
                        for (std::size_t channel = 0; channel < 4U; ++channel)
                        {
                            sum[channel] += previous.rgba8[source + channel];
                        }
                    }
                }

                std::size_t const destination = (static_cast<std::size_t>(y) * next.width + x) * 4U;
                for (std::size_t channel = 0; channel < 3U; ++channel)
                {
                    float const averaged = static_cast<float>(sum[channel] / 4U);
                    next.rgba8[destination + channel] =
                        static_cast<std::uint8_t>(std::clamp(averaged * tint[channel], 0.0F, 255.0F));
                }
                next.rgba8[destination + 3U] = 255U;
            }
        }
        mips.push_back(std::move(next));
    }
    return mips;
}

[[nodiscard]] std::array<Vertex, 6U> MakeVertices()
{
    constexpr float nearW = 0.70F;
    constexpr float farW = 2.85F;
    Vertex const bottomLeft{{-0.92F * nearW, -0.76F * nearW, 0.35F * nearW, nearW}, {0.0F, 18.0F}};
    Vertex const bottomRight{{0.92F * nearW, -0.76F * nearW, 0.35F * nearW, nearW}, {18.0F, 18.0F}};
    Vertex const topLeft{{-0.33F * farW, 0.68F * farW, 0.35F * farW, farW}, {0.0F, 0.0F}};
    Vertex const topRight{{0.33F * farW, 0.68F * farW, 0.35F * farW, farW}, {18.0F, 0.0F}};
    return {bottomLeft, bottomRight, topRight, bottomLeft, topRight, topLeft};
}

[[nodiscard]] D3D12_STATIC_SAMPLER_DESC MakeStaticSampler(D3D12_FILTER filter, UINT shaderRegister,
                                                          UINT maxAnisotropy = 1U) noexcept
{
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = filter;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0.0F;
    sampler.MaxAnisotropy = maxAnisotropy;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0F;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = shaderRegister;
    sampler.RegisterSpace = 0U;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    return sampler;
}

} // namespace

class Renderer::Impl final
{
  public:
    lgp::framework::DeviceResources *deviceResources{};
    lgp::framework::CompiledShader vertexShader{};
    lgp::framework::CompiledShader pixelShader{};
    ComPtr<ID3D12RootSignature> rootSignature{};
    ComPtr<ID3D12PipelineState> pipelineState{};
    lgp::framework::Buffer vertexBuffer{};
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
    ComPtr<ID3D12Resource> texture{};
    lgp::framework::DescriptorAllocation srv{};
    lgp::framework::DescriptorAllocation imguiFontDescriptor{};
    SamplerMode samplerMode{SamplerMode::Anisotropic};
    float lodBias{};
    bool visualizeMip{};
    bool headless{};
    bool imguiInitialized{};
    bool imguiFrameBegun{};

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context);
    [[nodiscard]] lgp::framework::Status CompileShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature(ID3D12Device &device);
    [[nodiscard]] lgp::framework::Status CreatePipelineState(ID3D12Device &device, DXGI_FORMAT renderTargetFormat);
    [[nodiscard]] lgp::framework::Status CreateVertexBuffer(ID3D12Device &device);
    [[nodiscard]] lgp::framework::Status CreateTexture(lgp::framework::DeviceResources &resources);
    [[nodiscard]] lgp::framework::Status InitializeImGui();
    [[nodiscard]] lgp::framework::Status BuildUi(lgp::framework::UpdateContext const &context);
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext);
    void Shutdown() noexcept;
};

lgp::framework::Status Renderer::Impl::CompileShaders()
{
    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    lgp::framework::ShaderCompiler compiler = std::move(compilerResult.value());

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

lgp::framework::Status Renderer::Impl::CreateRootSignature(ID3D12Device &device)
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1U;
    srvRange.BaseShaderRegister = 0U;
    srvRange.RegisterSpace = 0U;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 2U> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[0].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0U;
    parameters[1].Constants.RegisterSpace = 0U;
    parameters[1].Constants.Num32BitValues = 4U;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    std::array<D3D12_STATIC_SAMPLER_DESC, 4U> samplers{
        MakeStaticSampler(D3D12_FILTER_MIN_MAG_MIP_POINT, 0U),
        MakeStaticSampler(D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, 1U),
        MakeStaticSampler(D3D12_FILTER_MIN_MAG_MIP_LINEAR, 2U),
        MakeStaticSampler(D3D12_FILTER_ANISOTROPIC, 3U, 8U),
    };

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
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
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult,
                                             "Failed to serialize the texture sampling root signature."));
    }

    HRESULT const createResult =
        device.CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                   IID_PPV_ARGS(rootSignature.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                             "Failed to create the texture sampling root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::Impl::CreatePipelineState(ID3D12Device &device, DXGI_FORMAT renderTargetFormat)
{
    std::array<D3D12_INPUT_ELEMENT_DESC, 2U> const inputElements{{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 16U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    }};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature.Get();
    description.VS = vertexShader.Bytecode();
    description.PS = pixelShader.Bytecode();
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = static_cast<UINT8>(D3D12_COLOR_WRITE_ENABLE_ALL);
    description.SampleMask = (std::numeric_limits<UINT>::max)();
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.StencilEnable = FALSE;
    description.InputLayout = {inputElements.data(), static_cast<UINT>(inputElements.size())};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = renderTargetFormat;
    description.SampleDesc.Count = 1U;

    HRESULT const result =
        device.CreateGraphicsPipelineState(&description, IID_PPV_ARGS(pipelineState.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create the texture sampling PSO."));
    }
    return {};
}

lgp::framework::Status Renderer::Impl::CreateVertexBuffer(ID3D12Device &device)
{
    auto const vertices = MakeVertices();
    auto bufferResult =
        lgp::framework::CreateUploadBuffer(device, sizeof(Vertex) * vertices.size(), L"Ch03 textured quad vertices");
    if (!bufferResult)
    {
        return std::unexpected(std::move(bufferResult.error()));
    }
    vertexBuffer = std::move(bufferResult.value());

    auto writeStatus = lgp::framework::WriteBuffer(vertexBuffer, std::span<Vertex const>{vertices});
    if (!writeStatus)
    {
        return std::unexpected(std::move(writeStatus.error()));
    }

    vertexBufferView.BufferLocation = vertexBuffer.gpu_virtual_address();
    vertexBufferView.SizeInBytes = static_cast<UINT>(sizeof(Vertex) * vertices.size());
    vertexBufferView.StrideInBytes = sizeof(Vertex);
    return {};
}

lgp::framework::Status Renderer::Impl::CreateTexture(lgp::framework::DeviceResources &resources)
{
    ID3D12Device10 *const device = resources.device();
    std::vector<MipImage> const mips = BuildMipChain();

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CreationNodeMask = 1U;
    defaultHeap.VisibleNodeMask = 1U;

    D3D12_RESOURCE_DESC1 textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = mips.front().width;
    textureDesc.Height = mips.front().height;
    textureDesc.DepthOrArraySize = 1U;
    textureDesc.MipLevels = static_cast<UINT16>(mips.size());
    textureDesc.Format = kTextureFormat;
    textureDesc.SampleDesc.Count = 1U;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    HRESULT const textureResult = device->CreateCommittedResource3(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr, 0U, nullptr,
        IID_PPV_ARGS(texture.ReleaseAndGetAddressOf()));
    if (FAILED(textureResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device10::CreateCommittedResource3", textureResult, "Failed to create the chapter texture."));
    }
    texture->SetName(L"Ch03 generated mipmapped texture");

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mips.size());
    std::vector<UINT> rowCounts(mips.size());
    std::vector<UINT64> rowSizes(mips.size());
    UINT64 uploadBytes = 0U;
    device->GetCopyableFootprints1(&textureDesc, 0U, static_cast<UINT>(mips.size()), 0U, footprints.data(),
                                   rowCounts.data(), rowSizes.data(), &uploadBytes);

    auto uploadResult = lgp::framework::CreateUploadBuffer(*device, uploadBytes, L"Ch03 texture upload");
    if (!uploadResult)
    {
        return std::unexpected(std::move(uploadResult.error()));
    }
    lgp::framework::Buffer uploadBuffer = std::move(uploadResult.value());

    for (std::size_t mipIndex = 0; mipIndex < mips.size(); ++mipIndex)
    {
        MipImage const &mip = mips[mipIndex];
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT const &footprint = footprints[mipIndex];
        std::size_t const sourceRowBytes = static_cast<std::size_t>(mip.width) * 4U;
        for (std::uint32_t row = 0; row < mip.height; ++row)
        {
            std::byte *const destination = uploadBuffer.mapped_data() + footprint.Offset +
                                           (static_cast<std::size_t>(row) * footprint.Footprint.RowPitch);
            std::uint8_t const *const source = mip.rgba8.data() + (static_cast<std::size_t>(row) * sourceRowBytes);
            std::memcpy(destination, source, sourceRowBytes);
        }
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    HRESULT const allocatorResult = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                   IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf()));
    if (FAILED(allocatorResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandAllocator", allocatorResult,
                                                                "Failed to create a texture upload allocator."));
    }

    ComPtr<ID3D12GraphicsCommandList7> list;
    HRESULT const listResult = device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                         IID_PPV_ARGS(list.ReleaseAndGetAddressOf()));
    if (FAILED(listResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandList", listResult,
                                                                "Failed to create a texture upload command list."));
    }

    for (std::size_t mipIndex = 0; mipIndex < mips.size(); ++mipIndex)
    {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = static_cast<UINT>(mipIndex);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = uploadBuffer.resource();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprints[mipIndex];
        list->CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);
    }

    lgp::framework::TextureBarrierState constexpr copyDestination{
        D3D12_BARRIER_SYNC_COPY,
        D3D12_BARRIER_ACCESS_COPY_DEST,
        D3D12_BARRIER_LAYOUT_COPY_DEST,
    };
    lgp::framework::TextureBarrierState constexpr shaderResource{
        D3D12_BARRIER_SYNC_PIXEL_SHADING,
        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
        D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
    };
    lgp::framework::TransitionTexture(*list.Get(), *texture.Get(), copyDestination, shaderResource);

    HRESULT const closeResult = list->Close();
    if (FAILED(closeResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12GraphicsCommandList::Close", closeResult,
                                                                "Failed to close the texture upload command list."));
    }
    ID3D12CommandList *const commandLists[]{list.Get()};
    resources.graphics_queue()->ExecuteCommandLists(1U, commandLists);
    auto idleStatus = resources.WaitForGpuIdle();
    if (!idleStatus)
    {
        return std::unexpected(std::move(idleStatus.error()));
    }

    auto srvResult = resources.shader_visible_cbv_srv_uav_heap().Allocate();
    if (!srvResult)
    {
        return std::unexpected(std::move(srvResult.error()));
    }
    srv = srvResult.value();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = kTextureFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = static_cast<UINT>(mips.size());
    device->CreateShaderResourceView(texture.Get(), &srvDesc, srv.cpuHandle);
    return {};
}

lgp::framework::Status Renderer::Impl::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources = &context.deviceResources;
    headless = context.commandLine.headless;
    ID3D12Device *const device = context.deviceResources.device();

    auto shaderStatus = CompileShaders();
    if (!shaderStatus)
    {
        return shaderStatus;
    }
    auto rootStatus = CreateRootSignature(*device);
    if (!rootStatus)
    {
        return rootStatus;
    }
    auto textureStatus = CreateTexture(context.deviceResources);
    if (!textureStatus)
    {
        return textureStatus;
    }
    auto vertexStatus = CreateVertexBuffer(*device);
    if (!vertexStatus)
    {
        return vertexStatus;
    }
    auto pipelineStatus = CreatePipelineState(*device, context.deviceResources.back_buffer_format());
    if (!pipelineStatus)
    {
        return pipelineStatus;
    }
    if (!headless)
    {
        return InitializeImGui();
    }
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
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = deviceResources->shader_visible_cbv_srv_uav_heap().Get();
    info.LegacySingleSrvCpuDescriptor = imguiFontDescriptor.cpuHandle;
    info.LegacySingleSrvGpuDescriptor = imguiFontDescriptor.gpuHandle;
    if (!ImGui_ImplDX12_Init(&info))
    {
        return std::unexpected(
            lgp::framework::MakeError("ImGui_ImplDX12_Init", "Failed to initialize the texture sampling UI."));
    }
    imguiInitialized = true;
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

    ImGui::Begin("Texture Sampling");
    int sampler = static_cast<int>(samplerMode);
    ImGui::Combo("Sampler", &sampler, "Point\0Bilinear\0Trilinear\0Anisotropic\0");
    samplerMode = static_cast<SamplerMode>(std::clamp(sampler, 0, 3));
    ImGui::Checkbox("Visualize mip level", &visualizeMip);
    ImGui::SliderFloat("LOD bias", &lodBias, -2.0F, 2.0F, "%.2f");
    ImGui::Separator();
    ImGui::TextWrapped("The receding quad repeats a high-frequency texture. Compare shimmer/detail with point, "
                       "bilinear, trilinear, and anisotropic filtering; enable mip visualization to see the "
                       "implicit derivative-selected mip level. Negative LOD bias sharpens, positive bias blurs.");
    ImGui::End();
    return {};
}

lgp::framework::Status Renderer::Impl::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr)
    {
        if (imguiFrameBegun)
        {
            ImGui::EndFrame();
            imguiFrameBegun = false;
        }
        return {};
    }

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
    lgp::framework::TransitionTexture(*frameContext.commandList, *frameContext.renderTarget, initialState,
                                      renderTargetState);

    float const clearColor[]{0.030F, 0.055F, 0.080F, 1.0F};
    frameContext.commandList->OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    frameContext.commandList->ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    frameContext.commandList->RSSetViewports(1U, &frameContext.viewport);
    frameContext.commandList->RSSetScissorRects(1U, &frameContext.scissorRect);

    ID3D12DescriptorHeap *const descriptorHeaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    frameContext.commandList->SetDescriptorHeaps(1U, descriptorHeaps);
    frameContext.commandList->SetGraphicsRootSignature(rootSignature.Get());
    frameContext.commandList->SetGraphicsRootDescriptorTable(0U, srv.gpuHandle);
    struct DrawConstants final
    {
        std::uint32_t samplerMode;
        std::uint32_t visualizeMip;
        float lodBias;
        std::uint32_t padding;
    };
    DrawConstants const constants{
        static_cast<std::uint32_t>(samplerMode),
        visualizeMip ? 1U : 0U,
        lodBias,
        0U,
    };
    frameContext.commandList->SetGraphicsRoot32BitConstants(1U, 4U, &constants, 0U);
    frameContext.commandList->SetPipelineState(pipelineState.Get());
    frameContext.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    frameContext.commandList->IASetVertexBuffers(0U, 1U, &vertexBufferView);
    frameContext.commandList->DrawInstanced(6U, 1U, 0U, 0U);

    if (imguiFrameBegun)
    {
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), frameContext.commandList);
        imguiFrameBegun = false;
    }

    lgp::framework::TextureBarrierState const endState{
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT,
    };
    lgp::framework::TransitionTexture(*frameContext.commandList, *frameContext.renderTarget, renderTargetState,
                                      endState);
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
    if (deviceResources != nullptr && srv)
    {
        deviceResources->shader_visible_cbv_srv_uav_heap().Free(srv);
        srv = {};
    }
    texture.Reset();
    vertexBuffer = {};
    pipelineState.Reset();
    rootSignature.Reset();
    deviceResources = nullptr;
}

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}

Renderer::~Renderer() = default;

Renderer::Renderer(Renderer &&) noexcept = default;

Renderer &Renderer::operator=(Renderer &&) noexcept = default;

void Renderer::SetSamplerMode(SamplerMode mode) noexcept
{
    impl_->samplerMode = mode;
}

void Renderer::SetMipVisualization(bool enabled) noexcept
{
    impl_->visualizeMip = enabled;
}

void Renderer::SetLodBias(float bias) noexcept
{
    impl_->lodBias = std::clamp(bias, -2.0F, 2.0F);
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    return impl_->Initialize(context);
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D)
{
    return {};
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

} // namespace ch03::texture::solution
