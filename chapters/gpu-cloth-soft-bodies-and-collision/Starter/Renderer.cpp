#include "Renderer.hpp"
#include <algorithm>
#include <cstdio>
#include <format>
#include <utility>

namespace ch41::cloth::LGP_CLOTH_VARIANT
{
using namespace lgp::framework;
std::expected<void, Error> Renderer::Configure(Scene scene, Configuration configuration)
{
    auto valid = Validate(scene, configuration);
    if (!valid)
    {
        return valid;
    }
    scene_ = std::move(scene);
    configuration_ = configuration;
    ResetSimulation();
    return {};
}
Status Renderer::Initialize(ApplicationInitContext const &context)
{
    resources_ = &context.deviceResources;
    windowHandle_ = context.windowHandle;
    scene_ = MakeCloth();
    auto root = gpu::CreateRootSignature(*resources_->device());
    if (!root)
    {
        return std::unexpected(root.error());
    }
    root_ = std::move(*root);
    auto compiler = ShaderCompiler::Create();
    if (!compiler)
    {
        return std::unexpected(compiler.error());
    }
    std::array<std::wstring, static_cast<std::size_t>(Pass::Count)> const entries{
        L"BeginTick",   L"ResetLambda", L"Predict",     L"Tear",     L"Colored", L"ValidateVolumes",
        L"Environment", L"BinSort",     L"SelfContact", L"Velocity", L"Measure"};
    ShaderCompileOptions options{};
    options.sourcePath = std::filesystem::path(__FILE__).parent_path() / L"ClothLab.hlsl";
    options.targetProfile = L"cs_6_0";
    for (std::size_t index = 0U; index < entries.size(); ++index)
    {
        options.entryPoint = entries[index];
        options.additionalArguments = {L"-T", options.targetProfile, L"-E", options.entryPoint, L"-WX"};
        auto shader = compiler->Compile(options);
        if (!shader)
        {
            return std::unexpected(shader.error());
        }
        auto pipeline = gpu::CreateComputePipeline(*resources_->device(), *root_.Get(), *shader);
        if (!pipeline)
        {
            return std::unexpected(pipeline.error());
        }
        compute_[index] = std::move(*pipeline);
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
    auto pipeline = gpu::CreateGraphicsPipeline(*resources_->device(), *root_.Get(), *vertex, *pixel,
                                                resources_->back_buffer_format());
    if (!pipeline)
    {
        return std::unexpected(pipeline.error());
    }
    graphics_ = std::move(*pipeline);
    auto arena = CreateDefaultBuffer(*resources_->device(), gpu::kArenaBytes, D3D12_RESOURCE_STATE_COMMON,
                                     L"Cloth ordered persistent arena", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!arena)
    {
        return std::unexpected(arena.error());
    }
    arena_ = std::move(*arena);
    for (std::uint32_t i = 0U; i < resources_->back_buffer_count(); ++i)
    {
        auto slot = gpu::CreateSlot(*resources_->device());
        if (!slot)
        {
            return std::unexpected(slot.error());
        }
        slots_.push_back(std::move(*slot));
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
    constexpr std::array<std::uint32_t, 3U> iterations{1U, 8U, 24U};
    constexpr std::array<float, 3U> complianceMultipliers{0.0F, 1.0F, 100.0F};
    constexpr std::array<std::string_view, 6U> stages{"Prediction",  "Distance",    "Shape",
                                                      "Environment", "SelfContact", "All"};
    if (!context.commandLine.headless)
    {
        auto const &input = context.input;
        if (input.WasKeyPressed(VK_SPACE))
        {
            Pause(!configuration_.paused);
        }
        if (input.WasKeyPressed('R'))
        {
            ResetSimulation();
        }
        bool reconfigure = false;
        if (input.WasKeyPressed('M'))
        {
            softBody_ = !softBody_;
            reconfigure = true;
        }
        if (input.WasKeyPressed('I'))
        {
            iterationChoice_ = (iterationChoice_ + 1U) % 3U;
            reconfigure = true;
        }
        if (input.WasKeyPressed('C'))
        {
            complianceChoice_ = (complianceChoice_ + 1U) % 3U;
            reconfigure = true;
        }
        if (input.WasKeyPressed('F'))
        {
            configuration_.friction = configuration_.friction == 0.0F ? 0.4F : 0.0F;
            reconfigure = true;
        }
        if (input.WasKeyPressed('S'))
        {
            configuration_.selfContact = !configuration_.selfContact;
            reconfigure = true;
        }
        if (input.WasKeyPressed('T'))
        {
            configuration_.tearing = !configuration_.tearing;
            reconfigure = true;
        }
        if (reconfigure)
        {
            auto scene = softBody_ ? MakeSoftBody() : MakeCloth();
            configuration_.complianceScale = complianceMultipliers[complianceChoice_];
            configuration_.iterations = iterations[iterationChoice_];
            auto configured = Configure(std::move(scene), configuration_);
            if (!configured)
            {
                return std::unexpected(MakeError("Cloth controls", std::string(ErrorName(configured.error()))));
            }
        }
    }
    auto const settings = std::format(
        "Ch41 persistent fixed tick | stage={} model={} I={} dt={:.8g} substeps={} compliance_multiplier={:.5g} mu={} "
        "environment={} self={} tear={} paused={} | physical controls RESET",
        stages[static_cast<std::size_t>(kStage)], softBody_ ? "tet" : "cloth", configuration_.iterations,
        configuration_.tick, configuration_.substeps, configuration_.complianceScale, configuration_.friction,
        kStage >= Stage::Environment && configuration_.environment,
        kStage >= Stage::SelfContact && configuration_.selfContact, kStage == Stage::All && configuration_.tearing,
        configuration_.paused);
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
                return std::unexpected(MakeLastError("Cloth title"));
            }
        }
        settingsText_ = settings;
    }
    if (context.commandLine.headless && context.frameIndex == 1U && lastSlot_ && !diagnosticReported_)
    {
        auto output = ReadBackOutputs();
        if (!output)
        {
            return std::unexpected(output.error());
        }
        auto const &m = output->state.metrics;
        std::printf("GPU submitted tick=%u slot=%u distance_m=%.9g area_m2=%.9g volume_m3=%.9g "
                    "compliant_mixed_units=%.9g penetration_m=%.9g speed_m_s=%.9g kinetic_J=%.9g motion_m=%.9g "
                    "contacts=%u candidates=%u inactive=%u guards=%s error=%s\n",
                    output->state.ticks, output->frameSlot, static_cast<double>(m.distance),
                    static_cast<double>(m.area), static_cast<double>(m.volume), static_cast<double>(m.compliant),
                    static_cast<double>(m.penetration), static_cast<double>(m.speed), static_cast<double>(m.kinetic),
                    static_cast<double>(m.motion), m.contacts, m.candidates, m.broken,
                    output->guardsIntact ? "intact" : "FAILED",
                    output->error ? ErrorName(*output->error).data() : "none");
        if (output->error)
        {
            return std::unexpected(MakeError("Cloth GPU numerical status", std::string(ErrorName(*output->error))));
        }
        auto energy = MeasureMechanicalEnergy(output->state.particles, output->constants.gravity);
        auto reference = MeasureMechanicalEnergy(scene_.particles, configuration_.gravity);
        if (!energy || !reference)
        {
            auto const error = energy ? reference.error() : energy.error();
            return std::unexpected(MakeError("Cloth energy diagnostic", std::string(ErrorName(error))));
        }
        std::printf("Energy host reduction of GPU readback: kinetic_J=%.12g gravitational_potential_J=%.12g "
                    "K_plus_U_J=%.12g reference_rest_J=%.12g signed_change_J=%.12g; "
                    "pins omitted, elastic/model work excluded; NOT a conservation test\n",
                    energy->kinetic, energy->gravitationalPotential, energy->total, reference->total,
                    energy->total - reference->total);
        diagnosticReported_ = true;
    }
    return {};
}
void Renderer::Dispatch(ID3D12GraphicsCommandList7 &list, gpu::Constants const &constants, Pass pass)
{
    list.SetComputeRoot32BitConstants(0U, 16U, &constants, 0U);
    list.SetPipelineState(compute_[static_cast<std::size_t>(pass)].Get());
    list.Dispatch(1U, 1U, 1U);
    gpu::BufferBarrier(list, *arena_.resource(), D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                       D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                       D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
}
Status Renderer::Render(FrameContext const &frame)
{
    auto valid = Validate(scene_, configuration_);
    if (!valid)
    {
        return std::unexpected(MakeError("Cloth configuration", std::string(ErrorName(valid.error()))));
    }
    auto &slot = slots_[frame.frameSlot];
    // BeginFrame fenced this slot; only its uploads and readback are mutable CPU-owned resources.
    if (slot.used && slot.epoch == epoch_ && !resetPending_)
    {
        auto previous = gpu::DecodeReadback(slot, frame.frameSlot);
        if (!previous)
        {
            return std::unexpected(previous.error());
        }
        if (previous->error)
        {
            return std::unexpected(
                MakeError("Cloth fenced numerical status", std::string(ErrorName(*previous->error))));
        }
    }
    if (slot.epoch != epoch_ || resetPending_)
    {
        auto uploaded = gpu::UploadScene(slot, scene_, resetPending_);
        if (!uploaded)
        {
            return uploaded;
        }
        slot.epoch = epoch_;
    }
    auto &list = *frame.commandList;
    if (resetPending_)
    {
        gpu::BufferBarrier(list, *arena_.resource(),
                           arenaUsed_ ? D3D12_BARRIER_SYNC_COMPUTE_SHADING : D3D12_BARRIER_SYNC_NONE,
                           arenaUsed_ ? D3D12_BARRIER_ACCESS_UNORDERED_ACCESS : D3D12_BARRIER_ACCESS_NO_ACCESS,
                           D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST);
        list.CopyBufferRegion(arena_.resource(), 0U, slot.seed.resource(), 0U, gpu::kArenaBytes);
        gpu::BufferBarrier(list, *arena_.resource(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST,
                           D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        resetPending_ = false;
    }
    else
    {
        // Ordered same-queue arena dependency crosses submitted frames, independently of the frame-slot ring.
        gpu::BufferBarrier(list, *arena_.resource(), D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                           D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                           D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    }
    gpu::Constants constants{};
    constants.particles = static_cast<std::uint32_t>(scene_.particles.size());
    constants.constraints = static_cast<std::uint32_t>(scene_.constraints.size());
    constants.triangles = static_cast<std::uint32_t>(scene_.triangles.size());
    constants.tetrahedra = static_cast<std::uint32_t>(scene_.tetrahedra.size());
    constants.stage = static_cast<std::uint32_t>(kStage);
    constants.iterations = configuration_.iterations;
    constants.flags = (configuration_.environment ? 1U : 0U) | (configuration_.selfContact ? 2U : 0U) |
                      (configuration_.tearing ? 4U : 0U);
    constants.dt = configuration_.tick / static_cast<float>(configuration_.substeps);
    constants.friction = configuration_.friction;
    constants.complianceScale = configuration_.complianceScale;
    constants.motionLimit = configuration_.motionLimit;
    constants.gravity = configuration_.gravity;
    bool const validateVolumes = kStage >= Stage::Shape && constants.tetrahedra != 0U;
    list.SetComputeRootSignature(root_.Get());
    list.SetComputeRootShaderResourceView(1U, slot.input.gpu_virtual_address());
    list.SetComputeRootUnorderedAccessView(2U, arena_.gpu_virtual_address());
    if (!configuration_.paused)
    {
        Dispatch(list, constants, Pass::BeginTick);
        std::uint32_t colors{};
        for (auto const &c : scene_.constraints)
        {
            colors = std::max(colors, c.color + 1U);
        }
        for (std::uint32_t substep = 0U; substep < configuration_.substeps; ++substep)
        {
            Dispatch(list, constants, Pass::ResetLambda);
            Dispatch(list, constants, Pass::Predict);
            if (validateVolumes)
            {
                Dispatch(list, constants, Pass::ValidateVolumes);
            }
            if (kStage == Stage::All && configuration_.tearing)
            {
                Dispatch(list, constants, Pass::Tear);
            }
            for (std::uint32_t iteration = 0U; iteration < configuration_.iterations; ++iteration)
            {
                if (kStage >= Stage::Distance)
                {
                    for (std::uint32_t color = 0U; color < colors; ++color)
                    {
                        constants.color = color;
                        Dispatch(list, constants, Pass::Colored);
                        if (validateVolumes)
                        {
                            Dispatch(list, constants, Pass::ValidateVolumes);
                        }
                    }
                }
                if (kStage >= Stage::Environment && configuration_.environment)
                {
                    Dispatch(list, constants, Pass::Environment);
                    if (validateVolumes)
                    {
                        Dispatch(list, constants, Pass::ValidateVolumes);
                    }
                }
                if (kStage >= Stage::SelfContact && configuration_.selfContact)
                {
                    Dispatch(list, constants, Pass::BinSort);
                    Dispatch(list, constants, Pass::SelfContact);
                    if (validateVolumes)
                    {
                        Dispatch(list, constants, Pass::ValidateVolumes);
                    }
                }
            }
            Dispatch(list, constants, Pass::Velocity);
        }
        Dispatch(list, constants, Pass::Measure);
    }
    gpu::BufferBarrier(list, *arena_.resource(), D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                       D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                       D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    TransitionTexture(
        list, *frame.renderTarget,
        {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frame.renderTargetInitialLayout},
        {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET});
    list.OMSetRenderTargets(1U, &frame.renderTargetView, FALSE, nullptr);
    list.RSSetViewports(1U, &frame.viewport);
    list.RSSetScissorRects(1U, &frame.scissorRect);
    list.SetGraphicsRootSignature(root_.Get());
    list.SetGraphicsRoot32BitConstants(0U, 16U, &constants, 0U);
    list.SetGraphicsRootShaderResourceView(1U, slot.input.gpu_virtual_address());
    list.SetGraphicsRootShaderResourceView(3U, arena_.gpu_virtual_address());
    list.SetPipelineState(graphics_.Get());
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list.DrawInstanced(3U, 1U, 0U, 0U);
    TransitionTexture(
        list, *frame.renderTarget,
        {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET},
        {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
         frame.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT});
    gpu::BufferBarrier(list, *arena_.resource(), D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                       D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE);
    list.CopyBufferRegion(slot.readback.resource(), 0U, arena_.resource(), 0U, gpu::kArenaBytes);
    gpu::BufferBarrier(list, *arena_.resource(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                       D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    slot.used = true;
    arenaUsed_ = true;
    slot.constants = constants;
    lastSlot_ = frame.frameSlot;
    return {};
}
Result<gpu::FrameReadback> Renderer::ReadBackOutputs()
{
    if (!lastSlot_)
    {
        return std::unexpected(MakeError("Cloth readback", "No submitted frame"));
    }
    auto idle = resources_->WaitForGpuIdle();
    if (!idle)
    {
        return std::unexpected(idle.error());
    }
    return gpu::DecodeReadback(slots_[*lastSlot_], *lastSlot_);
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
    windowHandle_ = nullptr;
    lastSlot_.reset();
    arenaUsed_ = false;
    epoch_ = 1U;
    resetPending_ = true;
    diagnosticReported_ = false;
    settingsText_.clear();
}
} // namespace ch41::cloth::LGP_CLOTH_VARIANT
