#include "Renderer.hpp"

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/error.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace ch35::spatial::solution
{
namespace
{
using lgp::framework::MakeError;
using lgp::framework::MakeHResultError;
using lgp::framework::Status;
using Microsoft::WRL::ComPtr;

constexpr auto Align256(std::uint64_t value) noexcept -> std::uint64_t
{
    return (value + 255U) & ~255ULL;
}

// Root UAV offsets are 256-byte aligned; the last output word is a writable guard.
struct Layout final
{
    static constexpr std::uint64_t keyed = 0U;
    static constexpr std::uint64_t sorted = Align256(keyed + sizeof(gpu::GpuRecord) * kMaximumRecords);
    static constexpr std::uint64_t ranges = Align256(sorted + sizeof(gpu::GpuRecord) * kMaximumRecords);
    static constexpr std::uint64_t queries = Align256(ranges + sizeof(gpu::GpuRange) * kMaximumCells);
    static constexpr std::uint64_t neighbors = Align256(queries + sizeof(gpu::GpuQuery) * kMaximumRecords);
    static constexpr std::uint64_t stats =
        Align256(neighbors + sizeof(std::uint32_t) * (static_cast<std::uint64_t>(kMaximumNeighborOutputs) + 1U));
    static constexpr std::uint64_t bytes = Align256(stats + sizeof(gpu::GpuStats));
};

struct LabConstants final
{
    std::uint32_t inputCount{};
    std::uint32_t recordCapacity{};
    std::uint32_t pad0{};
    std::uint32_t pad1{};
    std::uint32_t dimensionsX{};
    std::uint32_t dimensionsY{};
    std::uint32_t dimensionsZ{};
    std::uint32_t cellCount{};
    std::uint32_t queryCount{};
    std::uint32_t queryCapacity{};
    std::uint32_t outputCapacity{};
    std::uint32_t pad2{};
    float originX{};
    float originY{};
    float originZ{};
    float cellSize{};
    float radius{};
    std::uint32_t pad3{};
    std::uint32_t pad4{};
    std::uint32_t pad5{};
};
static_assert(sizeof(LabConstants) == 80U);

template <typename T> [[nodiscard]] auto Load(std::byte const *bytes, std::uint64_t offset) noexcept -> T
{
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(T));
    return value;
}

[[nodiscard]] auto Compile(lgp::framework::ShaderCompiler &compiler, std::filesystem::path const &path,
                           wchar_t const *entry, wchar_t const *profile, lgp::framework::CompiledShader &shader)
    -> Status
{
    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = path;
    options.entryPoint = entry;
    options.targetProfile = profile;
    options.additionalArguments = {L"-E", entry, L"-T", profile};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif
    auto result = compiler.Compile(options);
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    shader = std::move(*result);
    return {};
}

[[nodiscard]] auto Root(ID3D12Device10 &device, D3D12_ROOT_SIGNATURE_DESC const &description,
                        ComPtr<ID3D12RootSignature> &root) -> Status
{
    ComPtr<ID3DBlob> blob{};
    ComPtr<ID3DBlob> errors{};
    auto const serialized = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(),
                                                        errors.GetAddressOf());
    if (FAILED(serialized))
    {
        std::string const diagnostics =
            errors ? std::string{static_cast<char const *>(errors->GetBufferPointer()), errors->GetBufferSize()} : "";
        return std::unexpected(MakeHResultError("D3D12SerializeRootSignature", serialized, diagnostics));
    }
    auto const created = device.CreateRootSignature(0U, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                    IID_PPV_ARGS(root.ReleaseAndGetAddressOf()));
    if (FAILED(created))
    {
        return std::unexpected(MakeHResultError("ID3D12Device::CreateRootSignature", created));
    }
    return {};
}

[[nodiscard]] auto ComputePipeline(ID3D12Device10 &device, ID3D12RootSignature &root,
                                   lgp::framework::CompiledShader const &shader, ComPtr<ID3D12PipelineState> &pipeline)
    -> Status
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &root;
    description.CS = shader.Bytecode();
    auto const created =
        device.CreateComputePipelineState(&description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
    if (FAILED(created))
    {
        return std::unexpected(MakeHResultError("ID3D12Device::CreateComputePipelineState", created));
    }
    return {};
}

[[nodiscard]] auto GraphicsPipeline(ID3D12Device10 &device, ID3D12RootSignature &root,
                                    lgp::framework::CompiledShader const &vertex,
                                    lgp::framework::CompiledShader const &pixel, DXGI_FORMAT format,
                                    ComPtr<ID3D12PipelineState> &pipeline) -> Status
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &root;
    description.VS = vertex.Bytecode();
    description.PS = pixel.Bytecode();
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1U;
    auto const created =
        device.CreateGraphicsPipelineState(&description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
    if (FAILED(created))
    {
        return std::unexpected(MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", created));
    }
    return {};
}
} // namespace

