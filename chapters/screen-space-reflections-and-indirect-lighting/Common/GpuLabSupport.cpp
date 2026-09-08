#include "GpuLabSupport.hpp"

#include <lgp/framework/barriers.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <span>
#include <string>
#include <utility>

namespace ch29::screen_space_reflections::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorCount = 8U;
inline constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_R32_FLOAT;
inline constexpr double kVerticalFieldOfViewRadians = std::numbers::pi / 3.0;
inline constexpr double kNearPlane = 1.0;
inline constexpr double kFarPlane = 30.0;
inline constexpr float kConstantNormalBias = 0.01F;
inline constexpr float kDepthProportionalNormalBias = 0.002F;
inline constexpr float kMinimumFacingCosine = 1.0e-3F;
inline constexpr float kIndirectMaximumRadiance = 100.0F;
inline constexpr float kScreenEdgeFadeUv = 0.1F;
inline constexpr float kDistanceFadeStartFraction = 0.5F;
inline constexpr float kThicknessFadeFraction = 1.0F;
inline constexpr float kRoughnessFadeStart = 0.3F;
inline constexpr float kRoughnessFadeEnd = 0.8F;
inline constexpr float kTowardCameraFadeStart = 0.2F;
inline constexpr float kTowardCameraFadeEnd = 0.9F;

enum DescriptorIndex : UINT
{
    DepthUav = 0U,
    SurfaceUav = 1U,
    DiagnosticsUav = 2U,
    HistoryWriteUav = 3U,
    DepthSrv = 4U,
    SurfaceSrv = 5U,
    HistoryReadSrv = 6U,
    DiagnosticsSrv = 7U,
};

enum ComputeRootParameter : UINT
{
    ComputeConstants = 0U,
    ComputeOutputs = 1U,
    ComputeInputs = 2U,
};

enum GraphicsRootParameter : UINT
{
    GraphicsConstants = 0U,
    GraphicsRecords = 1U,
};

struct DispatchConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t flags{};
    std::uint32_t maximumStepCount{};
    std::uint32_t refinementStepCount{};
    std::uint32_t indirectSampleCount{};
    std::uint32_t indirectMaximumStepCount{};
    std::uint32_t splitSumSampleCount{};
    std::uint32_t maximumTemporalSampleCount{};
    std::uint32_t solutionMode{};
    float nearPlane{};
    float farPlane{};
    float tanHalfVerticalFov{};
    float tanHalfHorizontalFov{};
    float stepLengthTexels{};
    float startOffsetFraction{};
    float constantThickness{};
    float depthProportionalThickness{};
    float constantNormalBias{};
    float depthProportionalNormalBias{};
    float minimumFacingCosine{};
    float maximumRayDistance{};
    float indirectMaximumRayDistance{};
    float indirectMaximumRadiance{};
    float screenEdgeFadeUv{};
    float distanceFadeStartFraction{};
    float thicknessFadeFraction{};
    float roughnessFadeStart{};
    float roughnessFadeEnd{};
    float towardCameraFadeStart{};
    float towardCameraFadeEnd{};
    float maximumHistoryWeight{};
    float absoluteDepthTolerance{};
    float relativeDepthTolerance{};
    float boxCenterX{};
    float boxCenterZ{};
    float previousBoxCenterX{};
    float previousBoxCenterZ{};
    std::uint32_t materialIdentityRequired{};
    float constantsPadding{};
};
static_assert(sizeof(DispatchConstants) == 160U);

struct DisplayConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t debugView{};
    std::uint32_t expectedStatus{};
    std::uint32_t solutionMode{};
    float nearPlane{};
    float farPlane{};
    float padding{};
};
static_assert(sizeof(DisplayConstants) == 32U);

inline constexpr std::uint32_t kFlagReversedDepth = 1U << 0U;
inline constexpr std::uint32_t kFlagConstantEnvironment = 1U << 1U;
inline constexpr std::uint32_t kFlagScreenTracing = 1U << 2U;
inline constexpr std::uint32_t kFlagIndirect = 1U << 3U;
inline constexpr std::uint32_t kFlagTemporal = 1U << 4U;
inline constexpr std::uint32_t kFlagHistoryValid = 1U << 5U;
inline constexpr std::uint32_t kFlagReset = 1U << 6U;

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
                                                   wchar_t const *entryPoint, wchar_t const *profile,
                                                   lgp::framework::CompiledShader &shader)
{
    options.entryPoint = entryPoint;
    options.targetProfile = profile;
    options.additionalArguments = {L"-E", entryPoint, L"-T", profile};
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
            lgp::framework::MakeError("ValidateExtent", "Chapter 29 requires a non-empty extent up to 400x240."));
    }
    return {};
}

[[nodiscard]] BufferBarrierState NoAccessState() noexcept
{
    return {};
}
[[nodiscard]] BufferBarrierState ComputeUavState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
}
[[nodiscard]] BufferBarrierState ComputeSrvState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
}
[[nodiscard]] BufferBarrierState PixelSrvState() noexcept
{
    return {D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
}
[[nodiscard]] BufferBarrierState CopySourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE};
}

