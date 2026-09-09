#include "GpuLabSupport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <utility>

namespace ch31::auto_exposure::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorCount = 12U;
enum DescriptorIndex : UINT
{
    RecordsUav = 0U,
    SceneUav = 1U,
    HistogramUav = 2U,
    PartialsUav = 3U,
    StatisticsUav = 4U,
    MeterUav = 5U,
    ExposureUav = 6U,
    HistoryUav = 7U,
    RecordsSrv = 8U,
    HistogramSrv = 9U,
    MeterSrv = 10U,
    ExposureSrv = 11U,
};
enum ComputeRootParameter : UINT
{
    ComputeConstants = 0U,
    ComputeResources = 1U,
};
enum GraphicsRootParameter : UINT
{
    GraphicsConstants = 0U,
    GraphicsResources = 1U,
};

inline constexpr std::uint32_t kFlagUseCentreWeighting = 1U << 0U;
inline constexpr std::uint32_t kFlagUseMask = 1U << 1U;
inline constexpr std::uint32_t kFlagCameraCut = 1U << 2U;
inline constexpr std::uint32_t kFlagUiEnabled = 1U << 3U;
inline constexpr std::uint32_t kFlagResetHistory = 1U << 4U;

// Byte offsets inside the per-slot staging buffer that the frame's four singleton records and the history slots are
// copied into. They are 256-byte aligned so a copy destination is easy to read in a capture rather than because any
// API demands it.
inline constexpr std::uint64_t kReadbackBinsOffset = 0U;
inline constexpr std::uint64_t kReadbackStatisticsOffset = 2048U;
inline constexpr std::uint64_t kReadbackMeterOffset = 2304U;
inline constexpr std::uint64_t kReadbackExposureOffset = 2560U;
inline constexpr std::uint64_t kReadbackHistoryOffset = 2816U;
inline constexpr std::uint64_t kReadbackTotalBytes = 2880U;
static_assert(kMaximumBinCount * sizeof(HistogramBin) <= kReadbackStatisticsOffset - kReadbackBinsOffset);
static_assert(sizeof(HistogramStatisticsRecord) <= kReadbackMeterOffset - kReadbackStatisticsOffset);
static_assert(sizeof(MeterRecord) <= kReadbackExposureOffset - kReadbackMeterOffset);
static_assert(sizeof(ExposureRecord) <= kReadbackHistoryOffset - kReadbackExposureOffset);
static_assert(kHistorySlotCount * sizeof(ExposureHistorySlot) <= kReadbackTotalBytes - kReadbackHistoryOffset);

// Mirrors the LabConstants cbuffer in AutoExposureShared.hlsli field for field.
struct LabConstants final
{
    std::uint32_t displayWidth{};
    std::uint32_t displayHeight{};
    std::uint32_t flags{};
    std::uint32_t sceneVariant{};
    std::uint32_t maskVariant{};
    std::uint32_t binCount{};
    std::uint32_t blackSamplePolicy{};
    std::uint32_t belowRangePolicy{};
    std::uint32_t aboveRangePolicy{};
    std::uint32_t meteringPolicy{};
    std::uint32_t exposureMode{};
    std::uint32_t preExposureMode{};
    std::uint32_t displayTransferFunction{};
    std::uint32_t frameIndex{};
    std::uint32_t animationFrame{};
    std::uint32_t outlierColumnCount{};
    std::uint32_t lowerPercentileFixed{};
    std::uint32_t upperPercentileFixed{};
    std::uint32_t configurationIdentityLow{};
    std::uint32_t configurationIdentityHigh{};
    std::uint32_t historyReadSlot{};
    std::uint32_t historyWriteSlot{};
    std::uint32_t tileCount{};
    std::uint32_t tileCountX{};
    std::uint32_t stageOrderWord{};
    std::uint32_t stageCount{};
    float minimumLog2Luminance{};
    float maximumLog2Luminance{};
    float centreWeight{};
    float edgeWeight{};
    float centreFalloffPower{};
    float middleGreyLuminance{};
    float exposureCompensationStops{};
    float minimumExposureStops{};
    float maximumExposureStops{};
    float exposureIncreaseSpeed{};
    float exposureDecreaseSpeed{};
    float manualExposureStops{};
    float resetSeedStops{};
    float initialPreExposure{};
    float fixedPreExposure{};
    float minimumPreExposure{};
    float maximumPreExposure{};
    float frameDeltaSeconds{};
    float baseLuminance{};
    float outlierLuminance{};
    float centreLuminance{};
    float edgeLuminance{};
    float maskedLuminance{};
    float backgroundLuminance{};
    float ladderBinOffset{};
    float uiAlpha{};
    float uiColorR{};
    float uiColorG{};
    float uiColorB{};
};
static_assert(sizeof(LabConstants) == 220U);
static_assert(sizeof(LabConstants) % sizeof(std::uint32_t) == 0U);
// A root signature holds 64 DWORDs. The constants plus the one descriptor table must fit, and the assertion is here
// so that adding a field is a build failure rather than a root-signature creation failure at runtime.
static_assert((sizeof(LabConstants) / sizeof(std::uint32_t)) + 1U <= 64U);

struct DisplayConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t debugView{};
    std::uint32_t expectedStatus{};
    std::uint32_t binCount{};
    float minimumLog2Luminance{};
    float maximumLog2Luminance{};
    float exposureStopRange{};
};
static_assert(sizeof(DisplayConstants) == 32U);

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] std::string ContractMessage(char const *what, ContractError error)
{
    return std::string{what} + " reported ch31::auto_exposure::ContractError code " +
           std::to_string(static_cast<unsigned>(error)) + ".";
}

[[nodiscard]] lgp::framework::Error MakeContractError(char const *where, char const *what, ContractError error)
{
    return lgp::framework::MakeError(where, ContractMessage(what, error));
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
            lgp::framework::MakeError("ValidateExtent", "Chapter 31 requires a non-empty extent up to 320x192."));
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
[[nodiscard]] BufferBarrierState PixelSrvState() noexcept
{
    return {D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
}
[[nodiscard]] BufferBarrierState CopySourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE};
}
[[nodiscard]] BufferBarrierState CopyDestinationState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST};
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

void SubmitBufferBarriers(ID3D12GraphicsCommandList7 &list, std::span<D3D12_BUFFER_BARRIER const> barriers)
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

[[nodiscard]] std::uint32_t GroupCount(std::uint32_t extent, std::uint32_t groupSize) noexcept
{
    return (extent + groupSize - 1U) / groupSize;
}

[[nodiscard]] std::uint32_t TileCountFor(lgp::framework::Extent2D size) noexcept
{
    return GroupCount(size.width, kHistogramGroupWidth) * GroupCount(size.height, kHistogramGroupHeight);
}

[[nodiscard]] bool SameFloat(float left, float right) noexcept
{
    return left == right;
}

} // namespace

HistogramLayout MakeHistogramLayout(LabConfiguration const &configuration) noexcept
{
    return {.binCount = configuration.binCount,
            .minimumLog2Luminance = static_cast<double>(configuration.minimumLog2Luminance),
            .maximumLog2Luminance = static_cast<double>(configuration.maximumLog2Luminance)};
}

HistogramSettings MakeHistogramSettings(LabConfiguration const &configuration) noexcept
{
    return {.layout = MakeHistogramLayout(configuration),
            .blackSamples = configuration.blackSamples,
            .belowRange = configuration.belowRange,
            .aboveRange = configuration.aboveRange};
}

CentreWeightSettings MakeCentreWeightSettings(LabConfiguration const &configuration) noexcept
{
    return {.centreWeight = static_cast<double>(configuration.centreWeight),
            .edgeWeight = static_cast<double>(configuration.edgeWeight),
            .falloffPower = static_cast<double>(configuration.centreFalloffPower)};
}

