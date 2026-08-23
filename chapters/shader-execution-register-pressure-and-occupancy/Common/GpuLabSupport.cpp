#include "GpuLabSupport.hpp"

#include <dxcapi.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace ch18::shader_occupancy::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorCount = 2U;

enum DescriptorIndex : UINT
{
    OutputUav = 0U,
    OutputSrv = 1U,
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
    std::uint32_t elementCount{};
};

struct DisplayConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t elementCount{};
    std::uint32_t diagnosticView{};
    std::uint32_t variant{};
    std::uint32_t expectedStatus{};
    std::uint32_t residentWaves{};
    std::uint32_t maximumResidentWaves{};
    std::uint32_t waveOpsSupported{};
    std::uint32_t waveLaneCountMinimum{};
    std::uint32_t waveLaneCountMaximum{};
    std::uint32_t isWarp{};
};

static_assert(sizeof(DispatchConstants) == 4U);
static_assert(sizeof(DisplayConstants) == 48U);

inline constexpr TeachingLivenessBounds kLivenessBounds{
    .maximumProgramPointInclusive = 5U,
    .maximumLiveIntervals = 32U,
    .maximumVectorRegisterWidth = 16U,
};

inline constexpr std::array<LiveValueInterval, 3U> kLowPressureLiveness{{
    {1U, 0U, 2U, 4U},
    {2U, 1U, 3U, 4U},
    {3U, 3U, 5U, 4U},
}};

inline constexpr std::array<LiveValueInterval, 18U> kHighPressureLiveness{{
    {1U, 0U, 2U, 4U},
    {2U, 2U, 4U, 4U},
    {3U, 2U, 4U, 4U},
    {4U, 2U, 4U, 4U},
    {5U, 2U, 4U, 4U},
    {6U, 2U, 4U, 4U},
    {7U, 2U, 4U, 4U},
    {8U, 2U, 4U, 4U},
    {9U, 2U, 4U, 4U},
    {10U, 2U, 4U, 4U},
    {11U, 2U, 4U, 4U},
    {12U, 2U, 4U, 4U},
    {13U, 2U, 4U, 4U},
    {14U, 2U, 4U, 4U},
    {15U, 2U, 4U, 4U},
    {16U, 2U, 4U, 4U},
    {17U, 2U, 4U, 4U},
    {18U, 4U, 5U, 4U},
}};

inline constexpr std::array<LiveValueInterval, 3U> kShortLiveRangeLiveness{{
    {1U, 0U, 2U, 4U},
    {2U, 1U, 4U, 4U},
    {3U, 4U, 5U, 4U},
}};

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] bool IsValidVariant(ShaderVariant const variant) noexcept
{
    return static_cast<std::uint32_t>(variant) <= static_cast<std::uint32_t>(ShaderVariant::CoherentShortLiveRange);
}

[[nodiscard]] bool IsValidDiagnosticView(DiagnosticView const view) noexcept
{
    return static_cast<std::uint32_t>(view) <= static_cast<std::uint32_t>(DiagnosticView::EvidenceBoundaries);
}

[[nodiscard]] BranchPattern PatternForVariant(ShaderVariant const variant) noexcept
{
    return variant == ShaderVariant::DivergentLowPressure ? BranchPattern::DivergentAlternatingLanes
                                                          : BranchPattern::CoherentAlternatingGroups;
}

[[nodiscard]] std::string_view VariantFileStem(ShaderVariant const variant) noexcept
{
    switch (variant)
    {
    case ShaderVariant::CoherentLowPressure:
        return "coherent-low-pressure";
    case ShaderVariant::DivergentLowPressure:
        return "divergent-low-pressure";
    case ShaderVariant::CoherentHighLiveRange:
        return "coherent-high-live-range";
    case ShaderVariant::CoherentShortLiveRange:
        return "coherent-short-live-range";
    default:
        return "invalid";
    }
}

[[nodiscard]] lgp::framework::Error ContractFailure(std::string_view operation)
{
    return lgp::framework::MakeError(std::string{operation}, "The Chapter 18 CPU teaching contract rejected evidence.");
}

[[nodiscard]] std::uint32_t HashValue(std::array<float, 4U> const &value, std::uint32_t threadIndex) noexcept
{
    std::uint32_t hash = 2'166'136'261U;
    for (float const component : value)
    {
        hash = (hash ^ std::bit_cast<std::uint32_t>(component)) * 16'777'619U;
    }
    return (hash ^ threadIndex) * 16'777'619U;
}

