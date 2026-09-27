#include "Renderer.hpp"

#include <lgp/framework/error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace ch36::fluid::solution
{
namespace
{
using lgp::framework::MakeError;
using lgp::framework::Status;
constexpr std::uint64_t kScalarBytes = 67U * 67U * sizeof(float);
constexpr std::uint64_t kOutputBytes = 67U * 67U * sizeof(float) * 2U;
struct Parameters final
{
    std::uint32_t width{}, height{};
    float cellSize{}, timeStep{};
    std::uint32_t boundary{}, corrected{}, screenWidth{}, screenHeight{};
    float accelerationX{}, accelerationY{};
};
static_assert(sizeof(Parameters) == 40U);

void Upload(gpu::Buffer &buffer, Field const &field)
{
    auto *destination = reinterpret_cast<float *>(buffer.Data());
    std::transform(field.values.begin(), field.values.end(), destination,
                   [](double value) { return static_cast<float>(value); });
}

} // namespace

auto Renderer::Active() const -> gpu::LabConfiguration
{
    return headless_.value_or(gpu::DefaultConfiguration());
}

auto Renderer::CreatePipelines() -> Status
{
    auto compiler = lgp::framework::ShaderCompiler::Create();
    if (!compiler)
    {
        return std::unexpected(std::move(compiler.error()));
    }
    auto const path = std::filesystem::path{__FILE__}.parent_path() / "FluidLab.hlsl";
    std::array<lgp::framework::CompiledShader, 9U> shaders{};
    constexpr wchar_t const *names[]{L"ForwardCS",    L"ReverseCS",       L"CorrectCS",    L"GhostCS", L"TransportUCS",
                                     L"TransportVCS", L"VelocityGhostCS", L"FullscreenVS", L"TracerPS"};
    for (std::size_t i = 0U; i < shaders.size(); ++i)
    {
        auto const *target = L"ps_6_0";
        if (i < 7U)
        {
            target = L"cs_6_0";
        }
        else if (i == 7U)
        {
            target = L"vs_6_0";
        }
        auto compiled = gpu::Compile(*compiler, path, names[i], target);
        if (!compiled)
        {
            return std::unexpected(std::move(compiled.error()));
        }
        shaders[i] = std::move(*compiled);
    }
    auto &device = *resources_->device();
    std::array<D3D12_ROOT_PARAMETER, 13U> params{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0U;
    params[0].Constants.Num32BitValues = 10U;
    for (UINT i = 0U; i < 8U; ++i)
    {
        params[i + 1U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[i + 1U].Descriptor.ShaderRegister = i;
    }
    for (UINT i = 0U; i < 4U; ++i)
    {
        params[i + 9U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[i + 9U].Descriptor.ShaderRegister = i;
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(params.size());
    desc.pParameters = params.data();
    auto root = gpu::MakeRoot(device, desc);
    if (!root)
    {
        return std::unexpected(std::move(root.error()));
    }
    computeRoot_ = std::move(*root);
    auto build = [&](std::size_t i, Microsoft::WRL::ComPtr<ID3D12PipelineState> &pipeline) -> Status
    {
        auto created = gpu::MakeCompute(device, *computeRoot_.Get(), shaders[i]);
        if (!created)
        {
            return std::unexpected(std::move(created.error()));
        }
        pipeline = std::move(*created);
        return {};
    };
    if (auto s = build(0U, forward_); !s)
    {
        return s;
    }
    if (auto s = build(1U, reverse_); !s)
    {
        return s;
    }
    if (auto s = build(2U, correct_); !s)
    {
        return s;
    }
    if (auto s = build(3U, ghosts_); !s)
    {
        return s;
    }
    if (auto s = build(4U, transportU_); !s)
    {
        return s;
    }
    if (auto s = build(5U, transportV_); !s)
    {
        return s;
    }
    if (auto s = build(6U, velocityGhosts_); !s)
    {
        return s;
    }
    std::array<D3D12_ROOT_PARAMETER, 2U> draw{};
    draw[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    draw[0].Constants.ShaderRegister = 0U;
    draw[0].Constants.Num32BitValues = 8U;
    draw[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    draw[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    draw[1].Descriptor.ShaderRegister = 5U;
    draw[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    desc.NumParameters = static_cast<UINT>(draw.size());
    desc.pParameters = draw.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    root = gpu::MakeRoot(device, desc);
    if (!root)
    {
        return std::unexpected(std::move(root.error()));
    }
    graphicsRoot_ = std::move(*root);
    auto pipeline =
        gpu::MakeGraphics(device, *graphicsRoot_.Get(), shaders[7], shaders[8], resources_->back_buffer_format());
    if (!pipeline)
    {
        return std::unexpected(std::move(pipeline.error()));
    }
    graphics_ = std::move(*pipeline);
    return {};
}

auto Renderer::CreateResources() -> Status
{
    slots_.resize(resources_->back_buffer_count());
    for (auto &slot : slots_)
    {
        auto create = [&](gpu::Buffer &buffer, std::uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                          bool mapped) -> Status
        {
            auto made = gpu::MakeBuffer(*resources_->device(), bytes, heap, flags, mapped);
            if (!made)
            {
                return std::unexpected(std::move(made.error()));
            }
            buffer = std::move(*made);
            return {};
        };
        for (auto *buffer : {&slot.scalar, &slot.u, &slot.v})
        {
            if (auto s = create(*buffer, kScalarBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, true); !s)
            {
                return s;
            }
        }
        for (auto *buffer : {&slot.prediction, &slot.reverse})
        {
            if (auto s = create(*buffer, kScalarBytes, D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, false);
                !s)
            {
                return s;
            }
        }
        for (auto *buffer : {&slot.corrected, &slot.final})
        {
            if (auto s = create(*buffer, kOutputBytes, D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, false);
                !s)
            {
                return s;
            }
        }
        if (auto s = create(slot.readback, kOutputBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, true); !s)
        {
            return s;
        }
        for (auto *buffer : {&slot.uIntermediate, &slot.vIntermediate, &slot.uFinal, &slot.vFinal})
        {
            if (auto s = create(*buffer, kScalarBytes, D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, false);
                !s)
            {
                return s;
            }
        }
        for (auto *buffer : {&slot.uReadback, &slot.vReadback})
        {
            if (auto s = create(*buffer, kScalarBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, true); !s)
            {
                return s;
            }
        }
    }
    return {};
}

auto Renderer::Initialize(lgp::framework::ApplicationInitContext const &context) -> Status
{
    resources_ = &context.deviceResources;
    if (auto s = CreatePipelines(); !s)
    {
        return s;
    }
    return CreateResources();
}
auto Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D) -> Status
{
    return {};
}
auto Renderer::Update(lgp::framework::UpdateContext const &) -> Status
{
    return gpu::ValidateConfiguration(Active());
}

auto Renderer::Render(lgp::framework::FrameContext const &frame) -> Status
{
    if (frame.frameSlot >= slots_.size())
    {
        return std::unexpected(MakeError("Ch36 Render", "Frame slot out of range."));
    }
    auto const config = Active();
    if (auto s = gpu::ValidateConfiguration(config); !s)
    {
        return s;
    }
    auto &slot = slots_[frame.frameSlot];
    Upload(slot.scalar, config.scalar);
    Upload(slot.u, config.velocityX);
    Upload(slot.v, config.velocityY);
    auto &list = *frame.commandList;
    auto transition = [&](gpu::Buffer &buffer, D3D12_BARRIER_SYNC beforeSync, D3D12_BARRIER_ACCESS before,
                          D3D12_BARRIER_SYNC afterSync, D3D12_BARRIER_ACCESS after)
    { gpu::Barrier(list, *buffer.Get(), beforeSync, before, afterSync, after); };
    constexpr auto compute = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    constexpr auto uav = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    constexpr auto srv = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    constexpr auto none = D3D12_BARRIER_ACCESS_NO_ACCESS;
    constexpr auto noSync = D3D12_BARRIER_SYNC_NONE;
    transition(slot.prediction, slot.used ? compute : noSync, slot.used ? srv : none, compute, uav);
    transition(slot.reverse, slot.used ? compute : noSync, slot.used ? srv : none, compute, uav);
    transition(slot.corrected, slot.used ? compute : noSync, slot.used ? srv : none, compute, uav);
    transition(slot.final, slot.used ? D3D12_BARRIER_SYNC_COPY : noSync,
               slot.used ? D3D12_BARRIER_ACCESS_COPY_SOURCE : none, compute, uav);
    for (auto *buffer : {&slot.uIntermediate, &slot.vIntermediate})
    {
        transition(*buffer, slot.used ? compute : noSync, slot.used ? srv : none, compute, uav);
    }
    for (auto *buffer : {&slot.uFinal, &slot.vFinal})
    {
        transition(*buffer, slot.used ? D3D12_BARRIER_SYNC_COPY : noSync,
                   slot.used ? D3D12_BARRIER_ACCESS_COPY_SOURCE : none, compute, uav);
    }
    slot.used = true;
    Parameters const parameters{config.grid.width,
                                config.grid.height,
                                static_cast<float>(config.grid.cellSize),
                                static_cast<float>(config.timeStep),
                                static_cast<std::uint32_t>(config.boundary),
                                static_cast<std::uint32_t>(config.method == AdvectionMethod::Corrected),
                                frame.viewport.Width > 0 ? static_cast<std::uint32_t>(frame.viewport.Width) : 1U,
                                frame.viewport.Height > 0 ? static_cast<std::uint32_t>(frame.viewport.Height) : 1U,
                                static_cast<float>(config.accelerationX),
                                static_cast<float>(config.accelerationY)};
    list.SetComputeRootSignature(computeRoot_.Get());
    list.SetComputeRoot32BitConstants(0U, 10U, &parameters, 0U);
    auto bind = [&](UINT index, gpu::Buffer &buffer)
    { list.SetComputeRootShaderResourceView(index + 1U, buffer.Get()->GetGPUVirtualAddress()); };
    bind(0U, slot.scalar);
    bind(1U, slot.u);
    bind(2U, slot.v);
    bind(3U, slot.prediction);
    bind(4U, slot.reverse);
    bind(5U, slot.corrected);
    bind(6U, slot.uIntermediate);
    bind(7U, slot.vIntermediate);
    auto const groupsX = (config.grid.width + 7U) / 8U;
    auto const groupsY = (config.grid.height + 7U) / 8U;
    list.SetComputeRootUnorderedAccessView(9U, slot.prediction.Get()->GetGPUVirtualAddress());
    list.SetComputeRootUnorderedAccessView(10U, slot.corrected.Get()->GetGPUVirtualAddress());
    list.SetComputeRootUnorderedAccessView(11U, slot.uIntermediate.Get()->GetGPUVirtualAddress());
    list.SetComputeRootUnorderedAccessView(12U, slot.vIntermediate.Get()->GetGPUVirtualAddress());
    list.SetPipelineState(forward_.Get());
    list.Dispatch(groupsX, groupsY, 1U);
    transition(slot.prediction, compute, uav, compute, srv);
    list.SetComputeRootUnorderedAccessView(9U, slot.reverse.Get()->GetGPUVirtualAddress());
    list.SetPipelineState(reverse_.Get());
    list.Dispatch(groupsX, groupsY, 1U);
    transition(slot.reverse, compute, uav, compute, srv);
    list.SetPipelineState(correct_.Get());
    list.Dispatch(groupsX, groupsY, 1U);
    transition(slot.corrected, compute, uav, compute, srv);
    list.SetComputeRootUnorderedAccessView(10U, slot.final.Get()->GetGPUVirtualAddress());
    list.SetPipelineState(ghosts_.Get());
    list.Dispatch((config.grid.width + 9U) / 8U, (config.grid.height + 9U) / 8U, 1U);
    list.SetPipelineState(transportU_.Get());
    list.Dispatch((config.grid.width + 8U) / 8U, groupsY, 1U);
    list.SetPipelineState(transportV_.Get());
    list.Dispatch(groupsX, (config.grid.height + 8U) / 8U, 1U);
    transition(slot.uIntermediate, compute, uav, compute, srv);
    transition(slot.vIntermediate, compute, uav, compute, srv);
    list.SetComputeRootUnorderedAccessView(11U, slot.uFinal.Get()->GetGPUVirtualAddress());
    list.SetComputeRootUnorderedAccessView(12U, slot.vFinal.Get()->GetGPUVirtualAddress());
    list.SetPipelineState(velocityGhosts_.Get());
    list.Dispatch((config.grid.width + 10U) / 8U, (config.grid.height + 10U) / 8U, 1U);
    transition(slot.final, compute, uav, D3D12_BARRIER_SYNC_PIXEL_SHADING, srv);
    gpu::BeginDraw(frame);
    list.SetGraphicsRootSignature(graphicsRoot_.Get());
    list.SetGraphicsRoot32BitConstants(0U, 8U, &parameters, 0U);
    list.SetGraphicsRootShaderResourceView(1U, slot.final.Get()->GetGPUVirtualAddress());
    list.SetPipelineState(graphics_.Get());
    list.DrawInstanced(3U, 1U, 0U, 0U);
    gpu::EndDraw(frame);
    transition(slot.final, D3D12_BARRIER_SYNC_PIXEL_SHADING, srv, D3D12_BARRIER_SYNC_COPY,
               D3D12_BARRIER_ACCESS_COPY_SOURCE);
    list.CopyBufferRegion(slot.readback.Get(), 0U, slot.final.Get(), 0U, kOutputBytes);
    for (auto [output, readback] : {std::pair{&slot.uFinal, &slot.uReadback}, std::pair{&slot.vFinal, &slot.vReadback}})
    {
        transition(*output, compute, uav, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE);
        list.CopyBufferRegion(readback->Get(), 0U, output->Get(), 0U, kScalarBytes);
    }
    lastSlot_ = frame.frameSlot;
    rendered_ = true;
    return {};
}

auto Renderer::ReadBackOutputs() -> std::expected<gpu::FrameReadback, lgp::framework::Error>
{
    if (!rendered_ || !resources_)
    {
        return std::unexpected(MakeError("Ch36 readback", "No rendered frame."));
    }
    auto idle = resources_->WaitForGpuIdle();
    if (!idle)
    {
        return std::unexpected(std::move(idle.error()));
    }
    auto const config = Active();
    auto result = *MakeField(config.grid, Layout::Cell);
    std::uint32_t clamped = 0U;
    auto const *data = slots_[lastSlot_].readback.Data();
    for (std::uint32_t y = 0U; y < config.grid.height + 2U; ++y)
    {
        for (std::uint32_t x = 0U; x < config.grid.width + 2U; ++x)
        {
            auto const index = y * (config.grid.width + 2U) + x;
            std::array<float, 2U> pair{};
            std::memcpy(pair.data(), data + index * sizeof(pair), sizeof(pair));
            if (!std::isfinite(pair[0]) || !std::isfinite(pair[1]))
            {
                return std::unexpected(MakeError("Ch36 readback", "GPU produced nonfinite output."));
            }
            result.values[index] = pair[0];
            if (x > 0U && x <= config.grid.width && y > 0U && y <= config.grid.height)
            {
                clamped += static_cast<std::uint32_t>(pair[1] != 0.0F);
            }
        }
    }
    auto diagnostics = gpu::Diagnostics(config.grid, config.scalar, result, clamped, true);
    auto readVelocity = [&](Layout layout, gpu::Buffer const &buffer) -> std::expected<Field, lgp::framework::Error>
    {
        auto field = *MakeField(config.grid, layout);
        for (std::size_t i = 0U; i < field.values.size(); ++i)
        {
            float value{};
            std::memcpy(&value, buffer.Data() + i * sizeof(float), sizeof(float));
            if (!std::isfinite(value))
            {
                return std::unexpected(MakeError("Ch36 readback", "GPU produced nonfinite face velocity."));
            }
            field.values[i] = value;
        }
        return field;
    };
    auto u = readVelocity(Layout::XFace, slots_[lastSlot_].uReadback);
    if (!u)
    {
        return std::unexpected(std::move(u.error()));
    }
    auto v = readVelocity(Layout::YFace, slots_[lastSlot_].vReadback);
    if (!v)
    {
        return std::unexpected(std::move(v.error()));
    }
    diagnostics.velocity = {std::move(*u), std::move(*v)};
    if (!std::isfinite(diagnostics.advection.sourceMass) || !std::isfinite(diagnostics.advection.sourceEnergy) ||
        !std::isfinite(diagnostics.advection.outputMass) || !std::isfinite(diagnostics.advection.outputEnergy))
    {
        return std::unexpected(MakeError("Ch36 readback", "Nonfinite field diagnostics."));
    }
    return diagnostics;
}
void Renderer::ConfigureHeadlessTest(gpu::LabConfiguration const &configuration)
{
    headless_ = configuration;
}
void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    slots_.clear();
    forward_.Reset();
    reverse_.Reset();
    correct_.Reset();
    ghosts_.Reset();
    graphics_.Reset();
    transportU_.Reset();
    transportV_.Reset();
    velocityGhosts_.Reset();
    computeRoot_.Reset();
    graphicsRoot_.Reset();
    resources_ = nullptr;
    rendered_ = false;
}
} // namespace ch36::fluid::solution
