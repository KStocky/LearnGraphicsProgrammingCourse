#include "GpuLabSupport.hpp"

#include <lgp/framework/buffer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace ch27::environment_lighting::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorCount = 3U;
inline constexpr DXGI_FORMAT kEnvironmentFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

enum DescriptorIndex : UINT
{
    StatisticsUav = 0U,
    EnvironmentSrv = 1U,
    StatisticsSrv = 2U,
};

enum ComputeRootParameter : UINT
{
    ComputeConstants = 0U,
    ComputeStatistics = 1U,
    ComputeEnvironment = 2U,
};

enum GraphicsRootParameter : UINT
{
    GraphicsConstants = 0U,
    GraphicsStatistics = 1U,
};

struct DispatchConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t environmentWidth{};
    std::uint32_t environmentHeight{};
    std::uint32_t environmentMipCount{};
    std::uint32_t prefilterSampleCount{};
    std::uint32_t splitSumSampleCount{};
    float roughness{};
    float nDotView{};
};

struct DisplayConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t debugView{};
    std::uint32_t expectedStatus{};
};

static_assert(sizeof(DispatchConstants) == 36U);
static_assert(sizeof(DisplayConstants) == 16U);

[[nodiscard]] double SmoothStep(double edge0, double edge1, double value) noexcept
{
    double const t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] lgp::framework::Status CompileShader(lgp::framework::ShaderCompiler &compiler,
                                                   lgp::framework::ShaderCompileOptions options,
                                                   wchar_t const *entryPoint, wchar_t const *targetProfile,
                                                   lgp::framework::CompiledShader &shader)
{
    options.entryPoint = entryPoint;
    options.targetProfile = targetProfile;
    options.additionalArguments = {L"-E", entryPoint, L"-T", targetProfile};
    auto result = compiler.Compile(options);
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    shader = std::move(*result);
    return {};
}

[[nodiscard]] lgp::framework::Status ValidateExtent(lgp::framework::Extent2D size)
{
    if (size.empty() || size.width > kMaximumWidth || size.height > kMaximumHeight)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateExtent", "Chapter 27 requires a non-empty extent up to 640x360."));
    }
    return {};
}

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frameContext.renderTargetInitialLayout};
}

[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
            frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT};
}

[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
}

[[nodiscard]] BufferBarrierState NoAccessState() noexcept
{
    return {};
}

[[nodiscard]] BufferBarrierState ComputeUnorderedAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
}

[[nodiscard]] BufferBarrierState PixelShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
}

[[nodiscard]] BufferBarrierState CopySourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE};
}

[[nodiscard]] D3D12_BUFFER_BARRIER MakeBufferBarrier(ID3D12Resource &resource, BufferBarrierState before,
                                                     BufferBarrierState after) noexcept
{
    D3D12_BUFFER_BARRIER barrier{};
    barrier.SyncBefore = before.sync;
    barrier.SyncAfter = after.sync;
    barrier.AccessBefore = before.access;
    barrier.AccessAfter = after.access;
    barrier.pResource = &resource;
    barrier.Offset = 0U;
    barrier.Size = UINT64_MAX;
    return barrier;
}

void SubmitBufferBarrier(ID3D12GraphicsCommandList7 &commandList, D3D12_BUFFER_BARRIER &barrier)
{
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_BUFFER;
    group.NumBarriers = 1U;
    group.pBufferBarriers = &barrier;
    commandList.Barrier(1U, &group);
}

[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(ID3D12Resource &resource,
                                                       lgp::framework::TextureBarrierState before,
                                                       lgp::framework::TextureBarrierState after) noexcept
{
    D3D12_TEXTURE_BARRIER barrier{};
    barrier.SyncBefore = before.sync;
    barrier.SyncAfter = after.sync;
    barrier.AccessBefore = before.access;
    barrier.AccessAfter = after.access;
    barrier.LayoutBefore = before.layout;
    barrier.LayoutAfter = after.layout;
    barrier.pResource = &resource;
    barrier.Subresources.IndexOrFirstMipLevel = UINT32_MAX;
    return barrier;
}

void SubmitTextureBarrier(ID3D12GraphicsCommandList7 &commandList, D3D12_TEXTURE_BARRIER &barrier)
{
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1U;
    group.pTextureBarriers = &barrier;
    commandList.Barrier(1U, &group);
}

} // namespace

lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration, LabVariant variant)
{
    if (!std::isfinite(configuration.roughness) || configuration.roughness < 0.0F || configuration.roughness > 1.0F)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Chapter 27 roughness must be finite and in [0,1]."));
    }
    if (!std::isfinite(configuration.nDotView) || configuration.nDotView <= 0.0F || configuration.nDotView > 1.0F)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Chapter 27 N.V must be finite and in (0,1]."));
    }
    std::uint32_t const view = static_cast<std::uint32_t>(configuration.debugView);
    std::uint32_t const maximum = variant == LabVariant::Starter
                                      ? static_cast<std::uint32_t>(DebugView::BaselineComponents)
                                      : static_cast<std::uint32_t>(DebugView::BaselineVsSolution);
    if (view > maximum)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "The selected Chapter 27 view is unavailable."));
    }
    return {};
}

Float3 PixelDirection(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) noexcept
{
    double const theta = kPi * (static_cast<double>(y) + 0.5) / static_cast<double>(height);
    double const phi = 2.0 * kPi * (static_cast<double>(x) + 0.5) / static_cast<double>(width);
    double const sine = std::sin(theta);
    return {sine * std::cos(phi), std::cos(theta), sine * std::sin(phi)};
}

Rgb EvaluateEnvironment(Float3 direction) noexcept
{
    double const sky = SmoothStep(-0.25, 0.7, direction.y);
    Rgb color{
        0.72 + ((0.18 - 0.72) * sky),
        0.76 + ((0.38 - 0.76) * sky),
        0.72 + ((0.90 - 0.72) * sky),
    };
    double const ground = SmoothStep(0.08, -0.08, direction.y);
    color.r += (0.16 - color.r) * ground;
    color.g += (0.12 - color.g) * ground;
    color.b += (0.09 - color.b) * ground;

    Float3 const rawSun{0.42, 0.72, -0.55};
    double const inverseSunLength =
        1.0 / std::sqrt((rawSun.x * rawSun.x) + (rawSun.y * rawSun.y) + (rawSun.z * rawSun.z));
    Float3 const sun{rawSun.x * inverseSunLength, rawSun.y * inverseSunLength, rawSun.z * inverseSunLength};
    double const cosine = std::max(0.0, (direction.x * sun.x) + (direction.y * sun.y) + (direction.z * sun.z));
    double const lobe = 10.0 * std::pow(cosine, 96.0);
    color.r += lobe;
    color.g += 0.82 * lobe;
    color.b += 0.58 * lobe;

    double const stripe = 0.12 * (0.5 + (0.5 * std::sin(5.0 * std::atan2(direction.z, direction.x))));
    color.r += stripe;
    color.g += 0.35 * stripe;
    return color;
}

std::vector<EnvironmentMip> BuildEnvironmentMipChain()
{
    std::vector<EnvironmentMip> mips;
    EnvironmentMip base{.width = kEnvironmentWidth, .height = kEnvironmentHeight};
    base.rgba.resize(static_cast<std::size_t>(base.width) * base.height * 4U);
    for (std::uint32_t row = 0U; row < base.height; ++row)
    {
        for (std::uint32_t column = 0U; column < base.width; ++column)
        {
            Rgb const value = EvaluateEnvironment(PixelDirection(column, row, base.width, base.height));
            std::size_t const index = (static_cast<std::size_t>(row) * base.width + column) * 4U;
            base.rgba[index + 0U] = static_cast<float>(value.r);
            base.rgba[index + 1U] = static_cast<float>(value.g);
            base.rgba[index + 2U] = static_cast<float>(value.b);
            base.rgba[index + 3U] = 1.0F;
        }
    }
    mips.push_back(std::move(base));

    while (mips.size() < kEnvironmentMipCount)
    {
        EnvironmentMip const &source = mips.back();
        EnvironmentMip destination{
            .width = std::max(1U, source.width / 2U),
            .height = std::max(1U, source.height / 2U),
        };
        destination.rgba.resize(static_cast<std::size_t>(destination.width) * destination.height * 4U);
        for (std::uint32_t y = 0U; y < destination.height; ++y)
        {
            for (std::uint32_t x = 0U; x < destination.width; ++x)
            {
                for (std::uint32_t channel = 0U; channel < 4U; ++channel)
                {
                    float sum = 0.0F;
                    for (std::uint32_t oy = 0U; oy < 2U; ++oy)
                    {
                        for (std::uint32_t ox = 0U; ox < 2U; ++ox)
                        {
                            std::uint32_t const sx = std::min(source.width - 1U, x * 2U + ox);
                            std::uint32_t const sy = std::min(source.height - 1U, y * 2U + oy);
                            sum += source.rgba[(static_cast<std::size_t>(sy) * source.width + sx) * 4U + channel];
                        }
                    }
                    destination.rgba[(static_cast<std::size_t>(y) * destination.width + x) * 4U + channel] =
                        0.25F * sum;
                }
            }
        }
        mips.push_back(std::move(destination));
    }
    return mips;
}