MeteringSettings MakeMeteringSettings(LabConfiguration const &configuration) noexcept
{
    return {.policy = configuration.meteringPolicy,
            .window = {.lowerPercentile = static_cast<double>(configuration.lowerPercentile),
                       .upperPercentile = static_cast<double>(configuration.upperPercentile)}};
}

TargetExposureSettings MakeTargetExposureSettings(LabConfiguration const &configuration) noexcept
{
    return {.calibration = {.middleGreyLuminance = static_cast<double>(configuration.middleGreyLuminance)},
            .exposureCompensationStops = static_cast<double>(configuration.exposureCompensationStops),
            .minimumExposureStops = static_cast<double>(configuration.minimumExposureStops),
            .maximumExposureStops = static_cast<double>(configuration.maximumExposureStops)};
}

AdaptationSettings MakeAdaptationSettings(LabConfiguration const &configuration) noexcept
{
    return {.exposureIncreaseSpeedPerSecond = static_cast<double>(configuration.exposureIncreaseSpeedPerSecond),
            .exposureDecreaseSpeedPerSecond = static_cast<double>(configuration.exposureDecreaseSpeedPerSecond)};
}

PreExposureSettings MakePreExposureSettings(LabConfiguration const &configuration) noexcept
{
    return {.mode = configuration.preExposureMode,
            .fixedPreExposure = static_cast<double>(configuration.fixedPreExposure),
            .minimumPreExposure = static_cast<double>(configuration.minimumPreExposure),
            .maximumPreExposure = static_cast<double>(configuration.maximumPreExposure)};
}

double MaskValueAt(LabConfiguration const &configuration, lgp::framework::Extent2D extent, std::uint32_t x,
                   std::uint32_t y) noexcept
{
    if (!configuration.useMask)
    {
        return 1.0;
    }
    switch (configuration.maskVariant)
    {
    case MaskVariant::CentreRectangle:
    {
        // Integer comparisons, exactly as the shader evaluates them, so the declared identity and the measured one
        // cannot disagree about a boundary pixel.
        bool const inside = (4U * x) >= extent.width && (4U * x) < (3U * extent.width) && (4U * y) >= extent.height &&
                            (4U * y) < (3U * extent.height);
        return inside ? 1.0 : 0.0;
    }
    case MaskVariant::HalfLeft:
        return (2U * x) < extent.width ? 0.5 : 1.0;
    case MaskVariant::FullFrame:
    default:
        return 1.0;
    }
}

std::vector<double> BuildMaskImage(LabConfiguration const &configuration, lgp::framework::Extent2D extent)
{
    std::vector<double> weights{};
    weights.resize(static_cast<std::size_t>(extent.width) * extent.height);
    for (std::uint32_t y = 0U; y < extent.height; ++y)
    {
        for (std::uint32_t x = 0U; x < extent.width; ++x)
        {
            weights[(static_cast<std::size_t>(y) * extent.width) + x] = MaskValueAt(configuration, extent, x, y);
        }
    }
    return weights;
}

std::expected<std::uint64_t, lgp::framework::Error> ComputeMaskIdentity(LabConfiguration const &configuration,
                                                                        lgp::framework::Extent2D extent)
{
    std::vector<double> const weights = BuildMaskImage(configuration, extent);
    auto const identity =
        MaskIdentity({.extent = {.width = extent.width, .height = extent.height}, .weights = std::span{weights}});
    if (!identity)
    {
        return std::unexpected(MakeContractError("ComputeMaskIdentity", "MaskIdentity", identity.error()));
    }
    return *identity;
}

std::expected<MeteringConfiguration, lgp::framework::Error> MakeMeteringConfiguration(
    LabConfiguration const &configuration, lgp::framework::Extent2D extent)
{
    MeteringConfiguration result{};
    result.histogram = MakeHistogramSettings(configuration);
    result.weighting.useCentreWeighting = configuration.useCentreWeighting;
    result.weighting.centre = MakeCentreWeightSettings(configuration);
    result.weighting.useMask = configuration.useMask;
    if (configuration.useMask)
    {
        auto const identity = ComputeMaskIdentity(configuration, extent);
        if (!identity)
        {
            return std::unexpected(identity.error());
        }
        result.weighting.maskIdentity = *identity;
    }
    result.metering = MakeMeteringSettings(configuration);
    result.mode = configuration.exposureMode;
    return result;
}

LabConfiguration DefaultConfiguration(LabVariant variant) noexcept
{
    LabConfiguration configuration{};
    if (variant == LabVariant::Starter)
    {
        // The Starter declares its exposure and its storage scale. It owns no measurement, so it owns no weighting
        // and no automatic mode either.
        configuration.exposureMode = ExposureMode::Manual;
        configuration.preExposureMode = PreExposureMode::Fixed;
        configuration.manualExposureStops = 1.5F;
        configuration.fixedPreExposure = 2.0F;
        configuration.useCentreWeighting = false;
        configuration.useMask = false;
    }
    return configuration;
}

lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration, LabVariant variant)
{
    auto const refuse = [](char const *message) -> lgp::framework::Status
    { return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration", message)); };

    if (static_cast<std::uint32_t>(configuration.debugView) >= kDebugViewCount ||
        static_cast<std::uint32_t>(configuration.sceneVariant) >= kSceneVariantCount ||
        static_cast<std::uint32_t>(configuration.maskVariant) >= kMaskVariantCount ||
        static_cast<std::uint32_t>(configuration.transferFunction) >
            static_cast<std::uint32_t>(TransferFunction::Gamma22))
    {
        return refuse("Chapter 31 received an enumerator outside its declared range.");
    }
    if (configuration.frameIndex > kMaximumLabFrameIndex)
    {
        // Every reuse decision asks whether this frame follows the one that produced the stored state, so a frame
        // index with no successor would let a stale state look current.
        return refuse("Chapter 31 refuses the last representable frame index, which has no successor.");
    }
    if (configuration.outlierColumnCount > kMaximumWidth)
    {
        return refuse("Chapter 31 outlier column count exceeds the widest legal frame.");
    }

    // The layout, the metering settings, the centre profile and the mode are all validated by the contract function
    // whose identity the adaptation loop compares, so the lab cannot drift from the rules it claims to follow.
    MeteringConfiguration probe{};
    probe.histogram = MakeHistogramSettings(configuration);
    probe.weighting.useCentreWeighting = configuration.useCentreWeighting;
    probe.weighting.centre = MakeCentreWeightSettings(configuration);
    probe.weighting.useMask = configuration.useMask;
    probe.metering = MakeMeteringSettings(configuration);
    probe.mode = configuration.exposureMode;
    if (auto const identity = MeteringConfigurationIdentity(probe); !identity)
    {
        return std::unexpected(
            MakeContractError("ValidateLabConfiguration", "MeteringConfigurationIdentity", identity.error()));
    }
    if (auto const target = ComputeTargetExposure(1.0, MakeTargetExposureSettings(configuration)); !target)
    {
        return std::unexpected(MakeContractError("ValidateLabConfiguration", "ComputeTargetExposure", target.error()));
    }
    AdaptationSettings const adaptation = MakeAdaptationSettings(configuration);
    if (auto const alpha = SmoothingAlpha(adaptation.exposureIncreaseSpeedPerSecond, 0.0); !alpha)
    {
        return std::unexpected(MakeContractError("ValidateLabConfiguration", "SmoothingAlpha", alpha.error()));
    }
    if (auto const alpha = SmoothingAlpha(adaptation.exposureDecreaseSpeedPerSecond, 0.0); !alpha)
    {
        return std::unexpected(MakeContractError("ValidateLabConfiguration", "SmoothingAlpha", alpha.error()));
    }
    if (auto const preExposure =
            ComputeNextFramePreExposure({.scale = 1.0, .stops = 0.0}, 1.0, MakePreExposureSettings(configuration));
        !preExposure)
    {
        return std::unexpected(
            MakeContractError("ValidateLabConfiguration", "ComputeNextFramePreExposure", preExposure.error()));
    }
    if (auto const frame =
            ValidateExposureFrame({.frameIndex = configuration.frameIndex,
                                   .preExposure = static_cast<double>(configuration.initialPreExposure),
                                   .frameDeltaSeconds = static_cast<double>(configuration.frameDeltaSeconds),
                                   .cameraCut = configuration.cameraCut});
        !frame)
    {
        return std::unexpected(MakeContractError("ValidateLabConfiguration", "ValidateExposureFrame", frame.error()));
    }
    if (!std::isfinite(configuration.manualExposureStops) ||
        std::abs(static_cast<double>(configuration.manualExposureStops)) > kMaximumExposureStops ||
        !std::isfinite(configuration.resetSeedStops) ||
        std::abs(static_cast<double>(configuration.resetSeedStops)) > kMaximumExposureStops)
    {
        return refuse("Chapter 31 manual and reset-seed stops must be finite and inside the contract's stop limit.");
    }
    for (float const luminance :
         {configuration.baseLuminance, configuration.outlierLuminance, configuration.centreLuminance,
          configuration.edgeLuminance, configuration.maskedLuminance, configuration.backgroundLuminance})
    {
        if (!std::isfinite(luminance) || luminance < 0.0F ||
            static_cast<double>(luminance) > kMaximumLabSceneLinearValue)
        {
            return refuse("Chapter 31 scene luminances must be finite and inside the compositing seam.");
        }
    }
    if (!std::isfinite(configuration.ladderBinOffset) || configuration.ladderBinOffset < 0.0F ||
        configuration.ladderBinOffset >= 1.0F)
    {
        return refuse("Chapter 31 ladder offsets must lie inside one bin.");
    }
    if (!std::isfinite(configuration.uiAlpha) || configuration.uiAlpha < 0.0F || configuration.uiAlpha > 1.0F)
    {
        return refuse("Chapter 31 UI alpha must lie in the unit range.");
    }

    if (variant == LabVariant::Starter)
    {
        if (configuration.exposureMode != ExposureMode::Manual)
        {
            return refuse("The Chapter 31 Starter measures nothing, so it only runs a declared manual exposure.");
        }
        if (configuration.preExposureMode != PreExposureMode::Fixed)
        {
            return refuse("The Chapter 31 Starter has no committed exposure to derive a pre-exposure from.");
        }
        if (configuration.useCentreWeighting || configuration.useMask)
        {
            return refuse("The Chapter 31 Starter builds no histogram, so a metering weight would weight nothing.");
        }
    }
    return {};
}

std::uint32_t BuildExecutedStages(LabConfiguration const &configuration, LabVariant variant,
                                  std::span<LabStage> stages) noexcept
{
    (void)configuration;
    std::uint32_t count = 0U;
    auto const push = [&stages, &count](LabStage stage)
    {
        if (count < stages.size())
        {
            stages[count] = stage;
            ++count;
        }
    };
    push(LabStage::Clear);
    push(LabStage::Scene);
    if (variant == LabVariant::Solution)
    {
        push(LabStage::Histogram);
        push(LabStage::Reduce);
        push(LabStage::Meter);
        push(LabStage::Adapt);
    }
    else
    {
        push(LabStage::DeclaredExposure);
    }
    push(LabStage::Compose);
    return count;
}

std::uint32_t EncodeStageOrder(std::span<LabStage const> stages) noexcept
{
    std::uint32_t word = 0U;
    std::uint32_t shift = 0U;
    for (LabStage const stage : stages)
    {
        if (shift >= 32U)
        {
            break;
        }
        word |= (static_cast<std::uint32_t>(stage) + 1U) << shift;
        shift += 4U;
    }
    return word;
}

std::uint32_t ExpectedStatus(LabConfiguration const &configuration, LabVariant variant) noexcept
{
    std::uint32_t status =
        kStatusScene | kStatusPreExposureRemoved | kStatusExposure | kStatusToneMap | kStatusDisplayEncode;
    if (variant == LabVariant::Solution)
    {
        status |= kStatusWeighted | kStatusClassified;
    }
    if (configuration.uiEnabled)
    {
        status |= kStatusUi;
    }
    return status;
}

InvalidationReason ComputeInvalidation(LabConfiguration const &previous, LabConfiguration const &current,
                                       bool firstFrame, bool extentChanged, bool explicitReset) noexcept
{
    InvalidationReason reasons = InvalidationReason::None;
    if (firstFrame)
    {
        reasons |= InvalidationReason::FirstFrame;
    }
    if (explicitReset)
    {
        reasons |= InvalidationReason::ExplicitReset;
    }
    if (extentChanged)
    {
        reasons |= InvalidationReason::ExtentChanged;
    }
    if (firstFrame)
    {
        return reasons;
    }
    bool const layoutChanged = previous.binCount != current.binCount ||
                               !SameFloat(previous.minimumLog2Luminance, current.minimumLog2Luminance) ||
                               !SameFloat(previous.maximumLog2Luminance, current.maximumLog2Luminance);
    if (layoutChanged || previous.blackSamples != current.blackSamples || previous.belowRange != current.belowRange ||
        previous.aboveRange != current.aboveRange)
    {
        reasons |= InvalidationReason::HistogramLayoutChanged;
    }
    if (previous.useCentreWeighting != current.useCentreWeighting || previous.useMask != current.useMask ||
        previous.maskVariant != current.maskVariant || !SameFloat(previous.centreWeight, current.centreWeight) ||
        !SameFloat(previous.edgeWeight, current.edgeWeight) ||
        !SameFloat(previous.centreFalloffPower, current.centreFalloffPower))
    {
        // Everything BuildMaskImage reads is here: the mask switch, the mask variant, and the centre profile the
        // mask is composed with. The remaining input to the mask image is the extent, which reports ExtentChanged.
        reasons |= InvalidationReason::WeightingChanged;
    }
    if (previous.meteringPolicy != current.meteringPolicy ||
        !SameFloat(previous.lowerPercentile, current.lowerPercentile) ||
        !SameFloat(previous.upperPercentile, current.upperPercentile))
    {
        reasons |= InvalidationReason::MeteringPolicyChanged;
    }
    if (previous.exposureMode != current.exposureMode)
    {
        reasons |= InvalidationReason::ExposureModeChanged;
    }
    // Every input EvaluateScene reads. The luminances of the analytic variants are listed individually rather than
    // as "the scene fields", because an omission here is invisible: the picture changes and the diagnostic does not
    // say so. The ladder variant additionally reads the histogram layout, which is why a layout change is a scene
    // change as well.
    if (previous.sceneVariant != current.sceneVariant || previous.animationFrame != current.animationFrame ||
        previous.outlierColumnCount != current.outlierColumnCount || layoutChanged ||
        !SameFloat(previous.baseLuminance, current.baseLuminance) ||
        !SameFloat(previous.outlierLuminance, current.outlierLuminance) ||
        !SameFloat(previous.centreLuminance, current.centreLuminance) ||
        !SameFloat(previous.edgeLuminance, current.edgeLuminance) ||
        !SameFloat(previous.maskedLuminance, current.maskedLuminance) ||
        !SameFloat(previous.backgroundLuminance, current.backgroundLuminance) ||
        !SameFloat(previous.ladderBinOffset, current.ladderBinOffset))
    {
        reasons |= InvalidationReason::SceneChanged;
    }
    // Everything the display path composites or encodes with: the transfer function, whether the overlay is drawn,
    // and the overlay's own colour and coverage.
    if (previous.transferFunction != current.transferFunction || previous.uiEnabled != current.uiEnabled ||
        !SameFloat(previous.uiAlpha, current.uiAlpha) || !SameFloat(previous.uiColorR, current.uiColorR) ||
        !SameFloat(previous.uiColorG, current.uiColorG) || !SameFloat(previous.uiColorB, current.uiColorB))
    {
        reasons |= InvalidationReason::DisplayEncodingChanged;
    }
    return reasons;
}

BufferResource::BufferResource(BufferResource &&other) noexcept
    : resource_(std::move(other.resource_)), sizeInBytes_(other.sizeInBytes_), mappedData_(other.mappedData_)
{
    other.sizeInBytes_ = 0U;
    other.mappedData_ = nullptr;
}

BufferResource &BufferResource::operator=(BufferResource &&other) noexcept
{
    if (this != &other)
    {
        Reset();
        resource_ = std::move(other.resource_);
        sizeInBytes_ = other.sizeInBytes_;
        mappedData_ = other.mappedData_;
        other.sizeInBytes_ = 0U;
        other.mappedData_ = nullptr;
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
        resource_->Unmap(0U, nullptr);
    }
    mappedData_ = nullptr;
    sizeInBytes_ = 0U;
    resource_.Reset();
}

std::expected<BufferResource, lgp::framework::Error> CreateBuffer(ID3D12Device10 &device, std::uint64_t sizeInBytes,
                                                                  D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
                                                                  std::wstring_view name, bool mapPersistently)
{
    if (sizeInBytes == 0U)
    {
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 31 buffers must be non-empty."));
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
                                                                createResult, "Failed to create Chapter 31 buffer."));
    }
    if (!name.empty())
    {
        std::wstring const ownedName{name};
        if (HRESULT const nameResult = result.resource_->SetName(ownedName.c_str()); FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name Chapter 31 buffer."));
        }
    }
    if (mapPersistently)
    {
        D3D12_RANGE const readRange{0U, static_cast<SIZE_T>(sizeInBytes)};
        void *mapping = nullptr;
        if (HRESULT const mapResult = result.resource_->Map(0U, &readRange, &mapping); FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map Chapter 31 readback."));
        }
        result.mappedData_ = static_cast<std::byte *>(mapping);
    }
    result.sizeInBytes_ = sizeInBytes;
    return result;
}

