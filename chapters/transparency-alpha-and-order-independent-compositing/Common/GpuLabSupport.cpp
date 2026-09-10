#include "GpuLabSupport.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>

namespace ch32::transparency::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorCount = 10U;
enum DescriptorIndex : UINT
{
    RecordsUav = 0U,
    FragmentsUav = 1U,
    FrameUav = 2U,
    RefractionSourceUav = 3U,
    PanesSrv = 4U,
    LightsSrv = 5U,
    LightIndicesSrv = 6U,
    ClustersSrv = 7U,
    DisplayRecordsSrv = 8U,
    DisplayFrameSrv = 9U,
};

enum RootParameter : UINT
{
    RootConstants = 0U,
    RootResources = 1U,
};

inline constexpr std::uint32_t kFlagAnimateStochastic = 1U << 0U;
inline constexpr std::uint32_t kFlagEnableRefraction = 1U << 1U;
inline constexpr std::uint32_t kFlagEnableReactiveMask = 1U << 2U;
inline constexpr std::uint32_t kFlagRejectForegroundOccluders = 1U << 3U;
inline constexpr std::uint32_t kFlagTestAgainstOpaqueDepth = 1U << 4U;
inline constexpr std::uint32_t kFlagWroteTransparentMotionVector = 1U << 5U;

// Byte offsets inside the per-frame staging buffer the four authored scene tables are copied from. They are
// 256-byte aligned so that a copy source is easy to read in a capture rather than because any API demands it.
inline constexpr std::uint64_t kUploadPanesOffset = 0U;
inline constexpr std::uint64_t kUploadLightsOffset = 1280U;
inline constexpr std::uint64_t kUploadClustersOffset = 1536U;
inline constexpr std::uint64_t kUploadLightIndicesOffset = 3328U;
inline constexpr std::uint64_t kUploadTotalBytes = 5888U;
inline constexpr std::uint64_t kPaneTableBytes = static_cast<std::uint64_t>(kLabPaneCount) * sizeof(PaneRecord);
inline constexpr std::uint64_t kLightTableBytes = static_cast<std::uint64_t>(kLabSceneLightCount) * sizeof(LightRecord);
inline constexpr std::uint64_t kClusterTableBytes =
    static_cast<std::uint64_t>(kMaximumLabClusterCount) * sizeof(ClusterRange);
inline constexpr std::uint64_t kLightIndexTableBytes =
    static_cast<std::uint64_t>(kMaximumLabLightIndexCount) * sizeof(std::uint32_t);
static_assert(kPaneTableBytes <= kUploadLightsOffset - kUploadPanesOffset);
static_assert(kLightTableBytes <= kUploadClustersOffset - kUploadLightsOffset);
static_assert(kClusterTableBytes <= kUploadLightIndicesOffset - kUploadClustersOffset);
static_assert(kLightIndexTableBytes <= kUploadTotalBytes - kUploadLightIndicesOffset);

// Mirrors the LabConstants cbuffer in TransparencyShared.hlsli field for field.
struct LabConstants final
{
    std::uint32_t displayWidth{};
    std::uint32_t displayHeight{};
    std::uint32_t variantId{};
    std::uint32_t sceneVariantId{};
    std::uint32_t activePaneCount{};
    std::uint32_t fragmentCapacity{};
    std::uint32_t overflowPolicyId{};
    std::uint32_t compositeModeId{};
    std::uint32_t oitTraversalId{};
    std::uint32_t oitWeightFunctionId{};
    std::uint32_t fogApplicationId{};
    std::uint32_t depthWritePolicyId{};
    std::uint32_t transferFunctionId{};
    std::uint32_t debugViewId{};
    std::uint32_t frameIndex{};
    std::uint32_t stochasticSampleCount{};
    std::uint32_t stochasticSeed{};
    std::uint32_t sceneLightCount{};
    std::uint32_t lightIndexCount{};
    std::uint32_t clusterTileCountX{};
    std::uint32_t clusterTileCountY{};
    std::uint32_t clusterSliceCount{};
    std::uint32_t clusterCount{};
    std::uint32_t stageOrderWord{};
    std::uint32_t stageCount{};
    std::uint32_t expectedStatus{};
    std::uint32_t flags{};
    std::uint32_t alphaTestThresholdFixed{};
    std::int32_t refractionOffsetTexelsX{};
    std::int32_t refractionOffsetTexelsY{};
    float alphaTestThreshold{};
    float uniformOitWeight{};
    float oitNearScaleMetres{};
    float oitFarScaleMetres{};
    float oitMinimumWeight{};
    float oitMaximumWeight{};
    float oitAlphaEpsilon{};
    float fogDensityPerMetre{};
    float fogInscatterR{};
    float fogInscatterG{};
    float fogInscatterB{};
    float refractionMaximumOffsetUv{};
    float reactiveAlphaWeight{};
    float reactiveRefractionWeight{};
    float reactiveStochasticWeight{};
    float reactiveReferenceOffsetUv{};
    float reactiveMaximumMask{};
    float displayExposureScale{};
    float clusterSliceDepthMetres{};
};
static_assert(sizeof(LabConstants) == 196U);
static_assert(sizeof(LabConstants) % sizeof(std::uint32_t) == 0U);
// A root signature holds 64 DWORDs. The constants plus the one descriptor table must fit, and the assertion is here
// so that adding a field is a build failure rather than a root-signature creation failure at runtime.
static_assert((sizeof(LabConstants) / sizeof(std::uint32_t)) + 1U <= 64U);

// A pane, spelled out once so the table below stays readable. Every rectangle is in the normalized 256-unit
// coordinate system, every depth slope is a negative power of two, and every colour and alpha is an integer number
// of 1 / kFixedOne, so the picture a test reasons about is the picture the dispatch drew to the last bit.
[[nodiscard]] constexpr PaneRecord MakePane(float baseDepth, float slopeU, float slopeV, std::uint32_t minU,
                                            std::uint32_t maxU, std::uint32_t minV, std::uint32_t maxV,
                                            std::uint32_t colorR, std::uint32_t colorG, std::uint32_t colorB,
                                            std::uint32_t alphaFixed, AlphaModulation modulation, CoverageMode mode,
                                            std::uint32_t paneIndex, std::uint32_t flags) noexcept
{
    return PaneRecord{.baseDepthMetres = baseDepth,
                      .depthSlopeU = slopeU,
                      .depthSlopeV = slopeV,
                      .paddingDepth = 0.0F,
                      .minU = minU,
                      .maxU = maxU,
                      .minV = minV,
                      .maxV = maxV,
                      .colorFixedR = colorR,
                      .colorFixedG = colorG,
                      .colorFixedB = colorB,
                      .alphaFixed = alphaFixed,
                      .alphaModulation = static_cast<std::uint32_t>(modulation),
                      .coverageMode = static_cast<std::uint32_t>(mode),
                      .drawOrder = paneIndex,
                      .primitiveId = 100U + paneIndex,
                      .paneIndex = paneIndex,
                      .flags = flags,
                      .paddingA = 0U,
                      .paddingB = 0U};
}