[[nodiscard]] lgp::framework::TextureBarrierState DepthUndefinedState() noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_UNDEFINED};
}
[[nodiscard]] lgp::framework::TextureBarrierState DepthWriteState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
            D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS};
}
[[nodiscard]] lgp::framework::TextureBarrierState DepthReadState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
            D3D12_BARRIER_LAYOUT_SHADER_RESOURCE};
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

void SubmitBufferBarriers(ID3D12GraphicsCommandList7 &list, std::span<D3D12_BUFFER_BARRIER> barriers)
{
    if (barriers.empty())
    {
        return;
    }
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_BUFFER;
    group.NumBarriers = static_cast<UINT>(barriers.size());
    group.pBufferBarriers = barriers.data();
    list.Barrier(1U, &group);
}

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(lgp::framework::FrameContext const &context) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, context.renderTargetInitialLayout};
}
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(lgp::framework::FrameContext const &context) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
            context.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT};
}
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
}

[[nodiscard]] bool SameFloat(float left, float right) noexcept
{
    return std::abs(left - right) <= 1.0e-6F;
}

} // namespace

lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration)
{
    if (static_cast<std::uint32_t>(configuration.debugView) >= kDebugViewCount)
    {
        return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration", "Unknown Chapter 29 view."));
    }
    if (configuration.depthConvention != DepthConvention::Forward &&
        configuration.depthConvention != DepthConvention::Reversed)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Unknown Chapter 29 depth convention."));
    }
    if (configuration.environmentMode != EnvironmentMode::Analytic &&
        configuration.environmentMode != EnvironmentMode::ConstantUniform)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Unknown Chapter 29 environment mode."));
    }
    if (configuration.maximumStepCount == 0U || configuration.maximumStepCount > kMaximumTraversalStepCount)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Maximum step count must be in [1,256]."));
    }
    if (configuration.indirectMaximumStepCount == 0U ||
        configuration.indirectMaximumStepCount > kMaximumTraversalStepCount)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Indirect step count must be in [1,256]."));
    }
    if (configuration.refinementStepCount > kMaximumRefinementStepCount)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Refinement step count must be at most 16."));
    }
    if (configuration.indirectSampleCount == 0U || configuration.indirectSampleCount > 64U)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Indirect sample count must be in [1,64]."));
    }
    if (configuration.splitSumSampleCount == 0U || configuration.splitSumSampleCount > 256U)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Split-sum sample count must be in [1,256]."));
    }
    if (configuration.maximumTemporalSampleCount == 0U || configuration.maximumTemporalSampleCount > 64U)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Temporal sample count must be in [1,64]."));
    }
    if (!std::isfinite(configuration.stepLengthTexels) || configuration.stepLengthTexels <= 0.0F ||
        configuration.stepLengthTexels > 64.0F)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Step length must be in (0,64] texels."));
    }
    if (!std::isfinite(configuration.startOffsetFraction) || configuration.startOffsetFraction < 0.0F ||
        configuration.startOffsetFraction >= 1.0F)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Start offset fraction must be in [0,1)."));
    }
    if (!std::isfinite(configuration.constantThickness) || configuration.constantThickness < 0.0F ||
        !std::isfinite(configuration.depthProportionalThickness) || configuration.depthProportionalThickness < 0.0F ||
        (configuration.constantThickness == 0.0F && configuration.depthProportionalThickness == 0.0F))
    {
        // A zero-width thickness interval can only accept an exact floating-point coincidence, which is never a
        // teachable outcome.
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Thickness interval must be positive somewhere."));
    }
    if (!std::isfinite(configuration.maximumRayDistance) || configuration.maximumRayDistance <= 0.0F ||
        configuration.maximumRayDistance > 1.0e4F || !std::isfinite(configuration.indirectMaximumRayDistance) ||
        configuration.indirectMaximumRayDistance <= 0.0F || configuration.indirectMaximumRayDistance > 1.0e4F)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Ray distances must be finite and positive."));
    }
    if (!std::isfinite(configuration.maximumHistoryWeight) || configuration.maximumHistoryWeight < 0.0F ||
        configuration.maximumHistoryWeight >= 1.0F)
    {
        // A ceiling of one would let a usable history sample erase the current frame and freeze stale lighting.
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Maximum history weight must be in [0,1)."));
    }
    if (!std::isfinite(configuration.absoluteDepthTolerance) || configuration.absoluteDepthTolerance < 0.0F ||
        !std::isfinite(configuration.relativeDepthTolerance) || configuration.relativeDepthTolerance < 0.0F)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Depth tolerances must be finite and non-negative."));
    }
    return {};
}

PerspectiveProjection MakeProjection(LabConfiguration const &configuration, lgp::framework::Extent2D extent) noexcept
{
    return PerspectiveProjection{
        .verticalFieldOfViewRadians = kVerticalFieldOfViewRadians,
        .aspectRatio =
            extent.height == 0U ? 1.0 : static_cast<double>(extent.width) / static_cast<double>(extent.height),
        .nearPlane = kNearPlane,
        .farPlane = kFarPlane,
        .depthConvention = configuration.depthConvention,
    };
}

RayConstructionSettings MakeRaySettings(LabConfiguration const &configuration) noexcept
{
    return RayConstructionSettings{
        .constantNormalBias = kConstantNormalBias,
        .depthProportionalNormalBias = kDepthProportionalNormalBias,
        .minimumFacingCosine = kMinimumFacingCosine,
        .maximumRayDistance = configuration.maximumRayDistance,
    };
}