[[nodiscard]] std::uint64_t AccumulateChecksum(std::uint64_t hash, std::uint32_t value) noexcept
{
    hash ^= value;
    return hash * 1'099'511'628'211ULL;
}

[[nodiscard]] std::expected<std::string, lgp::framework::Error> DisassembleShader(
    lgp::framework::CompiledShader const &shader)
{
    ComPtr<IDxcCompiler3> compiler{};
    HRESULT const createResult =
        ::DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "DxcCreateInstance", createResult, "Failed to create DXC for Chapter 18 DXIL disassembly."));
    }

    DxcBuffer buffer{};
    buffer.Ptr = shader.bytecode.data();
    buffer.Size = shader.bytecode.size();

    ComPtr<IDxcResult> result{};
    HRESULT const disassembleResult = compiler->Disassemble(&buffer, IID_PPV_ARGS(result.ReleaseAndGetAddressOf()));
    if (FAILED(disassembleResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("IDxcCompiler3::Disassemble", disassembleResult,
                                                                "Failed to disassemble a Chapter 18 shader."));
    }

    HRESULT status = S_OK;
    HRESULT const statusResult = result->GetStatus(&status);
    if (FAILED(statusResult) || FAILED(status))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("IDxcResult::GetStatus", FAILED(statusResult) ? statusResult : status,
                                             "DXC rejected a Chapter 18 shader disassembly request."));
    }

    ComPtr<IDxcBlobUtf8> listing{};
    HRESULT const outputResult =
        result->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(listing.ReleaseAndGetAddressOf()), nullptr);
    if (FAILED(outputResult) || listing == nullptr || listing->GetStringLength() == 0U)
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "IDxcResult::GetOutput", outputResult, "DXC did not return Chapter 18 DXIL disassembly text."));
    }
    return std::string{listing->GetStringPointer(), listing->GetStringLength()};
}

[[nodiscard]] std::expected<lgp::framework::CompiledShader, lgp::framework::Error> CompileShader(
    lgp::framework::ShaderCompiler &compiler, lgp::framework::ShaderCompileOptions &options, wchar_t const *entryPoint,
    wchar_t const *targetProfile)
{
    options.entryPoint = entryPoint;
    options.targetProfile = targetProfile;
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    return compiler.Compile(options);
}

[[nodiscard]] lgp::framework::Status ValidateExtent(lgp::framework::Extent2D const size)
{
    if (size.width == 0U || size.height == 0U || size.width > kMaximumWidth || size.height > kMaximumHeight)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateExtent", "Chapter 18 requires a non-empty extent up to 640x360."));
    }
    return {};
}

[[nodiscard]] RuntimeFunctionalEvidence BuildRuntimeFunctionalEvidence(LabConfiguration const &configuration,
                                                                       std::span<ThreadOutput const> outputs)
{
    RuntimeFunctionalEvidence evidence{};
    evidence.dispatchedThreadCount = static_cast<std::uint32_t>(outputs.size());
    evidence.referenceAgreement = outputs.size() == configuration.elementCount;
    evidence.branchClassificationAgreement = outputs.size() == configuration.elementCount;

    bool gpuStatusValid = outputs.size() == configuration.elementCount;
    std::uint64_t outputChecksum = 1'469'598'103'934'665'603ULL;
    std::uint64_t referenceChecksum = 1'469'598'103'934'665'603ULL;

    for (std::uint32_t index = 0U; index < configuration.elementCount; ++index)
    {
        std::array<float, 4U> const reference = ReferenceValue(index);
        std::uint32_t const expectedChecksum = ReferenceValueChecksum(index);
        referenceChecksum = AccumulateChecksum(referenceChecksum, expectedChecksum);

        if (index >= outputs.size())
        {
            evidence.referenceAgreement = false;
            evidence.branchClassificationAgreement = false;
            gpuStatusValid = false;
            continue;
        }

        ThreadOutput const &output = outputs[index];
        outputChecksum = AccumulateChecksum(outputChecksum, output.valueChecksum);
        gpuStatusValid = gpuStatusValid && output.status == kOutputValidStatus && output.threadIndex == index;
        evidence.referenceAgreement =
            evidence.referenceAgreement && output.valueChecksum == expectedChecksum && output.threadIndex == index;

        for (std::size_t component = 0U; component < reference.size(); ++component)
        {
            float const error = std::abs(output.value[component] - reference[component]);
            evidence.maximumAbsoluteError = std::max(evidence.maximumAbsoluteError, error);
            evidence.referenceAgreement = evidence.referenceAgreement && error <= kReferenceTolerance;
        }

        std::uint32_t const expectedBranchClass = ExpectedBranchClass(configuration.variant, index);
        evidence.branchClassificationAgreement =
            evidence.branchClassificationAgreement && output.branchClass == expectedBranchClass;
        if (output.branchClass == 1U)
        {
            ++evidence.activeThenLaneCount;
        }
        else if (output.branchClass == 0U)
        {
            ++evidence.activeElseLaneCount;
        }
        else
        {
            evidence.branchClassificationAgreement = false;
        }
    }

    evidence.outputChecksum = outputChecksum;
    evidence.referenceChecksum = referenceChecksum;
    evidence.referenceAgreement = evidence.referenceAgreement && outputChecksum == referenceChecksum;
    if (!gpuStatusValid)
    {
        evidence.status = FunctionalStatus::InvalidGpuStatus;
    }
    else if (!evidence.referenceAgreement)
    {
        evidence.status = FunctionalStatus::OutputMismatch;
    }
    else if (!evidence.branchClassificationAgreement)
    {
        evidence.status = FunctionalStatus::BranchClassificationMismatch;
    }
    else
    {
        evidence.status = FunctionalStatus::Valid;
    }
    return evidence;
}

} // namespace