auto Renderer::ActiveConfiguration() const -> gpu::LabConfiguration
{
    return headless_.value_or(gpu::LabConfiguration{});
}

auto Renderer::CreateShaders() -> Status
{
    auto compiler = lgp::framework::ShaderCompiler::Create();
    if (!compiler)
    {
        return std::unexpected(std::move(compiler.error()));
    }
    auto const path = std::filesystem::path{__FILE__}.parent_path() / "SpatialLab.hlsl";
    if (auto status = Compile(*compiler, path, L"KeyCS", L"cs_6_0", keyShader_); !status)
    {
        return status;
    }
    if (auto status = Compile(*compiler, path, L"SortCS", L"cs_6_0", sortShader_); !status)
    {
        return status;
    }
    if (auto status = Compile(*compiler, path, L"RangesCS", L"cs_6_0", rangeShader_); !status)
    {
        return status;
    }
    if (auto status = Compile(*compiler, path, L"QueryCS", L"cs_6_0", queryShader_); !status)
    {
        return status;
    }
    if (auto status = Compile(*compiler, path, L"PointVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    return Compile(*compiler, path, L"PointPS", L"ps_6_0", pixelShader_);
}

auto Renderer::CreatePipelines() -> Status
{
    auto &device = *resources_->device();
    std::array<D3D12_ROOT_PARAMETER, 9U> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0U;
    parameters[0].Constants.Num32BitValues = sizeof(LabConstants) / sizeof(std::uint32_t);
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor.ShaderRegister = 0U;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].Descriptor.ShaderRegister = 1U;
    for (UINT index = 0U; index < 6U; ++index)
    {
        parameters[index + 3U].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[index + 3U].Descriptor.ShaderRegister = index;
    }
    for (auto &parameter : parameters)
    {
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC computeDescription{};
    computeDescription.NumParameters = static_cast<UINT>(parameters.size());
    computeDescription.pParameters = parameters.data();
    if (auto status = Root(device, computeDescription, computeRoot_); !status)
    {
        return status;
    }
    if (auto status = ComputePipeline(device, *computeRoot_.Get(), keyShader_, keyPipeline_); !status)
    {
        return status;
    }
    if (auto status = ComputePipeline(device, *computeRoot_.Get(), sortShader_, sortPipeline_); !status)
    {
        return status;
    }
    if (auto status = ComputePipeline(device, *computeRoot_.Get(), rangeShader_, rangePipeline_); !status)
    {
        return status;
    }
    if (auto status = ComputePipeline(device, *computeRoot_.Get(), queryShader_, queryPipeline_); !status)
    {
        return status;
    }

    std::array<D3D12_ROOT_PARAMETER, 2U> graphicsParameters{};
    graphicsParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[0].Constants.ShaderRegister = 1U;
    graphicsParameters[0].Constants.Num32BitValues = 2U;
    graphicsParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    graphicsParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    graphicsParameters[1].Descriptor.ShaderRegister = 0U;
    graphicsParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC graphicsDescription{};
    graphicsDescription.NumParameters = static_cast<UINT>(graphicsParameters.size());
    graphicsDescription.pParameters = graphicsParameters.data();
    graphicsDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    if (auto status = Root(device, graphicsDescription, graphicsRoot_); !status)
    {
        return status;
    }
    return GraphicsPipeline(device, *graphicsRoot_.Get(), vertexShader_, pixelShader_, resources_->back_buffer_format(),
                            graphicsPipeline_);
}

auto Renderer::CreateResources() -> Status
{
    slots_.resize(resources_->back_buffer_count());
    for (auto &slot : slots_)
    {
        auto make = [&](gpu::BufferResource &buffer, std::uint64_t bytes, D3D12_HEAP_TYPE heap,
                        D3D12_RESOURCE_FLAGS flags, bool mapped) -> Status
        {
            auto created = gpu::CreateBuffer(*resources_->device(), bytes, heap, flags, mapped);
            if (!created)
            {
                return std::unexpected(std::move(created.error()));
            }
            buffer = std::move(*created);
            return {};
        };
        if (auto status = make(slot.input, sizeof(gpu::GpuParticle) * kMaximumRecords, D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_FLAG_NONE, true);
            !status)
        {
            return status;
        }
        if (auto status = make(slot.ids, sizeof(std::uint32_t) * kMaximumRecords, D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_FLAG_NONE, true);
            !status)
        {
            return status;
        }
        if (auto status = make(slot.output, Layout::bytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, false);
            !status)
        {
            return status;
        }
        if (auto status = make(slot.readback, Layout::bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, true);
            !status)
        {
            return status;
        }
    }
    return {};
}

auto Renderer::Initialize(lgp::framework::ApplicationInitContext const &context) -> Status
{
    resources_ = &context.deviceResources;
    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreatePipelines(); !status)
    {
        return status;
    }
    return CreateResources();
}

auto Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D) -> Status
{
    return {};
}