EnvironmentImageView BaseEnvironmentView(std::vector<EnvironmentMip> const &mips, std::vector<Rgb> &storage)
{
    EnvironmentMip const &base = mips.front();
    storage.resize(static_cast<std::size_t>(base.width) * base.height);
    for (std::size_t index = 0U; index < storage.size(); ++index)
    {
        storage[index] = {base.rgba[index * 4U], base.rgba[index * 4U + 1U], base.rgba[index * 4U + 2U]};
    }
    return {.width = base.width, .height = base.height, .pixels = storage};
}

BufferResource::BufferResource(BufferResource &&other) noexcept
{
    *this = std::move(other);
}

BufferResource &BufferResource::operator=(BufferResource &&other) noexcept
{
    if (this != &other)
    {
        Reset();
        resource_ = std::move(other.resource_);
        sizeInBytes_ = std::exchange(other.sizeInBytes_, 0U);
        mappedData_ = std::exchange(other.mappedData_, nullptr);
    }
    return *this;
}

BufferResource::~BufferResource()
{
    Reset();
}

void BufferResource::Reset() noexcept
{
    if (resource_ != nullptr && mappedData_ != nullptr)
    {
        D3D12_RANGE const written{0U, 0U};
        resource_->Unmap(0U, &written);
    }
    resource_.Reset();
    sizeInBytes_ = 0U;
    mappedData_ = nullptr;
}

std::expected<BufferResource, lgp::framework::Error> CreateBuffer(ID3D12Device10 &device, std::uint64_t sizeInBytes,
                                                                  D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
                                                                  std::wstring_view name, bool mapPersistently)
{
    if (sizeInBytes == 0U)
    {
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 27 buffers must be non-empty."));
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    heap.CreationNodeMask = 1U;
    heap.VisibleNodeMask = 1U;
    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = sizeInBytes;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;

    BufferResource buffer{};
    HRESULT const result = device.CreateCommittedResource3(
        &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0U, nullptr,
        IID_PPV_ARGS(buffer.resource_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                                "Failed to create a Chapter 27 buffer."));
    }
    if (!name.empty())
    {
        std::wstring const resourceName{name};
        if (HRESULT const nameResult = buffer.resource_->SetName(resourceName.c_str()); FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name a Chapter 27 buffer."));
        }
    }
    if (mapPersistently)
    {
        D3D12_RANGE const readRange{0U, static_cast<SIZE_T>(heapType == D3D12_HEAP_TYPE_READBACK ? sizeInBytes : 0U)};
        void *mapped = nullptr;
        if (HRESULT const mapResult = buffer.resource_->Map(0U, &readRange, &mapped); FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map a Chapter 27 buffer."));
        }
        buffer.mappedData_ = static_cast<std::byte *>(mapped);
    }
    buffer.sizeInBytes_ = sizeInBytes;
    return buffer;
}

RendererCore::RendererCore(std::filesystem::path shaderPath, LabVariant variant)
    : shaderPath_(std::move(shaderPath)), variant_(variant)
{
    if (variant_ == LabVariant::Starter)
    {
        interactiveConfiguration_.debugView = DebugView::Baseline;
    }
}

