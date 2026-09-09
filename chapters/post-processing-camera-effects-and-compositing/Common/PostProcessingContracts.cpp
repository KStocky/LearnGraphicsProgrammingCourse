#include "PostProcessingContracts.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ch30::post_processing
{
namespace
{

constexpr std::uint32_t kInvalidPosition = 0xFFFF'FFFFU;
constexpr double kMaximumMotionUv = 16.0;
constexpr double kMaximumFNumber = 1'024.0;
constexpr double kUnitDiskTolerance = 1.0e-9;

[[nodiscard]] bool IsFinite(double value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool IsFinite(Float2 value) noexcept
{
    return IsFinite(value.x) && IsFinite(value.y);
}

[[nodiscard]] bool IsFinite(Rgb value) noexcept
{
    return IsFinite(value.r) && IsFinite(value.g) && IsFinite(value.b);
}

[[nodiscard]] Rgb Scale(Rgb value, double factor) noexcept
{
    return {.r = value.r * factor, .g = value.g * factor, .b = value.b * factor};
}

[[nodiscard]] Rgb Add(Rgb left, Rgb right) noexcept
{
    return {.r = left.r + right.r, .g = left.g + right.g, .b = left.b + right.b};
}

[[nodiscard]] double Length(Float2 value) noexcept
{
    return std::hypot(value.x, value.y);
}

[[nodiscard]] std::expected<void, ContractError> ValidateExtent(Extent2D extent) noexcept
{
    if (extent.width == 0U || extent.height == 0U)
    {
        return std::unexpected(ContractError::InvalidExtent);
    }
    if (extent.width > kMaximumDimension || extent.height > kMaximumDimension)
    {
        return std::unexpected(ContractError::ExtentTooLarge);
    }
    return {};
}

[[nodiscard]] std::expected<std::uint32_t, ContractError> ValidateReferenceExtent(Extent2D extent) noexcept
{
    auto const extentCheck = ValidateExtent(extent);
    if (!extentCheck)
    {
        return std::unexpected(extentCheck.error());
    }
    if (extent.width > kMaximumReferenceDimension || extent.height > kMaximumReferenceDimension)
    {
        return std::unexpected(ContractError::ExtentTooLarge);
    }
    std::uint64_t const count = static_cast<std::uint64_t>(extent.width) * static_cast<std::uint64_t>(extent.height);
    if (count > static_cast<std::uint64_t>(kMaximumReferencePixelCount))
    {
        return std::unexpected(ContractError::ExtentTooLarge);
    }
    return static_cast<std::uint32_t>(count);
}

[[nodiscard]] std::expected<void, ContractError> ValidateRadiance(Rgb value) noexcept
{
    if (!IsFinite(value))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (value.r < 0.0 || value.g < 0.0 || value.b < 0.0)
    {
        return std::unexpected(ContractError::NegativeRadiance);
    }
    if (value.r > kMaximumSceneLinearValue || value.g > kMaximumSceneLinearValue || value.b > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidRadiance);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateViewDepth(double viewDepthMetres) noexcept
{
    if (!IsFinite(viewDepthMetres))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (viewDepthMetres <= 0.0)
    {
        return std::unexpected(ContractError::NonPositiveViewDepth);
    }
    if (viewDepthMetres > kMaximumViewDepthMetres)
    {
        return std::unexpected(ContractError::InvalidDepth);
    }
    return {};
}

// The depth image is evidence about surfaces the camera actually saw, so every texel must be a linear view depth
// inside the frustum the frame declared. A value beyond the far plane usually means device depth was written here
// by mistake, or that the caller's linearisation used the wrong plane pair.
[[nodiscard]] std::expected<void, ContractError> ValidateDepthImage(ScalarImageView image, Extent2D expectedExtent,
                                                                    double nearPlaneMetres,
                                                                    double farPlaneMetres) noexcept
{
    if (image.extent != expectedExtent)
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    auto const pixelCount = ValidateReferenceExtent(image.extent);
    if (!pixelCount)
    {
        return std::unexpected(pixelCount.error());
    }
    if (image.values.size() != static_cast<std::size_t>(*pixelCount))
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    if (!IsFinite(nearPlaneMetres) || !IsFinite(farPlaneMetres) || nearPlaneMetres <= 0.0 ||
        farPlaneMetres <= nearPlaneMetres)
    {
        return std::unexpected(ContractError::InvalidDepthRange);
    }
    for (double const value : image.values)
    {
        auto const depthCheck = ValidateViewDepth(value);
        if (!depthCheck)
        {
            return std::unexpected(depthCheck.error());
        }
        if (value < nearPlaneMetres || value > farPlaneMetres)
        {
            return std::unexpected(ContractError::InvalidDepthRange);
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateMotionImage(MotionImageView image,
                                                                     Extent2D expectedExtent) noexcept
{
    if (image.extent != expectedExtent)
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    auto const pixelCount = ValidateReferenceExtent(image.extent);
    if (!pixelCount)
    {
        return std::unexpected(pixelCount.error());
    }
    if (image.motionPreviousMinusCurrentUv.size() != static_cast<std::size_t>(*pixelCount))
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    for (Float2 const value : image.motionPreviousMinusCurrentUv)
    {
        if (!IsFinite(value))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (std::abs(value.x) > kMaximumMotionUv || std::abs(value.y) > kMaximumMotionUv)
        {
            return std::unexpected(ContractError::InvalidMotion);
        }
    }
    return {};
}

[[nodiscard]] std::uint32_t TexelIndex(Extent2D extent, PixelCoordinate pixel) noexcept
{
    return (pixel.y * extent.width) + pixel.x;
}

[[nodiscard]] bool IsInsideExtent(Extent2D extent, PixelCoordinate pixel) noexcept
{
    return pixel.x < extent.width && pixel.y < extent.height;
}

// Offsets are signed distances in pixels from the centre of the given pixel. A tap lands in the texel that contains
// the offset position, which keeps the centre sample on the centre texel exactly.
[[nodiscard]] bool OffsetTexel(Extent2D extent, PixelCoordinate center, Float2 offsetPixels,
                               PixelCoordinate &texel) noexcept
{
    double const x = static_cast<double>(center.x) + 0.5 + offsetPixels.x;
    double const y = static_cast<double>(center.y) + 0.5 + offsetPixels.y;
    if (!IsFinite(x) || !IsFinite(y))
    {
        return false;
    }
    if (x < 0.0 || y < 0.0)
    {
        return false;
    }
    double const flooredX = std::floor(x);
    double const flooredY = std::floor(y);
    if (flooredX >= static_cast<double>(extent.width) || flooredY >= static_cast<double>(extent.height))
    {
        return false;
    }
    texel = {.x = static_cast<std::uint32_t>(flooredX), .y = static_cast<std::uint32_t>(flooredY)};
    return true;
}

[[nodiscard]] std::uint32_t CeilingHalf(std::uint32_t value) noexcept
{
    return (value + 1U) / 2U;
}

[[nodiscard]] bool IsCameraEffect(PostStage stage) noexcept
{
    return stage == PostStage::DepthOfField || stage == PostStage::MotionBlur || stage == PostStage::Bloom;
}

[[nodiscard]] bool IsSceneLinearFilter(PostStage stage) noexcept
{
    return stage == PostStage::TemporalResolve || IsCameraEffect(stage);
}

[[nodiscard]] bool IsLinearDomainStage(PostStage stage) noexcept
{
    return IsSceneLinearFilter(stage) || stage == PostStage::Exposure || stage == PostStage::ToneMap;
}

[[nodiscard]] double RadicalInverseBase2(std::uint32_t index) noexcept
{
    std::uint32_t bits = index;
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x5555'5555U) << 1U) | ((bits & 0xAAAA'AAAAU) >> 1U);
    bits = ((bits & 0x3333'3333U) << 2U) | ((bits & 0xCCCC'CCCCU) >> 2U);
    bits = ((bits & 0x0F0F'0F0FU) << 4U) | ((bits & 0xF0F0'F0F0U) >> 4U);
    bits = ((bits & 0x00FF'00FFU) << 8U) | ((bits & 0xFF00'FF00U) >> 8U);
    return static_cast<double>(bits) * 2.3283064365386963e-10;
}

[[nodiscard]] double SceneLinearLuminanceUnchecked(Rgb color) noexcept
{
    return (0.2126 * color.r) + (0.7152 * color.g) + (0.0722 * color.b);
}

} // namespace

std::expected<std::uint32_t, ContractError> PixelCount(Extent2D extent) noexcept
{
    auto const extentCheck = ValidateExtent(extent);
    if (!extentCheck)
    {
        return std::unexpected(extentCheck.error());
    }
    std::uint64_t const count = static_cast<std::uint64_t>(extent.width) * static_cast<std::uint64_t>(extent.height);
    if (count > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        return std::unexpected(ContractError::ExtentTooLarge);
    }
    return static_cast<std::uint32_t>(count);
}

std::expected<double, ContractError> SceneLinearLuminance(Rgb color) noexcept
{
    auto const radianceCheck = ValidateRadiance(color);
    if (!radianceCheck)
    {
        return std::unexpected(radianceCheck.error());
    }
    return SceneLinearLuminanceUnchecked(color);
}

std::expected<StageContract, ContractError> DescribeStage(PostStage stage, PipelinePolicy const &policy) noexcept
{
    StageContract contract{};
    contract.stage = stage;
    switch (stage)
    {
    case PostStage::TemporalResolve:
        contract.inputDomain = SignalDomain::SceneLinearRenderJittered;
        contract.outputDomain = SignalDomain::SceneLinearDisplay;
        contract.consumesLinearDepth = true;
        contract.consumesMotion = true;
        contract.producesDisplayExtent = true;
        return contract;
    case PostStage::DepthOfField:
        contract.inputDomain = SignalDomain::SceneLinearDisplay;
        contract.outputDomain = SignalDomain::SceneLinearDisplay;
        contract.consumesLinearDepth = true;
        contract.producesDisplayExtent = true;
        return contract;
    case PostStage::MotionBlur:
        contract.inputDomain = SignalDomain::SceneLinearDisplay;
        contract.outputDomain = SignalDomain::SceneLinearDisplay;
        contract.consumesLinearDepth = true;
        contract.consumesMotion = true;
        contract.producesDisplayExtent = true;
        return contract;
    case PostStage::Bloom:
        contract.inputDomain = SignalDomain::SceneLinearDisplay;
        contract.outputDomain = SignalDomain::SceneLinearDisplay;
        contract.producesDisplayExtent = true;
        return contract;
    case PostStage::Exposure:
        contract.inputDomain = SignalDomain::SceneLinearDisplay;
        contract.outputDomain = SignalDomain::SceneLinearExposed;
        contract.producesDisplayExtent = true;
        return contract;
    case PostStage::ToneMap:
        contract.inputDomain = SignalDomain::SceneLinearExposed;
        contract.outputDomain = SignalDomain::DisplayLinear;
        contract.producesDisplayExtent = true;
        return contract;
    case PostStage::DisplayEncode:
        contract.inputDomain = SignalDomain::DisplayLinear;
        contract.outputDomain = SignalDomain::DisplayEncoded;
        contract.producesDisplayExtent = true;
        return contract;
    case PostStage::UiComposite:
        contract.inputDomain = policy.uiPlacement == UiCompositePlacement::AfterDisplayEncode
                                   ? SignalDomain::DisplayEncoded
                                   : SignalDomain::DisplayLinear;
        contract.outputDomain = contract.inputDomain;
        contract.producesDisplayExtent = true;
        return contract;
    }
    return std::unexpected(ContractError::InvalidStage);
}

std::expected<PipelinePlan, ContractError> PlanPipeline(std::span<PostStage const> stages,
                                                        PipelinePolicy const &policy) noexcept
{
    if (stages.empty())
    {
        return std::unexpected(ContractError::EmptyPipeline);
    }
    if (stages.size() > static_cast<std::size_t>(kMaximumStageCount))
    {
        return std::unexpected(ContractError::TooManyStages);
    }

    PipelinePlan plan{};
    plan.stageCount = static_cast<std::uint32_t>(stages.size());
    plan.firstViolationStageIndex = plan.stageCount;

    std::array<std::uint32_t, kPostStageCount> firstPosition{};
    firstPosition.fill(kInvalidPosition);

    for (std::uint32_t index = 0U; index < plan.stageCount; ++index)
    {
        auto const contract = DescribeStage(stages[index], policy);
        if (!contract)
        {
            return std::unexpected(contract.error());
        }
        plan.stages[index] = *contract;
    }

    auto const note = [&plan](OrderViolation violation, std::uint32_t index)
    {
        plan.violations |= violation;
        plan.firstViolationStageIndex = std::min(plan.firstViolationStageIndex, index);
    };

    for (std::uint32_t index = 0U; index < plan.stageCount; ++index)
    {
        auto const slot = static_cast<std::uint32_t>(stages[index]);
        if (firstPosition[slot] == kInvalidPosition)
        {
            firstPosition[slot] = index;
        }
        else
        {
            note(OrderViolation::DuplicateStage, index);
        }
    }

    auto const positionOf = [&firstPosition](PostStage stage)
    { return firstPosition[static_cast<std::uint32_t>(stage)]; };
    auto const present = [&positionOf](PostStage stage) { return positionOf(stage) != kInvalidPosition; };

    if (policy.requireTemporalResolve && !present(PostStage::TemporalResolve))
    {
        plan.violations |= OrderViolation::MissingTemporalResolve;
    }
    if (!present(PostStage::Exposure))
    {
        plan.violations |= OrderViolation::MissingExposure;
    }
    if (!present(PostStage::ToneMap))
    {
        plan.violations |= OrderViolation::MissingToneMap;
    }
    if (!present(PostStage::DisplayEncode))
    {
        plan.violations |= OrderViolation::MissingDisplayEncode;
    }
    if (policy.requireUiComposite && !present(PostStage::UiComposite))
    {
        plan.violations |= OrderViolation::MissingUiComposite;
    }

    for (std::uint32_t index = 0U; index < plan.stageCount; ++index)
    {
        PostStage const stage = stages[index];
        if (present(PostStage::DisplayEncode) && index > positionOf(PostStage::DisplayEncode) &&
            IsLinearDomainStage(stage))
        {
            note(OrderViolation::FilterAfterDisplayEncode, index);
        }
        if (present(PostStage::ToneMap) && index > positionOf(PostStage::ToneMap) && IsSceneLinearFilter(stage))
        {
            note(OrderViolation::FilterAfterToneMap, index);
        }
        if (present(PostStage::UiComposite) && index > positionOf(PostStage::UiComposite))
        {
            // Display encoding is the one stage that may follow a UI composited in display-linear: it converts the
            // whole composited image, it does not filter it.
            bool const encodesLinearUi = stage == PostStage::DisplayEncode &&
                                         policy.uiPlacement == UiCompositePlacement::BeforeDisplayEncodeLinear;
            if (!encodesLinearUi)
            {
                note(OrderViolation::StageAfterUiComposite, index);
            }
        }
        if (present(PostStage::Exposure) && index > positionOf(PostStage::Exposure) && IsCameraEffect(stage))
        {
            note(OrderViolation::CameraEffectAfterExposure, index);
        }
        if (present(PostStage::TemporalResolve) && IsCameraEffect(stage) &&
            positionOf(PostStage::TemporalResolve) > index)
        {
            note(OrderViolation::TemporalResolveAfterCameraEffect, positionOf(PostStage::TemporalResolve));
        }
    }

    if (present(PostStage::Exposure) && present(PostStage::ToneMap) &&
        positionOf(PostStage::Exposure) > positionOf(PostStage::ToneMap))
    {
        note(OrderViolation::ExposureAfterToneMap, positionOf(PostStage::ToneMap));
    }
    if (present(PostStage::DisplayEncode) && present(PostStage::ToneMap) &&
        positionOf(PostStage::DisplayEncode) < positionOf(PostStage::ToneMap))
    {
        note(OrderViolation::DisplayEncodeBeforeToneMap, positionOf(PostStage::DisplayEncode));
    }

    if (present(PostStage::DepthOfField) && present(PostStage::MotionBlur))
    {
        bool const defocusFirst = positionOf(PostStage::DepthOfField) < positionOf(PostStage::MotionBlur);
        bool const expectDefocusFirst = policy.cameraEffectOrder == CameraEffectOrder::DefocusThenShutter;
        if (defocusFirst != expectDefocusFirst)
        {
            note(OrderViolation::CameraEffectOrderMismatch,
                 std::min(positionOf(PostStage::DepthOfField), positionOf(PostStage::MotionBlur)));
        }
    }

    if (present(PostStage::Bloom))
    {
        std::uint32_t const bloomPosition = positionOf(PostStage::Bloom);
        for (PostStage const effect : {PostStage::DepthOfField, PostStage::MotionBlur})
        {
            if (!present(effect))
            {
                continue;
            }
            bool const bloomAfter = bloomPosition > positionOf(effect);
            bool const expectAfter = policy.bloomSource == BloomSourcePlacement::AfterCameraEffects;
            if (bloomAfter != expectAfter)
            {
                note(OrderViolation::BloomPlacementMismatch, std::min(bloomPosition, positionOf(effect)));
            }
        }
    }

    if (present(PostStage::UiComposite) && present(PostStage::DisplayEncode))
    {
        std::uint32_t const uiPosition = positionOf(PostStage::UiComposite);
        std::uint32_t const encodePosition = positionOf(PostStage::DisplayEncode);
        if (policy.uiPlacement == UiCompositePlacement::AfterDisplayEncode)
        {
            if (uiPosition < encodePosition)
            {
                note(OrderViolation::UiPlacementMismatch, uiPosition);
            }
        }
        else if (uiPosition > encodePosition)
        {
            note(OrderViolation::UiPlacementMismatch, encodePosition);
        }
    }

    plan.inputDomain = plan.stages[0].inputDomain;
    SignalDomain running = plan.inputDomain;
    for (std::uint32_t index = 0U; index < plan.stageCount; ++index)
    {
        if (plan.stages[index].inputDomain != running)
        {
            note(OrderViolation::SignalDomainMismatch, index);
        }
        running = plan.stages[index].outputDomain;
    }
    plan.outputDomain = running;
    return plan;
}

std::expected<PipelinePlan, ContractError> CanonicalPipeline(PipelinePolicy const &policy) noexcept
{
    std::array<PostStage, kMaximumStageCount> order{};
    std::uint32_t count = 0U;
    auto const append = [&order, &count](PostStage stage)
    {
        order[count] = stage;
        ++count;
    };

    if (policy.requireTemporalResolve)
    {
        append(PostStage::TemporalResolve);
    }
    if (policy.bloomSource == BloomSourcePlacement::BeforeCameraEffects)
    {
        append(PostStage::Bloom);
    }
    if (policy.cameraEffectOrder == CameraEffectOrder::DefocusThenShutter)
    {
        append(PostStage::DepthOfField);
        append(PostStage::MotionBlur);
    }
    else
    {
        append(PostStage::MotionBlur);
        append(PostStage::DepthOfField);
    }
    if (policy.bloomSource == BloomSourcePlacement::AfterCameraEffects)
    {
        append(PostStage::Bloom);
    }
    append(PostStage::Exposure);
    append(PostStage::ToneMap);
    if (policy.requireUiComposite && policy.uiPlacement == UiCompositePlacement::BeforeDisplayEncodeLinear)
    {
        append(PostStage::UiComposite);
    }
    append(PostStage::DisplayEncode);
    if (policy.requireUiComposite && policy.uiPlacement == UiCompositePlacement::AfterDisplayEncode)
    {
        append(PostStage::UiComposite);
    }

    return PlanPipeline(std::span<PostStage const>{order.data(), static_cast<std::size_t>(count)}, policy);
}

std::expected<PostFrameFacts, ContractError> ValidatePostFrame(CameraPostFrame const &frame) noexcept
{
    for (Extent2D const extent : {frame.renderExtent, frame.displayExtent, frame.depthExtent, frame.motionExtent})
    {
        auto const extentCheck = ValidateExtent(extent);
        if (!extentCheck)
        {
            return std::unexpected(extentCheck.error());
        }
    }
    if (frame.depthExtent != frame.renderExtent || frame.motionExtent != frame.renderExtent)
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    if (frame.displayExtent.width < frame.renderExtent.width || frame.displayExtent.height < frame.renderExtent.height)
    {
        return std::unexpected(ContractError::InvalidExtent);
    }
    if (!IsFinite(frame.currentJitterPixels) || !IsFinite(frame.previousJitterPixels))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (std::abs(frame.currentJitterPixels.x) > 1.0 || std::abs(frame.currentJitterPixels.y) > 1.0 ||
        std::abs(frame.previousJitterPixels.x) > 1.0 || std::abs(frame.previousJitterPixels.y) > 1.0)
    {
        return std::unexpected(ContractError::InvalidJitter);
    }
    if (!IsFinite(frame.preExposure) || !IsFinite(frame.previousPreExposure))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (frame.preExposure <= 0.0 || frame.previousPreExposure <= 0.0 || frame.preExposure > kMaximumSceneLinearValue ||
        frame.previousPreExposure > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidPreExposure);
    }
    if (!IsFinite(frame.exposureScale))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (frame.exposureScale <= 0.0 || frame.exposureScale > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidExposure);
    }
    if (!IsFinite(frame.frameDeltaSeconds) || !IsFinite(frame.shutterOpenSeconds))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (frame.frameDeltaSeconds <= 0.0 || frame.shutterOpenSeconds < 0.0 ||
        frame.shutterOpenSeconds > frame.frameDeltaSeconds)
    {
        return std::unexpected(ContractError::InvalidShutter);
    }
    if (!IsFinite(frame.nearPlaneMetres) || !IsFinite(frame.farPlaneMetres))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (frame.nearPlaneMetres <= 0.0 || frame.farPlaneMetres <= frame.nearPlaneMetres ||
        frame.farPlaneMetres > kMaximumViewDepthMetres)
    {
        return std::unexpected(ContractError::InvalidDepthRange);
    }

    PostFrameFacts facts{};
    facts.renderExtent = frame.renderExtent;
    facts.displayExtent = frame.displayExtent;
    facts.displayPixelsPerRenderPixel = {
        .x = static_cast<double>(frame.displayExtent.width) / static_cast<double>(frame.renderExtent.width),
        .y = static_cast<double>(frame.displayExtent.height) / static_cast<double>(frame.renderExtent.height)};
    facts.jitterDeltaPixels = {.x = frame.previousJitterPixels.x - frame.currentJitterPixels.x,
                               .y = frame.previousJitterPixels.y - frame.currentJitterPixels.y};
    facts.exposureTimeFraction = frame.shutterOpenSeconds / frame.frameDeltaSeconds;
    facts.preExposure = frame.preExposure;
    facts.exposureScale = frame.exposureScale;
    facts.historyToCurrentPreExposureScale = frame.preExposure / frame.previousPreExposure;
    facts.nearPlaneMetres = frame.nearPlaneMetres;
    facts.farPlaneMetres = frame.farPlaneMetres;
    facts.upscales = frame.displayExtent != frame.renderExtent;
    facts.temporalResetRequired = !frame.hasHistory || frame.cameraCut;
    return facts;
}

std::expected<BloomExtraction, ContractError> ExtractBloomHighlights(Rgb preExposedSceneLinear, double preExposure,
                                                                     BloomThresholdSettings const &settings) noexcept
{
    auto const radianceCheck = ValidateRadiance(preExposedSceneLinear);
    if (!radianceCheck)
    {
        return std::unexpected(radianceCheck.error());
    }
    if (!IsFinite(preExposure))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (preExposure <= 0.0 || preExposure > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidPreExposure);
    }
    if (!IsFinite(settings.thresholdLuminance) || !IsFinite(settings.softKneeLuminance))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.thresholdLuminance < 0.0 || settings.thresholdLuminance > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidThreshold);
    }
    if (settings.softKneeLuminance < 0.0 || settings.softKneeLuminance > settings.thresholdLuminance)
    {
        return std::unexpected(ContractError::InvalidKnee);
    }

    BloomExtraction extraction{};
    extraction.preExposedLuminance = SceneLinearLuminanceUnchecked(preExposedSceneLinear);
    Rgb const absoluteColor = Scale(preExposedSceneLinear, 1.0 / preExposure);
    extraction.absoluteLuminance = extraction.preExposedLuminance / preExposure;

    double const threshold = settings.thresholdLuminance;
    double const knee = settings.softKneeLuminance;
    double const luminance = extraction.absoluteLuminance;
    extraction.belowKnee = luminance <= threshold - knee;
    extraction.aboveKnee = luminance >= threshold + knee;
    if (extraction.belowKnee)
    {
        extraction.excessLuminance = 0.0;
    }
    else if (extraction.aboveKnee)
    {
        extraction.excessLuminance = luminance - threshold;
    }
    else
    {
        // Only reachable when the knee has a positive width: a zero knee makes the two branches above exhaustive,
        // so this division can never be by zero.
        double const kneeInput = luminance - threshold + knee;
        extraction.excessLuminance = (kneeInput * kneeInput) / (4.0 * knee);
    }

    double const rawWeight = luminance > 0.0 ? extraction.excessLuminance / luminance : 0.0;
    extraction.thresholdWeight = std::clamp(rawWeight, 0.0, 1.0);
    extraction.weightClamped = extraction.thresholdWeight != rawWeight;
    extraction.extractedPreExposed = Scale(preExposedSceneLinear, extraction.thresholdWeight);
    extraction.extractedAbsolute = Scale(absoluteColor, extraction.thresholdWeight);
    return extraction;
}

