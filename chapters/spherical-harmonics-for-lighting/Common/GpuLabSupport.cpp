#include "GpuLabSupport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace ch26::spherical_harmonics::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorCount = 2U;

enum DescriptorIndex : UINT
{
    StatisticsUav = 0U,
    StatisticsSrv = 1U,
};

enum ComputeRootParameter : UINT
{
    ComputeConstants = 0U,
    ComputeCoefficients = 1U,
    ComputeDescriptorTable = 2U,
};

enum GraphicsRootParameter : UINT
{
    GraphicsConstants = 0U,
    GraphicsSrvTable = 1U,
};

struct DispatchConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t activeBand{};
    std::uint32_t reserved{};
};

struct DisplayConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t debugView{};
    std::uint32_t expectedStatus{};
};

static_assert(sizeof(DispatchConstants) == 16U);
static_assert(sizeof(DisplayConstants) == 16U);

// Deterministic scene-authoring parameters for the analytic environment: a
// sky/ground gradient (smooth, effectively axisymmetric about Y) plus a
// single bounded, angularly narrow "sun" lobe placed off-axis so its energy
// spreads into every band once projected. These describe *where the light
// is*, exactly like choosing a light direction in any renderer -- they are
// not SH coefficients and are never substituted for the projection contract.
inline constexpr double kSkyZenithR = 0.30;
inline constexpr double kSkyZenithG = 0.50;
inline constexpr double kSkyZenithB = 0.85;
inline constexpr double kSkyHorizonR = 0.80;
inline constexpr double kSkyHorizonG = 0.80;
inline constexpr double kSkyHorizonB = 0.70;
inline constexpr double kGroundR = 0.20;
inline constexpr double kGroundG = 0.17;
inline constexpr double kGroundB = 0.14;
inline constexpr double kSunColorR = 1.00;
inline constexpr double kSunColorG = 0.86;
inline constexpr double kSunColorB = 0.68;
inline constexpr double kSunIntensity = 8.0;
inline constexpr double kSunSharpness = 48.0;

[[nodiscard]] double SmoothStep(double edge0, double edge1, double value) noexcept
{
    double const t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

// A fixed, off-pole unit direction for the sun lobe. Not a per-channel SH
// coefficient; this only selects where in the sky the sharp feature sits.
[[nodiscard]] ch26::spherical_harmonics::Float3 SunDirection() noexcept
{
    ch26::spherical_harmonics::Float3 const raw{0.35, 0.65, -0.60};
    double const length = std::sqrt((raw.x * raw.x) + (raw.y * raw.y) + (raw.z * raw.z));
    return {raw.x / length, raw.y / length, raw.z / length};
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
                                                   lgp::framework::ShaderCompileOptions &options,
                                                   wchar_t const *entryPoint, wchar_t const *targetProfile,
                                                   lgp::framework::CompiledShader &shader)
{
    options.entryPoint = entryPoint;
    options.targetProfile = targetProfile;
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
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
    if (size.width == 0U || size.height == 0U || size.width > kMaximumWidth || size.height > kMaximumHeight)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateExtent", "Chapter 26 requires a non-empty extent up to 640x360."));
    }
    return {};
}

[[nodiscard]] bool IsValidDebugView(DebugView view) noexcept
{
    return static_cast<std::uint32_t>(view) <= static_cast<std::uint32_t>(DebugView::BandContribution);
}

[[nodiscard]] lgp::framework::Error ContractFailure(char const *operation, ContractError error)
{
    return lgp::framework::MakeError(operation,
                                     "A Chapter 26 spherical-harmonics contract call failed with error code " +
                                         std::to_string(static_cast<int>(error)) + ".");
}

[[nodiscard]] SHCoefficientChannelLanes PackLanes(Coefficients const &coefficients) noexcept
{
    SHCoefficientChannelLanes lanes{};
    for (std::uint32_t lane = 0U; lane < 4U; ++lane)
    {
        std::uint32_t const base = lane * 4U;
        lanes.lanes[lane] = DirectX::XMFLOAT4(
            static_cast<float>(coefficients.values[base + 0U]), static_cast<float>(coefficients.values[base + 1U]),
            static_cast<float>(coefficients.values[base + 2U]), static_cast<float>(coefficients.values[base + 3U]));
    }
    return lanes;
}