lgp::framework::Status RendererCore::CreateShaders()
{
    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    lgp::framework::ShaderCompiler compiler = std::move(*compilerResult);
    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = shaderPath_;
    options.includeDirectories = {shaderPath_.parent_path()};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif
    if (auto status = CompileShader(compiler, options, L"SampleCS", L"cs_6_0", sampleShader_); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"FullscreenVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    return CompileShader(compiler, options, L"DisplayPS", L"ps_6_0", pixelShader_);
}

lgp::framework::Status RendererCore::CreateRootSignatures()
{
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1U;
    uavRange.BaseShaderRegister = 0U;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE environmentRange{};
    environmentRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    environmentRange.NumDescriptors = 1U;
    environmentRange.BaseShaderRegister = 0U;
    environmentRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER computeParameters[3]{};
    computeParameters[ComputeConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[ComputeConstants].Constants.ShaderRegister = 0U;
    computeParameters[ComputeConstants].Constants.Num32BitValues = sizeof(DispatchConstants) / sizeof(std::uint32_t);
    computeParameters[ComputeStatistics].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeStatistics].DescriptorTable = {1U, &uavRange};
    computeParameters[ComputeEnvironment].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeEnvironment].DescriptorTable = {1U, &environmentRange};

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC computeDescription{};
    computeDescription.NumParameters = static_cast<UINT>(std::size(computeParameters));
    computeDescription.pParameters = computeParameters;
    computeDescription.NumStaticSamplers = 1U;
    computeDescription.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};
    HRESULT const computeSerialize =
        D3D12SerializeRootSignature(&computeDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                    serialized.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (FAILED(computeSerialize))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", computeSerialize, BlobText(errors.Get())));
    }
    HRESULT const computeCreate =
        deviceResources_->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(computeRootSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(computeCreate))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", computeCreate,
                                                                "Failed to create Chapter 27 compute root signature."));
    }

    D3D12_DESCRIPTOR_RANGE statisticsRange{};
    statisticsRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    statisticsRange.NumDescriptors = 1U;
    statisticsRange.BaseShaderRegister = 0U;
    statisticsRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER graphicsParameters[2]{};
    graphicsParameters[GraphicsConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[GraphicsConstants].Constants.ShaderRegister = 0U;
    graphicsParameters[GraphicsConstants].Constants.Num32BitValues = sizeof(DisplayConstants) / sizeof(std::uint32_t);
    graphicsParameters[GraphicsConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    graphicsParameters[GraphicsStatistics].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParameters[GraphicsStatistics].DescriptorTable = {1U, &statisticsRange};
    graphicsParameters[GraphicsStatistics].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC graphicsDescription{};
    graphicsDescription.NumParameters = static_cast<UINT>(std::size(graphicsParameters));
    graphicsDescription.pParameters = graphicsParameters;
    graphicsDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    serialized.Reset();
    errors.Reset();
    HRESULT const graphicsSerialize =
        D3D12SerializeRootSignature(&graphicsDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                    serialized.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (FAILED(graphicsSerialize))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", graphicsSerialize, BlobText(errors.Get())));
    }
    HRESULT const graphicsCreate =
        deviceResources_->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(graphicsRootSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(graphicsCreate))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", graphicsCreate,
                                             "Failed to create Chapter 27 graphics root signature."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreatePipelines()
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = computeRootSignature_.Get();
    compute.CS = sampleShader_.Bytecode();
    if (HRESULT const result = deviceResources_->device()->CreateComputePipelineState(
            &compute, IID_PPV_ARGS(computePipeline_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState", result,
                                                                "Failed to create Chapter 27 compute PSO."));
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    graphics.pRootSignature = graphicsRootSignature_.Get();
    graphics.VS = vertexShader_.Bytecode();
    graphics.PS = pixelShader_.Bytecode();
    graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    graphics.SampleMask = UINT_MAX;
    graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    graphics.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    graphics.RasterizerState.DepthClipEnable = TRUE;
    graphics.DepthStencilState.DepthEnable = FALSE;
    graphics.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics.NumRenderTargets = 1U;
    graphics.RTVFormats[0] = deviceResources_->back_buffer_format();
    graphics.SampleDesc.Count = 1U;
    if (HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
            &graphics, IID_PPV_ARGS(graphicsPipeline_.ReleaseAndGetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create Chapter 27 graphics PSO."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreateEnvironmentTexture()
{
    std::vector<EnvironmentMip> const mips = BuildEnvironmentMipChain();
    ID3D12Device10 &device = *deviceResources_->device();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1U;
    heap.VisibleNodeMask = 1U;
    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = kEnvironmentWidth;
    description.Height = kEnvironmentHeight;
    description.DepthOrArraySize = 1U;
    description.MipLevels = kEnvironmentMipCount;
    description.Format = kEnvironmentFormat;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    HRESULT const textureResult = device.CreateCommittedResource3(
        &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr, 0U, nullptr,
        IID_PPV_ARGS(environmentTexture_.ReleaseAndGetAddressOf()));
    if (FAILED(textureResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3",
                                                                textureResult,
                                                                "Failed to create Chapter 27 environment texture."));
    }
    (void)environmentTexture_->SetName(L"Ch27 deterministic analytic environment");

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mips.size());
    std::vector<UINT> rowCounts(mips.size());
    std::vector<UINT64> rowSizes(mips.size());
    UINT64 uploadBytes = 0U;
    device.GetCopyableFootprints1(&description, 0U, static_cast<UINT>(mips.size()), 0U, footprints.data(),
                                  rowCounts.data(), rowSizes.data(), &uploadBytes);
    auto upload = lgp::framework::CreateUploadBuffer(device, uploadBytes, L"Ch27 environment upload");
    if (!upload)
    {
        return std::unexpected(std::move(upload.error()));
    }
    for (std::size_t mipIndex = 0U; mipIndex < mips.size(); ++mipIndex)
    {
        std::size_t const rowBytes = static_cast<std::size_t>(mips[mipIndex].width) * sizeof(float) * 4U;
        for (std::uint32_t row = 0U; row < mips[mipIndex].height; ++row)
        {
            std::byte *destination = upload->mapped_data() + footprints[mipIndex].Offset +
                                     static_cast<std::size_t>(row) * footprints[mipIndex].Footprint.RowPitch;
            float const *source =
                mips[mipIndex].rgba.data() + static_cast<std::size_t>(row) * mips[mipIndex].width * 4U;
            std::memcpy(destination, source, rowBytes);
        }
    }

    ComPtr<ID3D12CommandAllocator> allocator{};
    if (HRESULT const result =
            device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandAllocator", result,
                                                                "Failed to create Chapter 27 upload allocator."));
    }
    ComPtr<ID3D12GraphicsCommandList7> list{};
    if (HRESULT const result = device.CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                        IID_PPV_ARGS(list.GetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandList", result,
                                                                "Failed to create Chapter 27 upload command list."));
    }
    for (std::size_t mipIndex = 0U; mipIndex < mips.size(); ++mipIndex)
    {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = environmentTexture_.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = static_cast<UINT>(mipIndex);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload->resource();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprints[mipIndex];
        list->CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);
    }
    lgp::framework::TextureBarrierState constexpr copyDestination{
        D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_DEST};
    lgp::framework::TextureBarrierState constexpr shaderResource{
        D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE};
    lgp::framework::TransitionTexture(*list.Get(), *environmentTexture_.Get(), copyDestination, shaderResource);
    if (HRESULT const result = list->Close(); FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12GraphicsCommandList::Close", result,
                                                                "Failed to close Chapter 27 upload command list."));
    }
    ID3D12CommandList *lists[]{list.Get()};
    deviceResources_->graphics_queue()->ExecuteCommandLists(1U, lists);
    return deviceResources_->WaitForGpuIdle();
}

