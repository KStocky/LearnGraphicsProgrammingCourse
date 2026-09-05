#include "Renderer.hpp"

#include <lgp/framework/pix.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>

namespace ch19::gpu_profiling::starter
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr std::uint32_t kStarterTimestampsPerFrame = 2U;
inline constexpr std::uint32_t kStarterWorkloadBegin = 0U;
inline constexpr std::uint32_t kStarterWorkloadEnd = 1U;

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

} // namespace

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    gpu::LabConfiguration configuration =
        headless_ && headlessConfiguration_.has_value() ? *headlessConfiguration_ : gpu::LabConfiguration{};
    configuration.variant = gpu::LabVariant::Baseline;
    configuration.waveCount = gpu::NormalizeWaveCount(configuration.waveCount);
    configuration.iterations = std::max(configuration.iterations, 1U);
    return configuration;
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
    return gpu::CompileShader(compiler, options, L"ProfilingPS", L"ps_6_0", pixelShader_);
}

lgp::framework::Status Renderer::CreateRootSignature()
{
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0U;
    parameters[0].Constants.Num32BitValues = sizeof(gpu::RenderConstants) / sizeof(std::uint32_t);
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].Descriptor.ShaderRegister = 1U;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
            "ID3D12Device::CreateRootSignature", create, "Failed to create the Chapter 19 starter root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreatePipeline()
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
    description.pRootSignature = rootSignature_.Get();
    description.VS = vertexShader_.Bytecode();
    description.PS = pixelShader_.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = deviceResources_->back_buffer_format();
    description.SampleDesc.Count = 1U;
    HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(pipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create the Chapter 19 starter pipeline."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateResources()
{
    UINT const frameSlotCount = deviceResources_->back_buffer_count();
    auto heap = gpu::CreateTimestampHeap(*deviceResources_->device(), frameSlotCount * kStarterTimestampsPerFrame,
                                         L"Ch19 Starter Timestamps");
    if (!heap)
    {
        return std::unexpected(std::move(heap.error()));
    }
    timestampHeap_ = std::move(*heap);

    auto waves = gpu::CreateBuffer(*deviceResources_->device(), gpu::kMaximumWaveCount * sizeof(gpu::WaveParam),
                                   D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch19 Starter Waves", true);
    if (!waves)
    {
        return std::unexpected(std::move(waves.error()));
    }
    wavesBuffer_ = std::move(*waves);

    frameSlots_.resize(frameSlotCount);
    for (FrameSlot &slot : frameSlots_)
    {
        auto readback = gpu::CreateBuffer(*deviceResources_->device(),
                                          kStarterTimestampsPerFrame * sizeof(std::uint64_t), D3D12_HEAP_TYPE_READBACK,
                                          D3D12_RESOURCE_FLAG_NONE, L"Ch19 Starter Timestamp Readback", true);
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
    if (auto status = CreatePipeline(); !status)
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
    (void)context;
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
    gpu::LabConfiguration const configuration = ActiveConfiguration();
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    UINT const baseQuery = frameContext.frameSlot * kStarterTimestampsPerFrame;

    gpu::RenderConstants const constants{gpu::kRenderWidth, gpu::kRenderHeight, configuration.waveCount,
                                         configuration.iterations};

    auto const cpuBegin = std::chrono::steady_clock::now();
    {
        // Coarse frame-scope PIX marker. Nothing that can fail early runs inside the scope, so the begin/end
        // pairing is guaranteed even though this starter does not use RAII around the inner workload marker
        // beyond the framework's scoped helper.
        lgp::framework::PixEventScope const frameScope{commandList, lgp::framework::PixColor(64U, 128U, 200U),
                                                       L"Ch19 Profiling Frame"};

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
        commandList.SetPipelineState(pipeline_.Get());

        {
            lgp::framework::PixEventScope const workloadScope{commandList, lgp::framework::PixColor(200U, 160U, 40U),
                                                              L"Baseline Workload"};
            commandList.EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, baseQuery + kStarterWorkloadBegin);
            commandList.DrawInstanced(3U, 1U, 0U, 0U);
            commandList.EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, baseQuery + kStarterWorkloadEnd);
        }

        commandList.ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                     baseQuery + kStarterWorkloadBegin, kStarterTimestampsPerFrame,
                                     frameSlots_.at(frameContext.frameSlot).timestampReadback.Get(), 0U);

        std::vector<D3D12_TEXTURE_BARRIER> endBarriers{gpu::MakeTextureBarrier(
            *frameContext.renderTarget, gpu::RenderTargetState(), gpu::FrameEndState(frameContext))};
        gpu::SubmitTextureBarriers(commandList, endBarriers);
    }
    auto const cpuEnd = std::chrono::steady_clock::now();

    std::uint64_t frequency{};
    bool const frequencySupported =
        SUCCEEDED(deviceResources_->graphics_queue()->GetTimestampFrequency(&frequency)) && frequency != 0U;

    pendingEvidence_ = {};
    pendingEvidence_.cpuRecordingMilliseconds = MillisecondsBetween(cpuBegin, cpuEnd);
    pendingEvidence_.gpuTimestampFrequencyHz = frequency;
    pendingEvidence_.timestampsSupported = frequencySupported;
    lastFrameSlot_ = frameContext.frameSlot;
    pendingEvidenceValid_ = true;
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    (void)deviceResources;
    frameSlots_.clear();
    wavesBuffer_ = gpu::BufferResource{};
    timestampHeap_.Reset();
    pipeline_.Reset();
    rootSignature_.Reset();
    deviceResources_ = nullptr;
}

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
    waveUploadPending_ = true;
}

std::expected<CoarseEvidence, lgp::framework::Error> Renderer::ReadCoarseEvidence()
{
    if (!pendingEvidenceValid_)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadCoarseEvidence", "No Chapter 19 starter frame is pending readback."));
    }
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }
    // Consume exactly once: copy the pending metadata and invalidate the pending flag before interpreting the
    // readback, so a second read without an intervening frame fails closed above.
    CoarseEvidence evidence = pendingEvidence_;
    UINT const frameSlot = lastFrameSlot_;
    pendingEvidenceValid_ = false;

    FrameSlot const &slot = frameSlots_.at(frameSlot);
    auto const *timestamps = reinterpret_cast<std::uint64_t const *>(slot.timestampReadback.mapped_data());
    if (timestamps == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadCoarseEvidence", "Chapter 19 starter timestamp readback is not mapped."));
    }
    evidence.rawWorkloadBeginTick = timestamps[kStarterWorkloadBegin];
    evidence.rawWorkloadEndTick = timestamps[kStarterWorkloadEnd];
    return evidence;
}

} // namespace ch19::gpu_profiling::starter