constexpr std::array<PaneRecord, kLabPaneCount> kPanes{{
    // 0 and 1: two sheets of glass whose depths cross at the middle of the frame. The far sheet falls from 12 m to
    // 8 m and the near one rises from 8 m to 12 m, so no single per-draw order is correct on both sides of the
    // crossing and the two are exactly coplanar on the column where they meet.
    MakePane(12.0F, -0.015625F, 0.0F, 0U, 256U, 0U, 256U, 128U, 256U, 768U, 256U, AlphaModulation::Constant,
             CoverageMode::AlphaBlend, 0U, 0U),
    MakePane(8.0F, 0.015625F, 0.0F, 0U, 256U, 0U, 256U, 768U, 128U, 128U, 512U, AlphaModulation::Constant,
             CoverageMode::AlphaBlend, 1U, 0U),
    // 2: an alpha-tested leaf card. Its checker alpha is above the default threshold on one cell and a quarter of it
    // on the next, so the test keeps a hard silhouette and destroys every gradation between.
    MakePane(6.0F, 0.0F, 0.0F, 48U, 208U, 32U, 160U, 896U, 768U, 256U, 640U, AlphaModulation::Checker,
             CoverageMode::AlphaTest, 2U, 0U),
    // 3: a stochastic foliage card at a constant alpha, so every pixel asks the same question and only the hash
    // decides the answer.
    MakePane(7.0F, 0.0F, 0.0F, 16U, 120U, 96U, 240U, 256U, 832U, 320U, 384U, AlphaModulation::Constant,
             CoverageMode::StochasticCoverage, 3U, 0U),
    // 4: the refracting pane. It sits beside the near block and against the right edge of the frame, so both
    // declared fallbacks are reachable from it.
    MakePane(5.0F, 0.0F, 0.0078125F, 96U, 256U, 0U, 144U, 256U, 768U, 512U, 384U, AlphaModulation::Constant,
             CoverageMode::AlphaBlend, 4U, kPaneRefracts),
    // 5: a far tint at 20 m. The wall runs from 24 m down to 16 m, so the opaque depth test rejects this pane over
    // exactly the half of the frame where the wall is nearer than it.
    MakePane(20.0F, 0.0F, 0.0F, 0U, 256U, 0U, 256U, 512U, 512U, 512U, 128U, AlphaModulation::Constant,
             CoverageMode::AlphaBlend, 5U, 0U),
    // 6, 7, 8: three panes that share a colour and differ in alpha and depth, and that sit inside one depth slice so
    // that the cluster they consume is the same and the colour they share really is shared. A weighted average of
    // one colour is that colour, so weighted blended OIT is exact here for a reason that has nothing to do with
    // fragment count.
    MakePane(9.0F, 0.0F, 0.0F, 0U, 256U, 0U, 256U, 512U, 256U, 128U, 256U, AlphaModulation::Constant,
             CoverageMode::AlphaBlend, 6U, 0U),
    MakePane(10.0F, 0.0F, 0.0F, 0U, 256U, 0U, 256U, 512U, 256U, 128U, 512U, AlphaModulation::Constant,
             CoverageMode::AlphaBlend, 7U, 0U),
    MakePane(11.0F, 0.0F, 0.0F, 0U, 256U, 0U, 256U, 512U, 256U, 128U, 128U, AlphaModulation::Constant,
             CoverageMode::AlphaBlend, 8U, 0U),
    // 9: a fully opaque blended fragment. Revealage becomes exactly zero and the background contributes nothing.
    MakePane(9.0F, 0.0F, 0.0F, 0U, 256U, 0U, 256U, 256U, 512U, 768U, 1024U, AlphaModulation::Constant,
             CoverageMode::AlphaBlend, 9U, 0U),
    // 10 and 11: a saturated red pane in front of a saturated blue one, both at alpha one half and both inside the
    // same depth slice so that they are lit by the same cluster. Under a uniform weight the two techniques agree on
    // the layer alpha exactly and disagree on the colour by exactly one eighth of the lit channel in two channels.
    MakePane(10.0F, 0.0F, 0.0F, 0U, 256U, 0U, 256U, 1024U, 0U, 0U, 512U, AlphaModulation::Constant,
             CoverageMode::AlphaBlend, 10U, 0U),
    MakePane(11.0F, 0.0F, 0.0F, 0U, 256U, 0U, 256U, 0U, 0U, 1024U, 512U, AlphaModulation::Constant,
             CoverageMode::AlphaBlend, 11U, 0U),
    // 12: the alpha-tested twin of pane 3. Same rectangle, same requested alpha, half a metre nearer, and a coverage
    // decision that answers the same question in a completely different way.
    MakePane(7.5F, 0.0F, 0.0F, 16U, 120U, 96U, 240U, 256U, 832U, 320U, 384U, AlphaModulation::Constant,
             CoverageMode::AlphaTest, 12U, 0U),
}};

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
    return std::string{what} + " reported ch32::transparency::ContractError code " +
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
            lgp::framework::MakeError("ValidateExtent", "Chapter 32 requires a non-empty extent up to 192x120."));
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

// The pane's view depth at the centre of its own rectangle, which is the representative depth the CPU's per-draw
// sort orders by. It is a per-object number by construction: one depth for a surface that has a different depth at
// every pixel, which is precisely why a per-draw order cannot be right everywhere.
[[nodiscard]] double RepresentativePaneDepth(PaneRecord const &pane) noexcept
{
    double const centreU = 0.5 * (static_cast<double>(pane.minU) + static_cast<double>(pane.maxU - 1U));
    double const centreV = 0.5 * (static_cast<double>(pane.minV) + static_cast<double>(pane.maxV - 1U));
    return static_cast<double>(pane.baseDepthMetres) + (static_cast<double>(pane.depthSlopeU) * centreU) +
           (static_cast<double>(pane.depthSlopeV) * centreV);
}

