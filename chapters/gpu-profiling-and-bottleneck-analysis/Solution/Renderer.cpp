#include "Renderer.hpp"

#include <lgp/framework/pix.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>

namespace ch19::gpu_profiling::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kWavesSrvDescriptorCount = 1U;

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "ProfilingLab.hlsl";
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    return blob == nullptr ? std::string{}
                           : std::string{static_cast<char const *>(blob->GetBufferPointer()), blob->GetBufferSize()};
}

[[nodiscard]] double MillisecondsBetween(std::chrono::steady_clock::time_point begin,
                                         std::chrono::steady_clock::time_point end) noexcept
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

[[nodiscard]] lgp::framework::Status CreateGraphicsPipeline(ID3D12Device10 &device, ID3D12RootSignature &rootSignature,
                                                            lgp::framework::CompiledShader const &vertexShader,
                                                            lgp::framework::CompiledShader const &pixelShader,
                                                            DXGI_FORMAT renderTargetFormat,
                                                            ComPtr<ID3D12PipelineState> &pipeline)
{
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &rootSignature;
    description.VS = vertexShader.Bytecode();
    description.PS = pixelShader.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = renderTargetFormat;
    description.SampleDesc.Count = 1U;
    HRESULT const result =
        device.CreateGraphicsPipelineState(&description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create a Chapter 19 solution pipeline."));
    }
    return {};
}

} // namespace

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    gpu::LabConfiguration configuration =
        headless_ && headlessConfiguration_.has_value() ? *headlessConfiguration_ : gpu::LabConfiguration{};
    if (!headless_)
    {
        configuration.variant = interactiveVariant_;
    }
    configuration.waveCount = gpu::NormalizeWaveCount(configuration.waveCount);
    configuration.iterations = std::max(configuration.iterations, 1U);
    return configuration;
}

gpu::GpuClockCalibration const &Renderer::LastCalibration() const noexcept
{
    return lastCalibration_;
}

bool Renderer::TimestampsSupported() const noexcept
{
    return lastCalibration_.timestampsSupported;
}

lgp::framework::Status Renderer::CreateShaders()
{
    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    lgp::framework::ShaderCompiler compiler = std::move(*compilerResult);
    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = ShaderPath();
    options.includeDirectories = {options.sourcePath.parent_path(),
                                  options.sourcePath.parent_path().parent_path() / "Common"};
    if (auto status = gpu::CompileShader(compiler, options, L"FullScreenVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"BaselinePS", L"ps_6_0", baselinePixelShader_); !status)
    {
        return status;
    }
    return gpu::CompileShader(compiler, options, L"CandidatePS", L"ps_6_0", candidatePixelShader_);
}

lgp::framework::Status Renderer::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1U;
    srvRange.BaseShaderRegister = 0U;

    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0U;
    parameters[0].Constants.Num32BitValues = sizeof(gpu::RenderConstants) / sizeof(std::uint32_t);
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].Descriptor.ShaderRegister = 1U;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[2].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
    description.pParameters = parameters;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};
    HRESULT const serialize =
        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, serialized.ReleaseAndGetAddressOf(),
                                    errors.ReleaseAndGetAddressOf());
    if (FAILED(serialize))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serialize, BlobText(errors.Get())));
    }
    HRESULT const create =
        deviceResources_->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(rootSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(create))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device::CreateRootSignature", create, "Failed to create the Chapter 19 solution root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreatePipelines()
{
    if (auto status =
            CreateGraphicsPipeline(*deviceResources_->device(), *rootSignature_.Get(), vertexShader_,
                                   baselinePixelShader_, deviceResources_->back_buffer_format(), baselinePipeline_);
        !status)
    {
        return status;
    }
    return CreateGraphicsPipeline(*deviceResources_->device(), *rootSignature_.Get(), vertexShader_,
                                  candidatePixelShader_, deviceResources_->back_buffer_format(), candidatePipeline_);
}

