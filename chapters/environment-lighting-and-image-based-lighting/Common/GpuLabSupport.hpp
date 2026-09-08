#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EnvironmentLightingContracts.hpp"

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

namespace ch27::environment_lighting::gpu
{

inline constexpr std::uint32_t kMaximumWidth = 640U;
inline constexpr std::uint32_t kMaximumHeight = 360U;
inline constexpr std::uint32_t kEnvironmentWidth = 64U;
inline constexpr std::uint32_t kEnvironmentHeight = 32U;
inline constexpr std::uint32_t kEnvironmentMipCount = 7U;
inline constexpr std::uint32_t kPrefilterSampleCount = 128U;
inline constexpr std::uint32_t kSplitSumSampleCount = 256U;

enum class DebugView : std::uint32_t
{
    Baseline = 0U,
    Environment = 1U,
    BaselineComponents = 2U,
    Solution = 3U,
    DiffuseIrradiance = 4U,
    SpecularPrefilter = 5U,
    MipSelection = 6U,
    BrdfLut = 7U,
    BaselineVsSolution = 8U,
};

enum class LabVariant : std::uint8_t
{
    Starter,
    Solution,
};

struct LabConfiguration final
{
    float roughness{0.55F};
    float nDotView{0.65F};
    DebugView debugView{DebugView::BaselineVsSolution};

    [[nodiscard]] bool operator==(LabConfiguration const &) const noexcept = default;
};

inline constexpr std::uint32_t kSourceValid = 1U << 0U;
inline constexpr std::uint32_t kBaselineValid = 1U << 1U;
inline constexpr std::uint32_t kIrradianceValid = 1U << 2U;
inline constexpr std::uint32_t kPrefilterValid = 1U << 3U;
inline constexpr std::uint32_t kSplitSumValid = 1U << 4U;
inline constexpr std::uint32_t kMipValid = 1U << 5U;
inline constexpr std::uint32_t kProbeValid = 1U << 6U;
inline constexpr std::uint32_t kStarterValidStatus = kSourceValid | kBaselineValid | kProbeValid;
inline constexpr std::uint32_t kSolutionValidStatus =
    kStarterValidStatus | kIrradianceValid | kPrefilterValid | kSplitSumValid | kMipValid;

struct PixelStatistics final
{
    float sourceR{};
    float sourceG{};
    float sourceB{};
    float baselineDiffuseR{};
    float baselineDiffuseG{};
    float baselineDiffuseB{};
    float baselineSpecularR{};
    float baselineSpecularG{};
    float baselineSpecularB{};
    float irradianceR{};
    float irradianceG{};
    float irradianceB{};
    float shadedDiffuseR{};
    float shadedDiffuseG{};
    float shadedDiffuseB{};
    float prefilterBaseR{};
    float prefilterBaseG{};
    float prefilterBaseB{};
    float prefilterMipR{};
    float prefilterMipG{};
    float prefilterMipB{};
    float splitA{};
    float splitB{};
    float lutA{};
    float lutB{};
    float lutRoughness{};
    float lutNDotView{};
    float shadedSpecularR{};
    float shadedSpecularG{};
    float shadedSpecularB{};
    float meanSelectedMip{};
    float maximumSelectedMip{};
    float equatorMip{};
    float poleMip{};
    float probeRadicalInverse{};
    float probeHalfX{};
    float probeHalfY{};
    float probeHalfZ{};
    float probeLightX{};
    float probeLightY{};
    float probeLightZ{};
    float probeNDotH{};
    float probeVDotH{};
    float probeNDotL{};
    float probeNdf{};
    float probeHalfPdf{};
    float probeLightPdf{};
    std::uint32_t acceptedPrefilterSamples{};
    std::uint32_t status{};

    [[nodiscard]] bool operator==(PixelStatistics const &) const noexcept = default;
};
static_assert(sizeof(PixelStatistics) == 196U);
static_assert(alignof(PixelStatistics) == 4U);

struct EnvironmentMip final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<float> rgba{};
};

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

[[nodiscard]] lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration,
                                                              LabVariant variant);
[[nodiscard]] Float3 PixelDirection(std::uint32_t x, std::uint32_t y, std::uint32_t width,
                                    std::uint32_t height) noexcept;
[[nodiscard]] Rgb EvaluateEnvironment(Float3 direction) noexcept;
[[nodiscard]] std::vector<EnvironmentMip> BuildEnvironmentMipChain();
[[nodiscard]] EnvironmentImageView BaseEnvironmentView(std::vector<EnvironmentMip> const &mips,
                                                       std::vector<Rgb> &storage);

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
    struct FrameSlotResources final
    {
        BufferResource statistics{};
        BufferResource statisticsReadback{};
        lgp::framework::DescriptorAllocation descriptors{};
        bool initialized{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateEnvironmentTexture();
    [[nodiscard]] lgp::framework::Status CreateFrameSlotResources(lgp::framework::Extent2D size);
    void DestroyFrameSlotResources(lgp::framework::DeviceResources &deviceResources) noexcept;
    [[nodiscard]] LabConfiguration ActiveConfiguration() const noexcept;

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
    Microsoft::WRL::ComPtr<ID3D12Resource> environmentTexture_{};
    std::vector<FrameSlotResources> frameSlots_{};
    lgp::framework::Extent2D size_{};
    std::optional<LabConfiguration> headlessConfiguration_{};
    LabConfiguration interactiveConfiguration_{};
    LabConfiguration lastRenderedConfiguration_{};
    std::uint32_t lastRenderedFrameSlot_{};
};

} // namespace ch27::environment_lighting::gpu