std::expected<BloomPyramid, ContractError> BuildBloomPyramid(Extent2D baseExtent,
                                                             BloomPyramidSettings const &settings) noexcept
{
    auto const extentCheck = ValidateExtent(baseExtent);
    if (!extentCheck)
    {
        return std::unexpected(extentCheck.error());
    }
    if (settings.maximumLevelCount == 0U || settings.maximumLevelCount > kMaximumBloomLevelCount)
    {
        return std::unexpected(ContractError::InvalidLevelCount);
    }
    if (settings.minimumLevelExtent == 0U)
    {
        return std::unexpected(ContractError::InvalidExtent);
    }

    BloomPyramid pyramid{};
    pyramid.levelCount = 1U;
    pyramid.levelExtents[0] = baseExtent;
    Extent2D current = baseExtent;
    while (pyramid.levelCount < settings.maximumLevelCount && current.width > settings.minimumLevelExtent &&
           current.height > settings.minimumLevelExtent)
    {
        current = {.width = CeilingHalf(current.width), .height = CeilingHalf(current.height)};
        pyramid.levelExtents[pyramid.levelCount] = current;
        ++pyramid.levelCount;
    }
    return pyramid;
}

std::expected<DownsampleFootprint1D, ContractError> BloomDownsampleFootprint(std::uint32_t destinationIndex,
                                                                             std::uint32_t sourceExtent,
                                                                             std::uint32_t destinationExtent) noexcept
{
    if (sourceExtent == 0U || destinationExtent == 0U || sourceExtent > kMaximumDimension ||
        destinationExtent > sourceExtent || sourceExtent > 2U * destinationExtent)
    {
        return std::unexpected(ContractError::InvalidExtent);
    }
    if (destinationIndex >= destinationExtent)
    {
        return std::unexpected(ContractError::InvalidPixel);
    }

    // Exact rational arithmetic in units of one destination texel: the source interval owned by destination texel j
    // is [j * sourceExtent, (j + 1) * sourceExtent) and source texel i spans [i * destinationExtent,
    // (i + 1) * destinationExtent). No rounding happens before the single division that produces each weight.
    std::uint64_t const source = static_cast<std::uint64_t>(sourceExtent);
    std::uint64_t const destination = static_cast<std::uint64_t>(destinationExtent);
    std::uint64_t const start = static_cast<std::uint64_t>(destinationIndex) * source;
    std::uint64_t const end = start + source;

    DownsampleFootprint1D footprint{};
    std::uint64_t index = start / destination;
    while (index < source && (index * destination) < end)
    {
        std::uint64_t const texelStart = index * destination;
        std::uint64_t const texelEnd = texelStart + destination;
        std::uint64_t const overlap = std::min(end, texelEnd) - std::max(start, texelStart);
        if (overlap > 0U)
        {
            if (footprint.tapCount >= footprint.taps.size())
            {
                return std::unexpected(ContractError::InvalidExtent);
            }
            footprint.taps[footprint.tapCount] = {.index = static_cast<std::uint32_t>(index),
                                                  .weight = static_cast<double>(overlap) / static_cast<double>(source)};
            ++footprint.tapCount;
        }
        ++index;
    }
    return footprint;
}