lgp::framework::Status Renderer::CreateResources()
{
    UINT const frameSlotCount = deviceResources_->back_buffer_count();
    auto heap = gpu::CreateTimestampHeap(*deviceResources_->device(), frameSlotCount * gpu::kTimestampsPerFrame,
                                         L"Ch19 Solution Timestamps");
    if (!heap)
    {
        return std::unexpected(std::move(heap.error()));
    }
    timestampHeap_ = std::move(*heap);

    auto waves = gpu::CreateBuffer(*deviceResources_->device(), gpu::kMaximumWaveCount * sizeof(gpu::WaveParam),
                                   D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch19 Solution Waves", true);
    if (!waves)
    {
        return std::unexpected(std::move(waves.error()));
    }
    wavesBuffer_ = std::move(*waves);

    auto descriptor = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kWavesSrvDescriptorCount);
    if (!descriptor)
    {
        return std::unexpected(
            lgp::framework::MakeError("CreateResources", "Failed to allocate the Chapter 19 waves descriptor."));
    }
    wavesDescriptor_ = *descriptor;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = gpu::kMaximumWaveCount;
    srv.Buffer.StructureByteStride = sizeof(gpu::WaveParam);
    deviceResources_->device()->CreateShaderResourceView(wavesBuffer_.Get(), &srv, wavesDescriptor_.CpuHandle(0U));

    frameSlots_.resize(frameSlotCount);
    for (FrameSlot &slot : frameSlots_)
    {
        auto readback = gpu::CreateBuffer(*deviceResources_->device(), gpu::kTimestampsPerFrame * sizeof(std::uint64_t),
                                          D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                          L"Ch19 Solution Timestamp Readback", true);
        if (!readback)
        {
            return std::unexpected(std::move(readback.error()));
        }
        slot.timestampReadback = std::move(*readback);
    }
    return {};
}

lgp::framework::Status Renderer::UploadWaves()
{
    std::vector<gpu::WaveParam> waves = gpu::BuildWaveParams(ActiveConfiguration().waveCount);
    waves.resize(gpu::kMaximumWaveCount, gpu::WaveParam{});
    return gpu::WriteBuffer(wavesBuffer_, std::span<gpu::WaveParam const>{waves});
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreateRootSignature(); !status)
    {
        return status;
    }
    if (auto status = CreatePipelines(); !status)
    {
        return status;
    }
    return CreateResources();
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &deviceResources,
                                          lgp::framework::Extent2D drawableSize)
{
    (void)deviceResources;
    (void)drawableSize;
    return {};
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &context)
{
    if (!headless_)
    {
        if (context.input.WasKeyPressed('1'))
        {
            interactiveVariant_ = gpu::LabVariant::Baseline;
        }
        if (context.input.WasKeyPressed('2'))
        {
            interactiveVariant_ = gpu::LabVariant::Candidate;
        }
    }
    if (waveUploadPending_)
    {
        if (auto status = UploadWaves(); !status)
        {
            return status;
        }
        waveUploadPending_ = false;
    }
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    // Capture the CPU command-recording start here, at the top of Render (BeginFrame has already returned), so
    // the measured duration reflects command recording only. It intentionally excludes the framework's
    // BeginFrame fence wait and the later EndFrame execute/signal cost, which this renderer cannot time from
    // inside Render.
    auto const recordingStart = std::chrono::steady_clock::now();
    gpu::LabConfiguration const configuration = ActiveConfiguration();
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);

    // Calibration is sampled once per frame, close to submission, so the raw ticks resolved this frame align to
    // a nearby GPU/QPC anchor. One calibration per captured frame is an alignment aid for teaching, not a claim
    // of nanosecond truth.
    lastCalibration_ = gpu::CaptureQueueCalibration(*deviceResources_->graphics_queue());

    UINT const baseQuery = frameContext.frameSlot * gpu::kTimestampsPerFrame;
    bool const useCandidate = configuration.variant == gpu::LabVariant::Candidate;
    ID3D12PipelineState *const pipeline = useCandidate ? candidatePipeline_.Get() : baselinePipeline_.Get();
    gpu::RenderConstants const constants{gpu::kRenderWidth, gpu::kRenderHeight, configuration.waveCount,
                                         configuration.iterations};

    {
        lgp::framework::PixEventScope const frameScope{commandList, lgp::framework::PixColor(64U, 128U, 200U),
                                                       L"Ch19 Profiling Frame"};
        commandList.EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, baseQuery + gpu::kQueryFrameBegin);

        std::vector<D3D12_TEXTURE_BARRIER> textureBarriers{gpu::MakeTextureBarrier(
            *frameContext.renderTarget, gpu::FrameStartState(frameContext), gpu::RenderTargetState())};
        gpu::SubmitTextureBarriers(commandList, textureBarriers);

        float const clear[]{0.0F, 0.0F, 0.0F, 1.0F};
        commandList.ClearRenderTargetView(frameContext.renderTargetView, clear, 0U, nullptr);
        commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
        commandList.RSSetViewports(1U, &frameContext.viewport);
        commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
        commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList.SetGraphicsRootSignature(rootSignature_.Get());
        commandList.SetGraphicsRoot32BitConstants(0U, sizeof(constants) / sizeof(std::uint32_t), &constants, 0U);
        commandList.SetGraphicsRootConstantBufferView(1U, wavesBuffer_.Get()->GetGPUVirtualAddress());
        commandList.SetGraphicsRootDescriptorTable(2U, wavesDescriptor_.GpuHandle(0U));
        commandList.SetPipelineState(pipeline);

        {
            lgp::framework::PixEventScope const controlScope{commandList, lgp::framework::PixColor(120U, 120U, 120U),
                                                             L"Controlled Region"};
            {
                lgp::framework::PixEventScope const workloadScope{
                    commandList, lgp::framework::PixColor(200U, 160U, 40U),
                    useCandidate ? L"Candidate Workload" : L"Baseline Workload"};
                commandList.EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                     baseQuery + gpu::kQueryWorkloadBegin);
                commandList.DrawInstanced(3U, 1U, 0U, 0U);
                commandList.EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                     baseQuery + gpu::kQueryWorkloadEnd);
            }
        }

        commandList.EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, baseQuery + gpu::kQueryFrameEnd);

        {
            lgp::framework::PixEventScope const resolveScope{commandList, lgp::framework::PixColor(40U, 200U, 120U),
                                                             L"Timestamp Resolve"};
            commandList.ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, baseQuery,
                                         gpu::kTimestampsPerFrame,
                                         frameSlots_.at(frameContext.frameSlot).timestampReadback.Get(), 0U);
        }

        std::vector<D3D12_TEXTURE_BARRIER> endBarriers{gpu::MakeTextureBarrier(
            *frameContext.renderTarget, gpu::RenderTargetState(), gpu::FrameEndState(frameContext))};
        gpu::SubmitTextureBarriers(commandList, endBarriers);
    }
    auto const recordingEnd = std::chrono::steady_clock::now();

    pendingFrame_ = {};
    pendingFrame_.calibration = lastCalibration_;
    pendingFrame_.cpuRecordingMilliseconds = MillisecondsBetween(recordingStart, recordingEnd);
    pendingFrame_.frameSlot = frameContext.frameSlot;
    pendingFrame_.valid = true;
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    if (wavesDescriptor_)
    {
        deviceResources.shader_visible_cbv_srv_uav_heap().Free(wavesDescriptor_);
        wavesDescriptor_ = {};
    }
    frameSlots_.clear();
    wavesBuffer_ = gpu::BufferResource{};
    timestampHeap_.Reset();
    candidatePipeline_.Reset();
    baselinePipeline_.Reset();
    rootSignature_.Reset();
    deviceResources_ = nullptr;
}