RendererCore::RendererCore(std::filesystem::path shaderPath, LabVariant variant)
    : shaderPath_(std::move(shaderPath)), variant_(variant), interactiveConfiguration_(DefaultConfiguration(variant))
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
    options.includeDirectories = {shaderPath_.parent_path(), shaderPath_.parent_path().parent_path() / "Common"};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif
    if (auto status = CompileShader(compiler, options, L"ClearCS", L"cs_6_0", clearShader_); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"SceneCS", L"cs_6_0", sceneShader_); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"ExposureCS", L"cs_6_0", exposureShader_); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"ComposeCS", L"cs_6_0", composeShader_); !status)
    {
        return status;
    }
    if (variant_ == LabVariant::Solution)
    {
        if (auto status = CompileShader(compiler, options, L"HistogramCS", L"cs_6_0", histogramShader_); !status)
        {
            return status;
        }
        if (auto status = CompileShader(compiler, options, L"ReduceCS", L"cs_6_0", reduceShader_); !status)
        {
            return status;
        }
        if (auto status = CompileShader(compiler, options, L"MeterCS", L"cs_6_0", meterShader_); !status)
        {
            return status;
        }
    }
    if (auto status = CompileShader(compiler, options, L"FullscreenVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    return CompileShader(compiler, options, L"DisplayPS", L"ps_6_0", pixelShader_);
}

lgp::framework::Status RendererCore::CreateRootSignatures()
{
    D3D12_DESCRIPTOR_RANGE computeRange{};
    computeRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    computeRange.NumDescriptors = 8U;
    computeRange.BaseShaderRegister = 0U;
    D3D12_ROOT_PARAMETER computeParameters[2]{};
    computeParameters[ComputeConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[ComputeConstants].Constants.ShaderRegister = 0U;
    computeParameters[ComputeConstants].Constants.Num32BitValues = sizeof(LabConstants) / sizeof(std::uint32_t);
    computeParameters[ComputeResources].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeResources].DescriptorTable = {1U, &computeRange};
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
                                                                "Failed to create Chapter 31 compute root signature."));
    }

    D3D12_DESCRIPTOR_RANGE displayRange{};
    displayRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    displayRange.NumDescriptors = 4U;
    displayRange.BaseShaderRegister = 0U;
    D3D12_ROOT_PARAMETER graphicsParameters[2]{};
    graphicsParameters[GraphicsConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[GraphicsConstants].Constants.ShaderRegister = 0U;
    graphicsParameters[GraphicsConstants].Constants.Num32BitValues = sizeof(DisplayConstants) / sizeof(std::uint32_t);
    graphicsParameters[GraphicsConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    graphicsParameters[GraphicsResources].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParameters[GraphicsResources].DescriptorTable = {1U, &displayRange};
    graphicsParameters[GraphicsResources].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
            "ID3D12Device::CreateRootSignature", createResult, "Failed to create Chapter 31 graphics root signature."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreatePipelines()
{
    auto const createCompute = [this](lgp::framework::CompiledShader const &shader,
                                      ComPtr<ID3D12PipelineState> &pipeline) -> lgp::framework::Status
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
        description.pRootSignature = computeRootSignature_.Get();
        description.CS = shader.Bytecode();
        HRESULT const result = deviceResources_->device()->CreateComputePipelineState(
            &description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
        if (FAILED(result))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState", result,
                                                                    "Failed to create a Chapter 31 compute PSO."));
        }
        return {};
    };

    if (auto status = createCompute(clearShader_, clearPipeline_); !status)
    {
        return status;
    }
    if (auto status = createCompute(sceneShader_, scenePipeline_); !status)
    {
        return status;
    }
    if (auto status = createCompute(exposureShader_, exposurePipeline_); !status)
    {
        return status;
    }
    if (auto status = createCompute(composeShader_, composePipeline_); !status)
    {
        return status;
    }
    if (variant_ == LabVariant::Solution)
    {
        if (auto status = createCompute(histogramShader_, histogramPipeline_); !status)
        {
            return status;
        }
        if (auto status = createCompute(reduceShader_, reducePipeline_); !status)
        {
            return status;
        }
        if (auto status = createCompute(meterShader_, meterPipeline_); !status)
        {
            return status;
        }
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
    HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
        &graphics, IID_PPV_ARGS(graphicsPipeline_.GetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create Chapter 31 graphics PSO."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreateResources(lgp::framework::Extent2D size)
{
    if (auto status = ValidateExtent(size); !status)
    {
        return status;
    }

    std::uint64_t const pixelCount = static_cast<std::uint64_t>(size.width) * size.height;
    std::uint64_t const totalWeight = pixelCount * kWeightOne;
    if (totalWeight > kMaximumLabTotalWeight)
    {
        // The lab accumulates weight in 32-bit integers, so the extent that would exceed the accumulator is refused
        // instead of producing a wrapped histogram that looks entirely reasonable.
        return std::unexpected(lgp::framework::MakeError(
            "CreateResources", "Chapter 31 refuses an extent whose total sample weight exceeds a 32-bit accumulator."));
    }

    std::uint64_t const recordBytes = pixelCount * sizeof(PixelRecord);
    std::uint64_t const sceneBytes = pixelCount * sizeof(SceneRecord);
    std::uint64_t const histogramBytes = static_cast<std::uint64_t>(kMaximumBinCount) * sizeof(HistogramBin);
    std::uint64_t const partialBytes = static_cast<std::uint64_t>(kMaximumTileCount) * sizeof(TilePartial);

    auto historyBuffer = CreateBuffer(
        *deviceResources_->device(), static_cast<std::uint64_t>(kHistorySlotCount) * sizeof(ExposureHistorySlot),
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch31 exposure history");
    if (!historyBuffer)
    {
        return std::unexpected(std::move(historyBuffer.error()));
    }
    history_ = std::move(*historyBuffer);
    historyInitialized_ = false;
    pendingHistoryInjection_.reset();

    auto historyUpload = CreateBuffer(
        *deviceResources_->device(), static_cast<std::uint64_t>(kHistorySlotCount) * sizeof(ExposureHistorySlot),
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch31 exposure history upload", true);
    if (!historyUpload)
    {
        return std::unexpected(std::move(historyUpload.error()));
    }
    historyUpload_ = std::move(*historyUpload);

    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlotResources &slot : frameSlots_)
    {
        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorCount);
        if (!descriptors)
        {
            return std::unexpected(std::move(descriptors.error()));
        }
        auto records = CreateBuffer(*deviceResources_->device(), recordBytes, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch31 pixel records");
        if (!records)
        {
            return std::unexpected(std::move(records.error()));
        }
        auto scene = CreateBuffer(*deviceResources_->device(), sceneBytes, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch31 scene records");
        if (!scene)
        {
            return std::unexpected(std::move(scene.error()));
        }
        auto histogram = CreateBuffer(*deviceResources_->device(), histogramBytes, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch31 luminance histogram");
        if (!histogram)
        {
            return std::unexpected(std::move(histogram.error()));
        }
        auto partials = CreateBuffer(*deviceResources_->device(), partialBytes, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch31 tile partials");
        if (!partials)
        {
            return std::unexpected(std::move(partials.error()));
        }
        auto statistics =
            CreateBuffer(*deviceResources_->device(), sizeof(HistogramStatisticsRecord), D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch31 histogram statistics");
        if (!statistics)
        {
            return std::unexpected(std::move(statistics.error()));
        }
        auto meter = CreateBuffer(*deviceResources_->device(), sizeof(MeterRecord), D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch31 meter record");
        if (!meter)
        {
            return std::unexpected(std::move(meter.error()));
        }
        auto exposure = CreateBuffer(*deviceResources_->device(), sizeof(ExposureRecord), D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch31 exposure record");
        if (!exposure)
        {
            return std::unexpected(std::move(exposure.error()));
        }
        auto recordsReadback = CreateBuffer(*deviceResources_->device(), recordBytes, D3D12_HEAP_TYPE_READBACK,
                                            D3D12_RESOURCE_FLAG_NONE, L"Ch31 record readback", true);
        if (!recordsReadback)
        {
            return std::unexpected(std::move(recordsReadback.error()));
        }
        auto frameReadback = CreateBuffer(*deviceResources_->device(), kReadbackTotalBytes, D3D12_HEAP_TYPE_READBACK,
                                          D3D12_RESOURCE_FLAG_NONE, L"Ch31 frame readback", true);
        if (!frameReadback)
        {
            return std::unexpected(std::move(frameReadback.error()));
        }

        slot.descriptors = *descriptors;
        slot.records = std::move(*records);
        slot.scene = std::move(*scene);
        slot.histogram = std::move(*histogram);
        slot.partials = std::move(*partials);
        slot.statistics = std::move(*statistics);
        slot.meter = std::move(*meter);
        slot.exposure = std::move(*exposure);
        slot.recordsReadback = std::move(*recordsReadback);
        slot.frameReadback = std::move(*frameReadback);
        slot.initialized = false;

        auto const createStructuredUav =
            [this, &slot](DescriptorIndex index, ID3D12Resource *resource, std::uint64_t elements, std::uint32_t stride)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = static_cast<UINT>(elements);
            uav.Buffer.StructureByteStride = stride;
            deviceResources_->device()->CreateUnorderedAccessView(resource, nullptr, &uav,
                                                                  slot.descriptors.CpuHandle(index));
        };
        auto const createStructuredSrv =
            [this, &slot](DescriptorIndex index, ID3D12Resource *resource, std::uint64_t elements, std::uint32_t stride)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.NumElements = static_cast<UINT>(elements);
            srv.Buffer.StructureByteStride = stride;
            deviceResources_->device()->CreateShaderResourceView(resource, &srv, slot.descriptors.CpuHandle(index));
        };
        createStructuredUav(RecordsUav, slot.records.Get(), pixelCount, sizeof(PixelRecord));
        createStructuredUav(SceneUav, slot.scene.Get(), pixelCount, sizeof(SceneRecord));
        createStructuredUav(HistogramUav, slot.histogram.Get(), kMaximumBinCount, sizeof(HistogramBin));
        createStructuredUav(PartialsUav, slot.partials.Get(), kMaximumTileCount, sizeof(TilePartial));
        createStructuredUav(StatisticsUav, slot.statistics.Get(), 1U, sizeof(HistogramStatisticsRecord));
        createStructuredUav(MeterUav, slot.meter.Get(), 1U, sizeof(MeterRecord));
        createStructuredUav(ExposureUav, slot.exposure.Get(), 1U, sizeof(ExposureRecord));
        createStructuredUav(HistoryUav, history_.Get(), kHistorySlotCount, sizeof(ExposureHistorySlot));
        createStructuredSrv(RecordsSrv, slot.records.Get(), pixelCount, sizeof(PixelRecord));
        createStructuredSrv(HistogramSrv, slot.histogram.Get(), kMaximumBinCount, sizeof(HistogramBin));
        createStructuredSrv(MeterSrv, slot.meter.Get(), 1U, sizeof(MeterRecord));
        createStructuredSrv(ExposureSrv, slot.exposure.Get(), 1U, sizeof(ExposureRecord));
    }
    size_ = size;
    hasRendered_ = false;
    forceReset_ = true;
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
        interactiveConfiguration_.frameIndex = 0U;
    }
    if (context.input.WasKeyPressed('V'))
    {
        std::uint32_t next = static_cast<std::uint32_t>(interactiveConfiguration_.debugView);
        for (std::uint32_t attempt = 0U; attempt < kDebugViewCount; ++attempt)
        {
            next = (next + 1U) % kDebugViewCount;
            LabConfiguration candidate = interactiveConfiguration_;
            candidate.debugView = static_cast<DebugView>(next);
            if (ValidateLabConfiguration(candidate, variant_))
            {
                interactiveConfiguration_.debugView = candidate.debugView;
                break;
            }
        }
    }
    if (context.input.WasKeyPressed('C'))
    {
        // A camera cut is a single-frame event, so it is armed here and cleared by the frame that consumed it.
        interactiveConfiguration_.cameraCut = true;
    }
    if (context.input.WasKeyPressed('U'))
    {
        interactiveConfiguration_.uiEnabled = !interactiveConfiguration_.uiEnabled;
    }
    if (variant_ == LabVariant::Solution)
    {
        if (context.input.WasKeyPressed('P'))
        {
            std::uint32_t const next = (static_cast<std::uint32_t>(interactiveConfiguration_.meteringPolicy) + 1U) % 3U;
            interactiveConfiguration_.meteringPolicy = static_cast<MeteringPolicy>(next);
        }
        if (context.input.WasKeyPressed('W'))
        {
            interactiveConfiguration_.useCentreWeighting = !interactiveConfiguration_.useCentreWeighting;
        }
        if (context.input.WasKeyPressed('K'))
        {
            interactiveConfiguration_.useMask = !interactiveConfiguration_.useMask;
        }
        if (context.input.WasKeyPressed('M'))
        {
            interactiveConfiguration_.exposureMode = interactiveConfiguration_.exposureMode == ExposureMode::Automatic
                                                         ? ExposureMode::Manual
                                                         : ExposureMode::Automatic;
        }
    }
    if (context.input.WasKeyPressed('S'))
    {
        std::uint32_t const next =
            (static_cast<std::uint32_t>(interactiveConfiguration_.sceneVariant) + 1U) % kSceneVariantCount;
        interactiveConfiguration_.sceneVariant = static_cast<SceneVariant>(next);
    }
    interactiveConfiguration_.frameDeltaSeconds =
        static_cast<float>(std::clamp(context.deltaSeconds, 0.0, kMaximumFrameDeltaSeconds));
    ++interactiveConfiguration_.animationFrame;
    if (interactiveConfiguration_.frameIndex < kMaximumLabFrameIndex)
    {
        ++interactiveConfiguration_.frameIndex;
    }
    return {};
}

LabConfiguration RendererCore::ActiveConfiguration() const noexcept
{
    return headless_ && headlessConfiguration_ ? *headlessConfiguration_ : interactiveConfiguration_;
}

lgp::framework::Status RendererCore::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Render", "Chapter 31 frame slot is out of range."));
    }
    LabConfiguration const configuration = ActiveConfiguration();
    if (auto status = ValidateLabConfiguration(configuration, variant_); !status)
    {
        return status;
    }

    auto const layoutFacts = ValidateHistogramLayout(MakeHistogramLayout(configuration));
    if (!layoutFacts)
    {
        return std::unexpected(MakeContractError("Render", "ValidateHistogramLayout", layoutFacts.error()));
    }
    auto const meteringConfiguration = MakeMeteringConfiguration(configuration, size_);
    if (!meteringConfiguration)
    {
        return std::unexpected(meteringConfiguration.error());
    }
    auto const configurationIdentity = MeteringConfigurationIdentity(*meteringConfiguration);
    if (!configurationIdentity)
    {
        return std::unexpected(
            MakeContractError("Render", "MeteringConfigurationIdentity", configurationIdentity.error()));
    }
    // The percentile boundaries the shader compares against are quantized here, by the same rule the contract uses,
    // so the dispatch never sees a raw double whose last bit could move a boundary.
    auto const quantizePercentile = [](float percentile) -> std::uint32_t
    { return static_cast<std::uint32_t>(std::floor((static_cast<double>(percentile) * kPercentileOne) + 0.5)); };

    std::uint32_t const tileCount = TileCountFor(size_);
    if (tileCount > kMaximumTileCount)
    {
        return std::unexpected(
            lgp::framework::MakeError("Render", "Chapter 31 tile count exceeds the allocated partial buffer."));
    }

    // The explicit restart is the declared one: a configuration that asked for it, or a RequestReset the renderer
    // has not yet consumed. The absence of any stored value at all - a first frame, or a reallocated history
    // resource - is reported by FirstFrame and ExtentChanged instead, so the two causes stay distinguishable.
    InvalidationReason const invalidation =
        ComputeInvalidation(lastRenderedConfiguration_, configuration, !hasRendered_, lastRenderedExtent_ != size_,
                            configuration.resetHistory || forceReset_);
    std::array<LabStage, kMaximumStageCount> stages{};
    std::uint32_t const stageCount = BuildExecutedStages(configuration, variant_, stages);
    std::uint32_t const stageOrderWord = EncodeStageOrder(std::span<LabStage const>{stages.data(), stageCount});
    std::uint32_t const expectedStatus = ExpectedStatus(configuration, variant_);
    // A reset of the *stored adaptation value* is declared, never inferred from a scene change. The scene getting
    // brighter is exactly what adaptation exists for, and the identity comparisons that decide whether a stored
    // value still describes the same measurement are made on the GPU, against the identity the contract computed.
    // What forces a reset here is the absence of any stored value at all: a first frame, a resized frame whose
    // history resource was reallocated, or a request to start over.
    bool const resetHistory = configuration.resetHistory || forceReset_ || !historyInitialized_;

    std::uint32_t const historyWriteSlot = configuration.frameIndex % kHistorySlotCount;
    std::uint32_t const historyReadSlot = (configuration.frameIndex + 1U) % kHistorySlotCount;

    std::uint32_t flags = 0U;
    flags |= configuration.useCentreWeighting ? kFlagUseCentreWeighting : 0U;
    flags |= configuration.useMask ? kFlagUseMask : 0U;
    flags |= configuration.cameraCut ? kFlagCameraCut : 0U;
    flags |= configuration.uiEnabled ? kFlagUiEnabled : 0U;
    flags |= resetHistory ? kFlagResetHistory : 0U;

    LabConstants constants{};
    constants.displayWidth = size_.width;
    constants.displayHeight = size_.height;
    constants.flags = flags;
    constants.sceneVariant = static_cast<std::uint32_t>(configuration.sceneVariant);
    constants.maskVariant = static_cast<std::uint32_t>(configuration.maskVariant);
    constants.binCount = configuration.binCount;
    constants.blackSamplePolicy = static_cast<std::uint32_t>(configuration.blackSamples);
    constants.belowRangePolicy = static_cast<std::uint32_t>(configuration.belowRange);
    constants.aboveRangePolicy = static_cast<std::uint32_t>(configuration.aboveRange);
    constants.meteringPolicy = static_cast<std::uint32_t>(configuration.meteringPolicy);
    constants.exposureMode = static_cast<std::uint32_t>(configuration.exposureMode);
    constants.preExposureMode = static_cast<std::uint32_t>(configuration.preExposureMode);
    constants.displayTransferFunction = static_cast<std::uint32_t>(configuration.transferFunction);
    constants.frameIndex = configuration.frameIndex;
    constants.animationFrame = configuration.animationFrame;
    constants.outlierColumnCount = configuration.outlierColumnCount;
    constants.lowerPercentileFixed = quantizePercentile(configuration.lowerPercentile);
    constants.upperPercentileFixed = quantizePercentile(configuration.upperPercentile);
    constants.configurationIdentityLow = static_cast<std::uint32_t>(*configurationIdentity & 0xFFFF'FFFFULL);
    constants.configurationIdentityHigh = static_cast<std::uint32_t>(*configurationIdentity >> 32U);
    constants.historyReadSlot = historyReadSlot;
    constants.historyWriteSlot = historyWriteSlot;
    constants.tileCount = tileCount;
    constants.tileCountX = GroupCount(size_.width, kHistogramGroupWidth);
    constants.stageOrderWord = stageOrderWord;
    constants.stageCount = stageCount;
    constants.minimumLog2Luminance = configuration.minimumLog2Luminance;
    constants.maximumLog2Luminance = configuration.maximumLog2Luminance;
    constants.centreWeight = configuration.centreWeight;
    constants.edgeWeight = configuration.edgeWeight;
    constants.centreFalloffPower = configuration.centreFalloffPower;
    constants.middleGreyLuminance = configuration.middleGreyLuminance;
    constants.exposureCompensationStops = configuration.exposureCompensationStops;
    constants.minimumExposureStops = configuration.minimumExposureStops;
    constants.maximumExposureStops = configuration.maximumExposureStops;
    constants.exposureIncreaseSpeed = configuration.exposureIncreaseSpeedPerSecond;
    constants.exposureDecreaseSpeed = configuration.exposureDecreaseSpeedPerSecond;
    constants.manualExposureStops = configuration.manualExposureStops;
    constants.resetSeedStops = configuration.resetSeedStops;
    constants.initialPreExposure = configuration.initialPreExposure;
    constants.fixedPreExposure = configuration.fixedPreExposure;
    constants.minimumPreExposure = configuration.minimumPreExposure;
    constants.maximumPreExposure = configuration.maximumPreExposure;
    constants.frameDeltaSeconds = configuration.frameDeltaSeconds;
    constants.baseLuminance = configuration.baseLuminance;
    constants.outlierLuminance = configuration.outlierLuminance;
    constants.centreLuminance = configuration.centreLuminance;
    constants.edgeLuminance = configuration.edgeLuminance;
    constants.maskedLuminance = configuration.maskedLuminance;
    constants.backgroundLuminance = configuration.backgroundLuminance;
    constants.ladderBinOffset = configuration.ladderBinOffset;
    constants.uiAlpha = configuration.uiAlpha;
    constants.uiColorR = configuration.uiColorR;
    constants.uiColorG = configuration.uiColorG;
    constants.uiColorB = configuration.uiColorB;

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    ID3D12GraphicsCommandList7 &list = *frameContext.commandList;
    ID3D12DescriptorHeap *heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    list.SetDescriptorHeaps(1U, heaps);

    // Entry barriers. The four singleton records and the pixel records ended the previous frame as copy sources; the
    // history buffer is sequence-owned, so its previous producer is the previous frame rather than this slot. A
    // pending headless injection makes the history buffer a copy destination first, so the declared slot is in place
    // before the clear stage runs and every access it passes through is named.
    BufferBarrierState const previousSlotState = slot.initialized ? CopySourceState() : NoAccessState();
    BufferBarrierState const previousHistoryState = historyInitialized_ ? CopySourceState() : NoAccessState();
    bool const injectHistory = headless_ && pendingHistoryInjection_.has_value() && historyUpload_.Get() != nullptr &&
                               pendingHistoryInjection_->slotIndex < kHistorySlotCount;
    std::array<D3D12_BUFFER_BARRIER, 8U> const entryBarriers{
        MakeBufferBarrier(*slot.records.Get(), previousSlotState, ComputeUavState()),
        MakeBufferBarrier(*slot.scene.Get(), slot.initialized ? ComputeUavState() : NoAccessState(), ComputeUavState()),
        MakeBufferBarrier(*slot.histogram.Get(), previousSlotState, ComputeUavState()),
        MakeBufferBarrier(*slot.partials.Get(), slot.initialized ? ComputeUavState() : NoAccessState(),
                          ComputeUavState()),
        MakeBufferBarrier(*slot.statistics.Get(), previousSlotState, ComputeUavState()),
        MakeBufferBarrier(*slot.meter.Get(), previousSlotState, ComputeUavState()),
        MakeBufferBarrier(*slot.exposure.Get(), previousSlotState, ComputeUavState()),
        MakeBufferBarrier(*history_.Get(), previousHistoryState,
                          injectHistory ? CopyDestinationState() : ComputeUavState()),
    };
    SubmitBufferBarriers(list, entryBarriers);
    if (injectHistory)
    {
        std::uint64_t const offset =
            static_cast<std::uint64_t>(pendingHistoryInjection_->slotIndex) * sizeof(ExposureHistorySlot);
        std::memcpy(historyUpload_.mapped_data() + offset, &pendingHistoryInjection_->slot,
                    sizeof(ExposureHistorySlot));
        list.CopyBufferRegion(history_.Get(), offset, historyUpload_.Get(), offset, sizeof(ExposureHistorySlot));
        std::array<D3D12_BUFFER_BARRIER, 1U> const injectionBarrier{
            MakeBufferBarrier(*history_.Get(), CopyDestinationState(), ComputeUavState()),
        };
        SubmitBufferBarriers(list, injectionBarrier);
    }
    pendingHistoryInjection_.reset();

    list.SetComputeRootSignature(computeRootSignature_.Get());
    list.SetComputeRootDescriptorTable(ComputeResources, slot.descriptors.GpuHandle(RecordsUav));
    list.SetComputeRoot32BitConstants(ComputeConstants, sizeof(LabConstants) / sizeof(std::uint32_t), &constants, 0U);

    // Every stage writes buffers the next stage reads, so each transition names exactly the resources that carry the
    // dependency rather than draining the whole set.
    auto const orderStages = [&list](std::span<ID3D12Resource *const> resources)
    {
        std::array<D3D12_BUFFER_BARRIER, 8U> barriers{};
        std::size_t count = 0U;
        for (ID3D12Resource *resource : resources)
        {
            if (resource == nullptr || count >= barriers.size())
            {
                continue;
            }
            barriers[count] = MakeBufferBarrier(*resource, ComputeUavState(), ComputeUavState());
            ++count;
        }
        SubmitBufferBarriers(list, std::span<D3D12_BUFFER_BARRIER const>{barriers.data(), count});
    };

    std::uint32_t const clearElements = std::max(kMaximumBinCount, tileCount);
    list.SetPipelineState(clearPipeline_.Get());
    list.Dispatch(GroupCount(clearElements, 64U), 1U, 1U);
    {
        ID3D12Resource *const resources[]{slot.histogram.Get(), slot.partials.Get(), slot.statistics.Get(),
                                          slot.meter.Get(),     slot.exposure.Get(), history_.Get()};
        orderStages(resources);
    }

    list.SetPipelineState(scenePipeline_.Get());
    list.Dispatch(GroupCount(size_.width, kHistogramGroupWidth), GroupCount(size_.height, kHistogramGroupHeight), 1U);
    {
        ID3D12Resource *const resources[]{slot.scene.Get(), slot.records.Get()};
        orderStages(resources);
    }

    if (variant_ == LabVariant::Solution)
    {
        list.SetPipelineState(histogramPipeline_.Get());
        list.Dispatch(GroupCount(size_.width, kHistogramGroupWidth), GroupCount(size_.height, kHistogramGroupHeight),
                      1U);
        {
            ID3D12Resource *const resources[]{slot.histogram.Get(), slot.partials.Get(), slot.statistics.Get(),
                                              slot.scene.Get(), slot.records.Get()};
            orderStages(resources);
        }

        list.SetPipelineState(reducePipeline_.Get());
        list.Dispatch(1U, 1U, 1U);
        {
            ID3D12Resource *const resources[]{slot.statistics.Get()};
            orderStages(resources);
        }

        list.SetPipelineState(meterPipeline_.Get());
        list.Dispatch(1U, 1U, 1U);
        {
            ID3D12Resource *const resources[]{slot.meter.Get()};
            orderStages(resources);
        }
    }

    list.SetPipelineState(exposurePipeline_.Get());
    list.Dispatch(1U, 1U, 1U);
    {
        ID3D12Resource *const resources[]{slot.exposure.Get(), history_.Get()};
        orderStages(resources);
    }

    list.SetPipelineState(composePipeline_.Get());
    list.Dispatch(GroupCount(size_.width, kHistogramGroupWidth), GroupCount(size_.height, kHistogramGroupHeight), 1U);

    std::array<D3D12_BUFFER_BARRIER, 4U> const displayBarriers{
        MakeBufferBarrier(*slot.records.Get(), ComputeUavState(), PixelSrvState()),
        MakeBufferBarrier(*slot.histogram.Get(), ComputeUavState(), PixelSrvState()),
        MakeBufferBarrier(*slot.meter.Get(), ComputeUavState(), PixelSrvState()),
        MakeBufferBarrier(*slot.exposure.Get(), ComputeUavState(), PixelSrvState()),
    };
    SubmitBufferBarriers(list, displayBarriers);

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
        expectedStatus,
        configuration.binCount,
        configuration.minimumLog2Luminance,
        configuration.maximumLog2Luminance,
        std::max(std::abs(configuration.minimumExposureStops), std::abs(configuration.maximumExposureStops)),
    };
    list.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    list.SetGraphicsRoot32BitConstants(GraphicsConstants, sizeof(display) / sizeof(std::uint32_t), &display, 0U);
    list.SetGraphicsRootDescriptorTable(GraphicsResources, slot.descriptors.GpuHandle(RecordsSrv));
    list.SetPipelineState(graphicsPipeline_.Get());
    list.DrawInstanced(3U, 1U, 0U, 0U);

    std::array<D3D12_BUFFER_BARRIER, 6U> const copyBarriers{
        MakeBufferBarrier(*slot.records.Get(), PixelSrvState(), CopySourceState()),
        MakeBufferBarrier(*slot.histogram.Get(), PixelSrvState(), CopySourceState()),
        MakeBufferBarrier(*slot.meter.Get(), PixelSrvState(), CopySourceState()),
        MakeBufferBarrier(*slot.exposure.Get(), PixelSrvState(), CopySourceState()),
        MakeBufferBarrier(*slot.statistics.Get(), ComputeUavState(), CopySourceState()),
        MakeBufferBarrier(*history_.Get(), ComputeUavState(), CopySourceState()),
    };
    SubmitBufferBarriers(list, copyBarriers);
    list.CopyBufferRegion(slot.recordsReadback.Get(), 0U, slot.records.Get(), 0U, slot.records.size_in_bytes());
    list.CopyBufferRegion(slot.frameReadback.Get(), kReadbackBinsOffset, slot.histogram.Get(), 0U,
                          slot.histogram.size_in_bytes());
    list.CopyBufferRegion(slot.frameReadback.Get(), kReadbackStatisticsOffset, slot.statistics.Get(), 0U,
                          slot.statistics.size_in_bytes());
    list.CopyBufferRegion(slot.frameReadback.Get(), kReadbackMeterOffset, slot.meter.Get(), 0U,
                          slot.meter.size_in_bytes());
    list.CopyBufferRegion(slot.frameReadback.Get(), kReadbackExposureOffset, slot.exposure.Get(), 0U,
                          slot.exposure.size_in_bytes());
    list.CopyBufferRegion(slot.frameReadback.Get(), kReadbackHistoryOffset, history_.Get(), 0U,
                          history_.size_in_bytes());
    SubmitTextureTransition(list, *frameContext.renderTarget, RenderTargetState(), FrameEndState(frameContext));

    slot.initialized = true;
    historyInitialized_ = true;
    lastRenderedConfiguration_ = configuration;
    lastRenderedExtent_ = size_;
    lastMeteringConfiguration_ = *meteringConfiguration;
    lastLayoutFacts_ = *layoutFacts;
    lastConfigurationIdentity_ = *configurationIdentity;
    lastMaskIdentity_ = meteringConfiguration->weighting.maskIdentity;
    lastStages_ = stages;
    lastStageCount_ = stageCount;
    lastStageOrderWord_ = stageOrderWord;
    lastExpectedStatus_ = expectedStatus;
    lastHistoryReadSlot_ = historyReadSlot;
    lastHistoryWriteSlot_ = historyWriteSlot;
    lastTileCount_ = tileCount;
    lastInvalidation_ = invalidation;
    lastRenderedFrameSlot_ = frameContext.frameSlot;
    hasRendered_ = true;
    forceReset_ = false;
    if (!headless_)
    {
        interactiveConfiguration_.cameraCut = false;
    }
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
    historyUpload_ = {};
    pendingHistoryInjection_.reset();
    historyInitialized_ = false;
    hasRendered_ = false;
    size_ = {};
}

void RendererCore::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    DestroyResources(deviceResources);
    graphicsPipeline_.Reset();
    composePipeline_.Reset();
    exposurePipeline_.Reset();
    meterPipeline_.Reset();
    reducePipeline_.Reset();
    histogramPipeline_.Reset();
    scenePipeline_.Reset();
    clearPipeline_.Reset();
    graphicsRootSignature_.Reset();
    computeRootSignature_.Reset();
    clearShader_ = {};
    sceneShader_ = {};
    histogramShader_ = {};
    reduceShader_ = {};
    meterShader_ = {};
    exposureShader_ = {};
    composeShader_ = {};
    vertexShader_ = {};
    pixelShader_ = {};
    deviceResources_ = nullptr;
}

void RendererCore::ConfigureHeadlessTest(LabConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

void RendererCore::RequestReset() noexcept
{
    forceReset_ = true;
}

void RendererCore::InjectHistorySlotForTest(std::uint32_t slotIndex, ExposureHistorySlot const &slot) noexcept
{
    if (!headless_ || slotIndex >= kHistorySlotCount)
    {
        return;
    }
    pendingHistoryInjection_ = PendingHistoryInjection{.slotIndex = slotIndex, .slot = slot};
}

std::expected<FrameReadback, lgp::framework::Error> RendererCore::ReadBackOutputs()
{
    if (deviceResources_ == nullptr || !hasRendered_ || frameSlots_.empty())
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "Chapter 31 has no completed frame to read."));
    }
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }
    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    if (slot.recordsReadback.mapped_data() == nullptr || slot.frameReadback.mapped_data() == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "Chapter 31 readback staging is not mapped."));
    }

    FrameReadback output{};
    output.configuration = lastRenderedConfiguration_;
    output.displaySize = size_;
    output.meteringConfiguration = lastMeteringConfiguration_;
    output.layoutFacts = lastLayoutFacts_;
    output.configurationIdentity = lastConfigurationIdentity_;
    output.maskIdentity = lastMaskIdentity_;
    output.executedStages = lastStages_;
    output.executedStageCount = lastStageCount_;
    output.stageOrderWord = lastStageOrderWord_;
    output.expectedStatus = lastExpectedStatus_;
    output.historyReadSlot = lastHistoryReadSlot_;
    output.historyWriteSlot = lastHistoryWriteSlot_;
    output.tileCount = lastTileCount_;
    output.invalidation = lastInvalidation_;
    output.frameSlot = lastRenderedFrameSlot_;

    output.pixels.resize(static_cast<std::size_t>(size_.width) * size_.height);
    std::memcpy(output.pixels.data(), slot.recordsReadback.mapped_data(), output.pixels.size() * sizeof(PixelRecord));

    std::byte const *const frameBytes = slot.frameReadback.mapped_data();
    output.bins.resize(kMaximumBinCount);
    std::memcpy(output.bins.data(), frameBytes + kReadbackBinsOffset, kMaximumBinCount * sizeof(HistogramBin));
    std::memcpy(&output.statistics, frameBytes + kReadbackStatisticsOffset, sizeof(HistogramStatisticsRecord));
    std::memcpy(&output.meter, frameBytes + kReadbackMeterOffset, sizeof(MeterRecord));
    std::memcpy(&output.exposure, frameBytes + kReadbackExposureOffset, sizeof(ExposureRecord));
    std::memcpy(output.history.data(), frameBytes + kReadbackHistoryOffset,
                kHistorySlotCount * sizeof(ExposureHistorySlot));

    // The two exposures are lifted into the contract's own types here, so a test that wants to reason about them has
    // to say which one it means and cannot hand a committed exposure to something that only accepts a displayed one.
    output.displayed = {.scale = static_cast<double>(output.exposure.displayedScale),
                        .stops = static_cast<double>(output.exposure.displayedStops)};
    output.committed = {.scale = static_cast<double>(output.exposure.committedScale),
                        .stops = static_cast<double>(output.exposure.committedStops)};
    return output;
}

} // namespace ch31::auto_exposure::gpu
