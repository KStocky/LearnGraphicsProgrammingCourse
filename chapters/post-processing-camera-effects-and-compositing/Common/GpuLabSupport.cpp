#include "GpuLabSupport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <utility>

namespace ch30::post_processing::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorCount = 5U;
enum DescriptorIndex : UINT
{
    RecordsUav = 0U,
    SceneUav = 1U,
    ChainUav = 2U,
    BloomUav = 3U,
    RecordsSrv = 4U,
};
enum ComputeRootParameter : UINT
{
    ComputeConstants = 0U,
    ComputeResources = 1U,
};
enum GraphicsRootParameter : UINT
{
    GraphicsConstants = 0U,
    GraphicsRecords = 1U,
};

enum PassKind : std::uint32_t
{
    PassScene = 0U,
    PassDepthOfField = 1U,
    PassMotionBlur = 2U,
    PassBloomExtract = 3U,
    PassBloomDownsample = 4U,
    PassBloomUpsample = 5U,
    PassCompose = 6U,
};

inline constexpr std::uint32_t kFlagShutterMidpoint = 1U << 0U;
inline constexpr std::uint32_t kFlagClampShutterToBudget = 1U << 1U;
inline constexpr std::uint32_t kFlagBloomEnabled = 1U << 2U;
inline constexpr std::uint32_t kFlagDepthOfFieldEnabled = 1U << 3U;
inline constexpr std::uint32_t kFlagMotionBlurEnabled = 1U << 4U;
inline constexpr std::uint32_t kFlagUiEnabled = 1U << 5U;
inline constexpr std::uint32_t kFlagUiBeforeEncode = 1U << 6U;
inline constexpr std::uint32_t kFlagCreativeCrossfade = 1U << 7U;
inline constexpr std::uint32_t kFlagBloomTopLevel = 1U << 8U;

// Mirrors the PostConstants cbuffer in PostProcessingShared.hlsli field for field.
struct PostConstants final
{
    std::uint32_t displayWidth{};
    std::uint32_t displayHeight{};
    std::uint32_t sourceOffset{};
    std::uint32_t sourceWidth{};
    std::uint32_t sourceHeight{};
    std::uint32_t destinationOffset{};
    std::uint32_t destinationWidth{};
    std::uint32_t destinationHeight{};
    std::uint32_t passKind{};
    std::uint32_t flags{};
    std::uint32_t animationFrame{};
    std::uint32_t sceneVariant{};
    std::uint32_t apertureSampleCount{};
    std::uint32_t motionSampleCount{};
    std::uint32_t bloomLevelCount{};
    std::uint32_t bloomLevelOffset{};
    std::uint32_t stageOrderWord{};
    std::uint32_t stageCount{};
    std::uint32_t displayTransferFunction{};
    float preExposure{};
    float exposureScale{};
    float exposureTimeFraction{};
    float nearPlaneMetres{};
    float farPlaneMetres{};
    float bloomThresholdLuminance{};
    float bloomSoftKneeLuminance{};
    float bloomLevelWeight{};
    float bloomGain{};
    float bloomIntensity{};
    float focalLengthMillimetres{};
    float fNumber{};
    float focusDistanceMetres{};
    float sensorHeightMillimetres{};
    float maximumCocRadiusPixels{};
    float inFocusRadiusPixels{};
    float nearFieldSearchRadiusPixels{};
    float minimumFarCoverage{};
    float dofDepthCompareAbsoluteMetres{};
    float dofDepthCompareRelative{};
    float maximumShutterDisplacementPixels{};
    float motionDepthCompareAbsoluteMetres{};
    float motionDepthCompareRelative{};
    float uiAlpha{};
    float uiColorR{};
    float uiColorG{};
    float uiColorB{};
    float constantRadianceR{};
    float constantRadianceG{};
    float constantRadianceB{};
    float constantViewDepthMetres{};
    float constantMotionU{};
    float constantMotionV{};
    float splitNearDepthMetres{};
    float splitRadianceScale{};
    float splitMotionScale{};
};
static_assert(sizeof(PostConstants) == 220U);
static_assert(sizeof(PostConstants) % sizeof(std::uint32_t) == 0U);

struct DisplayConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t debugView{};
    std::uint32_t expectedStatus{};
    float maximumCocRadiusPixels{};
    float farPlaneMetres{};
};
static_assert(sizeof(DisplayConstants) == 24U);

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
    return std::string{what} + " reported ch30::post_processing::ContractError code " +
           std::to_string(static_cast<unsigned>(error)) + ".";
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
            lgp::framework::MakeError("ValidateExtent", "Chapter 30 requires a non-empty extent up to 320x192."));
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
    return left == right || std::abs(left - right) <= 1.0e-7F;
}

[[nodiscard]] std::uint32_t GroupCount(std::uint32_t extent) noexcept
{
    return (extent + 7U) / 8U;
}

} // namespace

lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration, LabVariant variant)
{
    if (static_cast<std::uint32_t>(configuration.debugView) >= kDebugViewCount ||
        static_cast<std::uint32_t>(configuration.sceneVariant) > static_cast<std::uint32_t>(SceneVariant::SplitField) ||
        static_cast<std::uint8_t>(configuration.cameraEffectOrder) >
            static_cast<std::uint8_t>(CameraEffectOrder::ShutterThenDefocus) ||
        static_cast<std::uint8_t>(configuration.bloomSource) >
            static_cast<std::uint8_t>(BloomSourcePlacement::BeforeCameraEffects) ||
        static_cast<std::uint8_t>(configuration.uiPlacement) >
            static_cast<std::uint8_t>(UiCompositePlacement::BeforeDisplayEncodeLinear) ||
        static_cast<std::uint8_t>(configuration.compositionMode) >
            static_cast<std::uint8_t>(BloomCompositionMode::CreativeCrossfade) ||
        static_cast<std::uint8_t>(configuration.transferFunction) >
            static_cast<std::uint8_t>(TransferFunction::Gamma22) ||
        static_cast<std::uint8_t>(configuration.shutterSchedule) >
            static_cast<std::uint8_t>(ShutterSampleSchedule::Midpoint))
    {
        return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration", "Unknown Chapter 30 enumerator."));
    }
    if (configuration.bloomMaximumLevelCount == 0U ||
        configuration.bloomMaximumLevelCount > kMaximumLabBloomLevelCount ||
        configuration.bloomMinimumLevelExtent == 0U)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateLabConfiguration", "Bloom level count must be in [1,6] with a non-zero minimum extent."));
    }
    if (configuration.motionBlurSampleCount == 0U ||
        configuration.motionBlurSampleCount > kMaximumLabMotionBlurSampleCount ||
        configuration.apertureSampleCount == 0U || configuration.apertureSampleCount > kMaximumLabApertureSampleCount)
    {
        return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration",
                                                         "Shutter and aperture sample counts must be in [1,32]."));
    }
    if (!std::isfinite(configuration.bloomLevelWeightFalloff) || configuration.bloomLevelWeightFalloff <= 0.0F ||
        configuration.bloomLevelWeightFalloff > 1.0F)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Bloom level weight falloff must be in (0,1]."));
    }
    for (float value : {configuration.constantRadianceR, configuration.constantRadianceG,
                        configuration.constantRadianceB, configuration.uiColorR, configuration.uiColorG,
                        configuration.uiColorB, configuration.splitRadianceScale, configuration.splitMotionScale})
    {
        if (!std::isfinite(value) || value < 0.0F)
        {
            return std::unexpected(lgp::framework::MakeError(
                "ValidateLabConfiguration", "Scene, UI, and split radiance values must be finite and non-negative."));
        }
    }
    if (!std::isfinite(configuration.uiAlpha) || configuration.uiAlpha < 0.0F || configuration.uiAlpha > 1.0F)
    {
        return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration", "UI alpha must be in [0,1]."));
    }
    if (configuration.uiColorR > 1.0F || configuration.uiColorG > 1.0F || configuration.uiColorB > 1.0F)
    {
        // The UI is composited in a display-referred domain, so its colour has to live in the unit range wherever
        // the policy places it.
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "UI colour must be display-referred in [0,1]."));
    }
    if (!std::isfinite(configuration.splitNearDepthMetres) || configuration.splitNearDepthMetres <= 0.0F ||
        !std::isfinite(configuration.constantViewDepthMetres) || configuration.constantViewDepthMetres <= 0.0F)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Scene view depths must be finite and positive."));
    }
    if (!std::isfinite(configuration.constantMotionU) || !std::isfinite(configuration.constantMotionV))
    {
        return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration", "Scene motion must be finite."));
    }

    // Everything below is validated by the CPU reference rather than restated here, so the lab cannot drift away
    // from the contracts it teaches.
    CameraPostFrame const frame = MakeCameraPostFrame(configuration, {kMaximumWidth, kMaximumHeight});
    auto const facts = ValidatePostFrame(frame);
    if (!facts)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", ContractMessage("ValidatePostFrame", facts.error())));
    }
    auto const extraction =
        ExtractBloomHighlights({.r = 1.0, .g = 1.0, .b = 1.0}, static_cast<double>(configuration.preExposure),
                               MakeBloomThresholdSettings(configuration));
    if (!extraction)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateLabConfiguration", ContractMessage("ExtractBloomHighlights", extraction.error())));
    }
    DepthOfFieldSettings const dof = MakeDepthOfFieldSettings(configuration);
    auto const coc = ComputeCircleOfConfusion(static_cast<double>(configuration.focusDistanceMetres), dof.camera,
                                              dof.coc, {kMaximumWidth, kMaximumHeight});
    if (!coc)
    {
        return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration",
                                                         ContractMessage("ComputeCircleOfConfusion", coc.error())));
    }
    auto const schedule = BuildShutterSchedule({}, *facts, MakeMotionBlurSettings(configuration));
    if (!schedule)
    {
        return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration",
                                                         ContractMessage("BuildShutterSchedule", schedule.error())));
    }
    auto const composition = ComposeSceneLinear({}, MakeCompositionSettings(configuration));
    if (!composition)
    {
        return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration",
                                                         ContractMessage("ComposeSceneLinear", composition.error())));
    }
    if (!std::isfinite(configuration.bloomGain) || configuration.bloomGain < 0.0F)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "Bloom gain must be finite and non-negative."));
    }
    if (!std::isfinite(configuration.nearFieldSearchRadiusPixels) || configuration.nearFieldSearchRadiusPixels < 0.0F ||
        configuration.nearFieldSearchRadiusPixels > static_cast<float>(kMaximumWidth))
    {
        return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration",
                                                         "Near-field search radius must be a bounded pixel count."));
    }

    // The gather contracts reject a view depth outside the frame's own range, so the configured planes have to
    // contain the analytic scene instead of the scene being clamped into them.
    double minimumDepth = static_cast<double>(configuration.constantViewDepthMetres);
    double maximumDepth = minimumDepth;
    if (configuration.sceneVariant == SceneVariant::CameraLab)
    {
        minimumDepth = std::min(minimumDepth, kSceneMinimumFixedDepthMetres);
        maximumDepth = std::max(maximumDepth, kSceneMaximumFixedDepthMetres);
    }
    else if (configuration.sceneVariant == SceneVariant::SplitField)
    {
        minimumDepth = std::min(minimumDepth, static_cast<double>(configuration.splitNearDepthMetres));
        maximumDepth = std::max(maximumDepth, static_cast<double>(configuration.splitNearDepthMetres));
    }
    if (static_cast<double>(configuration.nearPlaneMetres) > minimumDepth ||
        static_cast<double>(configuration.farPlaneMetres) < maximumDepth)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateLabConfiguration", "The configured depth range must contain every analytic scene depth."));
    }

    if (variant == LabVariant::Starter)
    {
        if (configuration.depthOfFieldEnabled || configuration.motionBlurEnabled || configuration.bloomEnabled)
        {
            return std::unexpected(lgp::framework::MakeError(
                "ValidateLabConfiguration",
                "The Starter owns no camera effects or bloom pyramid and refuses to pretend otherwise."));
        }
        switch (configuration.debugView)
        {
        case DebugView::Final:
        case DebugView::SceneLinearBaseline:
        case DebugView::ViewDepth:
        case DebugView::Motion:
        case DebugView::Composition:
        case DebugView::Exposed:
        case DebugView::ToneMapped:
        case DebugView::DisplayEncoded:
            break;
        default:
            return std::unexpected(lgp::framework::MakeError(
                "ValidateLabConfiguration", "The Starter exposes only baseline and output-path views."));
        }
    }
    return {};
}

LabConfiguration DefaultConfiguration(LabVariant variant) noexcept
{
    LabConfiguration configuration{};
    if (variant == LabVariant::Starter)
    {
        configuration.depthOfFieldEnabled = false;
        configuration.motionBlurEnabled = false;
        configuration.bloomEnabled = false;
        configuration.debugView = DebugView::Final;
    }
    return configuration;
}

CameraPostFrame MakeCameraPostFrame(LabConfiguration const &configuration,
                                    lgp::framework::Extent2D displayExtent) noexcept
{
    Extent2D const extent{.width = displayExtent.width, .height = displayExtent.height};
    CameraPostFrame frame{};
    // The lab consumes an already resolved image, so the render extent is the display extent and upscaling stays
    // where it belongs, in Chapter 28. Depth and motion are published at that same extent.
    frame.renderExtent = extent;
    frame.displayExtent = extent;
    frame.depthExtent = extent;
    frame.motionExtent = extent;
    // The resolve this lab consumes is converged over the jitter sequence, so no residual jitter is carried in.
    frame.currentJitterPixels = {};
    frame.previousJitterPixels = {};
    frame.preExposure = static_cast<double>(configuration.preExposure);
    frame.previousPreExposure = static_cast<double>(configuration.preExposure);
    frame.exposureScale = static_cast<double>(configuration.exposureScale);
    frame.frameDeltaSeconds = static_cast<double>(configuration.frameDeltaSeconds);
    frame.shutterOpenSeconds = static_cast<double>(configuration.shutterOpenSeconds);
    frame.nearPlaneMetres = static_cast<double>(configuration.nearPlaneMetres);
    frame.farPlaneMetres = static_cast<double>(configuration.farPlaneMetres);
    frame.hasHistory = true;
    frame.cameraCut = false;
    return frame;
}

