#include "Renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <utility>

namespace ch40::pbd::LGP_PBD_VARIANT
{
using namespace lgp::framework;

std::expected<void, Error> Renderer::Configure(Scene scene, Configuration configuration, bool project)
{
    auto valid = ValidateConfiguration(configuration, scene);
    if (!valid)
    {
        return valid;
    }
    if (project && kStage == gpu::Stage::FreeMotion)
    {
        return std::unexpected(Error::InvalidConfiguration);
    }
    if (project && kStage < gpu::Stage::Jacobi && configuration.strategy != Strategy::Colored)
    {
        return std::unexpected(Error::InvalidConfiguration);
    }
    if (project && kStage == gpu::Stage::Jacobi && configuration.strategy == Strategy::Atomic)
    {
        return std::unexpected(Error::InvalidConfiguration);
    }
    scene_ = std::move(scene);
    configuration_ = configuration;
    projectionEnabled_ = project;
    return {};
}

Status Renderer::Initialize(ApplicationInitContext const &context)
{
    resources_ = &context.deviceResources;
    windowHandle_ = context.windowHandle;
    settingsText_.clear();
    diagnosticReported_ = false;
    auto scene = MakeDefaultScene();
    if (!scene)
    {
        return std::unexpected(MakeError("PBD default scene", std::string(ErrorName(scene.error()))));
    }
    scene_ = std::move(*scene);
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
        L"Predict",    L"Colored",       L"Corrections", L"Gather",  L"ResetIntegers",
        L"Accumulate", L"ApplyIntegers", L"Velocity",    L"Measure", L"Guards"};
    ShaderCompileOptions options{};
    options.sourcePath = std::filesystem::path(__FILE__).parent_path() / L"PbdLab.hlsl";
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
    for (std::uint32_t index = 0U; index < resources_->back_buffer_count(); ++index)
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
    constexpr std::array<std::string_view, 6U> stages{"FreeMotion", "Colored", "Compliance", "Jacobi", "Atomic", "All"};
    constexpr std::array<std::string_view, 3U> strategies{"Colored", "Jacobi", "Atomic"};
    constexpr std::array<std::uint32_t, 5U> iterations{1U, 4U, 16U, 32U, 64U};
    constexpr std::array<float, 3U> dt{1.0F / 240.0F, 1.0F / 60.0F, 1.0F / 15.0F};
    constexpr std::array<float, 3U> compliance{0.0F, 1.0e-4F, 1.0e-2F};
    constexpr std::array<float, 3U> masses{1.0F, 10.0F, 100.0F};
    if (!context.commandLine.headless)
    {
        auto const &input = context.input;
        if (input.WasKeyPressed('S'))
        {
            constexpr std::uint32_t strategyCount = gpu::StrategyCount(kStage);
            configuration_.strategy =
                static_cast<Strategy>((static_cast<std::uint32_t>(configuration_.strategy) + 1U) % strategyCount);
        }
        if (input.WasKeyPressed('I'))
        {
            iterationChoice_ = (iterationChoice_ + 1U) % 5U;
        }
        if (input.WasKeyPressed('T'))
        {
            dtChoice_ = (dtChoice_ + 1U) % 3U;
        }
        if (input.WasKeyPressed('C'))
        {
            complianceChoice_ = (complianceChoice_ + 1U) % 3U;
        }
        if (input.WasKeyPressed('M'))
        {
            massChoice_ = (massChoice_ + 1U) % 3U;
        }
        if (input.WasKeyPressed('P'))
        {
            pinned_ = !pinned_;
        }
        if (input.WasKeyPressed('V'))
        {
            view_ = (view_ + 1U) % 3U;
        }
        configuration_.iterations = iterations[iterationChoice_];
        configuration_.timeStep = dt[dtChoice_];
        auto scene = MakeDefaultScene(pinned_, masses[massChoice_], compliance[complianceChoice_]);
        if (!scene)
        {
            return std::unexpected(MakeError("PBD controls", std::string(ErrorName(scene.error()))));
        }
        auto valid = Configure(std::move(*scene), configuration_, projectionEnabled_);
        if (!valid)
        {
            return std::unexpected(MakeError("PBD controls", std::string(ErrorName(valid.error()))));
        }
    }
    std::string const settings =
        std::format("Ch40 single step | stage={} | projection={} | solver={} | iterations={} | dt={:.9g}s | "
                    "c_selected={:.9g} c_active={} | mass_ratio={:g} | root={} | V={}",
                    stages[static_cast<std::size_t>(kStage)], projectionEnabled_ ? "on" : "off (prediction)",
                    strategies[static_cast<std::size_t>(configuration_.strategy)], configuration_.iterations,
                    configuration_.timeStep, compliance[complianceChoice_],
                    kStage >= gpu::Stage::Compliance && projectionEnabled_ ? "selected" : "off", masses[massChoice_],
                    pinned_ ? "pinned" : "unpinned", view_);
    if (settings != settingsText_)
    {
        if (windowHandle_ != nullptr)
        {
            auto title = Utf8ToWide(settings);
            if (!title)
            {
                return std::unexpected(title.error());
            }
            if (!SetWindowTextW(windowHandle_, title->c_str()))
            {
                return std::unexpected(MakeLastError("PBD settings title"));
            }
        }
        std::puts(settings.c_str());
        settingsText_ = settings;
    }
    // Update precedes BeginFrame; frame 0 has reached EndFrame before Update 1.
    // Never wait/map from Render, which only records an unsubmitted command list.
    if (context.commandLine.headless && context.frameIndex == 1U && lastSlot_ && !diagnosticReported_)
    {
        std::puts("GPU diagnostic: deliberate idle wait for submitted frame 0, once; not a performance measurement.");
        auto output = ReadBackOutputs();
        if (!output)
        {
            return std::unexpected(output.error());
        }
        auto const &final = output->step.final;
        std::printf("GPU final (submitted frame 0): slot=%u executed_iterations=%u distance_m=%.9g attachment_m=%.9g "
                    "compliant_m=%.9g reconstructed_speed_m_per_s=%.9g kinetic_energy=%.9g guards=%s error=%s\n",
                    output->frameSlot, output->executedIterations, static_cast<double>(final.maximumDistanceError),
                    static_cast<double>(final.maximumAttachmentError),
                    static_cast<double>(final.maximumCompliantResidual), static_cast<double>(final.maximumSpeed),
                    static_cast<double>(final.kineticEnergy), output->guardsIntact ? "intact" : "FAILED",
                    output->error ? ErrorName(*output->error).data() : "none");
        if (output->error || !output->guardsIntact)
        {
            return std::unexpected(MakeError(
                "PBD GPU diagnostic", output->error ? std::string(ErrorName(*output->error)) : "Guard corruption"));
        }
        diagnosticReported_ = true;
    }
    return {};
}

