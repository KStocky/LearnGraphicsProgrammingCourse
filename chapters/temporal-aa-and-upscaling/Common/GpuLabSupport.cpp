#include "GpuLabSupport.hpp"

#include <lgp/framework/buffer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace ch28::temporal_aa::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorCount = 4U;
enum DescriptorIndex : UINT
{
    StatisticsUav = 0U,
    HistoryWriteUav = 1U,
    HistoryReadSrv = 2U,
    StatisticsSrv = 3U,
};
enum ComputeRootParameter : UINT
{
    ComputeConstants = 0U,
    ComputeOutputs = 1U,
    ComputeHistory = 2U,
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
    std::uint32_t renderWidth{};
    std::uint32_t renderHeight{};
    std::uint32_t animationFrame{};
    std::uint32_t previousAnimationFrame{};
    std::uint32_t jitterPhasePeriod{};
    std::uint32_t flags{};
    float currentPreExposure{};
    float previousPreExposure{};
    float sharpeningStrength{};
    float padding{};
};
static_assert(sizeof(DispatchConstants) == 48U);

struct DisplayConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t debugView{};
    std::uint32_t expectedStatus{};
};
static_assert(sizeof(DisplayConstants) == 16U);

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
            lgp::framework::MakeError("ValidateExtent", "Chapter 28 requires a non-empty extent up to 640x360."));
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

void SubmitTextureTransition(ID3D12GraphicsCommandList7 &list, ID3D12Resource &resource,
                             lgp::framework::TextureBarrierState before, lgp::framework::TextureBarrierState after)
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
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1U;
    group.pTextureBarriers = &barrier;
    list.Barrier(1U, &group);
}

[[nodiscard]] bool SameFloat(float left, float right) noexcept
{
    return std::abs(left - right) <= 1.0e-6F;
}

} // namespace

lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration, LabVariant variant)
{
    if (configuration.jitterPhasePeriod == 0U || configuration.jitterPhasePeriod > kMaximumJitterPhasePeriod)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Jitter phase period must be in [1,64]."));
    }
    if (!std::isfinite(configuration.upscaleRenderScale) || configuration.upscaleRenderScale < 0.25F ||
        configuration.upscaleRenderScale >= 1.0F)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Upscale render scale must be in [0.25,1)."));
    }
    if (!std::isfinite(configuration.preExposure) || configuration.preExposure <= 0.0F)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Pre-exposure must be finite and positive."));
    }
    if (!std::isfinite(configuration.sharpeningStrength) || configuration.sharpeningStrength < 0.0F ||
        configuration.sharpeningStrength > 2.0F)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Sharpening strength must be in [0,2]."));
    }
    if (static_cast<std::uint32_t>(configuration.mode) >
            static_cast<std::uint32_t>(ReconstructionMode::TemporalUpscale) ||
        static_cast<std::uint32_t>(configuration.debugView) > static_cast<std::uint32_t>(DebugView::Sharpening))
    {
        return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration", "Unknown Chapter 28 mode/view."));
    }
    if (variant == LabVariant::Starter && configuration.debugView != DebugView::CurrentSpatial &&
        configuration.debugView != DebugView::NativeVsUpscale)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateLabConfiguration", "Starter exposes only spatial and native-versus-reduced views."));
    }
    return {};
}