PipelinePolicy MakePipelinePolicy(LabConfiguration const &configuration) noexcept
{
    PipelinePolicy policy{};
    policy.cameraEffectOrder = configuration.cameraEffectOrder;
    policy.bloomSource = configuration.bloomSource;
    policy.uiPlacement = configuration.uiPlacement;
    policy.requireTemporalResolve = true;
    policy.requireUiComposite = configuration.uiEnabled;
    return policy;
}

BloomThresholdSettings MakeBloomThresholdSettings(LabConfiguration const &configuration) noexcept
{
    return {.thresholdLuminance = static_cast<double>(configuration.bloomThresholdLuminance),
            .softKneeLuminance = static_cast<double>(configuration.bloomSoftKneeLuminance)};
}

BloomPyramidSettings MakeBloomPyramidSettings(LabConfiguration const &configuration) noexcept
{
    return {.maximumLevelCount = configuration.bloomMaximumLevelCount,
            .minimumLevelExtent = configuration.bloomMinimumLevelExtent};
}

BloomCombineSettings MakeBloomCombineSettings(LabConfiguration const &configuration, std::uint32_t levelCount) noexcept
{
    BloomCombineSettings settings{};
    double weight = 1.0;
    for (std::uint32_t level = 0U; level < levelCount && level < kMaximumBloomLevelCount; ++level)
    {
        settings.levelWeights[level] = weight;
        weight *= static_cast<double>(configuration.bloomLevelWeightFalloff);
    }
    settings.normalizeLevelWeights = configuration.normalizeBloomLevelWeights;
    settings.bloomGain = static_cast<double>(configuration.bloomGain);
    return settings;
}

MotionBlurSettings MakeMotionBlurSettings(LabConfiguration const &configuration) noexcept
{
    return {
        .sampleCount = configuration.motionBlurSampleCount,
        .schedule = configuration.shutterSchedule,
        .maximumDisplacementPixels = static_cast<double>(configuration.maximumShutterDisplacementPixels),
        .clampDisplacementToBudget = configuration.clampShutterToBudget,
        .depthCompareAbsoluteMetres = static_cast<double>(configuration.motionDepthCompareAbsoluteMetres),
        .depthCompareRelative = static_cast<double>(configuration.motionDepthCompareRelative),
    };
}

DepthOfFieldSettings MakeDepthOfFieldSettings(LabConfiguration const &configuration) noexcept
{
    DepthOfFieldSettings settings{};
    settings.camera = {
        .focalLengthMillimetres = static_cast<double>(configuration.focalLengthMillimetres),
        .fNumber = static_cast<double>(configuration.fNumber),
        .focusDistanceMetres = static_cast<double>(configuration.focusDistanceMetres),
        .sensorHeightMillimetres = static_cast<double>(configuration.sensorHeightMillimetres),
    };
    settings.coc = {
        .maximumRadiusPixels = static_cast<double>(configuration.maximumCocRadiusPixels),
        .inFocusRadiusPixels = static_cast<double>(configuration.inFocusRadiusPixels),
    };
    settings.nearFieldSearchRadiusPixels = static_cast<double>(configuration.nearFieldSearchRadiusPixels);
    settings.minimumFarCoverage = static_cast<double>(configuration.minimumFarCoverage);
    settings.depthCompareAbsoluteMetres = static_cast<double>(configuration.dofDepthCompareAbsoluteMetres);
    settings.depthCompareRelative = static_cast<double>(configuration.dofDepthCompareRelative);
    return settings;
}

CompositionSettings MakeCompositionSettings(LabConfiguration const &configuration) noexcept
{
    return {.mode = configuration.compositionMode, .bloomIntensity = static_cast<double>(configuration.bloomIntensity)};
}

std::uint32_t BuildExecutedStages(LabConfiguration const &configuration, LabVariant variant,
                                  std::span<PostStage> stages) noexcept
{
    bool const solution = variant == LabVariant::Solution;
    bool const depthOfField = solution && configuration.depthOfFieldEnabled;
    bool const motionBlur = solution && configuration.motionBlurEnabled;
    bool const bloom = solution && configuration.bloomEnabled;

    std::uint32_t count = 0U;
    auto const append = [&stages, &count](PostStage stage)
    {
        if (count < stages.size())
        {
            stages[count] = stage;
        }
        ++count;
    };

    append(PostStage::TemporalResolve);
    if (bloom && configuration.bloomSource == BloomSourcePlacement::BeforeCameraEffects)
    {
        append(PostStage::Bloom);
    }
    if (configuration.cameraEffectOrder == CameraEffectOrder::DefocusThenShutter)
    {
        if (depthOfField)
        {
            append(PostStage::DepthOfField);
        }
        if (motionBlur)
        {
            append(PostStage::MotionBlur);
        }
    }
    else
    {
        if (motionBlur)
        {
            append(PostStage::MotionBlur);
        }
        if (depthOfField)
        {
            append(PostStage::DepthOfField);
        }
    }
    if (bloom && configuration.bloomSource == BloomSourcePlacement::AfterCameraEffects)
    {
        append(PostStage::Bloom);
    }
    append(PostStage::Exposure);
    append(PostStage::ToneMap);
    if (configuration.uiEnabled && configuration.uiPlacement == UiCompositePlacement::BeforeDisplayEncodeLinear)
    {
        append(PostStage::UiComposite);
    }
    append(PostStage::DisplayEncode);
    if (configuration.uiEnabled && configuration.uiPlacement == UiCompositePlacement::AfterDisplayEncode)
    {
        append(PostStage::UiComposite);
    }
    return count;
}

std::uint32_t EncodeStageOrder(std::span<PostStage const> stages) noexcept
{
    std::uint32_t word = 0U;
    std::uint32_t const encoded = static_cast<std::uint32_t>(std::min<std::size_t>(stages.size(), 8U));
    for (std::uint32_t index = 0U; index < encoded; ++index)
    {
        std::uint32_t const nibble = static_cast<std::uint32_t>(stages[index]) + 1U;
        word |= (nibble & 0xFU) << (4U * index);
    }
    return word;
}

std::uint32_t ExpectedStatus(LabConfiguration const &configuration, LabVariant variant) noexcept
{
    std::uint32_t status = kStatusScene | kStatusComposition | kStatusExposure | kStatusToneMap | kStatusDisplayEncode;
    if (configuration.uiEnabled)
    {
        status |= kStatusUi;
    }
    if (variant == LabVariant::Solution)
    {
        if (configuration.depthOfFieldEnabled)
        {
            status |= kStatusDepthOfField;
        }
        if (configuration.motionBlurEnabled)
        {
            status |= kStatusMotionBlur;
        }
        if (configuration.bloomEnabled)
        {
            status |= kStatusBloom;
        }
    }
    return status;
}