[[nodiscard]] DirectX::XMFLOAT4 MeasureBandEnergy(Coefficients const &r, Coefficients const &g,
                                                  Coefficients const &b) noexcept
{
    std::array<float, kMaximumBand + 1U> energy{};
    for (std::uint32_t index = 0U; index < kMaximumCoefficientCount; ++index)
    {
        auto const bandOrder = DecodeCoefficientIndex(index);
        double const luminance = (0.2126 * r.values[index]) + (0.7152 * g.values[index]) + (0.0722 * b.values[index]);
        energy[bandOrder->band] += static_cast<float>(luminance * luminance);
    }
    return {energy[0], energy[1], energy[2], energy[3]};
}

} // namespace

lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration, LabVariant)
{
    if (configuration.activeBand > kMaximumBand)
    {
        return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration",
                                                         "The Chapter 26 active band must be in [0, kMaximumBand]."));
    }
    if (!std::isfinite(configuration.rotationDegrees))
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "The Chapter 26 rotation angle must be finite."));
    }
    if (!IsValidDebugView(configuration.debugView))
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "The Chapter 26 diagnostic view is invalid."));
    }
    return {};
}

ch26::spherical_harmonics::Float3 PixelDirection(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                                                 std::uint32_t height) noexcept
{
    double const theta = kPi * (static_cast<double>(y) + 0.5) / static_cast<double>(height);
    double const azimuth = 2.0 * kPi * (static_cast<double>(x) + 0.5) / static_cast<double>(width);
    double const cosTheta = std::cos(theta);
    double const sinTheta = std::sin(theta);
    return {sinTheta * std::cos(azimuth), cosTheta, sinTheta * std::sin(azimuth)};
}

RadianceRgb EvaluateEnvironment(ch26::spherical_harmonics::Float3 direction) noexcept
{
    double const skyFactor = SmoothStep(-0.2, 0.6, direction.y);
    double r = kSkyHorizonR + ((kSkyZenithR - kSkyHorizonR) * skyFactor);
    double g = kSkyHorizonG + ((kSkyZenithG - kSkyHorizonG) * skyFactor);
    double b = kSkyHorizonB + ((kSkyZenithB - kSkyHorizonB) * skyFactor);

    double const groundFactor = SmoothStep(0.05, -0.05, direction.y);
    r += (kGroundR - r) * groundFactor;
    g += (kGroundG - g) * groundFactor;
    b += (kGroundB - b) * groundFactor;

    ch26::spherical_harmonics::Float3 const sunDirection = SunDirection();
    double const cosineToSun = std::clamp(
        (direction.x * sunDirection.x) + (direction.y * sunDirection.y) + (direction.z * sunDirection.z), 0.0, 1.0);
    double const lobe = std::pow(cosineToSun, kSunSharpness);
    r += kSunColorR * kSunIntensity * lobe;
    g += kSunColorG * kSunIntensity * lobe;
    b += kSunColorB * kSunIntensity * lobe;

    return {r, g, b};
}

lgp::framework::TextureBarrierState FrameStartState(lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frameContext.renderTargetInitialLayout};
}

lgp::framework::TextureBarrierState FrameEndState(lgp::framework::FrameContext const &frameContext) noexcept
{
    return {
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT,
    };
}

lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
}

BufferBarrierState NoAccessState() noexcept
{
    return {};
}

BufferBarrierState ComputeUnorderedAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
}

BufferBarrierState PixelShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
}

BufferBarrierState CopySourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE};
}

D3D12_TEXTURE_BARRIER MakeTextureBarrier(ID3D12Resource &resource, lgp::framework::TextureBarrierState before,
                                         lgp::framework::TextureBarrierState after,
                                         D3D12_TEXTURE_BARRIER_FLAGS flags) noexcept
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
    barrier.Flags = flags;
    return barrier;
}

