#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SphericalHarmonicsContracts.hpp"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/application.hpp>
#include <lgp/framework/barriers.hpp>
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

namespace ch26::spherical_harmonics::gpu
{

inline constexpr std::uint32_t kMaximumWidth = 640U;
inline constexpr std::uint32_t kMaximumHeight = 360U;

// The CPU-side projection grid used to feed ch26::spherical_harmonics::Project.
// This is independent of the render resolution: it only has to be dense
// enough for an accurate order-3 projection of the analytic environment, not
// match how many screen pixels are eventually shaded.
inline constexpr std::uint32_t kProjectionSampleWidth = 128U;
inline constexpr std::uint32_t kProjectionSampleHeight = 64U;

// Diagnostic views. Numeric values are shared verbatim between the C++ side
// and both HLSL files, so keep the two in lockstep if this ever changes.
//   0 Source                - the analytic RGB environment radiance, evaluated
//                              directly (not through any SH coefficients) at
//                              this pixel's direction.
//   1 Reconstruction         - the order-3 real-SH reconstruction of the base
//                              (unrotated) environment, from CPU-projected
//                              coefficients.
//   2 AbsoluteError           - |Reconstruction - Source|, exposing the
//                              truncation/ringing error a low band-limit
//                              introduces around the sharp lobe.
//   3 RotatedReconstruction  - reconstruction from coefficients produced by
//                              ch26::spherical_harmonics::Rotate on the CPU
//                              (Solution only).
//   4 Irradiance              - clamped-cosine diffuse irradiance, reconstructed
//                              from ch26::spherical_harmonics::ConvolveClampedCosine
//                              coefficients, treating this pixel's direction as
//                              a surface normal (Solution only).
//   5 Interpolated             - reconstruction from a per-pixel linear blend of
//                              the base and rotated coefficient sets (blended
//                              before HLSL evaluation), with the blend factor
//                              swept left-to-right across the screen (Solution only).
//   6 BandContribution        - reconstruction using only the currently selected
//                              band's coefficients, isolating that band's
//                              contribution/energy (Solution only).
enum class DebugView : std::uint32_t
{
    Source = 0U,
    Reconstruction = 1U,
    AbsoluteError = 2U,
    RotatedReconstruction = 3U,
    Irradiance = 4U,
    Interpolated = 5U,
    BandContribution = 6U,
};

inline constexpr std::uint32_t kSourceValid = 1U << 0U;
inline constexpr std::uint32_t kReconstructionValid = 1U << 1U;
inline constexpr std::uint32_t kErrorValid = 1U << 2U;
inline constexpr std::uint32_t kRotatedValid = 1U << 3U;
inline constexpr std::uint32_t kIrradianceValid = 1U << 4U;
inline constexpr std::uint32_t kInterpolatedValid = 1U << 5U;
inline constexpr std::uint32_t kBandContributionValid = 1U << 6U;
inline constexpr std::uint32_t kStarterValidStatus = kSourceValid | kReconstructionValid | kErrorValid;
inline constexpr std::uint32_t kSolutionValidStatus =
    kStarterValidStatus | kRotatedValid | kIrradianceValid | kInterpolatedValid | kBandContributionValid;

struct LabConfiguration final
{
    // Selects which single band (0..kMaximumBand) DebugView::BandContribution
    // isolates. Unused by every other view.
    std::uint32_t activeBand{0U};
    // Rotation (about the Y/up axis) applied, in coefficient space via
    // ch26::spherical_harmonics::Rotate, to build the "rotated" probe.
    double rotationDegrees{60.0};
    DebugView debugView{DebugView::Source};

    [[nodiscard]] bool operator==(LabConfiguration const &) const noexcept = default;
};

// 16 real-SH coefficients for one color channel, packed 4-per-lane in
// ascending coefficient-index order: lane[0] holds indices [0..3], lane[1]
// holds [4..7], lane[2] holds [8..11], lane[3] holds [12..15]. HLSL's
// SHCoefficientSetRGB (float4 x4 per channel) mirrors this exactly.
struct SHCoefficientChannelLanes final
{
    std::array<DirectX::XMFLOAT4, 4U> lanes{};
};
static_assert(sizeof(SHCoefficientChannelLanes) == 64U);

struct SHCoefficientSetRGB final
{
    SHCoefficientChannelLanes r{};
    SHCoefficientChannelLanes g{};
    SHCoefficientChannelLanes b{};
};
static_assert(sizeof(SHCoefficientSetRGB) == 192U);
static_assert(offsetof(SHCoefficientSetRGB, r) == 0U);
static_assert(offsetof(SHCoefficientSetRGB, g) == 64U);
static_assert(offsetof(SHCoefficientSetRGB, b) == 128U);

// Uploaded once per frame slot (see FrameSlotResources below) and bound as a
// root CBV at register(b1). Matches the Coefficients cbuffer declared in both
// Starter/SphericalHarmonicsLab.hlsl and Solution/SphericalHarmonicsLab.hlsl.
struct SHCoefficientBundle final
{
    SHCoefficientSetRGB base{};       // The unrotated environment ("probe A").
    SHCoefficientSetRGB rotated{};    // CPU-rotated environment ("probe B").
    SHCoefficientSetRGB irradiance{}; // ConvolveClampedCosine(base).
    // Squared L2 energy of the luminance coefficients in bands 0..3.
    DirectX::XMFLOAT4 bandEnergy{};
};
static_assert(sizeof(SHCoefficientBundle) == 592U);
static_assert(offsetof(SHCoefficientBundle, base) == 0U);
static_assert(offsetof(SHCoefficientBundle, rotated) == 192U);
static_assert(offsetof(SHCoefficientBundle, irradiance) == 384U);
static_assert(offsetof(SHCoefficientBundle, bandEnergy) == 576U);

// Per-pixel readback. Every field is a plain 4-byte scalar so the layout has
// no implicit padding and matches the HLSL PixelStatistics struct exactly
// (see the static_assert below and the HLSL definitions in Starter/Solution).
struct PixelStatistics final
{
    float sourceR{};
    float sourceG{};
    float sourceB{};
    float reconstructionR{};
    float reconstructionG{};
    float reconstructionB{};
    float absoluteErrorR{};
    float absoluteErrorG{};
    float absoluteErrorB{};
    float rotatedReconstructionR{};
    float rotatedReconstructionG{};
    float rotatedReconstructionB{};
    float irradianceR{};
    float irradianceG{};
    float irradianceB{};
    float interpolatedR{};
    float interpolatedG{};
    float interpolatedB{};
    float probeBlendFactor{};
    float bandContribution{};
    float bandEnergy{};
    std::uint32_t activeBand{};
    std::uint32_t status{};