[[nodiscard]] bool IsFixedPointMultiple(float value) noexcept
{
    double const scaled = static_cast<double>(value) * static_cast<double>(kFixedOne);
    return std::isfinite(scaled) && scaled >= 0.0 && scaled <= static_cast<double>(kFixedOne) &&
           std::floor(scaled) == scaled;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Contract-typed settings. Every one of them is the value the dispatch actually received, so a test that replays a
// published fragment through the contract is driven by the same numbers rather than by a second transcription.
// ---------------------------------------------------------------------------------------------------------------

OitWeightSettings MakeOitWeightSettings(LabConfiguration const &configuration) noexcept
{
    return {.function = configuration.weightFunction,
            .uniformWeight = static_cast<double>(configuration.uniformOitWeight),
            .nearScaleMetres = static_cast<double>(configuration.oitNearScaleMetres),
            .farScaleMetres = static_cast<double>(configuration.oitFarScaleMetres),
            .minimumWeight = static_cast<double>(configuration.oitMinimumWeight),
            .maximumWeight = static_cast<double>(configuration.oitMaximumWeight)};
}

WeightedOitResolveSettings MakeResolveSettings(LabConfiguration const &configuration) noexcept
{
    return {.alphaEpsilon = static_cast<double>(configuration.oitAlphaEpsilon)};
}

AlphaTestSettings MakeAlphaTestSettings(LabConfiguration const &configuration) noexcept
{
    return {.threshold = static_cast<double>(configuration.alphaTestThreshold)};
}

StochasticCoverageSettings MakeStochasticSettings(LabConfiguration const &configuration) noexcept
{
    return {.sampleCount = configuration.stochasticSampleCount,
            .seed = configuration.stochasticSeed,
            .animateWithFrameIndex = configuration.animateStochasticWithFrameIndex};
}

FogSettings MakeFogSettings(LabConfiguration const &configuration) noexcept
{
    return {.densityPerMetre = static_cast<double>(configuration.fogDensityPerMetre),
            .inscatteringRadiance = {.r = static_cast<double>(configuration.fogInscatterR),
                                     .g = static_cast<double>(configuration.fogInscatterG),
                                     .b = static_cast<double>(configuration.fogInscatterB)}};
}

RefractionSettings MakeRefractionSettings(LabConfiguration const &configuration) noexcept
{
    return {.maximumOffsetUv = static_cast<double>(configuration.refractionMaximumOffsetUv),
            .rejectForegroundOccluders = configuration.rejectForegroundOccluders};
}

ReactiveMaskSettings MakeReactiveSettings(LabConfiguration const &configuration) noexcept
{
    return {.alphaWeight = static_cast<double>(configuration.reactiveAlphaWeight),
            .refractionWeight = static_cast<double>(configuration.reactiveRefractionWeight),
            .stochasticWeight = static_cast<double>(configuration.reactiveStochasticWeight),
            .referenceOffsetUv = static_cast<double>(configuration.reactiveReferenceOffsetUv),
            .maximumMask = static_cast<double>(configuration.reactiveMaximumMask)};
}

BoundedFragmentSettings MakeBoundedSettings(LabConfiguration const &configuration) noexcept
{
    return {.capacity = configuration.fragmentCapacity, .policy = configuration.overflowPolicy};
}

SortedCompositionSettings MakeSortedSettings(LabConfiguration const &configuration) noexcept
{
    return {.depthWrite = configuration.depthWrite, .testAgainstOpaqueDepth = configuration.testAgainstOpaqueDepth};
}

TransparencyPipelinePolicy MakePipelinePolicy(LabConfiguration const &configuration, LabVariant variant) noexcept
{
    return {.fog = configuration.fogApplication,
            // The Starter reads no refraction source, so it declares that it needs none rather than submitting a
            // copy it never samples.
            .requiresRefractionSource = variant == LabVariant::Solution,
            // Chapter 28 owns the resolve. This chapter publishes a reactive mask for it and does not run it, so the
            // policy says so instead of claiming a stage the frame does not contain.
            .requiresTemporalResolve = false};
}

// ---------------------------------------------------------------------------------------------------------------
// The clustered lighting seam. Chapter 14 builds all of this; the lab reproduces its layout so that a transparent
// fragment has a real light list to consume and a real way to consume the wrong one.
// ---------------------------------------------------------------------------------------------------------------

ClusterGridDescription MakeClusterGrid(lgp::framework::Extent2D extent) noexcept
{
    std::uint32_t const tilesX = GroupCount(extent.width, kClusterTileSize);
    std::uint32_t const tilesY = GroupCount(extent.height, kClusterTileSize);
    return {.tileCountX = tilesX,
            .tileCountY = tilesY,
            .sliceCount = kClusterSliceCount,
            .clusterCount = tilesX * tilesY * kClusterSliceCount};
}

std::uint32_t SliceIndexForDepth(double viewDepthMetres) noexcept
{
    if (!(viewDepthMetres > 0.0))
    {
        return 0U;
    }
    double const slice = std::floor(viewDepthMetres / kClusterSliceDepthMetres);
    if (slice >= static_cast<double>(kClusterSliceCount - 1U))
    {
        return kClusterSliceCount - 1U;
    }
    return static_cast<std::uint32_t>(slice);
}

std::vector<LightRecord> BuildLights(LabConfiguration const &configuration)
{
    std::vector<LightRecord> lights{};
    lights.resize(kLabSceneLightCount);
    for (std::uint32_t index = 0U; index < kLabSceneLightCount; ++index)
    {
        // Eighths, so a cluster's summed radiance is an exact dyadic rational and the lit colour a fragment carries
        // stays inside a float's mantissa through the whole composite.
        float const radiance = index < configuration.sceneLightCount ? (static_cast<float>(index + 1U) / 8.0F) : 0.0F;
        lights[index] = {.radianceR = radiance, .radianceG = radiance, .radianceB = radiance, .padding = 0.0F};
    }
    return lights;
}

std::vector<ClusterRange> BuildClusterRanges(ClusterGridDescription const &grid, std::uint32_t sceneLightCount)
{
    std::vector<ClusterRange> ranges{};
    ranges.resize(kMaximumLabClusterCount);
    std::uint32_t offset = 0U;
    for (std::uint32_t cluster = 0U; cluster < grid.clusterCount && cluster < kMaximumLabClusterCount; ++cluster)
    {
        std::uint32_t const count = std::min(1U + (cluster % 3U), sceneLightCount);
        ranges[cluster] = {.lightOffset = offset, .lightCount = count};
        offset += count;
    }
    return ranges;
}

std::vector<std::uint32_t> BuildLightIndices(ClusterGridDescription const &grid, std::uint32_t sceneLightCount)
{
    std::vector<std::uint32_t> indices{};
    indices.resize(kMaximumLabLightIndexCount);
    std::uint32_t offset = 0U;
    for (std::uint32_t cluster = 0U; cluster < grid.clusterCount && cluster < kMaximumLabClusterCount; ++cluster)
    {
        std::uint32_t const count = std::min(1U + (cluster % 3U), sceneLightCount);
        for (std::uint32_t slot = 0U; slot < count; ++slot)
        {
            indices[offset + slot] = (cluster + slot) % sceneLightCount;
        }
        offset += count;
    }
    indices.resize(offset);
    return indices;
}

// ---------------------------------------------------------------------------------------------------------------
// The authored scene.
// ---------------------------------------------------------------------------------------------------------------

std::uint32_t ScenePaneMask(SceneVariant scene) noexcept
{
    switch (scene)
    {
    case SceneVariant::Showcase:
        return 0x003FU;
    case SceneVariant::IntersectingPair:
        return 0x0003U;
    case SceneVariant::SinglePane:
        return 0x0002U;
    case SceneVariant::SharedColorStack:
        return 0x01C0U;
    case SceneVariant::KnownApproximation:
        return 0x0C00U;
    case SceneVariant::AllTransparentOpaque:
        return 0x0200U;
    case SceneVariant::CoverageContrast:
        return 0x1008U;
    case SceneVariant::RefractionField:
        return 0x0010U;
    case SceneVariant::OverflowStack:
        return 0x1FFFU;
    case SceneVariant::Empty:
    default:
        return 0x0000U;
    }
}

std::span<PaneRecord const> PaneTable() noexcept
{
    return std::span<PaneRecord const>{kPanes};
}

std::vector<PaneRecord> BuildActivePanes(LabConfiguration const &configuration)
{
    std::uint32_t const mask = ScenePaneMask(configuration.sceneVariant);
    std::vector<PaneRecord> active{};
    active.reserve(kLabPaneCount);
    for (std::uint32_t index = 0U; index < kLabPaneCount; ++index)
    {
        if ((mask & (1U << index)) != 0U)
        {
            active.push_back(kPanes[index]);
        }
    }
    // The declared per-draw order: descending representative depth, then ascending draw order. It is a total order
    // over the *draws*, which is the strongest thing a CPU sort can produce and is still not a per-pixel order.
    std::stable_sort(active.begin(), active.end(),
                     [](PaneRecord const &left, PaneRecord const &right) noexcept
                     {
                         double const leftDepth = RepresentativePaneDepth(left);
                         double const rightDepth = RepresentativePaneDepth(right);
                         if (leftDepth != rightDepth)
                         {
                             return leftDepth > rightDepth;
                         }
                         return left.drawOrder < right.drawOrder;
                     });
    return active;
}

// ---------------------------------------------------------------------------------------------------------------
// Stage lists and the status word.
// ---------------------------------------------------------------------------------------------------------------

std::uint32_t BuildExecutedStages(LabVariant variant, std::span<LabStage> stages) noexcept
{
    std::uint32_t count = 0U;
    auto const push = [&stages, &count](LabStage stage) noexcept
    {
        if (count < stages.size())
        {
            stages[count] = stage;
            ++count;
        }
    };
    push(LabStage::Clear);
    push(LabStage::Opaque);
    if (variant == LabVariant::Solution)
    {
        push(LabStage::RefractionSource);
    }
    push(LabStage::Fragments);
    push(LabStage::ForwardComposite);
    if (variant == LabVariant::Solution)
    {
        push(LabStage::OitAccumulate);
        push(LabStage::OitResolve);
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

std::uint32_t BuildFrameStages(LabConfiguration const &configuration, LabVariant variant,
                               std::span<FrameStage> stages) noexcept
{
    std::uint32_t count = 0U;
    auto const push = [&stages, &count](FrameStage stage) noexcept
    {
        if (count < stages.size())
        {
            stages[count] = stage;
            ++count;
        }
    };
    push(FrameStage::OpaqueGBuffer);
    push(FrameStage::OpaqueLighting);
    if (variant == LabVariant::Solution)
    {
        push(FrameStage::RefractionSourceCopy);
    }
    push(FrameStage::TransparentComposite);
    if (configuration.fogApplication == FogApplication::ScreenSpaceAfterComposite)
    {
        push(FrameStage::ScreenSpaceFog);
    }
    // The exposure and the tone curve belong to Chapters 31 and 30. They appear here because the frame genuinely
    // performs them, after the composite, which is the only ordering that leaves the transparent layer exposed
    // identically to the scene behind it.
    push(FrameStage::Exposure);
    push(FrameStage::ToneMap);
    return count;
}

std::uint32_t ExpectedStatus(LabConfiguration const &configuration, LabVariant variant) noexcept
{
    std::uint32_t status = kStatusOpaque | kStatusFragments | kStatusObjectOrdered | kStatusForwardComposite |
                           kStatusExposure | kStatusToneMap | kStatusDisplayEncode;
    status |= configuration.fogApplication == FogApplication::ScreenSpaceAfterComposite ? kStatusFogScreenSpace
                                                                                        : kStatusFogPerFragment;
    if (variant == LabVariant::Starter)
    {
        return status | kStatusBlendOnly;
    }
    status |= kStatusRefractionSource | kStatusPerPixelSorted | kStatusOitAccumulated | kStatusOitResolved;
    if (configuration.enableReactiveMask)
    {
        status |= kStatusReactiveMask;
    }
    return status;
}

// ---------------------------------------------------------------------------------------------------------------
// Configuration validation. Every refusal here is a claim the variant makes about itself, so a Starter that grew a
// weighted accumulator would stop configuring rather than quietly become a Solution.
// ---------------------------------------------------------------------------------------------------------------

LabConfiguration DefaultConfiguration(LabVariant variant) noexcept
{
    LabConfiguration configuration{};
    if (variant == LabVariant::Starter)
    {
        configuration.compositeMode = CompositeMode::ObjectSorted;
        configuration.weightFunction = OitWeightFunction::Uniform;
        configuration.oitTraversal = OitTraversal::Stored;
        configuration.overflowPolicy = FragmentOverflowPolicy::KeepNearest;
        configuration.fogApplication = FogApplication::PerFragmentBeforeComposite;
        configuration.depthWrite = DepthWritePolicy::Never;
        configuration.fragmentCapacity = kLabFragmentCapacity;
        configuration.fogDensityPerMetre = 0.0F;
        configuration.enableRefraction = false;
        configuration.enableReactiveMask = false;
        return configuration;
    }
    configuration.compositeMode = CompositeMode::SortedReference;
    configuration.depthWrite = DepthWritePolicy::CoverageDecided;
    return configuration;
}

lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration, LabVariant variant)
{
    auto const refuse = [](char const *message) -> lgp::framework::Status
    { return std::unexpected(lgp::framework::MakeError("ValidateLabConfiguration", message)); };

    if (static_cast<std::uint32_t>(configuration.debugView) >= kDebugViewCount)
    {
        return refuse("Chapter 32 refuses an unknown debug view.");
    }
    if (static_cast<std::uint32_t>(configuration.sceneVariant) >= kSceneVariantCount)
    {
        return refuse("Chapter 32 refuses an unknown scene variant.");
    }
    if (configuration.fragmentCapacity == 0U || configuration.fragmentCapacity > kLabFragmentCapacity)
    {
        return refuse("Chapter 32 refuses a per-pixel fragment capacity outside the store it allocated.");
    }
    if (configuration.stochasticSampleCount == 0U || configuration.stochasticSampleCount > kMaximumStochasticSamples)
    {
        return refuse("Chapter 32 refuses a stochastic sample count outside the published mask width.");
    }
    if (configuration.sceneLightCount == 0U || configuration.sceneLightCount > kLabSceneLightCount)
    {
        return refuse("Chapter 32 refuses a scene light count the uploaded light table cannot describe.");
    }
    // The alpha test compares fixed-point integers so that the CPU reference and the dispatch cannot disagree about
    // a boundary alpha. A threshold that is not an exact multiple of 1 / kFixedOne has no such integer.
    if (!IsFixedPointMultiple(configuration.alphaTestThreshold))
    {
        return refuse("Chapter 32 requires an alpha-test threshold that is an exact multiple of 1 / 1024.");
    }
    if (!std::isfinite(configuration.displayExposureScale) || configuration.displayExposureScale <= 0.0F)
    {
        return refuse("Chapter 32 requires a positive, finite display exposure scale.");
    }

    auto const weight = OitFragmentWeight(1.0, MakeOitWeightSettings(configuration));
    if (!weight)
    {
        return std::unexpected(MakeContractError("ValidateLabConfiguration", "OitFragmentWeight", weight.error()));
    }
    auto const resolve = ResolveWeightedOit({}, {}, MakeResolveSettings(configuration));
    if (!resolve)
    {
        return std::unexpected(MakeContractError("ValidateLabConfiguration", "ResolveWeightedOit", resolve.error()));
    }
    auto const alphaTest = EvaluateAlphaTest({}, MakeAlphaTestSettings(configuration));
    if (!alphaTest)
    {
        return std::unexpected(MakeContractError("ValidateLabConfiguration", "EvaluateAlphaTest", alphaTest.error()));
    }
    auto const coverage =
        EvaluateStochasticCoverage({}, {}, configuration.frameIndex, MakeStochasticSettings(configuration));
    if (!coverage)
    {
        return std::unexpected(
            MakeContractError("ValidateLabConfiguration", "EvaluateStochasticCoverage", coverage.error()));
    }
    auto const fog = FogTransmittance(1.0, MakeFogSettings(configuration));
    if (!fog)
    {
        return std::unexpected(MakeContractError("ValidateLabConfiguration", "FogTransmittance", fog.error()));
    }
    RefractionInput const probe{.surfaceUv = {},
                                .requestedOffsetUv = {},
                                .sourceExtent = {.width = kMaximumWidth, .height = kMaximumHeight},
                                .surfaceViewDepthMetres = 1.0,
                                .offsetOpaque = {}};
    auto const refraction = ComputeRefractionSample(probe, MakeRefractionSettings(configuration));
    if (!refraction)
    {
        return std::unexpected(
            MakeContractError("ValidateLabConfiguration", "ComputeRefractionSample", refraction.error()));
    }
    auto const reactive = ComputeReactiveMask({}, MakeReactiveSettings(configuration));
    if (!reactive)
    {
        return std::unexpected(MakeContractError("ValidateLabConfiguration", "ComputeReactiveMask", reactive.error()));
    }

    std::array<FrameStage, kMaximumStageCount> frameStages{};
    std::uint32_t const frameStageCount = BuildFrameStages(configuration, variant, frameStages);
    auto const plan = PlanTransparencyOrder(std::span<FrameStage const>{frameStages.data(), frameStageCount},
                                            MakePipelinePolicy(configuration, variant));
    if (!plan)
    {
        return std::unexpected(MakeContractError("ValidateLabConfiguration", "PlanTransparencyOrder", plan.error()));
    }
    if (!plan->valid)
    {
        return refuse("Chapter 32 refuses a submitted stage order the transparency contract reports as invalid.");
    }

    if (variant != LabVariant::Starter)
    {
        return {};
    }

    // Starter refusals. Each one is the claim the Starter's own comments make, enforced rather than asserted.
    if (configuration.compositeMode != CompositeMode::ObjectSorted)
    {
        return refuse("The Chapter 32 Starter owns only the CPU's per-draw composite.");
    }
    if (configuration.depthWrite != DepthWritePolicy::Never)
    {
        return refuse("The Chapter 32 Starter blends every fragment, so nothing it draws may write depth.");
    }
    if (configuration.fragmentCapacity != kLabFragmentCapacity)
    {
        return refuse("The Chapter 32 Starter owns no bounded store, so it cannot declare a smaller capacity.");
    }
    if (configuration.fogApplication != FogApplication::PerFragmentBeforeComposite)
    {
        return refuse("The Chapter 32 Starter owns no screen-space fog pass.");
    }
    if (configuration.fogDensityPerMetre != 0.0F)
    {
        return refuse("The Chapter 32 Starter folds no fog into its fragments.");
    }
    if (configuration.enableRefraction)
    {
        return refuse("The Chapter 32 Starter copies no refraction source and samples none.");
    }
    if (configuration.enableReactiveMask)
    {
        return refuse("The Chapter 32 Starter publishes no reactive mask.");
    }
    return {};
}

// ---------------------------------------------------------------------------------------------------------------
// Resources and the frame.
// ---------------------------------------------------------------------------------------------------------------

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
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 32 buffers must be non-empty."));
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
    description.Format = DXGI_FORMAT_UNKNOWN;
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
                                                                createResult, "Failed to create Chapter 32 buffer."));
    }
    if (!name.empty())
    {
        std::wstring const ownedName{name};
        if (HRESULT const nameResult = result.resource_->SetName(ownedName.c_str()); FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name Chapter 32 buffer."));
        }
    }
    if (mapPersistently)
    {
        void *mapped = nullptr;
        D3D12_RANGE const emptyRange{0U, 0U};
        HRESULT const mapResult =
            result.resource_->Map(0U, heapType == D3D12_HEAP_TYPE_READBACK ? nullptr : &emptyRange, &mapped);
        if (FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map a Chapter 32 buffer."));
        }
        result.mappedData_ = static_cast<std::byte *>(mapped);
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
    if (auto status = CompileShader(compiler, options, L"OpaqueCS", L"cs_6_0", opaqueShader_); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"FragmentsCS", L"cs_6_0", fragmentsShader_); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"ForwardCompositeCS", L"cs_6_0", forwardShader_); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"ComposeCS", L"cs_6_0", composeShader_); !status)
    {
        return status;
    }
    if (variant_ == LabVariant::Solution)
    {
        if (auto status = CompileShader(compiler, options, L"RefractionSourceCS", L"cs_6_0", refractionSourceShader_);
            !status)
        {
            return status;
        }
        if (auto status = CompileShader(compiler, options, L"OitAccumulateCS", L"cs_6_0", oitAccumulateShader_);
            !status)
        {
            return status;
        }
        if (auto status = CompileShader(compiler, options, L"OitResolveCS", L"cs_6_0", oitResolveShader_); !status)
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
    D3D12_DESCRIPTOR_RANGE computeRanges[2]{};
    computeRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    computeRanges[0].NumDescriptors = 4U;
    computeRanges[0].BaseShaderRegister = 0U;
    computeRanges[0].OffsetInDescriptorsFromTableStart = RecordsUav;
    computeRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    computeRanges[1].NumDescriptors = 4U;
    computeRanges[1].BaseShaderRegister = 0U;
    computeRanges[1].OffsetInDescriptorsFromTableStart = PanesSrv;

    D3D12_ROOT_PARAMETER computeParameters[2]{};
    computeParameters[RootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[RootConstants].Constants.ShaderRegister = 0U;
    computeParameters[RootConstants].Constants.Num32BitValues = sizeof(LabConstants) / sizeof(std::uint32_t);
    computeParameters[RootResources].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[RootResources].DescriptorTable = {static_cast<UINT>(std::size(computeRanges)), computeRanges};
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
                                                                "Failed to create Chapter 32 compute root signature."));
    }

    D3D12_DESCRIPTOR_RANGE displayRange{};
    displayRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    displayRange.NumDescriptors = 2U;
    displayRange.BaseShaderRegister = 4U;
    displayRange.OffsetInDescriptorsFromTableStart = 0U;
    D3D12_ROOT_PARAMETER graphicsParameters[2]{};
    graphicsParameters[RootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[RootConstants].Constants.ShaderRegister = 0U;
    graphicsParameters[RootConstants].Constants.Num32BitValues = sizeof(LabConstants) / sizeof(std::uint32_t);
    graphicsParameters[RootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    graphicsParameters[RootResources].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParameters[RootResources].DescriptorTable = {1U, &displayRange};
    graphicsParameters[RootResources].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
            "ID3D12Device::CreateRootSignature", createResult, "Failed to create Chapter 32 graphics root signature."));
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
                                                                    "Failed to create a Chapter 32 compute PSO."));
        }
        return {};
    };

    if (auto status = createCompute(clearShader_, clearPipeline_); !status)
    {
        return status;
    }
    if (auto status = createCompute(opaqueShader_, opaquePipeline_); !status)
    {
        return status;
    }
    if (auto status = createCompute(fragmentsShader_, fragmentsPipeline_); !status)
    {
        return status;
    }
    if (auto status = createCompute(forwardShader_, forwardPipeline_); !status)
    {
        return status;
    }
    if (auto status = createCompute(composeShader_, composePipeline_); !status)
    {
        return status;
    }
    if (variant_ == LabVariant::Solution)
    {
        if (auto status = createCompute(refractionSourceShader_, refractionSourcePipeline_); !status)
        {
            return status;
        }
        if (auto status = createCompute(oitAccumulateShader_, oitAccumulatePipeline_); !status)
        {
            return status;
        }
        if (auto status = createCompute(oitResolveShader_, oitResolvePipeline_); !status)
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
                                                                "Failed to create Chapter 32 graphics PSO."));
    }
    return {};
}

