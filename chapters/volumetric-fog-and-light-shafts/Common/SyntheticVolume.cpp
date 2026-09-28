#include "GpuSupport.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <lgp/framework/error.hpp>
#include <utility>

namespace ch39::fog::gpu
{
namespace
{
using lgp::framework::MakeError;
using lgp::framework::Status;
constexpr std::uint64_t kFroxelBytes = kWidth * kHeight * kSlices * 4U * sizeof(float);
constexpr std::uint64_t kPixelBytes = kWidth * kHeight * 4U * sizeof(float);
struct Constants final
{
    std::uint32_t frame, width, height, historyValid;
    float distance, extinction, albedo, anisotropy;
    float light, signature, shift, historyCompatible;
};
static_assert(sizeof(Constants) == 12 * sizeof(float));
float HistoryFingerprint(Configuration const &config)
{
    std::uint32_t hash = 2166136261U;
    for (float value : {config.medium.extinction, config.medium.albedo, config.medium.anisotropy, config.distance,
                        config.light, config.signature})
    {
        hash = (hash ^ std::bit_cast<std::uint32_t>(value)) * 16777619U;
    }
    hash ^= hash >> 16;
    hash *= 0x7feb352dU;
    hash ^= hash >> 15;
    // float32 represents every integer below 2^24 exactly.
    return static_cast<float>(hash & 0x00ffffffU);
}
} // namespace
Status SyntheticVolume::Initialize(lgp::framework::DeviceResources &resources, std::filesystem::path const &path)
{
    auto compiler = lgp::framework::ShaderCompiler::Create();
    if (!compiler)
    {
        return std::unexpected(std::move(compiler.error()));
    }
    std::array<D3D12_ROOT_PARAMETER, 5> compute{};
    compute[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    compute[0].Constants.ShaderRegister = 0;
    compute[0].Constants.Num32BitValues = 12;
    compute[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    compute[1].Descriptor.ShaderRegister = 0;
    for (UINT i = 0; i < 3; ++i)
    {
        compute[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        compute[i + 2].Descriptor.ShaderRegister = i;
    }
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(compute.size());
    desc.pParameters = compute.data();
    auto root = MakeRoot(*resources.device(), desc);
    if (!root)
    {
        return std::unexpected(std::move(root.error()));
    }
    computeRoot_ = std::move(*root);
    auto stage = [&](wchar_t const *entry, Microsoft::WRL::ComPtr<ID3D12PipelineState> &target) -> Status
    {
        auto shader = Compile(*compiler, path, entry, L"cs_6_0");
        if (!shader)
        {
            return std::unexpected(std::move(shader.error()));
        }
        auto pipeline = MakeCompute(*resources.device(), *computeRoot_.Get(), *shader);
        if (!pipeline)
        {
            return std::unexpected(std::move(pipeline.error()));
        }
        target = std::move(*pipeline);
        return {};
    };
    if (auto status = stage(L"InjectCS", inject_); !status)
    {
        return status;
    }
    if (auto status = stage(L"IntegrateCS", integrate_); !status)
    {
        return status;
    }
    std::array<D3D12_ROOT_PARAMETER, 2> draw{};
    draw[0] = compute[0];
    draw[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    draw[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    draw[1].Descriptor.ShaderRegister = 1;
    draw[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    desc.NumParameters = static_cast<UINT>(draw.size());
    desc.pParameters = draw.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    root = MakeRoot(*resources.device(), desc);
    if (!root)
    {
        return std::unexpected(std::move(root.error()));
    }
    graphicsRoot_ = std::move(*root);
    auto vertex = Compile(*compiler, path, L"FullscreenVS", L"vs_6_0");
    if (!vertex)
    {
        return std::unexpected(std::move(vertex.error()));
    }
    auto pixel = Compile(*compiler, path, L"PreviewPS", L"ps_6_0");
    if (!pixel)
    {
        return std::unexpected(std::move(pixel.error()));
    }
    auto graphics =
        MakeGraphics(*resources.device(), *graphicsRoot_.Get(), *vertex, *pixel, resources.back_buffer_format());
    if (!graphics)
    {
        return std::unexpected(std::move(graphics.error()));
    }
    graphics_ = std::move(*graphics);
    slots_.resize(resources.back_buffer_count());
    auto create = [&](Buffer &buffer, std::uint64_t bytes, bool readback) -> Status
    {
        auto resource =
            MakeBuffer(*resources.device(), bytes, readback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT,
                       readback ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, readback);
        if (!resource)
        {
            return std::unexpected(std::move(resource.error()));
        }
        buffer = std::move(*resource);
        return {};
    };
    for (auto &slot : slots_)
    {
        if (auto status = create(slot.froxel, kFroxelBytes, false); !status)
        {
            return status;
        }
        if (auto status = create(slot.result, kPixelBytes, false); !status)
        {
            return status;
        }
        if (auto status = create(slot.readback, kPixelBytes, true); !status)
        {
            return status;
        }
        if (auto status = create(slot.froxelReadback, kFroxelBytes, true); !status)
        {
            return status;
        }
    }
    for (auto &buffer : history_)
    {
        if (auto status = create(buffer, kPixelBytes, false); !status)
        {
            return status;
        }
    }
    return {};
}
Status SyntheticVolume::Render(lgp::framework::FrameContext const &frame, Configuration const &config)
{
    if (auto status = Validate(config); !status)
    {
        return status;
    }
    if (frame.frameSlot >= slots_.size())
    {
        return std::unexpected(MakeError("Synthetic volume render", "Invalid frame slot."));
    }
    auto &slot = slots_[frame.frameSlot];
    auto &list = *frame.commandList;
    constexpr auto cs = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    constexpr auto uav = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    auto const write = (frameIndex_ + 1) % 2;
    auto const read = frameIndex_ % 2;
    bool const compatible = frameIndex_ != 0 && config.medium.extinction == lastConfig_.medium.extinction &&
                            config.medium.albedo == lastConfig_.medium.albedo &&
                            config.medium.anisotropy == lastConfig_.medium.anisotropy &&
                            config.distance == lastConfig_.distance && config.light == lastConfig_.light &&
                            config.signature == lastConfig_.signature;
    Barrier(list, *slot.froxel.Get(), slot.used ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_NONE,
            slot.used ? D3D12_BARRIER_ACCESS_COPY_SOURCE : D3D12_BARRIER_ACCESS_NO_ACCESS, cs, uav);
    Barrier(list, *slot.result.Get(), slot.used ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_NONE,
            slot.used ? D3D12_BARRIER_ACCESS_COPY_SOURCE : D3D12_BARRIER_ACCESS_NO_ACCESS, cs, uav);
    Barrier(list, *history_[write].Get(), historyUsed_[write] ? cs : D3D12_BARRIER_SYNC_NONE,
            historyUsed_[write] ? D3D12_BARRIER_ACCESS_SHADER_RESOURCE : D3D12_BARRIER_ACCESS_NO_ACCESS, cs, uav);
    slot.used = true;
    Constants const constants{frameIndex_,
                              static_cast<std::uint32_t>(frame.viewport.Width),
                              static_cast<std::uint32_t>(frame.viewport.Height),
                              static_cast<std::uint32_t>(frameIndex_ != 0 && !config.resetHistory),
                              config.distance,
                              config.medium.extinction,
                              config.medium.albedo,
                              config.medium.anisotropy,
                              config.light,
                              HistoryFingerprint(config),
                              config.historyShiftX,
                              compatible ? 1.F : 0.F};
    list.SetComputeRootSignature(computeRoot_.Get());
    list.SetComputeRoot32BitConstants(0, 12, &constants, 0);
    list.SetComputeRootShaderResourceView(1, history_[read].Get()->GetGPUVirtualAddress());
    list.SetComputeRootUnorderedAccessView(2, slot.froxel.Get()->GetGPUVirtualAddress());
    list.SetComputeRootUnorderedAccessView(3, slot.result.Get()->GetGPUVirtualAddress());
    list.SetComputeRootUnorderedAccessView(4, history_[write].Get()->GetGPUVirtualAddress());
    list.SetPipelineState(inject_.Get());
    list.Dispatch(kWidth / 8, kHeight / 8, kSlices);
    Barrier(list, *slot.froxel.Get(), cs, uav, cs, D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    list.SetPipelineState(integrate_.Get());
    list.Dispatch(kWidth / 8, kHeight / 8, 1);
    Barrier(list, *history_[write].Get(), cs, uav, cs, D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    historyUsed_[write] = true;
    Barrier(list, *slot.result.Get(), cs, uav, D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    BeginDraw(frame);
    list.SetGraphicsRootSignature(graphicsRoot_.Get());
    list.SetGraphicsRoot32BitConstants(0, 12, &constants, 0);
    list.SetGraphicsRootShaderResourceView(1, slot.result.Get()->GetGPUVirtualAddress());
    list.SetPipelineState(graphics_.Get());
    list.DrawInstanced(3, 1, 0, 0);
    EndDraw(frame);
    Barrier(list, *slot.result.Get(), D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
            D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE);
    Barrier(list, *slot.froxel.Get(), cs, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_SYNC_COPY,
            D3D12_BARRIER_ACCESS_COPY_SOURCE);
    list.CopyBufferRegion(slot.readback.Get(), 0, slot.result.Get(), 0, kPixelBytes);
    list.CopyBufferRegion(slot.froxelReadback.Get(), 0, slot.froxel.Get(), 0, kFroxelBytes);
    lastSlot_ = frame.frameSlot;
    lastConfig_ = config;
    ++frameIndex_;
    rendered_ = true;
    return {};
}
std::expected<Readback, lgp::framework::Error> SyntheticVolume::ReadBack(lgp::framework::DeviceResources &resources)
{
    if (!rendered_)
    {
        return std::unexpected(MakeError("Synthetic volume readback", "No rendered frame."));
    }
    if (auto status = resources.WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }
    Readback result{};
    result.froxels.resize(kWidth * kHeight * kSlices * 4U);
    std::memcpy(result.pixels.data(), slots_[lastSlot_].readback.Data(), kPixelBytes);
    std::memcpy(result.froxels.data(), slots_[lastSlot_].froxelReadback.Data(), kFroxelBytes);
    if (!std::all_of(result.pixels.begin(), result.pixels.end(), [](float f) { return std::isfinite(f); }) ||
        !std::all_of(result.froxels.begin(), result.froxels.end(), [](float f) { return std::isfinite(f); }))
    {
        return std::unexpected(MakeError("Synthetic volume readback", "Nonfinite GPU output."));
    }
    return result;
}
void SyntheticVolume::Shutdown() noexcept
{
    slots_.clear();
    history_ = {};
    computeRoot_.Reset();
    graphicsRoot_.Reset();
    inject_.Reset();
    integrate_.Reset();
    graphics_.Reset();
    frameIndex_ = lastSlot_ = 0;
    historyUsed_ = {};
    lastConfig_ = {};
    rendered_ = false;
}
} // namespace ch39::fog::gpu
