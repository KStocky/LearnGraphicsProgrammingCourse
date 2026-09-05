#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

namespace ch19::gpu_profiling::solution
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    // Programmatic profiling API used by the GPU tests, which sequence the baseline and candidate variants into
    // a controlled A/B experiment. A windowed run selects one stable variant at a time with 1 or 2 so each PIX
    // capture contains one unambiguous workload rather than automatic in-app alternation.
    void ConfigureExperiment(HeadlessTestConfiguration const &configuration) noexcept;
    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;
    [[nodiscard]] gpu::GpuClockCalibration const &LastCalibration() const noexcept;
    [[nodiscard]] bool TimestampsSupported() const noexcept;

    // Reads back the timestamps resolved for the most recently submitted frame, calibrates them onto the shared
    // QPC timeline, and returns one canonical MeasurementSample. Must be called only after the frame's fence has
    // completed (the harness waits for GPU idle first). Pending evidence is consumed exactly once: a second call
    // without an intervening frame fails closed rather than returning a stale sample.
    [[nodiscard]] std::expected<MeasurementSample, lgp::framework::Error> ReadPendingSample();

  private:
    struct FrameSlot final
    {
        gpu::BufferResource timestampReadback{};
    };

    struct PendingFrame final
    {
        gpu::GpuClockCalibration calibration{};
        double cpuRecordingMilliseconds{};
        UINT frameSlot{};
        bool valid{false};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateResources();
    [[nodiscard]] lgp::framework::Status UploadWaves();

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader baselinePixelShader_{};
    lgp::framework::CompiledShader candidatePixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> baselinePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> candidatePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestampHeap_{};
    gpu::BufferResource wavesBuffer_{};
    lgp::framework::DescriptorAllocation wavesDescriptor_{};
    std::vector<FrameSlot> frameSlots_{};
    std::optional<HeadlessTestConfiguration> headlessConfiguration_{};
    gpu::LabVariant interactiveVariant_{gpu::LabVariant::Baseline};
    PendingFrame pendingFrame_{};
    gpu::GpuClockCalibration lastCalibration_{};
    bool headless_{};
    bool waveUploadPending_{true};
};

} // namespace ch19::gpu_profiling::solution
