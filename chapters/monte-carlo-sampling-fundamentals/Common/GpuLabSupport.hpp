#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

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

namespace ch16::monte_carlo::gpu
{

inline constexpr std::uint32_t kMaximumWidth = 640U;
inline constexpr std::uint32_t kMaximumHeight = 360U;
inline constexpr std::uint32_t kMaximumSamplesPerDispatch = 64U;

enum class DebugView : std::uint32_t
{
    Estimate = 0U,
    AbsoluteError,
    StandardError,
    SampleCount,
};

struct LabConfiguration final
{
    float exponent{1.0F};
    std::uint32_t samplesPerDispatch{8U};
    std::uint32_t seed{0xC001C0DEU};
    DebugView debugView{DebugView::Estimate};
    bool resetAccumulation{};
};

struct PixelStatistics final
{
    float estimate{};
    float exact{};
    float sampleVariance{};
    float standardError{};
    std::uint32_t sampleCount{};
    std::uint32_t status{};
    float lastCosine{};
    float lastAzimuth{};

    [[nodiscard]] bool operator==(PixelStatistics const &) const noexcept = default;
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
    friend std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
        ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
        std::wstring_view name, bool mapPersistently);

    void Reset() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::uint64_t sizeInBytes_{};
    std::byte *mappedData_{};
};

static_assert(sizeof(PixelStatistics) == 32U);

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
[[nodiscard]] BufferBarrierState PixelShaderResourceState() noexcept;
[[nodiscard]] BufferBarrierState CopySourceState() noexcept;
[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(
    ID3D12Resource &resource, lgp::framework::TextureBarrierState before, lgp::framework::TextureBarrierState after,
    D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) noexcept;
void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers);
[[nodiscard]] D3D12_BUFFER_BARRIER MakeBufferBarrier(ID3D12Resource &resource, BufferBarrierState before,
                                                     BufferBarrierState after) noexcept;
void SubmitBufferBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_BUFFER_BARRIER> &barriers);

class RendererCore : public lgp::framework::IChapterRenderer
{
  public:
    RendererCore(std::filesystem::path shaderPath, bool progressive);
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
        BufferResource moments{};
        BufferResource statistics{};
        BufferResource statisticsReadback{};
        lgp::framework::DescriptorAllocation descriptors{};
        std::uint32_t sampleCount{};
        float exponent{};
        std::uint64_t resetGeneration{};
        bool initialized{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateFrameSlotResources(lgp::framework::Extent2D size);
    void DestroyFrameSlotResources(lgp::framework::DeviceResources &deviceResources) noexcept;
    [[nodiscard]] LabConfiguration ActiveConfiguration() const noexcept;

    std::filesystem::path shaderPath_{};
    bool progressive_{};
    bool headless_{};
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
    std::uint64_t resetGeneration_{1U};
    std::uint32_t lastRenderedFrameSlot_{};
};

} // namespace ch16::monte_carlo::gpu
