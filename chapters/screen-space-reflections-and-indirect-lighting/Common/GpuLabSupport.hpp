#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ScreenSpaceReflectionContracts.hpp"

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
#include <string_view>
#include <vector>

// Chapter 29 paired GPU lab.
//
// The lab runs two compute passes and one display pass every frame:
//
//   1. GBufferCS resolves the deterministic analytic scene once per display pixel and publishes the only screen data
//      the reflection pass is allowed to see: device depth in a typed R32_FLOAT texture, and a structured surface
//      record holding the view normal, roughness, albedo, material identity, scene-linear outgoing radiance, and the
//      Chapter 28 motion convention previousUV - currentUV. The analytic scene is never consulted again.
//   2. ScreenSpaceCS consumes that screen data. The Starter shades the honest deferred baseline: direct outgoing
//      radiance plus a mirror-direction environment specular term with the split-sum weight applied exactly once.
//      The Solution additionally reconstructs the view position, traces a specular reflection ray through screen
//      depth, classifies the outcome, decomposes confidence, composes screen and environment radiance with weights
//      that sum to one, adds a bounded one-bounce diffuse screen-space estimate, and accumulates the result through
//      the Chapter 28 temporal conventions.
//   3. DisplayPS turns the per-pixel diagnostic record into an inspectable image.
//
// Traversal is deterministic perspective-correct linear marching with bisection refinement, matching
// TraceScreenSpaceRay in the CPU contracts. The hierarchical depth pyramid contracts remain a separate optimization
// contract: nothing in this lab implies that acceleration is running.

namespace ch29::screen_space_reflections::gpu
{

inline constexpr std::uint32_t kMaximumWidth = 400U;
inline constexpr std::uint32_t kMaximumHeight = 240U;

enum class LabVariant : std::uint8_t
{
    Starter,
    Solution,
};

enum class DebugView : std::uint32_t
{
    Baseline = 0U,
    LinearDepth = 1U,
    NormalRoughness = 2U,
    HitClassification = 3U,
    ConfidenceFactors = 4U,
    ConfidenceCombined = 5U,
    ScreenReflection = 6U,
    EnvironmentFallback = 7U,
    DiffuseIndirect = 8U,
    FinalComposition = 9U,
    TemporalHistory = 10U,
};

inline constexpr std::uint32_t kDebugViewCount = static_cast<std::uint32_t>(DebugView::TemporalHistory) + 1U;

enum class EnvironmentMode : std::uint32_t
{
    Analytic = 0U,
    ConstantUniform = 1U,
};

// Bits mirrored by ScreenSpaceReflectionShared.hlsli. Each stage sets its own bit only after publishing finite,
// in-domain evidence, so the display can reject an incompletely written pixel instead of drawing a plausible colour.
inline constexpr std::uint32_t kStatusGBuffer = 1U << 0U;
inline constexpr std::uint32_t kStatusReconstruction = 1U << 1U;
inline constexpr std::uint32_t kStatusBaseline = 1U << 2U;
inline constexpr std::uint32_t kStatusRay = 1U << 3U;
inline constexpr std::uint32_t kStatusTraversal = 1U << 4U;
inline constexpr std::uint32_t kStatusConfidence = 1U << 5U;
inline constexpr std::uint32_t kStatusComposition = 1U << 6U;
inline constexpr std::uint32_t kStatusIndirect = 1U << 7U;
inline constexpr std::uint32_t kStatusTemporal = 1U << 8U;
inline constexpr std::uint32_t kStarterValidStatus = kStatusGBuffer | kStatusReconstruction | kStatusBaseline;
inline constexpr std::uint32_t kSolutionValidStatus = (1U << 9U) - 1U;

// Per-pixel record flags.
inline constexpr std::uint32_t kRecordHit = 1U << 0U;
inline constexpr std::uint32_t kRecordStepBudgetExhausted = 1U << 1U;
inline constexpr std::uint32_t kRecordSegmentValid = 1U << 2U;
inline constexpr std::uint32_t kRecordHistoryUsable = 1U << 3U;
inline constexpr std::uint32_t kRecordBackground = 1U << 4U;

// History rejection bits. The first five mirror ch29::screen_space_reflections::HistoryRejection exactly; the last
// is lab-only and records that temporal accumulation was switched off before anything was sampled.
inline constexpr std::uint32_t kHistoryRejectNoHistory = static_cast<std::uint32_t>(HistoryRejection::NoHistory);
inline constexpr std::uint32_t kHistoryRejectOffScreen = static_cast<std::uint32_t>(HistoryRejection::OffScreen);
inline constexpr std::uint32_t kHistoryRejectNoSamples = static_cast<std::uint32_t>(HistoryRejection::NoSamples);
inline constexpr std::uint32_t kHistoryRejectMaterial = static_cast<std::uint32_t>(HistoryRejection::MaterialMismatch);
inline constexpr std::uint32_t kHistoryRejectDepth = static_cast<std::uint32_t>(HistoryRejection::DepthMismatch);
inline constexpr std::uint32_t kHistoryRejectDisabled = 1U << 5U;

// The shader stamps this into the final record word. Reading it back at a hard-coded byte offset proves the CPU and
// HLSL layouts still agree.
inline constexpr std::uint32_t kAbiMarker = 0x53535229U;

struct LabConfiguration final
{
    DebugView debugView{DebugView::FinalComposition};
    DepthConvention depthConvention{DepthConvention::Forward};
    EnvironmentMode environmentMode{EnvironmentMode::Analytic};
    std::uint32_t animationFrame{};
    std::uint32_t maximumStepCount{48U};
    std::uint32_t refinementStepCount{4U};
    std::uint32_t indirectSampleCount{6U};
    std::uint32_t indirectMaximumStepCount{20U};
    std::uint32_t splitSumSampleCount{16U};
    std::uint32_t maximumTemporalSampleCount{8U};
    float stepLengthTexels{3.0F};
    float startOffsetFraction{0.0F};
    float constantThickness{0.15F};
    float depthProportionalThickness{0.02F};
    float maximumRayDistance{24.0F};
    float indirectMaximumRayDistance{9.0F};
    // A usable history sample may never own the whole result; see TemporalReuseSettings::maximumHistoryWeight.
    float maximumHistoryWeight{0.95F};
    // History is only reused when the surface it describes is still the same surface. Depth agreement is decided in
    // linear view units with |expected - sampled| <= absolute + relative * max(expected, sampled).
    float absoluteDepthTolerance{0.05F};
    float relativeDepthTolerance{0.02F};
    bool materialIdentityRequired{true};
    bool screenTracingEnabled{true};
    bool indirectEnabled{true};
    bool temporalEnabled{true};
    bool resetHistory{};