std::expected<UpsampleFootprint1D, ContractError> BloomUpsampleFootprint(std::uint32_t destinationIndex,
                                                                         std::uint32_t sourceExtent,
                                                                         std::uint32_t destinationExtent) noexcept
{
    if (sourceExtent == 0U || destinationExtent == 0U || destinationExtent > kMaximumDimension ||
        destinationExtent < sourceExtent)
    {
        return std::unexpected(ContractError::InvalidExtent);
    }
    if (destinationIndex >= destinationExtent)
    {
        return std::unexpected(ContractError::InvalidPixel);
    }

    double const scale = static_cast<double>(sourceExtent) / static_cast<double>(destinationExtent);
    double const coordinate = ((static_cast<double>(destinationIndex) + 0.5) * scale) - 0.5;
    double const flooredCoordinate = std::floor(coordinate);
    double const fraction = coordinate - flooredCoordinate;
    double const highestIndex = static_cast<double>(sourceExtent) - 1.0;
    double const lowCoordinate = std::clamp(flooredCoordinate, 0.0, highestIndex);
    double const highCoordinate = std::clamp(flooredCoordinate + 1.0, 0.0, highestIndex);

    UpsampleFootprint1D footprint{};
    footprint.lowIndex = static_cast<std::uint32_t>(lowCoordinate);
    footprint.highIndex = static_cast<std::uint32_t>(highCoordinate);
    footprint.lowWeight = 1.0 - fraction;
    footprint.highWeight = fraction;
    footprint.clampedToBorder = flooredCoordinate < 0.0 || (flooredCoordinate + 1.0) > highestIndex;
    return footprint;
}

