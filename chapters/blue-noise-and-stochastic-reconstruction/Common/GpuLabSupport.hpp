#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "BlueNoiseContracts.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/application.hpp>
#include <lgp/framework/barriers.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace ch25::blue_noise::gpu
{

inline constexpr std::uint32_t kMaximumWidth = 640U;
inline constexpr std::uint32_t kMaximumHeight = 360U;

// Status bits: each records whether the corresponding PixelStatistics field
// was actually computed and is finite this frame. The Starter only ever sets
// the exact/white/filtered-white bits; it never claims to have low-discrepancy
// or blue-noise evidence it did not compute.
inline constexpr std::uint32_t kExactValid = 1U << 0U;
inline constexpr std::uint32_t kWhiteValid = 1U << 1U;
inline constexpr std::uint32_t kLowDiscrepancyValid = 1U << 2U;
inline constexpr std::uint32_t kBlueValid = 1U << 3U;
inline constexpr std::uint32_t kFilteredWhiteValid = 1U << 4U;
inline constexpr std::uint32_t kFilteredBlueValid = 1U << 5U;
inline constexpr std::uint32_t kStarterValidStatus = kExactValid | kWhiteValid | kFilteredWhiteValid;
inline constexpr std::uint32_t kSolutionValidStatus =
    kExactValid | kWhiteValid | kLowDiscrepancyValid | kBlueValid | kFilteredWhiteValid | kFilteredBlueValid;

// Diagnostic views. Numeric values are shared verbatim between the C++ side
// and both HLSL files, so keep the two in lockstep if this ever changes.
//   0 Exact                    - the analytic p(x, y) reference field.
//   1 RawWhite                 - the one-sample white-noise Bernoulli hit.
//   2 RawLowDiscrepancy        - the one-sample low-discrepancy Bernoulli hit (Solution only).
//   3 RawBlue                  - the one-sample blue-noise-tile Bernoulli hit (Solution only).
//   4 FilteredWhite            - the normalized tent filter of the white-noise hit field.
//   5 FilteredBlue             - the normalized tent filter of the blue-noise hit field (Solution only).
//   6 RawComparison            - raw white | low-discrepancy | blue side by side (Solution only).
//   7 FilteredComparison       - filtered white | blue side by side (Solution only).
//   8 AbsoluteErrorComparison  - |filtered - exact| heat map, white | blue side by side (Solution only).
//   9 PatternDiagnostic        - blue-tile threshold membership | equal-density white threshold membership,
//                                independent of p(x, y) (Solution only).
enum class DebugView : std::uint32_t
{
    Exact = 0U,
    RawWhite = 1U,
    RawLowDiscrepancy = 2U,
    RawBlue = 3U,
    FilteredWhite = 4U,
    FilteredBlue = 5U,
    RawComparison = 6U,
    FilteredComparison = 7U,
    AbsoluteErrorComparison = 8U,
    PatternDiagnostic = 9U,
};

struct LabConfiguration final
{
    std::uint32_t seed{0xB1075001U};
    // Drives the temporal low-discrepancy index (Van der Corput of this
    // value) and the blue-noise tile's per-frame toroidal offset. It is not a
    // progressive sample count: every frame is one observation with the same
    // one-evaluation-per-pixel cost.
    std::uint32_t animationFrame{0U};
    // Rank threshold used only by DebugView::PatternDiagnostic to visualize a
    // fixed-size subset of the tile/white field, independent of p(x, y). Must
    // be in [1, kBlueNoiseTileCellCount - 1].
    std::uint32_t blueNoiseThresholdRank{512U};
    DebugView debugView{DebugView::Exact};

    [[nodiscard]] bool operator==(LabConfiguration const &) const noexcept = default;
};

// Per-pixel readback. Every field is a plain 4-byte scalar so the layout has
// no implicit padding and matches the HLSL PixelStatistics struct exactly
// (see the static_assert below and the HLSL definitions in Starter/Solution).
struct PixelStatistics final
{
    float exact{};
    float rawWhiteUnit{};
    float rawLowDiscrepancyUnit{};
    float rawBlueUnit{};
    float rawWhiteHit{};
    float rawLowDiscrepancyHit{};
    float rawBlueHit{};
    float filteredWhite{};
    float filteredBlue{};
    std::uint32_t blueTileRank{};
    std::uint32_t animationFrame{};
    std::uint32_t status{};
    std::uint32_t reserved{};

    [[nodiscard]] bool operator==(PixelStatistics const &) const noexcept = default;
};

static_assert(sizeof(PixelStatistics) == 52U);
static_assert(alignof(PixelStatistics) == 4U);

struct FrameReadback final
{
    LabConfiguration configuration{};
    std::vector<PixelStatistics> pixels{};
    lgp::framework::Extent2D size{};
    std::uint32_t frameSlot{};
};

struct BufferBarrierState final
{
    D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};
    D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};
};