    [[nodiscard]] bool operator==(LabConfiguration const &) const noexcept = default;
};

// One display pixel of byte-inspectable evidence. Every quantity is scene-linear or a pure geometric term; nothing
// here is tone mapped or gamma encoded.
struct PixelRecord final
{
    float deviceDepth{};
    float viewDepth{};
    float viewPositionX{};
    float viewPositionY{};
    float viewPositionZ{};
    float normalX{};
    float normalY{};
    float normalZ{};
    float roughness{};
    float albedoR{};
    float albedoG{};
    float albedoB{};
    float directR{};
    float directG{};
    float directB{};
    float motionX{};
    float motionY{};
    float rayDirectionX{};
    float rayDirectionY{};
    float rayDirectionZ{};
    float nDotV{};
    float appliedNormalBias{};
    float towardCameraCosine{};
    float segmentStartUvX{};
    float segmentStartUvY{};
    float segmentEndUvX{};
    float segmentEndUvY{};
    float segmentScreenLengthTexels{};
    float segmentExitDistance{};
    float hitUvX{};
    float hitUvY{};
    float hitRayViewDepth{};
    float hitSceneViewDepth{};
    float hitDepthDelta{};
    float hitThicknessInterval{};
    float hitRayDistance{};
    float hitParameter{};
    float confidenceValidity{};
    float confidenceScreenEdge{};
    float confidenceRayDistance{};
    float confidenceThickness{};
    float confidenceRoughness{};
    float confidenceTowardCamera{};
    float confidenceCombined{};
    float screenWeight{};
    float environmentWeight{};
    float screenRadianceR{};
    float screenRadianceG{};
    float screenRadianceB{};
    float environmentRadianceR{};
    float environmentRadianceG{};
    float environmentRadianceB{};
    float incomingRadianceR{};
    float incomingRadianceG{};
    float incomingRadianceB{};
    float splitSumA{};
    float splitSumB{};
    float specularWeightR{};
    float specularWeightG{};
    float specularWeightB{};
    float reflectionR{};
    float reflectionG{};
    float reflectionB{};
    float baselineReflectionR{};
    float baselineReflectionG{};
    float baselineReflectionB{};
    float indirectMeanIncomingR{};
    float indirectMeanIncomingG{};
    float indirectMeanIncomingB{};
    float indirectIrradianceR{};
    float indirectIrradianceG{};
    float indirectIrradianceB{};
    float indirectOutgoingR{};
    float indirectOutgoingG{};
    float indirectOutgoingB{};
    float indirectAverageCosineOverPdf{};
    float indirectConfidence{};
    float currentConfidence{};
    float previousUvX{};
    float previousUvY{};
    float previousViewDepth{};
    float currentWeight{};
    float historyWeight{};
    float historyR{};
    float historyG{};
    float historyB{};
    float historyConfidence{};
    float historyViewDepth{};
    float historyDepthDifference{};
    float historyDepthTolerance{};
    float outputConfidence{};
    float temporalR{};
    float temporalG{};
    float temporalB{};
    float baselineR{};
    float baselineG{};
    float baselineB{};
    float finalR{};
    float finalG{};
    float finalB{};
    std::uint32_t materialId{};
    std::uint32_t missReason{};
    std::uint32_t stepCount{};
    std::uint32_t refinementCount{};
    std::uint32_t geometrySampleCount{};
    std::uint32_t thicknessRejectionCount{};
    std::uint32_t requiredStepCount{};
    std::uint32_t indirectHitCount{};
    std::uint32_t indirectMissCount{};
    std::uint32_t indirectClampedCount{};
    std::uint32_t indirectSampleCount{};
    std::uint32_t previousSampleCount{};
    std::uint32_t nextSampleCount{};
    std::uint32_t historyMaterialId{};
    std::uint32_t historyRejectionReasons{};
    std::uint32_t flags{};
    std::uint32_t status{};
    std::uint32_t abiMarker{};