std::expected<void, ContractError> ValidateSceneLinearImage(SceneLinearImageView image) noexcept
{
    auto const pixelCount = ValidateReferenceExtent(image.extent);
    if (!pixelCount)
    {
        return std::unexpected(pixelCount.error());
    }
    if (image.pixels.size() != static_cast<std::size_t>(*pixelCount))
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    for (Rgb const pixel : image.pixels)
    {
        auto const radianceCheck = ValidateRadiance(pixel);
        if (!radianceCheck)
        {
            return std::unexpected(radianceCheck.error());
        }
    }
    return {};
}

std::expected<Rgb, ContractError> SampleSceneLinear(SceneLinearImageView image, PixelCoordinate pixel) noexcept
{
    auto const imageCheck = ValidateSceneLinearImage(image);
    if (!imageCheck)
    {
        return std::unexpected(imageCheck.error());
    }
    if (!IsInsideExtent(image.extent, pixel))
    {
        return std::unexpected(ContractError::InvalidPixel);
    }
    return image.pixels[TexelIndex(image.extent, pixel)];
}

std::expected<void, ContractError> DownsampleSceneLinear(SceneLinearImageView source, Extent2D destinationExtent,
                                                         std::span<Rgb> destination) noexcept
{
    auto const sourceCheck = ValidateSceneLinearImage(source);
    if (!sourceCheck)
    {
        return std::unexpected(sourceCheck.error());
    }
    auto const destinationCount = ValidateReferenceExtent(destinationExtent);
    if (!destinationCount)
    {
        return std::unexpected(destinationCount.error());
    }
    if (destination.size() != static_cast<std::size_t>(*destinationCount))
    {
        return std::unexpected(ContractError::SizeMismatch);
    }

    for (std::uint32_t y = 0U; y < destinationExtent.height; ++y)
    {
        auto const verticalFootprint = BloomDownsampleFootprint(y, source.extent.height, destinationExtent.height);
        if (!verticalFootprint)
        {
            return std::unexpected(verticalFootprint.error());
        }
        for (std::uint32_t x = 0U; x < destinationExtent.width; ++x)
        {
            auto const horizontalFootprint = BloomDownsampleFootprint(x, source.extent.width, destinationExtent.width);
            if (!horizontalFootprint)
            {
                return std::unexpected(horizontalFootprint.error());
            }
            Rgb accumulated{};
            for (std::uint32_t verticalTap = 0U; verticalTap < verticalFootprint->tapCount; ++verticalTap)
            {
                ResampleTap const vertical = verticalFootprint->taps[verticalTap];
                for (std::uint32_t horizontalTap = 0U; horizontalTap < horizontalFootprint->tapCount; ++horizontalTap)
                {
                    ResampleTap const horizontal = horizontalFootprint->taps[horizontalTap];
                    Rgb const texel =
                        source.pixels[TexelIndex(source.extent, {.x = horizontal.index, .y = vertical.index})];
                    accumulated = Add(accumulated, Scale(texel, horizontal.weight * vertical.weight));
                }
            }
            destination[TexelIndex(destinationExtent, {.x = x, .y = y})] = accumulated;
        }
    }
    return {};
}

std::expected<void, ContractError> UpsampleSceneLinear(SceneLinearImageView source, Extent2D destinationExtent,
                                                       std::span<Rgb> destination) noexcept
{
    auto const sourceCheck = ValidateSceneLinearImage(source);
    if (!sourceCheck)
    {
        return std::unexpected(sourceCheck.error());
    }
    auto const destinationCount = ValidateReferenceExtent(destinationExtent);
    if (!destinationCount)
    {
        return std::unexpected(destinationCount.error());
    }
    if (destination.size() != static_cast<std::size_t>(*destinationCount))
    {
        return std::unexpected(ContractError::SizeMismatch);
    }

    for (std::uint32_t y = 0U; y < destinationExtent.height; ++y)
    {
        auto const verticalFootprint = BloomUpsampleFootprint(y, source.extent.height, destinationExtent.height);
        if (!verticalFootprint)
        {
            return std::unexpected(verticalFootprint.error());
        }
        for (std::uint32_t x = 0U; x < destinationExtent.width; ++x)
        {
            auto const horizontalFootprint = BloomUpsampleFootprint(x, source.extent.width, destinationExtent.width);
            if (!horizontalFootprint)
            {
                return std::unexpected(horizontalFootprint.error());
            }
            std::array<std::uint32_t, 2U> const columns{horizontalFootprint->lowIndex, horizontalFootprint->highIndex};
            std::array<double, 2U> const columnWeights{horizontalFootprint->lowWeight, horizontalFootprint->highWeight};
            std::array<std::uint32_t, 2U> const rows{verticalFootprint->lowIndex, verticalFootprint->highIndex};
            std::array<double, 2U> const rowWeights{verticalFootprint->lowWeight, verticalFootprint->highWeight};

            Rgb accumulated{};
            for (std::size_t row = 0U; row < rows.size(); ++row)
            {
                for (std::size_t column = 0U; column < columns.size(); ++column)
                {
                    Rgb const texel = source.pixels[TexelIndex(source.extent, {.x = columns[column], .y = rows[row]})];
                    accumulated = Add(accumulated, Scale(texel, columnWeights[column] * rowWeights[row]));
                }
            }
            destination[TexelIndex(destinationExtent, {.x = x, .y = y})] = accumulated;
        }
    }
    return {};
}

std::expected<std::uint32_t, ContractError> BloomScratchPixelCount(BloomPyramid const &pyramid) noexcept
{
    if (pyramid.levelCount == 0U || pyramid.levelCount > kMaximumBloomLevelCount)
    {
        return std::unexpected(ContractError::InvalidLevelCount);
    }
    std::uint64_t total = 0U;
    for (std::uint32_t level = 0U; level < pyramid.levelCount; ++level)
    {
        auto const count = ValidateReferenceExtent(pyramid.levelExtents[level]);
        if (!count)
        {
            return std::unexpected(count.error());
        }
        if (level > 0U && (pyramid.levelExtents[level].width > pyramid.levelExtents[level - 1U].width ||
                           pyramid.levelExtents[level].height > pyramid.levelExtents[level - 1U].height))
        {
            return std::unexpected(ContractError::InvalidExtent);
        }
        // Levels one and above are stored, and the accumulation buffer is as large as level zero.
        total += static_cast<std::uint64_t>(*count);
    }
    if (total > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        return std::unexpected(ContractError::ExtentTooLarge);
    }
    return static_cast<std::uint32_t>(total);
}