lgp::framework::Status RendererCore::CreateFrameSlotResources(lgp::framework::Extent2D size)
{
    if (auto status = ValidateExtent(size); !status)
    {
        return status;
    }
    std::uint64_t const pixelCount = static_cast<std::uint64_t>(size.width) * size.height;
    std::uint64_t const statisticsBytes = pixelCount * sizeof(PixelStatistics);
    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlotResources &slot : frameSlots_)
    {
        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorCount);
        auto statistics = CreateBuffer(*deviceResources_->device(), statisticsBytes, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch27 pixel statistics");
        auto readback = CreateBuffer(*deviceResources_->device(), statisticsBytes, D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_FLAG_NONE, L"Ch27 statistics readback", true);
        if (!descriptors)
        {
            return std::unexpected(std::move(descriptors.error()));
        }
        if (!statistics)
        {
            return std::unexpected(std::move(statistics.error()));
        }
        if (!readback)
        {
            return std::unexpected(std::move(readback.error()));
        }
        slot.descriptors = *descriptors;
        slot.statistics = std::move(*statistics);
        slot.statisticsReadback = std::move(*readback);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = static_cast<UINT>(pixelCount);
        uav.Buffer.StructureByteStride = sizeof(PixelStatistics);
        deviceResources_->device()->CreateUnorderedAccessView(slot.statistics.Get(), nullptr, &uav,
                                                              slot.descriptors.CpuHandle(StatisticsUav));
        D3D12_SHADER_RESOURCE_VIEW_DESC environmentSrv{};
        environmentSrv.Format = kEnvironmentFormat;
        environmentSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        environmentSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        environmentSrv.Texture2D.MipLevels = kEnvironmentMipCount;
        deviceResources_->device()->CreateShaderResourceView(environmentTexture_.Get(), &environmentSrv,
                                                             slot.descriptors.CpuHandle(EnvironmentSrv));
        D3D12_SHADER_RESOURCE_VIEW_DESC statisticsSrv{};
        statisticsSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        statisticsSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        statisticsSrv.Buffer.NumElements = static_cast<UINT>(pixelCount);
        statisticsSrv.Buffer.StructureByteStride = sizeof(PixelStatistics);
        deviceResources_->device()->CreateShaderResourceView(slot.statistics.Get(), &statisticsSrv,
                                                             slot.descriptors.CpuHandle(StatisticsSrv));
    }
    size_ = size;
    hasRendered_ = false;
    return {};
}

