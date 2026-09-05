#include "GpuLabSupport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace ch19::gpu_profiling::gpu
{
namespace
{

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] std::uint64_t FnvHashBytes(std::uint64_t hash, std::byte const *data, std::size_t size) noexcept
{
    for (std::size_t index = 0U; index < size; ++index)
    {
        hash ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[index]));
        hash *= kFnvPrime;
    }
    return hash;
}

[[nodiscard]] std::uint64_t FnvHashUint32(std::uint64_t hash, std::uint32_t value) noexcept
{
    std::array<std::byte, 4U> bytes{};
    std::memcpy(bytes.data(), &value, bytes.size());
    return FnvHashBytes(hash, bytes.data(), bytes.size());
}

[[nodiscard]] lgp::framework::Error ContractFailure(char const *operation, ContractError error)
{
    return lgp::framework::MakeError(operation, "Chapter 19 profiling contract failed with code " +
                                                    std::to_string(static_cast<unsigned int>(error)) + ".");
}

[[nodiscard]] float DeterministicWave(std::uint32_t index, std::uint32_t component) noexcept
{
    // A fixed, transcendental-seeded pattern: deterministic, spread across the plane, and independent of any
    // runtime state so every run and both variants build identical wave tables.
    double const seed = static_cast<double>(index) * 12.9898 + static_cast<double>(component) * 78.233 + 1.0;
    double const wrapped = std::sin(seed) * 43758.5453;
    return static_cast<float>(wrapped - std::floor(wrapped));
}

// Validates a readback fully before any hash or comparison touches its bytes: nonzero dimensions, the expected
// RGBA8 format, a row pitch at least width*4 (computed without overflow), and pixel storage that actually
// covers every row. Any failure is returned as a typed error so callers fail closed instead of hashing partial
// or mis-shaped data.
[[nodiscard]] std::expected<std::uint64_t, lgp::framework::Error> ValidateReadbackRowBytes(
    lgp::framework::RenderTargetReadback const &frame, char const *operation)
{
    if (frame.size.width == 0U || frame.size.height == 0U)
    {
        return std::unexpected(lgp::framework::MakeError(operation, "Chapter 19 readback has zero dimensions."));
    }
    if (frame.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        return std::unexpected(
            lgp::framework::MakeError(operation, "Chapter 19 readback format is not DXGI_FORMAT_R8G8B8A8_UNORM."));
    }
    // width is 32-bit, so width * 4 cannot overflow a 64-bit accumulator.
    std::uint64_t const rowBytes = static_cast<std::uint64_t>(frame.size.width) * 4ULL;
    std::uint64_t const rowPitch = frame.rowPitch;
    if (rowPitch < rowBytes)
    {
        return std::unexpected(
            lgp::framework::MakeError(operation, "Chapter 19 readback row pitch is smaller than width * 4."));
    }
    // Coverage of every row is the offset of the last row plus its meaningful bytes. Guard the multiplication
    // (rowsBeforeLast * rowPitch) against 64-bit overflow before comparing against the pixel storage.
    std::uint64_t const rowsBeforeLast = static_cast<std::uint64_t>(frame.size.height) - 1ULL;
    if (rowsBeforeLast != 0ULL && rowPitch > (std::numeric_limits<std::uint64_t>::max() - rowBytes) / rowsBeforeLast)
    {
        return std::unexpected(
            lgp::framework::MakeError(operation, "Chapter 19 readback dimensions overflow the address space."));
    }
    std::uint64_t const requiredBytes = rowsBeforeLast * rowPitch + rowBytes;
    if (requiredBytes > frame.pixels.size())
    {
        return std::unexpected(
            lgp::framework::MakeError(operation, "Chapter 19 readback pixel storage does not cover every row."));
    }
    return rowBytes;
}

} // namespace

std::wstring_view PixFrameScopeWide() noexcept
{
    static std::wstring const name{L"Ch19 Profiling Frame"};
    return name;
}

std::wstring_view PixControlScopeWide() noexcept
{
    static std::wstring const name{L"Controlled Region"};
    return name;
}

