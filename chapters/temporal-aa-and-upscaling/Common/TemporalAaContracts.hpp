#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>

namespace ch28::temporal_aa
{

// Phase 1 is a deterministic reference model, not a production upscaler. It
// deliberately leaves velocity dilation, lock status, responsive AA,
// anti-flicker policy, and ML reconstruction to later implementation choices.
inline constexpr std::uint32_t kMaximumDimension = 16'384U;
inline constexpr std::uint32_t kMaximumJitterPhasePeriod = 64U;
inline constexpr std::uint32_t kMaximumNeighborhoodSampleCount = 25U;
inline constexpr std::uint32_t kMaximumReconstructionTapCount = 16U;
inline constexpr std::uint32_t kMaximumHistorySampleCount = 1'024U;
inline constexpr double kMaximumSceneLinearValue = 1.0e6;

enum class ContractError : std::uint8_t
{
    NonFinite,
    InvalidDimensions,
    DimensionsTooLarge,
    CountOverflow,
    PixelOutOfBounds,
    InvalidPhasePeriod,
    InvalidUv,
    HistoryFootprintOutOfBounds,
    InvalidDepth,
    InvalidNormal,
    InvalidMask,
    InvalidExposure,
    InvalidThreshold,
    InvalidColor,
    ColorOutOfRange,
    EmptyNeighborhood,
    TooManyNeighborhoodSamples,
    InvalidStatistics,
    InvalidFeedback,
    InvalidSampleCount,
    InvalidKernel,
    InvalidKernelRadius,
    SizeMismatch,
    InvalidSharpening,
    ArithmeticOverflow,
};

struct Float2 final
{
    double x{};
    double y{};

    [[nodiscard]] bool operator==(Float2 const &) const noexcept = default;
};

struct Float3 final
{
    double x{};
    double y{};
    double z{};

    [[nodiscard]] bool operator==(Float3 const &) const noexcept = default;
};

struct Rgb final
{
    double r{};
    double g{};
    double b{};

    [[nodiscard]] bool operator==(Rgb const &) const noexcept = default;
};

struct Extent2D final
{
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] bool operator==(Extent2D const &) const noexcept = default;
};

struct PixelCoordinate final
{
    std::uint32_t x{};
    std::uint32_t y{};

    [[nodiscard]] bool operator==(PixelCoordinate const &) const noexcept = default;
};

// Jitter is measured in render pixels. DirectX NDC uses +Y upward, so a
// positive raster-space Y offset becomes a negative NDC Y offset.
struct JitterSample final
{
    std::uint32_t phaseIndex{};
    std::uint32_t phasePeriod{};
    Float2 pixelOffset{};
    Float2 ndcOffset{};

    [[nodiscard]] bool operator==(JitterSample const &) const noexcept = default;
};

struct DisplayToRenderMapping final
{
    PixelCoordinate displayPixel{};
    Float2 displayCenterUv{};
    Float2 renderCenterTexel{};
    Float2 renderFootprintMinimumTexel{};
    Float2 renderFootprintMaximumTexel{};
    Float2 renderTexelsPerDisplayPixel{};

    [[nodiscard]] bool operator==(DisplayToRenderMapping const &) const noexcept = default;
};

// The prerequisite chapter stores motion as previousUV-currentUV. Motion is
// unjittered. Reprojection starts from the current jittered raster sample and
// adds previousJitter-currentJitter exactly once.
struct ReprojectionInput final
{
    Float2 currentJitteredUv{};
    Float2 motionPreviousMinusCurrentUv{};
    Float2 currentJitterUv{};
    Float2 previousJitterUv{};
};

struct ReprojectionResult final
{
    Float2 currentJitteredUv{};
    Float2 currentUnjitteredUv{};
    Float2 motionPreviousMinusCurrentUv{};
    Float2 jitterDeltaPreviousMinusCurrentUv{};
    Float2 previousHistoryUv{};

    [[nodiscard]] bool operator==(ReprojectionResult const &) const noexcept = default;
};

struct BilinearTap final
{
    PixelCoordinate pixel{};
    double weight{};

    [[nodiscard]] bool operator==(BilinearTap const &) const noexcept = default;
};

struct BilinearFootprint final
{
    Float2 historyUv{};
    Float2 texelCoordinate{};
    std::array<BilinearTap, 4U> taps{};

    [[nodiscard]] bool operator==(BilinearFootprint const &) const noexcept = default;
};

enum class HistoryRejectReason : std::uint32_t
{
    None = 0U,
    NoHistory = 1U << 0U,
    Reset = 1U << 1U,
    FootprintOutOfBounds = 1U << 2U,
    DepthMismatch = 1U << 3U,
    NormalMismatch = 1U << 4U,
    ObjectMismatch = 1U << 5U,
    Reactive = 1U << 6U,
    Disocclusion = 1U << 7U,
    Exposure = 1U << 8U,
    LuminanceChange = 1U << 9U,
};

