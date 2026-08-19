#pragma once

#include "PassSchedule.hpp"

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/device_resources.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <vector>

namespace ch10::pass_scheduling::gpu
{

inline constexpr DXGI_FORMAT kTransientFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

enum class LabTextureIndex : std::uint8_t
{
    ComputeScratch = 0U,
    ComputeFinal,
    GraphicsScratch,
    GraphicsFinal,
    Count,
};

inline constexpr std::size_t kLabTextureCount = static_cast<std::size_t>(LabTextureIndex::Count);

struct LabGraph final
{
    ch08::frame_graph::CompiledPassGraph graph{};
    ch08::frame_graph::TextureResourceId computeScratch{};
    ch08::frame_graph::TextureResourceId computeFinal{};
    ch08::frame_graph::TextureResourceId graphicsScratch{};
    ch08::frame_graph::TextureResourceId graphicsFinal{};
    ch08::frame_graph::TextureResourceId frameTarget{};
    ch08::frame_graph::PassId computeGenerate{};
    ch08::frame_graph::PassId computeCollapse{};
    ch08::frame_graph::PassId graphicsGeometry{};
    ch08::frame_graph::PassId graphicsResolve{};
    ch08::frame_graph::PassId composite{};
};

struct PhysicalTextureByteSizes final
{
    std::array<std::uint64_t, kLabTextureCount> byTextureIndex{};
};

struct ScheduleComparison final
{
    SchedulePlan serial{};
    SchedulePlan async{};
};

struct ExecutedTextureBarrier final
{
    ch08::frame_graph::TextureResourceId resourceId{};
    lgp::framework::TextureBarrierState before{};
    lgp::framework::TextureBarrierState after{};
    D3D12_TEXTURE_BARRIER_FLAGS flags{D3D12_TEXTURE_BARRIER_FLAG_NONE};
};

enum class QueueExecutionTraceKind : std::uint8_t
{
    ComputeBranchRecorded = 0U,
    IndependentGraphicsBranchRecorded,
    CompositeRecordedForFrameworkSubmission,
    ComputeExecute,
    ComputeSignal,
    IndependentDirectExecute,
    DirectWaitBeforeComposite,
};

struct QueueExecutionTraceRecord final
{
    QueueExecutionTraceKind kind{};
    QueueKind queue{QueueKind::Direct};
    std::optional<std::uint64_t> fenceValue{};
};

enum class GpuIntervalKind : std::uint8_t
{
    ComputeBranch = 0U,
    IndependentGraphicsBranch,
    Composite,
};

struct GpuTimestampInterval final
{
    GpuIntervalKind kind{};
    QueueKind queue{QueueKind::Direct};
    std::uint64_t startTimestamp{};
    std::uint64_t endTimestamp{};
    std::optional<double> durationSeconds{};
    std::optional<double> normalizedStartSeconds{};
    std::optional<double> normalizedEndSeconds{};
};

struct GpuQueueCalibration final
{
    std::uint64_t frequency{};
    std::uint64_t gpuTimestamp{};
    std::uint64_t cpuQpc{};
    bool valid{};
};

struct GpuSubmissionCalibration final
{
    std::uint64_t frameIndex{};
    std::uint64_t qpcFrequency{};
    GpuQueueCalibration compute{};
    GpuQueueCalibration direct{};
    bool sharedCalibrationValid{};
};

struct GpuTimingSample final
{
    std::uint64_t frameIndex{};
    bool sharedCalibrationValid{};
    GpuSubmissionCalibration submissionCalibration{};
    std::array<GpuTimestampInterval, 3U> intervals{};
};

struct FrameSlotUsageRecord final
{
    std::uint64_t frameIndex{};
    UINT frameSlot{};
    std::uint64_t computeFenceValue{};
    std::uint64_t calibrationFrameIndex{};
};

struct LabShaders final
{
    lgp::framework::CompiledShader fullscreenVertex{};
    lgp::framework::CompiledShader computeGenerate{};
    lgp::framework::CompiledShader computeCollapse{};
    lgp::framework::CompiledShader graphicsGeometry{};
    lgp::framework::CompiledShader graphicsResolve{};
    lgp::framework::CompiledShader composite{};
};

struct LabPipelines final
{
    Microsoft::WRL::ComPtr<ID3D12PipelineState> computeGenerate{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> computeCollapse{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsGeometry{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsResolve{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> composite{};
};

[[nodiscard]] lgp::framework::TextureBarrierState UndefinedState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState ComputeUnorderedAccessState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState ComputeShaderResourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState ComputeProducedSharedShaderResourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState PixelShaderResourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(bool headless) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(bool headless) noexcept;

[[nodiscard]] D3D12_RESOURCE_DESC1 ComputeTextureDescription(lgp::framework::Extent2D size) noexcept;
[[nodiscard]] D3D12_RESOURCE_DESC1 GraphicsTextureDescription(lgp::framework::Extent2D size) noexcept;
[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(
    ID3D12Resource &resource, lgp::framework::TextureBarrierState before, lgp::framework::TextureBarrierState after,
    D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) noexcept;
void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers);

[[nodiscard]] std::expected<LabGraph, lgp::framework::Error> CompileLabGraph(bool headless);
[[nodiscard]] std::expected<PhysicalTextureByteSizes, lgp::framework::Error> QueryPhysicalTextureByteSizes(
    ID3D12Device10 &device, lgp::framework::Extent2D size);
[[nodiscard]] std::expected<ScheduleComparison, lgp::framework::Error> CompileScheduleComparison(
    LabGraph const &graph, PhysicalTextureByteSizes const &byteSizes);

[[nodiscard]] lgp::framework::Status CreateLabShaders(std::filesystem::path const &path, LabShaders &shaders);
[[nodiscard]] lgp::framework::Status CreateLabRootSignature(ID3D12Device10 &device,
                                                            Microsoft::WRL::ComPtr<ID3D12RootSignature> &rootSignature);
[[nodiscard]] lgp::framework::Status CreateLabPipelines(ID3D12Device10 &device, DXGI_FORMAT frameFormat,
                                                        ID3D12RootSignature &rootSignature, LabShaders const &shaders,
                                                        LabPipelines &pipelines);

} // namespace ch10::pass_scheduling::gpu