LabConfiguration RendererCore::ActiveConfiguration() const noexcept
{
    return headless_ && headlessConfiguration_ ? *headlessConfiguration_ : interactiveConfiguration_;
}

lgp::framework::Status RendererCore::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreateRootSignatures(); !status)
    {
        return status;
    }
    if (auto status = CreatePipelines(); !status)
    {
        return status;
    }
    return CreateEnvironmentTexture();
}

lgp::framework::Status RendererCore::OnResize(lgp::framework::DeviceResources &deviceResources,
                                              lgp::framework::Extent2D drawableSize)
{
    DestroyFrameSlotResources(deviceResources);
    return CreateFrameSlotResources(drawableSize);
}

lgp::framework::Status RendererCore::Update(lgp::framework::UpdateContext const &context)
{
    if (headless_)
    {
        return {};
    }
    DebugView const maximumView =
        variant_ == LabVariant::Starter ? DebugView::BaselineComponents : DebugView::BaselineVsSolution;
    for (std::uint32_t index = 0U; index <= static_cast<std::uint32_t>(maximumView); ++index)
    {
        if (context.input.WasKeyPressed('1' + index))
        {
            interactiveConfiguration_.debugView = static_cast<DebugView>(index);
        }
    }
    if (context.input.WasKeyPressed('Q'))
    {
        interactiveConfiguration_.roughness = std::max(0.0F, interactiveConfiguration_.roughness - 0.05F);
    }
    if (context.input.WasKeyPressed('E'))
    {
        interactiveConfiguration_.roughness = std::min(1.0F, interactiveConfiguration_.roughness + 0.05F);
    }
    if (context.input.WasKeyPressed('A'))
    {
        interactiveConfiguration_.nDotView = std::max(0.05F, interactiveConfiguration_.nDotView - 0.05F);
    }
    if (context.input.WasKeyPressed('D'))
    {
        interactiveConfiguration_.nDotView = std::min(1.0F, interactiveConfiguration_.nDotView + 0.05F);
    }
    return {};
}

