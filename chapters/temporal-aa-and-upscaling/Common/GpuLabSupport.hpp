#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TemporalAaContracts.hpp"

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

namespace ch28::temporal_aa::gpu
{

inline constexpr std::uint32_t kMaximumWidth = 640U;
inline constexpr std::uint32_t kMaximumHeight = 360U;

enum class LabVariant : std::uint8_t
{
    Starter,
    Solution,
};

enum class ReconstructionMode : std::uint32_t
{
    NativeTaa = 0U,
    TemporalUpscale = 1U,
};

enum class DebugView : std::uint32_t
{
    CurrentSpatial = 0U,
    Motion = 1U,
    HistoryValidity = 2U,
    HistoryConstraint = 3U,
    FeedbackAndCount = 4U,
    Resolved = 5U,
    NativeVsUpscale = 6U,
    Sharpening = 7U,
};

struct LabConfiguration final
{
    ReconstructionMode mode{ReconstructionMode::TemporalUpscale};
    DebugView debugView{DebugView::NativeVsUpscale};
    std::uint32_t animationFrame{};
    std::uint32_t jitterPhasePeriod{8U};
    float upscaleRenderScale{0.63F};
    float preExposure{1.0F};
    float sharpeningStrength{0.2F};
    bool jitterEnabled{true};
    bool resetHistory{};

    [[nodiscard]] bool operator==(LabConfiguration const &) const noexcept = default;
};

inline constexpr std::uint32_t kSpatialValid = 1U << 0U;
inline constexpr std::uint32_t kJitterValid = 1U << 1U;
inline constexpr std::uint32_t kReprojectionValid = 1U << 2U;
inline constexpr std::uint32_t kValidationValid = 1U << 3U;
inline constexpr std::uint32_t kConstraintValid = 1U << 4U;
inline constexpr std::uint32_t kFeedbackValid = 1U << 5U;
inline constexpr std::uint32_t kResolveValid = 1U << 6U;
inline constexpr std::uint32_t kSharpenValid = 1U << 7U;
inline constexpr std::uint32_t kHistoryWritten = 1U << 8U;
inline constexpr std::uint32_t kStarterValidStatus = kSpatialValid | kJitterValid;
inline constexpr std::uint32_t kSolutionValidStatus = (1U << 9U) - 1U;

struct PixelStatistics final
{
    float currentR{};
    float currentG{};
    float currentB{};
    float currentLuminance{};
    float spatialR{};
    float spatialG{};
    float spatialB{};
    float currentDepth{};
    float motionX{};
    float motionY{};
    float currentJitterX{};
    float currentJitterY{};
    float previousJitterX{};
    float previousJitterY{};
    float previousHistoryX{};
    float previousHistoryY{};
    float historyStorageX{};
    float historyStorageY{};
    float historyR{};
    float historyG{};
    float historyB{};
    float historyLuminance{};
    float constrainedR{};
    float constrainedG{};
    float constrainedB{};
    float varianceClipScale{};
    float validityFactor{};
    float sampleCountFactor{};
    float lockStatus{};
    float motionFactor{};
    float reactiveFactor{};
    float disocclusionFactor{};
    float historyFeedback{};
    float resolvedR{};
    float resolvedG{};
    float resolvedB{};
    float sharpenedR{};
    float sharpenedG{};
    float sharpenedB{};
    float sharpeningDeltaR{};
    float sharpeningDeltaG{};
    float sharpeningDeltaB{};
    float overshootObserved{};
    std::uint32_t rejectionReasons{};
    std::uint32_t previousSampleCount{};
    std::uint32_t nextSampleCount{};
    std::uint32_t currentObjectId{};
    std::uint32_t sampledObjectId{};
    std::uint32_t status{};
    std::uint32_t renderWidth{};
    std::uint32_t renderHeight{};

    [[nodiscard]] bool operator==(PixelStatistics const &) const noexcept = default;
};
static_assert(sizeof(PixelStatistics) == 204U);
static_assert(alignof(PixelStatistics) == 4U);

struct HistoryPixel final
{
    float colorR{};
    float colorG{};
    float colorB{};
    float luminance{};
    float depth{};
    float normalX{};
    float normalY{};
    float normalZ{};
    std::uint32_t objectId{};
    std::uint32_t sampleCount{};
};
static_assert(sizeof(HistoryPixel) == 40U);

struct FrameReadback final
{
    LabConfiguration configuration{};
    std::vector<PixelStatistics> pixels{};
    lgp::framework::Extent2D displaySize{};
    lgp::framework::Extent2D renderSize{};
    Float2 currentJitterPixels{};
    Float2 previousJitterPixels{};
    std::uint32_t frameSlot{};
    bool historyWasValid{};
};

struct BufferBarrierState final
{
    D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};
    D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};
};

[[nodiscard]] lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration,
                                                              LabVariant variant);
[[nodiscard]] lgp::framework::Extent2D RenderExtent(LabConfiguration const &configuration,
                                                    lgp::framework::Extent2D displayExtent) noexcept;

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
    // Readback and diagnostics are frame-slot-owned; the two display-sized
    // history buffers are sequence-owned and alternate read/write roles.
    struct FrameSlotResources final
    {
        BufferResource statistics{};
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
    lgp::framework::CompiledShader computeShader_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> computePipeline_{};
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
    Float2 lastCurrentJitter_{};
    Float2 lastPreviousJitter_{};
    bool lastHistoryWasValid_{};
    std::uint32_t lastRenderedFrameSlot_{};
};

} // namespace ch28::temporal_aa::gpu
