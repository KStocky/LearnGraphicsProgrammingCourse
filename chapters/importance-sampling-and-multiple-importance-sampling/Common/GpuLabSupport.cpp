#include "GpuLabSupport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace ch17::importance_sampling::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorCount = 3U;
inline constexpr UINT kMomentsStride = 64U;

enum DescriptorIndex : UINT
{
    MomentsUav = 0U,
    StatisticsUav = 1U,
    StatisticsSrv = 2U,
};

enum ComputeRootParameter : UINT
{
    ComputeConstants = 0U,
    ComputeUavTable = 1U,
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
    std::uint32_t integrandEvaluationsPerDispatch{};
    std::uint32_t seed{};
    float targetExponent{};
    float proposalExponent{};
    std::uint32_t reset{};
    std::uint32_t misHeuristic{};
};

struct DisplayConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t debugView{};
    std::uint32_t expectedStatus{};
};

static_assert(sizeof(DispatchConstants) == 32U);
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
            lgp::framework::MakeError("ValidateExtent", "Chapter 17 requires a non-empty extent up to 640x360."));
    }
    return {};
}

[[nodiscard]] bool IsValidDebugView(DebugView view) noexcept
{
    return static_cast<std::uint32_t>(view) <= static_cast<std::uint32_t>(DebugView::WorkBudget);
}

[[nodiscard]] bool IsValidMisHeuristic(MisHeuristic heuristic) noexcept
{
    return heuristic == MisHeuristic::Balance || heuristic == MisHeuristic::Power;
}

} // namespace

lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration, LabVariant variant)
{
    if (!std::isfinite(configuration.targetExponent) || configuration.targetExponent < 0.0F ||
        configuration.targetExponent > kMaximumExponent)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateLabConfiguration", "The Chapter 17 target exponent must be finite and in [0, 64]."));
    }
    if (!std::isfinite(configuration.proposalExponent) || configuration.proposalExponent < 0.0F ||
        configuration.proposalExponent > kMaximumExponent)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateLabConfiguration", "The Chapter 17 proposal exponent must be finite and in [0, 64]."));
    }
    if (configuration.integrandEvaluationsPerDispatch == 0U ||
        configuration.integrandEvaluationsPerDispatch > kMaximumIntegrandEvaluationsPerDispatch)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateLabConfiguration", "Chapter 17 requires 1 to 64 integrand evaluations per dispatch."));
    }
    if (variant == LabVariant::Solution && (configuration.integrandEvaluationsPerDispatch < 2U ||
                                            (configuration.integrandEvaluationsPerDispatch % 2U) != 0U))
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateLabConfiguration",
            "The Chapter 17 Solution requires an even integrand-evaluation budget of at least two."));
    }
    if (!IsValidDebugView(configuration.debugView))
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "The Chapter 17 diagnostic view is invalid."));
    }
    if (!IsValidMisHeuristic(configuration.misHeuristic))
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "The Chapter 17 MIS heuristic is invalid."));
    }
    return {};
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
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 17 buffers must be non-empty."));
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
                                                                "Failed to create a Chapter 17 buffer."));
    }

    if (!name.empty())
    {
        std::wstring const resourceName{name};
        HRESULT const nameResult = buffer.resource_->SetName(resourceName.c_str());
        if (FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name a Chapter 17 buffer."));
        }
    }

    if (mapPersistently)
    {
        D3D12_RANGE const readRange{0U, static_cast<SIZE_T>(sizeInBytes)};
        void *mapped = nullptr;
        HRESULT const mapResult = buffer.resource_->Map(0U, &readRange, &mapped);
        if (FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map a Chapter 17 readback buffer."));
        }
        buffer.mappedData_ = static_cast<std::byte *>(mapped);
    }

    buffer.sizeInBytes_ = sizeInBytes;
    return buffer;
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
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 2U;
    uavRange.BaseShaderRegister = 0U;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER computeParameters[2]{};
    computeParameters[ComputeConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[ComputeConstants].Constants.ShaderRegister = 0U;
    computeParameters[ComputeConstants].Constants.Num32BitValues = sizeof(DispatchConstants) / sizeof(std::uint32_t);
    computeParameters[ComputeUavTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeUavTable].DescriptorTable.NumDescriptorRanges = 1U;
    computeParameters[ComputeUavTable].DescriptorTable.pDescriptorRanges = &uavRange;

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
                                             "Failed to create the Chapter 17 compute root signature."));
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
                                             "Failed to create the Chapter 17 graphics root signature."));
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
                                                                "Failed to create the Chapter 17 sampling pipeline."));
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
                                                                "Failed to create the Chapter 17 display pipeline."));
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
    std::uint64_t const momentsBytes = pixelCount * kMomentsStride;
    std::uint64_t const statisticsBytes = pixelCount * sizeof(PixelStatistics);
    frameSlots_.resize(deviceResources_->back_buffer_count());

    for (FrameSlotResources &slot : frameSlots_)
    {
        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorCount);
        auto moments = CreateBuffer(*deviceResources_->device(), momentsBytes, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch17 Estimator Moments");
        auto statistics = CreateBuffer(*deviceResources_->device(), statisticsBytes, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch17 Pixel Statistics");
        auto readback = CreateBuffer(*deviceResources_->device(), statisticsBytes, D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_FLAG_NONE, L"Ch17 Statistics Readback", true);
        if (!descriptors)
        {
            return std::unexpected(std::move(descriptors.error()));
        }
        if (!moments)
        {
            return std::unexpected(std::move(moments.error()));
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
        slot.moments = std::move(*moments);
        slot.statistics = std::move(*statistics);
        slot.statisticsReadback = std::move(*readback);

        D3D12_UNORDERED_ACCESS_VIEW_DESC momentsUav{};
        momentsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        momentsUav.Buffer.NumElements = static_cast<UINT>(pixelCount);
        momentsUav.Buffer.StructureByteStride = kMomentsStride;
        deviceResources_->device()->CreateUnorderedAccessView(slot.moments.Get(), nullptr, &momentsUav,
                                                              slot.descriptors.CpuHandle(MomentsUav));

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

void RendererCore::ResetForConfigurationChange() noexcept
{
    ++resetGeneration_;
}

lgp::framework::Status RendererCore::Update(lgp::framework::UpdateContext const &context)
{
    if (headless_)
    {
        return {};
    }

    if (context.input.WasKeyPressed('1'))
    {
        interactiveConfiguration_.debugView = DebugView::Uniform;
    }
    if (context.input.WasKeyPressed('2'))
    {
        interactiveConfiguration_.debugView = DebugView::Matched;
    }
    if (context.input.WasKeyPressed('3'))
    {
        interactiveConfiguration_.debugView = DebugView::Mismatched;
    }
    if (context.input.WasKeyPressed('4'))
    {
        interactiveConfiguration_.debugView = DebugView::Mis;
    }
    if (context.input.WasKeyPressed('5'))
    {
        interactiveConfiguration_.debugView = DebugView::RelativeErrorComparison;
    }
    if (context.input.WasKeyPressed('6'))
    {
        interactiveConfiguration_.debugView = DebugView::StandardErrorComparison;
    }
    if (context.input.WasKeyPressed('7'))
    {
        interactiveConfiguration_.debugView = DebugView::WorkBudget;
    }
    if (context.input.WasKeyPressed('R'))
    {
        ResetForConfigurationChange();
    }

    if (context.input.WasKeyPressed('T') && interactiveConfiguration_.targetExponent < kMaximumExponent)
    {
        interactiveConfiguration_.targetExponent += 1.0F;
        ResetForConfigurationChange();
    }
    if (context.input.WasKeyPressed('G') && interactiveConfiguration_.targetExponent > 0.0F)
    {
        interactiveConfiguration_.targetExponent -= 1.0F;
        ResetForConfigurationChange();
    }
    if (variant_ == LabVariant::Solution && context.input.WasKeyPressed('P') &&
        interactiveConfiguration_.proposalExponent < kMaximumExponent)
    {
        interactiveConfiguration_.proposalExponent += 1.0F;
        ResetForConfigurationChange();
    }
    if (variant_ == LabVariant::Solution && context.input.WasKeyPressed('O') &&
        interactiveConfiguration_.proposalExponent > 0.0F)
    {
        interactiveConfiguration_.proposalExponent -= 1.0F;
        ResetForConfigurationChange();
    }
    if (variant_ == LabVariant::Solution && context.input.WasKeyPressed('H'))
    {
        interactiveConfiguration_.misHeuristic = interactiveConfiguration_.misHeuristic == MisHeuristic::Balance
                                                     ? MisHeuristic::Power
                                                     : MisHeuristic::Balance;
        ResetForConfigurationChange();
    }

    std::uint32_t const budgetStep = variant_ == LabVariant::Solution ? 2U : 1U;
    if (context.input.WasKeyPressed(VK_OEM_PLUS))
    {
        interactiveConfiguration_.integrandEvaluationsPerDispatch =
            std::min(kMaximumIntegrandEvaluationsPerDispatch,
                     interactiveConfiguration_.integrandEvaluationsPerDispatch + budgetStep);
    }
    if (context.input.WasKeyPressed(VK_OEM_MINUS) &&
        interactiveConfiguration_.integrandEvaluationsPerDispatch > budgetStep)
    {
        interactiveConfiguration_.integrandEvaluationsPerDispatch -= budgetStep;
    }
    return {};
}

lgp::framework::Status RendererCore::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 17 frame slot is out of range."));
    }

    LabConfiguration const configuration = ActiveConfiguration();
    if (auto status = ValidateLabConfiguration(configuration, variant_); !status)
    {
        return status;
    }

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    bool reset = !slot.initialized || slot.targetExponent != configuration.targetExponent ||
                 slot.seed != configuration.seed || slot.resetGeneration != resetGeneration_ ||
                 configuration.resetAccumulation;
    if (variant_ == LabVariant::Solution)
    {
        reset = reset || slot.proposalExponent != configuration.proposalExponent ||
                slot.misHeuristic != configuration.misHeuristic;
    }

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);

    BufferBarrierState const momentsBefore = slot.initialized ? ComputeUnorderedAccessState() : NoAccessState();
    BufferBarrierState const statisticsBefore = slot.initialized ? CopySourceState() : NoAccessState();
    std::vector<D3D12_BUFFER_BARRIER> bufferBarriers{
        MakeBufferBarrier(*slot.moments.Get(), momentsBefore, ComputeUnorderedAccessState()),
        MakeBufferBarrier(*slot.statistics.Get(), statisticsBefore, ComputeUnorderedAccessState()),
    };
    SubmitBufferBarriers(commandList, bufferBarriers);

    DispatchConstants const dispatch{
        size_.width,
        size_.height,
        configuration.integrandEvaluationsPerDispatch,
        configuration.seed,
        configuration.targetExponent,
        configuration.proposalExponent,
        reset ? 1U : 0U,
        static_cast<std::uint32_t>(configuration.misHeuristic),
    };
    commandList.SetComputeRootSignature(computeRootSignature_.Get());
    commandList.SetComputeRoot32BitConstants(ComputeConstants, sizeof(dispatch) / sizeof(std::uint32_t), &dispatch, 0U);
    commandList.SetComputeRootDescriptorTable(ComputeUavTable, slot.descriptors.GpuHandle(MomentsUav));
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

    slot.targetExponent = configuration.targetExponent;
    slot.proposalExponent = configuration.proposalExponent;
    slot.seed = configuration.seed;
    slot.misHeuristic = configuration.misHeuristic;
    slot.resetGeneration = resetGeneration_;
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
    if (configuration.resetAccumulation)
    {
        ++resetGeneration_;
    }
    headlessConfiguration_ = configuration;
}

std::expected<FrameReadback, lgp::framework::Error> RendererCore::ReadBackOutputs()
{
    if (deviceResources_ == nullptr || frameSlots_.empty() || !hasRendered_)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "Chapter 17 has no rendered frame to read back."));
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

} // namespace ch17::importance_sampling::gpu
