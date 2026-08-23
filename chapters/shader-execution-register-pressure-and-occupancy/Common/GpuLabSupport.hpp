#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ShaderOccupancyContracts.hpp"

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
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ch18::shader_occupancy::gpu
{

inline constexpr std::uint32_t kThreadGroupSize = 64U;
inline constexpr std::uint32_t kDefaultElementCount = 1'024U;
inline constexpr std::uint32_t kMaximumElementCount = 4'096U;
inline constexpr std::uint32_t kMaximumWidth = 640U;
inline constexpr std::uint32_t kMaximumHeight = 360U;
inline constexpr std::uint32_t kOutputValidStatus = 0x7U;
inline constexpr float kReferenceTolerance = 1.0e-6F;

enum class ShaderVariant : std::uint32_t
{
    CoherentLowPressure = 0U,
    DivergentLowPressure,
    CoherentHighLiveRange,
    CoherentShortLiveRange,
};

enum class BranchPattern : std::uint32_t
{
    CoherentAlternatingGroups = 0U,
    DivergentAlternatingLanes,
};

enum class DiagnosticView : std::uint32_t
{
    OutputValue = 0U,
    BranchClassification,
    AbstractOccupancyModel,
    EvidenceBoundaries,
};

enum class LabEdition : std::uint8_t
{
    Starter = 0U,
    Solution,
};

enum class FunctionalStatus : std::uint32_t
{
    Valid = 0U,
    OutputMismatch,
    BranchClassificationMismatch,
    InvalidGpuStatus,
};

enum class PhysicalProfilingStatus : std::uint32_t
{
    NotCollected = 0U,
};

struct BranchInstructionModel final
{
    std::uint64_t thenPathInstructionCount{};
    std::uint64_t elsePathInstructionCount{};
    std::uint64_t convergedInstructionCount{};

    [[nodiscard]] bool operator==(BranchInstructionModel const &) const noexcept = default;
};

struct VariantMetadata final
{
    ShaderVariant variant{ShaderVariant::CoherentLowPressure};
    std::wstring_view entryPoint{};
    std::string_view displayName{};
    std::array<std::uint32_t, 3U> threadGroupSize{};
    BranchPattern expectedBranchPattern{BranchPattern::CoherentAlternatingGroups};
    BranchInstructionModel branchInstructionModel{};
    ShaderResourceUsage abstractResourceUsage{};
    TeachingLivenessBounds livenessBounds{};
    std::span<LiveValueInterval const> livenessIntervals{};
};

struct LabConfiguration final
{
    ShaderVariant variant{ShaderVariant::CoherentLowPressure};
    DiagnosticView diagnosticView{DiagnosticView::OutputValue};
    std::uint32_t elementCount{kDefaultElementCount};
};

struct ThreadOutput final
{
    std::array<float, 4U> value{};
    std::uint32_t branchClass{};
    std::uint32_t status{};
    std::uint32_t threadIndex{};
    std::uint32_t valueChecksum{};

    [[nodiscard]] bool operator==(ThreadOutput const &) const noexcept = default;
};

struct WaveLaneCapability final
{
    bool waveOpsSupported{};
    std::uint32_t minimumLaneCount{};
    std::uint32_t maximumLaneCount{};
    bool isWarp{};

    [[nodiscard]] bool operator==(WaveLaneCapability const &) const noexcept = default;
};

struct AbstractModelEvidence final
{
    HardwareModel teachingArchitecture{};
    ShaderResourceUsage teachingUsage{};
    OccupancyResult occupancy{};
    TeachingLivenessResult liveness{};
    std::uint64_t usefulLaneInstructions{};
    std::uint64_t issuedLaneSlots{};
    std::uint32_t modeledThenLaneCount{};
    std::uint32_t modeledElseLaneCount{};

    [[nodiscard]] bool operator==(AbstractModelEvidence const &) const noexcept = default;
};

struct RuntimeFunctionalEvidence final
{
    std::uint32_t dispatchedThreadCount{};
    std::uint32_t activeThenLaneCount{};
    std::uint32_t activeElseLaneCount{};
    std::uint64_t outputChecksum{};
    std::uint64_t referenceChecksum{};
    float maximumAbsoluteError{};
    bool referenceAgreement{};
    bool branchClassificationAgreement{};
    FunctionalStatus status{FunctionalStatus::InvalidGpuStatus};

    [[nodiscard]] bool operator==(RuntimeFunctionalEvidence const &) const noexcept = default;
};

struct FrameReadback final
{
    LabConfiguration configuration{};
    VariantMetadata metadata{};
    std::vector<ThreadOutput> outputs{};
    AbstractModelEvidence abstractModel{};
    RuntimeFunctionalEvidence runtimeFunctional{};
    WaveLaneCapability runtimeWaveCapability{};
    PhysicalProfilingStatus physicalProfiling{PhysicalProfilingStatus::NotCollected};
    lgp::framework::Extent2D size{};
    std::uint32_t frameSlot{};
};

struct ShaderArtifact final
{
    ShaderVariant variant{ShaderVariant::CoherentLowPressure};
    std::wstring entryPoint{};
    std::wstring targetProfile{};
    std::string dxilDisassembly{};
    std::size_t bytecodeSize{};
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

static_assert(sizeof(ThreadOutput) == 32U);

[[nodiscard]] std::expected<VariantMetadata, lgp::framework::Error> GetVariantMetadata(ShaderVariant variant);
[[nodiscard]] lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration,
                                                              LabEdition edition);
[[nodiscard]] std::expected<std::uint64_t, lgp::framework::Error> ComputeOutputBufferSize(std::uint64_t elementCount);
[[nodiscard]] std::array<float, 4U> ReferenceValue(std::uint32_t threadIndex) noexcept;
[[nodiscard]] std::uint32_t ReferenceValueChecksum(std::uint32_t threadIndex) noexcept;
[[nodiscard]] std::uint32_t ExpectedBranchClass(ShaderVariant variant, std::uint32_t threadIndex);
[[nodiscard]] std::expected<AbstractModelEvidence, lgp::framework::Error> BuildAbstractModelEvidence(
    ShaderVariant variant, std::uint32_t elementCount);

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
    RendererCore(std::filesystem::path shaderPath, LabEdition edition);
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
    [[nodiscard]] std::span<ShaderArtifact const> shader_artifacts() const noexcept;
    [[nodiscard]] lgp::framework::Status WriteShaderListings(std::filesystem::path const &directory) const;

  private:
    struct VariantPipeline final
    {
        ShaderVariant variant{ShaderVariant::CoherentLowPressure};
        lgp::framework::CompiledShader shader{};
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline{};
    };

    struct FrameSlotResources final
    {
        BufferResource output{};
        BufferResource outputReadback{};
        lgp::framework::DescriptorAllocation descriptors{};
        bool initialized{};
    };

    [[nodiscard]] lgp::framework::Status QueryWaveLaneCapability();
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateFrameSlotResources(lgp::framework::Extent2D size);
    void DestroyFrameSlotResources(lgp::framework::DeviceResources &deviceResources) noexcept;
    [[nodiscard]] LabConfiguration ActiveConfiguration() const noexcept;
    [[nodiscard]] VariantPipeline const *FindPipeline(ShaderVariant variant) const noexcept;

    std::filesystem::path shaderPath_{};
    LabEdition edition_{LabEdition::Starter};
    bool headless_{};
    bool hasRendered_{};
    lgp::framework::DeviceResources *deviceResources_{};
    std::vector<VariantPipeline> variantPipelines_{};
    std::vector<ShaderArtifact> shaderArtifacts_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
    std::vector<FrameSlotResources> frameSlots_{};
    lgp::framework::Extent2D size_{};
    LabConfiguration headlessConfiguration_{};
    LabConfiguration interactiveConfiguration_{};
    LabConfiguration lastRenderedConfiguration_{};
    WaveLaneCapability waveLaneCapability_{};
    std::uint32_t lastRenderedFrameSlot_{};
};

} // namespace ch18::shader_occupancy::gpu
