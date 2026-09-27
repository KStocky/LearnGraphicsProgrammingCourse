#include "Renderer.hpp"

#include <lgp/framework/error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace ch37::projection::starter
{
namespace
{
using lgp::framework::MakeError;
using lgp::framework::Status;
constexpr std::uint64_t kCells = 66U * 66U * sizeof(float);
constexpr std::uint64_t kFaces = 67U * 66U * sizeof(float);
constexpr std::array<std::uint64_t, 3> kWorkSizes{kCells, kFaces, kFaces};
struct Parameters final
{
    std::uint32_t nx, ny;
    float h, dt;
    std::uint32_t boundary, screenWidth, screenHeight;
};
static_assert(sizeof(Parameters) == 28U);

void Upload(gpu::Buffer &buffer, Field const &field)
{
    auto *target = reinterpret_cast<float *>(buffer.Data());
    for (std::size_t i = 0; i < field.values.size(); ++i)
    {
        target[i] = static_cast<float>(field.values[i]);
    }
}

auto ReadField(gpu::Buffer const &buffer, Grid grid, Layout layout) -> std::expected<Field, lgp::framework::Error>
{
    auto field = *MakeField(grid, layout);
    for (std::size_t i = 0; i < field.values.size(); ++i)
    {
        float v{};
        std::memcpy(&v, buffer.Data() + i * sizeof(float), sizeof(float));
        if (!std::isfinite(v))
        {
            return std::unexpected(MakeError("Ch37 readback", "Nonfinite GPU field."));
        }
        field.values[i] = v;
    }
    return field;
}

[[nodiscard]] auto ImageRange(lgp::framework::DeviceResources &resources, std::uint32_t slot) -> std::optional<unsigned>
{
    auto image = resources.ReadBackRenderTarget(slot);
    if (!image)
    {
        return std::nullopt;
    }
    std::uint8_t minimum = 255U;
    std::uint8_t maximum = 0U;
    for (std::uint32_t y = 0; y < image->size.height; ++y)
    {
        auto const *row = reinterpret_cast<std::uint8_t const *>(image->pixels.data() + y * image->rowPitch);
        for (std::uint32_t x = 0; x < image->size.width; ++x)
        {
            minimum = std::min(minimum, row[x * 4U]);
            maximum = std::max(maximum, row[x * 4U]);
        }
    }
    return static_cast<unsigned>(maximum - minimum);
}
} // namespace

auto Renderer::Active() const -> gpu::Configuration
{
    return headless_.value_or(gpu::DefaultConfiguration());
}

auto Renderer::Initialize(lgp::framework::ApplicationInitContext const &context) -> Status
{
    resources_ = &context.deviceResources;
    headlessCli_ = context.commandLine.headless;
    auto compiler = lgp::framework::ShaderCompiler::Create();
    if (!compiler)
    {
        return std::unexpected(std::move(compiler.error()));
    }
    auto const path = std::filesystem::path{__FILE__}.parent_path() / "ProjectionParallel.hlsl";
    auto shader = gpu::Compile(*compiler, path, L"DivergenceCS", L"cs_6_0");
    if (!shader)
    {
        return std::unexpected(std::move(shader.error()));
    }
    std::array<D3D12_ROOT_PARAMETER, 6> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0U;
    parameters[0].Constants.Num32BitValues = 7U;
    for (UINT i = 0; i < 2U; ++i)
    {
        parameters[i + 1U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[i + 1U].Descriptor.ShaderRegister = i;
    }
    for (UINT i = 0; i < 3U; ++i)
    {
        parameters[i + 3U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[i + 3U].Descriptor.ShaderRegister = i;
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    auto root = gpu::MakeRoot(*resources_->device(), desc);
    if (!root)
    {
        return std::unexpected(std::move(root.error()));
    }
    root_ = std::move(*root);
    auto pipeline = gpu::MakeCompute(*resources_->device(), *root_.Get(), *shader);
    if (!pipeline)
    {
        return std::unexpected(std::move(pipeline.error()));
    }
    pipeline_ = std::move(*pipeline);
    std::array<D3D12_ROOT_PARAMETER, 2> draw{};
    draw[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    draw[0].Constants.ShaderRegister = 0U;
    draw[0].Constants.Num32BitValues = 7U;
    draw[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    draw[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    draw[1].Descriptor.ShaderRegister = 2U;
    draw[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    desc.NumParameters = static_cast<UINT>(draw.size());
    desc.pParameters = draw.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    auto graphicRoot = gpu::MakeRoot(*resources_->device(), desc);
    if (!graphicRoot)
    {
        return std::unexpected(std::move(graphicRoot.error()));
    }
    graphicsRoot_ = std::move(*graphicRoot);
    auto vertex = gpu::Compile(*compiler, path, L"FullscreenVS", L"vs_6_0");
    if (!vertex)
    {
        return std::unexpected(std::move(vertex.error()));
    }
    auto pixel = gpu::Compile(*compiler, path, L"HeatPS", L"ps_6_0");
    if (!pixel)
    {
        return std::unexpected(std::move(pixel.error()));
    }
    auto graphicPipeline = gpu::MakeGraphics(*resources_->device(), *graphicsRoot_.Get(), *vertex, *pixel,
                                             resources_->back_buffer_format());
    if (!graphicPipeline)
    {
        return std::unexpected(std::move(graphicPipeline.error()));
    }
    graphics_ = std::move(*graphicPipeline);
    slots_.resize(resources_->back_buffer_count());
    for (auto &slot : slots_)
    {
        auto create = [&](gpu::Buffer &buffer, std::uint64_t bytes, D3D12_HEAP_TYPE heap, bool mapped) -> Status
        {
            auto made = gpu::MakeBuffer(*resources_->device(), bytes, heap,
                                        heap == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                                        : D3D12_RESOURCE_FLAG_NONE,
                                        mapped);
            if (!made)
            {
                return std::unexpected(std::move(made.error()));
            }
            buffer = std::move(*made);
            return {};
        };
        for (auto &input : slot.inputs)
        {
            if (auto s = create(input, kFaces, D3D12_HEAP_TYPE_UPLOAD, true); !s)
            {
                return s;
            }
        }
        for (std::size_t i = 0; i < slot.work.size(); ++i)
        {
            if (auto s = create(slot.work[i], kWorkSizes[i], D3D12_HEAP_TYPE_DEFAULT, false); !s)
            {
                return s;
            }
        }
        for (std::size_t i = 0; i < slot.readback.size(); ++i)
        {
            if (auto s = create(slot.readback[i], kWorkSizes[i], D3D12_HEAP_TYPE_READBACK, true); !s)
            {
                return s;
            }
        }
    }
    return {};
}

auto Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D) -> Status
{
    return {};
}
auto Renderer::Update(lgp::framework::UpdateContext const &) -> Status
{
    return gpu::Validate(Active());
}
void Renderer::ConfigureHeadlessTest(gpu::Configuration const &config)
{
    headless_ = config;
}

auto Renderer::Render(lgp::framework::FrameContext const &frame) -> Status
{
    if (frame.frameSlot >= slots_.size())
    {
        return std::unexpected(MakeError("Ch37 render", "Invalid frame slot."));
    }
    auto config = Active();
    if (auto status = gpu::Validate(config); !status)
    {
        return status;
    }
    auto &slot = slots_[frame.frameSlot];
    Upload(slot.inputs[0], config.velocity.x);
    Upload(slot.inputs[1], config.velocity.y);
    auto &list = *frame.commandList;
    constexpr auto compute = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    constexpr auto uav = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    for (std::size_t i = 0; i < slot.work.size(); ++i)
    {
        bool copied = true;
        auto beforeSync = D3D12_BARRIER_SYNC_NONE;
        auto beforeAccess = D3D12_BARRIER_ACCESS_NO_ACCESS;
        if (slot.used)
        {
            beforeSync = copied ? D3D12_BARRIER_SYNC_COPY : compute;
            beforeAccess = copied ? D3D12_BARRIER_ACCESS_COPY_SOURCE : uav;
        }
        gpu::Barrier(list, *slot.work[i].Get(), beforeSync, beforeAccess, compute, uav);
    }
    slot.used = true;
    Parameters params{config.grid.nx,
                      config.grid.ny,
                      static_cast<float>(config.grid.h),
                      static_cast<float>(config.dt),
                      static_cast<std::uint32_t>(config.boundary),
                      static_cast<std::uint32_t>(frame.viewport.Width),
                      static_cast<std::uint32_t>(frame.viewport.Height)};
    list.SetComputeRootSignature(root_.Get());
    list.SetComputeRoot32BitConstants(0U, 7U, &params, 0U);
    for (UINT i = 0; i < 2U; ++i)
    {
        list.SetComputeRootShaderResourceView(i + 1U, slot.inputs[i].Get()->GetGPUVirtualAddress());
    }
    for (UINT i = 0; i < 3U; ++i)
    {
        list.SetComputeRootUnorderedAccessView(i + 3U, slot.work[i].Get()->GetGPUVirtualAddress());
    }
    list.SetPipelineState(pipeline_.Get());
    list.Dispatch((config.grid.nx + 10U) / 8U, (config.grid.ny + 10U) / 8U, 1U);
    gpu::Barrier(list, *slot.work[0].Get(), compute, uav, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                 D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    gpu::BeginDraw(frame);
    list.SetGraphicsRootSignature(graphicsRoot_.Get());
    list.SetGraphicsRoot32BitConstants(0U, 7U, &params, 0U);
    list.SetGraphicsRootShaderResourceView(1U, slot.work[0].Get()->GetGPUVirtualAddress());
    list.SetPipelineState(graphics_.Get());
    list.DrawInstanced(3U, 1U, 0U, 0U);
    gpu::EndDraw(frame);
    for (std::size_t i = 0; i < slot.work.size(); ++i)
    {
        gpu::Barrier(list, *slot.work[i].Get(), i == 0U ? D3D12_BARRIER_SYNC_PIXEL_SHADING : compute,
                     i == 0U ? D3D12_BARRIER_ACCESS_SHADER_RESOURCE : uav, D3D12_BARRIER_SYNC_COPY,
                     D3D12_BARRIER_ACCESS_COPY_SOURCE);
        list.CopyBufferRegion(slot.readback[i].Get(), 0U, slot.work[i].Get(), 0U, kWorkSizes[i]);
    }
    lastSlot_ = frame.frameSlot;
    rendered_ = true;
    return {};
}

auto Renderer::ReadBackOutputs() -> std::expected<gpu::Readback, lgp::framework::Error>
{
    if (!rendered_ || !resources_)
    {
        return std::unexpected(MakeError("Ch37 readback", "No rendered frame."));
    }
    auto status = resources_->WaitForGpuIdle();
    if (!status)
    {
        return std::unexpected(std::move(status.error()));
    }
    auto config = Active();
    auto const &slot = slots_[lastSlot_];
    auto divergence = *MakeField(config.grid, Layout::Cell);
    auto const stride = config.grid.nx + 2U;
    for (std::uint32_t y = 0; y < config.grid.ny; ++y)
    {
        for (std::uint32_t x = 0; x < config.grid.nx; ++x)
        {
            auto i = (y + 1U) * stride + x + 1U;
            float value{};
            std::memcpy(&value, slot.readback[0].Data() + i * sizeof(float), sizeof(float));
            if (!std::isfinite(value))
            {
                return std::unexpected(MakeError("Ch37 baseline", "GPU divergence was nonfinite."));
            }
            divergence.values[i] = value;
        }
    }
    auto u = ReadField(slot.readback[1], config.grid, Layout::XFace);
    auto v = ReadField(slot.readback[2], config.grid, Layout::YFace);
    if (!u)
    {
        return std::unexpected(std::move(u.error()));
    }
    if (!v)
    {
        return std::unexpected(std::move(v.error()));
    }
    auto beforeRms = RootMeanSquare(config.grid, divergence);
    auto after = Divergence(config.grid, Velocity{*u, *v}, config.boundary);
    if (!beforeRms || !after)
    {
        return std::unexpected(MakeError("Ch37 readback", "Invalid GPU velocity."));
    }
    auto afterRms = RootMeanSquare(config.grid, *after);
    if (!afterRms)
    {
        return std::unexpected(MakeError("Ch37 readback", "Invalid divergence."));
    }
    auto p = *MakeField(config.grid, Layout::Cell);
    return gpu::Readback{
        {std::move(p), {std::move(*u), std::move(*v)}, 0U, false, {*beforeRms / config.dt}, *beforeRms, *afterRms},
        true,
        std::move(divergence)};
}
void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    if (headlessCli_ && !headless_ && rendered_)
    {
        auto result = ReadBackOutputs();
        auto imageRange = ImageRange(*resources_, lastSlot_);
        if (result && imageRange)
        {
            std::printf("Ch37 Starter baseline iterations=0 divergenceBefore=%.8g divergenceAfter=%.8g "
                        "imageRange=%u\n",
                        result->projection.divergenceBefore, result->projection.divergenceAfter, *imageRange);
        }
        else
        {
            std::fprintf(stderr, "Ch37 Starter GPU readback: %s\n",
                         result ? "render-target readback failed"
                                : lgp::framework::FormatError(result.error()).c_str());
        }
    }
    slots_.clear();
    root_.Reset();
    graphicsRoot_.Reset();
    pipeline_.Reset();
    graphics_.Reset();
    resources_ = nullptr;
    rendered_ = false;
}
} // namespace ch37::projection::starter