void Renderer::ConfigureExperiment(HeadlessTestConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
    waveUploadPending_ = true;
}

std::expected<MeasurementSample, lgp::framework::Error> Renderer::ReadPendingSample()
{
    if (!pendingFrame_.valid)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadPendingSample", "No Chapter 19 measured frame is pending readback."));
    }
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }
    // Consume the pending evidence exactly once: copy its metadata locally and invalidate the pending flag
    // before interpreting the readback, so a second read without an intervening frame fails closed above rather
    // than returning a stale sample.
    PendingFrame const pending = pendingFrame_;
    pendingFrame_.valid = false;

    if (!pending.calibration.valid)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ReadPendingSample",
            "GPU timestamp calibration is unsupported on this device; refusing to fabricate timing samples."));
    }

    FrameSlot const &slot = frameSlots_.at(pending.frameSlot);
    auto const *timestamps = reinterpret_cast<std::uint64_t const *>(slot.timestampReadback.mapped_data());
    if (timestamps == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadPendingSample", "Chapter 19 timestamp readback is not mapped."));
    }
    std::uint64_t const rawWorkloadBegin = timestamps[gpu::kQueryWorkloadBegin];
    std::uint64_t const rawWorkloadEnd = timestamps[gpu::kQueryWorkloadEnd];
    return gpu::BuildMeasurementSample(rawWorkloadBegin, rawWorkloadEnd, pending.calibration,
                                       pending.cpuRecordingMilliseconds);
}

} // namespace ch19::gpu_profiling::solution