std::expected<BloomLayout, lgp::framework::Error> BuildBloomLayout(LabConfiguration const &configuration,
                                                                   lgp::framework::Extent2D displayExtent)
{
    Extent2D const base{.width = displayExtent.width, .height = displayExtent.height};
    auto const pyramid = BuildBloomPyramid(base, MakeBloomPyramidSettings(configuration));
    if (!pyramid)
    {
        return std::unexpected(
            lgp::framework::MakeError("BuildBloomLayout", ContractMessage("BuildBloomPyramid", pyramid.error())));
    }
    if (pyramid->levelCount > kMaximumLabBloomLevelCount)
    {
        return std::unexpected(
            lgp::framework::MakeError("BuildBloomLayout", "Chapter 30 caps the bloom pyramid at six levels."));
    }

    BloomCombineSettings const combine = MakeBloomCombineSettings(configuration, pyramid->levelCount);
    double requestedWeightSum = 0.0;
    for (std::uint32_t level = 0U; level < pyramid->levelCount; ++level)
    {
        requestedWeightSum += combine.levelWeights[level];
    }
    if (!(requestedWeightSum > 0.0))
    {
        return std::unexpected(
            lgp::framework::MakeError("BuildBloomLayout", "Bloom level weights must sum to a positive value."));
    }

    BloomLayout layout{};
    layout.pyramid = *pyramid;
    layout.requestedWeightSum = requestedWeightSum;
    std::uint32_t offset = 0U;
    for (std::uint32_t level = 0U; level < pyramid->levelCount; ++level)
    {
        auto const count = PixelCount(pyramid->levelExtents[level]);
        if (!count)
        {
            return std::unexpected(
                lgp::framework::MakeError("BuildBloomLayout", ContractMessage("PixelCount", count.error())));
        }
        double const weight = combine.normalizeLevelWeights ? combine.levelWeights[level] / requestedWeightSum
                                                            : combine.levelWeights[level];
        layout.levels[level] = {.extent = pyramid->levelExtents[level],
                                .pyramidOffset = offset,
                                .accumulatorOffset = 0U,
                                .appliedWeight = weight};
        layout.appliedWeightSum += weight;
        offset += *count;
    }
    layout.pyramidPixelCount = offset;
    for (std::uint32_t level = 0U; level < pyramid->levelCount; ++level)
    {
        layout.levels[level].accumulatorOffset = layout.pyramidPixelCount + layout.levels[level].pyramidOffset;
    }
    return layout;
}