TraceSettings MakeTraceSettings(LabConfiguration const &configuration) noexcept
{
    return TraceSettings{
        .stepLengthTexels = configuration.stepLengthTexels,
        .maximumStepCount = configuration.maximumStepCount,
        .startOffsetFraction = configuration.startOffsetFraction,
        .constantThickness = configuration.constantThickness,
        .depthProportionalThickness = configuration.depthProportionalThickness,
        .refinementStepCount = configuration.refinementStepCount,
    };
}

ConfidenceSettings MakeConfidenceSettings(LabConfiguration const &) noexcept
{
    return ConfidenceSettings{
        .screenEdgeFadeUv = kScreenEdgeFadeUv,
        .distanceFadeStartFraction = kDistanceFadeStartFraction,
        .thicknessFadeFraction = kThicknessFadeFraction,
        .roughnessFadeStart = kRoughnessFadeStart,
        .roughnessFadeEnd = kRoughnessFadeEnd,
        .towardCameraFadeStart = kTowardCameraFadeStart,
        .towardCameraFadeEnd = kTowardCameraFadeEnd,
    };
}

TemporalReuseSettings MakeTemporalSettings(LabConfiguration const &configuration) noexcept
{
    return TemporalReuseSettings{
        .maximumSampleCount = configuration.maximumTemporalSampleCount,
        .maximumHistoryWeight = configuration.maximumHistoryWeight,
        .absoluteDepthTolerance = configuration.absoluteDepthTolerance,
        .relativeDepthTolerance = configuration.relativeDepthTolerance,
        .requireMaterialIdentity = configuration.materialIdentityRequired,
    };
}