std::wstring_view PixVariantScopeWide(LabVariant const variant) noexcept
{
    static std::wstring const baseline{L"Baseline Workload"};
    static std::wstring const candidate{L"Candidate Workload"};
    return variant == LabVariant::Baseline ? baseline : candidate;
}

std::wstring_view PixResolveScopeWide() noexcept
{
    static std::wstring const name{L"Timestamp Resolve"};
    return name;
}

std::string_view PixVariantScopeName(LabVariant const variant) noexcept
{
    return variant == LabVariant::Baseline ? kPixBaselineScopeName : kPixCandidateScopeName;
}

BufferResource::BufferResource(BufferResource &&other) noexcept
{
    *this = std::move(other);
}

BufferResource &BufferResource::operator=(BufferResource &&other) noexcept
{
    if (this != &other)
    {
        Reset();
        resource_ = std::move(other.resource_);
        sizeInBytes_ = std::exchange(other.sizeInBytes_, 0U);
        mappedData_ = std::exchange(other.mappedData_, nullptr);
        heapType_ = std::exchange(other.heapType_, D3D12_HEAP_TYPE_DEFAULT);
    }
    return *this;
}

BufferResource::~BufferResource()
{
    Reset();
}

ID3D12Resource *BufferResource::Get() const noexcept
{
    return resource_.Get();
}

std::uint64_t BufferResource::size_in_bytes() const noexcept
{
    return sizeInBytes_;
}

std::byte *BufferResource::mapped_data() noexcept
{
    return mappedData_;
}

std::byte const *BufferResource::mapped_data() const noexcept
{
    return mappedData_;
}

void BufferResource::Reset() noexcept
{
    if (resource_ != nullptr && mappedData_ != nullptr)
    {
        // Persistently mapped buffers are unmapped only here, at destruction/move. Declare an honest written
        // range so the runtime flushes exactly what the CPU may have written: the whole buffer for upload/default
        // heaps, nothing for readback heaps.
        D3D12_RANGE const writtenRange = PersistentUnmapWrittenRange(heapType_, sizeInBytes_);
        resource_->Unmap(0U, &writtenRange);
    }
    resource_.Reset();
    sizeInBytes_ = 0U;
    mappedData_ = nullptr;
    heapType_ = D3D12_HEAP_TYPE_DEFAULT;
}

D3D12_RANGE PersistentMapReadRange(D3D12_HEAP_TYPE const heapType, std::uint64_t const sizeInBytes) noexcept
{
    if (heapType == D3D12_HEAP_TYPE_READBACK)
    {
        return D3D12_RANGE{0U, static_cast<SIZE_T>(sizeInBytes)};
    }
    // End <= Begin declares that the CPU will not read the mapped upload/default buffer.
    return D3D12_RANGE{0U, 0U};
}

D3D12_RANGE PersistentUnmapWrittenRange(D3D12_HEAP_TYPE const heapType, std::uint64_t const sizeInBytes) noexcept
{
    if (heapType == D3D12_HEAP_TYPE_READBACK)
    {
        // The CPU never writes a readback buffer, so nothing needs flushing on Unmap.
        return D3D12_RANGE{0U, 0U};
    }
    // Conservatively report the whole mapped upload/default buffer as written by the CPU.
    return D3D12_RANGE{0U, static_cast<SIZE_T>(sizeInBytes)};
}

std::vector<WaveParam> BuildWaveParams(std::uint32_t const waveCount)
{
    std::uint32_t const count = NormalizeWaveCount(waveCount);
    std::vector<WaveParam> waves{};
    waves.reserve(count);
    for (std::uint32_t index = 0U; index < count; ++index)
    {
        float const angle = DeterministicWave(index, 0U) * 6.2831853F;
        float const frequency = 2.0F + DeterministicWave(index, 1U) * 9.0F;
        float const phase = DeterministicWave(index, 2U) * 6.2831853F;
        float const amplitude = 0.25F + DeterministicWave(index, 3U) * 0.75F;
        float const centerX = -0.8F + DeterministicWave(index, 4U) * 1.6F;
        float const centerY = -0.8F + DeterministicWave(index, 5U) * 1.6F;
        float const weight = 0.4F + DeterministicWave(index, 6U) * 1.6F;
        waves.push_back(WaveParam{
            .a = {std::cos(angle), std::sin(angle), frequency, phase},
            .b = {amplitude, centerX, centerY, weight},
        });
    }
    return waves;
}

