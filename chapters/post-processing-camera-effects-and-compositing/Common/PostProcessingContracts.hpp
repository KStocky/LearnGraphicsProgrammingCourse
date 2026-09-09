#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>

// Chapter 30 teaching contracts for post-processing camera effects and final compositing.
//
// Scope and honesty rules for this phase:
//
//   * Everything here is a deterministic CPU reference model of the parts of a post-processing stack that are
//     normally invisible inside a chain of full-screen passes: which stage may run where, which signal domain each
//     stage consumes and produces, how highlights are extracted in radiance units, how a bounded pyramid is built on
//     odd extents, how a shutter is integrated along a signed pixel displacement, and how a thin lens turns a view
//     depth into a signed circle of confusion.
//   * There is no universal post-processing order. Real engines disagree about whether bloom sees the motion-blurred
//     and defocused image, whether depth of field precedes motion blur, and whether the UI is composited before or
//     after display encoding. This chapter therefore takes a declared PipelinePolicy and validates a submitted order
//     against it, instead of pretending one sequence is correct everywhere. What is *not* negotiable is the signal
//     domain: scene-linear filtering never runs on tone-mapped or transfer-encoded values, exposure is applied
//     exactly once, temporal reconstruction never consumes an already camera-blurred image, and nothing filters the
//     UI after it has been composited.
//   * Physical units are explicit. Radiance and luminance are scene-linear and pre-exposure is a separate, recorded
//     scale rather than an assumption. Shutter integration is expressed in signed display pixels and a
//     dimensionless exposure-time fraction. Lens quantities are millimetres on the sensor and metres in the scene,
//     converted in exactly one place.
//   * The reference computes in double so that a test can state an exact expectation, but every formula is
//     arranged to survive single precision on the GPU: the intermediates stay far inside the float range, texel
//     addressing is integer, and no expression subtracts two nearly equal large quantities. The one quantity that
//     genuinely needs care in half precision is stored radiance itself, which is what the frame pre-exposure
//     exists to keep in range.
//
// Conventions inherited from the prerequisite chapters:
//
//   * Motion is stored as previousUV - currentUV, unjittered, exactly as Chapter 11 produces it and Chapter 28
//     consumes it. Forward motion, where the surface moved to during this frame, is therefore the negation of the
//     stored vector, and that conversion happens once in MotionToDisplayPixels.
//   * Colour is scene-linear RGB with Rec.709 primaries. Nothing in this chapter blends transfer-encoded values.
//   * Depth used for classification is linear view depth in metres, positive in front of the eye, never raw device
//     depth. Device depth is non-linear, so a difference of device depths is not a distance.
//   * Texture UV has its origin at the top-left texel and pixel (x, y) samples at ((x + 0.5) / width,
//     (y + 0.5) / height).
//
// Deliberately out of scope, and not approximated here: a physically complete lens model with elements, distortion,
// vignetting, aberration, and cat-eye bokeh occlusion; rolling shutter; chromatic aberration; sub-frame object
// transforms; cinematic ground truth; tile and neighbour-max acceleration structures; vendor upscalers; frame
// generation; tone-curve design, which Chapter 4 owns; and UI framework design.

namespace ch30::post_processing
{

inline constexpr std::uint32_t kMaximumDimension = 16'384U;
// The image-level references exist to pin footprints and energy on small hand-checkable pictures, so they are
// bounded well below a real render target. A production pass obeys the same contract at a much larger extent.
inline constexpr std::uint32_t kMaximumReferenceDimension = 512U;
inline constexpr std::uint32_t kMaximumReferencePixelCount = 262'144U;
inline constexpr std::uint32_t kMaximumBloomLevelCount = 12U;
inline constexpr std::uint32_t kMaximumMotionBlurSampleCount = 32U;
inline constexpr std::uint32_t kMaximumApertureSampleCount = 64U;
inline constexpr std::uint32_t kMaximumStageCount = 16U;
inline constexpr double kMaximumSceneLinearValue = 1.0e6;
inline constexpr double kMaximumViewDepthMetres = 1.0e6;
inline constexpr double kMaximumFocalLengthMillimetres = 1.0e4;
inline constexpr double kMaximumSensorSizeMillimetres = 1.0e3;
// A tap whose circle of confusion is smaller than this is treated as a point deposit of radiance one pixel across.
// Without the floor the scatter-as-gather weight 1 / radius^2 would diverge for a perfectly focused tap.
inline constexpr double kMinimumScatterRadiusPixels = 0.5;

enum class ContractError : std::uint8_t
{
    NonFinite,
    InvalidExtent,
    ExtentTooLarge,
    SizeMismatch,
    InvalidPixel,
    InvalidRadiance,
    NegativeRadiance,
    InvalidAlpha,
    InvalidExposure,
    InvalidPreExposure,
    InvalidThreshold,
    InvalidKnee,
    InvalidLevelCount,
    InvalidWeight,
    InvalidSampleCount,
    InvalidMotion,
    InvalidDepth,
    NonPositiveViewDepth,
    InvalidDepthRange,
    InvalidShutter,
    InvalidJitter,
    InvalidRadius,
    InvalidFocalLength,
    InvalidAperture,
    InvalidFocusDistance,
    InvalidSensorSize,
    InvalidUnitSample,
    InvalidSettings,
    InvalidStage,
    EmptyPipeline,
    TooManyStages,
};

struct Float2 final
{
    double x{};
    double y{};