[[nodiscard]] constexpr HistoryRejectReason operator|(HistoryRejectReason left, HistoryRejectReason right) noexcept
{
    return static_cast<HistoryRejectReason>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr HistoryRejectReason &operator|=(HistoryRejectReason &left, HistoryRejectReason right) noexcept
{
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool HasReason(HistoryRejectReason reasons, HistoryRejectReason reason) noexcept
{
    return (static_cast<std::uint32_t>(reasons) & static_cast<std::uint32_t>(reason)) != 0U;
}

struct HistoryValidationSettings final
{
    double absoluteDepthThreshold{0.001};
    double relativeDepthThreshold{0.005};
    double minimumNormalCosine{0.8};
    double reactiveRejectThreshold{0.95};
    double disocclusionRejectThreshold{0.5};
    double maximumExposureRatio{4.0};
    double maximumLuminanceRatio{8.0};
    double luminanceEpsilon{1.0e-4};
};

struct HistoryValidationInput final
{
    bool hasHistory{};
    bool resetRequested{};
    Extent2D historyExtent{};
    Float2 previousHistoryUv{};
    double expectedPreviousViewDepth{};
    double sampledPreviousViewDepth{};
    Float3 currentNormal{};
    Float3 sampledPreviousNormal{};
    std::uint32_t currentObjectId{};
    std::uint32_t sampledPreviousObjectId{};
    double reactiveMask{};
    double disocclusionEvidence{};
    double currentPreExposure{1.0};
    double previousPreExposure{1.0};
    double currentLuminance{};
    double sampledHistoryLuminance{};
};

struct HistoryValidationResult final
{
    HistoryRejectReason reasons{HistoryRejectReason::None};
    double depthTolerance{};
    double normalAgreement{1.0};
    double historyToCurrentExposureScale{1.0};
    double exposureRatio{1.0};
    double luminanceRatio{1.0};

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return reasons == HistoryRejectReason::None;
    }
};

// All colors used by the temporal contracts are scene-linear RGB. YCoCg is
// used only as a reversible statistics space; resolve never blends gamma-coded
// values.
struct YCoCg final
{
    double y{};
    double co{};
    double cg{};

    [[nodiscard]] bool operator==(YCoCg const &) const noexcept = default;
};

struct NeighborhoodStatistics final
{
    std::uint32_t sampleCount{};
    Rgb rgbMinimum{};
    Rgb rgbMaximum{};
    YCoCg mean{};
    YCoCg variance{};

    [[nodiscard]] bool operator==(NeighborhoodStatistics const &) const noexcept = default;
};

struct NeighborhoodConstraintSettings final
{
    // This bounded teaching heuristic line-clips history to a variance box in
    // scene-linear YCoCg, then clamps to the scene-linear RGB neighborhood
    // box. It is one inspectable choice, not a claim of universal optimality.
    double luminanceSigmaScale{1.5};
    double chromaSigmaScale{1.0};
};

struct NeighborhoodConstraintResult final
{
    Rgb inputHistory{};
    Rgb varianceClippedHistory{};
    Rgb constrainedHistory{};
    double varianceClipScale{1.0};
    bool rgbBoxClamped{};

    [[nodiscard]] bool operator==(NeighborhoodConstraintResult const &) const noexcept = default;
};

struct FeedbackSettings final
{
    double minimumFeedback{0.05};
    double maximumFeedback{0.95};
    double motionForZeroFeedbackUv{0.1};
    double reactiveInfluence{1.0};
    double disocclusionInfluence{1.0};
    std::uint32_t fullConfidenceSampleCount{16U};
    std::uint32_t maximumSampleCount{64U};
};

struct FeedbackInput final
{
    HistoryRejectReason rejectionReasons{HistoryRejectReason::None};
    Float2 motionPreviousMinusCurrentUv{};
    double reactiveMask{};
    double disocclusionEvidence{};
    std::uint32_t previousSampleCount{};
};

struct FeedbackResult final
{
    double validityFactor{};
    double sampleCountFactor{};
    double motionFactor{};
    double reactiveFactor{};
    double disocclusionFactor{};
    double unconstrainedFeedback{};
    double historyFeedback{};

    [[nodiscard]] bool operator==(FeedbackResult const &) const noexcept = default;
};

struct TemporalResolveInput final
{
    Rgb currentSceneLinear{};
    Rgb historySceneLinearPreviousExposure{};
    NeighborhoodStatistics neighborhood{};
    HistoryValidationResult validation{};
    Float2 motionPreviousMinusCurrentUv{};
    double reactiveMask{};
    double disocclusionEvidence{};
    std::uint32_t previousSampleCount{};
};

struct TemporalResolveResult final
{
    Rgb exposureAdjustedHistory{};
    NeighborhoodConstraintResult constraint{};
    FeedbackResult feedback{};
    HistoryRejectReason rejectionReasons{HistoryRejectReason::None};
    Rgb outputSceneLinear{};
    std::uint32_t nextSampleCount{};
};

enum class ReconstructionKernel : std::uint8_t
{
    Bilinear,
    CatmullRom,
};

struct ReconstructionTap final
{
    PixelCoordinate pixel{};
    Float2 offsetFromCenterTexel{};
    double weight{};

    [[nodiscard]] bool operator==(ReconstructionTap const &) const noexcept = default;
};

struct ReconstructionFootprint final
{
    ReconstructionKernel kernel{ReconstructionKernel::Bilinear};
    std::uint32_t tapCount{};
    std::array<ReconstructionTap, kMaximumReconstructionTapCount> taps{};
    bool hasNegativeWeights{};

    [[nodiscard]] bool operator==(ReconstructionFootprint const &) const noexcept = default;
};

struct SceneLinearImageView final
{
    Extent2D extent{};
    std::span<Rgb const> pixels{};
};

struct SharpeningSettings final
{
    double strength{0.2};
    double negativeLobeLimit{0.0};
    double overshootLimit{0.05};
};

struct SharpeningInput final
{
    Rgb resolvedSceneLinear{};
    Rgb neighborAverageSceneLinear{};
    Rgb neighborhoodMinimumSceneLinear{};
    Rgb neighborhoodMaximumSceneLinear{};
};

struct SharpeningResult final
{
    Rgb requestedDetail{};
    Rgb appliedDetail{};
    Rgb outputSceneLinear{};
};

[[nodiscard]] std::expected<std::uint32_t, ContractError> PixelCount(Extent2D extent) noexcept;
[[nodiscard]] std::expected<Float2, ContractError> PixelJitterToNdc(Float2 pixelOffset, Extent2D renderExtent) noexcept;
[[nodiscard]] std::expected<JitterSample, ContractError> GenerateJitter(std::uint64_t frameIndex,
                                                                        std::uint32_t phasePeriod,
                                                                        Extent2D renderExtent) noexcept;
[[nodiscard]] std::expected<DisplayToRenderMapping, ContractError> MapDisplayPixelToRender(
    PixelCoordinate displayPixel, Extent2D displayExtent, Extent2D renderExtent) noexcept;
[[nodiscard]] std::expected<ReprojectionResult, ContractError> ReprojectToHistory(
    ReprojectionInput const &input) noexcept;
[[nodiscard]] std::expected<BilinearFootprint, ContractError> BuildBilinearHistoryFootprint(
    Float2 historyUv, Extent2D historyExtent) noexcept;
[[nodiscard]] std::expected<HistoryValidationResult, ContractError> ValidateHistory(
    HistoryValidationInput const &input, HistoryValidationSettings const &settings) noexcept;

[[nodiscard]] std::expected<YCoCg, ContractError> SceneLinearRgbToYCoCg(Rgb color) noexcept;
[[nodiscard]] std::expected<Rgb, ContractError> YCoCgToSceneLinearRgb(YCoCg color) noexcept;
[[nodiscard]] std::expected<NeighborhoodStatistics, ContractError> ComputeNeighborhoodStatistics(
    std::span<Rgb const> sceneLinearSamples) noexcept;
[[nodiscard]] std::expected<NeighborhoodConstraintResult, ContractError> ConstrainHistory(
    Rgb historySceneLinear, NeighborhoodStatistics const &statistics,
    NeighborhoodConstraintSettings const &settings) noexcept;

[[nodiscard]] std::expected<FeedbackResult, ContractError> ComputeHistoryFeedback(
    FeedbackInput const &input, FeedbackSettings const &settings) noexcept;
[[nodiscard]] std::expected<TemporalResolveResult, ContractError> ResolveTemporal(
    TemporalResolveInput const &input, NeighborhoodConstraintSettings const &constraintSettings,
    FeedbackSettings const &feedbackSettings) noexcept;

// Bilinear has radius 1 and no negative weights. Catmull-Rom has radius 2,
// four taps per axis, and explicit negative lobes. Clamped edge indices are
// returned as duplicate taps; weights remain visible and sum to one. The
// reconstructed result is clamped to the valid scene-linear signal domain so
// Catmull-Rom ringing cannot feed negative or overflowing history downstream.
[[nodiscard]] std::expected<ReconstructionFootprint, ContractError> BuildReconstructionFootprint(
    DisplayToRenderMapping const &mapping, Extent2D renderExtent, ReconstructionKernel kernel,
    double kernelRadius) noexcept;
[[nodiscard]] std::expected<Rgb, ContractError> ReconstructSceneLinear(
    SceneLinearImageView image, ReconstructionFootprint const &footprint) noexcept;

// Sharpening is an explicit downstream stage. The requested unsharp-mask
// detail is bounded by the local range, negative-lobe limit, and overshoot
// limit; it does not alter history validity or confidence.
[[nodiscard]] std::expected<SharpeningResult, ContractError> SharpenResolved(
    SharpeningInput const &input, SharpeningSettings const &settings) noexcept;

} // namespace ch28::temporal_aa