std::expected<VariantMetadata, lgp::framework::Error> GetVariantMetadata(ShaderVariant const variant)
{
    switch (variant)
    {
    case ShaderVariant::CoherentLowPressure:
        return VariantMetadata{
            .variant = variant,
            .entryPoint = L"CoherentLowPressureCS",
            .displayName = "coherent low pressure",
            .threadGroupSize = {kThreadGroupSize, 1U, 1U},
            .expectedBranchPattern = BranchPattern::CoherentAlternatingGroups,
            .branchInstructionModel = {2U, 2U, 6U},
            .abstractResourceUsage =
                {
                    .threadsPerGroup = kThreadGroupSize,
                    .vectorRegistersPerThreadInLane32BitValues = 24U,
                    .scalarRegistersPerWaveIn32BitRegisters = 20U,
                    .groupsharedBytesPerGroup = 0U,
                },
            .livenessBounds = kLivenessBounds,
            .livenessIntervals = kLowPressureLiveness,
        };
    case ShaderVariant::DivergentLowPressure:
        return VariantMetadata{
            .variant = variant,
            .entryPoint = L"DivergentLowPressureCS",
            .displayName = "divergent low pressure",
            .threadGroupSize = {kThreadGroupSize, 1U, 1U},
            .expectedBranchPattern = BranchPattern::DivergentAlternatingLanes,
            .branchInstructionModel = {2U, 2U, 6U},
            .abstractResourceUsage =
                {
                    .threadsPerGroup = kThreadGroupSize,
                    .vectorRegistersPerThreadInLane32BitValues = 24U,
                    .scalarRegistersPerWaveIn32BitRegisters = 20U,
                    .groupsharedBytesPerGroup = 0U,
                },
            .livenessBounds = kLivenessBounds,
            .livenessIntervals = kLowPressureLiveness,
        };
    case ShaderVariant::CoherentHighLiveRange:
        return VariantMetadata{
            .variant = variant,
            .entryPoint = L"CoherentHighLiveRangeCS",
            .displayName = "coherent high live range",
            .threadGroupSize = {kThreadGroupSize, 1U, 1U},
            .expectedBranchPattern = BranchPattern::CoherentAlternatingGroups,
            .branchInstructionModel = {18U, 18U, 20U},
            .abstractResourceUsage =
                {
                    .threadsPerGroup = kThreadGroupSize,
                    .vectorRegistersPerThreadInLane32BitValues = 80U,
                    .scalarRegistersPerWaveIn32BitRegisters = 20U,
                    .groupsharedBytesPerGroup = 0U,
                },
            .livenessBounds = kLivenessBounds,
            .livenessIntervals = kHighPressureLiveness,
        };
    case ShaderVariant::CoherentShortLiveRange:
        return VariantMetadata{
            .variant = variant,
            .entryPoint = L"CoherentShortLiveRangeCS",
            .displayName = "coherent shortened live range",
            .threadGroupSize = {kThreadGroupSize, 1U, 1U},
            .expectedBranchPattern = BranchPattern::CoherentAlternatingGroups,
            .branchInstructionModel = {18U, 18U, 20U},
            .abstractResourceUsage =
                {
                    .threadsPerGroup = kThreadGroupSize,
                    .vectorRegistersPerThreadInLane32BitValues = 32U,
                    .scalarRegistersPerWaveIn32BitRegisters = 20U,
                    .groupsharedBytesPerGroup = 0U,
                },
            .livenessBounds = kLivenessBounds,
            .livenessIntervals = kShortLiveRangeLiveness,
        };
    default:
        return std::unexpected(
            lgp::framework::MakeError("GetVariantMetadata", "The Chapter 18 shader variant is invalid."));
    }
}

lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration, LabEdition const edition)
{
    if (!IsValidVariant(configuration.variant))
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "The Chapter 18 shader variant is invalid."));
    }
    if (edition == LabEdition::Starter && configuration.variant != ShaderVariant::CoherentLowPressure)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateLabConfiguration", "The Chapter 18 Starter contains only the coherent low-pressure baseline."));
    }
    if (!IsValidDiagnosticView(configuration.diagnosticView))
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "The Chapter 18 diagnostic view is invalid."));
    }
    if (configuration.elementCount == 0U || configuration.elementCount > kMaximumElementCount ||
        (configuration.elementCount % kThreadGroupSize) != 0U)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateLabConfiguration", "Chapter 18 requires 64 to 4096 elements in complete 64-thread groups."));
    }
    return {};
}

std::expected<std::uint64_t, lgp::framework::Error> ComputeOutputBufferSize(std::uint64_t const elementCount)
{
    if (elementCount == 0U || elementCount > std::numeric_limits<std::uint64_t>::max() / sizeof(ThreadOutput))
    {
        return std::unexpected(
            lgp::framework::MakeError("ComputeOutputBufferSize", "The Chapter 18 output buffer size overflows."));
    }
    return elementCount * sizeof(ThreadOutput);
}

std::array<float, 4U> ReferenceValue(std::uint32_t const threadIndex) noexcept
{
    return {
        static_cast<float>(threadIndex & 255U) / 256.0F,
        static_cast<float>((threadIndex * 3U) & 255U) / 256.0F,
        static_cast<float>((threadIndex * 5U + 7U) & 255U) / 256.0F,
        static_cast<float>((threadIndex * 11U + 13U) & 255U) / 256.0F,
    };
}

std::uint32_t ReferenceValueChecksum(std::uint32_t const threadIndex) noexcept
{
    return HashValue(ReferenceValue(threadIndex), threadIndex);
}

std::uint32_t ExpectedBranchClass(ShaderVariant const variant, std::uint32_t const threadIndex)
{
    if (!IsValidVariant(variant))
    {
        return std::numeric_limits<std::uint32_t>::max();
    }
    if (PatternForVariant(variant) == BranchPattern::DivergentAlternatingLanes)
    {
        return (threadIndex & 1U) == 0U ? 1U : 0U;
    }
    return ((threadIndex / kThreadGroupSize) & 1U) == 0U ? 1U : 0U;
}