lgp::framework::Status RendererCore::CreateResources(lgp::framework::Extent2D size)
{
    if (auto status = ValidateExtent(size); !status)
    {
        return status;
    }

    ID3D12Device10 &device = *deviceResources_->device();
    std::uint64_t const pixelCount = static_cast<std::uint64_t>(size.width) * size.height;
    std::uint64_t const recordBytes = pixelCount * sizeof(PixelRecord);
    std::uint64_t const fragmentBytes = pixelCount * kLabFragmentCapacity * sizeof(FragmentRecord);
    std::uint64_t const sourceBytes = pixelCount * (4U * sizeof(float));

    auto panes =
        CreateBuffer(device, kPaneTableBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, L"Ch32 pane table");
    if (!panes)
    {
        return std::unexpected(std::move(panes.error()));
    }
    auto lights =
        CreateBuffer(device, kLightTableBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, L"Ch32 light table");
    if (!lights)
    {
        return std::unexpected(std::move(lights.error()));
    }
    auto lightIndices = CreateBuffer(device, kLightIndexTableBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                                     L"Ch32 cluster light indices");
    if (!lightIndices)
    {
        return std::unexpected(std::move(lightIndices.error()));
    }
    auto clusters = CreateBuffer(device, kClusterTableBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                                 L"Ch32 cluster ranges");
    if (!clusters)
    {
        return std::unexpected(std::move(clusters.error()));
    }
    panes_ = std::move(*panes);
    lights_ = std::move(*lights);
    lightIndices_ = std::move(*lightIndices);
    clusters_ = std::move(*clusters);
    sceneInitialized_ = false;

    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlotResources &slot : frameSlots_)
    {
        auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorCount);
        if (!descriptors)
        {
            return std::unexpected(std::move(descriptors.error()));
        }
        auto records = CreateBuffer(device, recordBytes, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch32 pixel records");
        if (!records)
        {
            return std::unexpected(std::move(records.error()));
        }
        auto fragments = CreateBuffer(device, fragmentBytes, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch32 fragment store");
        if (!fragments)
        {
            return std::unexpected(std::move(fragments.error()));
        }
        auto frame = CreateBuffer(device, sizeof(FrameRecord), D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch32 frame record");
        if (!frame)
        {
            return std::unexpected(std::move(frame.error()));
        }
        auto refractionSource = CreateBuffer(device, sourceBytes, D3D12_HEAP_TYPE_DEFAULT,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch32 refraction source");
        if (!refractionSource)
        {
            return std::unexpected(std::move(refractionSource.error()));
        }
        auto recordsReadback = CreateBuffer(device, recordBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                            L"Ch32 record readback", true);
        if (!recordsReadback)
        {
            return std::unexpected(std::move(recordsReadback.error()));
        }
        auto fragmentsReadback = CreateBuffer(device, fragmentBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                              L"Ch32 fragment readback", true);
        if (!fragmentsReadback)
        {
            return std::unexpected(std::move(fragmentsReadback.error()));
        }
        auto frameReadback = CreateBuffer(device, sizeof(FrameRecord), D3D12_HEAP_TYPE_READBACK,
                                          D3D12_RESOURCE_FLAG_NONE, L"Ch32 frame readback", true);
        if (!frameReadback)
        {
            return std::unexpected(std::move(frameReadback.error()));
        }
        // One upload buffer per frame slot. A single shared one would be written by the CPU for the next frame
        // while the copy of a frame still in flight was reading it, and the only cure for that with a shared
        // buffer is a device-wide wait the lab must not pay every frame.
        auto sceneUpload = CreateBuffer(device, kUploadTotalBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                        L"Ch32 scene upload", true);
        if (!sceneUpload)
        {
            return std::unexpected(std::move(sceneUpload.error()));
        }

        slot.descriptors = *descriptors;
        slot.records = std::move(*records);
        slot.fragments = std::move(*fragments);
        slot.frame = std::move(*frame);
        slot.refractionSource = std::move(*refractionSource);
        slot.recordsReadback = std::move(*recordsReadback);
        slot.fragmentsReadback = std::move(*fragmentsReadback);
        slot.frameReadback = std::move(*frameReadback);
        slot.sceneUpload = std::move(*sceneUpload);
        slot.initialized = false;

        auto const createStructuredUav = [&device, &slot](DescriptorIndex index, ID3D12Resource *resource,
                                                          std::uint64_t elements, std::uint32_t stride)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = static_cast<UINT>(elements);
            uav.Buffer.StructureByteStride = stride;
            device.CreateUnorderedAccessView(resource, nullptr, &uav, slot.descriptors.CpuHandle(index));
        };
        auto const createStructuredSrv = [&device, &slot](DescriptorIndex index, ID3D12Resource *resource,
                                                          std::uint64_t elements, std::uint32_t stride)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.NumElements = static_cast<UINT>(elements);
            srv.Buffer.StructureByteStride = stride;
            device.CreateShaderResourceView(resource, &srv, slot.descriptors.CpuHandle(index));
        };
        createStructuredUav(RecordsUav, slot.records.Get(), pixelCount, sizeof(PixelRecord));
        createStructuredUav(FragmentsUav, slot.fragments.Get(), pixelCount * kLabFragmentCapacity,
                            sizeof(FragmentRecord));
        createStructuredUav(FrameUav, slot.frame.Get(), 1U, sizeof(FrameRecord));
        createStructuredUav(RefractionSourceUav, slot.refractionSource.Get(), pixelCount, 4U * sizeof(float));
        createStructuredSrv(PanesSrv, panes_.Get(), kLabPaneCount, sizeof(PaneRecord));
        createStructuredSrv(LightsSrv, lights_.Get(), kLabSceneLightCount, sizeof(LightRecord));
        createStructuredSrv(LightIndicesSrv, lightIndices_.Get(), kMaximumLabLightIndexCount, sizeof(std::uint32_t));
        createStructuredSrv(ClustersSrv, clusters_.Get(), kMaximumLabClusterCount, sizeof(ClusterRange));
        createStructuredSrv(DisplayRecordsSrv, slot.records.Get(), pixelCount, sizeof(PixelRecord));
        createStructuredSrv(DisplayFrameSrv, slot.frame.Get(), 1U, sizeof(FrameRecord));
    }
    size_ = size;
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
    // Every buffer about to be destroyed is frame-slot-owned and may still be referenced by a submitted frame, and
    // a headless resize does not drain the queue on the caller's behalf. This is the one place the lab waits: a
    // resize is not a frame, so waiting here costs nothing per frame and makes the destruction below correct
    // without depending on what the caller did first.
    if (auto status = deviceResources.WaitForGpuIdle(); !status)
    {
        return status;
    }
    DestroyResources(deviceResources);
    return CreateResources(drawableSize);
}