    [[nodiscard]] bool operator==(PixelRecord const &) const noexcept = default;
};
static_assert(sizeof(PixelRecord) == 472U);
static_assert(alignof(PixelRecord) == 4U);
static_assert(offsetof(PixelRecord, deviceDepth) == 0U);
static_assert(offsetof(PixelRecord, normalX) == 20U);
static_assert(offsetof(PixelRecord, rayDirectionX) == 68U);
static_assert(offsetof(PixelRecord, hitUvX) == 116U);
static_assert(offsetof(PixelRecord, confidenceValidity) == 148U);
static_assert(offsetof(PixelRecord, screenWeight) == 176U);
static_assert(offsetof(PixelRecord, splitSumA) == 220U);
static_assert(offsetof(PixelRecord, indirectMeanIncomingR) == 264U);
static_assert(offsetof(PixelRecord, indirectConfidence) == 304U);
static_assert(offsetof(PixelRecord, currentConfidence) == 308U);
static_assert(offsetof(PixelRecord, previousUvX) == 312U);
static_assert(offsetof(PixelRecord, previousViewDepth) == 320U);
static_assert(offsetof(PixelRecord, historyViewDepth) == 348U);
static_assert(offsetof(PixelRecord, outputConfidence) == 360U);
static_assert(offsetof(PixelRecord, finalR) == 388U);
static_assert(offsetof(PixelRecord, materialId) == 400U);
static_assert(offsetof(PixelRecord, historyMaterialId) == 452U);
static_assert(offsetof(PixelRecord, historyRejectionReasons) == 456U);
static_assert(offsetof(PixelRecord, flags) == 460U);
static_assert(offsetof(PixelRecord, status) == 464U);
static_assert(offsetof(PixelRecord, abiMarker) == 468U);

struct SurfaceRecord final
{
    float normalX{};
    float normalY{};
    float normalZ{};
    float roughness{};
    float albedoR{};
    float albedoG{};
    float albedoB{};
    float viewDepth{};
    float radianceR{};
    float radianceG{};
    float radianceB{};
    float motionX{};
    float motionY{};
    float previousViewDepth{};
    std::uint32_t materialId{};
    std::uint32_t padding{};
};
static_assert(sizeof(SurfaceRecord) == 64U);
static_assert(offsetof(SurfaceRecord, radianceR) == 32U);
static_assert(offsetof(SurfaceRecord, previousViewDepth) == 52U);
static_assert(offsetof(SurfaceRecord, materialId) == 56U);

struct HistoryPixel final
{
    float colorR{};
    float colorG{};
    float colorB{};
    float confidence{};
    float viewDepth{};
    std::uint32_t materialId{};
    std::uint32_t sampleCount{};
    std::uint32_t padding{};
};
static_assert(sizeof(HistoryPixel) == 32U);
static_assert(offsetof(HistoryPixel, viewDepth) == 16U);

struct EmitterCenter final
{
    float x{};
    float z{};

