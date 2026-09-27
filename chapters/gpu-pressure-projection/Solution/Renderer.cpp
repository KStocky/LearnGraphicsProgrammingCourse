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

namespace ch37::projection::solution
{
namespace
{
using lgp::framework::MakeError;
using lgp::framework::Status;
constexpr std::uint64_t kCells = 66U * 66U * sizeof(float);
constexpr std::uint64_t kFaces = 67U * 66U * sizeof(float);
constexpr std::uint64_t kHistory = (gpu::kMaxGpuIterations + 8U) * sizeof(float);
constexpr std::array<std::uint64_t, 9> kWorkSizes{kCells, kCells, kCells, kCells,  kCells,
                                                  kCells, kFaces, kFaces, kHistory};
struct Parameters final
{
    std::uint32_t nx, ny;
    float h, dt;
    std::uint32_t boundary, method, stop, limit;
    float tolerance;
    std::uint32_t iteration, colour, screenWidth, screenHeight;
};
static_assert(sizeof(Parameters) == 52U);
enum Stage : std::size_t
{
    Source,
    InitializeStage,
    Jacobi,
    CopyPressure,
    RedBlack,
    CgOperator,
    CgAlpha,
    CgUpdate,
    CgBeta,
    CgDirection,
    GaugeMean,
    GaugeApply,
    TrueResidual,
    ReduceResidual,
    PressureGhost,
    GradientX,
    GradientY
};
constexpr std::array<wchar_t const *, 17> kStages{
    L"SourceCS",       L"InitializeCS",     L"JacobiCS",        L"CopyPressureCS", L"RedBlackCS",  L"CgOperatorCS",
    L"CgAlphaCS",      L"CgUpdateCS",       L"CgBetaCS",        L"CgDirectionCS",  L"GaugeMeanCS", L"GaugeApplyCS",
    L"TrueResidualCS", L"ReduceResidualCS", L"PressureGhostCS", L"GradientXCS",    L"GradientYCS"};

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
    std::array<D3D12_ROOT_PARAMETER, 12> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0U;
    parameters[0].Constants.Num32BitValues = 13U;
    for (UINT i = 0; i < 2U; ++i)
    {
        parameters[i + 1U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[i + 1U].Descriptor.ShaderRegister = i;
    }
    for (UINT i = 0; i < 9U; ++i)
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
    for (std::size_t i = 0; i < pipelines_.size(); ++i)
    {
        auto compiled = gpu::Compile(*compiler, path, kStages[i], L"cs_6_0");
        if (!compiled)
        {
            return std::unexpected(std::move(compiled.error()));
        }
        auto pipeline = gpu::MakeCompute(*resources_->device(), *root_.Get(), *compiled);
        if (!pipeline)
        {
            return std::unexpected(std::move(pipeline.error()));
        }
        pipelines_[i] = std::move(*pipeline);
    }
    std::array<D3D12_ROOT_PARAMETER, 2> draw{};
    draw[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    draw[0].Constants.ShaderRegister = 0U;
    draw[0].Constants.Num32BitValues = 13U;
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
            auto bytes = kWorkSizes[std::array<std::size_t, 4>{0U, 6U, 7U, 8U}[i]];
            if (auto s = create(slot.readback[i], bytes, D3D12_HEAP_TYPE_READBACK, true); !s)
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
        bool copied = i == 0U || i == 6U || i == 7U || i == 8U;
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
                      static_cast<std::uint32_t>(config.settings.method),
                      static_cast<std::uint32_t>(config.settings.stop),
                      config.settings.maxIterations,
                      static_cast<float>(config.settings.tolerance),
                      0U,
                      0U,
                      static_cast<std::uint32_t>(frame.viewport.Width),
                      static_cast<std::uint32_t>(frame.viewport.Height)};
    list.SetComputeRootSignature(root_.Get());
    for (UINT i = 0; i < 2U; ++i)
    {
        list.SetComputeRootShaderResourceView(i + 1U, slot.inputs[i].Get()->GetGPUVirtualAddress());
    }
    for (UINT i = 0; i < 9U; ++i)
    {
        list.SetComputeRootUnorderedAccessView(i + 3U, slot.work[i].Get()->GetGPUVirtualAddress());
    }
    auto syncWork = [&]()
    {
        // Enhanced UAV barriers establish device-wide ordering between per-cell passes and reductions.
        for (auto &buffer : slot.work)
        {
            gpu::Barrier(list, *buffer.Get(), compute, uav, compute, uav);
        }
    };
    auto dispatch = [&](Stage stage, std::uint32_t width, std::uint32_t height)
    {
        list.SetComputeRoot32BitConstants(0U, 13U, &params, 0U);
        list.SetPipelineState(pipelines_[stage].Get());
        list.Dispatch(width, height, 1U);
        syncWork();
    };
    auto cellsX = (config.grid.nx + 7U) / 8U;
    auto cellsY = (config.grid.ny + 7U) / 8U;
    dispatch(Source, cellsX, cellsY);
    dispatch(InitializeStage, 1U, 1U);
    // Record a bounded schedule once; GPU convergence flags make later passes no-ops.
    for (std::uint32_t iteration = 0; iteration < config.settings.maxIterations; ++iteration)
    {
        params.iteration = iteration;
        if (config.settings.method == Method::Jacobi)
        {
            dispatch(Jacobi, cellsX, cellsY);
            dispatch(CopyPressure, cellsX, cellsY);
        }
        else if (config.settings.method == Method::RedBlack)
        {
            params.colour = 0U;
            dispatch(RedBlack, cellsX, cellsY);
            params.colour = 1U;
            dispatch(RedBlack, cellsX, cellsY);
        }
        else
        {
            dispatch(CgOperator, cellsX, cellsY);
            dispatch(CgAlpha, 1U, 1U);
            dispatch(CgUpdate, cellsX, cellsY);
            dispatch(CgBeta, 1U, 1U);
            dispatch(CgDirection, cellsX, cellsY);
        }
        if (config.boundary != Boundary::Open)
        {
            dispatch(GaugeMean, 1U, 1U);
            dispatch(GaugeApply, cellsX, cellsY);
        }
        dispatch(TrueResidual, cellsX, cellsY);
        dispatch(ReduceResidual, 1U, 1U);
    }
    dispatch(PressureGhost, (config.grid.nx + 9U) / 8U, (config.grid.ny + 9U) / 8U);
    dispatch(GradientX, (config.grid.nx + 10U) / 8U, (config.grid.ny + 9U) / 8U);
    dispatch(GradientY, (config.grid.nx + 9U) / 8U, (config.grid.ny + 10U) / 8U);
    gpu::Barrier(list, *slot.work[0].Get(), compute, uav, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                 D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    gpu::BeginDraw(frame);
    list.SetGraphicsRootSignature(graphicsRoot_.Get());
    list.SetGraphicsRoot32BitConstants(0U, 13U, &params, 0U);
    list.SetGraphicsRootShaderResourceView(1U, slot.work[0].Get()->GetGPUVirtualAddress());
    list.SetPipelineState(graphics_.Get());
    list.DrawInstanced(3U, 1U, 0U, 0U);
    gpu::EndDraw(frame);
    constexpr std::array<std::size_t, 4> outputs{0U, 6U, 7U, 8U};
    for (std::size_t i = 0; i < outputs.size(); ++i)
    {
        auto index = outputs[i];
        gpu::Barrier(list, *slot.work[index].Get(), index == 0U ? D3D12_BARRIER_SYNC_PIXEL_SHADING : compute,
                     index == 0U ? D3D12_BARRIER_ACCESS_SHADER_RESOURCE : uav, D3D12_BARRIER_SYNC_COPY,
                     D3D12_BARRIER_ACCESS_COPY_SOURCE);
        list.CopyBufferRegion(slot.readback[i].Get(), 0U, slot.work[index].Get(), 0U, kWorkSizes[index]);
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
    auto getHistory = [&](std::size_t i)
    {
        float v{};
        std::memcpy(&v, slot.readback[3].Data() + i * sizeof(float), sizeof(float));
        return v;
    };
    auto const iterationsFloat = getHistory(config.settings.maxIterations + 1U);
    auto const flag = getHistory(config.settings.maxIterations + 2U);
    if (!std::isfinite(iterationsFloat) || iterationsFloat < 0.0F ||
        iterationsFloat > static_cast<float>(config.settings.maxIterations) || flag < 0.0F)
    {
        return std::unexpected(MakeError("Ch37 readback", "GPU solver breakdown or incompatible source."));
    }
    auto const iterations = static_cast<std::uint32_t>(iterationsFloat);
    if (config.settings.stop == Stop::Tolerance && flag == 0.0F)
    {
        return std::unexpected(MakeError("Ch37 readback", "GPU solver did not converge within the iteration cap."));
    }
    auto p = ReadField(slot.readback[0], config.grid, Layout::Cell);
    auto u = ReadField(slot.readback[1], config.grid, Layout::XFace);
    auto v = ReadField(slot.readback[2], config.grid, Layout::YFace);
    if (!p)
    {
        return std::unexpected(std::move(p.error()));
    }
    if (!u)
    {
        return std::unexpected(std::move(u.error()));
    }
    if (!v)
    {
        return std::unexpected(std::move(v.error()));
    }
    std::vector<double> history;
    history.reserve(iterations + 1U);
    for (std::uint32_t i = 0; i <= iterations; ++i)
    {
        double value = getHistory(i);
        if (!std::isfinite(value))
        {
            return std::unexpected(MakeError("Ch37 readback", "Nonfinite GPU residual."));
        }
        history.push_back(value);
    }
    auto before = Divergence(config.grid, config.velocity, config.boundary);
    auto after = Divergence(config.grid, Velocity{*u, *v}, config.boundary);
    if (!before || !after)
    {
        return std::unexpected(MakeError("Ch37 readback", "Invalid GPU velocity."));
    }
    auto beforeRms = RootMeanSquare(config.grid, *before);
    auto afterRms = RootMeanSquare(config.grid, *after);
    if (!beforeRms || !afterRms)
    {
        return std::unexpected(MakeError("Ch37 readback", "Invalid divergence."));
    }
    return gpu::Readback{{std::move(*p),
                          {std::move(*u), std::move(*v)},
                          iterations,
                          flag == 1.0F,
                          std::move(history),
                          *beforeRms,
                          *afterRms},
                         true};
}
void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    if (headlessCli_ && !headless_ && rendered_)
    {
        auto result = ReadBackOutputs();
        auto imageRange = ImageRange(*resources_, lastSlot_);
        if (result && imageRange)
        {
            std::printf("Ch37 Solution GPU method=%u iterations=%u converged=%u "
                        "divergenceBefore=%.8g divergenceAfter=%.8g residual=%.8g imageRange=%u\n",
                        static_cast<unsigned>(Active().settings.method), result->projection.iterations,
                        static_cast<unsigned>(result->projection.converged), result->projection.divergenceBefore,
                        result->projection.divergenceAfter, result->projection.residualHistory.back(), *imageRange);
        }
        else
        {
            std::fprintf(stderr, "Ch37 Solution GPU readback: %s\n",
                         result ? "render-target readback failed"
                                : lgp::framework::FormatError(result.error()).c_str());
        }
    }
    slots_.clear();
    root_.Reset();
    graphicsRoot_.Reset();
    graphics_.Reset();
    for (auto &pipeline : pipelines_)
    {
        pipeline.Reset();
    }
    resources_ = nullptr;
    rendered_ = false;
}
} // namespace ch37::projection::solution