void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers)
{
    if (barriers.empty())
    {
        return;
    }
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = static_cast<UINT>(barriers.size());
    group.pTextureBarriers = barriers.data();
    commandList.Barrier(1U, &group);
}

D3D12_BUFFER_BARRIER MakeBufferBarrier(ID3D12Resource &resource, BufferBarrierState before,
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

void SubmitBufferBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_BUFFER_BARRIER> &barriers)
{
    if (barriers.empty())
    {
        return;
    }
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_BUFFER;
    group.NumBarriers = static_cast<UINT>(barriers.size());
    group.pBufferBarriers = barriers.data();
    commandList.Barrier(1U, &group);
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
        D3D12_RANGE const writtenRange{0U, 0U};
        resource_->Unmap(0U, &writtenRange);
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
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 26 buffers must be non-empty."));
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = heapType;
    heapProperties.CreationNodeMask = 1U;
    heapProperties.VisibleNodeMask = 1U;

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
        &heapProperties, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0U,
        nullptr, IID_PPV_ARGS(buffer.resource_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                                "Failed to create a Chapter 26 buffer."));
    }

    if (!name.empty())
    {
        std::wstring const resourceName{name};
        HRESULT const nameResult = buffer.resource_->SetName(resourceName.c_str());
        if (FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name a Chapter 26 buffer."));
        }
    }

    if (mapPersistently)
    {
        D3D12_RANGE const readRange{0U, static_cast<SIZE_T>(heapType == D3D12_HEAP_TYPE_READBACK ? sizeInBytes : 0U)};
        void *mapped = nullptr;
        HRESULT const mapResult = buffer.resource_->Map(0U, &readRange, &mapped);
        if (FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map a Chapter 26 buffer."));
        }
        buffer.mappedData_ = static_cast<std::byte *>(mapped);
    }

    buffer.sizeInBytes_ = sizeInBytes;
    return buffer;
}