lgp::framework::Extent2D RenderExtent(LabConfiguration const &configuration,
                                      lgp::framework::Extent2D displayExtent) noexcept
{
    if (configuration.mode == ReconstructionMode::NativeTaa)
    {
        return displayExtent;
    }
    return {
        std::max(1U, static_cast<std::uint32_t>(
                         std::ceil(static_cast<double>(displayExtent.width) * configuration.upscaleRenderScale))),
        std::max(1U, static_cast<std::uint32_t>(
                         std::ceil(static_cast<double>(displayExtent.height) * configuration.upscaleRenderScale))),
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
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 28 buffers must be non-empty."));
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
                                                                createResult, "Failed to create Chapter 28 buffer."));
    }
    if (!name.empty())
    {
        std::wstring const ownedName{name};
        if (HRESULT const nameResult = result.resource_->SetName(ownedName.c_str()); FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name Chapter 28 buffer."));
        }
    }
    if (mapPersistently)
    {
        D3D12_RANGE const readRange{0U, static_cast<SIZE_T>(sizeInBytes)};
        void *mapping = nullptr;
        if (HRESULT const mapResult = result.resource_->Map(0U, &readRange, &mapping); FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map Chapter 28 readback."));
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
        interactiveConfiguration_.debugView = DebugView::CurrentSpatial;
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
    if (auto status = CompileShader(compiler, options, L"TemporalCS", L"cs_6_0", computeShader_); !status)
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
    outputRange.NumDescriptors = 2U;
    outputRange.BaseShaderRegister = 0U;
    D3D12_DESCRIPTOR_RANGE historyRange{};
    historyRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    historyRange.NumDescriptors = 1U;
    historyRange.BaseShaderRegister = 0U;
    D3D12_ROOT_PARAMETER computeParameters[3]{};
    computeParameters[ComputeConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[ComputeConstants].Constants.ShaderRegister = 0U;
    computeParameters[ComputeConstants].Constants.Num32BitValues = sizeof(DispatchConstants) / sizeof(std::uint32_t);
    computeParameters[ComputeOutputs].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeOutputs].DescriptorTable = {1U, &outputRange};
    computeParameters[ComputeHistory].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeHistory].DescriptorTable = {1U, &historyRange};
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
                                                                "Failed to create Chapter 28 compute root signature."));
    }

    D3D12_DESCRIPTOR_RANGE statisticsRange{};
    statisticsRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    statisticsRange.NumDescriptors = 1U;
    statisticsRange.BaseShaderRegister = 0U;
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
            "ID3D12Device::CreateRootSignature", createResult, "Failed to create Chapter 28 graphics root signature."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreatePipelines()
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = computeRootSignature_.Get();
    compute.CS = computeShader_.Bytecode();
    HRESULT result =
        deviceResources_->device()->CreateComputePipelineState(&compute, IID_PPV_ARGS(computePipeline_.GetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState", result,
                                                                "Failed to create Chapter 28 compute PSO."));
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
                                                                "Failed to create Chapter 28 graphics PSO."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreateResources(lgp::framework::Extent2D size)
{
    if (auto status = ValidateExtent(size); !status)
    {
        return status;
    }
    std::uint64_t const count = static_cast<std::uint64_t>(size.width) * size.height;
    std::uint64_t const statisticsBytes = count * sizeof(PixelStatistics);
    std::uint64_t const historyBytes = count * sizeof(HistoryPixel);
    for (std::uint32_t index = 0U; index < history_.size(); ++index)
    {
        auto history = CreateBuffer(*deviceResources_->device(), historyBytes, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    index == 0U ? L"Ch28 history A" : L"Ch28 history B");
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
        auto statistics = CreateBuffer(*deviceResources_->device(), statisticsBytes, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch28 pixel diagnostics");
        auto readback = CreateBuffer(*deviceResources_->device(), statisticsBytes, D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_FLAG_NONE, L"Ch28 diagnostics readback", true);
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
        slot.readback = std::move(*readback);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = static_cast<UINT>(count);
        uav.Buffer.StructureByteStride = sizeof(PixelStatistics);
        deviceResources_->device()->CreateUnorderedAccessView(slot.statistics.Get(), nullptr, &uav,
                                                              slot.descriptors.CpuHandle(StatisticsUav));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = static_cast<UINT>(count);
        srv.Buffer.StructureByteStride = sizeof(PixelStatistics);
        deviceResources_->device()->CreateShaderResourceView(slot.statistics.Get(), &srv,
                                                             slot.descriptors.CpuHandle(StatisticsSrv));
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
    if (context.input.WasKeyPressed('R'))
    {
        forceReset_ = true;
        interactiveConfiguration_.animationFrame = 0U;
    }
    if (context.input.WasKeyPressed('T'))
    {
        interactiveConfiguration_.mode = interactiveConfiguration_.mode == ReconstructionMode::NativeTaa
                                             ? ReconstructionMode::TemporalUpscale
                                             : ReconstructionMode::NativeTaa;
    }
    if (context.input.WasKeyPressed('J'))
    {
        interactiveConfiguration_.jitterEnabled = !interactiveConfiguration_.jitterEnabled;
    }
    for (std::uint32_t view = 0U; view <= static_cast<std::uint32_t>(DebugView::Sharpening); ++view)
    {
        DebugView const candidate = static_cast<DebugView>(view);
        bool const available = variant_ == LabVariant::Solution || candidate == DebugView::CurrentSpatial ||
                               candidate == DebugView::NativeVsUpscale;
        if (available && context.input.WasKeyPressed('1' + view))
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
    return animationReset || configuration.mode != lastHistoryConfiguration_.mode ||
           configuration.jitterPhasePeriod != lastHistoryConfiguration_.jitterPhasePeriod ||
           configuration.jitterEnabled != lastHistoryConfiguration_.jitterEnabled ||
           !SameFloat(configuration.upscaleRenderScale, lastHistoryConfiguration_.upscaleRenderScale) ||
           !SameFloat(configuration.preExposure, lastHistoryConfiguration_.preExposure);
}

lgp::framework::Status RendererCore::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Render", "Chapter 28 frame slot is out of range."));
    }
    LabConfiguration const configuration = ActiveConfiguration();
    if (auto status = ValidateLabConfiguration(configuration, variant_); !status)
    {
        return status;
    }
    lgp::framework::Extent2D const renderSize = RenderExtent(configuration, size_);
    bool const configurationReset = HistoryAffectingConfigurationChanged(configuration);
    bool const reset = forceReset_ || configuration.resetHistory || configurationReset;
    bool const canReadHistory = variant_ == LabVariant::Solution && historyValid_ && !reset;
    std::uint32_t const previousAnimationFrame =
        canReadHistory ? lastHistoryConfiguration_.animationFrame : configuration.animationFrame;
    Float2 currentJitter{};
    Float2 previousJitter{};
    if (configuration.jitterEnabled)
    {
        auto current = GenerateJitter(configuration.animationFrame, configuration.jitterPhasePeriod,
                                      {.width = renderSize.width, .height = renderSize.height});
        auto previous = GenerateJitter(previousAnimationFrame, configuration.jitterPhasePeriod,
                                       {.width = renderSize.width, .height = renderSize.height});
        if (!current || !previous)
        {
            return std::unexpected(lgp::framework::MakeError("GenerateJitter", "Chapter 28 jitter generation failed."));
        }
        currentJitter = current->pixelOffset;
        previousJitter = previous->pixelOffset;
    }

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
    std::array<D3D12_BUFFER_BARRIER, 3U> beforeBarriers{
        MakeBufferBarrier(*slot.statistics.Get(), slot.initialized ? CopySourceState() : NoAccessState(),
                          ComputeUavState()),
        MakeBufferBarrier(*history_[writeIndex].Get(), historyStates_[writeIndex], ComputeUavState()),
        MakeBufferBarrier(*history_[historyReadIndex_].Get(), historyStates_[historyReadIndex_], ComputeSrvState()),
    };
    SubmitBufferBarriers(list, beforeBarriers);
    historyStates_[writeIndex] = ComputeUavState();
    historyStates_[historyReadIndex_] = ComputeSrvState();

    std::uint32_t flags = 0U;
    flags |= canReadHistory ? 1U : 0U;
    flags |= reset ? 2U : 0U;
    flags |= configuration.jitterEnabled ? 4U : 0U;
    flags |= variant_ == LabVariant::Solution && historyValid_ ? 8U : 0U;
    DispatchConstants const constants{
        size_.width,
        size_.height,
        renderSize.width,
        renderSize.height,
        configuration.animationFrame,
        previousAnimationFrame,
        configuration.jitterPhasePeriod,
        flags,
        configuration.preExposure,
        hasRendered_ ? lastHistoryConfiguration_.preExposure : configuration.preExposure,
        configuration.sharpeningStrength,
        0.0F,
    };
    list.SetComputeRootSignature(computeRootSignature_.Get());
    list.SetComputeRoot32BitConstants(ComputeConstants, sizeof(constants) / sizeof(std::uint32_t), &constants, 0U);
    list.SetComputeRootDescriptorTable(ComputeOutputs, slot.descriptors.GpuHandle(StatisticsUav));
    list.SetComputeRootDescriptorTable(ComputeHistory, slot.descriptors.GpuHandle(HistoryReadSrv));
    list.SetPipelineState(computePipeline_.Get());
    list.Dispatch((size_.width + 7U) / 8U, (size_.height + 7U) / 8U, 1U);

    std::array<D3D12_BUFFER_BARRIER, 2U> afterBarriers{
        MakeBufferBarrier(*slot.statistics.Get(), ComputeUavState(), PixelSrvState()),
        MakeBufferBarrier(*history_[writeIndex].Get(), ComputeUavState(), ComputeSrvState()),
    };
    SubmitBufferBarriers(list, afterBarriers);
    historyStates_[writeIndex] = ComputeSrvState();
    SubmitTextureTransition(list, *frameContext.renderTarget, FrameStartState(frameContext), RenderTargetState());
    float const clear[]{0.0F, 0.0F, 0.0F, 1.0F};
    list.ClearRenderTargetView(frameContext.renderTargetView, clear, 0U, nullptr);
    list.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    list.RSSetViewports(1U, &frameContext.viewport);
    list.RSSetScissorRects(1U, &frameContext.scissorRect);
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    DisplayConstants const display{
        size_.width,
        size_.height,
        static_cast<std::uint32_t>(configuration.debugView),
        variant_ == LabVariant::Solution ? kSolutionValidStatus : kStarterValidStatus,
    };
    list.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    list.SetGraphicsRoot32BitConstants(GraphicsConstants, sizeof(display) / sizeof(std::uint32_t), &display, 0U);
    list.SetGraphicsRootDescriptorTable(GraphicsStatistics, slot.descriptors.GpuHandle(StatisticsSrv));
    list.SetPipelineState(graphicsPipeline_.Get());
    list.DrawInstanced(3U, 1U, 0U, 0U);
    D3D12_BUFFER_BARRIER copyBarrier = MakeBufferBarrier(*slot.statistics.Get(), PixelSrvState(), CopySourceState());
    SubmitBufferBarriers(list, std::span{&copyBarrier, 1U});
    list.CopyBufferRegion(slot.readback.Get(), 0U, slot.statistics.Get(), 0U, slot.statistics.size_in_bytes());
    SubmitTextureTransition(list, *frameContext.renderTarget, RenderTargetState(), FrameEndState(frameContext));

    slot.initialized = true;
    if (variant_ == LabVariant::Solution)
    {
        historyReadIndex_ = writeIndex;
        historyValid_ = true;
    }
    lastRenderedConfiguration_ = configuration;
    lastHistoryConfiguration_ = configuration;
    lastCurrentJitter_ = currentJitter;
    lastPreviousJitter_ = previousJitter;
    lastHistoryWasValid_ = canReadHistory;
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
    computePipeline_.Reset();
    graphicsRootSignature_.Reset();
    computeRootSignature_.Reset();
    computeShader_ = {};
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
            lgp::framework::MakeError("ReadBackOutputs", "Chapter 28 has no completed frame to read."));
    }
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }
    FrameReadback output{};
    output.configuration = lastRenderedConfiguration_;
    output.displaySize = size_;
    output.renderSize = RenderExtent(lastRenderedConfiguration_, size_);
    output.currentJitterPixels = lastCurrentJitter_;
    output.previousJitterPixels = lastPreviousJitter_;
    output.frameSlot = lastRenderedFrameSlot_;
    output.historyWasValid = lastHistoryWasValid_;
    output.pixels.resize(static_cast<std::size_t>(size_.width) * size_.height);
    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    std::memcpy(output.pixels.data(), slot.readback.mapped_data(), output.pixels.size() * sizeof(PixelStatistics));
    return output;
}

} // namespace ch28::temporal_aa::gpu