std::expected<AbstractModelEvidence, lgp::framework::Error> BuildAbstractModelEvidence(ShaderVariant const variant,
                                                                                       std::uint32_t const elementCount)
{
    auto metadata = GetVariantMetadata(variant);
    if (!metadata)
    {
        return std::unexpected(std::move(metadata.error()));
    }
    if (elementCount == 0U || elementCount > kMaximumElementCount || (elementCount % kThreadGroupSize) != 0U)
    {
        return std::unexpected(lgp::framework::MakeError(
            "BuildAbstractModelEvidence", "The Chapter 18 abstract model requires complete bounded thread groups."));
    }

    HardwareModel const architecture = MakeAbstractNarrowWaveTeachingArchitecture();
    auto occupancy = ComputeOccupancy(architecture, metadata->abstractResourceUsage);
    if (!occupancy)
    {
        return std::unexpected(ContractFailure("ComputeOccupancy"));
    }
    auto liveness = AnalyzeTeachingLiveness(metadata->livenessBounds, metadata->livenessIntervals);
    if (!liveness)
    {
        return std::unexpected(ContractFailure("AnalyzeTeachingLiveness"));
    }

    AbstractModelEvidence evidence{
        .teachingArchitecture = architecture,
        .teachingUsage = metadata->abstractResourceUsage,
        .occupancy = std::move(*occupancy),
        .liveness = std::move(*liveness),
    };

    std::uint32_t const waveSize = architecture.waveSizeInLanes;
    std::uint32_t const waveCount = elementCount / waveSize;
    for (std::uint32_t waveIndex = 0U; waveIndex < waveCount; ++waveIndex)
    {
        std::uint64_t thenMask = 0U;
        std::uint64_t elseMask = 0U;
        std::uint32_t const firstThread = waveIndex * waveSize;
        for (std::uint32_t lane = 0U; lane < waveSize; ++lane)
        {
            std::uint32_t const branchClass = ExpectedBranchClass(variant, firstThread + lane);
            if (branchClass == 1U)
            {
                thenMask |= std::uint64_t{1U} << lane;
            }
            else
            {
                elseMask |= std::uint64_t{1U} << lane;
            }
        }

        auto branch = AccountBranchExecution({
            .waveSizeInLanes = waveSize,
            .thenLaneMask = thenMask,
            .elseLaneMask = elseMask,
            .thenPathInstructionCount = metadata->branchInstructionModel.thenPathInstructionCount,
            .elsePathInstructionCount = metadata->branchInstructionModel.elsePathInstructionCount,
            .convergedInstructionCount = metadata->branchInstructionModel.convergedInstructionCount,
        });
        if (!branch)
        {
            return std::unexpected(ContractFailure("AccountBranchExecution"));
        }

        evidence.usefulLaneInstructions += branch->usefulLaneInstructions;
        evidence.issuedLaneSlots += branch->issuedLaneSlots;
        evidence.modeledThenLaneCount += branch->activeLanesInThenPath;
        evidence.modeledElseLaneCount += branch->activeLanesInElsePath;
    }
    return evidence;
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

std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
    ID3D12Device10 &device, std::uint64_t const sizeInBytes, D3D12_HEAP_TYPE const heapType,
    D3D12_RESOURCE_FLAGS const flags, std::wstring_view const name, bool const mapPersistently)
{
    if (sizeInBytes == 0U)
    {
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 18 buffers must be non-empty."));
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
                                                                "Failed to create a Chapter 18 buffer."));
    }

    if (!name.empty())
    {
        std::wstring const resourceName{name};
        HRESULT const nameResult = buffer.resource_->SetName(resourceName.c_str());
        if (FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name a Chapter 18 buffer."));
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
                                                                    "Failed to map a Chapter 18 readback buffer."));
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

D3D12_TEXTURE_BARRIER MakeTextureBarrier(ID3D12Resource &resource, lgp::framework::TextureBarrierState const before,
                                         lgp::framework::TextureBarrierState const after,
                                         D3D12_TEXTURE_BARRIER_FLAGS const flags) noexcept
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

D3D12_BUFFER_BARRIER MakeBufferBarrier(ID3D12Resource &resource, BufferBarrierState const before,
                                       BufferBarrierState const after) noexcept
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

RendererCore::RendererCore(std::filesystem::path shaderPath, LabEdition const edition)
    : shaderPath_(std::move(shaderPath)), edition_(edition)
{
}