RendererCore::RendererCore(std::filesystem::path shaderPath, LabVariant variant)
    : shaderPath_(std::move(shaderPath)), variant_(variant)
{
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
    D3D12_DESCRIPTOR_RANGE computeUavRange{};
    computeUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    computeUavRange.NumDescriptors = 1U;
    computeUavRange.BaseShaderRegister = 0U;
    computeUavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER computeParameters[3]{};
    computeParameters[ComputeConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[ComputeConstants].Constants.ShaderRegister = 0U;
    computeParameters[ComputeConstants].Constants.Num32BitValues = sizeof(DispatchConstants) / sizeof(std::uint32_t);
    computeParameters[ComputeCoefficients].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    computeParameters[ComputeCoefficients].Descriptor.ShaderRegister = 1U;
    computeParameters[ComputeDescriptorTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeDescriptorTable].DescriptorTable.NumDescriptorRanges = 1U;
    computeParameters[ComputeDescriptorTable].DescriptorTable.pDescriptorRanges = &computeUavRange;

    D3D12_ROOT_SIGNATURE_DESC computeDescription{};
    computeDescription.NumParameters = static_cast<UINT>(std::size(computeParameters));
    computeDescription.pParameters = computeParameters;

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
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", computeCreate,
                                             "Failed to create the Chapter 26 compute root signature."));
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1U;
    srvRange.BaseShaderRegister = 0U;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER graphicsParameters[2]{};
    graphicsParameters[GraphicsConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[GraphicsConstants].Constants.ShaderRegister = 0U;
    graphicsParameters[GraphicsConstants].Constants.Num32BitValues = sizeof(DisplayConstants) / sizeof(std::uint32_t);
    graphicsParameters[GraphicsConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    graphicsParameters[GraphicsSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParameters[GraphicsSrvTable].DescriptorTable.NumDescriptorRanges = 1U;
    graphicsParameters[GraphicsSrvTable].DescriptorTable.pDescriptorRanges = &srvRange;
    graphicsParameters[GraphicsSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
                                             "Failed to create the Chapter 26 graphics root signature."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreatePipelines()
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC computeDescription{};
    computeDescription.pRootSignature = computeRootSignature_.Get();
    computeDescription.CS = sampleShader_.Bytecode();
    HRESULT const computeResult = deviceResources_->device()->CreateComputePipelineState(
        &computeDescription, IID_PPV_ARGS(computePipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(computeResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState",
                                                                computeResult,
                                                                "Failed to create the Chapter 26 sampling pipeline."));
    }

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

    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsDescription{};
    graphicsDescription.pRootSignature = graphicsRootSignature_.Get();
    graphicsDescription.VS = vertexShader_.Bytecode();
    graphicsDescription.PS = pixelShader_.Bytecode();
    graphicsDescription.BlendState = blend;
    graphicsDescription.SampleMask = UINT_MAX;
    graphicsDescription.RasterizerState = rasterizer;
    graphicsDescription.DepthStencilState = depth;
    graphicsDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphicsDescription.NumRenderTargets = 1U;
    graphicsDescription.RTVFormats[0] = deviceResources_->back_buffer_format();
    graphicsDescription.SampleDesc.Count = 1U;

    HRESULT const graphicsResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &graphicsDescription, IID_PPV_ARGS(graphicsPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(graphicsResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState",
                                                                graphicsResult,
                                                                "Failed to create the Chapter 26 display pipeline."));
    }
    return {};
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
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch26 Pixel Statistics");
        auto readback = CreateBuffer(*deviceResources_->device(), statisticsBytes, D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_FLAG_NONE, L"Ch26 Statistics Readback", true);
        auto coefficients =
            CreateBuffer(*deviceResources_->device(), sizeof(SHCoefficientBundle), D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_FLAG_NONE, L"Ch26 SH Coefficients", true);
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
        if (!coefficients)
        {
            return std::unexpected(std::move(coefficients.error()));
        }

        slot.descriptors = *descriptors;
        slot.statistics = std::move(*statistics);
        slot.statisticsReadback = std::move(*readback);
        slot.coefficients = std::move(*coefficients);
        slot.initialized = false;

        D3D12_UNORDERED_ACCESS_VIEW_DESC statisticsUav{};
        statisticsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        statisticsUav.Buffer.NumElements = static_cast<UINT>(pixelCount);
        statisticsUav.Buffer.StructureByteStride = sizeof(PixelStatistics);
        deviceResources_->device()->CreateUnorderedAccessView(slot.statistics.Get(), nullptr, &statisticsUav,
                                                              slot.descriptors.CpuHandle(StatisticsUav));

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
    return headless_ && headlessConfiguration_.has_value() ? *headlessConfiguration_ : interactiveConfiguration_;
}

std::expected<SHCoefficientBundle, lgp::framework::Error> RendererCore::BuildCoefficientBundle(
    LabConfiguration const &configuration) const
{
    constexpr std::uint32_t width = kProjectionSampleWidth;
    constexpr std::uint32_t height = kProjectionSampleHeight;
    std::vector<double> valuesR(static_cast<std::size_t>(width) * height);
    std::vector<double> valuesG(valuesR.size());
    std::vector<double> valuesB(valuesR.size());
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        double const theta0 = kPi * static_cast<double>(row) / static_cast<double>(height);
        double const theta1 = kPi * static_cast<double>(row + 1U) / static_cast<double>(height);
        double const theta = 0.5 * (theta0 + theta1);
        double const y = std::cos(theta);
        double const radius = std::sin(theta);
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            double const azimuth = 2.0 * kPi * (static_cast<double>(column) + 0.5) / static_cast<double>(width);
            Float3 const direction{radius * std::cos(azimuth), y, radius * std::sin(azimuth)};
            RadianceRgb const radiance = EvaluateEnvironment(direction);
            std::size_t const index = static_cast<std::size_t>(row) * width + column;
            valuesR[index] = radiance.r;
            valuesG[index] = radiance.g;
            valuesB[index] = radiance.b;
        }
    }

    auto samplesR = BuildLatitudeLongitudeSamples(width, height, valuesR);
    if (!samplesR)
    {
        return std::unexpected(ContractFailure("BuildLatitudeLongitudeSamples[R]", samplesR.error()));
    }
    auto samplesG = BuildLatitudeLongitudeSamples(width, height, valuesG);
    if (!samplesG)
    {
        return std::unexpected(ContractFailure("BuildLatitudeLongitudeSamples[G]", samplesG.error()));
    }
    auto samplesB = BuildLatitudeLongitudeSamples(width, height, valuesB);
    if (!samplesB)
    {
        return std::unexpected(ContractFailure("BuildLatitudeLongitudeSamples[B]", samplesB.error()));
    }

    auto baseR = Project(kMaximumBand, *samplesR);
    if (!baseR)
    {
        return std::unexpected(ContractFailure("Project[R]", baseR.error()));
    }
    auto baseG = Project(kMaximumBand, *samplesG);
    if (!baseG)
    {
        return std::unexpected(ContractFailure("Project[G]", baseG.error()));
    }
    auto baseB = Project(kMaximumBand, *samplesB);
    if (!baseB)
    {
        return std::unexpected(ContractFailure("Project[B]", baseB.error()));
    }

    Matrix3 const rotation = RotationY(configuration.rotationDegrees * kPi / 180.0);
    auto coefficientRotation = BuildCoefficientRotation(kMaximumBand, rotation);
    if (!coefficientRotation)
    {
        return std::unexpected(ContractFailure("BuildCoefficientRotation", coefficientRotation.error()));
    }

    auto rotatedR = Rotate(*baseR, *coefficientRotation);
    if (!rotatedR)
    {
        return std::unexpected(ContractFailure("Rotate[R]", rotatedR.error()));
    }
    auto rotatedG = Rotate(*baseG, *coefficientRotation);
    if (!rotatedG)
    {
        return std::unexpected(ContractFailure("Rotate[G]", rotatedG.error()));
    }
    auto rotatedB = Rotate(*baseB, *coefficientRotation);
    if (!rotatedB)
    {
        return std::unexpected(ContractFailure("Rotate[B]", rotatedB.error()));
    }

    auto irradianceR = ConvolveClampedCosine(*baseR);
    if (!irradianceR)
    {
        return std::unexpected(ContractFailure("ConvolveClampedCosine[R]", irradianceR.error()));
    }
    auto irradianceG = ConvolveClampedCosine(*baseG);
    if (!irradianceG)
    {
        return std::unexpected(ContractFailure("ConvolveClampedCosine[G]", irradianceG.error()));
    }
    auto irradianceB = ConvolveClampedCosine(*baseB);
    if (!irradianceB)
    {
        return std::unexpected(ContractFailure("ConvolveClampedCosine[B]", irradianceB.error()));
    }

    SHCoefficientBundle bundle{};
    bundle.base.r = PackLanes(*baseR);
    bundle.base.g = PackLanes(*baseG);
    bundle.base.b = PackLanes(*baseB);
    bundle.rotated.r = PackLanes(*rotatedR);
    bundle.rotated.g = PackLanes(*rotatedG);
    bundle.rotated.b = PackLanes(*rotatedB);
    bundle.irradiance.r = PackLanes(*irradianceR);
    bundle.irradiance.g = PackLanes(*irradianceG);
    bundle.irradiance.b = PackLanes(*irradianceB);
    bundle.bandEnergy = MeasureBandEnergy(*baseR, *baseG, *baseB);
    return bundle;
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
    return CreatePipelines();
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

    if (context.input.WasKeyPressed('1'))
    {
        interactiveConfiguration_.debugView = DebugView::Source;
    }
    if (context.input.WasKeyPressed('2'))
    {
        interactiveConfiguration_.debugView = DebugView::Reconstruction;
    }
    if (context.input.WasKeyPressed('3'))
    {
        interactiveConfiguration_.debugView = DebugView::AbsoluteError;
    }
    if (context.input.WasKeyPressed('4'))
    {
        interactiveConfiguration_.debugView = DebugView::RotatedReconstruction;
    }
    if (context.input.WasKeyPressed('5'))
    {
        interactiveConfiguration_.debugView = DebugView::Irradiance;
    }
    if (context.input.WasKeyPressed('6'))
    {
        interactiveConfiguration_.debugView = DebugView::Interpolated;
    }
    if (context.input.WasKeyPressed('7'))
    {
        interactiveConfiguration_.debugView = DebugView::BandContribution;
    }
    if (context.input.WasKeyPressed('Q'))
    {
        interactiveConfiguration_.rotationDegrees -= 15.0;
    }
    if (context.input.WasKeyPressed('E'))
    {
        interactiveConfiguration_.rotationDegrees += 15.0;
    }
    if (context.input.WasKeyPressed('B'))
    {
        interactiveConfiguration_.activeBand = (interactiveConfiguration_.activeBand + 1U) % (kMaximumBand + 1U);
    }
    return {};
}