std::uint64_t ComputeWorkloadId(LabConfiguration const &configuration) noexcept
{
    std::uint64_t hash = kFnvOffsetBasis;
    hash = FnvHashUint32(hash, kRenderWidth);
    hash = FnvHashUint32(hash, kRenderHeight);
    hash = FnvHashUint32(hash, NormalizeWaveCount(configuration.waveCount));
    hash = FnvHashUint32(hash, std::max(configuration.iterations, 1U));
    return hash;
}

std::expected<std::uint64_t, lgp::framework::Error> ComputeFrameFingerprint(
    lgp::framework::RenderTargetReadback const &frame)
{
    auto const rowBytes = ValidateReadbackRowBytes(frame, "ComputeFrameFingerprint");
    if (!rowBytes)
    {
        return std::unexpected(rowBytes.error());
    }
    // Bind the dimensions and format into the hash so frames that differ only in shape can never collide with a
    // frame whose meaningful bytes happen to match.
    std::uint64_t hash = kFnvOffsetBasis;
    hash = FnvHashUint32(hash, frame.size.width);
    hash = FnvHashUint32(hash, frame.size.height);
    hash = FnvHashUint32(hash, static_cast<std::uint32_t>(frame.format));
    for (std::uint32_t y = 0U; y < frame.size.height; ++y)
    {
        std::size_t const offset = static_cast<std::size_t>(y) * frame.rowPitch;
        hash = FnvHashBytes(hash, frame.pixels.data() + offset, static_cast<std::size_t>(*rowBytes));
    }
    return hash;
}

std::expected<bool, lgp::framework::Error> FramesEqual(lgp::framework::RenderTargetReadback const &left,
                                                       lgp::framework::RenderTargetReadback const &right)
{
    auto const leftRowBytes = ValidateReadbackRowBytes(left, "FramesEqual");
    if (!leftRowBytes)
    {
        return std::unexpected(leftRowBytes.error());
    }
    auto const rightRowBytes = ValidateReadbackRowBytes(right, "FramesEqual");
    if (!rightRowBytes)
    {
        return std::unexpected(rightRowBytes.error());
    }
    // Both inputs are valid RGBA8 readbacks. A dimension mismatch means the two valid outputs genuinely differ,
    // which is inequality rather than malformed input.
    if (left.size != right.size)
    {
        return false;
    }
    for (std::uint32_t y = 0U; y < left.size.height; ++y)
    {
        std::size_t const leftOffset = static_cast<std::size_t>(y) * left.rowPitch;
        std::size_t const rightOffset = static_cast<std::size_t>(y) * right.rowPitch;
        if (std::memcmp(left.pixels.data() + leftOffset, right.pixels.data() + rightOffset,
                        static_cast<std::size_t>(*leftRowBytes)) != 0)
        {
            return false;
        }
    }
    return true;
}

std::vector<PixScopeMarker> BuildPixScopePlan(LabVariant const variant)
{
    return {
        {PixScopeMarkerKind::Begin, kPixFrameScopeName},
        {PixScopeMarkerKind::Begin, kPixControlScopeName},
        {PixScopeMarkerKind::Begin, PixVariantScopeName(variant)},
        {PixScopeMarkerKind::End, {}},
        {PixScopeMarkerKind::End, {}},
        {PixScopeMarkerKind::Begin, kPixResolveScopeName},
        {PixScopeMarkerKind::End, {}},
        {PixScopeMarkerKind::End, {}},
    };
}