// The emitter slides along +/-X and also toward and away from the eye so the lab has real motion vectors, a
// reflection that moves independently of the surface carrying it, and a history depth that genuinely changes
// between frames. The phase is evaluated on the CPU and uploaded, so the shader and the tests see one value.
EmitterCenter BoxCenter(std::uint32_t animationFrame) noexcept
{
    float const frame = static_cast<float>(animationFrame);
    return EmitterCenter{
        .x = -1.5F + (0.9F * std::sin(0.21F * frame)),
        .z = 14.0F + (0.6F * std::sin(0.17F * frame)),
    };
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
        D3D12_RANGE const range{0U, 0U};
        resource_->Unmap(0U, &range);
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
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 29 buffers must be non-empty."));
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

    BufferResource result{};
    HRESULT const createResult = device.CreateCommittedResource3(
        &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0U, nullptr,
        IID_PPV_ARGS(result.resource_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3",
                                                                createResult, "Failed to create Chapter 29 buffer."));
    }
    if (!name.empty())
    {
        std::wstring const ownedName{name};
        if (HRESULT const nameResult = result.resource_->SetName(ownedName.c_str()); FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name Chapter 29 buffer."));
        }
    }
    if (mapPersistently)
    {
        D3D12_RANGE const readRange{0U, static_cast<SIZE_T>(sizeInBytes)};
        void *mapping = nullptr;
        if (HRESULT const mapResult = result.resource_->Map(0U, &readRange, &mapping); FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map Chapter 29 readback."));
        }
        result.mappedData_ = static_cast<std::byte *>(mapping);
    }
    result.sizeInBytes_ = sizeInBytes;
    return result;
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
    options.includeDirectories = {shaderPath_.parent_path(), shaderPath_.parent_path().parent_path() / "Common"};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif
    if (auto status = CompileShader(compiler, options, L"GBufferCS", L"cs_6_0", gbufferShader_); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"ScreenSpaceCS", L"cs_6_0", screenSpaceShader_); !status)
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
    D3D12_DESCRIPTOR_RANGE outputRange{};
    outputRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRange.NumDescriptors = 4U;
    outputRange.BaseShaderRegister = 0U;
    D3D12_DESCRIPTOR_RANGE inputRange{};
    inputRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    inputRange.NumDescriptors = 3U;
    inputRange.BaseShaderRegister = 0U;
    D3D12_ROOT_PARAMETER computeParameters[3]{};
    computeParameters[ComputeConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[ComputeConstants].Constants.ShaderRegister = 0U;
    computeParameters[ComputeConstants].Constants.Num32BitValues = sizeof(DispatchConstants) / sizeof(std::uint32_t);
    computeParameters[ComputeOutputs].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeOutputs].DescriptorTable = {1U, &outputRange};
    computeParameters[ComputeInputs].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeInputs].DescriptorTable = {1U, &inputRange};
    D3D12_ROOT_SIGNATURE_DESC computeDescription{};
    computeDescription.NumParameters = static_cast<UINT>(std::size(computeParameters));
    computeDescription.pParameters = computeParameters;

    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};
    HRESULT serializeResult = D3D12SerializeRootSignature(&computeDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                                          serialized.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(serializeResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult, BlobText(errors.Get())));
    }
    HRESULT createResult =
        deviceResources_->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(computeRootSignature_.GetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                                                "Failed to create Chapter 29 compute root signature."));
    }

    D3D12_DESCRIPTOR_RANGE recordRange{};
    recordRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    recordRange.NumDescriptors = 1U;
    recordRange.BaseShaderRegister = 0U;
    D3D12_ROOT_PARAMETER graphicsParameters[2]{};
    graphicsParameters[GraphicsConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[GraphicsConstants].Constants.ShaderRegister = 0U;
    graphicsParameters[GraphicsConstants].Constants.Num32BitValues = sizeof(DisplayConstants) / sizeof(std::uint32_t);
    graphicsParameters[GraphicsConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    graphicsParameters[GraphicsRecords].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParameters[GraphicsRecords].DescriptorTable = {1U, &recordRange};
    graphicsParameters[GraphicsRecords].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC graphicsDescription{};
    graphicsDescription.NumParameters = static_cast<UINT>(std::size(graphicsParameters));
    graphicsDescription.pParameters = graphicsParameters;
    graphicsDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    serialized.Reset();
    errors.Reset();
    serializeResult = D3D12SerializeRootSignature(&graphicsDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  serialized.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(serializeResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult, BlobText(errors.Get())));
    }
    createResult =
        deviceResources_->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(graphicsRootSignature_.GetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device::CreateRootSignature", createResult, "Failed to create Chapter 29 graphics root signature."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreatePipelines()
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = computeRootSignature_.Get();
    compute.CS = gbufferShader_.Bytecode();
    HRESULT result =
        deviceResources_->device()->CreateComputePipelineState(&compute, IID_PPV_ARGS(gbufferPipeline_.GetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState", result,
                                                                "Failed to create Chapter 29 G-buffer PSO."));
    }
    compute.CS = screenSpaceShader_.Bytecode();
    result = deviceResources_->device()->CreateComputePipelineState(&compute,
                                                                    IID_PPV_ARGS(screenSpacePipeline_.GetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState", result,
                                                                "Failed to create Chapter 29 screen-space PSO."));
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
    result = deviceResources_->device()->CreateGraphicsPipelineState(&graphics,
                                                                     IID_PPV_ARGS(graphicsPipeline_.GetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create Chapter 29 graphics PSO."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreateResources(lgp::framework::Extent2D size)
{
    if (auto status = ValidateExtent(size); !status)
    {
        return status;
    }
    ID3D12Device10 &device = *deviceResources_->device();
    std::uint64_t const count = static_cast<std::uint64_t>(size.width) * size.height;
    std::uint64_t const recordBytes = count * sizeof(PixelRecord);
    std::uint64_t const surfaceBytes = count * sizeof(SurfaceRecord);
    std::uint64_t const historyBytes = count * sizeof(HistoryPixel);

    for (std::uint32_t index = 0U; index < history_.size(); ++index)
    {
        auto history =
            CreateBuffer(device, historyBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         index == 0U ? L"Ch29 history A" : L"Ch29 history B");
        if (!history)
        {
            return std::unexpected(std::move(history.error()));
        }
        history_[index] = std::move(*history);
        historyStates_[index] = NoAccessState();
    }

    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlotResources &slot : frameSlots_)
    {
        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorCount);
        if (!descriptors)
        {
            return std::unexpected(std::move(descriptors.error()));
        }
        slot.descriptors = *descriptors;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1U;
        heap.VisibleNodeMask = 1U;
        D3D12_RESOURCE_DESC1 depthDescription{};
        depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDescription.Width = size.width;
        depthDescription.Height = size.height;
        depthDescription.DepthOrArraySize = 1U;
        depthDescription.MipLevels = 1U;
        depthDescription.Format = kDepthFormat;
        depthDescription.SampleDesc.Count = 1U;
        depthDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT const depthResult = device.CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &depthDescription, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0U,
            nullptr, IID_PPV_ARGS(slot.depth.ReleaseAndGetAddressOf()));
        if (FAILED(depthResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError(
                "ID3D12Device10::CreateCommittedResource3", depthResult, "Failed to create Chapter 29 depth image."));
        }
        (void)slot.depth->SetName(L"Ch29 screen depth");

        auto surface = CreateBuffer(device, surfaceBytes, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch29 surface records");
        auto diagnostics = CreateBuffer(device, recordBytes, D3D12_HEAP_TYPE_DEFAULT,
                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch29 pixel diagnostics");
        auto readback = CreateBuffer(device, recordBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                     L"Ch29 diagnostics readback", true);
        if (!surface)
        {
            return std::unexpected(std::move(surface.error()));
        }
        if (!diagnostics)
        {
            return std::unexpected(std::move(diagnostics.error()));
        }
        if (!readback)
        {
            return std::unexpected(std::move(readback.error()));
        }
        slot.surface = std::move(*surface);
        slot.diagnostics = std::move(*diagnostics);
        slot.readback = std::move(*readback);

        D3D12_UNORDERED_ACCESS_VIEW_DESC depthUav{};
        depthUav.Format = kDepthFormat;
        depthUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device.CreateUnorderedAccessView(slot.depth.Get(), nullptr, &depthUav, slot.descriptors.CpuHandle(DepthUav));
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
        depthSrv.Format = kDepthFormat;
        depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrv.Texture2D.MipLevels = 1U;
        device.CreateShaderResourceView(slot.depth.Get(), &depthSrv, slot.descriptors.CpuHandle(DepthSrv));

        D3D12_UNORDERED_ACCESS_VIEW_DESC surfaceUav{};
        surfaceUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        surfaceUav.Buffer.NumElements = static_cast<UINT>(count);
        surfaceUav.Buffer.StructureByteStride = sizeof(SurfaceRecord);
        device.CreateUnorderedAccessView(slot.surface.Get(), nullptr, &surfaceUav,
                                         slot.descriptors.CpuHandle(SurfaceUav));
        D3D12_SHADER_RESOURCE_VIEW_DESC surfaceSrv{};
        surfaceSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        surfaceSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        surfaceSrv.Buffer.NumElements = static_cast<UINT>(count);
        surfaceSrv.Buffer.StructureByteStride = sizeof(SurfaceRecord);
        device.CreateShaderResourceView(slot.surface.Get(), &surfaceSrv, slot.descriptors.CpuHandle(SurfaceSrv));

        D3D12_UNORDERED_ACCESS_VIEW_DESC recordUav{};
        recordUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        recordUav.Buffer.NumElements = static_cast<UINT>(count);
        recordUav.Buffer.StructureByteStride = sizeof(PixelRecord);
        device.CreateUnorderedAccessView(slot.diagnostics.Get(), nullptr, &recordUav,
                                         slot.descriptors.CpuHandle(DiagnosticsUav));
        D3D12_SHADER_RESOURCE_VIEW_DESC recordSrv{};
        recordSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        recordSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        recordSrv.Buffer.NumElements = static_cast<UINT>(count);
        recordSrv.Buffer.StructureByteStride = sizeof(PixelRecord);
        device.CreateShaderResourceView(slot.diagnostics.Get(), &recordSrv, slot.descriptors.CpuHandle(DiagnosticsSrv));
    }

    size_ = size;
    historyReadIndex_ = 0U;
    historyValid_ = false;
    forceReset_ = true;
    hasRendered_ = false;
    return {};
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
    DestroyResources(deviceResources);
    return CreateResources(drawableSize);
}

lgp::framework::Status RendererCore::Update(lgp::framework::UpdateContext const &context)
{
    if (headless_)
    {
        return {};
    }
    bool const solution = variant_ == LabVariant::Solution;
    if (context.input.WasKeyPressed('R'))
    {
        forceReset_ = true;
        interactiveConfiguration_.animationFrame = 0U;
    }
    if (context.input.WasKeyPressed('D'))
    {
        interactiveConfiguration_.depthConvention =
            interactiveConfiguration_.depthConvention == DepthConvention::Forward ? DepthConvention::Reversed
                                                                                  : DepthConvention::Forward;
    }
    if (context.input.WasKeyPressed('E'))
    {
        interactiveConfiguration_.environmentMode =
            interactiveConfiguration_.environmentMode == EnvironmentMode::Analytic ? EnvironmentMode::ConstantUniform
                                                                                   : EnvironmentMode::Analytic;
    }
    // Solution-only controls are ignored by the Starter rather than rejected: the Starter has no screen-space state
    // for them to change.
    if (solution && context.input.WasKeyPressed('S'))
    {
        interactiveConfiguration_.screenTracingEnabled = !interactiveConfiguration_.screenTracingEnabled;
    }
    if (solution && context.input.WasKeyPressed('G'))
    {
        interactiveConfiguration_.indirectEnabled = !interactiveConfiguration_.indirectEnabled;
    }
    if (solution && context.input.WasKeyPressed('T'))
    {
        interactiveConfiguration_.temporalEnabled = !interactiveConfiguration_.temporalEnabled;
    }
    if (solution && context.input.WasKeyPressed('B'))
    {
        interactiveConfiguration_.stepLengthTexels = interactiveConfiguration_.stepLengthTexels >= 8.0F
                                                         ? 1.0F
                                                         : interactiveConfiguration_.stepLengthTexels * 2.0F;
    }
    if (solution && context.input.WasKeyPressed('M'))
    {
        interactiveConfiguration_.maximumStepCount =
            interactiveConfiguration_.maximumStepCount >= 96U ? 8U : interactiveConfiguration_.maximumStepCount * 2U;
    }
    for (std::uint32_t view = 0U; view < kDebugViewCount; ++view)
    {
        DebugView const candidate = static_cast<DebugView>(view);
        bool const solutionOnly = view >= static_cast<std::uint32_t>(DebugView::HitClassification);
        // Views one to nine are the digit keys, the tenth is zero, and the history view has no digit left.
        std::uint32_t key = '1' + view;
        if (candidate == DebugView::FinalComposition)
        {
            key = '0';
        }
        else if (candidate == DebugView::TemporalHistory)
        {
            key = 'H';
        }
        if ((solution || !solutionOnly) && context.input.WasKeyPressed(key))
        {
            interactiveConfiguration_.debugView = candidate;
        }
    }
    ++interactiveConfiguration_.animationFrame;
    return {};
}

LabConfiguration RendererCore::ActiveConfiguration() const noexcept
{
    return headless_ && headlessConfiguration_ ? *headlessConfiguration_ : interactiveConfiguration_;
}

bool RendererCore::HistoryAffectingConfigurationChanged(LabConfiguration const &configuration) const noexcept
{
    if (!hasRendered_)
    {
        return true;
    }
    bool const animationReset = configuration.animationFrame < lastHistoryConfiguration_.animationFrame;
    return animationReset || configuration.depthConvention != lastHistoryConfiguration_.depthConvention ||
           configuration.environmentMode != lastHistoryConfiguration_.environmentMode ||
           configuration.screenTracingEnabled != lastHistoryConfiguration_.screenTracingEnabled ||
           configuration.indirectEnabled != lastHistoryConfiguration_.indirectEnabled ||
           configuration.temporalEnabled != lastHistoryConfiguration_.temporalEnabled ||
           configuration.maximumStepCount != lastHistoryConfiguration_.maximumStepCount ||
           configuration.refinementStepCount != lastHistoryConfiguration_.refinementStepCount ||
           configuration.indirectSampleCount != lastHistoryConfiguration_.indirectSampleCount ||
           configuration.indirectMaximumStepCount != lastHistoryConfiguration_.indirectMaximumStepCount ||
           configuration.splitSumSampleCount != lastHistoryConfiguration_.splitSumSampleCount ||
           configuration.maximumTemporalSampleCount != lastHistoryConfiguration_.maximumTemporalSampleCount ||
           configuration.materialIdentityRequired != lastHistoryConfiguration_.materialIdentityRequired ||
           !SameFloat(configuration.maximumHistoryWeight, lastHistoryConfiguration_.maximumHistoryWeight) ||
           !SameFloat(configuration.absoluteDepthTolerance, lastHistoryConfiguration_.absoluteDepthTolerance) ||
           !SameFloat(configuration.relativeDepthTolerance, lastHistoryConfiguration_.relativeDepthTolerance) ||
           !SameFloat(configuration.stepLengthTexels, lastHistoryConfiguration_.stepLengthTexels) ||
           !SameFloat(configuration.startOffsetFraction, lastHistoryConfiguration_.startOffsetFraction) ||
           !SameFloat(configuration.constantThickness, lastHistoryConfiguration_.constantThickness) ||
           !SameFloat(configuration.depthProportionalThickness, lastHistoryConfiguration_.depthProportionalThickness) ||
           !SameFloat(configuration.maximumRayDistance, lastHistoryConfiguration_.maximumRayDistance) ||
           !SameFloat(configuration.indirectMaximumRayDistance, lastHistoryConfiguration_.indirectMaximumRayDistance);
}

lgp::framework::Status RendererCore::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Render", "Chapter 29 frame slot is out of range."));
    }
    LabConfiguration const configuration = ActiveConfiguration();
    if (auto status = ValidateLabConfiguration(configuration); !status)
    {
        return status;
    }

    bool const configurationReset = HistoryAffectingConfigurationChanged(configuration);
    bool const reset = forceReset_ || configuration.resetHistory || configurationReset;
    bool const canReadHistory = variant_ == LabVariant::Solution && historyValid_ && !reset;
    // The history buffer holds whatever the previous rendered frame produced, so reprojection must be expressed
    // against that frame's configuration. Deriving it as animationFrame - 1 would be a guess: the sequence-owned
    // history survives repeated and skipped frame numbers, and only a rewind or a history-affecting change
    // invalidates it. Chapter 28 reprojects against the recorded history configuration for the same reason.
    std::uint32_t const previousAnimationFrame =
        canReadHistory ? lastHistoryConfiguration_.animationFrame : configuration.animationFrame;

    PerspectiveProjection const projection = MakeProjection(configuration, size_);
    double const tanHalfVertical = std::tan(projection.verticalFieldOfViewRadians * 0.5);

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    std::uint32_t const writeIndex = 1U - historyReadIndex_;
    std::uint64_t const pixelCount = static_cast<std::uint64_t>(size_.width) * size_.height;
    D3D12_UNORDERED_ACCESS_VIEW_DESC historyUav{};
    historyUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    historyUav.Buffer.NumElements = static_cast<UINT>(pixelCount);
    historyUav.Buffer.StructureByteStride = sizeof(HistoryPixel);
    deviceResources_->device()->CreateUnorderedAccessView(history_[writeIndex].Get(), nullptr, &historyUav,
                                                          slot.descriptors.CpuHandle(HistoryWriteUav));
    D3D12_SHADER_RESOURCE_VIEW_DESC historySrv{};
    historySrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    historySrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    historySrv.Buffer.NumElements = static_cast<UINT>(pixelCount);
    historySrv.Buffer.StructureByteStride = sizeof(HistoryPixel);
    deviceResources_->device()->CreateShaderResourceView(history_[historyReadIndex_].Get(), &historySrv,
                                                         slot.descriptors.CpuHandle(HistoryReadSrv));

    ID3D12GraphicsCommandList7 &list = *frameContext.commandList;
    ID3D12DescriptorHeap *heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    list.SetDescriptorHeaps(1U, heaps);

    std::array<D3D12_BUFFER_BARRIER, 4U> beforeBarriers{
        MakeBufferBarrier(*slot.surface.Get(), slot.initialized ? ComputeSrvState() : NoAccessState(),
                          ComputeUavState()),
        MakeBufferBarrier(*slot.diagnostics.Get(), slot.initialized ? CopySourceState() : NoAccessState(),
                          ComputeUavState()),
        MakeBufferBarrier(*history_[writeIndex].Get(), historyStates_[writeIndex], ComputeUavState()),
        MakeBufferBarrier(*history_[historyReadIndex_].Get(), historyStates_[historyReadIndex_], ComputeSrvState()),
    };
    SubmitBufferBarriers(list, beforeBarriers);
    lgp::framework::TransitionTexture(list, *slot.depth.Get(),
                                      slot.initialized ? DepthReadState() : DepthUndefinedState(), DepthWriteState());
    historyStates_[writeIndex] = ComputeUavState();
    historyStates_[historyReadIndex_] = ComputeSrvState();

    std::uint32_t flags = 0U;
    flags |= configuration.depthConvention == DepthConvention::Reversed ? kFlagReversedDepth : 0U;
    flags |= configuration.environmentMode == EnvironmentMode::ConstantUniform ? kFlagConstantEnvironment : 0U;
    flags |= configuration.screenTracingEnabled ? kFlagScreenTracing : 0U;
    flags |= configuration.indirectEnabled ? kFlagIndirect : 0U;
    flags |= configuration.temporalEnabled ? kFlagTemporal : 0U;
    flags |= canReadHistory ? kFlagHistoryValid : 0U;
    flags |= reset ? kFlagReset : 0U;

    DispatchConstants const constants{
        .width = size_.width,
        .height = size_.height,
        .flags = flags,
        .maximumStepCount = configuration.maximumStepCount,
        .refinementStepCount = configuration.refinementStepCount,
        .indirectSampleCount = configuration.indirectSampleCount,
        .indirectMaximumStepCount = configuration.indirectMaximumStepCount,
        .splitSumSampleCount = configuration.splitSumSampleCount,
        .maximumTemporalSampleCount = configuration.maximumTemporalSampleCount,
        .solutionMode = variant_ == LabVariant::Solution ? 1U : 0U,
        .nearPlane = static_cast<float>(projection.nearPlane),
        .farPlane = static_cast<float>(projection.farPlane),
        .tanHalfVerticalFov = static_cast<float>(tanHalfVertical),
        .tanHalfHorizontalFov = static_cast<float>(tanHalfVertical * projection.aspectRatio),
        .stepLengthTexels = configuration.stepLengthTexels,
        .startOffsetFraction = configuration.startOffsetFraction,
        .constantThickness = configuration.constantThickness,
        .depthProportionalThickness = configuration.depthProportionalThickness,
        .constantNormalBias = kConstantNormalBias,
        .depthProportionalNormalBias = kDepthProportionalNormalBias,
        .minimumFacingCosine = kMinimumFacingCosine,
        .maximumRayDistance = configuration.maximumRayDistance,
        .indirectMaximumRayDistance = configuration.indirectMaximumRayDistance,
        .indirectMaximumRadiance = kIndirectMaximumRadiance,
        .screenEdgeFadeUv = kScreenEdgeFadeUv,
        .distanceFadeStartFraction = kDistanceFadeStartFraction,
        .thicknessFadeFraction = kThicknessFadeFraction,
        .roughnessFadeStart = kRoughnessFadeStart,
        .roughnessFadeEnd = kRoughnessFadeEnd,
        .towardCameraFadeStart = kTowardCameraFadeStart,
        .towardCameraFadeEnd = kTowardCameraFadeEnd,
        .maximumHistoryWeight = configuration.maximumHistoryWeight,
        .absoluteDepthTolerance = configuration.absoluteDepthTolerance,
        .relativeDepthTolerance = configuration.relativeDepthTolerance,
        .boxCenterX = BoxCenter(configuration.animationFrame).x,
        .boxCenterZ = BoxCenter(configuration.animationFrame).z,
        .previousBoxCenterX = BoxCenter(previousAnimationFrame).x,
        .previousBoxCenterZ = BoxCenter(previousAnimationFrame).z,
        .materialIdentityRequired = configuration.materialIdentityRequired ? 1U : 0U,
        .constantsPadding = 0.0F,
    };

    list.SetComputeRootSignature(computeRootSignature_.Get());
    list.SetComputeRoot32BitConstants(ComputeConstants, sizeof(constants) / sizeof(std::uint32_t), &constants, 0U);
    list.SetComputeRootDescriptorTable(ComputeOutputs, slot.descriptors.GpuHandle(DepthUav));
    list.SetComputeRootDescriptorTable(ComputeInputs, slot.descriptors.GpuHandle(DepthSrv));
    list.SetPipelineState(gbufferPipeline_.Get());
    list.Dispatch((size_.width + 7U) / 8U, (size_.height + 7U) / 8U, 1U);

    // The G-buffer producer must finish and change layout before the screen-space consumer may read it.
    std::array<D3D12_BUFFER_BARRIER, 1U> gbufferBarriers{
        MakeBufferBarrier(*slot.surface.Get(), ComputeUavState(), ComputeSrvState()),
    };
    SubmitBufferBarriers(list, gbufferBarriers);
    lgp::framework::TransitionTexture(list, *slot.depth.Get(), DepthWriteState(), DepthReadState());

    list.SetPipelineState(screenSpacePipeline_.Get());
    list.Dispatch((size_.width + 7U) / 8U, (size_.height + 7U) / 8U, 1U);

    std::array<D3D12_BUFFER_BARRIER, 2U> afterBarriers{
        MakeBufferBarrier(*slot.diagnostics.Get(), ComputeUavState(), PixelSrvState()),
        MakeBufferBarrier(*history_[writeIndex].Get(), ComputeUavState(), ComputeSrvState()),
    };
    SubmitBufferBarriers(list, afterBarriers);
    historyStates_[writeIndex] = ComputeSrvState();

    lgp::framework::TransitionTexture(list, *frameContext.renderTarget, FrameStartState(frameContext),
                                      RenderTargetState());
    float const clear[]{0.0F, 0.0F, 0.0F, 1.0F};
    list.ClearRenderTargetView(frameContext.renderTargetView, clear, 0U, nullptr);
    list.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    list.RSSetViewports(1U, &frameContext.viewport);
    list.RSSetScissorRects(1U, &frameContext.scissorRect);
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    DisplayConstants const display{
        .width = size_.width,
        .height = size_.height,
        .debugView = static_cast<std::uint32_t>(configuration.debugView),
        .expectedStatus = variant_ == LabVariant::Solution ? kSolutionValidStatus : kStarterValidStatus,
        .solutionMode = variant_ == LabVariant::Solution ? 1U : 0U,
        .nearPlane = static_cast<float>(projection.nearPlane),
        .farPlane = static_cast<float>(projection.farPlane),
        .padding = 0.0F,
    };
    list.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    list.SetGraphicsRoot32BitConstants(GraphicsConstants, sizeof(display) / sizeof(std::uint32_t), &display, 0U);
    list.SetGraphicsRootDescriptorTable(GraphicsRecords, slot.descriptors.GpuHandle(DiagnosticsSrv));
    list.SetPipelineState(graphicsPipeline_.Get());
    list.DrawInstanced(3U, 1U, 0U, 0U);

    D3D12_BUFFER_BARRIER copyBarrier = MakeBufferBarrier(*slot.diagnostics.Get(), PixelSrvState(), CopySourceState());
    SubmitBufferBarriers(list, std::span{&copyBarrier, 1U});
    list.CopyBufferRegion(slot.readback.Get(), 0U, slot.diagnostics.Get(), 0U, slot.diagnostics.size_in_bytes());
    lgp::framework::TransitionTexture(list, *frameContext.renderTarget, RenderTargetState(),
                                      FrameEndState(frameContext));

    slot.initialized = true;
    if (variant_ == LabVariant::Solution)
    {
        historyReadIndex_ = writeIndex;
        historyValid_ = true;
    }
    lastRenderedConfiguration_ = configuration;
    lastHistoryConfiguration_ = configuration;
    lastHistoryWasValid_ = canReadHistory;
    lastPreviousAnimationFrame_ = previousAnimationFrame;
    lastRenderedFrameSlot_ = frameContext.frameSlot;
    hasRendered_ = true;
    forceReset_ = false;
    return {};
}

