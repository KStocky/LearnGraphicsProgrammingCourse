#include "Renderer.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <lgp/framework/pix.hpp>
#include <limits>
#include <utility>

namespace ch43::transfers::LGP_TRANSFER_VARIANT
{
using namespace lgp::framework;
namespace
{
template <typename T> T Load(std::byte const *bytes, std::uint32_t offset)
{
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(T));
    return value;
}
template <typename T> void Store(std::byte *bytes, std::uint32_t offset, T const &value)
{
    std::memcpy(bytes + offset, &value, sizeof(T));
}
} // namespace
std::expected<void, Error> Renderer::Configure(Scene scene, Configuration configuration)
{
    auto valid = Validate(scene, configuration);
    if (!valid)
    {
        return valid;
    }
    scene_ = std::move(scene);
    configuration_ = configuration;
    fault_.reset();
    ResetExperiment();
    return {};
}
Status Renderer::Initialize(ApplicationInitContext const &context)
{
    resources_ = &context.deviceResources;
    windowHandle_ = context.windowHandle;
    std::array<D3D12_ROOT_PARAMETER1, 3U> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0U, 0U, 8U};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor = {0U, 0U, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].Descriptor = {0U, 0U, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC description{};
    description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    description.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    description.Desc_1_1.pParameters = parameters.data();
    Microsoft::WRL::ComPtr<ID3DBlob> blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> errors{};
    HRESULT hr = D3D12SerializeVersionedRootSignature(&description, &blob, &errors);
    if (FAILED(hr))
    {
        return std::unexpected(MakeHResultError("Transfer root serialization", hr));
    }
    hr = resources_->device()->CreateRootSignature(0U, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&root_));
    if (FAILED(hr))
    {
        return std::unexpected(MakeHResultError("Transfer root", hr));
    }
    auto compiler = ShaderCompiler::Create();
    if (!compiler)
    {
        return std::unexpected(compiler.error());
    }
    std::array<wchar_t const *, static_cast<std::size_t>(Pass::Count)> const entries{
        L"Preflight", L"Snapshot", L"Clear", L"Scatter", L"Normalize", L"Transfer", L"Measure"};
    ShaderCompileOptions options{};
    options.sourcePath = std::filesystem::path(__FILE__).parent_path() / L"TransferLab.hlsl";
    options.targetProfile = L"cs_6_0";
    for (std::size_t i = 0U; i < entries.size(); ++i)
    {
        options.entryPoint = entries[i];
        options.additionalArguments = {L"-T", options.targetProfile, L"-E", options.entryPoint, L"-WX"};
        auto shader = compiler->Compile(options);
        if (!shader)
        {
            return std::unexpected(shader.error());
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root_.Get();
        pipeline.CS = shader->Bytecode();
        hr = resources_->device()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&compute_[i]));
        if (FAILED(hr))
        {
            return std::unexpected(MakeHResultError("Transfer compute PSO", hr));
        }
    }
    options.targetProfile = L"vs_6_0";
    options.entryPoint = L"Fullscreen";
    options.additionalArguments = {L"-T", options.targetProfile, L"-E", options.entryPoint, L"-WX"};
    auto vertex = compiler->Compile(options);
    options.targetProfile = L"ps_6_0";
    options.entryPoint = L"Diagnostic";
    options.additionalArguments = {L"-T", options.targetProfile, L"-E", options.entryPoint, L"-WX"};
    auto pixel = compiler->Compile(options);
    if (!vertex || !pixel)
    {
        return std::unexpected(!vertex ? vertex.error() : pixel.error());
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root_.Get();
    pipeline.VS = vertex->Bytecode();
    pipeline.PS = pixel->Bytecode();
    auto &blend = pipeline.BlendState.RenderTarget[0];
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1U;
    pipeline.RTVFormats[0] = resources_->back_buffer_format();
    pipeline.SampleDesc.Count = 1U;
    hr = resources_->device()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&graphics_));
    if (FAILED(hr))
    {
        return std::unexpected(MakeHResultError("Transfer diagnostic PSO", hr));
    }
    auto arena = CreateDefaultBuffer(*resources_->device(), kBytes, D3D12_RESOURCE_STATE_COMMON,
                                     L"Transfer ordered persistent arena", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!arena)
    {
        return std::unexpected(arena.error());
    }
    arena_ = std::move(*arena);
    for (std::uint32_t i = 0U; i < resources_->back_buffer_count(); ++i)
    {
        Slot slot{};
        auto seed = CreateUploadBuffer(*resources_->device(), kBytes, L"Transfer fenced reset seed");
        BufferCreateDesc desc{};
        desc.sizeInBytes = kBytes;
        desc.heapType = D3D12_HEAP_TYPE_READBACK;
        desc.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
        desc.name = L"Transfer fenced readback";
        auto readback = CreateCommittedBuffer(*resources_->device(), desc);
        if (!seed || !readback)
        {
            return std::unexpected(!seed ? seed.error() : readback.error());
        }
        slot.seed = std::move(*seed);
        slot.readback = std::move(*readback);
        slots_.push_back(std::move(slot));
    }
    return {};
}
Status Renderer::OnResize(DeviceResources &resources, Extent2D size)
{
    (void)resources;
    (void)size;
    return {};
}
Status Renderer::Update(UpdateContext const &context)
{
    if (!context.commandLine.headless)
    {
        auto const &input = context.input;
        if (input.WasKeyPressed(VK_SPACE))
        {
            Pause(!configuration_.paused);
        }
        if (input.WasKeyPressed('R'))
        {
            fault_.reset();
            ResetExperiment();
        }
        if (input.WasKeyPressed('V'))
        {
            configuration_.view = (configuration_.view + 1U) % 4U;
        }
        bool reset{};
        if (input.WasKeyPressed('A'))
        {
            constexpr std::array<float, 3U> alphas{0.95F, 0.0F, 1.0F};
            auto const current = std::ranges::find(alphas, configuration_.alpha);
            auto const index = static_cast<std::size_t>(current - alphas.begin());
            configuration_.alpha = alphas[(index + 1U) % alphas.size()];
            reset = true;
        }
        if (input.WasKeyPressed('U'))
        {
            configuration_.increment = configuration_.increment.x == 0.0F ? Vec3{0.25F, -0.125F, 0.0625F} : Vec3{};
            reset = true;
        }
        if (input.WasKeyPressed('F'))
        {
            configuration_.field = (configuration_.field + 1U) % 3U;
            reset = true;
        }
        if (reset)
        {
            fault_.reset();
            ResetExperiment();
        }
    }
    auto const settings =
        std::format("Ch43 transfers stage={} alpha={} increment=({},{},{})m/s "
                    "field={} fixed_positions pause={} view={}",
                    static_cast<std::uint32_t>(kStage), configuration_.alpha, configuration_.increment.x,
                    configuration_.increment.y, configuration_.increment.z, configuration_.field, configuration_.paused,
                    configuration_.view);
    if (settings != settingsText_)
    {
        std::puts(settings.c_str());
        if (windowHandle_ != nullptr)
        {
            auto title = Utf8ToWide(settings);
            if (!title)
            {
                return std::unexpected(title.error());
            }
            if (!SetWindowTextW(windowHandle_, title->c_str()))
            {
                return std::unexpected(MakeLastError("Transfer title"));
            }
        }
        settingsText_ = settings;
    }
    if (context.commandLine.headless && context.frameIndex == 1U && lastSlot_ && !diagnosticReported_)
    {
        auto result = ReadBackOutputs();
        if (!result)
        {
            return std::unexpected(result.error());
        }
        auto const &m = result->state.metrics;
        std::printf("Transfers first-submitted round=%u slot=%u epoch=%llu stage=%u "
                    "family_mass=(%.9g,%.9g,%.9g)kg family_momentum=(%.9g,%.9g,%.9g)kg*m/s "
                    "particle_momentum=(%.9g,%.9g,%.9g)kg*m/s kinetic=%.9gJ input_kinetic=%.9gJ "
                    "occupied_faces=%u stencil_entries=%u support_min=%.9g CAS_attempts=%u guards=%s error=%s\n",
                    m.round, result->frameSlot, static_cast<unsigned long long>(result->epoch),
                    static_cast<std::uint32_t>(kStage), static_cast<double>(m.familyMass.x),
                    static_cast<double>(m.familyMass.y), static_cast<double>(m.familyMass.z),
                    static_cast<double>(m.familyMomentum.x), static_cast<double>(m.familyMomentum.y),
                    static_cast<double>(m.familyMomentum.z), static_cast<double>(m.particleMomentum.x),
                    static_cast<double>(m.particleMomentum.y), static_cast<double>(m.particleMomentum.z),
                    static_cast<double>(m.kinetic), static_cast<double>(m.inputKinetic), m.occupied, m.entries,
                    static_cast<double>(m.supportMin), m.attempts, result->guardsIntact ? "intact" : "FAILED",
                    result->error ? ErrorName(*result->error).data() : "none");
        diagnosticReported_ = true;
        if (result->error)
        {
            return std::unexpected(MakeError("Transfer GPU", std::string(ErrorName(*result->error))));
        }
    }
    return {};
}
void Renderer::Barrier(ID3D12GraphicsCommandList7 &list, D3D12_BARRIER_SYNC beforeSync,
                       D3D12_BARRIER_ACCESS beforeAccess, D3D12_BARRIER_SYNC afterSync,
                       D3D12_BARRIER_ACCESS afterAccess) const noexcept
{
    D3D12_BUFFER_BARRIER barrier{};
    barrier.SyncBefore = beforeSync;
    barrier.SyncAfter = afterSync;
    barrier.AccessBefore = beforeAccess;
    barrier.AccessAfter = afterAccess;
    barrier.pResource = arena_.resource();
    barrier.Size = UINT64_MAX;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_BUFFER;
    group.NumBarriers = 1U;
    group.pBufferBarriers = &barrier;
    list.Barrier(1U, &group);
}
void Renderer::Dispatch(ID3D12GraphicsCommandList7 &list, Constants const &constants, Pass pass)
{
    constexpr std::array<wchar_t const *, static_cast<std::size_t>(Pass::Count)> labels{
        L"Transfer validate configuration",
        L"Transfer particle snapshot validation",
        L"Transfer clear face validity",
        L"Transfer concurrent mass momentum CAS scatter",
        L"Transfer normalize snapshot manufactured update",
        L"Transfer PIC FLIP gather blend",
        L"Transfer measure completed round"};
    PixEventScope scope(list, PixColor(40U, 130U, 220U), labels[static_cast<std::size_t>(pass)]);
    list.SetComputeRoot32BitConstants(0U, 8U, &constants, 0U);
    list.SetPipelineState(compute_[static_cast<std::size_t>(pass)].Get());
    list.Dispatch(pass == Pass::Clear || pass == Pass::Normalize ? 4U : 1U, 1U, 1U);
    Barrier(list, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
            D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
}
Status Renderer::Upload(Slot &slot)
{
    auto *bytes = slot.seed.mapped_data();
    for (std::uint32_t offset = 0U; offset < kBytes; offset += 4U)
    {
        Store(bytes, offset, kGuard);
    }
    for (std::uint32_t i = 0U; i < scene_.size(); ++i)
    {
        auto p = scene_[i];
        if (i == 0U && fault_)
        {
            switch (*fault_)
            {
            case Error::NonFinite:
                p.position.x = std::numeric_limits<float>::quiet_NaN();
                break;
            case Error::Domain:
                p.position.x = 1.0F;
                break;
            case Error::Mass:
                p.mass = 0.0F;
                break;
            case Error::Identity:
                p.identity = 63U;
                break;
            default:
                break;
            }
        }
        Store(bytes, kP + i * 32U, p);
        Store(bytes, kSnapshot + i * 32U, p);
        Store(bytes, kSamples + i * 32U, Sample{});
    }
    for (std::uint32_t f = 0U; f < kFaces; ++f)
    {
        Store(bytes, kFaceArena + f * 32U, Face{});
    }
    Store(bytes, kMetrics, Metrics{});
    Store(bytes, kStatus, std::array<std::uint32_t, 4U>{});
    slot.epoch = epoch_;
    return {};
}
Status Renderer::Render(FrameContext const &frame)
{
    auto valid = Validate(scene_, configuration_);
    if (!valid)
    {
        return std::unexpected(MakeError("Transfer configuration", std::string(ErrorName(valid.error()))));
    }
    auto &slot = slots_[frame.frameSlot];
    // BeginFrame waits for THIS slot before CPU seed writes or mapped readback access.
    if (slot.used && slot.epoch == epoch_ && !resetPending_)
    {
        auto old = Decode(slot, frame.frameSlot);
        if (!old)
        {
            return std::unexpected(old.error());
        }
        if (old->error)
        {
            return std::unexpected(MakeError("Transfer fenced sticky GPU error", std::string(ErrorName(*old->error))));
        }
    }
    auto &list = *frame.commandList;
    if (resetPending_)
    {
        auto uploaded = Upload(slot);
        if (!uploaded)
        {
            return uploaded;
        }
        Barrier(list, arenaUsed_ ? D3D12_BARRIER_SYNC_COMPUTE_SHADING : D3D12_BARRIER_SYNC_NONE,
                arenaUsed_ ? D3D12_BARRIER_ACCESS_UNORDERED_ACCESS : D3D12_BARRIER_ACCESS_NO_ACCESS,
                D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST);
        list.CopyBufferRegion(arena_.resource(), 0U, slot.seed.resource(), 0U, kBytes);
        Barrier(list, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        resetPending_ = false;
    }
    else
    {
        // One persistent arena: all slots submit to the same ordered direct queue.
        Barrier(list, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    }
    Constants c{fault_ == Error::Capacity ? 65U : static_cast<std::uint32_t>(scene_.size()),
                static_cast<std::uint32_t>(kStage),
                configuration_.field,
                configuration_.view,
                fault_ == Error::Parameters ? -1.0F : configuration_.alpha,
                configuration_.increment};
    list.SetComputeRootSignature(root_.Get());
    list.SetComputeRootUnorderedAccessView(1U, arena_.gpu_virtual_address());
    if (!configuration_.paused)
    {
        for (std::size_t pass = 0U; pass < static_cast<std::size_t>(Pass::Count); ++pass)
        {
            Dispatch(list, c, static_cast<Pass>(pass));
        }
    }
    Barrier(list, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
            D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    TransitionTexture(
        list, *frame.renderTarget,
        {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frame.renderTargetInitialLayout},
        {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET});
    list.OMSetRenderTargets(1U, &frame.renderTargetView, FALSE, nullptr);
    list.RSSetViewports(1U, &frame.viewport);
    list.RSSetScissorRects(1U, &frame.scissorRect);
    list.SetGraphicsRootSignature(root_.Get());
    list.SetGraphicsRoot32BitConstants(0U, 8U, &c, 0U);
    list.SetGraphicsRootShaderResourceView(2U, arena_.gpu_virtual_address());
    list.SetPipelineState(graphics_.Get());
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    {
        PixEventScope scope(list, PixColor(90U, 210U, 140U), L"Transfer actual grid particle diagnostic draw");
        list.DrawInstanced(3U, 1U, 0U, 0U);
    }
    TransitionTexture(
        list, *frame.renderTarget,
        {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET},
        {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
         frame.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT});
    Barrier(list, D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_SYNC_COPY,
            D3D12_BARRIER_ACCESS_COPY_SOURCE);
    list.CopyBufferRegion(slot.readback.resource(), 0U, arena_.resource(), 0U, kBytes);
    Barrier(list, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    slot.constants = c;
    slot.epoch = epoch_;
    slot.used = true;
    arenaUsed_ = true;
    lastSlot_ = frame.frameSlot;
    return {};
}
Result<Readback> Renderer::Decode(Slot const &slot, std::uint32_t index) const
{
    D3D12_RANGE const range{0U, kBytes};
    void *mapped{};
    HRESULT const hr = slot.readback.resource()->Map(0U, &range, &mapped);
    if (FAILED(hr))
    {
        return std::unexpected(MakeHResultError("Transfer Map", hr));
    }
    auto const *bytes = static_cast<std::byte const *>(mapped);
    Readback result{};
    result.frameSlot = index;
    result.epoch = slot.epoch;
    result.constants = slot.constants;
    std::uint32_t const n = std::min(slot.constants.count, kCapacity);
    auto const error = Load<std::uint32_t>(bytes, kStatus);
    if (error != 0U)
    {
        result.error = static_cast<Error>(error);
    }
    result.state.rounds = Load<std::uint32_t>(bytes, kStatus + 4U);
    result.state.metrics = Load<Metrics>(bytes, kMetrics);
    for (std::uint32_t i = 0U; i < n; ++i)
    {
        result.state.particles.push_back(Load<Particle>(bytes, kP + i * 32U));
        result.state.snapshot.push_back(Load<Particle>(bytes, kSnapshot + i * 32U));
        result.state.samples.push_back(Load<Sample>(bytes, kSamples + i * 32U));
    }
    for (std::uint32_t f = 0U; f < kFaces; ++f)
    {
        result.state.faces[f] = Load<Face>(bytes, kFaceArena + f * 32U);
    }
    result.guardsIntact = true;
    std::array<std::array<std::uint32_t, 3U>, 6U> const regions{
        {{kP, static_cast<std::uint32_t>(scene_.size()) * 32U, kSnapshot},
         {kSnapshot, static_cast<std::uint32_t>(scene_.size()) * 32U, kFaceArena},
         {kFaceArena, kFaces * 32U, kSamples},
         {kSamples, static_cast<std::uint32_t>(scene_.size()) * 32U, kMetrics},
         {kMetrics, 64U, kStatus},
         {kStatus, 16U, kBytes}}};
    for (auto const &region : regions)
    {
        for (std::uint32_t offset = region[0] + region[1]; offset < region[2]; offset += 4U)
        {
            result.guardsIntact &= Load<std::uint32_t>(bytes, offset) == kGuard;
        }
    }
    if (!result.guardsIntact)
    {
        result.error = Error::Guard;
    }
    D3D12_RANGE const written{0U, 0U};
    slot.readback.resource()->Unmap(0U, &written);
    return result;
}
Result<Readback> Renderer::ReadBackOutputs()
{
    if (!lastSlot_)
    {
        return std::unexpected(MakeError("Transfer readback", "No submitted frame"));
    }
    auto idle = resources_->WaitForGpuIdle();
    if (!idle)
    {
        return std::unexpected(idle.error());
    }
    return Decode(slots_[*lastSlot_], *lastSlot_);
}
void Renderer::Shutdown(DeviceResources &resources) noexcept
{
    (void)resources.WaitForGpuIdle();
    slots_.clear();
    arena_ = {};
    compute_ = {};
    graphics_.Reset();
    root_.Reset();
    resources_ = nullptr;
    lastSlot_.reset();
    windowHandle_ = nullptr;
    arenaUsed_ = false;
    resetPending_ = true;
}
} // namespace ch43::transfers::LGP_TRANSFER_VARIANT
