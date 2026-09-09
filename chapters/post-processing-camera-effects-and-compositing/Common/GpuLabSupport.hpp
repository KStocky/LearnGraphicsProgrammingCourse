#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "PostProcessingContracts.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Chapter 30 paired GPU lab.
//
// Every frame the lab runs one scene pass, an explicit sequence of post-processing dispatches, and one display
// pass:
//
//   1. SceneCS resolves the deterministic analytic scene once per display pixel and publishes the only screen data
//      the post chain is allowed to see: a scene-linear, pre-exposed, temporally resolved radiance image at the
//      display extent, the linear view depth in metres, and the Chapter 11 motion convention previousUV -
//      currentUV. Temporal reconstruction itself belongs to Chapter 28; this lab consumes its converged result.
//   2. The Starter stops there and runs only ComposeCS, which removes the pre-exposure, applies the camera
//      exposure exactly once, tone maps, encodes for the display, and composites the UI. It performs no bloom, no
//      motion blur, and no depth of field, and neither do the shared helpers it calls.
//   3. The Solution submits the stages of a validated PipelinePlan: depth of field and motion blur in the declared
//      order, bloom extraction from the image the declared policy names, a ceiling-halved pyramid recombined
//      progressively on exact area and bilinear footprints, and then the same output path.
//
// The renderer never invents an order. It builds the stage list from the configuration, validates it with
// ch30::post_processing::PlanPipeline, and fails the frame if the plan is illegal, so an ordering mistake is a
// reported error rather than a silently corrected one. The frame constants likewise come from ValidatePostFrame,
// so the shutter fraction, the pre-exposure, and the depth range the shader sees are the ones the CPU reference
// validated.