std::expected<BloomResult, ContractError> ComputeBloom(SceneLinearImageView extracted, BloomPyramid const &pyramid,
                                                       BloomCombineSettings const &settings, std::span<Rgb> scratch,
                                                       std::span<Rgb> output) noexcept
{
    auto const extractedCheck = ValidateSceneLinearImage(extracted);
    if (!extractedCheck)
    {
        return std::unexpected(extractedCheck.error());
    }
    auto const scratchRequirement = BloomScratchPixelCount(pyramid);
    if (!scratchRequirement)
    {
        return std::unexpected(scratchRequirement.error());
    }
    if (pyramid.levelExtents[0] != extracted.extent)
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    if (scratch.size() < static_cast<std::size_t>(*scratchRequirement))
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    if (output.size() != extracted.pixels.size())
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    if (!IsFinite(settings.bloomGain) || settings.bloomGain < 0.0 || settings.bloomGain > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidWeight);
    }

    double requestedWeightSum = 0.0;
    for (std::uint32_t level = 0U; level < pyramid.levelCount; ++level)
    {
        double const weight = settings.levelWeights[level];
        if (!IsFinite(weight) || weight < 0.0 || weight > kMaximumSceneLinearValue)
        {
            return std::unexpected(ContractError::InvalidWeight);
        }
        requestedWeightSum += weight;
    }
    if (requestedWeightSum <= 0.0)
    {
        return std::unexpected(ContractError::InvalidWeight);
    }

    BloomResult result{};
    result.levelCount = pyramid.levelCount;
    result.requestedWeightSum = requestedWeightSum;
    result.weightsNormalized = settings.normalizeLevelWeights;
    result.appliedGain = settings.bloomGain;

    // Level zero is the caller's extracted image. Levels one and above live at the front of the scratch span; the
    // accumulation buffer occupies the tail and is always as large as level zero.
    std::array<std::span<Rgb>, kMaximumBloomLevelCount> levelStorage{};
    std::size_t scratchOffset = 0U;
    for (std::uint32_t level = 1U; level < pyramid.levelCount; ++level)
    {
        auto const count = PixelCount(pyramid.levelExtents[level]);
        if (!count)
        {
            return std::unexpected(count.error());
        }
        levelStorage[level] = scratch.subspan(scratchOffset, static_cast<std::size_t>(*count));
        scratchOffset += static_cast<std::size_t>(*count);
    }
    std::span<Rgb> const accumulator = scratch.subspan(scratchOffset, extracted.pixels.size());

    auto const levelView = [&](std::uint32_t level)
    {
        return level == 0U ? extracted
                           : SceneLinearImageView{.extent = pyramid.levelExtents[level],
                                                  .pixels = std::span<Rgb const>{levelStorage[level]}};
    };

    for (std::uint32_t level = 1U; level < pyramid.levelCount; ++level)
    {
        auto const downsample =
            DownsampleSceneLinear(levelView(level - 1U), pyramid.levelExtents[level], levelStorage[level]);
        if (!downsample)
        {
            return std::unexpected(downsample.error());
        }
    }

    double appliedWeightSum = 0.0;
    for (std::uint32_t level = 0U; level < pyramid.levelCount; ++level)
    {
        double const weight = settings.normalizeLevelWeights ? settings.levelWeights[level] / requestedWeightSum
                                                             : settings.levelWeights[level];
        appliedWeightSum += weight;
        SceneLinearImageView const view = levelView(level);
        double luminanceSum = 0.0;
        for (Rgb const pixel : view.pixels)
        {
            luminanceSum += SceneLinearLuminanceUnchecked(pixel);
        }
        result.levels[level] = {.extent = pyramid.levelExtents[level],
                                .luminanceSum = luminanceSum,
                                .meanLuminance = luminanceSum / static_cast<double>(view.pixels.size()),
                                .appliedWeight = weight};
    }
    result.appliedWeightSum = appliedWeightSum;
    result.extractedLuminanceSum = result.levels[0].luminanceSum;

    // Progressive combine from the smallest level down to level zero. The accumulator always holds the partial sum
    // at the level currently being processed, so nothing is ever resampled twice.
    std::uint32_t const topLevel = pyramid.levelCount - 1U;
    {
        SceneLinearImageView const view = levelView(topLevel);
        for (std::size_t index = 0U; index < view.pixels.size(); ++index)
        {
            accumulator[index] = Scale(view.pixels[index], result.levels[topLevel].appliedWeight);
        }
    }
    for (std::uint32_t level = topLevel; level > 0U; --level)
    {
        Extent2D const sourceExtent = pyramid.levelExtents[level];
        Extent2D const destinationExtent = pyramid.levelExtents[level - 1U];
        auto const destinationCount = PixelCount(destinationExtent);
        if (!destinationCount)
        {
            return std::unexpected(destinationCount.error());
        }
        auto const sourceCount = PixelCount(sourceExtent);
        if (!sourceCount)
        {
            return std::unexpected(sourceCount.error());
        }
        SceneLinearImageView const accumulated{
            .extent = sourceExtent,
            .pixels = std::span<Rgb const>{accumulator.first(static_cast<std::size_t>(*sourceCount))}};
        std::span<Rgb> const target = output.first(static_cast<std::size_t>(*destinationCount));
        auto const upsample = UpsampleSceneLinear(accumulated, destinationExtent, target);
        if (!upsample)
        {
            return std::unexpected(upsample.error());
        }
        SceneLinearImageView const view = levelView(level - 1U);
        for (std::size_t index = 0U; index < target.size(); ++index)
        {
            target[index] = Add(target[index], Scale(view.pixels[index], result.levels[level - 1U].appliedWeight));
            accumulator[index] = target[index];
        }
    }
    if (pyramid.levelCount == 1U)
    {
        for (std::size_t index = 0U; index < output.size(); ++index)
        {
            output[index] = accumulator[index];
        }
    }

    double outputLuminanceSum = 0.0;
    for (Rgb &pixel : output)
    {
        pixel = Scale(pixel, settings.bloomGain);
        auto const radianceCheck = ValidateRadiance(pixel);
        if (!radianceCheck)
        {
            return std::unexpected(radianceCheck.error());
        }
        outputLuminanceSum += SceneLinearLuminanceUnchecked(pixel);
    }
    result.outputLuminanceSum = outputLuminanceSum;
    result.energyRatio = result.extractedLuminanceSum > 0.0 ? outputLuminanceSum / result.extractedLuminanceSum : 0.0;
    return result;
}

std::expected<Float2, ContractError> MotionToDisplayPixels(Float2 motionPreviousMinusCurrentUv,
                                                           Extent2D displayExtent) noexcept
{
    auto const extentCheck = ValidateExtent(displayExtent);
    if (!extentCheck)
    {
        return std::unexpected(extentCheck.error());
    }
    if (!IsFinite(motionPreviousMinusCurrentUv))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (std::abs(motionPreviousMinusCurrentUv.x) > kMaximumMotionUv ||
        std::abs(motionPreviousMinusCurrentUv.y) > kMaximumMotionUv)
    {
        return std::unexpected(ContractError::InvalidMotion);
    }
    return Float2{.x = -motionPreviousMinusCurrentUv.x * static_cast<double>(displayExtent.width),
                  .y = -motionPreviousMinusCurrentUv.y * static_cast<double>(displayExtent.height)};
}

std::expected<ShutterSchedule, ContractError> BuildShutterSchedule(Float2 motionPreviousMinusCurrentUv,
                                                                   PostFrameFacts const &frame,
                                                                   MotionBlurSettings const &settings) noexcept
{
    if (settings.sampleCount == 0U || settings.sampleCount > kMaximumMotionBlurSampleCount)
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }
    if (!IsFinite(settings.maximumDisplacementPixels) || settings.maximumDisplacementPixels <= 0.0)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    if (!IsFinite(settings.depthCompareAbsoluteMetres) || !IsFinite(settings.depthCompareRelative) ||
        settings.depthCompareAbsoluteMetres < 0.0 || settings.depthCompareRelative < 0.0)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    if (!IsFinite(frame.exposureTimeFraction) || frame.exposureTimeFraction < 0.0 || frame.exposureTimeFraction > 1.0)
    {
        return std::unexpected(ContractError::InvalidShutter);
    }

    auto const frameDisplacement = MotionToDisplayPixels(motionPreviousMinusCurrentUv, frame.displayExtent);
    if (!frameDisplacement)
    {
        return std::unexpected(frameDisplacement.error());
    }

    ShutterSchedule schedule{};
    schedule.sampleCount = settings.sampleCount;
    schedule.frameDisplacementPixels = *frameDisplacement;
    Float2 shutterDisplacement{.x = frameDisplacement->x * frame.exposureTimeFraction,
                               .y = frameDisplacement->y * frame.exposureTimeFraction};
    double const shutterLength = Length(shutterDisplacement);
    schedule.exceedsBudget = shutterLength > settings.maximumDisplacementPixels;
    schedule.appliedScale = 1.0;
    if (schedule.exceedsBudget && settings.clampDisplacementToBudget)
    {
        schedule.appliedScale = settings.maximumDisplacementPixels / shutterLength;
        shutterDisplacement = {.x = shutterDisplacement.x * schedule.appliedScale,
                               .y = shutterDisplacement.y * schedule.appliedScale};
        schedule.clampedByBudget = true;
    }
    schedule.shutterDisplacementPixels = shutterDisplacement;
    schedule.gatherRadiusPixels = 0.5 * Length(shutterDisplacement);

    double parameterSum = 0.0;
    for (std::uint32_t sample = 0U; sample < settings.sampleCount; ++sample)
    {
        double parameter = 0.0;
        if (settings.schedule == ShutterSampleSchedule::EndpointInclusive)
        {
            parameter = settings.sampleCount == 1U
                            ? 0.0
                            : -0.5 + (static_cast<double>(sample) / static_cast<double>(settings.sampleCount - 1U));
        }
        else
        {
            parameter = ((static_cast<double>(sample) + 0.5) / static_cast<double>(settings.sampleCount)) - 0.5;
        }
        schedule.parameters[sample] = parameter;
        schedule.offsetsPixels[sample] = {.x = parameter * shutterDisplacement.x,
                                          .y = parameter * shutterDisplacement.y};
        parameterSum += parameter;
        if (parameter == 0.0)
        {
            schedule.includesCenterSample = true;
        }
    }
    schedule.centroidParameter = parameterSum / static_cast<double>(settings.sampleCount);
    return schedule;
}