enum class LabVariant : std::uint8_t
{
    Starter,
    Solution,
};

[[nodiscard]] lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration,
                                                              LabVariant variant);

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

    [[nodiscard]] std::byte *mutable_mapped_data() noexcept
    {
        return mappedData_;
    }

  private:
    friend std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
        ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
        std::wstring_view name, bool mapPersistently);

    void Reset() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::uint64_t sizeInBytes_{};
    std::byte *mappedData_{};
};

[[nodiscard]] std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
    ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
    std::wstring_view name, bool mapPersistently = false);

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] BufferBarrierState NoAccessState() noexcept;
[[nodiscard]] BufferBarrierState ComputeUnorderedAccessState() noexcept;
[[nodiscard]] BufferBarrierState ComputeShaderResourceState() noexcept;
[[nodiscard]] BufferBarrierState PixelShaderResourceState() noexcept;
[[nodiscard]] BufferBarrierState CopySourceState() noexcept;
[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(
    ID3D12Resource &resource, lgp::framework::TextureBarrierState before, lgp::framework::TextureBarrierState after,
    D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) noexcept;
void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers);
[[nodiscard]] D3D12_BUFFER_BARRIER MakeBufferBarrier(ID3D12Resource &resource, BufferBarrierState before,
                                                     BufferBarrierState after) noexcept;
void SubmitBufferBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_BUFFER_BARRIER> &barriers);

// Shared renderer for both the Starter and Solution executables (and their
// WARP GPU tests). The two variants only differ in which HLSL file is
// compiled and which debug views/validation rules are accepted; every D3D12
// resource, root signature, and barrier sequence below is identical.
class RendererCore : public lgp::framework::IChapterRenderer
{
  public:
    RendererCore(std::filesystem::path shaderPath, LabVariant variant);
    RendererCore(RendererCore &&) noexcept = default;
    RendererCore &operator=(RendererCore &&) noexcept = default;
    RendererCore(RendererCore const &) = delete;
    RendererCore &operator=(RendererCore const &) = delete;
    ~RendererCore() override = default;

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void ConfigureHeadlessTest(LabConfiguration const &configuration) noexcept;
    [[nodiscard]] std::expected<FrameReadback, lgp::framework::Error> ReadBackOutputs();

  private:
    // Each swap-chain frame slot owns its own statistics buffer/readback and
    // its own copy of the (immutable) blue-noise tile buffer, so in-flight
    // frames never alias GPU writes across slots.
    struct FrameSlotResources final
    {
        BufferResource statistics{};
        BufferResource statisticsReadback{};
        BufferResource blueNoiseTile{};
        lgp::framework::DescriptorAllocation descriptors{};
        bool initialized{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateFrameSlotResources(lgp::framework::Extent2D size);
    void DestroyFrameSlotResources(lgp::framework::DeviceResources &deviceResources) noexcept;
    [[nodiscard]] LabConfiguration ActiveConfiguration() const noexcept;

    std::filesystem::path shaderPath_{};
    LabVariant variant_{};
    bool headless_{};
    bool hasRendered_{};
    bool animationPaused_{};
    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader sampleShader_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> computePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
    std::vector<FrameSlotResources> frameSlots_{};
    lgp::framework::Extent2D size_{};
    std::optional<LabConfiguration> headlessConfiguration_{};
    LabConfiguration interactiveConfiguration_{};
    LabConfiguration lastRenderedConfiguration_{};
    std::uint32_t lastRenderedFrameSlot_{};
};

} // namespace ch25::blue_noise::gpu