lgp::framework::Status RendererCore::QueryWaveLaneCapability()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
    HRESULT const result =
        deviceResources_->device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CheckFeatureSupport", result,
                                                                "Failed to query Chapter 18 wave-lane capabilities."));
    }
    waveLaneCapability_ = {
        .waveOpsSupported = options.WaveOps != FALSE,
        .minimumLaneCount = options.WaveOps != FALSE ? options.WaveLaneCountMin : 0U,
        .maximumLaneCount = options.WaveOps != FALSE ? options.WaveLaneCountMax : 0U,
        .isWarp = deviceResources_->adapter_info().isWarp,
    };
    return {};
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
#endif

    std::array<ShaderVariant, 4U> const variants{
        ShaderVariant::CoherentLowPressure,
        ShaderVariant::DivergentLowPressure,
        ShaderVariant::CoherentHighLiveRange,
        ShaderVariant::CoherentShortLiveRange,
    };
    std::size_t const variantCount = edition_ == LabEdition::Starter ? 1U : variants.size();
    variantPipelines_.reserve(variantCount);
    shaderArtifacts_.reserve(variantCount);

    for (std::size_t index = 0U; index < variantCount; ++index)
    {
        auto metadata = GetVariantMetadata(variants[index]);
        if (!metadata)
        {
            return std::unexpected(std::move(metadata.error()));
        }
        std::wstring const entryPoint{metadata->entryPoint};
        auto shader = CompileShader(compiler, options, entryPoint.c_str(), L"cs_6_0");
        if (!shader)
        {
            return std::unexpected(std::move(shader.error()));
        }
        auto disassembly = DisassembleShader(*shader);
        if (!disassembly)
        {
            return std::unexpected(std::move(disassembly.error()));
        }

        shaderArtifacts_.push_back({
            .variant = variants[index],
            .entryPoint = entryPoint,
            .targetProfile = L"cs_6_0",
            .dxilDisassembly = std::move(*disassembly),
            .bytecodeSize = shader->bytecode.size(),
        });
        variantPipelines_.push_back({
            .variant = variants[index],
            .shader = std::move(*shader),
        });
    }

    auto vertex = CompileShader(compiler, options, L"FullscreenVS", L"vs_6_0");
    if (!vertex)
    {
        return std::unexpected(std::move(vertex.error()));
    }
    vertexShader_ = std::move(*vertex);

    auto pixel = CompileShader(compiler, options, L"DisplayPS", L"ps_6_0");
    if (!pixel)
    {
        return std::unexpected(std::move(pixel.error()));
    }
    pixelShader_ = std::move(*pixel);
    return {};
}

lgp::framework::Status RendererCore::CreateRootSignatures()
{
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1U;
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
                                             "Failed to create the Chapter 18 compute root signature."));
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
                                             "Failed to create the Chapter 18 graphics root signature."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreatePipelines()
{
    for (VariantPipeline &variant : variantPipelines_)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
        description.pRootSignature = computeRootSignature_.Get();
        description.CS = variant.shader.Bytecode();
        HRESULT const result = deviceResources_->device()->CreateComputePipelineState(
            &description, IID_PPV_ARGS(variant.pipeline.ReleaseAndGetAddressOf()));
        if (FAILED(result))
        {
            return std::unexpected(
                lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState", result,
                                                 "Failed to create a Chapter 18 independent shader-variant pipeline."));
        }
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

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = graphicsRootSignature_.Get();
    description.VS = vertexShader_.Bytecode();
    description.PS = pixelShader_.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = deviceResources_->back_buffer_format();
    description.SampleDesc.Count = 1U;

    HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(graphicsPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                             "Failed to create the Chapter 18 diagnostic display pipeline."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreateFrameSlotResources(lgp::framework::Extent2D const size)
{
    if (auto status = ValidateExtent(size); !status)
    {
        return status;
    }
    auto bufferSize = ComputeOutputBufferSize(kMaximumElementCount);
    if (!bufferSize)
    {
        return std::unexpected(std::move(bufferSize.error()));
    }

    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlotResources &slot : frameSlots_)
    {
        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorCount);
        auto output = CreateBuffer(*deviceResources_->device(), *bufferSize, D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch18 Shader Variant Output");
        auto readback = CreateBuffer(*deviceResources_->device(), *bufferSize, D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_FLAG_NONE, L"Ch18 Shader Variant Readback", true);
        if (!descriptors)
        {
            return std::unexpected(std::move(descriptors.error()));
        }
        if (!output)
        {
            return std::unexpected(std::move(output.error()));
        }
        if (!readback)
        {
            return std::unexpected(std::move(readback.error()));
        }

        slot.descriptors = *descriptors;
        slot.output = std::move(*output);
        slot.outputReadback = std::move(*readback);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = kMaximumElementCount;
        uav.Buffer.StructureByteStride = sizeof(ThreadOutput);
        deviceResources_->device()->CreateUnorderedAccessView(slot.output.Get(), nullptr, &uav,
                                                              slot.descriptors.CpuHandle(OutputUav));

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = kMaximumElementCount;
        srv.Buffer.StructureByteStride = sizeof(ThreadOutput);
        deviceResources_->device()->CreateShaderResourceView(slot.output.Get(), &srv,
                                                             slot.descriptors.CpuHandle(OutputSrv));
    }
    size_ = size;
    hasRendered_ = false;
    return {};
}