    [[nodiscard]] bool operator==(EmitterCenter const &) const noexcept = default;
};

struct FrameReadback final
{
    LabConfiguration configuration{};
    std::vector<PixelRecord> pixels{};
    std::vector<std::byte> rawBytes{};
    lgp::framework::Extent2D displaySize{};
    PerspectiveProjection projection{};
    EmitterCenter boxCenter{};
    EmitterCenter previousBoxCenter{};
    // The animation frame the reprojection was expressed against. It is the frame that produced the history the
    // shader was allowed to read, which is not necessarily animationFrame - 1.
    std::uint32_t previousAnimationFrame{};
    std::uint32_t frameSlot{};
    bool historyWasValid{};
};

struct BufferBarrierState final
{
    D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};
    D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};
};

[[nodiscard]] lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration);

// The CPU reference settings the renderer uploads. Tests reuse them so a contract replay is driven by the same
// numbers the shader received, not by a second transcription of them.
[[nodiscard]] PerspectiveProjection MakeProjection(LabConfiguration const &configuration,
                                                   lgp::framework::Extent2D extent) noexcept;
[[nodiscard]] RayConstructionSettings MakeRaySettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] TraceSettings MakeTraceSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] ConfidenceSettings MakeConfidenceSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] TemporalReuseSettings MakeTemporalSettings(LabConfiguration const &configuration) noexcept;
[[nodiscard]] EmitterCenter BoxCenter(std::uint32_t animationFrame) noexcept;

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
    void RequestHistoryReset() noexcept;
    [[nodiscard]] std::expected<FrameReadback, lgp::framework::Error> ReadBackOutputs();

  private:
    // The G-buffer, the diagnostic record, and its readback copy are frame-slot-owned because they are rewritten
    // every frame while an earlier frame may still be in flight. The two temporal history buffers are sequence-owned
    // at display resolution and alternate read and write roles, exactly as Chapter 28 requires.
    struct FrameSlotResources final
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> depth{};
        BufferResource surface{};
        BufferResource diagnostics{};
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
    [[nodiscard]] bool HistoryAffectingConfigurationChanged(LabConfiguration const &configuration) const noexcept;

    std::filesystem::path shaderPath_{};
    LabVariant variant_{};
    bool headless_{};
    bool hasRendered_{};
    bool historyValid_{};
    bool forceReset_{true};
    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader gbufferShader_{};
    lgp::framework::CompiledShader screenSpaceShader_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> gbufferPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> screenSpacePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
    std::vector<FrameSlotResources> frameSlots_{};
    std::array<BufferResource, 2U> history_{};
    std::array<BufferBarrierState, 2U> historyStates_{};
    std::uint32_t historyReadIndex_{};
    lgp::framework::Extent2D size_{};
    std::optional<LabConfiguration> headlessConfiguration_{};
    LabConfiguration interactiveConfiguration_{};
    LabConfiguration lastHistoryConfiguration_{};
    LabConfiguration lastRenderedConfiguration_{};
    std::uint32_t lastPreviousAnimationFrame_{};
    bool lastHistoryWasValid_{};
    std::uint32_t lastRenderedFrameSlot_{};
};

} // namespace ch29::screen_space_reflections::gpu