namespace ch30::post_processing::gpu
{

inline constexpr std::uint32_t kMaximumWidth = 320U;
inline constexpr std::uint32_t kMaximumHeight = 192U;
// The lab caps the pyramid below the contract's own limit so a level is never smaller than the group size can
// dispatch usefully and so the buffer footprint stays bounded on WARP.
inline constexpr std::uint32_t kMaximumLabBloomLevelCount = 6U;
inline constexpr std::uint32_t kMaximumLabApertureSampleCount = 32U;
inline constexpr std::uint32_t kMaximumLabMotionBlurSampleCount = 32U;
// Depth extremes of the analytic scene, in metres. The configured near and far planes must contain them, because
// the motion-blur and depth-of-field contracts reject a depth outside the frame's own range.
inline constexpr double kSceneMinimumFixedDepthMetres = 0.8;
inline constexpr double kSceneMaximumFixedDepthMetres = 40.0;

enum class LabVariant : std::uint8_t
{
    Starter,
    Solution,
};

// Which analytic picture the scene pass publishes. The constant and split fields exist so a test can state an
// exact expectation on a hand-checkable image; the camera lab is the picture the chapter actually teaches with.
enum class SceneVariant : std::uint32_t
{
    CameraLab = 0U,
    ConstantField = 1U,
    SplitField = 2U,
};

enum class DebugView : std::uint32_t
{
    Final = 0U,
    SceneLinearBaseline = 1U,
    ViewDepth = 2U,
    Motion = 3U,
    CircleOfConfusion = 4U,
    DepthOfField = 5U,
    MotionBlur = 6U,
    BloomExtraction = 7U,
    BloomContribution = 8U,
    Composition = 9U,
    Exposed = 10U,
    ToneMapped = 11U,
    DisplayEncoded = 12U,
};

inline constexpr std::uint32_t kDebugViewCount = static_cast<std::uint32_t>(DebugView::DisplayEncoded) + 1U;

// Bits mirrored by PostProcessingShared.hlsli.
inline constexpr std::uint32_t kStatusScene = 1U << 0U;
inline constexpr std::uint32_t kStatusDepthOfField = 1U << 1U;
inline constexpr std::uint32_t kStatusMotionBlur = 1U << 2U;
inline constexpr std::uint32_t kStatusBloom = 1U << 3U;
inline constexpr std::uint32_t kStatusComposition = 1U << 4U;
inline constexpr std::uint32_t kStatusExposure = 1U << 5U;
inline constexpr std::uint32_t kStatusToneMap = 1U << 6U;
inline constexpr std::uint32_t kStatusDisplayEncode = 1U << 7U;
inline constexpr std::uint32_t kStatusUi = 1U << 8U;

inline constexpr std::uint32_t kDofInsufficientFarCoverage = 1U << 0U;
inline constexpr std::uint32_t kDofClampedByBudget = 1U << 1U;
inline constexpr std::uint32_t kDofEvaluated = 1U << 2U;

inline constexpr std::uint32_t kMotionExceedsBudget = 1U << 0U;
inline constexpr std::uint32_t kMotionClampedByBudget = 1U << 1U;
inline constexpr std::uint32_t kMotionIncludesCenterSample = 1U << 2U;
inline constexpr std::uint32_t kMotionFellBackToCenter = 1U << 3U;
inline constexpr std::uint32_t kMotionEvaluated = 1U << 4U;

inline constexpr std::uint32_t kBloomBelowKnee = 1U << 0U;
inline constexpr std::uint32_t kBloomAboveKnee = 1U << 1U;
inline constexpr std::uint32_t kBloomWeightClamped = 1U << 2U;
inline constexpr std::uint32_t kBloomEvaluated = 1U << 3U;

// The shader stamps this into the final record word. Reading it back proves the CPU and HLSL layouts still agree.
inline constexpr std::uint32_t kAbiMarker = 0x5050'3330U;

struct LabConfiguration final
{
    DebugView debugView{DebugView::Final};
    SceneVariant sceneVariant{SceneVariant::CameraLab};
    CameraEffectOrder cameraEffectOrder{CameraEffectOrder::DefocusThenShutter};
    BloomSourcePlacement bloomSource{BloomSourcePlacement::AfterCameraEffects};
    UiCompositePlacement uiPlacement{UiCompositePlacement::AfterDisplayEncode};
    BloomCompositionMode compositionMode{BloomCompositionMode::AdditiveContribution};
    TransferFunction transferFunction{TransferFunction::Srgb};
    ShutterSampleSchedule shutterSchedule{ShutterSampleSchedule::EndpointInclusive};
    std::uint32_t animationFrame{};
    std::uint32_t bloomMaximumLevelCount{5U};
    std::uint32_t bloomMinimumLevelExtent{2U};
    std::uint32_t motionBlurSampleCount{9U};
    std::uint32_t apertureSampleCount{16U};
    float preExposure{1.0F};
    float exposureScale{1.0F};
    float frameDeltaSeconds{1.0F / 60.0F};
    float shutterOpenSeconds{1.0F / 120.0F};
    float nearPlaneMetres{0.1F};
    float farPlaneMetres{100.0F};
    float bloomThresholdLuminance{1.0F};
    float bloomSoftKneeLuminance{0.5F};
    // Relative level weight falloff: level i requests falloff^i before normalisation, so a single number covers
    // both the uniform reconstruction filter and a tighter one without hiding the gain in the weights.
    float bloomLevelWeightFalloff{1.0F};
    float bloomGain{1.0F};
    float bloomIntensity{0.05F};
    float focalLengthMillimetres{50.0F};
    float fNumber{2.8F};
    float focusDistanceMetres{3.0F};
    float sensorHeightMillimetres{24.0F};
    float maximumCocRadiusPixels{12.0F};
    float inFocusRadiusPixels{0.5F};
    float nearFieldSearchRadiusPixels{8.0F};
    float minimumFarCoverage{0.25F};
    float dofDepthCompareAbsoluteMetres{0.01F};
    float dofDepthCompareRelative{0.02F};
    float maximumShutterDisplacementPixels{24.0F};
    float motionDepthCompareAbsoluteMetres{0.01F};
    float motionDepthCompareRelative{0.02F};
    float uiAlpha{0.5F};
    float uiColorR{0.85F};
    float uiColorG{0.85F};
    float uiColorB{0.20F};
    float constantRadianceR{0.25F};
    float constantRadianceG{0.50F};
    float constantRadianceB{1.00F};
    float constantViewDepthMetres{4.0F};
    float constantMotionU{};
    float constantMotionV{};
    float splitNearDepthMetres{1.2F};
    float splitRadianceScale{4.0F};
    // Motion of the far slab relative to the near slab. Values above one make the background outrun the
    // foreground, which is the case that produces a foreground tap whose trajectory never covered the pixel.
    float splitMotionScale{0.25F};
    bool depthOfFieldEnabled{true};
    bool motionBlurEnabled{true};
    bool bloomEnabled{true};
    bool uiEnabled{true};
    bool clampShutterToBudget{true};
    bool normalizeBloomLevelWeights{true};
    bool resetChain{};