LabConfiguration RendererCore::ActiveConfiguration() const noexcept
{
    return headlessConfiguration_.has_value() ? *headlessConfiguration_ : interactiveConfiguration_;
}

lgp::framework::Status RendererCore::Update(lgp::framework::UpdateContext const &context)
{
    if (headless_)
    {
        return {};
    }
    LabConfiguration configuration = interactiveConfiguration_;
    auto const cycle = [](std::uint32_t value, std::uint32_t count) noexcept { return (value + 1U) % count; };
    if (context.input.WasKeyPressed('V'))
    {
        configuration.debugView =
            static_cast<DebugView>(cycle(static_cast<std::uint32_t>(configuration.debugView), kDebugViewCount));
    }
    if (context.input.WasKeyPressed('S'))
    {
        configuration.sceneVariant = static_cast<SceneVariant>(
            cycle(static_cast<std::uint32_t>(configuration.sceneVariant), kSceneVariantCount));
    }
    if (context.input.WasKeyPressed('M') && variant_ == LabVariant::Solution)
    {
        configuration.compositeMode =
            static_cast<CompositeMode>(cycle(static_cast<std::uint32_t>(configuration.compositeMode), 3U));
    }
    if (context.input.WasKeyPressed('O') && variant_ == LabVariant::Solution)
    {
        configuration.oitTraversal =
            static_cast<OitTraversal>(cycle(static_cast<std::uint32_t>(configuration.oitTraversal), 5U));
    }
    if (context.input.WasKeyPressed('W') && variant_ == LabVariant::Solution)
    {
        configuration.weightFunction = configuration.weightFunction == OitWeightFunction::Uniform
                                           ? OitWeightFunction::InverseDepthPolynomial
                                           : OitWeightFunction::Uniform;
    }
    if (context.input.WasKeyPressed('F') && variant_ == LabVariant::Solution)
    {
        configuration.fogApplication = configuration.fogApplication == FogApplication::PerFragmentBeforeComposite
                                           ? FogApplication::ScreenSpaceAfterComposite
                                           : FogApplication::PerFragmentBeforeComposite;
    }
    if (context.input.WasKeyPressed('G') && variant_ == LabVariant::Solution)
    {
        configuration.fogDensityPerMetre = configuration.fogDensityPerMetre > 0.0F ? 0.0F : 0.02F;
    }
    if (context.input.WasKeyPressed('K') && variant_ == LabVariant::Solution)
    {
        configuration.fragmentCapacity =
            configuration.fragmentCapacity <= 1U ? kLabFragmentCapacity : (configuration.fragmentCapacity - 1U);
    }
    configuration.frameIndex = static_cast<std::uint32_t>(context.frameIndex & 0xFFFF'FFFFULL);
    if (auto status = ValidateLabConfiguration(configuration, variant_); !status)
    {
        // A refused interactive request leaves the previous configuration in place rather than rendering a frame the
        // variant has said it does not own.
        interactiveConfiguration_.frameIndex = configuration.frameIndex;
        return {};
    }
    interactiveConfiguration_ = configuration;
    return {};
}

void RendererCore::ConfigureHeadlessTest(LabConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
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
    panes_ = {};
    lights_ = {};
    lightIndices_ = {};
    clusters_ = {};
    sceneInitialized_ = false;
    hasRendered_ = false;
    lastSceneUploadGpuAddress_ = 0U;
    lastSceneUploadCpuAddress_ = 0U;
    size_ = {};
}

void RendererCore::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    (void)deviceResources.WaitForGpuIdle();
    DestroyResources(deviceResources);
    clearPipeline_.Reset();
    opaquePipeline_.Reset();
    refractionSourcePipeline_.Reset();
    fragmentsPipeline_.Reset();
    forwardPipeline_.Reset();
    oitAccumulatePipeline_.Reset();
    oitResolvePipeline_.Reset();
    composePipeline_.Reset();
    graphicsPipeline_.Reset();
    computeRootSignature_.Reset();
    graphicsRootSignature_.Reset();
    deviceResources_ = nullptr;
}