    [[nodiscard]] bool operator==(Float2 const &) const noexcept = default;
};

// Scene-linear RGB with Rec.709 primaries.
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

[[nodiscard]] std::expected<std::uint32_t, ContractError> PixelCount(Extent2D extent) noexcept;
// Rec.709 luminance of a scene-linear colour, in the same radiometric unit as the colour itself.
[[nodiscard]] std::expected<double, ContractError> SceneLinearLuminance(Rgb color) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// A. Pipeline order and signal domains
// ---------------------------------------------------------------------------------------------------------------

// The signal domain is the part of the contract that is not a matter of taste. A stage that filters, averages, or
// integrates radiance is only meaningful in a scene-linear domain; a stage that writes to a swap chain is only
// meaningful after the transfer encoding.
enum class SignalDomain : std::uint8_t
{
    // Render extent, jittered raster samples, scene-linear radiance multiplied by the frame pre-exposure.
    SceneLinearRenderJittered = 0U,
    // Display extent, temporally resolved or upscaled, still scene-linear and still pre-exposed.
    SceneLinearDisplay,
    // Display extent, pre-exposure removed and camera exposure applied. This is the tone curve's input.
    SceneLinearExposed,
    // Display-referred linear values in [0, 1] produced by the tone curve.
    DisplayLinear,
    // Transfer-encoded values ready for presentation. Filtering here averages code values, not light.
    DisplayEncoded,
};

enum class PostStage : std::uint8_t
{
    TemporalResolve = 0U,
    DepthOfField,
    MotionBlur,
    Bloom,
    Exposure,
    ToneMap,
    DisplayEncode,
    UiComposite,
};

inline constexpr std::uint32_t kPostStageCount = 8U;

// Depth of field then motion blur models a camera that focuses light and then smears the focused image across the
// open shutter, so defocused highlights stay round before they are dragged. Motion blur then depth of field is
// easier to combine with a velocity-tiled pass and keeps fast movement from turning bokeh into streaks. Both are
// approximations of one integral over the aperture and the shutter, so the chapter validates the declared choice
// instead of asserting a winner.
enum class CameraEffectOrder : std::uint8_t
{
    DefocusThenShutter = 0U,
    ShutterThenDefocus,
};

// Bloom after the camera effects lets a defocused or smeared highlight glow with the shape it actually has on
// screen, at the cost of running the pyramid over an already blurred image. Bloom before them keeps the extraction
// sharp and stable under motion, but a bokeh disc then glows as if it were still a point.
enum class BloomSourcePlacement : std::uint8_t
{
    AfterCameraEffects = 0U,
    BeforeCameraEffects,
};

// UI after display encoding is the ordinary SDR choice: the UI is authored in the display encoding and composited
// with straight alpha. UI in display-linear before encoding is what an HDR UI needs, so that its brightness is
// defined in the same linear domain as the scene, at the cost of authoring in a space artists do not preview.
enum class UiCompositePlacement : std::uint8_t
{
    AfterDisplayEncode = 0U,
    BeforeDisplayEncodeLinear,
};

struct PipelinePolicy final
{
    CameraEffectOrder cameraEffectOrder{CameraEffectOrder::DefocusThenShutter};
    BloomSourcePlacement bloomSource{BloomSourcePlacement::AfterCameraEffects};
    UiCompositePlacement uiPlacement{UiCompositePlacement::AfterDisplayEncode};
    bool requireTemporalResolve{true};
    bool requireUiComposite{false};