void RendererCore::DestroyResources(lgp::framework::DeviceResources &deviceResources) noexcept
{
    for (FrameSlotResources &slot : frameSlots_)
    {
        if (slot.descriptors)
        {
            deviceResources.shader_visible_cbv_srv_uav_heap().Free(slot.descriptors);
        }
    }
    frameSlots_.clear();
    history_ = {};
    historyStates_ = {};
    historyValid_ = false;
    hasRendered_ = false;
    size_ = {};
}

void RendererCore::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    DestroyResources(deviceResources);
    graphicsPipeline_.Reset();
    screenSpacePipeline_.Reset();
    gbufferPipeline_.Reset();
    graphicsRootSignature_.Reset();
    computeRootSignature_.Reset();
    gbufferShader_ = {};
    screenSpaceShader_ = {};
    vertexShader_ = {};
    pixelShader_ = {};
    deviceResources_ = nullptr;
}

void RendererCore::ConfigureHeadlessTest(LabConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

void RendererCore::RequestHistoryReset() noexcept
{
    forceReset_ = true;
}

std::expected<FrameReadback, lgp::framework::Error> RendererCore::ReadBackOutputs()
{
    if (deviceResources_ == nullptr || !hasRendered_ || frameSlots_.empty())
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "Chapter 29 has no completed frame to read."));
    }
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }
    FrameReadback output{};
    output.configuration = lastRenderedConfiguration_;
    output.displaySize = size_;
    output.projection = MakeProjection(lastRenderedConfiguration_, size_);
    output.boxCenter = BoxCenter(lastRenderedConfiguration_.animationFrame);
    // Report the frame the shader was actually told to reproject against rather than recomputing a guess here.
    output.previousAnimationFrame = lastPreviousAnimationFrame_;
    output.previousBoxCenter = BoxCenter(lastPreviousAnimationFrame_);
    output.frameSlot = lastRenderedFrameSlot_;
    output.historyWasValid = lastHistoryWasValid_;
    std::size_t const count = static_cast<std::size_t>(size_.width) * size_.height;
    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    output.pixels.resize(count);
    output.rawBytes.resize(count * sizeof(PixelRecord));
    std::memcpy(output.rawBytes.data(), slot.readback.mapped_data(), output.rawBytes.size());
    std::memcpy(output.pixels.data(), output.rawBytes.data(), output.rawBytes.size());
    return output;
}

} // namespace ch29::screen_space_reflections::gpu