    [[nodiscard]] bool operator==(LabConfiguration const &) const noexcept = default;
};

// Why the renderer had to rebuild the state it derives from a configuration. Reporting the reasons as flags keeps
// an invalidation observable instead of implicit: a test can change one field and see exactly what it invalidated.
enum class InvalidationReason : std::uint32_t
{
    None = 0U,
    FirstFrame = 1U << 0U,
    ExplicitReset = 1U << 1U,
    ExtentChanged = 1U << 2U,
    PipelinePolicyChanged = 1U << 3U,
    BloomPyramidChanged = 1U << 4U,
    BloomSettingsChanged = 1U << 5U,
    ApertureSamplesChanged = 1U << 6U,
    ShutterScheduleChanged = 1U << 7U,
    SceneChanged = 1U << 8U,
    FrameFactsChanged = 1U << 9U,
    DisplayEncodingChanged = 1U << 10U,
};

[[nodiscard]] constexpr InvalidationReason operator|(InvalidationReason left, InvalidationReason right) noexcept
{
    return static_cast<InvalidationReason>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr InvalidationReason &operator|=(InvalidationReason &left, InvalidationReason right) noexcept
{
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool HasInvalidation(InvalidationReason reasons, InvalidationReason reason) noexcept
{
    return (static_cast<std::uint32_t>(reasons) & static_cast<std::uint32_t>(reason)) != 0U;
}

// One display pixel of byte-inspectable evidence. Everything before encodedR is scene-linear or a pure geometric
// term; nothing before it is tone mapped or transfer encoded.
struct PixelRecord final
{
    float sceneR{};
    float sceneG{};
    float sceneB{};
    float sceneLuminance{};
    float viewDepthMetres{};
    float motionX{};
    float motionY{};
    float coverageAlpha{};
    float cocSignedRadiusPixels{};
    float cocClampedRadiusPixels{};
    float cocDiameterMillimetres{};
    float cocApertureDiameterMillimetres{};
    float cocPixelsPerMillimetre{};
    float farGatherRadiusPixels{};
    float nearGatherRadiusPixels{};
    float nearCoverage{};
    float farCoverage{};
    float farWeightSum{};
    float nearWeightSum{};
    float dofFarR{};
    float dofFarG{};
    float dofFarB{};
    float dofNearR{};
    float dofNearG{};
    float dofNearB{};
    float dofOutR{};
    float dofOutG{};
    float dofOutB{};
    float apertureFirstX{};
    float apertureFirstY{};
    float apertureLastX{};
    float apertureLastY{};
    float frameDisplacementX{};
    float frameDisplacementY{};
    float shutterDisplacementX{};
    float shutterDisplacementY{};
    float shutterAppliedScale{};
    float shutterGatherRadiusPixels{};
    float shutterCentroidParameter{};
    float shutterFirstParameter{};
    float shutterLastParameter{};
    float motionWeightSum{};
    float motionBlurR{};
    float motionBlurG{};
    float motionBlurB{};
    float bloomPreExposedLuminance{};
    float bloomAbsoluteLuminance{};
    float bloomExcessLuminance{};
    float bloomThresholdWeight{};
    float bloomExtractedR{};
    float bloomExtractedG{};
    float bloomExtractedB{};
    float bloomContributionR{};
    float bloomContributionG{};
    float bloomContributionB{};
    float baseR{};
    float baseG{};
    float baseB{};
    float composedR{};
    float composedG{};
    float composedB{};
    float composedBaseFraction{};
    float composedBloomFraction{};
    float composedDiscardedBaseLuminance{};
    float composedOutputLuminance{};
    float absoluteR{};
    float absoluteG{};
    float absoluteB{};
    float exposedR{};
    float exposedG{};
    float exposedB{};
    float exposureAppliedScale{};
    float toneMappedR{};
    float toneMappedG{};
    float toneMappedB{};
    float encodedR{};
    float encodedG{};
    float encodedB{};
    float uiR{};
    float uiG{};
    float uiB{};
    float uiAlpha{};
    float finalR{};
    float finalG{};
    float finalB{};
    std::uint32_t cocRegion{};
    std::uint32_t dofSampleCount{};
    std::uint32_t dofFarAcceptedCount{};
    std::uint32_t dofNearAcceptedCount{};
    std::uint32_t dofRejectedOutOfBounds{};
    std::uint32_t dofRejectedOutsideCoc{};
    std::uint32_t dofRejectedOwnedByNearField{};
    std::uint32_t dofFlags{};
    std::uint32_t motionSampleCount{};
    std::uint32_t motionAcceptedCount{};
    std::uint32_t motionRejectedOutOfBounds{};
    std::uint32_t motionRejectedForeground{};
    std::uint32_t motionRejectedBackground{};
    std::uint32_t motionFlags{};
    std::uint32_t bloomFlags{};
    std::uint32_t bloomLevelCount{};
    std::uint32_t exposureApplicationCount{};
    std::uint32_t stageOrderWord{};
    std::uint32_t stageCount{};
    std::uint32_t status{};
    std::uint32_t abiMarker{};

    [[nodiscard]] bool operator==(PixelRecord const &) const noexcept = default;
};
static_assert(sizeof(PixelRecord) == 424U);
static_assert(alignof(PixelRecord) == 4U);
static_assert(offsetof(PixelRecord, viewDepthMetres) == 16U);
static_assert(offsetof(PixelRecord, cocSignedRadiusPixels) == 32U);
static_assert(offsetof(PixelRecord, apertureFirstX) == 112U);
static_assert(offsetof(PixelRecord, frameDisplacementX) == 128U);
static_assert(offsetof(PixelRecord, bloomPreExposedLuminance) == 180U);
static_assert(offsetof(PixelRecord, baseR) == 220U);
static_assert(offsetof(PixelRecord, composedR) == 232U);
static_assert(offsetof(PixelRecord, absoluteR) == 260U);
static_assert(offsetof(PixelRecord, toneMappedR) == 288U);
static_assert(offsetof(PixelRecord, finalR) == 328U);
static_assert(offsetof(PixelRecord, cocRegion) == 340U);
static_assert(offsetof(PixelRecord, exposureApplicationCount) == 404U);
static_assert(offsetof(PixelRecord, status) == 416U);
static_assert(offsetof(PixelRecord, abiMarker) == 420U);

struct SceneRecord final
{
    float radianceR{};
    float radianceG{};
    float radianceB{};
    float viewDepthMetres{};
    float motionX{};
    float motionY{};
    float coverageAlpha{};
    float padding{};
};
static_assert(sizeof(SceneRecord) == 32U);

struct BloomLevelLayout final
{
    Extent2D extent{};
    std::uint32_t pyramidOffset{};
    std::uint32_t accumulatorOffset{};
    double appliedWeight{};

    [[nodiscard]] bool operator==(BloomLevelLayout const &) const noexcept = default;
};

struct BloomLayout final
{
    BloomPyramid pyramid{};
    std::array<BloomLevelLayout, kMaximumBloomLevelCount> levels{};
    std::uint32_t pyramidPixelCount{};
    double requestedWeightSum{};
    double appliedWeightSum{};

    [[nodiscard]] bool operator==(BloomLayout const &) const noexcept = default;
};

struct FrameReadback final
{
    LabConfiguration configuration{};
    std::vector<PixelRecord> pixels{};
    lgp::framework::Extent2D displaySize{};
    PostFrameFacts frameFacts{};
    PipelinePlan plan{};
    BloomLayout bloomLayout{};
    std::array<PostStage, kMaximumStageCount> executedStages{};
    std::uint32_t executedStageCount{};
    std::uint32_t stageOrderWord{};
    std::uint32_t expectedStatus{};
    InvalidationReason invalidation{InvalidationReason::None};
    std::uint32_t frameSlot{};
};

struct BufferBarrierState final
{
    D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};
    D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};
};

[[nodiscard]] lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration,
                                                              LabVariant variant);