std::expected<MeasurementSample, lgp::framework::Error> BuildMeasurementSample(std::uint64_t const rawWorkloadBeginTick,
                                                                               std::uint64_t const rawWorkloadEndTick,
                                                                               GpuClockCalibration const &calibration,
                                                                               double const cpuRecordingMilliseconds)
{
    if (!calibration.valid)
    {
        return std::unexpected(lgp::framework::MakeError(
            "BuildMeasurementSample", "The queue clock calibration is invalid; timestamps are unsupported."));
    }
    RawTimestampInterval const raw{
        .queue = QueueKind::Graphics, .rawBeginTick = rawWorkloadBeginTick, .rawEndTick = rawWorkloadEndTick};
    QueueClockCalibration const queueCalibration{
        .queue = QueueKind::Graphics,
        .gpuCalibrationTick = calibration.gpuCalibrationTick,
        .qpcCalibrationTick = calibration.qpcCalibrationTick,
        .gpuTimestampFrequencyHz = calibration.gpuTimestampFrequencyHz,
        .qpcFrequencyHz = calibration.qpcFrequencyHz,
    };
    auto const calibrated = CalibrateRawInterval(raw, queueCalibration);
    if (!calibrated)
    {
        return std::unexpected(ContractFailure("CalibrateRawInterval", calibrated.error()));
    }
    return MeasurementSample{
        .cpuRecordingMilliseconds = cpuRecordingMilliseconds,
        .qpcFrequencyHz = calibration.qpcFrequencyHz,
        .calibratedIntervals = {*calibrated},
    };
}

std::expected<MeasuredExperiment, lgp::framework::Error> AssembleExperiment(LabConfiguration const &configuration,
                                                                            std::span<MeasurementSample const> samples,
                                                                            std::uint64_t const outputFingerprint,
                                                                            GpuClockCalibration const &calibration,
                                                                            SampleMetric const metric)
{
    // A series with no measured frames has nothing to aggregate; never coerce it to one.
    if (configuration.measuredFrames == 0U)
    {
        return std::unexpected(ContractFailure("AssembleExperiment", ContractError::InvalidMinimumSampleCount));
    }
    // Guard the uint32 total-frame addition before it can wrap.
    if (configuration.warmupFrames > std::numeric_limits<std::uint32_t>::max() - configuration.measuredFrames)
    {
        return std::unexpected(ContractFailure("AssembleExperiment", ContractError::MeasuredSampleCountOverflow));
    }
    std::uint32_t const expectedSampleCount = configuration.warmupFrames + configuration.measuredFrames;
    // Require exactly warmup + measured samples: extra samples are rejected rather than silently averaged in.
    if (samples.size() != static_cast<std::size_t>(expectedSampleCount))
    {
        return std::unexpected(ContractFailure("AssembleExperiment", ContractError::MismatchedMeasuredSampleCount));
    }
    SeriesPolicy const policy{
        .warmupSampleCount = configuration.warmupFrames,
        .minimumMeasuredSamples = configuration.measuredFrames,
        .stabilityRelativeSpreadThreshold = 0.1,
    };
    auto const statistics = AggregateSeries(policy, samples, metric);
    if (!statistics)
    {
        return std::unexpected(ContractFailure("AggregateSeries", statistics.error()));
    }
    MeasuredExperiment experiment{};
    experiment.variant = configuration.variant;
    experiment.workloadId = ComputeWorkloadId(configuration);
    experiment.outputFingerprint = outputFingerprint;
    experiment.representativeCalibration = calibration;
    experiment.policy = policy;
    experiment.samples.assign(samples.begin(), samples.end());
    experiment.statistics = *statistics;
    return experiment;
}

ExperimentObservation BuildObservation(MeasuredExperiment const &experiment) noexcept
{
    return ExperimentObservation{
        .workloadId = experiment.workloadId,
        .outputFingerprint = experiment.outputFingerprint,
        .policy = experiment.policy,
        .statistics = experiment.statistics,
    };
}