auto Renderer::Update(lgp::framework::UpdateContext const &) -> Status
{
    return gpu::ValidateConfiguration(ActiveConfiguration());
}

auto Renderer::Render(lgp::framework::FrameContext const &frame) -> Status
{
    if (frame.frameSlot >= slots_.size())
    {
        return std::unexpected(MakeError("Render", "Chapter 35 frame slot is out of range."));
    }
    auto const configuration = ActiveConfiguration();
    auto &slot = slots_[frame.frameSlot];
    lastFrameSlot_ = frame.frameSlot;
    auto &list = *frame.commandList;

    // Only raw particles and query IDs cross the CPU-to-GPU boundary.
    std::array<gpu::GpuParticle, kMaximumRecords> particles{};
    for (std::size_t index = 0U; index < configuration.particles.size(); ++index)
    {
        auto const &source = configuration.particles[index];
        particles[index] = {static_cast<float>(source.position.x), static_cast<float>(source.position.y),
                            static_cast<float>(source.position.z), source.identity, source.alive};
    }
    std::memcpy(slot.input.mapped_data(), particles.data(), sizeof(particles));
    std::array<std::uint32_t, kMaximumRecords> ids{};
    std::copy(configuration.queryIds.begin(), configuration.queryIds.end(), ids.begin());
    std::memcpy(slot.ids.mapped_data(), ids.data(), sizeof(ids));

    auto const cells = *ValidateGrid(configuration.grid);
    LabConstants constants{};
    constants.inputCount = static_cast<std::uint32_t>(configuration.particles.size());
    constants.recordCapacity = configuration.recordCapacity;
    constants.dimensionsX = configuration.grid.dimensions.x;
    constants.dimensionsY = configuration.grid.dimensions.y;
    constants.dimensionsZ = configuration.grid.dimensions.z;
    constants.cellCount = cells;
    constants.queryCount = static_cast<std::uint32_t>(configuration.queryIds.size());
    constants.queryCapacity = configuration.queryCapacity;
    constants.outputCapacity = configuration.outputCapacity;
    constants.originX = static_cast<float>(configuration.grid.origin.x);
    constants.originY = static_cast<float>(configuration.grid.origin.y);
    constants.originZ = static_cast<float>(configuration.grid.origin.z);
    constants.cellSize = static_cast<float>(configuration.grid.cellSize);
    constants.radius = static_cast<float>(configuration.radius);

    gpu::BufferBarrier(list, *slot.output.Get(), slot.initialized ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_NONE,
                       slot.initialized ? D3D12_BARRIER_ACCESS_COPY_SOURCE : D3D12_BARRIER_ACCESS_NO_ACCESS,
                       D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    slot.initialized = true;
    list.SetComputeRootSignature(computeRoot_.Get());
    list.SetComputeRoot32BitConstants(0U, sizeof(constants) / sizeof(std::uint32_t), &constants, 0U);
    list.SetComputeRootShaderResourceView(1U, slot.input.Get()->GetGPUVirtualAddress());
    list.SetComputeRootShaderResourceView(2U, slot.ids.Get()->GetGPUVirtualAddress());
    std::array<std::uint64_t, 6U> const offsets{Layout::keyed,   Layout::sorted,    Layout::ranges,
                                                Layout::queries, Layout::neighbors, Layout::stats};
    for (UINT index = 0U; index < offsets.size(); ++index)
    {
        list.SetComputeRootUnorderedAccessView(index + 3U, slot.output.Get()->GetGPUVirtualAddress() + offsets[index]);
    }
    list.SetPipelineState(keyPipeline_.Get());
    list.Dispatch(1U, 1U, 1U);
    auto uavBarrier = [&]()
    {
        gpu::BufferBarrier(list, *slot.output.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                           D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                           D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    };
    uavBarrier();
    list.SetPipelineState(sortPipeline_.Get());
    list.Dispatch(1U, 1U, 1U);
    uavBarrier();
    list.SetPipelineState(rangePipeline_.Get());
    list.Dispatch((cells + 255U) / 256U, 1U, 1U);
    uavBarrier();
    list.SetPipelineState(queryPipeline_.Get());
    list.Dispatch(1U, 1U, 1U);
    gpu::BufferBarrier(list, *slot.output.Get(), D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                       D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_VERTEX_SHADING,
                       D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

    lgp::framework::TransitionTexture(
        list, *frame.renderTarget,
        {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frame.renderTargetInitialLayout},
        {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET});
    constexpr float kClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    list.ClearRenderTargetView(frame.renderTargetView, kClear, 0U, nullptr);
    list.OMSetRenderTargets(1U, &frame.renderTargetView, FALSE, nullptr);
    list.RSSetViewports(1U, &frame.viewport);
    list.RSSetScissorRects(1U, &frame.scissorRect);
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list.SetGraphicsRootSignature(graphicsRoot_.Get());
    std::array<float, 2U> const drawConstants{configuration.viewScale, configuration.pointHalfExtent};
    list.SetGraphicsRoot32BitConstants(0U, static_cast<UINT>(drawConstants.size()), drawConstants.data(), 0U);
    list.SetGraphicsRootShaderResourceView(1U, slot.output.Get()->GetGPUVirtualAddress() + Layout::sorted);
    list.SetPipelineState(graphicsPipeline_.Get());
    if (configuration.recordCapacity != 0U)
    {
        list.DrawInstanced(6U, configuration.recordCapacity, 0U, 0U);
    }
    lgp::framework::TransitionTexture(
        list, *frame.renderTarget,
        {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET},
        {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
         frame.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT});
    gpu::BufferBarrier(list, *slot.output.Get(), D3D12_BARRIER_SYNC_VERTEX_SHADING,
                       D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE);
    list.CopyBufferRegion(slot.readback.Get(), 0U, slot.output.Get(), 0U, Layout::bytes);
    return {};
}

auto Renderer::ReadBackOutputs() -> std::expected<gpu::FrameReadback, lgp::framework::Error>
{
    auto const idle = resources_->WaitForGpuIdle();
    if (!idle)
    {
        return std::unexpected(std::move(idle.error()));
    }
    auto const configuration = ActiveConfiguration();
    auto const *data = slots_[lastFrameSlot_].readback.mapped_data();
    gpu::FrameReadback result{};
    result.stats = Load<gpu::GpuStats>(data, Layout::stats);
    if (result.stats.emittedCount > configuration.recordCapacity ||
        result.stats.outputCount > configuration.outputCapacity)
    {
        return std::unexpected(MakeError("ReadBackOutputs", "GPU produced an out-of-bounds count."));
    }
    for (std::uint32_t index = 0U; index < result.stats.emittedCount; ++index)
    {
        result.records.push_back(Load<gpu::GpuRecord>(data, Layout::sorted + sizeof(gpu::GpuRecord) * index));
    }
    result.paddingIntact = true;
    for (std::uint32_t index = result.stats.emittedCount; index < kMaximumRecords; ++index)
    {
        auto const record = Load<gpu::GpuRecord>(data, Layout::sorted + sizeof(gpu::GpuRecord) * index);
        if (record.key != kInvalidCell || record.identity != kInvalidParticleId)
        {
            result.paddingIntact = false;
        }
    }
    auto const cells = *ValidateGrid(configuration.grid);
    for (std::uint32_t index = 0U; index < cells; ++index)
    {
        result.ranges.push_back(Load<gpu::GpuRange>(data, Layout::ranges + sizeof(gpu::GpuRange) * index));
    }
    auto const stored =
        std::min(static_cast<std::uint32_t>(configuration.queryIds.size()), configuration.queryCapacity);
    for (std::uint32_t index = 0U; index < stored; ++index)
    {
        result.queries.push_back(Load<gpu::GpuQuery>(data, Layout::queries + sizeof(gpu::GpuQuery) * index));
    }
    for (std::uint32_t index = 0U; index < result.stats.outputCount; ++index)
    {
        result.neighbors.push_back(Load<std::uint32_t>(data, Layout::neighbors + sizeof(std::uint32_t) * index));
    }
    result.outputGuardIntact =
        Load<std::uint32_t>(data, Layout::neighbors + sizeof(std::uint32_t) * configuration.outputCapacity) ==
        kInvalidParticleId;
    result.gpuComputed = true;
    return result;
}

void Renderer::ConfigureHeadlessTest(gpu::LabConfiguration const &configuration)
{
    headless_ = configuration;
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    slots_.clear();
    keyPipeline_.Reset();
    sortPipeline_.Reset();
    rangePipeline_.Reset();
    queryPipeline_.Reset();
    graphicsPipeline_.Reset();
    computeRoot_.Reset();
    graphicsRoot_.Reset();
    resources_ = nullptr;
}
} // namespace ch35::spatial::solution