lgp::framework::Status RendererCore::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 26 frame slot is out of range."));
    }

    LabConfiguration const configuration = ActiveConfiguration();
    if (auto status = ValidateLabConfiguration(configuration, variant_); !status)
    {
        return status;
    }

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];

    auto bundle = BuildCoefficientBundle(configuration);
    if (!bundle)
    {
        return std::unexpected(std::move(bundle.error()));
    }
    std::memcpy(slot.coefficients.mutable_mapped_data(), &(*bundle), sizeof(SHCoefficientBundle));

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);

    BufferBarrierState const statisticsBefore = slot.initialized ? CopySourceState() : NoAccessState();
    std::vector<D3D12_BUFFER_BARRIER> bufferBarriers{
        MakeBufferBarrier(*slot.statistics.Get(), statisticsBefore, ComputeUnorderedAccessState()),
    };
    SubmitBufferBarriers(commandList, bufferBarriers);

    DispatchConstants const dispatch{
        size_.width,
        size_.height,
        configuration.activeBand,
        0U,
    };
    commandList.SetComputeRootSignature(computeRootSignature_.Get());
    commandList.SetComputeRoot32BitConstants(ComputeConstants, sizeof(dispatch) / sizeof(std::uint32_t), &dispatch, 0U);
    commandList.SetComputeRootConstantBufferView(ComputeCoefficients, slot.coefficients.Get()->GetGPUVirtualAddress());
    commandList.SetComputeRootDescriptorTable(ComputeDescriptorTable, slot.descriptors.GpuHandle(StatisticsUav));
    commandList.SetPipelineState(computePipeline_.Get());
    commandList.Dispatch((size_.width + 7U) / 8U, (size_.height + 7U) / 8U, 1U);

    bufferBarriers = {
        MakeBufferBarrier(*slot.statistics.Get(), ComputeUnorderedAccessState(), PixelShaderResourceState()),
    };
    SubmitBufferBarriers(commandList, bufferBarriers);

    std::vector<D3D12_TEXTURE_BARRIER> textureBarriers{
        MakeTextureBarrier(*frameContext.renderTarget, FrameStartState(frameContext), RenderTargetState()),
    };
    SubmitTextureBarriers(commandList, textureBarriers);

    float const clearColor[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
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
    commandList.SetGraphicsRootDescriptorTable(GraphicsSrvTable, slot.descriptors.GpuHandle(StatisticsSrv));
    commandList.SetPipelineState(graphicsPipeline_.Get());
    commandList.DrawInstanced(3U, 1U, 0U, 0U);

    bufferBarriers = {
        MakeBufferBarrier(*slot.statistics.Get(), PixelShaderResourceState(), CopySourceState()),
    };
    SubmitBufferBarriers(commandList, bufferBarriers);
    commandList.CopyBufferRegion(slot.statisticsReadback.Get(), 0U, slot.statistics.Get(), 0U,
                                 slot.statistics.size_in_bytes());

    textureBarriers = {
        MakeTextureBarrier(*frameContext.renderTarget, RenderTargetState(), FrameEndState(frameContext)),
    };
    SubmitTextureBarriers(commandList, textureBarriers);

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
            lgp::framework::MakeError("ReadBackOutputs", "Chapter 26 has no rendered frame to read back."));
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

} // namespace ch26::spherical_harmonics::gpu