lgp::framework::Status RendererCore::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Render", "Chapter 32 frame slot is out of range."));
    }
    LabConfiguration const configuration = ActiveConfiguration();
    if (auto status = ValidateLabConfiguration(configuration, variant_); !status)
    {
        return status;
    }

    ClusterGridDescription const grid = MakeClusterGrid(size_);
    if (auto const valid = ValidateClusterGrid(grid); !valid)
    {
        return std::unexpected(MakeContractError("Render", "ValidateClusterGrid", valid.error()));
    }
    if (grid.clusterCount > kMaximumLabClusterCount)
    {
        return std::unexpected(
            lgp::framework::MakeError("Render", "Chapter 32 cluster count exceeds the allocated cluster tables."));
    }

    std::vector<PaneRecord> const activePanes = BuildActivePanes(configuration);
    std::vector<LightRecord> const lights = BuildLights(configuration);
    std::vector<ClusterRange> const clusterRanges = BuildClusterRanges(grid, configuration.sceneLightCount);
    std::vector<std::uint32_t> const lightIndices = BuildLightIndices(grid, configuration.sceneLightCount);
    if (lightIndices.size() > kMaximumLabLightIndexCount)
    {
        return std::unexpected(
            lgp::framework::MakeError("Render", "Chapter 32 light index count exceeds the allocated table."));
    }

    std::array<FrameStage, kMaximumStageCount> frameStages{};
    std::uint32_t const frameStageCount = BuildFrameStages(configuration, variant_, frameStages);
    auto const plan = PlanTransparencyOrder(std::span<FrameStage const>{frameStages.data(), frameStageCount},
                                            MakePipelinePolicy(configuration, variant_));
    if (!plan)
    {
        return std::unexpected(MakeContractError("Render", "PlanTransparencyOrder", plan.error()));
    }
    if (!plan->valid)
    {
        return std::unexpected(
            lgp::framework::MakeError("Render", "Chapter 32 refuses to submit an invalid transparency stage order."));
    }

    std::array<LabStage, kMaximumLabStageCount> stages{};
    std::uint32_t const stageCount = BuildExecutedStages(variant_, stages);
    std::uint32_t const stageOrderWord = EncodeStageOrder(std::span<LabStage const>{stages.data(), stageCount});
    std::uint32_t const expectedStatus = ExpectedStatus(configuration, variant_);

    std::uint32_t flags = 0U;
    flags |= configuration.animateStochasticWithFrameIndex ? kFlagAnimateStochastic : 0U;
    flags |= configuration.enableRefraction ? kFlagEnableRefraction : 0U;
    flags |= configuration.enableReactiveMask ? kFlagEnableReactiveMask : 0U;
    flags |= configuration.rejectForegroundOccluders ? kFlagRejectForegroundOccluders : 0U;
    flags |= configuration.testAgainstOpaqueDepth ? kFlagTestAgainstOpaqueDepth : 0U;
    flags |= configuration.wroteTransparentMotionVector ? kFlagWroteTransparentMotionVector : 0U;

    LabConstants constants{};
    constants.displayWidth = size_.width;
    constants.displayHeight = size_.height;
    constants.variantId = static_cast<std::uint32_t>(variant_);
    constants.sceneVariantId = static_cast<std::uint32_t>(configuration.sceneVariant);
    constants.activePaneCount = static_cast<std::uint32_t>(activePanes.size());
    constants.fragmentCapacity = configuration.fragmentCapacity;
    constants.overflowPolicyId = static_cast<std::uint32_t>(configuration.overflowPolicy);
    constants.compositeModeId = static_cast<std::uint32_t>(configuration.compositeMode);
    constants.oitTraversalId = static_cast<std::uint32_t>(configuration.oitTraversal);
    constants.oitWeightFunctionId = static_cast<std::uint32_t>(configuration.weightFunction);
    constants.fogApplicationId = static_cast<std::uint32_t>(configuration.fogApplication);
    constants.depthWritePolicyId = static_cast<std::uint32_t>(configuration.depthWrite);
    constants.transferFunctionId = static_cast<std::uint32_t>(configuration.transferFunction);
    constants.debugViewId = static_cast<std::uint32_t>(configuration.debugView);
    constants.frameIndex = configuration.frameIndex;
    constants.stochasticSampleCount = configuration.stochasticSampleCount;
    constants.stochasticSeed = configuration.stochasticSeed;
    constants.sceneLightCount = configuration.sceneLightCount;
    constants.lightIndexCount = static_cast<std::uint32_t>(lightIndices.size());
    constants.clusterTileCountX = grid.tileCountX;
    constants.clusterTileCountY = grid.tileCountY;
    constants.clusterSliceCount = grid.sliceCount;
    constants.clusterCount = grid.clusterCount;
    constants.stageOrderWord = stageOrderWord;
    constants.stageCount = stageCount;
    constants.expectedStatus = expectedStatus;
    constants.flags = flags;
    // The threshold the shader compares against is quantized here, by the same rule validation enforced, so the
    // dispatch never takes a coverage decision on a raw float whose last bit could move a boundary.
    constants.alphaTestThresholdFixed = static_cast<std::uint32_t>(
        std::lround(static_cast<double>(configuration.alphaTestThreshold) * static_cast<double>(kFixedOne)));
    constants.refractionOffsetTexelsX = configuration.refractionOffsetTexelsX;
    constants.refractionOffsetTexelsY = configuration.refractionOffsetTexelsY;
    constants.alphaTestThreshold = configuration.alphaTestThreshold;
    constants.uniformOitWeight = configuration.uniformOitWeight;
    constants.oitNearScaleMetres = configuration.oitNearScaleMetres;
    constants.oitFarScaleMetres = configuration.oitFarScaleMetres;
    constants.oitMinimumWeight = configuration.oitMinimumWeight;
    constants.oitMaximumWeight = configuration.oitMaximumWeight;
    constants.oitAlphaEpsilon = configuration.oitAlphaEpsilon;
    constants.fogDensityPerMetre = configuration.fogDensityPerMetre;
    constants.fogInscatterR = configuration.fogInscatterR;
    constants.fogInscatterG = configuration.fogInscatterG;
    constants.fogInscatterB = configuration.fogInscatterB;
    constants.refractionMaximumOffsetUv = configuration.refractionMaximumOffsetUv;
    constants.reactiveAlphaWeight = configuration.reactiveAlphaWeight;
    constants.reactiveRefractionWeight = configuration.reactiveRefractionWeight;
    constants.reactiveStochasticWeight = configuration.reactiveStochasticWeight;
    constants.reactiveReferenceOffsetUv = configuration.reactiveReferenceOffsetUv;
    constants.reactiveMaximumMask = configuration.reactiveMaximumMask;
    constants.displayExposureScale = configuration.displayExposureScale;
    constants.clusterSliceDepthMetres = static_cast<float>(kClusterSliceDepthMetres);

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    ID3D12GraphicsCommandList7 &list = *frameContext.commandList;
    ID3D12DescriptorHeap *heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    list.SetDescriptorHeaps(1U, heaps);

    // Entry barriers. The three read-back buffers ended the previous frame as copy sources and the refraction source
    // ended it as a compute UAV; the four authored scene tables are sequence-owned, so their previous access is the
    // shader read of the previous frame rather than anything this slot did.
    BufferBarrierState const previousSlotState = slot.initialized ? CopySourceState() : NoAccessState();
    BufferBarrierState const previousSceneState = sceneInitialized_ ? ComputeSrvState() : NoAccessState();
    std::array<D3D12_BUFFER_BARRIER, 8U> const entryBarriers{
        MakeBufferBarrier(*slot.records.Get(), previousSlotState, ComputeUavState()),
        MakeBufferBarrier(*slot.fragments.Get(), previousSlotState, ComputeUavState()),
        MakeBufferBarrier(*slot.frame.Get(), previousSlotState, ComputeUavState()),
        MakeBufferBarrier(*slot.refractionSource.Get(), slot.initialized ? ComputeUavState() : NoAccessState(),
                          ComputeUavState()),
        MakeBufferBarrier(*panes_.Get(), previousSceneState, CopyDestinationState()),
        MakeBufferBarrier(*lights_.Get(), previousSceneState, CopyDestinationState()),
        MakeBufferBarrier(*lightIndices_.Get(), previousSceneState, CopyDestinationState()),
        MakeBufferBarrier(*clusters_.Get(), previousSceneState, CopyDestinationState()),
    };
    SubmitBufferBarriers(list, entryBarriers);

    // The scene tables are staged through the buffer this frame slot owns. BeginFrame has already waited on this
    // slot's fence, so the only frame that could have been reading this buffer has completed, and the frame still
    // in flight in the other slot is reading a different buffer entirely.
    std::byte *const staging = slot.sceneUpload.mapped_data();
    if (staging == nullptr)
    {
        return std::unexpected(lgp::framework::MakeError("Render", "Chapter 32 scene staging is not mapped."));
    }
    std::memset(staging, 0, static_cast<std::size_t>(kUploadTotalBytes));
    if (!activePanes.empty())
    {
        std::memcpy(staging + kUploadPanesOffset, activePanes.data(), activePanes.size() * sizeof(PaneRecord));
    }
    std::memcpy(staging + kUploadLightsOffset, lights.data(), lights.size() * sizeof(LightRecord));
    std::memcpy(staging + kUploadClustersOffset, clusterRanges.data(), clusterRanges.size() * sizeof(ClusterRange));
    if (!lightIndices.empty())
    {
        std::memcpy(staging + kUploadLightIndicesOffset, lightIndices.data(),
                    lightIndices.size() * sizeof(std::uint32_t));
    }
    list.CopyBufferRegion(panes_.Get(), 0U, slot.sceneUpload.Get(), kUploadPanesOffset, kPaneTableBytes);
    list.CopyBufferRegion(lights_.Get(), 0U, slot.sceneUpload.Get(), kUploadLightsOffset, kLightTableBytes);
    list.CopyBufferRegion(clusters_.Get(), 0U, slot.sceneUpload.Get(), kUploadClustersOffset, kClusterTableBytes);
    list.CopyBufferRegion(lightIndices_.Get(), 0U, slot.sceneUpload.Get(), kUploadLightIndicesOffset,
                          kLightIndexTableBytes);
    std::array<D3D12_BUFFER_BARRIER, 4U> const sceneBarriers{
        MakeBufferBarrier(*panes_.Get(), CopyDestinationState(), ComputeSrvState()),
        MakeBufferBarrier(*lights_.Get(), CopyDestinationState(), ComputeSrvState()),
        MakeBufferBarrier(*lightIndices_.Get(), CopyDestinationState(), ComputeSrvState()),
        MakeBufferBarrier(*clusters_.Get(), CopyDestinationState(), ComputeSrvState()),
    };
    SubmitBufferBarriers(list, sceneBarriers);

    list.SetComputeRootSignature(computeRootSignature_.Get());
    list.SetComputeRootDescriptorTable(RootResources, slot.descriptors.GpuHandle(RecordsUav));
    list.SetComputeRoot32BitConstants(RootConstants, sizeof(LabConstants) / sizeof(std::uint32_t), &constants, 0U);

    // Every stage writes buffers the next stage reads, so each transition names exactly the resources that carry the
    // dependency rather than draining the whole set.
    auto const orderStages = [&list](std::span<ID3D12Resource *const> resources)
    {
        std::array<D3D12_BUFFER_BARRIER, 4U> barriers{};
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

    std::uint32_t const pixelCount32 = size_.width * size_.height;
    std::uint32_t const groupsX = GroupCount(size_.width, kGroupWidth);
    std::uint32_t const groupsY = GroupCount(size_.height, kGroupHeight);

    list.SetPipelineState(clearPipeline_.Get());
    list.Dispatch(GroupCount(pixelCount32, 64U), 1U, 1U);
    {
        ID3D12Resource *const resources[]{slot.records.Get(), slot.fragments.Get(), slot.frame.Get(),
                                          slot.refractionSource.Get()};
        orderStages(resources);
    }

    list.SetPipelineState(opaquePipeline_.Get());
    list.Dispatch(groupsX, groupsY, 1U);
    {
        ID3D12Resource *const resources[]{slot.records.Get()};
        orderStages(resources);
    }

    if (variant_ == LabVariant::Solution)
    {
        list.SetPipelineState(refractionSourcePipeline_.Get());
        list.Dispatch(groupsX, groupsY, 1U);
        {
            ID3D12Resource *const resources[]{slot.refractionSource.Get(), slot.records.Get()};
            orderStages(resources);
        }
    }

    list.SetPipelineState(fragmentsPipeline_.Get());
    list.Dispatch(groupsX, groupsY, 1U);
    {
        ID3D12Resource *const resources[]{slot.fragments.Get(), slot.records.Get(), slot.frame.Get()};
        orderStages(resources);
    }

    list.SetPipelineState(forwardPipeline_.Get());
    list.Dispatch(groupsX, groupsY, 1U);
    {
        ID3D12Resource *const resources[]{slot.fragments.Get(), slot.records.Get(), slot.frame.Get()};
        orderStages(resources);
    }

    if (variant_ == LabVariant::Solution)
    {
        list.SetPipelineState(oitAccumulatePipeline_.Get());
        list.Dispatch(groupsX, groupsY, 1U);
        {
            ID3D12Resource *const resources[]{slot.records.Get(), slot.frame.Get()};
            orderStages(resources);
        }

        list.SetPipelineState(oitResolvePipeline_.Get());
        list.Dispatch(groupsX, groupsY, 1U);
        {
            ID3D12Resource *const resources[]{slot.records.Get()};
            orderStages(resources);
        }
    }

    list.SetPipelineState(composePipeline_.Get());
    list.Dispatch(groupsX, groupsY, 1U);

    std::array<D3D12_BUFFER_BARRIER, 2U> const displayBarriers{
        MakeBufferBarrier(*slot.records.Get(), ComputeUavState(), PixelSrvState()),
        MakeBufferBarrier(*slot.frame.Get(), ComputeUavState(), PixelSrvState()),
    };
    SubmitBufferBarriers(list, displayBarriers);

    SubmitTextureTransition(list, *frameContext.renderTarget, FrameStartState(frameContext), RenderTargetState());
    float const clear[]{0.0F, 0.0F, 0.0F, 1.0F};
    list.ClearRenderTargetView(frameContext.renderTargetView, clear, 0U, nullptr);
    list.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    list.RSSetViewports(1U, &frameContext.viewport);
    list.RSSetScissorRects(1U, &frameContext.scissorRect);
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    list.SetGraphicsRoot32BitConstants(RootConstants, sizeof(LabConstants) / sizeof(std::uint32_t), &constants, 0U);
    list.SetGraphicsRootDescriptorTable(RootResources, slot.descriptors.GpuHandle(DisplayRecordsSrv));
    list.SetPipelineState(graphicsPipeline_.Get());
    list.DrawInstanced(3U, 1U, 0U, 0U);

    std::array<D3D12_BUFFER_BARRIER, 3U> const copyBarriers{
        MakeBufferBarrier(*slot.records.Get(), PixelSrvState(), CopySourceState()),
        MakeBufferBarrier(*slot.frame.Get(), PixelSrvState(), CopySourceState()),
        MakeBufferBarrier(*slot.fragments.Get(), ComputeUavState(), CopySourceState()),
    };
    SubmitBufferBarriers(list, copyBarriers);
    list.CopyBufferRegion(slot.recordsReadback.Get(), 0U, slot.records.Get(), 0U, slot.records.size_in_bytes());
    list.CopyBufferRegion(slot.fragmentsReadback.Get(), 0U, slot.fragments.Get(), 0U, slot.fragments.size_in_bytes());
    list.CopyBufferRegion(slot.frameReadback.Get(), 0U, slot.frame.Get(), 0U, slot.frame.size_in_bytes());
    SubmitTextureTransition(list, *frameContext.renderTarget, RenderTargetState(), FrameEndState(frameContext));

    slot.initialized = true;
    sceneInitialized_ = true;
    lastRenderedConfiguration_ = configuration;
    lastPanes_ = activePanes;
    lastLights_ = lights;
    lastLightIndices_ = lightIndices;
    lastClusters_ = clusterRanges;
    lastClusterGrid_ = grid;
    lastOrderPlan_ = *plan;
    lastFrameStages_ = frameStages;
    lastFrameStageCount_ = frameStageCount;
    lastStages_ = stages;
    lastStageCount_ = stageCount;
    lastStageOrderWord_ = stageOrderWord;
    lastExpectedStatus_ = expectedStatus;
    lastActivePaneCount_ = static_cast<std::uint32_t>(activePanes.size());
    lastRenderedFrameSlot_ = frameContext.frameSlot;
    // Recorded from the very object the copies above named, so the published identity is the staging the frame
    // actually used rather than whatever the slot holds by the time the readback runs.
    lastSceneUploadGpuAddress_ = slot.sceneUpload.Get()->GetGPUVirtualAddress();
    lastSceneUploadCpuAddress_ = std::bit_cast<std::uint64_t>(staging);
    hasRendered_ = true;
    return {};
}