void Renderer::Dispatch(ID3D12GraphicsCommandList7 &list, gpu::Slot &slot, gpu::Constants const &constants, Pass pass)
{
    list.SetComputeRoot32BitConstants(0U, 16U, &constants, 0U);
    list.SetPipelineState(compute_[static_cast<std::size_t>(pass)].Get());
    list.Dispatch(1U, 1U, 1U);
    // Dispatches consume the previous dispatch's writes, including in-place colors,
    // lambda updates, gather inputs, resets and metrics. This is ordering, not a no-op.
    gpu::BufferBarrier(list, *slot.arena.resource(), D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                       D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                       D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
}

Status Renderer::Render(FrameContext const &frame)
{
    auto valid = ValidateConfiguration(configuration_, scene_);
    if (!valid)
    {
        return std::unexpected(MakeError("PBD configuration", std::string(ErrorName(valid.error()))));
    }
    // BeginFrame has waited for this slot's fence. No mapped uploads are changed before it.
    auto &slot = slots_[frame.frameSlot];
    auto uploaded = gpu::UploadScene(slot, scene_);
    if (!uploaded)
    {
        return uploaded;
    }
    auto &list = *frame.commandList;
    gpu::BufferBarrier(list, *slot.arena.resource(),
                       slot.used ? D3D12_BARRIER_SYNC_COMPUTE_SHADING : D3D12_BARRIER_SYNC_NONE,
                       slot.used ? D3D12_BARRIER_ACCESS_UNORDERED_ACCESS : D3D12_BARRIER_ACCESS_NO_ACCESS,
                       D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST);
    list.CopyBufferRegion(slot.arena.resource(), 0U, slot.seed.resource(), 0U, gpu::kArenaBytes);
    gpu::BufferBarrier(list, *slot.arena.resource(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST,
                       D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    gpu::Constants constants{};
    constants.particles = static_cast<std::uint32_t>(scene_.particles.size());
    constants.constraints = static_cast<std::uint32_t>(scene_.constraints.size());
    constants.dt = configuration_.timeStep;
    constants.relaxation = configuration_.relaxation;
    constants.gravity = configuration_.gravity;
    constants.strategy = static_cast<std::uint32_t>(configuration_.strategy);
    constants.iterations = configuration_.iterations;
    constants.project = projectionEnabled_ ? 1U : 0U;
    constants.view = view_;
    constants.complianceEnabled = kStage >= gpu::Stage::Compliance ? 1U : 0U;
    list.SetComputeRootSignature(root_.Get());
    list.SetComputeRootShaderResourceView(1U, slot.input.gpu_virtual_address());
    list.SetComputeRootUnorderedAccessView(2U, slot.arena.gpu_virtual_address());
    Dispatch(list, slot, constants, Pass::Predict);
    Dispatch(list, slot, constants, Pass::Measure);
    std::uint32_t maximumColor{};
    for (auto const &constraint : scene_.constraints)
    {
        maximumColor = std::max(maximumColor, constraint.color);
    }
    for (std::uint32_t iteration = 0U; constants.project != 0U && iteration < constants.iterations; ++iteration)
    {
        if (configuration_.strategy == Strategy::Colored)
        {
            for (std::uint32_t color = 0U; color <= maximumColor; ++color)
            {
                constants.color = color;
                Dispatch(list, slot, constants, Pass::Colored);
            }
        }
        else
        {
            if (configuration_.strategy == Strategy::Jacobi)
            {
                Dispatch(list, slot, constants, Pass::Corrections);
                Dispatch(list, slot, constants, Pass::Gather);
            }
            else
            {
                Dispatch(list, slot, constants, Pass::ResetIntegers);
                Dispatch(list, slot, constants, Pass::Accumulate);
                Dispatch(list, slot, constants, Pass::ApplyIntegers);
            }
            std::swap(constants.inputOffset, constants.outputOffset);
        }
        constants.metricIndex = iteration + 1U;
        Dispatch(list, slot, constants, Pass::Measure);
    }
    Dispatch(list, slot, constants, Pass::Velocity);
    constants.metricIndex = 65U;
    Dispatch(list, slot, constants, Pass::Measure);
    Dispatch(list, slot, constants, Pass::Guards);
    gpu::BufferBarrier(list, *slot.arena.resource(), D3D12_BARRIER_SYNC_COMPUTE_SHADING,
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
    list.SetGraphicsRootShaderResourceView(3U, slot.arena.gpu_virtual_address());
    list.SetPipelineState(graphics_.Get());
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list.DrawInstanced(3U, 1U, 0U, 0U);
    TransitionTexture(
        list, *frame.renderTarget,
        {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET},
        {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
         frame.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT});
    gpu::BufferBarrier(list, *slot.arena.resource(), D3D12_BARRIER_SYNC_PIXEL_SHADING,
                       D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE);
    list.CopyBufferRegion(slot.readback.resource(), 0U, slot.arena.resource(), 0U, gpu::kArenaBytes);
    gpu::BufferBarrier(list, *slot.arena.resource(), D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                       D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    slot.used = true;
    slot.constants = constants;
    lastSlot_ = frame.frameSlot;
    return {};
}

Result<gpu::FrameReadback> Renderer::ReadBackOutputs()
{
    if (!lastSlot_)
    {
        return std::unexpected(MakeError("PBD readback", "No frame has been submitted"));
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
    compute_ = {};
    graphics_.Reset();
    root_.Reset();
    resources_ = nullptr;
    windowHandle_ = nullptr;
    settingsText_.clear();
    diagnosticReported_ = false;
    lastSlot_.reset();
}

} // namespace ch40::pbd::LGP_PBD_VARIANT