InvalidationReason ComputeInvalidation(LabConfiguration const &previous, LabConfiguration const &current,
                                       bool firstFrame, bool extentChanged, bool explicitReset) noexcept
{
    InvalidationReason reasons = InvalidationReason::None;
    if (firstFrame)
    {
        reasons |= InvalidationReason::FirstFrame;
    }
    if (explicitReset || current.resetChain)
    {
        reasons |= InvalidationReason::ExplicitReset;
    }
    if (extentChanged)
    {
        reasons |= InvalidationReason::ExtentChanged;
        reasons |= InvalidationReason::BloomPyramidChanged;
        reasons |= InvalidationReason::FrameFactsChanged;
    }
    if (previous.cameraEffectOrder != current.cameraEffectOrder || previous.bloomSource != current.bloomSource ||
        previous.uiPlacement != current.uiPlacement || previous.uiEnabled != current.uiEnabled ||
        previous.depthOfFieldEnabled != current.depthOfFieldEnabled ||
        previous.motionBlurEnabled != current.motionBlurEnabled || previous.bloomEnabled != current.bloomEnabled)
    {
        reasons |= InvalidationReason::PipelinePolicyChanged;
    }
    if (previous.bloomMaximumLevelCount != current.bloomMaximumLevelCount ||
        previous.bloomMinimumLevelExtent != current.bloomMinimumLevelExtent)
    {
        reasons |= InvalidationReason::BloomPyramidChanged;
    }
    if (!SameFloat(previous.bloomThresholdLuminance, current.bloomThresholdLuminance) ||
        !SameFloat(previous.bloomSoftKneeLuminance, current.bloomSoftKneeLuminance) ||
        !SameFloat(previous.bloomLevelWeightFalloff, current.bloomLevelWeightFalloff) ||
        !SameFloat(previous.bloomGain, current.bloomGain) ||
        !SameFloat(previous.bloomIntensity, current.bloomIntensity) ||
        previous.normalizeBloomLevelWeights != current.normalizeBloomLevelWeights ||
        previous.compositionMode != current.compositionMode)
    {
        reasons |= InvalidationReason::BloomSettingsChanged;
    }
    if (previous.apertureSampleCount != current.apertureSampleCount ||
        !SameFloat(previous.focalLengthMillimetres, current.focalLengthMillimetres) ||
        !SameFloat(previous.fNumber, current.fNumber) ||
        !SameFloat(previous.focusDistanceMetres, current.focusDistanceMetres) ||
        !SameFloat(previous.sensorHeightMillimetres, current.sensorHeightMillimetres) ||
        !SameFloat(previous.maximumCocRadiusPixels, current.maximumCocRadiusPixels) ||
        !SameFloat(previous.inFocusRadiusPixels, current.inFocusRadiusPixels) ||
        !SameFloat(previous.nearFieldSearchRadiusPixels, current.nearFieldSearchRadiusPixels) ||
        !SameFloat(previous.minimumFarCoverage, current.minimumFarCoverage) ||
        !SameFloat(previous.dofDepthCompareAbsoluteMetres, current.dofDepthCompareAbsoluteMetres) ||
        !SameFloat(previous.dofDepthCompareRelative, current.dofDepthCompareRelative))
    {
        reasons |= InvalidationReason::ApertureSamplesChanged;
    }
    if (previous.motionBlurSampleCount != current.motionBlurSampleCount ||
        previous.shutterSchedule != current.shutterSchedule ||
        previous.clampShutterToBudget != current.clampShutterToBudget ||
        !SameFloat(previous.maximumShutterDisplacementPixels, current.maximumShutterDisplacementPixels) ||
        !SameFloat(previous.motionDepthCompareAbsoluteMetres, current.motionDepthCompareAbsoluteMetres) ||
        !SameFloat(previous.motionDepthCompareRelative, current.motionDepthCompareRelative))
    {
        reasons |= InvalidationReason::ShutterScheduleChanged;
    }
    if (previous.sceneVariant != current.sceneVariant || previous.animationFrame != current.animationFrame ||
        !SameFloat(previous.preExposure, current.preExposure) ||
        !SameFloat(previous.constantRadianceR, current.constantRadianceR) ||
        !SameFloat(previous.constantRadianceG, current.constantRadianceG) ||
        !SameFloat(previous.constantRadianceB, current.constantRadianceB) ||
        !SameFloat(previous.constantViewDepthMetres, current.constantViewDepthMetres) ||
        !SameFloat(previous.constantMotionU, current.constantMotionU) ||
        !SameFloat(previous.constantMotionV, current.constantMotionV) ||
        !SameFloat(previous.splitNearDepthMetres, current.splitNearDepthMetres) ||
        !SameFloat(previous.splitRadianceScale, current.splitRadianceScale) ||
        !SameFloat(previous.splitMotionScale, current.splitMotionScale))
    {
        reasons |= InvalidationReason::SceneChanged;
    }
    if (!SameFloat(previous.preExposure, current.preExposure) ||
        !SameFloat(previous.exposureScale, current.exposureScale) ||
        !SameFloat(previous.frameDeltaSeconds, current.frameDeltaSeconds) ||
        !SameFloat(previous.shutterOpenSeconds, current.shutterOpenSeconds) ||
        !SameFloat(previous.nearPlaneMetres, current.nearPlaneMetres) ||
        !SameFloat(previous.farPlaneMetres, current.farPlaneMetres))
    {
        reasons |= InvalidationReason::FrameFactsChanged;
    }
    if (previous.transferFunction != current.transferFunction || !SameFloat(previous.uiAlpha, current.uiAlpha) ||
        !SameFloat(previous.uiColorR, current.uiColorR) || !SameFloat(previous.uiColorG, current.uiColorG) ||
        !SameFloat(previous.uiColorB, current.uiColorB))
    {
        reasons |= InvalidationReason::DisplayEncodingChanged;
    }
    return reasons;
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
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 30 buffers must be non-empty."));
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
                                                                createResult, "Failed to create Chapter 30 buffer."));
    }
    if (!name.empty())
    {
        std::wstring const ownedName{name};
        if (HRESULT const nameResult = result.resource_->SetName(ownedName.c_str()); FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name Chapter 30 buffer."));
        }
    }
    if (mapPersistently)
    {
        D3D12_RANGE const readRange{0U, static_cast<SIZE_T>(sizeInBytes)};
        void *mapping = nullptr;
        if (HRESULT const mapResult = result.resource_->Map(0U, &readRange, &mapping); FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map Chapter 30 readback."));
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
    if (auto status = CompileShader(compiler, options, L"SceneCS", L"cs_6_0", sceneShader_); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"ComposeCS", L"cs_6_0", composeShader_); !status)
    {
        return status;
    }
    if (variant_ == LabVariant::Solution)
    {
        if (auto status = CompileShader(compiler, options, L"CameraEffectCS", L"cs_6_0", cameraEffectShader_); !status)
        {
            return status;
        }
        if (auto status = CompileShader(compiler, options, L"BloomExtractCS", L"cs_6_0", bloomExtractShader_); !status)
        {
            return status;
        }
        if (auto status = CompileShader(compiler, options, L"BloomDownsampleCS", L"cs_6_0", bloomDownsampleShader_);
            !status)
        {
            return status;
        }
        if (auto status = CompileShader(compiler, options, L"BloomUpsampleCS", L"cs_6_0", bloomUpsampleShader_);
            !status)
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
    computeRange.NumDescriptors = 4U;
    computeRange.BaseShaderRegister = 0U;
    D3D12_ROOT_PARAMETER computeParameters[2]{};
    computeParameters[ComputeConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[ComputeConstants].Constants.ShaderRegister = 0U;
    computeParameters[ComputeConstants].Constants.Num32BitValues = sizeof(PostConstants) / sizeof(std::uint32_t);
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
                                                                "Failed to create Chapter 30 compute root signature."));
    }

    D3D12_DESCRIPTOR_RANGE recordsRange{};
    recordsRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    recordsRange.NumDescriptors = 1U;
    recordsRange.BaseShaderRegister = 0U;
    D3D12_ROOT_PARAMETER graphicsParameters[2]{};
    graphicsParameters[GraphicsConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[GraphicsConstants].Constants.ShaderRegister = 0U;
    graphicsParameters[GraphicsConstants].Constants.Num32BitValues = sizeof(DisplayConstants) / sizeof(std::uint32_t);
    graphicsParameters[GraphicsConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    graphicsParameters[GraphicsRecords].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParameters[GraphicsRecords].DescriptorTable = {1U, &recordsRange};
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
            "ID3D12Device::CreateRootSignature", createResult, "Failed to create Chapter 30 graphics root signature."));
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
                                                                    "Failed to create a Chapter 30 compute PSO."));
        }
        return {};
    };

    if (auto status = createCompute(sceneShader_, scenePipeline_); !status)
    {
        return status;
    }
    if (auto status = createCompute(composeShader_, composePipeline_); !status)
    {
        return status;
    }
    if (variant_ == LabVariant::Solution)
    {
        if (auto status = createCompute(cameraEffectShader_, cameraEffectPipeline_); !status)
        {
            return status;
        }
        if (auto status = createCompute(bloomExtractShader_, bloomExtractPipeline_); !status)
        {
            return status;
        }
        if (auto status = createCompute(bloomDownsampleShader_, bloomDownsamplePipeline_); !status)
        {
            return status;
        }
        if (auto status = createCompute(bloomUpsampleShader_, bloomUpsamplePipeline_); !status)
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
                                                                "Failed to create Chapter 30 graphics PSO."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreateResources(lgp::framework::Extent2D size)
{
    if (auto status = ValidateExtent(size); !status)
    {
        return status;
    }

    // The pyramid footprint is bounded by the deepest, finest chain any legal configuration can ask for, so the
    // buffer never has to be reallocated when a configuration changes the level count.
    auto const capacityPyramid =
        BuildBloomPyramid({.width = size.width, .height = size.height},
                          {.maximumLevelCount = kMaximumLabBloomLevelCount, .minimumLevelExtent = 1U});
    if (!capacityPyramid)
    {
        return std::unexpected(lgp::framework::MakeError(
            "CreateResources", ContractMessage("BuildBloomPyramid", capacityPyramid.error())));
    }
    std::uint32_t capacity = 0U;
    for (std::uint32_t level = 0U; level < capacityPyramid->levelCount; ++level)
    {
        auto const count = PixelCount(capacityPyramid->levelExtents[level]);
        if (!count)
        {
            return std::unexpected(
                lgp::framework::MakeError("CreateResources", ContractMessage("PixelCount", count.error())));
        }
        capacity += *count;
    }
    bloomCapacityPixels_ = capacity;

    std::uint64_t const pixelCount = static_cast<std::uint64_t>(size.width) * size.height;
    std::uint64_t const recordBytes = pixelCount * sizeof(PixelRecord);
    std::uint64_t const sceneBytes = pixelCount * sizeof(SceneRecord);
    std::uint64_t const chainBytes = 2U * pixelCount * (4U * sizeof(float));
    std::uint64_t const bloomBytes = 2U * static_cast<std::uint64_t>(capacity) * (4U * sizeof(float));

    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlotResources &slot : frameSlots_)
    {
        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorCount);
        if (!descriptors)
        {
            return std::unexpected(std::move(descriptors.error()));
        }
        auto records = CreateBuffer(*deviceResources_->device(), recordBytes, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch30 pixel records");
        if (!records)
        {
            return std::unexpected(std::move(records.error()));
        }
        auto scene = CreateBuffer(*deviceResources_->device(), sceneBytes, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch30 scene records");
        if (!scene)
        {
            return std::unexpected(std::move(scene.error()));
        }
        auto chain = CreateBuffer(*deviceResources_->device(), chainBytes, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch30 scene-linear chain");
        if (!chain)
        {
            return std::unexpected(std::move(chain.error()));
        }
        auto bloom = CreateBuffer(*deviceResources_->device(), bloomBytes, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch30 bloom pyramid");
        if (!bloom)
        {
            return std::unexpected(std::move(bloom.error()));
        }
        auto readback = CreateBuffer(*deviceResources_->device(), recordBytes, D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_FLAG_NONE, L"Ch30 record readback", true);
        if (!readback)
        {
            return std::unexpected(std::move(readback.error()));
        }

        slot.descriptors = *descriptors;
        slot.records = std::move(*records);
        slot.scene = std::move(*scene);
        slot.chain = std::move(*chain);
        slot.bloom = std::move(*bloom);
        slot.readback = std::move(*readback);
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
        createStructuredUav(RecordsUav, slot.records.Get(), pixelCount, sizeof(PixelRecord));
        createStructuredUav(SceneUav, slot.scene.Get(), pixelCount, sizeof(SceneRecord));
        createStructuredUav(ChainUav, slot.chain.Get(), 2U * pixelCount, 4U * sizeof(float));
        createStructuredUav(BloomUav, slot.bloom.Get(), 2U * static_cast<std::uint64_t>(capacity), 4U * sizeof(float));

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = static_cast<UINT>(pixelCount);
        srv.Buffer.StructureByteStride = sizeof(PixelRecord);
        deviceResources_->device()->CreateShaderResourceView(slot.records.Get(), &srv,
                                                             slot.descriptors.CpuHandle(RecordsSrv));
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
    if (context.input.WasKeyPressed('O'))
    {
        interactiveConfiguration_.cameraEffectOrder =
            interactiveConfiguration_.cameraEffectOrder == CameraEffectOrder::DefocusThenShutter
                ? CameraEffectOrder::ShutterThenDefocus
                : CameraEffectOrder::DefocusThenShutter;
    }
    if (context.input.WasKeyPressed('P'))
    {
        interactiveConfiguration_.bloomSource =
            interactiveConfiguration_.bloomSource == BloomSourcePlacement::AfterCameraEffects
                ? BloomSourcePlacement::BeforeCameraEffects
                : BloomSourcePlacement::AfterCameraEffects;
    }
    if (context.input.WasKeyPressed('I'))
    {
        interactiveConfiguration_.uiPlacement =
            interactiveConfiguration_.uiPlacement == UiCompositePlacement::AfterDisplayEncode
                ? UiCompositePlacement::BeforeDisplayEncodeLinear
                : UiCompositePlacement::AfterDisplayEncode;
    }
    if (context.input.WasKeyPressed('U'))
    {
        interactiveConfiguration_.uiEnabled = !interactiveConfiguration_.uiEnabled;
    }
    if (variant_ == LabVariant::Solution)
    {
        if (context.input.WasKeyPressed('D'))
        {
            interactiveConfiguration_.depthOfFieldEnabled = !interactiveConfiguration_.depthOfFieldEnabled;
        }
        if (context.input.WasKeyPressed('M'))
        {
            interactiveConfiguration_.motionBlurEnabled = !interactiveConfiguration_.motionBlurEnabled;
        }
        if (context.input.WasKeyPressed('B'))
        {
            interactiveConfiguration_.bloomEnabled = !interactiveConfiguration_.bloomEnabled;
        }
    }
    ++interactiveConfiguration_.animationFrame;
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
        return std::unexpected(lgp::framework::MakeError("Render", "Chapter 30 frame slot is out of range."));
    }
    LabConfiguration const configuration = ActiveConfiguration();
    if (auto status = ValidateLabConfiguration(configuration, variant_); !status)
    {
        return status;
    }

    auto const facts = ValidatePostFrame(MakeCameraPostFrame(configuration, size_));
    if (!facts)
    {
        return std::unexpected(
            lgp::framework::MakeError("Render", ContractMessage("ValidatePostFrame", facts.error())));
    }
    PipelinePolicy const policy = MakePipelinePolicy(configuration);
    std::array<PostStage, kMaximumStageCount> stages{};
    std::uint32_t const stageCount = BuildExecutedStages(configuration, variant_, stages);
    auto const plan = PlanPipeline(std::span<PostStage const>{stages.data(), stageCount}, policy);
    if (!plan)
    {
        return std::unexpected(lgp::framework::MakeError("Render", ContractMessage("PlanPipeline", plan.error())));
    }
    if (!plan->IsLegal())
    {
        // An illegal order is reported, never quietly reordered: the learner has to see which rule broke.
        return std::unexpected(lgp::framework::MakeError(
            "Render", "Chapter 30 refused an illegal stage order with violation bits " +
                          std::to_string(static_cast<std::uint32_t>(plan->violations)) + "."));
    }
    auto const layout = BuildBloomLayout(configuration, size_);
    if (!layout)
    {
        return std::unexpected(lgp::framework::Error{layout.error()});
    }
    if (layout->pyramidPixelCount > bloomCapacityPixels_)
    {
        // The pyramid buffer is sized for the deepest legal chain, so this can only fire if the capacity and the
        // per-configuration layout ever stop agreeing. Reporting it beats writing past the allocation.
        return std::unexpected(lgp::framework::MakeError(
            "Render", "Chapter 30 bloom pyramid exceeds the capacity allocated for this extent."));
    }

    InvalidationReason const invalidation = ComputeInvalidation(
        lastRenderedConfiguration_, configuration, !hasRendered_, lastRenderedExtent_ != size_, forceReset_);
    std::uint32_t const stageOrderWord = EncodeStageOrder(std::span<PostStage const>{stages.data(), stageCount});
    std::uint32_t const expectedStatus = ExpectedStatus(configuration, variant_);

    std::uint32_t flags = 0U;
    flags |= configuration.shutterSchedule == ShutterSampleSchedule::Midpoint ? kFlagShutterMidpoint : 0U;
    flags |= configuration.clampShutterToBudget ? kFlagClampShutterToBudget : 0U;
    flags |= configuration.bloomEnabled && variant_ == LabVariant::Solution ? kFlagBloomEnabled : 0U;
    flags |= configuration.depthOfFieldEnabled && variant_ == LabVariant::Solution ? kFlagDepthOfFieldEnabled : 0U;
    flags |= configuration.motionBlurEnabled && variant_ == LabVariant::Solution ? kFlagMotionBlurEnabled : 0U;
    flags |= configuration.uiEnabled ? kFlagUiEnabled : 0U;
    flags |= configuration.uiPlacement == UiCompositePlacement::BeforeDisplayEncodeLinear ? kFlagUiBeforeEncode : 0U;
    flags |= configuration.compositionMode == BloomCompositionMode::CreativeCrossfade ? kFlagCreativeCrossfade : 0U;

    PostConstants constants{};
    constants.displayWidth = size_.width;
    constants.displayHeight = size_.height;
    constants.sourceWidth = size_.width;
    constants.sourceHeight = size_.height;
    constants.destinationWidth = size_.width;
    constants.destinationHeight = size_.height;
    constants.flags = flags;
    constants.animationFrame = configuration.animationFrame;
    constants.sceneVariant = static_cast<std::uint32_t>(configuration.sceneVariant);
    constants.apertureSampleCount = configuration.apertureSampleCount;
    constants.motionSampleCount = configuration.motionBlurSampleCount;
    constants.bloomLevelCount = layout->pyramid.levelCount;
    constants.stageOrderWord = stageOrderWord;
    constants.stageCount = stageCount;
    constants.displayTransferFunction = static_cast<std::uint32_t>(configuration.transferFunction);
    constants.preExposure = configuration.preExposure;
    constants.exposureScale = configuration.exposureScale;
    constants.exposureTimeFraction = static_cast<float>(facts->exposureTimeFraction);
    constants.nearPlaneMetres = configuration.nearPlaneMetres;
    constants.farPlaneMetres = configuration.farPlaneMetres;
    constants.bloomThresholdLuminance = configuration.bloomThresholdLuminance;
    constants.bloomSoftKneeLuminance = configuration.bloomSoftKneeLuminance;
    constants.bloomGain = configuration.bloomGain;
    constants.bloomIntensity = configuration.bloomIntensity;
    constants.focalLengthMillimetres = configuration.focalLengthMillimetres;
    constants.fNumber = configuration.fNumber;
    constants.focusDistanceMetres = configuration.focusDistanceMetres;
    constants.sensorHeightMillimetres = configuration.sensorHeightMillimetres;
    constants.maximumCocRadiusPixels = configuration.maximumCocRadiusPixels;
    constants.inFocusRadiusPixels = configuration.inFocusRadiusPixels;
    constants.nearFieldSearchRadiusPixels = configuration.nearFieldSearchRadiusPixels;
    constants.minimumFarCoverage = configuration.minimumFarCoverage;
    constants.dofDepthCompareAbsoluteMetres = configuration.dofDepthCompareAbsoluteMetres;
    constants.dofDepthCompareRelative = configuration.dofDepthCompareRelative;
    constants.maximumShutterDisplacementPixels = configuration.maximumShutterDisplacementPixels;
    constants.motionDepthCompareAbsoluteMetres = configuration.motionDepthCompareAbsoluteMetres;
    constants.motionDepthCompareRelative = configuration.motionDepthCompareRelative;
    constants.uiAlpha = configuration.uiAlpha;
    constants.uiColorR = configuration.uiColorR;
    constants.uiColorG = configuration.uiColorG;
    constants.uiColorB = configuration.uiColorB;
    constants.constantRadianceR = configuration.constantRadianceR;
    constants.constantRadianceG = configuration.constantRadianceG;
    constants.constantRadianceB = configuration.constantRadianceB;
    constants.constantViewDepthMetres = configuration.constantViewDepthMetres;
    constants.constantMotionU = configuration.constantMotionU;
    constants.constantMotionV = configuration.constantMotionV;
    constants.splitNearDepthMetres = configuration.splitNearDepthMetres;
    constants.splitRadianceScale = configuration.splitRadianceScale;
    constants.splitMotionScale = configuration.splitMotionScale;

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    ID3D12GraphicsCommandList7 &list = *frameContext.commandList;
    ID3D12DescriptorHeap *heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    list.SetDescriptorHeaps(1U, heaps);

    std::array<D3D12_BUFFER_BARRIER, 4U> entryBarriers{
        MakeBufferBarrier(*slot.records.Get(), slot.initialized ? CopySourceState() : NoAccessState(),
                          ComputeUavState()),
        MakeBufferBarrier(*slot.scene.Get(), slot.initialized ? ComputeUavState() : NoAccessState(), ComputeUavState()),
        MakeBufferBarrier(*slot.chain.Get(), slot.initialized ? ComputeUavState() : NoAccessState(), ComputeUavState()),
        MakeBufferBarrier(*slot.bloom.Get(), slot.initialized ? ComputeUavState() : NoAccessState(), ComputeUavState()),
    };
    SubmitBufferBarriers(list, entryBarriers);

    list.SetComputeRootSignature(computeRootSignature_.Get());
    list.SetComputeRootDescriptorTable(ComputeResources, slot.descriptors.GpuHandle(RecordsUav));

    // Every dispatch writes buffers the next one reads, so the chain is ordered by an unordered-access barrier
    // between each pair rather than by hope.
    auto const orderDispatches = [&list, &slot]()
    {
        std::array<D3D12_BUFFER_BARRIER, 4U> barriers{
            MakeBufferBarrier(*slot.records.Get(), ComputeUavState(), ComputeUavState()),
            MakeBufferBarrier(*slot.scene.Get(), ComputeUavState(), ComputeUavState()),
            MakeBufferBarrier(*slot.chain.Get(), ComputeUavState(), ComputeUavState()),
            MakeBufferBarrier(*slot.bloom.Get(), ComputeUavState(), ComputeUavState()),
        };
        SubmitBufferBarriers(list, barriers);
    };
    auto const dispatch =
        [&list, &constants, &orderDispatches](ID3D12PipelineState *pipeline, std::uint32_t width, std::uint32_t height)
    {
        list.SetComputeRoot32BitConstants(ComputeConstants, sizeof(PostConstants) / sizeof(std::uint32_t), &constants,
                                          0U);
        list.SetPipelineState(pipeline);
        list.Dispatch(GroupCount(width), GroupCount(height), 1U);
        orderDispatches();
    };

    std::uint32_t const displayPixels = size_.width * size_.height;
    constants.passKind = PassScene;
    constants.sourceOffset = 0U;
    constants.destinationOffset = 0U;
    dispatch(scenePipeline_.Get(), size_.width, size_.height);

    std::uint32_t chainOffset = 0U;
    for (std::uint32_t index = 0U; index < stageCount; ++index)
    {
        switch (stages[index])
        {
        case PostStage::DepthOfField:
        case PostStage::MotionBlur:
        {
            constants.passKind = stages[index] == PostStage::DepthOfField ? PassDepthOfField : PassMotionBlur;
            constants.sourceOffset = chainOffset;
            constants.destinationOffset = displayPixels - chainOffset;
            constants.sourceWidth = size_.width;
            constants.sourceHeight = size_.height;
            constants.destinationWidth = size_.width;
            constants.destinationHeight = size_.height;
            dispatch(cameraEffectPipeline_.Get(), size_.width, size_.height);
            chainOffset = constants.destinationOffset;
            break;
        }
        case PostStage::Bloom:
        {
            constants.passKind = PassBloomExtract;
            constants.sourceOffset = chainOffset;
            constants.sourceWidth = size_.width;
            constants.sourceHeight = size_.height;
            constants.destinationOffset = layout->levels[0].pyramidOffset;
            constants.destinationWidth = size_.width;
            constants.destinationHeight = size_.height;
            constants.bloomLevelOffset = layout->levels[0].pyramidOffset;
            constants.bloomLevelWeight = static_cast<float>(layout->levels[0].appliedWeight);
            dispatch(bloomExtractPipeline_.Get(), size_.width, size_.height);

            constants.passKind = PassBloomDownsample;
            for (std::uint32_t level = 1U; level < layout->pyramid.levelCount; ++level)
            {
                BloomLevelLayout const &source = layout->levels[level - 1U];
                BloomLevelLayout const &destination = layout->levels[level];
                constants.sourceOffset = source.pyramidOffset;
                constants.sourceWidth = source.extent.width;
                constants.sourceHeight = source.extent.height;
                constants.destinationOffset = destination.pyramidOffset;
                constants.destinationWidth = destination.extent.width;
                constants.destinationHeight = destination.extent.height;
                dispatch(bloomDownsamplePipeline_.Get(), destination.extent.width, destination.extent.height);
            }

            constants.passKind = PassBloomUpsample;
            std::uint32_t const topLevel = layout->pyramid.levelCount - 1U;
            constants.flags = flags | kFlagBloomTopLevel;
            constants.sourceOffset = layout->levels[topLevel].accumulatorOffset;
            constants.sourceWidth = layout->levels[topLevel].extent.width;
            constants.sourceHeight = layout->levels[topLevel].extent.height;
            constants.destinationOffset = layout->levels[topLevel].accumulatorOffset;
            constants.destinationWidth = layout->levels[topLevel].extent.width;
            constants.destinationHeight = layout->levels[topLevel].extent.height;
            constants.bloomLevelOffset = layout->levels[topLevel].pyramidOffset;
            constants.bloomLevelWeight = static_cast<float>(layout->levels[topLevel].appliedWeight);
            dispatch(bloomUpsamplePipeline_.Get(), layout->levels[topLevel].extent.width,
                     layout->levels[topLevel].extent.height);

            constants.flags = flags;
            for (std::uint32_t level = topLevel; level > 0U; --level)
            {
                BloomLevelLayout const &source = layout->levels[level];
                BloomLevelLayout const &destination = layout->levels[level - 1U];
                constants.sourceOffset = source.accumulatorOffset;
                constants.sourceWidth = source.extent.width;
                constants.sourceHeight = source.extent.height;
                constants.destinationOffset = destination.accumulatorOffset;
                constants.destinationWidth = destination.extent.width;
                constants.destinationHeight = destination.extent.height;
                constants.bloomLevelOffset = destination.pyramidOffset;
                constants.bloomLevelWeight = static_cast<float>(destination.appliedWeight);
                dispatch(bloomUpsamplePipeline_.Get(), destination.extent.width, destination.extent.height);
            }
            break;
        }
        case PostStage::TemporalResolve:
        case PostStage::Exposure:
        case PostStage::ToneMap:
        case PostStage::DisplayEncode:
        case PostStage::UiComposite:
            // TemporalResolve is the scene pass; the output-path stages are performed, in this order, inside the
            // single compose dispatch below.
            break;
        }
    }

    constants.passKind = PassCompose;
    constants.sourceOffset = chainOffset;
    constants.sourceWidth = size_.width;
    constants.sourceHeight = size_.height;
    constants.destinationOffset = chainOffset;
    constants.destinationWidth = size_.width;
    constants.destinationHeight = size_.height;
    constants.bloomLevelOffset = layout->levels[0].accumulatorOffset;
    dispatch(composePipeline_.Get(), size_.width, size_.height);

    D3D12_BUFFER_BARRIER displayBarrier = MakeBufferBarrier(*slot.records.Get(), ComputeUavState(), PixelSrvState());
    SubmitBufferBarriers(list, std::span{&displayBarrier, 1U});

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
        configuration.maximumCocRadiusPixels,
        configuration.farPlaneMetres,
    };
    list.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    list.SetGraphicsRoot32BitConstants(GraphicsConstants, sizeof(display) / sizeof(std::uint32_t), &display, 0U);
    list.SetGraphicsRootDescriptorTable(GraphicsRecords, slot.descriptors.GpuHandle(RecordsSrv));
    list.SetPipelineState(graphicsPipeline_.Get());
    list.DrawInstanced(3U, 1U, 0U, 0U);

    D3D12_BUFFER_BARRIER copyBarrier = MakeBufferBarrier(*slot.records.Get(), PixelSrvState(), CopySourceState());
    SubmitBufferBarriers(list, std::span{&copyBarrier, 1U});
    list.CopyBufferRegion(slot.readback.Get(), 0U, slot.records.Get(), 0U, slot.records.size_in_bytes());
    SubmitTextureTransition(list, *frameContext.renderTarget, RenderTargetState(), FrameEndState(frameContext));

    slot.initialized = true;
    lastRenderedConfiguration_ = configuration;
    lastRenderedExtent_ = size_;
    lastFrameFacts_ = *facts;
    lastPlan_ = *plan;
    lastBloomLayout_ = *layout;
    lastStages_ = stages;
    lastStageCount_ = stageCount;
    lastStageOrderWord_ = stageOrderWord;
    lastExpectedStatus_ = expectedStatus;
    lastInvalidation_ = invalidation;
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
    bloomCapacityPixels_ = 0U;
    hasRendered_ = false;
    size_ = {};
}

void RendererCore::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    DestroyResources(deviceResources);
    graphicsPipeline_.Reset();
    composePipeline_.Reset();
    bloomUpsamplePipeline_.Reset();
    bloomDownsamplePipeline_.Reset();
    bloomExtractPipeline_.Reset();
    cameraEffectPipeline_.Reset();
    scenePipeline_.Reset();
    graphicsRootSignature_.Reset();
    computeRootSignature_.Reset();
    sceneShader_ = {};
    cameraEffectShader_ = {};
    bloomExtractShader_ = {};
    bloomDownsampleShader_ = {};
    bloomUpsampleShader_ = {};
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

std::expected<FrameReadback, lgp::framework::Error> RendererCore::ReadBackOutputs()
{
    if (deviceResources_ == nullptr || !hasRendered_ || frameSlots_.empty())
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "Chapter 30 has no completed frame to read."));
    }
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }
    FrameReadback output{};
    output.configuration = lastRenderedConfiguration_;
    output.displaySize = size_;
    output.frameFacts = lastFrameFacts_;
    output.plan = lastPlan_;
    output.bloomLayout = lastBloomLayout_;
    output.executedStages = lastStages_;
    output.executedStageCount = lastStageCount_;
    output.stageOrderWord = lastStageOrderWord_;
    output.expectedStatus = lastExpectedStatus_;
    output.invalidation = lastInvalidation_;
    output.frameSlot = lastRenderedFrameSlot_;
    output.pixels.resize(static_cast<std::size_t>(size_.width) * size_.height);
    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    std::memcpy(output.pixels.data(), slot.readback.mapped_data(), output.pixels.size() * sizeof(PixelRecord));
    return output;
}

} // namespace ch30::post_processing::gpu