// The configuration a variant starts from. The Starter owns no camera effects and no bloom pyramid, so its
// defaults have them switched off and its validation refuses to switch them on.
[[nodiscard]] LabConfiguration DefaultConfiguration(LabVariant variant) noexcept;

// The CPU reference inputs the renderer uploads. Tests reuse them so a contract replay is driven by the same
// numbers the shader received rather than by a second transcription of them.
[[nodiscard]] CameraPostFrame MakeCameraPostFrame(LabConfiguration const &configuration,
                                                  lgp::framework::Extent2D displayExtent) noexcept;
[[nodiscard]] PipelinePolicy MakePipelinePolicy(LabConfiguration const &configuration) noexcept;
[[nodiscard]] BloomThresholdSettings MakeBloomThresholdSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] BloomPyramidSettings MakeBloomPyramidSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] BloomCombineSettings MakeBloomCombineSettings(LabConfiguration const &configuration,
                                                            std::uint32_t levelCount) noexcept;
[[nodiscard]] MotionBlurSettings MakeMotionBlurSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] DepthOfFieldSettings MakeDepthOfFieldSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] CompositionSettings MakeCompositionSettings(LabConfiguration const &configuration) noexcept;

// The stage sequence the renderer submits, in submission order, with the stages the configuration disabled left
// out. It is the canonical order for the policy, so PlanPipeline accepts it for every legal configuration.
[[nodiscard]] std::uint32_t BuildExecutedStages(LabConfiguration const &configuration, LabVariant variant,
                                                std::span<PostStage> stages) noexcept;