std::expected<MotionBlurResult, ContractError> EvaluateMotionBlur(PixelCoordinate displayPixel,
                                                                  MotionBlurImages const &images,
                                                                  PostFrameFacts const &frame,
                                                                  MotionBlurSettings const &settings) noexcept
{
    if (images.sceneLinear.extent != frame.displayExtent)
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    auto const colorCheck = ValidateSceneLinearImage(images.sceneLinear);
    if (!colorCheck)
    {
        return std::unexpected(colorCheck.error());
    }
    auto const depthCheck =
        ValidateDepthImage(images.viewDepthMetres, frame.displayExtent, frame.nearPlaneMetres, frame.farPlaneMetres);
    if (!depthCheck)
    {
        return std::unexpected(depthCheck.error());
    }
    auto const motionCheck = ValidateMotionImage(images.motion, frame.displayExtent);
    if (!motionCheck)
    {
        return std::unexpected(motionCheck.error());
    }
    if (!IsInsideExtent(frame.displayExtent, displayPixel))
    {
        return std::unexpected(ContractError::InvalidPixel);
    }

    std::uint32_t const centerIndex = TexelIndex(frame.displayExtent, displayPixel);
    auto const schedule =
        BuildShutterSchedule(images.motion.motionPreviousMinusCurrentUv[centerIndex], frame, settings);
    if (!schedule)
    {
        return std::unexpected(schedule.error());
    }

    MotionBlurResult result{};
    result.schedule = *schedule;
    Rgb const centerColor = images.sceneLinear.pixels[centerIndex];
    double const centerDepth = images.viewDepthMetres.values[centerIndex];
    double const depthTolerance = settings.depthCompareAbsoluteMetres + (settings.depthCompareRelative * centerDepth);

    Rgb accumulated{};
    for (std::uint32_t sample = 0U; sample < schedule->sampleCount; ++sample)
    {
        MotionBlurTap tap{};
        tap.parameter = schedule->parameters[sample];
        tap.offsetPixels = schedule->offsetsPixels[sample];

        PixelCoordinate texel{};
        if (!OffsetTexel(frame.displayExtent, displayPixel, tap.offsetPixels, texel))
        {
            tap.reason = MotionTapReason::OutOfBounds;
            ++result.rejectedOutOfBounds;
            result.taps[sample] = tap;
            continue;
        }
        std::uint32_t const tapIndex = TexelIndex(frame.displayExtent, texel);
        tap.pixel = texel;
        tap.sceneLinear = images.sceneLinear.pixels[tapIndex];
        tap.viewDepthMetres = images.viewDepthMetres.values[tapIndex];

        bool accepted = false;
        if (tap.parameter == 0.0)
        {
            tap.reason = MotionTapReason::CenterSample;
            accepted = true;
        }
        else if (std::abs(tap.viewDepthMetres - centerDepth) <= depthTolerance)
        {
            tap.reason = MotionTapReason::Accepted;
            accepted = true;
        }
        else if (tap.viewDepthMetres < centerDepth)
        {
            // The tap is in front of the centre. It may only contribute if its own shutter trajectory is long
            // enough to have covered this pixel while the shutter was open.
            auto const tapDisplacement =
                MotionToDisplayPixels(images.motion.motionPreviousMinusCurrentUv[tapIndex], frame.displayExtent);
            if (!tapDisplacement)
            {
                return std::unexpected(tapDisplacement.error());
            }
            double tapShutterLength = Length(*tapDisplacement) * frame.exposureTimeFraction;
            if (settings.clampDisplacementToBudget)
            {
                tapShutterLength = std::min(tapShutterLength, settings.maximumDisplacementPixels);
            }
            double const distance = Length(tap.offsetPixels);
            if ((0.5 * tapShutterLength) >= distance)
            {
                tap.reason = MotionTapReason::Accepted;
                accepted = true;
            }
            else
            {
                tap.reason = MotionTapReason::ForegroundNotCovering;
                ++result.rejectedForeground;
            }
        }
        else
        {
            tap.reason = MotionTapReason::BackgroundOccluded;
            ++result.rejectedBackground;
        }

        if (accepted)
        {
            tap.weight = 1.0;
            ++result.acceptedCount;
            result.weightSum += tap.weight;
            accumulated = Add(accumulated, Scale(tap.sceneLinear, tap.weight));
        }
        result.taps[sample] = tap;
    }

    if (result.weightSum > 0.0)
    {
        result.outputSceneLinear = Scale(accumulated, 1.0 / result.weightSum);
    }
    else
    {
        result.outputSceneLinear = centerColor;
        result.fellBackToCenter = true;
    }
    return result;
}

namespace
{

[[nodiscard]] std::expected<void, ContractError> ValidateThinLens(ThinLensCamera const &camera) noexcept
{
    if (!IsFinite(camera.focalLengthMillimetres) || !IsFinite(camera.fNumber) ||
        !IsFinite(camera.focusDistanceMetres) || !IsFinite(camera.sensorHeightMillimetres))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (camera.focalLengthMillimetres <= 0.0 || camera.focalLengthMillimetres > kMaximumFocalLengthMillimetres)
    {
        return std::unexpected(ContractError::InvalidFocalLength);
    }
    if (camera.fNumber <= 0.0 || camera.fNumber > kMaximumFNumber)
    {
        return std::unexpected(ContractError::InvalidAperture);
    }
    if (camera.sensorHeightMillimetres <= 0.0 || camera.sensorHeightMillimetres > kMaximumSensorSizeMillimetres)
    {
        return std::unexpected(ContractError::InvalidSensorSize);
    }
    if (camera.focusDistanceMetres <= 0.0 || camera.focusDistanceMetres > kMaximumViewDepthMetres)
    {
        return std::unexpected(ContractError::InvalidFocusDistance);
    }
    // A thin lens cannot focus at or inside its own focal length: the image distance diverges there.
    if ((camera.focusDistanceMetres * 1'000.0) <= camera.focalLengthMillimetres)
    {
        return std::unexpected(ContractError::InvalidFocusDistance);
    }
    return {};
}

[[nodiscard]] std::expected<void, ContractError> ValidateCocSettings(CircleOfConfusionSettings const &settings) noexcept
{
    if (!IsFinite(settings.maximumRadiusPixels) || !IsFinite(settings.inFocusRadiusPixels))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (settings.maximumRadiusPixels <= 0.0 || settings.maximumRadiusPixels > static_cast<double>(kMaximumDimension))
    {
        return std::unexpected(ContractError::InvalidRadius);
    }
    if (settings.inFocusRadiusPixels < 0.0 || settings.inFocusRadiusPixels > settings.maximumRadiusPixels)
    {
        return std::unexpected(ContractError::InvalidRadius);
    }
    return {};
}

} // namespace

std::expected<CircleOfConfusion, ContractError> ComputeCircleOfConfusion(double viewDepthMetres,
                                                                         ThinLensCamera const &camera,
                                                                         CircleOfConfusionSettings const &settings,
                                                                         Extent2D displayExtent) noexcept
{
    auto const cameraCheck = ValidateThinLens(camera);
    if (!cameraCheck)
    {
        return std::unexpected(cameraCheck.error());
    }
    auto const settingsCheck = ValidateCocSettings(settings);
    if (!settingsCheck)
    {
        return std::unexpected(settingsCheck.error());
    }
    auto const extentCheck = ValidateExtent(displayExtent);
    if (!extentCheck)
    {
        return std::unexpected(extentCheck.error());
    }
    auto const depthCheck = ValidateViewDepth(viewDepthMetres);
    if (!depthCheck)
    {
        return std::unexpected(depthCheck.error());
    }

    double const focalLength = camera.focalLengthMillimetres;
    double const apertureDiameter = focalLength / camera.fNumber;
    double const focusDistance = camera.focusDistanceMetres * 1'000.0;
    double const subjectDistance = viewDepthMetres * 1'000.0;
    double const diameter = (apertureDiameter * focalLength * std::abs(subjectDistance - focusDistance)) /
                            (subjectDistance * (focusDistance - focalLength));
    if (!IsFinite(diameter))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    double const pixelsPerMillimetre = static_cast<double>(displayExtent.height) / camera.sensorHeightMillimetres;
    double const sign = subjectDistance >= focusDistance ? 1.0 : -1.0;
    double const signedRadius = sign * 0.5 * diameter * pixelsPerMillimetre;
    if (!IsFinite(signedRadius))
    {
        return std::unexpected(ContractError::NonFinite);
    }

    CircleOfConfusion coc{};
    coc.apertureDiameterMillimetres = apertureDiameter;
    coc.diameterMillimetres = diameter;
    coc.pixelsPerMillimetre = pixelsPerMillimetre;
    coc.signedRadiusPixels = signedRadius;
    coc.clampedSignedRadiusPixels =
        std::clamp(signedRadius, -settings.maximumRadiusPixels, settings.maximumRadiusPixels);
    coc.clampedByBudget = coc.clampedSignedRadiusPixels != signedRadius;
    if (std::abs(signedRadius) <= settings.inFocusRadiusPixels)
    {
        coc.region = FocusRegion::InFocus;
    }
    else
    {
        coc.region = signedRadius < 0.0 ? FocusRegion::NearField : FocusRegion::FarField;
    }
    return coc;
}

std::expected<double, ContractError> FarFieldLimitRadiusPixels(ThinLensCamera const &camera,
                                                               CircleOfConfusionSettings const &settings,
                                                               Extent2D displayExtent) noexcept
{
    auto const cameraCheck = ValidateThinLens(camera);
    if (!cameraCheck)
    {
        return std::unexpected(cameraCheck.error());
    }
    auto const settingsCheck = ValidateCocSettings(settings);
    if (!settingsCheck)
    {
        return std::unexpected(settingsCheck.error());
    }
    auto const extentCheck = ValidateExtent(displayExtent);
    if (!extentCheck)
    {
        return std::unexpected(extentCheck.error());
    }

    double const focalLength = camera.focalLengthMillimetres;
    double const apertureDiameter = focalLength / camera.fNumber;
    double const focusDistance = camera.focusDistanceMetres * 1'000.0;
    double const diameter = (apertureDiameter * focalLength) / (focusDistance - focalLength);
    double const pixelsPerMillimetre = static_cast<double>(displayExtent.height) / camera.sensorHeightMillimetres;
    double const radius = 0.5 * diameter * pixelsPerMillimetre;
    if (!IsFinite(radius))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return radius;
}

std::expected<Float2, ContractError> MapConcentricDisk(Float2 unitSquareSample) noexcept
{
    if (!IsFinite(unitSquareSample))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (unitSquareSample.x < 0.0 || unitSquareSample.x > 1.0 || unitSquareSample.y < 0.0 || unitSquareSample.y > 1.0)
    {
        return std::unexpected(ContractError::InvalidUnitSample);
    }

    double const a = (2.0 * unitSquareSample.x) - 1.0;
    double const b = (2.0 * unitSquareSample.y) - 1.0;
    if (a == 0.0 && b == 0.0)
    {
        return Float2{.x = 0.0, .y = 0.0};
    }

    constexpr double quarterPi = 0.785398163397448309615660845819875721;
    double radius = 0.0;
    double angle = 0.0;
    if ((a * a) > (b * b))
    {
        radius = a;
        angle = quarterPi * (b / a);
    }
    else
    {
        radius = b;
        angle = (2.0 * quarterPi) - (quarterPi * (a / b));
    }
    return Float2{.x = radius * std::cos(angle), .y = radius * std::sin(angle)};
}

std::expected<std::uint32_t, ContractError> BuildApertureSamples(std::uint32_t sampleCount,
                                                                 std::span<ApertureSample> destination) noexcept
{
    if (sampleCount == 0U || sampleCount > kMaximumApertureSampleCount)
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }
    if (destination.size() < static_cast<std::size_t>(sampleCount))
    {
        return std::unexpected(ContractError::SizeMismatch);
    }

    for (std::uint32_t sample = 0U; sample < sampleCount; ++sample)
    {
        Float2 const unitSquare{.x = (static_cast<double>(sample) + 0.5) / static_cast<double>(sampleCount),
                                .y = RadicalInverseBase2(sample)};
        auto const disk = MapConcentricDisk(unitSquare);
        if (!disk)
        {
            return std::unexpected(disk.error());
        }
        destination[sample] = {.unitSquare = unitSquare, .unitDisk = *disk};
    }
    return sampleCount;
}

std::expected<DepthOfFieldResult, ContractError> EvaluateDepthOfField(
    PixelCoordinate displayPixel, DepthOfFieldImages const &images, PostFrameFacts const &frame,
    DepthOfFieldSettings const &settings, std::span<ApertureSample const> apertureSamples) noexcept
{
    if (images.sceneLinear.extent != frame.displayExtent)
    {
        return std::unexpected(ContractError::SizeMismatch);
    }
    auto const colorCheck = ValidateSceneLinearImage(images.sceneLinear);
    if (!colorCheck)
    {
        return std::unexpected(colorCheck.error());
    }
    auto const depthCheck =
        ValidateDepthImage(images.viewDepthMetres, frame.displayExtent, frame.nearPlaneMetres, frame.farPlaneMetres);
    if (!depthCheck)
    {
        return std::unexpected(depthCheck.error());
    }
    if (!IsInsideExtent(frame.displayExtent, displayPixel))
    {
        return std::unexpected(ContractError::InvalidPixel);
    }
    if (apertureSamples.empty() || apertureSamples.size() > static_cast<std::size_t>(kMaximumApertureSampleCount))
    {
        return std::unexpected(ContractError::InvalidSampleCount);
    }
    for (ApertureSample const &sample : apertureSamples)
    {
        if (!IsFinite(sample.unitSquare) || !IsFinite(sample.unitDisk))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        if (Length(sample.unitDisk) > 1.0 + kUnitDiskTolerance)
        {
            return std::unexpected(ContractError::InvalidUnitSample);
        }
    }
    if (!IsFinite(settings.nearFieldSearchRadiusPixels) || settings.nearFieldSearchRadiusPixels < 0.0 ||
        settings.nearFieldSearchRadiusPixels > static_cast<double>(kMaximumDimension))
    {
        return std::unexpected(ContractError::InvalidRadius);
    }
    if (!IsFinite(settings.minimumFarCoverage) || settings.minimumFarCoverage < 0.0 ||
        settings.minimumFarCoverage > 1.0)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    if (!IsFinite(settings.depthCompareAbsoluteMetres) || !IsFinite(settings.depthCompareRelative) ||
        settings.depthCompareAbsoluteMetres < 0.0 || settings.depthCompareRelative < 0.0)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }

    std::uint32_t const centerIndex = TexelIndex(frame.displayExtent, displayPixel);
    Rgb const centerColor = images.sceneLinear.pixels[centerIndex];
    double const centerDepth = images.viewDepthMetres.values[centerIndex];
    auto const centerCoc = ComputeCircleOfConfusion(centerDepth, settings.camera, settings.coc, frame.displayExtent);
    if (!centerCoc)
    {
        return std::unexpected(centerCoc.error());
    }

    DepthOfFieldResult result{};
    result.centerCoc = *centerCoc;
    result.clampedByBudget = centerCoc->clampedByBudget;
    result.sampleCount = static_cast<std::uint32_t>(apertureSamples.size());
    result.farGatherRadiusPixels = std::abs(centerCoc->clampedSignedRadiusPixels);
    result.nearGatherRadiusPixels = std::min(settings.nearFieldSearchRadiusPixels, settings.coc.maximumRadiusPixels);

    double const depthTolerance = settings.depthCompareAbsoluteMetres + (settings.depthCompareRelative * centerDepth);

    Rgb farAccumulated{};
    Rgb nearAccumulated{};
    for (std::uint32_t sample = 0U; sample < result.sampleCount; ++sample)
    {
        Float2 const disk = apertureSamples[sample].unitDisk;

        DepthOfFieldTap farTap{};
        farTap.offsetPixels = {.x = disk.x * result.farGatherRadiusPixels, .y = disk.y * result.farGatherRadiusPixels};
        PixelCoordinate farTexel{};
        if (!OffsetTexel(frame.displayExtent, displayPixel, farTap.offsetPixels, farTexel))
        {
            farTap.reason = DofTapReason::OutOfBounds;
            ++result.rejectedOutOfBounds;
        }
        else
        {
            std::uint32_t const tapIndex = TexelIndex(frame.displayExtent, farTexel);
            farTap.pixel = farTexel;
            farTap.sceneLinear = images.sceneLinear.pixels[tapIndex];
            farTap.viewDepthMetres = images.viewDepthMetres.values[tapIndex];
            auto const tapCoc =
                ComputeCircleOfConfusion(farTap.viewDepthMetres, settings.camera, settings.coc, frame.displayExtent);
            if (!tapCoc)
            {
                return std::unexpected(tapCoc.error());
            }
            farTap.signedRadiusPixels = tapCoc->clampedSignedRadiusPixels;
            double const distance = Length(farTap.offsetPixels);
            double const scatterRadius = std::max(std::abs(farTap.signedRadiusPixels), kMinimumScatterRadiusPixels);
            if (distance == 0.0)
            {
                farTap.reason = DofTapReason::CenterSample;
                farTap.weight = 1.0 / (scatterRadius * scatterRadius);
            }
            else if (tapCoc->region == FocusRegion::NearField && farTap.viewDepthMetres < centerDepth - depthTolerance)
            {
                // Owned by the near-field pass, which composites it with coverage. Every other tap, including a
                // defocused far-field surface that happens to be nearer than the centre, stays in the far field and
                // is judged only by whether its own circle of confusion reaches this pixel.
                farTap.reason = DofTapReason::OwnedByNearField;
                ++result.rejectedOwnedByNearField;
            }
            else if (std::abs(farTap.signedRadiusPixels) < distance)
            {
                farTap.reason = DofTapReason::OutsideCircleOfConfusion;
                ++result.rejectedOutsideCoc;
            }
            else
            {
                farTap.reason = DofTapReason::AcceptedFar;
                farTap.weight = 1.0 / (scatterRadius * scatterRadius);
            }
            if (farTap.weight > 0.0)
            {
                ++result.farAcceptedCount;
                result.farWeightSum += farTap.weight;
                farAccumulated = Add(farAccumulated, Scale(farTap.sceneLinear, farTap.weight));
            }
        }
        result.farTaps[sample] = farTap;

        DepthOfFieldTap nearTap{};
        nearTap.offsetPixels = {.x = disk.x * result.nearGatherRadiusPixels,
                                .y = disk.y * result.nearGatherRadiusPixels};
        PixelCoordinate nearTexel{};
        if (!OffsetTexel(frame.displayExtent, displayPixel, nearTap.offsetPixels, nearTexel))
        {
            nearTap.reason = DofTapReason::OutOfBounds;
        }
        else
        {
            std::uint32_t const tapIndex = TexelIndex(frame.displayExtent, nearTexel);
            nearTap.pixel = nearTexel;
            nearTap.sceneLinear = images.sceneLinear.pixels[tapIndex];
            nearTap.viewDepthMetres = images.viewDepthMetres.values[tapIndex];
            auto const tapCoc =
                ComputeCircleOfConfusion(nearTap.viewDepthMetres, settings.camera, settings.coc, frame.displayExtent);
            if (!tapCoc)
            {
                return std::unexpected(tapCoc.error());
            }
            nearTap.signedRadiusPixels = tapCoc->clampedSignedRadiusPixels;
            double const distance = Length(nearTap.offsetPixels);
            double const scatterRadius = std::max(std::abs(nearTap.signedRadiusPixels), kMinimumScatterRadiusPixels);
            bool const isNearField = tapCoc->region == FocusRegion::NearField;
            bool const isInFront = nearTap.viewDepthMetres < centerDepth - depthTolerance;
            if (!isNearField || !isInFront)
            {
                nearTap.reason = DofTapReason::NotNearField;
            }
            else if (std::abs(nearTap.signedRadiusPixels) < distance)
            {
                nearTap.reason = DofTapReason::OutsideCircleOfConfusion;
            }
            else
            {
                nearTap.reason = DofTapReason::AcceptedNear;
                nearTap.weight = 1.0 / (scatterRadius * scatterRadius);
                ++result.nearAcceptedCount;
                result.nearWeightSum += nearTap.weight;
                nearAccumulated = Add(nearAccumulated, Scale(nearTap.sceneLinear, nearTap.weight));
            }
        }
        result.nearTaps[sample] = nearTap;
    }