std::expected<FrameReadback, lgp::framework::Error> RendererCore::ReadBackOutputs()
{
    if (deviceResources_ == nullptr || !hasRendered_ || frameSlots_.empty())
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "Chapter 32 has no completed frame to read."));
    }
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }
    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    if (slot.recordsReadback.mapped_data() == nullptr || slot.fragmentsReadback.mapped_data() == nullptr ||
        slot.frameReadback.mapped_data() == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "Chapter 32 readback staging is not mapped."));
    }

    FrameReadback output{};
    output.configuration = lastRenderedConfiguration_;
    output.variant = variant_;
    output.displaySize = size_;
    output.panes = lastPanes_;
    output.lights = lastLights_;
    output.lightIndices = lastLightIndices_;
    output.clusters = lastClusters_;
    output.clusterGrid = lastClusterGrid_;
    output.weightSettings = MakeOitWeightSettings(lastRenderedConfiguration_);
    output.resolveSettings = MakeResolveSettings(lastRenderedConfiguration_);
    output.alphaTestSettings = MakeAlphaTestSettings(lastRenderedConfiguration_);
    output.stochasticSettings = MakeStochasticSettings(lastRenderedConfiguration_);
    output.fogSettings = MakeFogSettings(lastRenderedConfiguration_);
    output.refractionSettings = MakeRefractionSettings(lastRenderedConfiguration_);
    output.reactiveSettings = MakeReactiveSettings(lastRenderedConfiguration_);
    output.boundedSettings = MakeBoundedSettings(lastRenderedConfiguration_);
    output.sortedSettings = MakeSortedSettings(lastRenderedConfiguration_);
    output.pipelinePolicy = MakePipelinePolicy(lastRenderedConfiguration_, variant_);
    output.orderPlan = lastOrderPlan_;
    output.frameStages = lastFrameStages_;
    output.frameStageCount = lastFrameStageCount_;
    output.executedStages = lastStages_;
    output.executedStageCount = lastStageCount_;
    output.stageOrderWord = lastStageOrderWord_;
    output.expectedStatus = lastExpectedStatus_;
    output.activePaneCount = lastActivePaneCount_;
    output.frameSlot = lastRenderedFrameSlot_;
    output.sceneUploadGpuAddress = lastSceneUploadGpuAddress_;
    output.sceneUploadCpuAddress = lastSceneUploadCpuAddress_;

    std::size_t const pixelCount = static_cast<std::size_t>(size_.width) * size_.height;
    output.pixels.resize(pixelCount);
    std::memcpy(output.pixels.data(), slot.recordsReadback.mapped_data(), pixelCount * sizeof(PixelRecord));
    output.fragments.resize(pixelCount * kLabFragmentCapacity);
    std::memcpy(output.fragments.data(), slot.fragmentsReadback.mapped_data(),
                output.fragments.size() * sizeof(FragmentRecord));
    std::memcpy(&output.frame, slot.frameReadback.mapped_data(), sizeof(FrameRecord));
    return output;
}

} // namespace ch32::transparency::gpu