lgp::framework::Status RendererCore::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Render", "Chapter 27 frame slot is out of range."));
    }
    LabConfiguration const configuration = ActiveConfiguration();
    if (auto status = ValidateLabConfiguration(configuration, variant_); !status)
    {
        return status;
    }
    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);

    D3D12_BUFFER_BARRIER statisticsBarrier = MakeBufferBarrier(
        *slot.statistics.Get(), slot.initialized ? CopySourceState() : NoAccessState(), ComputeUnorderedAccessState());
    SubmitBufferBarrier(commandList, statisticsBarrier);

    DispatchConstants const dispatch{
        size_.width,
        size_.height,
        kEnvironmentWidth,
        kEnvironmentHeight,
        kEnvironmentMipCount,
        kPrefilterSampleCount,
        kSplitSumSampleCount,
        configuration.roughness,
        configuration.nDotView,
    };
    commandList.SetComputeRootSignature(computeRootSignature_.Get());
    commandList.SetComputeRoot32BitConstants(ComputeConstants, sizeof(dispatch) / sizeof(std::uint32_t), &dispatch, 0U);
    commandList.SetComputeRootDescriptorTable(ComputeStatistics, slot.descriptors.GpuHandle(StatisticsUav));
    commandList.SetComputeRootDescriptorTable(ComputeEnvironment, slot.descriptors.GpuHandle(EnvironmentSrv));
    commandList.SetPipelineState(computePipeline_.Get());
    commandList.Dispatch((size_.width + 7U) / 8U, (size_.height + 7U) / 8U, 1U);

    statisticsBarrier =
        MakeBufferBarrier(*slot.statistics.Get(), ComputeUnorderedAccessState(), PixelShaderResourceState());
    SubmitBufferBarrier(commandList, statisticsBarrier);
    D3D12_TEXTURE_BARRIER targetBarrier =
        MakeTextureBarrier(*frameContext.renderTarget, FrameStartState(frameContext), RenderTargetState());
    SubmitTextureBarrier(commandList, targetBarrier);

    float const clear[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clear, 0U, nullptr);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    DisplayConstants const display{
        size_.width,
        size_.height,
        static_cast<std::uint32_t>(configuration.debugView),
        variant_ == LabVariant::Solution ? kSolutionValidStatus : kStarterValidStatus,
    };
    commandList.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    commandList.SetGraphicsRoot32BitConstants(GraphicsConstants, sizeof(display) / sizeof(std::uint32_t), &display, 0U);
    commandList.SetGraphicsRootDescriptorTable(GraphicsStatistics, slot.descriptors.GpuHandle(StatisticsSrv));
    commandList.SetPipelineState(graphicsPipeline_.Get());
    commandList.DrawInstanced(3U, 1U, 0U, 0U);

    statisticsBarrier = MakeBufferBarrier(*slot.statistics.Get(), PixelShaderResourceState(), CopySourceState());
    SubmitBufferBarrier(commandList, statisticsBarrier);
    commandList.CopyBufferRegion(slot.statisticsReadback.Get(), 0U, slot.statistics.Get(), 0U,
                                 slot.statistics.size_in_bytes());
    targetBarrier = MakeTextureBarrier(*frameContext.renderTarget, RenderTargetState(), FrameEndState(frameContext));
    SubmitTextureBarrier(commandList, targetBarrier);

    slot.initialized = true;
    lastRenderedConfiguration_ = configuration;
    lastRenderedFrameSlot_ = frameContext.frameSlot;
    hasRendered_ = true;
    return {};
}

void RendererCore::DestroyFrameSlotResources(lgp::framework::DeviceResources &deviceResources) noexcept
{
    for (FrameSlotResources &slot : frameSlots_)
    {
        if (slot.descriptors)
        {
            deviceResources.shader_visible_cbv_srv_uav_heap().Free(slot.descriptors);
            slot.descriptors = {};
        }
    }
    frameSlots_.clear();
    size_ = {};
    hasRendered_ = false;
}

void RendererCore::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    DestroyFrameSlotResources(deviceResources);
    environmentTexture_.Reset();
    graphicsPipeline_.Reset();
    computePipeline_.Reset();
    graphicsRootSignature_.Reset();
    computeRootSignature_.Reset();
    sampleShader_ = {};
    vertexShader_ = {};
    pixelShader_ = {};
    deviceResources_ = nullptr;
}

void RendererCore::ConfigureHeadlessTest(LabConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

std::expected<FrameReadback, lgp::framework::Error> RendererCore::ReadBackOutputs()
{
    if (deviceResources_ == nullptr || frameSlots_.empty() || !hasRendered_)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "Chapter 27 has no rendered frame to read back."));
    }
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }
    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    std::size_t const pixelCount = static_cast<std::size_t>(size_.width) * size_.height;
    FrameReadback outputs{};
    outputs.configuration = lastRenderedConfiguration_;
    outputs.size = size_;
    outputs.frameSlot = lastRenderedFrameSlot_;
    outputs.pixels.resize(pixelCount);
    std::memcpy(outputs.pixels.data(), slot.statisticsReadback.mapped_data(), pixelCount * sizeof(PixelStatistics));
    return outputs;
}

} // namespace ch27::environment_lighting::gpu
