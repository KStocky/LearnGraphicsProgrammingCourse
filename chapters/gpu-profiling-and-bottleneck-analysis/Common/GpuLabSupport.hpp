#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ProfilingContracts.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/device_resources.hpp>
#include <lgp/framework/error.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace ch19::gpu_profiling::gpu
{

// Modest resolution keeps the WARP software rasterizer fast while still producing a visually nontrivial,
// deterministic image.
inline constexpr std::uint32_t kRenderWidth = 128U;
inline constexpr std::uint32_t kRenderHeight = 72U;
inline constexpr std::uint32_t kMaximumWaveCount = 64U;
inline constexpr std::uint32_t kDefaultWaveCount = 32U;
inline constexpr std::uint32_t kDefaultIterations = 24U;
inline constexpr std::uint32_t kDefaultWarmupFrames = 3U;
inline constexpr std::uint32_t kDefaultMeasuredFrames = 8U;

// Four timestamps bracket every measured frame: frame begin, workload begin, workload end, frame end.
inline constexpr std::uint32_t kTimestampsPerFrame = 4U;
inline constexpr std::uint32_t kQueryFrameBegin = 0U;
inline constexpr std::uint32_t kQueryWorkloadBegin = 1U;
inline constexpr std::uint32_t kQueryWorkloadEnd = 2U;
inline constexpr std::uint32_t kQueryFrameEnd = 3U;

// The two equal-output variants under test. They differ only in how wave parameters are fetched.
enum class LabVariant : std::uint8_t
{
    Baseline = 0U,
    Candidate,
};

// Stable, printable PIX marker names. They match the scopes recorded by the Solution renderer and the plan
// validated against `ValidatePixScopes`.
inline constexpr std::string_view kPixFrameScopeName = "Ch19 Profiling Frame";
inline constexpr std::string_view kPixControlScopeName = "Controlled Region";
inline constexpr std::string_view kPixBaselineScopeName = "Baseline Workload";
inline constexpr std::string_view kPixCandidateScopeName = "Candidate Workload";
inline constexpr std::string_view kPixResolveScopeName = "Timestamp Resolve";

[[nodiscard]] std::wstring_view PixFrameScopeWide() noexcept;
[[nodiscard]] std::wstring_view PixControlScopeWide() noexcept;
[[nodiscard]] std::wstring_view PixVariantScopeWide(LabVariant variant) noexcept;
[[nodiscard]] std::wstring_view PixResolveScopeWide() noexcept;
[[nodiscard]] std::string_view PixVariantScopeName(LabVariant variant) noexcept;

struct LabConfiguration final
{
    LabVariant variant{LabVariant::Baseline};
    std::uint32_t waveCount{kDefaultWaveCount};
    std::uint32_t iterations{kDefaultIterations};
    std::uint32_t warmupFrames{kDefaultWarmupFrames};
    std::uint32_t measuredFrames{kDefaultMeasuredFrames};

    [[nodiscard]] bool operator==(LabConfiguration const &) const noexcept = default;
};

// GPU/CPU 32-byte wave record. The identical layout is uploaded to both the constant buffer and the structured
// buffer so the two variants load identical bits.
struct WaveParam final
{
    std::array<float, 4U> a{}; // xy = direction, z = frequency, w = phase
    std::array<float, 4U> b{}; // x = amplitude, yz = center, w = falloff weight

    [[nodiscard]] bool operator==(WaveParam const &) const noexcept = default;
};

static_assert(sizeof(WaveParam) == 32U);

// 16-byte root constants matching the HLSL RenderConstants layout.
struct RenderConstants final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t waveCount{};
    std::uint32_t iterations{};
};

static_assert(sizeof(RenderConstants) == 16U);

// A single queue's `GetClockCalibration`/`GetTimestampFrequency` sample, paired with the QPC frequency captured
// from the same instant. `valid` and `timestampsSupported` record honestly whether the API delivered a usable
// alignment rather than fabricating zeros.
struct GpuClockCalibration final
{
    bool timestampsSupported{false};
    bool valid{false};
    std::uint64_t gpuTimestampFrequencyHz{};
    std::uint64_t gpuCalibrationTick{};
    std::uint64_t qpcCalibrationTick{};
    std::uint64_t qpcFrequencyHz{};