std::expected<HypothesisAssessment, lgp::framework::Error> AssessArithmeticHypothesis(
    bool const equalOutputExperimentSupported)
{
    std::array<EvidenceRecord, 3U> const evidence{{
        {.kind = EvidenceKind::ArithmeticUtilizationCounter,
         .availability = EvidenceAvailability::CounterUnavailable,
         .supportsHypothesis = false},
        {.kind = EvidenceKind::CompilerResourceReport,
         .availability = EvidenceAvailability::NotCollected,
         .supportsHypothesis = false},
        {.kind = EvidenceKind::EqualOutputControlledExperiment,
         .availability = EvidenceAvailability::Measured,
         .supportsHypothesis = equalOutputExperimentSupported},
    }};
    auto const assessment = ClassifyHypothesis(BottleneckHypothesis::ArithmeticThroughput, evidence);
    if (!assessment)
    {
        return std::unexpected(ContractFailure("ClassifyHypothesis", assessment.error()));
    }
    return *assessment;
}

std::expected<ProfilingReport, lgp::framework::Error> BuildProfilingReport(MeasuredExperiment const &baseline,
                                                                           MeasuredExperiment const &candidate,
                                                                           double const noiseRelativeThreshold)
{
    ExperimentObservation const baselineObservation = BuildObservation(baseline);
    ExperimentObservation const candidateObservation = BuildObservation(candidate);
    auto const comparison = CompareExperiment(baselineObservation, candidateObservation, noiseRelativeThreshold);
    if (!comparison)
    {
        return std::unexpected(ContractFailure("CompareExperiment", comparison.error()));
    }

    std::vector<PixScopeMarker> const plan = BuildPixScopePlan(LabVariant::Baseline);
    auto const depth = ValidatePixScopes(plan);
    if (!depth)
    {
        return std::unexpected(ContractFailure("ValidatePixScopes", depth.error()));
    }

    bool const outputsIdentical =
        baseline.workloadId == candidate.workloadId && baseline.outputFingerprint == candidate.outputFingerprint;
    auto const hypothesis = AssessArithmeticHypothesis(outputsIdentical);
    if (!hypothesis)
    {
        return std::unexpected(hypothesis.error());
    }

    ProfilingReport report{};
    report.workloadId = baseline.workloadId;
    report.baselineFingerprint = baseline.outputFingerprint;
    report.candidateFingerprint = candidate.outputFingerprint;
    report.outputsIdentical = outputsIdentical;
    report.comparison = *comparison;
    report.pixScopeDepth = *depth;
    report.pixScopePlan = plan;
    report.hypothesis = *hypothesis;
    report.nextExperiments = SuggestNextExperiments(*hypothesis);
    return report;
}

lgp::framework::TextureBarrierState FrameStartState(lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frameContext.renderTargetInitialLayout};
}

lgp::framework::TextureBarrierState FrameEndState(lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
            frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT};
}

lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
}

D3D12_TEXTURE_BARRIER MakeTextureBarrier(ID3D12Resource &resource, lgp::framework::TextureBarrierState before,
                                         lgp::framework::TextureBarrierState after,
                                         D3D12_TEXTURE_BARRIER_FLAGS flags) noexcept
{
    D3D12_TEXTURE_BARRIER barrier{};
    barrier.SyncBefore = before.sync;
    barrier.SyncAfter = after.sync;
    barrier.AccessBefore = before.access;
    barrier.AccessAfter = after.access;
    barrier.LayoutBefore = before.layout;
    barrier.LayoutAfter = after.layout;
    barrier.pResource = &resource;
    barrier.Subresources.IndexOrFirstMipLevel = UINT32_MAX;
    barrier.Flags = flags;
    return barrier;
}

void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers)
{
    if (!barriers.empty())
    {
        D3D12_BARRIER_GROUP group{};
        group.Type = D3D12_BARRIER_TYPE_TEXTURE;
        group.NumBarriers = static_cast<UINT>(barriers.size());
        group.pTextureBarriers = barriers.data();
        commandList.Barrier(1U, &group);
    }
}