    [[nodiscard]] bool operator==(PipelinePolicy const &) const noexcept = default;
};

// Every way a submitted order can contradict the declared policy or the signal domains. The reasons are flags, not
// a single first error: one wrong placement usually breaks more than one rule, and reporting only the first would
// teach the learner to fix an order one symptom at a time.
enum class OrderViolation : std::uint32_t
{
    None = 0U,
    DuplicateStage = 1U << 0U,
    MissingTemporalResolve = 1U << 1U,
    MissingExposure = 1U << 2U,
    MissingToneMap = 1U << 3U,
    MissingDisplayEncode = 1U << 4U,
    MissingUiComposite = 1U << 5U,
    // Filtering after the transfer encoding averages code values instead of light.
    FilterAfterDisplayEncode = 1U << 6U,
    // Filtering after the tone curve filters a compressed signal, so a bright neighbour no longer carries its own
    // radiance into the average.
    FilterAfterToneMap = 1U << 7U,
    StageAfterUiComposite = 1U << 8U,
    ExposureAfterToneMap = 1U << 9U,
    DisplayEncodeBeforeToneMap = 1U << 10U,
    // A camera effect placed after exposure integrates a signal that already carries the camera exposure, which is
    // how a stack ends up applying exposure twice to the blurred part of the image.
    CameraEffectAfterExposure = 1U << 11U,
    // Temporal reconstruction after a camera effect feeds an already filtered image back into the history, which
    // both destroys the jittered subpixel signal and filters the same frame temporally twice.
    TemporalResolveAfterCameraEffect = 1U << 12U,
    CameraEffectOrderMismatch = 1U << 13U,
    BloomPlacementMismatch = 1U << 14U,
    UiPlacementMismatch = 1U << 15U,
    SignalDomainMismatch = 1U << 16U,
};

[[nodiscard]] constexpr OrderViolation operator|(OrderViolation left, OrderViolation right) noexcept
{
    return static_cast<OrderViolation>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr OrderViolation &operator|=(OrderViolation &left, OrderViolation right) noexcept
{
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool HasViolation(OrderViolation violations, OrderViolation violation) noexcept
{
    return (static_cast<std::uint32_t>(violations) & static_cast<std::uint32_t>(violation)) != 0U;
}

struct StageContract final
{
    PostStage stage{PostStage::TemporalResolve};
    SignalDomain inputDomain{SignalDomain::SceneLinearRenderJittered};
    SignalDomain outputDomain{SignalDomain::SceneLinearRenderJittered};
    bool consumesLinearDepth{};
    bool consumesMotion{};
    bool producesDisplayExtent{};

    [[nodiscard]] bool operator==(StageContract const &) const noexcept = default;
};

struct PipelinePlan final
{
    std::uint32_t stageCount{};
    std::array<StageContract, kMaximumStageCount> stages{};
    OrderViolation violations{OrderViolation::None};
    // Index of the first submitted stage that broke a placement rule. Equal to stageCount when the only problems
    // are missing stages.
    std::uint32_t firstViolationStageIndex{};
    SignalDomain inputDomain{SignalDomain::SceneLinearRenderJittered};
    SignalDomain outputDomain{SignalDomain::SceneLinearRenderJittered};

    [[nodiscard]] constexpr bool IsLegal() const noexcept
    {
        return violations == OrderViolation::None;
    }

    [[nodiscard]] bool operator==(PipelinePlan const &) const noexcept = default;
};

// The signal contract of a single stage under a policy. It is a pure description: a stage has the same domains no
// matter where a caller mistakenly placed it, which is exactly what makes a misplacement detectable.
[[nodiscard]] std::expected<StageContract, ContractError> DescribeStage(PostStage stage,
                                                                        PipelinePolicy const &policy) noexcept;
// The canonical legal order for a policy, including the optional stages that the policy requires.
[[nodiscard]] std::expected<PipelinePlan, ContractError> CanonicalPipeline(PipelinePolicy const &policy) noexcept;
// Errors report malformed submissions: empty, oversized, or an unknown enumerator. Every ordering mistake is
// reported as a typed violation inside the returned plan, because an ordering mistake is data, not a programming
// error, and the learner needs to see all of it at once.
[[nodiscard]] std::expected<PipelinePlan, ContractError> PlanPipeline(std::span<PostStage const> stages,
                                                                      PipelinePolicy const &policy) noexcept;

// Everything a post-processing frame needs to know about the camera, the temporal state, and the signal it was
// handed. The extents are separate fields because the whole chapter turns on the difference between where a signal
// was rendered and where it is displayed.
struct CameraPostFrame final
{
    Extent2D renderExtent{};
    Extent2D displayExtent{};
    // Depth and motion are produced by the raster passes, so they live at the render extent. A stack that forgets
    // this and samples them at the display extent silently reads the wrong surface.
    Extent2D depthExtent{};
    Extent2D motionExtent{};
    // Sub-pixel jitter applied to this frame's projection, in render pixels.
    Float2 currentJitterPixels{};
    Float2 previousJitterPixels{};
    // Scale already baked into every stored scene-linear value, used to keep half-precision targets in range.
    double preExposure{1.0};
    double previousPreExposure{1.0};
    // Camera exposure, applied once, at the exposure stage, to the pre-exposure-free radiance.
    double exposureScale{1.0};
    double frameDeltaSeconds{};
    // Shutter open time. The exposure-time fraction shutterOpenSeconds / frameDeltaSeconds is the only way a
    // shutter angle enters the shutter integral.
    double shutterOpenSeconds{};
    double nearPlaneMetres{};
    double farPlaneMetres{};
    bool hasHistory{};
    bool cameraCut{};

    [[nodiscard]] bool operator==(CameraPostFrame const &) const noexcept = default;
};

struct PostFrameFacts final
{
    Extent2D renderExtent{};
    Extent2D displayExtent{};
    Float2 displayPixelsPerRenderPixel{};
    // previousJitter - currentJitter, in render pixels, matching the previousUV - currentUV motion convention.
    Float2 jitterDeltaPixels{};
    double exposureTimeFraction{};
    double preExposure{1.0};
    double exposureScale{1.0};
    // Multiplier that brings a value stored under the previous pre-exposure into the current pre-exposure.
    double historyToCurrentPreExposureScale{1.0};
    double nearPlaneMetres{};
    double farPlaneMetres{};
    bool upscales{};
    bool temporalResetRequired{};

    [[nodiscard]] bool operator==(PostFrameFacts const &) const noexcept = default;
};

[[nodiscard]] std::expected<PostFrameFacts, ContractError> ValidatePostFrame(CameraPostFrame const &frame) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// B. Bloom
// ---------------------------------------------------------------------------------------------------------------

// The threshold is a luminance in the scene's own radiometric unit, not a code value, and it is compared against
// the pre-exposure-free luminance. A stack that thresholds the pre-exposed value changes which highlights bloom
// every time the pre-exposure moves, which is the classic bloom that breathes with the auto-exposure system.
struct BloomThresholdSettings final
{
    double thresholdLuminance{1.0};
    // Half-width of the soft knee, in the same luminance unit. The knee spans
    // [threshold - knee, threshold + knee], so it may not reach below zero: softKneeLuminance must not exceed
    // thresholdLuminance.
    double softKneeLuminance{0.5};

    [[nodiscard]] bool operator==(BloomThresholdSettings const &) const noexcept = default;
};

struct BloomExtraction final
{
    double preExposedLuminance{};
    double absoluteLuminance{};
    // Radiance above the threshold, in luminance units. Below the knee it is exactly zero; above the knee it is
    // exactly absoluteLuminance - thresholdLuminance; inside the knee it is the quadratic that joins the two with
    // matching value and slope at both ends.
    double excessLuminance{};
    // excessLuminance / absoluteLuminance, clamped to [0, 1]. This is the fraction of the pixel's radiance that
    // bloom owns, and the composition stage must not add the remaining fraction a second time.
    double thresholdWeight{};
    Rgb extractedPreExposed{};
    Rgb extractedAbsolute{};
    bool belowKnee{};
    bool aboveKnee{};
    bool weightClamped{};

    [[nodiscard]] bool operator==(BloomExtraction const &) const noexcept = default;
};

[[nodiscard]] std::expected<BloomExtraction, ContractError> ExtractBloomHighlights(
    Rgb preExposedSceneLinear, double preExposure, BloomThresholdSettings const &settings) noexcept;

struct BloomPyramidSettings final
{
    std::uint32_t maximumLevelCount{6U};
    // Halving stops once either axis reaches this extent, so a level is never degenerate and the chain always
    // terminates, even for a one-texel-wide image.
    std::uint32_t minimumLevelExtent{2U};

    [[nodiscard]] bool operator==(BloomPyramidSettings const &) const noexcept = default;
};

struct BloomPyramid final
{
    std::uint32_t levelCount{};
    std::array<Extent2D, kMaximumBloomLevelCount> levelExtents{};

    [[nodiscard]] bool operator==(BloomPyramid const &) const noexcept = default;
};

// Each axis halves with a ceiling, so a 5-texel axis becomes 3 and then 2 and no column is ever dropped. Rounding
// down instead would discard the last column of every odd level and drift the image toward the top-left corner.
[[nodiscard]] std::expected<BloomPyramid, ContractError> BuildBloomPyramid(
    Extent2D baseExtent, BloomPyramidSettings const &settings) noexcept;

struct ResampleTap final
{
    std::uint32_t index{};
    double weight{};

    [[nodiscard]] bool operator==(ResampleTap const &) const noexcept = default;
};

// At most three taps per axis: a ceiling halving never reduces an axis by more than a factor of two, so one
// destination texel covers less than three source texels.
struct DownsampleFootprint1D final
{
    std::uint32_t tapCount{};
    std::array<ResampleTap, 3U> taps{};

    [[nodiscard]] bool operator==(DownsampleFootprint1D const &) const noexcept = default;
};

struct UpsampleFootprint1D final
{
    std::uint32_t lowIndex{};
    std::uint32_t highIndex{};
    double lowWeight{};
    double highWeight{};
    bool clampedToBorder{};

    [[nodiscard]] bool operator==(UpsampleFootprint1D const &) const noexcept = default;
};

// Exact area, or box, resampling: destination texel j owns the source interval [j * s, (j + 1) * s) with
// s = sourceExtent / destinationExtent, and each tap is weighted by its overlap divided by s. The weights sum to
// one for every destination texel and every legal extent pair, including odd ones, so a constant image survives the
// chain unchanged and no tap ever reaches outside the image.
//
// The overlap is computed in exact integers scaled by the destination extent, which a shader can reproduce in
// 32-bit unsigned arithmetic: the largest product is destinationIndex * sourceExtent, bounded by
// kMaximumDimension squared over two, well inside 32 bits.
[[nodiscard]] std::expected<DownsampleFootprint1D, ContractError> BloomDownsampleFootprint(
    std::uint32_t destinationIndex, std::uint32_t sourceExtent, std::uint32_t destinationExtent) noexcept;
// Bilinear reconstruction at destination texel centres with clamp-to-edge borders. The two weights sum to one, so
// upsampling is also a partition of unity and cannot change the level of a constant.
[[nodiscard]] std::expected<UpsampleFootprint1D, ContractError> BloomUpsampleFootprint(
    std::uint32_t destinationIndex, std::uint32_t sourceExtent, std::uint32_t destinationExtent) noexcept;

struct SceneLinearImageView final
{
    Extent2D extent{};
    std::span<Rgb const> pixels{};
};

// Validates the extent, the storage size, and every stored value. The whole-image scan is deliberate: a malformed
// image becomes a deterministic error instead of one that only appears when a particular tap reaches the bad texel.
[[nodiscard]] std::expected<void, ContractError> ValidateSceneLinearImage(SceneLinearImageView image) noexcept;
[[nodiscard]] std::expected<Rgb, ContractError> SampleSceneLinear(SceneLinearImageView image,
                                                                  PixelCoordinate pixel) noexcept;

[[nodiscard]] std::expected<void, ContractError> DownsampleSceneLinear(SceneLinearImageView source,
                                                                       Extent2D destinationExtent,
                                                                       std::span<Rgb> destination) noexcept;
[[nodiscard]] std::expected<void, ContractError> UpsampleSceneLinear(SceneLinearImageView source,
                                                                     Extent2D destinationExtent,
                                                                     std::span<Rgb> destination) noexcept;

struct BloomCombineSettings final
{
    // Relative contribution of each pyramid level, largest level first. Only the first levelCount entries are read.
    std::array<double, kMaximumBloomLevelCount> levelWeights{};
    // Normalised weights make the combine a reconstruction filter: a constant extracted signal comes back at the
    // same level whatever the pyramid depth or the image extent. Turning normalisation off is legal, but then the
    // weight sum is an energy gain that the caller owns and the result reports.
    bool normalizeLevelWeights{true};
    // Explicit, separately reported gain. Bloom gain is an artistic decision, and it must never hide inside the
    // filter weights, where a change of resolution or level count would silently change it.
    double bloomGain{1.0};

    [[nodiscard]] bool operator==(BloomCombineSettings const &) const noexcept = default;
};

struct BloomLevelStatistics final
{
    Extent2D extent{};
    double luminanceSum{};
    double meanLuminance{};
    double appliedWeight{};

    [[nodiscard]] bool operator==(BloomLevelStatistics const &) const noexcept = default;
};

struct BloomResult final
{
    std::uint32_t levelCount{};
    std::array<BloomLevelStatistics, kMaximumBloomLevelCount> levels{};
    double requestedWeightSum{};
    double appliedWeightSum{};
    double appliedGain{};
    bool weightsNormalized{};
    double extractedLuminanceSum{};
    double outputLuminanceSum{};
    // outputLuminanceSum / extractedLuminanceSum, or zero when nothing was extracted. A value below one is border
    // loss from clamp-to-edge reconstruction, which is expected; a value that changes with the extent is not.
    double energyRatio{};

    [[nodiscard]] bool operator==(BloomResult const &) const noexcept = default;
};

[[nodiscard]] std::expected<std::uint32_t, ContractError> BloomScratchPixelCount(BloomPyramid const &pyramid) noexcept;
// Progressive combine: the smallest level is weighted, upsampled into the next level, added to that level's
// weighted contribution, and so on down to level zero. The output is the bloom contribution only. It never contains
// the base image, so the composition stage owns exactly one copy of the base radiance.
//
// The scratch span holds every level above zero plus one accumulator the size of level zero, which is what
// BloomScratchPixelCount reports. Scratch, output, and the extracted image must not overlap.
[[nodiscard]] std::expected<BloomResult, ContractError> ComputeBloom(SceneLinearImageView extracted,
                                                                     BloomPyramid const &pyramid,
                                                                     BloomCombineSettings const &settings,
                                                                     std::span<Rgb> scratch,
                                                                     std::span<Rgb> output) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// C. Motion blur
// ---------------------------------------------------------------------------------------------------------------

struct ScalarImageView final
{
    Extent2D extent{};
    std::span<double const> values{};
};

// Motion stored as previousUV - currentUV, unjittered.
struct MotionImageView final
{
    Extent2D extent{};
    std::span<Float2 const> motionPreviousMinusCurrentUv{};
};

// The shutter is sampled symmetrically about the current frame time. EndpointInclusive places the first and last
// sample exactly on the ends of the exposure interval, which integrates the whole segment and makes the endpoints
// checkable. Midpoint places samples at interval midpoints, which never double-counts an endpoint when neighbouring
// pixels tile the same trajectory but never reaches the ends. Both schedules are centred: the mean parameter is
// exactly zero, so a symmetric shutter cannot shift the image.
enum class ShutterSampleSchedule : std::uint8_t
{
    EndpointInclusive = 0U,
    Midpoint,
};

struct MotionBlurSettings final
{
    std::uint32_t sampleCount{9U};
    ShutterSampleSchedule schedule{ShutterSampleSchedule::EndpointInclusive};
    // Gather budget in display pixels. Real trajectories are longer than any budget, so exceeding it is reported
    // rather than hidden.
    double maximumDisplacementPixels{64.0};
    bool clampDisplacementToBudget{true};
    // A tap is on the same surface as the centre when the linear view depths agree within
    // absolute + relative * centreDepth metres.
    double depthCompareAbsoluteMetres{0.01};
    double depthCompareRelative{0.02};

    [[nodiscard]] bool operator==(MotionBlurSettings const &) const noexcept = default;
};

struct ShutterSchedule final
{
    std::uint32_t sampleCount{};
    // Where the surface moved to during the whole frame, in signed display pixels. This is the negation of the
    // stored previousUV - currentUV motion, scaled by the display extent.
    Float2 frameDisplacementPixels{};
    // The frame displacement scaled by the exposure-time fraction and by any budget clamp.
    Float2 shutterDisplacementPixels{};
    double appliedScale{1.0};
    bool exceedsBudget{};
    bool clampedByBudget{};
    // Half the shutter displacement length: the furthest any sample can be from the centre pixel.
    double gatherRadiusPixels{};
    std::array<double, kMaximumMotionBlurSampleCount> parameters{};
    std::array<Float2, kMaximumMotionBlurSampleCount> offsetsPixels{};
    double centroidParameter{};
    bool includesCenterSample{};

    [[nodiscard]] bool operator==(ShutterSchedule const &) const noexcept = default;
};

// The single place where the previousUV - currentUV convention becomes a forward, signed, display-pixel
// displacement. A surface that moved right during the frame has a negative stored motion x and a positive
// displacement x.
[[nodiscard]] std::expected<Float2, ContractError> MotionToDisplayPixels(Float2 motionPreviousMinusCurrentUv,
                                                                         Extent2D displayExtent) noexcept;
[[nodiscard]] std::expected<ShutterSchedule, ContractError> BuildShutterSchedule(
    Float2 motionPreviousMinusCurrentUv, PostFrameFacts const &frame, MotionBlurSettings const &settings) noexcept;

// Why a shutter sample did or did not contribute. Rejected samples are never replaced by the centre colour: they
// are removed from the average and the remaining weights are renormalised, so a partially rejected trajectory
// darkens nothing.
enum class MotionTapReason : std::uint8_t
{
    CenterSample = 0U,
    Accepted,
    OutOfBounds,
    // The tap is in front of the centre surface, but its own shutter trajectory is too short to cover the centre
    // pixel, so it never actually smeared over it.
    ForegroundNotCovering,
    // The tap is behind the centre surface. Accepting it is what turns a disocclusion into a trail, because the
    // background it exposes was never in front of the moving object.
    BackgroundOccluded,
};

struct MotionBlurTap final
{
    double parameter{};
    Float2 offsetPixels{};
    PixelCoordinate pixel{};
    MotionTapReason reason{MotionTapReason::OutOfBounds};
    double weight{};
    double viewDepthMetres{};
    Rgb sceneLinear{};

    [[nodiscard]] bool operator==(MotionBlurTap const &) const noexcept = default;
};

struct MotionBlurImages final
{
    SceneLinearImageView sceneLinear{};
    ScalarImageView viewDepthMetres{};
    MotionImageView motion{};
};

struct MotionBlurResult final
{
    Rgb outputSceneLinear{};
    ShutterSchedule schedule{};
    std::uint32_t acceptedCount{};
    std::uint32_t rejectedOutOfBounds{};
    std::uint32_t rejectedForeground{};
    std::uint32_t rejectedBackground{};
    double weightSum{};
    bool fellBackToCenter{};
    std::array<MotionBlurTap, kMaximumMotionBlurSampleCount> taps{};

    [[nodiscard]] bool operator==(MotionBlurResult const &) const noexcept = default;
};

// Colour, depth, and motion must all be at the display extent: motion blur runs after temporal resolve, so the
// stack owes it resolved evidence. Handing it a render-extent depth or motion buffer is a size mismatch, not a
// resampling opportunity. Every stored view depth must also lie inside the frame's own near and far planes, which
// catches a depth buffer that was never linearised.
//
// Tile and neighbour-max acceleration is deliberately absent. A tile bound is only conservative when it is built
// from the same trajectories the gather uses, and building it correctly needs a second pass over the motion image
// with its own dilation contract. The chapter states that cost instead of teaching an unsound shortcut: this
// reference gathers along the centre pixel's own trajectory and reports the radius it needed.
[[nodiscard]] std::expected<MotionBlurResult, ContractError> EvaluateMotionBlur(
    PixelCoordinate displayPixel, MotionBlurImages const &images, PostFrameFacts const &frame,
    MotionBlurSettings const &settings) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// D. Depth of field
// ---------------------------------------------------------------------------------------------------------------

// A thin lens in physical units. Sensor height maps to display height, so the same lens produces a proportionally
// larger pixel circle of confusion on a taller output.
struct ThinLensCamera final
{
    double focalLengthMillimetres{50.0};
    double fNumber{2.8};
    double focusDistanceMetres{2.0};
    double sensorHeightMillimetres{24.0};

    [[nodiscard]] bool operator==(ThinLensCamera const &) const noexcept = default;
};

struct CircleOfConfusionSettings final
{
    double maximumRadiusPixels{32.0};
    // A circle this small is indistinguishable from a point sample, so the pixel is declared in focus.
    double inFocusRadiusPixels{0.5};

    [[nodiscard]] bool operator==(CircleOfConfusionSettings const &) const noexcept = default;
};

enum class FocusRegion : std::uint8_t
{
    InFocus = 0U,
    NearField,
    FarField,
};

struct CircleOfConfusion final
{
    double apertureDiameterMillimetres{};
    double diameterMillimetres{};
    double pixelsPerMillimetre{};
    // Signed by convention: negative in front of the focus plane, in the near field, and positive behind it, in the
    // far field. The sign is what lets the compositor keep a near-field silhouette from being filled in by the
    // background behind it.
    double signedRadiusPixels{};
    double clampedSignedRadiusPixels{};
    FocusRegion region{FocusRegion::InFocus};
    bool clampedByBudget{};

    [[nodiscard]] bool operator==(CircleOfConfusion const &) const noexcept = default;
};

// c = A * f * |z - S| / (z * (S - f)) millimetres on the sensor, with A = f / N and all lengths in millimetres.
// Scene distances arrive in metres and are converted exactly once. The radius is half the diameter, and the
// conversion to pixels is displayHeight / sensorHeight.
[[nodiscard]] std::expected<CircleOfConfusion, ContractError> ComputeCircleOfConfusion(
    double viewDepthMetres, ThinLensCamera const &camera, CircleOfConfusionSettings const &settings,
    Extent2D displayExtent) noexcept;
// The limit of the far-field radius as the subject distance goes to infinity, from a diameter of A * f / (S - f)
// millimetres. Far-field blur is bounded; near-field blur is not, which is why the two fields need different
// budgets.
[[nodiscard]] std::expected<double, ContractError> FarFieldLimitRadiusPixels(ThinLensCamera const &camera,
                                                                             CircleOfConfusionSettings const &settings,
                                                                             Extent2D displayExtent) noexcept;

struct ApertureSample final
{
    Float2 unitSquare{};
    Float2 unitDisk{};

    [[nodiscard]] bool operator==(ApertureSample const &) const noexcept = default;
};

// Shirley and Chiu's concentric mapping: area preserving, continuous, and it maps the unit square boundary onto the
// unit circle, so equal-area regions of the square carry equal aperture area. A naive polar mapping would bunch
// samples at the centre of the aperture and quietly bias every bokeh disc.
[[nodiscard]] std::expected<Float2, ContractError> MapConcentricDisk(Float2 unitSquareSample) noexcept;
// Deterministic Hammersley points, offset to cell centres so that no sample lands on the degenerate square centre.
[[nodiscard]] std::expected<std::uint32_t, ContractError> BuildApertureSamples(
    std::uint32_t sampleCount, std::span<ApertureSample> destination) noexcept;

struct DepthOfFieldSettings final
{
    ThinLensCamera camera{};
    CircleOfConfusionSettings coc{};
    // Radius of the near-field search disc in display pixels. A production stack derives it from a dilated
    // near-field circle-of-confusion tile map; that dilation pass is deferred, so the caller states the bound and
    // the result reports the coverage it actually found.
    double nearFieldSearchRadiusPixels{16.0};
    // Below this accepted fraction the far-field gather has no honest evidence, so it falls back to the centre
    // colour instead of inventing an average from a handful of taps.
    double minimumFarCoverage{0.25};
    double depthCompareAbsoluteMetres{0.01};
    double depthCompareRelative{0.02};

    [[nodiscard]] bool operator==(DepthOfFieldSettings const &) const noexcept = default;
};

enum class DofTapReason : std::uint8_t
{
    CenterSample = 0U,
    AcceptedFar,
    AcceptedNear,
    OutOfBounds,
    // The tap's own circle of confusion does not reach the centre pixel, so scattering it here would invent light.
    // This is the only occlusion test the far field needs against sharper surfaces: an in-focus silhouette has a
    // sub-pixel circle, so it cannot reach a neighbour however close it is to the camera.
    OutsideCircleOfConfusion,
    // The tap is a near-field surface in front of the centre, so the near-field pass owns it. Letting it into the
    // far-field gather as well would count the same surface twice and would composite it without any coverage.
    OwnedByNearField,
    // Only the near-field pass reports this: the tap is not itself a near-field surface in front of the centre.
    NotNearField,
};

struct DepthOfFieldTap final
{
    Float2 offsetPixels{};
    PixelCoordinate pixel{};
    double viewDepthMetres{};
    double signedRadiusPixels{};
    DofTapReason reason{DofTapReason::OutOfBounds};
    double weight{};
    Rgb sceneLinear{};

    [[nodiscard]] bool operator==(DepthOfFieldTap const &) const noexcept = default;
};

struct DepthOfFieldImages final
{
    SceneLinearImageView sceneLinear{};
    ScalarImageView viewDepthMetres{};
};

struct DepthOfFieldResult final
{
    Rgb outputSceneLinear{};
    Rgb farFieldSceneLinear{};
    Rgb nearFieldSceneLinear{};
    CircleOfConfusion centerCoc{};
    double farGatherRadiusPixels{};
    double nearGatherRadiusPixels{};
    // Fraction of aperture directions in which a near-field surface covers this pixel. It is the alpha the near
    // field is composited with, so a half-covered silhouette is half foreground and half whatever lies behind it.
    double nearCoverage{};
    double farWeightSum{};
    double nearWeightSum{};
    std::uint32_t sampleCount{};
    std::uint32_t farAcceptedCount{};
    std::uint32_t nearAcceptedCount{};
    // The rejection counters describe the far-field pass, whose acceptance decides whether the gather had enough
    // evidence. The near-field pass reports itself through nearCoverage and its own typed tap reasons.
    std::uint32_t rejectedOutOfBounds{};
    std::uint32_t rejectedOutsideCoc{};
    std::uint32_t rejectedOwnedByNearField{};
    bool insufficientFarCoverage{};
    bool clampedByBudget{};
    std::array<DepthOfFieldTap, kMaximumApertureSampleCount> farTaps{};
    std::array<DepthOfFieldTap, kMaximumApertureSampleCount> nearTaps{};

    [[nodiscard]] bool operator==(DepthOfFieldResult const &) const noexcept = default;
};

// Scatter as gather. A tap deposits its radiance uniformly over its own circle of confusion, so the radiance it
// delivers per unit area is proportional to 1 / radius^2 and it only reaches pixels inside that radius. Sampling
// the gather disc uniformly and weighting each accepted tap by 1 / max(radius, kMinimumScatterRadiusPixels)^2 is
// therefore the estimator for that scatter, not an arbitrary blur kernel.
//
// The two fields are separated because they fail differently, and every tap belongs to exactly one of them:
//
//   * The near field is unbounded, spreads *over* whatever is behind it, and needs a coverage fraction rather than
//     a colour, so that a silhouette dissolves instead of being replaced. A tap qualifies when its own circle of
//     confusion is a near-field circle and it lies in front of the centre surface.
//   * The far field owns every other tap, including a defocused surface that is nearer than the centre but still
//     behind the focus plane. Those surfaces are exactly what a lens smears across a more distant background, so
//     rejecting them on depth alone would punch a hole on one side of an edge and leave the other side intact.
//
// Occlusion between far-field surfaces is therefore decided by the scatter footprint, not by depth: a tap only
// reaches the centre when its own circle of confusion covers the distance between them. A sharp, in-focus
// silhouette has a sub-pixel circle, so it still cannot bleed across an edge, which is the case the depth-only
// rule was really protecting.
[[nodiscard]] std::expected<DepthOfFieldResult, ContractError> EvaluateDepthOfField(
    PixelCoordinate displayPixel, DepthOfFieldImages const &images, PostFrameFacts const &frame,
    DepthOfFieldSettings const &settings, std::span<ApertureSample const> apertureSamples) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// E. Composition, exposure, display encoding, and diagnostics
// ---------------------------------------------------------------------------------------------------------------

// How the bloom contribution meets the base image.
//
// AdditiveContribution is what the chapter's stack uses and the only mode with a defensible physical reading. The
// bloom image produced by ComputeBloom contains the *extracted excess* and nothing else, so adding a gained copy of
// it to the base counts the scene's radiance exactly once and adds only the light the lens scattered out of the
// highlights.
//
// CreativeCrossfade is the familiar `lerp(base, bloom, intensity)` slider. It is kept because it is common in
// production and a learner will meet it, but its behaviour must be stated plainly rather than dressed up: because
// the bloom image is contribution-only, the crossfade removes `intensity` of the base radiance from *every* pixel,
// including pixels whose radiance never reached the bloom threshold and which therefore receive nothing back. It
// darkens the image, it does not conserve energy, and it is not a redistribution of the same light. It is a
// creative attenuation, and CompositionResult reports exactly how much base radiance it discarded.
enum class BloomCompositionMode : std::uint8_t
{
    AdditiveContribution = 0U,
    CreativeCrossfade,
};

struct CompositionSettings final
{
    BloomCompositionMode mode{BloomCompositionMode::AdditiveContribution};
    // The explicit multiplier applied to the bloom contribution. Under CreativeCrossfade it is also the fraction of
    // the base that is thrown away, so it is restricted to [0, 1] there.
    double bloomIntensity{0.05};

    [[nodiscard]] bool operator==(CompositionSettings const &) const noexcept = default;
};

struct CompositionInput final
{
    // The base image after the camera effects. It owns the whole scene radiance exactly once.
    Rgb baseSceneLinear{};
    // The bloom contribution only, as produced by ComputeBloom from extracted highlights. It must not contain the
    // base image; if it did, every highlight would be counted twice.
    Rgb bloomSceneLinear{};
    // Scene coverage for compositing over a background or a UI layer, straight rather than premultiplied.
    double coverageAlpha{1.0};

    [[nodiscard]] bool operator==(CompositionInput const &) const noexcept = default;
};

struct CompositionResult final
{
    Rgb sceneLinear{};
    Rgb premultipliedSceneLinear{};
    Rgb bloomContribution{};
    double baseFraction{};
    double bloomFraction{};
    double coverageAlpha{};
    // Decomposed luminance accounting, so the cost of each mode is visible rather than asserted.
    double baseLuminance{};
    double addedBloomLuminance{};
    // Base radiance the mode threw away: exactly zero for AdditiveContribution and bloomIntensity * baseLuminance
    // for CreativeCrossfade.
    double discardedBaseLuminance{};
    double outputLuminance{};
    // True only when every unit of base radiance survived composition.
    bool preservesBaseRadiance{};

    [[nodiscard]] bool operator==(CompositionResult const &) const noexcept = default;
};

// Errors reject malformed radiance, an alpha outside [0, 1], a negative intensity, a crossfade intensity above one,
// and any composition that leaves the representable scene-linear domain.
[[nodiscard]] std::expected<CompositionResult, ContractError> ComposeSceneLinear(
    CompositionInput const &input, CompositionSettings const &settings) noexcept;

struct ExposureInput final
{
    Rgb preExposedSceneLinear{};
    double preExposure{1.0};
    double exposureScale{1.0};

    [[nodiscard]] bool operator==(ExposureInput const &) const noexcept = default;
};

struct ExposureResult final
{
    // The physical radiance the renderer meant, with the storage scale removed.
    Rgb absoluteSceneLinear{};
    // The tone curve's input: absolute radiance multiplied by the camera exposure exactly once.
    Rgb exposedSceneLinear{};
    double appliedScale{};

    [[nodiscard]] bool operator==(ExposureResult const &) const noexcept = default;
};

// Removes the pre-exposure and applies the camera exposure in one place, so the stack has exactly one opportunity
// to apply exposure and the plan validator can prove that it happened once.
[[nodiscard]] std::expected<ExposureResult, ContractError> ApplyExposure(ExposureInput const &input) noexcept;

enum class TransferFunction : std::uint8_t
{
    Linear = 0U,
    Srgb,
    Gamma22,
};

// Display encoding closes the linear domain. The input must already be display-referred linear in [0, 1]; a value
// outside that range means the tone curve did not finish its job, and encoding it would clip invisibly.
[[nodiscard]] std::expected<Rgb, ContractError> EncodeDisplay(Rgb displayLinear, TransferFunction transfer) noexcept;

struct UiLayer final
{
    Rgb color{};
    double alpha{};

    [[nodiscard]] bool operator==(UiLayer const &) const noexcept = default;
};

// Straight-alpha source-over composition in whichever domain the policy chose. Nothing may filter the result: text
// and thin UI lines are authored at display resolution, and any later blur destroys exactly the detail they exist
// to show.
[[nodiscard]] std::expected<Rgb, ContractError> CompositeUi(Rgb destination, UiLayer const &ui) noexcept;

struct SceneLinearStatistics final
{
    std::uint32_t pixelCount{};
    double minimumChannel{};
    double maximumChannel{};
    double minimumLuminance{};
    double maximumLuminance{};
    double meanLuminance{};
    double luminanceSum{};

    [[nodiscard]] bool operator==(SceneLinearStatistics const &) const noexcept = default;
};

[[nodiscard]] std::expected<SceneLinearStatistics, ContractError> SummarizeSceneLinear(
    SceneLinearImageView image) noexcept;

} // namespace ch30::post_processing