// Four bits per submitted stage, holding the stage enumerator plus one so an unused nibble reads as zero.
[[nodiscard]] std::uint32_t EncodeStageOrder(std::span<PostStage const> stages) noexcept;
[[nodiscard]] std::uint32_t ExpectedStatus(LabConfiguration const &configuration, LabVariant variant) noexcept;
[[nodiscard]] std::expected<BloomLayout, lgp::framework::Error> BuildBloomLayout(
    LabConfiguration const &configuration, lgp::framework::Extent2D displayExtent);
// Pure: the reasons a change from one configuration to another forces derived state to be rebuilt.
[[nodiscard]] InvalidationReason ComputeInvalidation(LabConfiguration const &previous, LabConfiguration const &current,
                                                     bool firstFrame, bool extentChanged, bool explicitReset) noexcept;

class BufferResource final
{
  public:
    BufferResource() = default;
    BufferResource(BufferResource &&other) noexcept;
    BufferResource &operator=(BufferResource &&other) noexcept;
    BufferResource(BufferResource const &) = delete;
    BufferResource &operator=(BufferResource const &) = delete;
    ~BufferResource();

    [[nodiscard]] ID3D12Resource *Get() const noexcept
    {
        return resource_.Get();
    }
    [[nodiscard]] std::uint64_t size_in_bytes() const noexcept
    {
        return sizeInBytes_;
    }
    [[nodiscard]] std::byte const *mapped_data() const noexcept
    {
        return mappedData_;
    }

  private:
    friend std::expected<BufferResource, lgp::framework::Error> CreateBuffer(ID3D12Device10 &, std::uint64_t,
                                                                             D3D12_HEAP_TYPE, D3D12_RESOURCE_FLAGS,
                                                                             std::wstring_view, bool);
    void Reset() noexcept;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::uint64_t sizeInBytes_{};
    std::byte *mappedData_{};
};

[[nodiscard]] std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
    ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
    std::wstring_view name, bool mapPersistently = false);

class RendererCore : public lgp::framework::IChapterRenderer
{
  public:
    RendererCore(std::filesystem::path shaderPath, LabVariant variant);
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void ConfigureHeadlessTest(LabConfiguration const &configuration) noexcept;
    void RequestReset() noexcept;
    [[nodiscard]] std::expected<FrameReadback, lgp::framework::Error> ReadBackOutputs();

  private:
    // Every buffer is frame-slot-owned: the post chain carries no state between frames, so nothing here is
    // sequence-owned and no pass can read a value an earlier frame left behind.
    struct FrameSlotResources final
    {
        BufferResource records{};
        BufferResource scene{};
        BufferResource chain{};
        BufferResource bloom{};
        BufferResource readback{};
        lgp::framework::DescriptorAllocation descriptors{};
        bool initialized{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateResources(lgp::framework::Extent2D size);
    void DestroyResources(lgp::framework::DeviceResources &deviceResources) noexcept;
    [[nodiscard]] LabConfiguration ActiveConfiguration() const noexcept;

    std::filesystem::path shaderPath_{};
    LabVariant variant_{};
    bool headless_{};
    bool hasRendered_{};
    bool forceReset_{true};
    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader sceneShader_{};
    lgp::framework::CompiledShader cameraEffectShader_{};
    lgp::framework::CompiledShader bloomExtractShader_{};
    lgp::framework::CompiledShader bloomDownsampleShader_{};
    lgp::framework::CompiledShader bloomUpsampleShader_{};
    lgp::framework::CompiledShader composeShader_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> scenePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> cameraEffectPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> bloomExtractPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> bloomDownsamplePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> bloomUpsamplePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> composePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
    std::vector<FrameSlotResources> frameSlots_{};
    std::uint32_t bloomCapacityPixels_{};
    lgp::framework::Extent2D size_{};
    std::optional<LabConfiguration> headlessConfiguration_{};
    LabConfiguration interactiveConfiguration_{};
    LabConfiguration lastRenderedConfiguration_{};
    lgp::framework::Extent2D lastRenderedExtent_{};
    PostFrameFacts lastFrameFacts_{};
    PipelinePlan lastPlan_{};
    BloomLayout lastBloomLayout_{};
    std::array<PostStage, kMaximumStageCount> lastStages_{};
    std::uint32_t lastStageCount_{};
    std::uint32_t lastStageOrderWord_{};
    std::uint32_t lastExpectedStatus_{};
    InvalidationReason lastInvalidation_{InvalidationReason::None};
    std::uint32_t lastRenderedFrameSlot_{};
};

} // namespace ch30::post_processing::gpu