LabConfiguration RendererCore::ActiveConfiguration() const noexcept
{
    return headless_ ? headlessConfiguration_ : interactiveConfiguration_;
}

RendererCore::VariantPipeline const *RendererCore::FindPipeline(ShaderVariant const variant) const noexcept
{
    auto const iterator =
        std::find_if(variantPipelines_.begin(), variantPipelines_.end(),
                     [variant](VariantPipeline const &pipeline) { return pipeline.variant == variant; });
    return iterator == variantPipelines_.end() ? nullptr : &*iterator;
}

lgp::framework::Status RendererCore::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    if (auto status = QueryWaveLaneCapability(); !status)
    {
        return status;
    }
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
                                              lgp::framework::Extent2D const drawableSize)
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

    if (edition_ == LabEdition::Solution)
    {
        if (context.input.WasKeyPressed('1'))
        {
            interactiveConfiguration_.variant = ShaderVariant::CoherentLowPressure;
        }
        if (context.input.WasKeyPressed('2'))
        {
            interactiveConfiguration_.variant = ShaderVariant::DivergentLowPressure;
        }
        if (context.input.WasKeyPressed('3'))
        {
            interactiveConfiguration_.variant = ShaderVariant::CoherentHighLiveRange;
        }
        if (context.input.WasKeyPressed('4'))
        {
            interactiveConfiguration_.variant = ShaderVariant::CoherentShortLiveRange;
        }
    }
    if (context.input.WasKeyPressed('Z'))
    {
        interactiveConfiguration_.diagnosticView = DiagnosticView::OutputValue;
    }
    if (context.input.WasKeyPressed('X'))
    {
        interactiveConfiguration_.diagnosticView = DiagnosticView::BranchClassification;
    }
    if (context.input.WasKeyPressed('C'))
    {
        interactiveConfiguration_.diagnosticView = DiagnosticView::AbstractOccupancyModel;
    }
    if (context.input.WasKeyPressed('V'))
    {
        interactiveConfiguration_.diagnosticView = DiagnosticView::EvidenceBoundaries;
    }
    return {};
}