std::expected<BufferResource, lgp::framework::Error> CreateBuffer(ID3D12Device10 &device, std::uint64_t sizeInBytes,
                                                                  D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
                                                                  std::wstring_view name, bool mapPersistently)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    heap.CreationNodeMask = 1U;
    heap.VisibleNodeMask = 1U;
    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = sizeInBytes;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;

    BufferResource buffer{};
    HRESULT const result = device.CreateCommittedResource3(
        &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0U, nullptr,
        IID_PPV_ARGS(buffer.resource_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                                "Failed to create a Chapter 19 buffer."));
    }
    std::wstring const resourceName{name};
    if (!resourceName.empty())
    {
        (void)buffer.resource_->SetName(resourceName.c_str());
    }
    buffer.heapType_ = heapType;
    if (mapPersistently)
    {
        // Declare CPU read intent honestly: readback heaps span the whole buffer, upload/default heaps declare
        // no reads. The buffer stays persistently mapped until Reset, which unmaps with the matching written
        // range.
        D3D12_RANGE const readRange = PersistentMapReadRange(heapType, sizeInBytes);
        void *mapped{};
        HRESULT const mapResult = buffer.resource_->Map(0U, &readRange, &mapped);
        if (FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map a Chapter 19 buffer."));
        }
        buffer.mappedData_ = static_cast<std::byte *>(mapped);
    }
    buffer.sizeInBytes_ = sizeInBytes;
    return buffer;
}

lgp::framework::Status WriteBuffer(BufferResource &buffer, std::span<std::byte const> bytes,
                                   std::uint64_t destinationOffset)
{
    if (buffer.mapped_data() == nullptr || destinationOffset > buffer.size_in_bytes() ||
        bytes.size_bytes() > buffer.size_in_bytes() - destinationOffset)
    {
        return std::unexpected(lgp::framework::MakeError("WriteBuffer", "Chapter 19 upload write is invalid."));
    }
    std::memcpy(buffer.mapped_data() + destinationOffset, bytes.data(), bytes.size_bytes());
    return {};
}

std::expected<Microsoft::WRL::ComPtr<ID3D12QueryHeap>, lgp::framework::Error> CreateTimestampHeap(
    ID3D12Device10 &device, std::uint32_t const timestampCount, std::wstring_view name)
{
    D3D12_QUERY_HEAP_DESC description{};
    description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    description.Count = timestampCount;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap{};
    HRESULT const result = device.CreateQueryHeap(&description, IID_PPV_ARGS(heap.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateQueryHeap", result,
                                                                "Failed to create a Chapter 19 timestamp query heap."));
    }
    std::wstring const heapName{name};
    if (!heapName.empty())
    {
        (void)heap->SetName(heapName.c_str());
    }
    return heap;
}

GpuClockCalibration CaptureQueueCalibration(ID3D12CommandQueue &queue) noexcept
{
    GpuClockCalibration calibration{};
    LARGE_INTEGER qpcFrequency{};
    if (QueryPerformanceFrequency(&qpcFrequency) != FALSE)
    {
        calibration.qpcFrequencyHz = static_cast<std::uint64_t>(qpcFrequency.QuadPart);
    }
    HRESULT const frequencyResult = queue.GetTimestampFrequency(&calibration.gpuTimestampFrequencyHz);
    HRESULT const clockResult =
        queue.GetClockCalibration(&calibration.gpuCalibrationTick, &calibration.qpcCalibrationTick);
    calibration.timestampsSupported = SUCCEEDED(frequencyResult) && calibration.gpuTimestampFrequencyHz != 0U;
    calibration.valid = calibration.timestampsSupported && SUCCEEDED(clockResult) && calibration.qpcFrequencyHz != 0U;
    return calibration;
}

lgp::framework::Status CompileShader(lgp::framework::ShaderCompiler &compiler,
                                     lgp::framework::ShaderCompileOptions &options, wchar_t const *entryPoint,
                                     wchar_t const *targetProfile, lgp::framework::CompiledShader &shader)
{
    options.entryPoint = entryPoint;
    options.targetProfile = targetProfile;
    options.additionalArguments = {L"-E", entryPoint, L"-T", targetProfile};
    auto result = compiler.Compile(options);
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    shader = std::move(*result);
    return {};
}

} // namespace ch19::gpu_profiling::gpu