    double const farCoverage = static_cast<double>(result.farAcceptedCount) / static_cast<double>(result.sampleCount);
    if (result.farWeightSum > 0.0)
    {
        result.farFieldSceneLinear = Scale(farAccumulated, 1.0 / result.farWeightSum);
    }
    else
    {
        result.farFieldSceneLinear = centerColor;
    }
    if (farCoverage < settings.minimumFarCoverage || result.farWeightSum <= 0.0)
    {
        result.insufficientFarCoverage = true;
        result.farFieldSceneLinear = centerColor;
    }

    result.nearCoverage = static_cast<double>(result.nearAcceptedCount) / static_cast<double>(result.sampleCount);
    if (result.nearWeightSum > 0.0)
    {
        result.nearFieldSceneLinear = Scale(nearAccumulated, 1.0 / result.nearWeightSum);
    }

    result.outputSceneLinear = Add(Scale(result.nearFieldSceneLinear, result.nearCoverage),
                                   Scale(result.farFieldSceneLinear, 1.0 - result.nearCoverage));
    return result;
}

std::expected<CompositionResult, ContractError> ComposeSceneLinear(CompositionInput const &input,
                                                                   CompositionSettings const &settings) noexcept
{
    auto const baseCheck = ValidateRadiance(input.baseSceneLinear);
    if (!baseCheck)
    {
        return std::unexpected(baseCheck.error());
    }
    auto const bloomCheck = ValidateRadiance(input.bloomSceneLinear);
    if (!bloomCheck)
    {
        return std::unexpected(bloomCheck.error());
    }
    if (!IsFinite(input.coverageAlpha))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (input.coverageAlpha < 0.0 || input.coverageAlpha > 1.0)
    {
        return std::unexpected(ContractError::InvalidAlpha);
    }
    if (!IsFinite(settings.bloomIntensity) || settings.bloomIntensity < 0.0)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    if (settings.mode == BloomCompositionMode::CreativeCrossfade && settings.bloomIntensity > 1.0)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }
    if (settings.mode == BloomCompositionMode::AdditiveContribution &&
        settings.bloomIntensity > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }

    CompositionResult result{};
    result.coverageAlpha = input.coverageAlpha;
    result.bloomContribution = Scale(input.bloomSceneLinear, settings.bloomIntensity);
    result.baseLuminance = SceneLinearLuminanceUnchecked(input.baseSceneLinear);
    result.addedBloomLuminance = SceneLinearLuminanceUnchecked(result.bloomContribution);
    if (settings.mode == BloomCompositionMode::AdditiveContribution)
    {
        result.baseFraction = 1.0;
        result.bloomFraction = settings.bloomIntensity;
        result.sceneLinear = Add(input.baseSceneLinear, result.bloomContribution);
        result.discardedBaseLuminance = 0.0;
        result.preservesBaseRadiance = true;
    }
    else
    {
        // The crossfade attenuates the base by the same fraction it mixes bloom in. Since the bloom image carries
        // only the extracted excess, that attenuation is a loss, not a redistribution.
        result.baseFraction = 1.0 - settings.bloomIntensity;
        result.bloomFraction = settings.bloomIntensity;
        result.sceneLinear = Add(Scale(input.baseSceneLinear, result.baseFraction), result.bloomContribution);
        result.discardedBaseLuminance = result.baseLuminance * settings.bloomIntensity;
        result.preservesBaseRadiance = settings.bloomIntensity == 0.0;
    }
    auto const composedCheck = ValidateRadiance(result.sceneLinear);
    if (!composedCheck)
    {
        return std::unexpected(composedCheck.error());
    }
    result.outputLuminance = SceneLinearLuminanceUnchecked(result.sceneLinear);
    result.premultipliedSceneLinear = Scale(result.sceneLinear, input.coverageAlpha);
    return result;
}

std::expected<ExposureResult, ContractError> ApplyExposure(ExposureInput const &input) noexcept
{
    auto const radianceCheck = ValidateRadiance(input.preExposedSceneLinear);
    if (!radianceCheck)
    {
        return std::unexpected(radianceCheck.error());
    }
    if (!IsFinite(input.preExposure) || !IsFinite(input.exposureScale))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (input.preExposure <= 0.0 || input.preExposure > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidPreExposure);
    }
    if (input.exposureScale <= 0.0 || input.exposureScale > kMaximumSceneLinearValue)
    {
        return std::unexpected(ContractError::InvalidExposure);
    }

    ExposureResult result{};
    result.absoluteSceneLinear = Scale(input.preExposedSceneLinear, 1.0 / input.preExposure);
    result.appliedScale = input.exposureScale / input.preExposure;
    result.exposedSceneLinear = Scale(result.absoluteSceneLinear, input.exposureScale);
    auto const absoluteCheck = ValidateRadiance(result.absoluteSceneLinear);
    if (!absoluteCheck)
    {
        return std::unexpected(absoluteCheck.error());
    }
    auto const exposedCheck = ValidateRadiance(result.exposedSceneLinear);
    if (!exposedCheck)
    {
        return std::unexpected(exposedCheck.error());
    }
    return result;
}

std::expected<Rgb, ContractError> EncodeDisplay(Rgb displayLinear, TransferFunction transfer) noexcept
{
    if (!IsFinite(displayLinear))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (displayLinear.r < 0.0 || displayLinear.g < 0.0 || displayLinear.b < 0.0)
    {
        return std::unexpected(ContractError::NegativeRadiance);
    }
    if (displayLinear.r > 1.0 || displayLinear.g > 1.0 || displayLinear.b > 1.0)
    {
        return std::unexpected(ContractError::InvalidRadiance);
    }
    if (transfer != TransferFunction::Linear && transfer != TransferFunction::Srgb &&
        transfer != TransferFunction::Gamma22)
    {
        return std::unexpected(ContractError::InvalidSettings);
    }

    auto const encodeChannel = [transfer](double value)
    {
        switch (transfer)
        {
        case TransferFunction::Linear:
            return value;
        case TransferFunction::Srgb:
            return value <= 0.0031308 ? 12.92 * value : (1.055 * std::pow(value, 1.0 / 2.4)) - 0.055;
        case TransferFunction::Gamma22:
            return std::pow(value, 1.0 / 2.2);
        }
        return value;
    };

    return Rgb{
        .r = encodeChannel(displayLinear.r), .g = encodeChannel(displayLinear.g), .b = encodeChannel(displayLinear.b)};
}

std::expected<Rgb, ContractError> CompositeUi(Rgb destination, UiLayer const &ui) noexcept
{
    if (!IsFinite(destination) || !IsFinite(ui.color) || !IsFinite(ui.alpha))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (destination.r < 0.0 || destination.g < 0.0 || destination.b < 0.0 || ui.color.r < 0.0 || ui.color.g < 0.0 ||
        ui.color.b < 0.0)
    {
        return std::unexpected(ContractError::NegativeRadiance);
    }
    if (destination.r > 1.0 || destination.g > 1.0 || destination.b > 1.0 || ui.color.r > 1.0 || ui.color.g > 1.0 ||
        ui.color.b > 1.0)
    {
        return std::unexpected(ContractError::InvalidRadiance);
    }
    if (ui.alpha < 0.0 || ui.alpha > 1.0)
    {
        return std::unexpected(ContractError::InvalidAlpha);
    }
    return Add(Scale(ui.color, ui.alpha), Scale(destination, 1.0 - ui.alpha));
}

std::expected<SceneLinearStatistics, ContractError> SummarizeSceneLinear(SceneLinearImageView image) noexcept
{
    auto const imageCheck = ValidateSceneLinearImage(image);
    if (!imageCheck)
    {
        return std::unexpected(imageCheck.error());
    }

    SceneLinearStatistics statistics{};
    statistics.pixelCount = static_cast<std::uint32_t>(image.pixels.size());
    statistics.minimumChannel = std::numeric_limits<double>::infinity();
    statistics.maximumChannel = -std::numeric_limits<double>::infinity();
    statistics.minimumLuminance = std::numeric_limits<double>::infinity();
    statistics.maximumLuminance = -std::numeric_limits<double>::infinity();
    for (Rgb const pixel : image.pixels)
    {
        statistics.minimumChannel = std::min({statistics.minimumChannel, pixel.r, pixel.g, pixel.b});
        statistics.maximumChannel = std::max({statistics.maximumChannel, pixel.r, pixel.g, pixel.b});
        double const luminance = SceneLinearLuminanceUnchecked(pixel);
        statistics.minimumLuminance = std::min(statistics.minimumLuminance, luminance);
        statistics.maximumLuminance = std::max(statistics.maximumLuminance, luminance);
        statistics.luminanceSum += luminance;
    }
    statistics.meanLuminance = statistics.luminanceSum / static_cast<double>(statistics.pixelCount);
    return statistics;
}

} // namespace ch30::post_processing