lgp::framework::Status RendererCore::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 18 frame slot is out of range."));
    }

    LabConfiguration const configuration = ActiveConfiguration();
    if (auto status = ValidateLabConfiguration(configuration, edition_); !status)
    {
        return status;
    }
    VariantPipeline const *const variantPipeline = FindPipeline(configuration.variant);
    if (variantPipeline == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("Render", "The Chapter 18 shader variant has no independent pipeline."));
    }
    auto abstractModel = BuildAbstractModelEvidence(configuration.variant, configuration.elementCount);
    if (!abstractModel)
    {
        return std::unexpected(std::move(abstractModel.error()));
    }

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);

    BufferBarrierState const outputBefore = slot.initialized ? CopySourceState() : NoAccessState();
    std::vector<D3D12_BUFFER_BARRIER> bufferBarriers{
        MakeBufferBarrier(*slot.output.Get(), outputBefore, ComputeUnorderedAccessState()),
    };
    SubmitBufferBarriers(commandList, bufferBarriers);

    DispatchConstants const dispatch{configuration.elementCount};
    commandList.SetComputeRootSignature(computeRootSignature_.Get());
    commandList.SetComputeRoot32BitConstants(ComputeConstants, sizeof(dispatch) / sizeof(std::uint32_t), &dispatch, 0U);
    commandList.SetComputeRootDescriptorTable(ComputeUavTable, slot.descriptors.GpuHandle(OutputUav));
    commandList.SetPipelineState(variantPipeline->pipeline.Get());
    commandList.Dispatch(configuration.elementCount / kThreadGroupSize, 1U, 1U);

    bufferBarriers = {
        MakeBufferBarrier(*slot.output.Get(), ComputeUnorderedAccessState(), PixelShaderResourceState()),
    };
    SubmitBufferBarriers(commandList, bufferBarriers);

    std::vector<D3D12_TEXTURE_BARRIER> textureBarriers{
        MakeTextureBarrier(*frameContext.renderTarget, FrameStartState(frameContext), RenderTargetState()),
    };
    SubmitTextureBarriers(commandList, textureBarriers);

    float const clearColor[]{0.01F, 0.01F, 0.02F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    DisplayConstants const display{
        size_.width,
        size_.height,
        configuration.elementCount,
        static_cast<std::uint32_t>(configuration.diagnosticView),
        static_cast<std::uint32_t>(configuration.variant),
        kOutputValidStatus,
        static_cast<std::uint32_t>(abstractModel->occupancy.residentWaves),
        static_cast<std::uint32_t>(abstractModel->teachingArchitecture.maximumResidentWavesPerProcessingBlock),
        waveLaneCapability_.waveOpsSupported ? 1U : 0U,
        waveLaneCapability_.minimumLaneCount,
        waveLaneCapability_.maximumLaneCount,
        waveLaneCapability_.isWarp ? 1U : 0U,
    };
    commandList.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    commandList.SetGraphicsRoot32BitConstants(GraphicsConstants, sizeof(display) / sizeof(std::uint32_t), &display, 0U);
    commandList.SetGraphicsRootDescriptorTable(GraphicsSrvTable, slot.descriptors.GpuHandle(OutputSrv));
    commandList.SetPipelineState(graphicsPipeline_.Get());
    commandList.DrawInstanced(3U, 1U, 0U, 0U);

    bufferBarriers = {
        MakeBufferBarrier(*slot.output.Get(), PixelShaderResourceState(), CopySourceState()),
    };
    SubmitBufferBarriers(commandList, bufferBarriers);
    commandList.CopyBufferRegion(slot.outputReadback.Get(), 0U, slot.output.Get(), 0U, slot.output.size_in_bytes());

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
    graphicsRootSignature_.Reset();
    computeRootSignature_.Reset();
    variantPipelines_.clear();
    shaderArtifacts_.clear();
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
            lgp::framework::MakeError("ReadBackOutputs", "Chapter 18 has no rendered frame to read back."));
    }
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }

    auto metadata = GetVariantMetadata(lastRenderedConfiguration_.variant);
    if (!metadata)
    {
        return std::unexpected(std::move(metadata.error()));
    }
    auto abstractModel =
        BuildAbstractModelEvidence(lastRenderedConfiguration_.variant, lastRenderedConfiguration_.elementCount);
    if (!abstractModel)
    {
        return std::unexpected(std::move(abstractModel.error()));
    }

    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    FrameReadback readback{};
    readback.configuration = lastRenderedConfiguration_;
    readback.metadata = *metadata;
    readback.outputs.resize(lastRenderedConfiguration_.elementCount);
    std::memcpy(readback.outputs.data(), slot.outputReadback.mapped_data(),
                readback.outputs.size() * sizeof(ThreadOutput));
    readback.abstractModel = std::move(*abstractModel);
    readback.runtimeFunctional = BuildRuntimeFunctionalEvidence(lastRenderedConfiguration_, readback.outputs);
    readback.runtimeWaveCapability = waveLaneCapability_;
    readback.physicalProfiling = PhysicalProfilingStatus::NotCollected;
    readback.size = size_;
    readback.frameSlot = lastRenderedFrameSlot_;
    return readback;
}

std::span<ShaderArtifact const> RendererCore::shader_artifacts() const noexcept
{
    return shaderArtifacts_;
}

lgp::framework::Status RendererCore::WriteShaderListings(std::filesystem::path const &directory) const
{
    if (shaderArtifacts_.empty())
    {
        return std::unexpected(
            lgp::framework::MakeError("WriteShaderListings", "Chapter 18 has no compiled shader listings."));
    }

    std::error_code error{};
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        return std::unexpected(
            lgp::framework::MakeError("WriteShaderListings", "Failed to create the shader-listing directory."));
    }

    for (ShaderArtifact const &artifact : shaderArtifacts_)
    {
        std::filesystem::path const path =
            directory / (std::string{VariantFileStem(artifact.variant)} + std::string{".dxil.txt"});
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        if (!stream)
        {
            return std::unexpected(
                lgp::framework::MakeError("WriteShaderListings", "Failed to open a Chapter 18 listing file."));
        }
        stream.write(artifact.dxilDisassembly.data(), static_cast<std::streamsize>(artifact.dxilDisassembly.size()));
        if (!stream)
        {
            return std::unexpected(
                lgp::framework::MakeError("WriteShaderListings", "Failed to write a Chapter 18 listing file."));
        }
    }
    return {};
}

} // namespace ch18::shader_occupancy::gpu
