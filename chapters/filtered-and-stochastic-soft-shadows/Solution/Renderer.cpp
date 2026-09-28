#include "Renderer.hpp"

#include <lgp/framework/error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

namespace ch38::soft_shadows::solution
{
namespace
{
using lgp::framework::MakeError;
using lgp::framework::Status;
constexpr std::uint64_t kBytes = gpu::kResolution * gpu::kResolution * sizeof(float);
struct Parameters final
{
    std::uint32_t technique;
    float receiverDepth, blockerDepth, lightRadius, bias;
    std::uint32_t screenWidth, screenHeight;
    std::uint32_t padding; // HLSL float2 starts at the next 16-byte constant register.
    float receiverUvOffsetX, receiverUvOffsetY;
};
static_assert(sizeof(Parameters) == 10U * sizeof(std::uint32_t));
} // namespace

Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    resources_ = &context.deviceResources;
    headlessCli_ = context.commandLine.headless;
    auto compiler = lgp::framework::ShaderCompiler::Create();
    if (!compiler)
    {
        return std::unexpected(std::move(compiler.error()));
    }
    auto const path = std::filesystem::path{__FILE__}.parent_path() / "SoftShadowLab.hlsl";

    std::array<D3D12_ROOT_PARAMETER, 4> compute{};
    compute[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    compute[0].Constants.ShaderRegister = 0U;
    compute[0].Constants.Num32BitValues = 10U;
    compute[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    compute[1].Descriptor.ShaderRegister = 0U;
    for (UINT i = 0U; i < 2U; ++i)
    {
        compute[i + 2U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        compute[i + 2U].Descriptor.ShaderRegister = i;
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(compute.size());
    desc.pParameters = compute.data();
    auto root = gpu::MakeRoot(*resources_->device(), desc);
    if (!root)
    {
        return std::unexpected(std::move(root.error()));
    }
    computeRoot_ = std::move(*root);
    auto makeStage = [&](wchar_t const *entry, Microsoft::WRL::ComPtr<ID3D12PipelineState> &target) -> Status
    {
        auto shader = gpu::Compile(*compiler, path, entry, L"cs_6_0");
        if (!shader)
        {
            return std::unexpected(std::move(shader.error()));
        }
        auto pipeline = gpu::MakeCompute(*resources_->device(), *computeRoot_.Get(), *shader);
        if (!pipeline)
        {
            return std::unexpected(std::move(pipeline.error()));
        }
        target = std::move(*pipeline);
        return {};
    };
    if (auto result = makeStage(L"GenerateDepthCS", generate_); !result)
    {
        return result;
    }
    if (auto result = makeStage(L"ShadeCS", shade_); !result)
    {
        return result;
    }

    std::array<D3D12_ROOT_PARAMETER, 2> draw{};
    draw[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    draw[0].Constants.ShaderRegister = 0U;
    draw[0].Constants.Num32BitValues = 10U;
    draw[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    draw[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    draw[1].Descriptor.ShaderRegister = 1U;
    draw[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    desc.NumParameters = static_cast<UINT>(draw.size());
    desc.pParameters = draw.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    root = gpu::MakeRoot(*resources_->device(), desc);
    if (!root)
    {
        return std::unexpected(std::move(root.error()));
    }
    graphicsRoot_ = std::move(*root);
    auto vertex = gpu::Compile(*compiler, path, L"FullscreenVS", L"vs_6_0");
    if (!vertex)
    {
        return std::unexpected(std::move(vertex.error()));
    }
    auto pixel = gpu::Compile(*compiler, path, L"VisibilityPS", L"ps_6_0");
    if (!pixel)
    {
        return std::unexpected(std::move(pixel.error()));
    }
    auto graphics = gpu::MakeGraphics(*resources_->device(), *graphicsRoot_.Get(), *vertex, *pixel,
                                      resources_->back_buffer_format());
    if (!graphics)
    {
        return std::unexpected(std::move(graphics.error()));
    }
    graphics_ = std::move(*graphics);

    slots_.resize(resources_->back_buffer_count());
    for (auto &slot : slots_)
    {
        auto depth = gpu::MakeBuffer(*resources_->device(), kBytes, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, false);
        if (!depth)
        {
            return std::unexpected(std::move(depth.error()));
        }
        slot.depth = std::move(*depth);
        auto visibility = gpu::MakeBuffer(*resources_->device(), kBytes, D3D12_HEAP_TYPE_DEFAULT,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, false);
        if (!visibility)
        {
            return std::unexpected(std::move(visibility.error()));
        }
        slot.visibility = std::move(*visibility);
        auto readback =
            gpu::MakeBuffer(*resources_->device(), kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, true);
        if (!readback)
        {
            return std::unexpected(std::move(readback.error()));
        }
        slot.readback = std::move(*readback);
    }
    return {};
}

Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D)
{
    return {};
}
Status Renderer::Update(lgp::framework::UpdateContext const &context)
{
    if (!testConfig_)
    {
        if (context.input.WasKeyPressed('1'))
        {
            interactive_.technique = gpu::Technique::Hard;
        }
        if (context.input.WasKeyPressed('2'))
        {
            interactive_.technique = gpu::Technique::Pcf;
        }
        if (context.input.WasKeyPressed('3'))
        {
            interactive_.technique = gpu::Technique::Pcss;
        }
        if (context.input.WasKeyPressed('4'))
        {
            interactive_.technique = gpu::Technique::Area;
        }
        if (context.input.WasKeyPressed(VK_OEM_4))
        {
            interactive_.lightRadius = std::max(0.0F, interactive_.lightRadius - 0.01F);
        }
        if (context.input.WasKeyPressed(VK_OEM_6))
        {
            interactive_.lightRadius = std::min(0.2F, interactive_.lightRadius + 0.01F);
        }
    }
    return gpu::Validate(testConfig_.value_or(interactive_));
}
void Renderer::ConfigureHeadlessTest(gpu::Configuration const &config)
{
    testConfig_ = config;
}

Status Renderer::Render(lgp::framework::FrameContext const &frame)
{
    if (frame.frameSlot >= slots_.size())
    {
        return std::unexpected(MakeError("Soft shadow render", "Invalid frame slot."));
    }
    auto const config = testConfig_.value_or(interactive_);
    if (auto result = gpu::Validate(config); !result)
    {
        return result;
    }
    auto &slot = slots_[frame.frameSlot];
    auto &list = *frame.commandList;
    constexpr auto cs = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    constexpr auto uav = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    gpu::Barrier(list, *slot.depth.Get(), slot.used ? cs : D3D12_BARRIER_SYNC_NONE,
                 slot.used ? D3D12_BARRIER_ACCESS_SHADER_RESOURCE : D3D12_BARRIER_ACCESS_NO_ACCESS, cs, uav);
    gpu::Barrier(list, *slot.visibility.Get(), slot.used ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_NONE,
                 slot.used ? D3D12_BARRIER_ACCESS_COPY_SOURCE : D3D12_BARRIER_ACCESS_NO_ACCESS, cs, uav);
    slot.used = true;
    Parameters const params{static_cast<std::uint32_t>(config.technique),
                            config.receiverDepth,
                            config.blockerDepth,
                            config.lightRadius,
                            config.bias,
                            static_cast<std::uint32_t>(frame.viewport.Width),
                            static_cast<std::uint32_t>(frame.viewport.Height),
                            0U,
                            config.receiverUvOffsetX,
                            config.receiverUvOffsetY};
    list.SetComputeRootSignature(computeRoot_.Get());
    list.SetComputeRoot32BitConstants(0U, 10U, &params, 0U);
    list.SetComputeRootUnorderedAccessView(2U, slot.depth.Get()->GetGPUVirtualAddress());
    list.SetComputeRootUnorderedAccessView(3U, slot.visibility.Get()->GetGPUVirtualAddress());
    list.SetPipelineState(generate_.Get());
    list.Dispatch(gpu::kResolution / 8U, gpu::kResolution / 8U, 1U);
    gpu::Barrier(list, *slot.depth.Get(), cs, uav, cs, D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    list.SetComputeRootShaderResourceView(1U, slot.depth.Get()->GetGPUVirtualAddress());
    list.SetPipelineState(shade_.Get());
    list.Dispatch(gpu::kResolution / 8U, gpu::kResolution / 8U, 1U);
    gpu::Barrier(list, *slot.visibility.Get(), cs, uav, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                 D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

    gpu::BeginDraw(frame);
    list.SetGraphicsRootSignature(graphicsRoot_.Get());
    list.SetGraphicsRoot32BitConstants(0U, 10U, &params, 0U);
    list.SetGraphicsRootShaderResourceView(1U, slot.visibility.Get()->GetGPUVirtualAddress());
    list.SetPipelineState(graphics_.Get());
    list.DrawInstanced(3U, 1U, 0U, 0U);
    gpu::EndDraw(frame);
    gpu::Barrier(list, *slot.visibility.Get(), D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                 D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE);
    list.CopyBufferRegion(slot.readback.Get(), 0U, slot.visibility.Get(), 0U, kBytes);
    lastConfig_ = config;
    lastSlot_ = frame.frameSlot;
    rendered_ = true;
    return {};
}

std::expected<gpu::Readback, lgp::framework::Error> Renderer::ReadBackOutputs()
{
    if (!resources_ || !rendered_)
    {
        return std::unexpected(MakeError("Soft shadow readback", "No rendered frame."));
    }
    auto status = resources_->WaitForGpuIdle();
    if (!status)
    {
        return std::unexpected(std::move(status.error()));
    }
    gpu::Readback result{};
    result.configuration = lastConfig_;
    if (lastConfig_.technique == gpu::Technique::Area)
    {
        result.domain = gpu::VisibilityDomain::AnalyticOccluderRays;
        result.lightSamples = lastConfig_.lightRadius == 0.0F ? 1U : gpu::kAreaSamples;
        if (lastConfig_.lightRadius > 0.0F)
        {
            auto const radius = static_cast<double>(lastConfig_.lightRadius);
            result.lightSamplePdf = 1.0 / (3.14159265358979323846 * radius * radius);
        }
    }
    std::memcpy(result.visibility.data(), slots_[lastSlot_].readback.Data(), kBytes);
    if (!std::all_of(result.visibility.begin(), result.visibility.end(),
                     [](float value) { return std::isfinite(value) && value >= 0.0F && value <= 1.0F; }))
    {
        return std::unexpected(MakeError("Soft shadow readback", "GPU produced invalid visibility."));
    }
    return result;
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    if (headlessCli_ && rendered_ && !testConfig_)
    {
        auto result = ReadBackOutputs();
        if (result)
        {
            std::printf("Soft shadow Solution synthetic-depth area visibility: center=%.3f outside=%.3f\n",
                        result->visibility[32U * gpu::kResolution + 32U],
                        result->visibility[32U * gpu::kResolution + 4U]);
        }
        else
        {
            std::fprintf(stderr, "Soft shadow GPU readback: %s\n", lgp::framework::FormatError(result.error()).c_str());
        }
    }
    slots_.clear();
    computeRoot_.Reset();
    graphicsRoot_.Reset();
    generate_.Reset();
    shade_.Reset();
    graphics_.Reset();
    resources_ = nullptr;
    rendered_ = false;
}
} // namespace ch38::soft_shadows::solution