    [[nodiscard]] bool operator==(PixelStatistics const &) const noexcept = default;
};
static_assert(sizeof(PixelStatistics) == 92U);
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

// Maps a screen pixel to a Y-up, right-handed unit direction using the same
// equirectangular convention as ch26::spherical_harmonics::BuildLatitudeLongitudeSamples:
// pixel centers are sampled at half-texel offsets, so row 0 lies near (but not
// on) the north pole and no row samples either pole. Increasing azimuth sweeps
// from +X toward +Z. Both
// Starter/SphericalHarmonicsLab.hlsl and Solution/SphericalHarmonicsLab.hlsl
// implement this identical mapping in float precision; this double-precision
// copy exists so CPU-side tests can build a golden reference at the exact
// direction a given screen pixel maps to on the GPU.
[[nodiscard]] ch26::spherical_harmonics::Float3 PixelDirection(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                                                               std::uint32_t height) noexcept;

struct RadianceRgb final
{
    double r{};
    double g{};
    double b{};

    [[nodiscard]] bool operator==(RadianceRgb const &) const noexcept = default;
};

// The deterministic analytic environment: a smooth low-frequency sky/ground
// gradient plus a bounded, angularly narrow "sun" lobe. Order-3 SH cannot
// represent the sun lobe faithfully, so its reconstruction shows visible
// truncation/ringing/negative lobes -- that is the point of this lab. Both
// HLSL files implement an identical float-precision Environment() function;
// keep the two in lockstep if this ever changes.
[[nodiscard]] RadianceRgb EvaluateEnvironment(ch26::spherical_harmonics::Float3 direction) noexcept;

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] BufferBarrierState NoAccessState() noexcept;
[[nodiscard]] BufferBarrierState ComputeUnorderedAccessState() noexcept;
[[nodiscard]] BufferBarrierState PixelShaderResourceState() noexcept;
[[nodiscard]] BufferBarrierState CopySourceState() noexcept;
[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(
    ID3D12Resource &resource, lgp::framework::TextureBarrierState before, lgp::framework::TextureBarrierState after,
    D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) noexcept;
void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers);
[[nodiscard]] D3D12_BUFFER_BARRIER MakeBufferBarrier(ID3D12Resource &resource, BufferBarrierState before,
                                                     BufferBarrierState after) noexcept;
void SubmitBufferBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_BUFFER_BARRIER> &barriers);

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

// Shared renderer for both the Starter and Solution executables (and their
// WARP GPU tests). Both variants build the identical CPU-projected
// coefficient bundle every frame (base/rotated/irradiance); they only differ
// in which HLSL file is compiled and which debug views/validation rules are
// accepted -- every D3D12 resource, root signature, and barrier sequence
// below is identical.
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
    // its own copy of the coefficient bundle (recomputed every frame from the
    // active configuration into an upload-heap buffer), so in-flight frames
    // never alias GPU writes or CPU uploads across slots.
    struct FrameSlotResources final
    {
        BufferResource statistics{};
        BufferResource statisticsReadback{};
        BufferResource coefficients{};
        lgp::framework::DescriptorAllocation descriptors{};
        bool initialized{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateFrameSlotResources(lgp::framework::Extent2D size);
    void DestroyFrameSlotResources(lgp::framework::DeviceResources &deviceResources) noexcept;
    [[nodiscard]] LabConfiguration ActiveConfiguration() const noexcept;
    [[nodiscard]] std::expected<SHCoefficientBundle, lgp::framework::Error> BuildCoefficientBundle(
        LabConfiguration const &configuration) const;

    std::filesystem::path shaderPath_{};
    LabVariant variant_{};
    bool headless_{};
    bool hasRendered_{};
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

} // namespace ch26::spherical_harmonics::gpu