    [[nodiscard]] bool operator==(GpuClockCalibration const &) const noexcept = default;
};

// Everything captured for one variant across a warm-up + measured series.
struct MeasuredExperiment final
{
    LabVariant variant{LabVariant::Baseline};
    std::uint64_t workloadId{};
    std::uint64_t outputFingerprint{};
    // Reporting only: a single representative queue calibration captured for the series. Each MeasurementSample
    // already carries calibratedIntervals produced from its OWN per-frame calibration, so this field must never
    // be read as the calibration that produced every sample's intervals.
    GpuClockCalibration representativeCalibration{};
    SeriesPolicy policy{};
    std::vector<MeasurementSample> samples{};
    SeriesStatistics statistics{};
};

// The complete, honest evidence bundle a disciplined profiling pass produces.
struct ProfilingReport final
{
    std::uint64_t workloadId{};
    std::uint64_t baselineFingerprint{};
    std::uint64_t candidateFingerprint{};
    bool outputsIdentical{false};
    ExperimentComparison comparison{};
    std::uint32_t pixScopeDepth{};
    std::vector<PixScopeMarker> pixScopePlan{};
    HypothesisAssessment hypothesis{};
    std::vector<NextExperiment> nextExperiments{};
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

    [[nodiscard]] ID3D12Resource *Get() const noexcept;
    [[nodiscard]] std::uint64_t size_in_bytes() const noexcept;
    [[nodiscard]] std::byte *mapped_data() noexcept;
    [[nodiscard]] std::byte const *mapped_data() const noexcept;

  private:
    friend std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
        ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
        std::wstring_view name, bool mapPersistently);

    void Reset() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::uint64_t sizeInBytes_{};
    std::byte *mappedData_{};
    // Records the heap type the buffer was created on so the persistent map/unmap can declare honest CPU access
    // ranges (see PersistentMapReadRange / PersistentUnmapWrittenRange).
    D3D12_HEAP_TYPE heapType_{D3D12_HEAP_TYPE_DEFAULT};
};

// Persistent-map CPU access ranges by heap type. A readback buffer is mapped so the CPU can read every byte the
// GPU wrote, so its Map read range spans the whole buffer and its Unmap written range is empty. An upload (or
// default) buffer is write-only from the CPU, so its Map read range is empty (End <= Begin, declaring no CPU
// reads) and its Unmap written range conservatively covers the whole mapped buffer.
[[nodiscard]] D3D12_RANGE PersistentMapReadRange(D3D12_HEAP_TYPE heapType, std::uint64_t sizeInBytes) noexcept;
[[nodiscard]] D3D12_RANGE PersistentUnmapWrittenRange(D3D12_HEAP_TYPE heapType, std::uint64_t sizeInBytes) noexcept;

[[nodiscard]] constexpr std::uint32_t NormalizeWaveCount(std::uint32_t waveCount) noexcept
{
    if (waveCount == 0U)
    {
        return 1U;
    }
    return waveCount < kMaximumWaveCount ? waveCount : kMaximumWaveCount;
}

// Deterministic wave parameters. Identical for both variants and every run.
[[nodiscard]] std::vector<WaveParam> BuildWaveParams(std::uint32_t waveCount);

// Identifies the workload dimensions (resolution + wave count + iterations). Deliberately variant-independent:
// baseline and candidate share a workload identity so `CompareExperiment` can treat them as one experiment.
[[nodiscard]] std::uint64_t ComputeWorkloadId(LabConfiguration const &configuration) noexcept;

// Stable 64-bit fingerprint binding the frame dimensions, format, and tightly packed RGBA pixels (row padding
// excluded). Fails closed via std::expected on malformed or non-RGBA8 input instead of returning a partial hash.
[[nodiscard]] std::expected<std::uint64_t, lgp::framework::Error> ComputeFrameFingerprint(
    lgp::framework::RenderTargetReadback const &frame);

