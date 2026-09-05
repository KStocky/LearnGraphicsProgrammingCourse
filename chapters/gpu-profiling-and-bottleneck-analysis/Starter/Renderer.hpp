#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

namespace ch19::gpu_profiling::starter
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

// Coarse, single-sample evidence. It is deliberately incomplete: one uncalibrated raw timestamp pair and one
// CPU command-recording span, with no warm-up, repeats, calibration, fingerprint, or comparison. It exists only
// to *motivate* the disciplined Solution, never to draw a conclusion.
struct CoarseEvidence final
{
    // CPU wall-clock time spent recording this frame's command list (command recording only; excludes the
    // framework fence wait and the EndFrame execute/signal cost). Same honest meaning as the Solution's
    // per-sample cpuRecordingMilliseconds.
    double cpuRecordingMilliseconds{};
    std::uint64_t rawWorkloadBeginTick{};
    std::uint64_t rawWorkloadEndTick{};
    std::uint64_t gpuTimestampFrequencyHz{};
    bool timestampsSupported{false};
};

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept;
    // Consumes the coarse evidence recorded for the most recently submitted frame exactly once: a second call
    // without an intervening frame fails closed rather than returning a stale, already-consumed reading.
    [[nodiscard]] std::expected<CoarseEvidence, lgp::framework::Error> ReadCoarseEvidence();
    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;

  private:
    struct FrameSlot final
    {
        gpu::BufferResource timestampReadback{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipeline();
    [[nodiscard]] lgp::framework::Status CreateResources();
    [[nodiscard]] lgp::framework::Status UploadWaves();

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_{};
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestampHeap_{};
    gpu::BufferResource wavesBuffer_{};
    std::vector<FrameSlot> frameSlots_{};
    std::optional<HeadlessTestConfiguration> headlessConfiguration_{};
    CoarseEvidence pendingEvidence_{};
    UINT lastFrameSlot_{};
    bool pendingEvidenceValid_{false};
    bool headless_{};
    bool waveUploadPending_{true};
};

} // namespace ch19::gpu_profiling::starter