// Byte-for-byte equality over the meaningful (non-padding) pixels of two readbacks. Malformed input is reported
// as a distinct error rather than as inequality; two valid readbacks that differ in dimensions or contents
// compare unequal.
[[nodiscard]] std::expected<bool, lgp::framework::Error> FramesEqual(lgp::framework::RenderTargetReadback const &left,
                                                                     lgp::framework::RenderTargetReadback const &right);

// The canonical nested-scope plan the Solution records every measured frame:
//   Frame [ Controlled Region [ <Variant> Workload ] Timestamp Resolve ]
[[nodiscard]] std::vector<PixScopeMarker> BuildPixScopePlan(LabVariant variant);

// Assembles the per-variant evidence: aggregates the series statistics for `metric` under `policy`.
[[nodiscard]] std::expected<MeasuredExperiment, lgp::framework::Error> AssembleExperiment(
    LabConfiguration const &configuration, std::span<MeasurementSample const> samples, std::uint64_t outputFingerprint,
    GpuClockCalibration const &calibration, SampleMetric metric = SampleMetric::GpuBusyUnionMilliseconds);

[[nodiscard]] ExperimentObservation BuildObservation(MeasuredExperiment const &experiment) noexcept;

// Assesses the arithmetic-throughput hypothesis honestly: on WARP the utilization counters are unavailable, so
// the equal-output experiment alone can never promote the conclusion past `CounterUnavailable`.
[[nodiscard]] std::expected<HypothesisAssessment, lgp::framework::Error> AssessArithmeticHypothesis(
    bool equalOutputExperimentSupported);

// Combines two equal-output variants into a full report: comparison, PIX plan, and honest hypothesis.
[[nodiscard]] std::expected<ProfilingReport, lgp::framework::Error> BuildProfilingReport(
    MeasuredExperiment const &baseline, MeasuredExperiment const &candidate,
    double noiseRelativeThreshold = kDefaultExperimentNoiseThreshold);

// --- D3D12 helpers (mirroring the shared framework conventions used by the other GPU labs) ---

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;

[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(
    ID3D12Resource &resource, lgp::framework::TextureBarrierState before, lgp::framework::TextureBarrierState after,
    D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) noexcept;
void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers);

[[nodiscard]] std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
    ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
    std::wstring_view name, bool mapPersistently = false);
[[nodiscard]] lgp::framework::Status WriteBuffer(BufferResource &buffer, std::span<std::byte const> bytes,
                                                 std::uint64_t destinationOffset = 0U);

template <typename T>
[[nodiscard]] inline lgp::framework::Status WriteBuffer(BufferResource &buffer, std::span<T const> values,
                                                        std::uint64_t destinationOffset = 0U)
{
    return WriteBuffer(buffer, std::as_bytes(values), destinationOffset);
}

[[nodiscard]] std::expected<Microsoft::WRL::ComPtr<ID3D12QueryHeap>, lgp::framework::Error> CreateTimestampHeap(
    ID3D12Device10 &device, std::uint32_t timestampCount, std::wstring_view name);

// Reads the queue's timestamp frequency, its GPU/CPU clock calibration, and the matching QPC frequency. Returns
// a record whose `timestampsSupported`/`valid` flags report honestly whether the API delivered usable data.
[[nodiscard]] GpuClockCalibration CaptureQueueCalibration(ID3D12CommandQueue &queue) noexcept;

// Calibrates one raw graphics-queue workload interval onto the shared QPC timeline (via the contracts'
// `CalibrateRawInterval`) and packages it with the CPU-side steady_clock timings as a `MeasurementSample`.
[[nodiscard]] std::expected<MeasurementSample, lgp::framework::Error> BuildMeasurementSample(
    std::uint64_t rawWorkloadBeginTick, std::uint64_t rawWorkloadEndTick, GpuClockCalibration const &calibration,
    double cpuRecordingMilliseconds);

[[nodiscard]] lgp::framework::Status CompileShader(lgp::framework::ShaderCompiler &compiler,
                                                   lgp::framework::ShaderCompileOptions &options,
                                                   wchar_t const *entryPoint, wchar_t const *targetProfile,
                                                   lgp::framework::CompiledShader &shader);

} // namespace ch19::gpu_profiling::gpu
