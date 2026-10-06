#include "GpuLabSupport.hpp"

#include <cstring>
#include <utility>

namespace ch40::pbd::gpu
{
namespace
{
template <typename T> T Read(std::byte const *bytes, std::uint32_t offset)
{
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(T));
    return value;
}
} // namespace

lgp::framework::Result<Slot> CreateSlot(ID3D12Device &device)
{
    using namespace lgp::framework;
    Slot slot{};
    auto input = CreateUploadBuffer(device, kInputBytes, L"PBD slot immutable scene");
    auto seed = CreateUploadBuffer(device, kArenaBytes, L"PBD slot sentinel seed");
    auto arena = CreateDefaultBuffer(device, kArenaBytes, D3D12_RESOURCE_STATE_COMMON, L"PBD slot arena",
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    BufferCreateDesc description{};
    description.sizeInBytes = kArenaBytes;
    description.heapType = D3D12_HEAP_TYPE_READBACK;
    description.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    description.name = L"PBD slot typed readback";
    auto readback = CreateCommittedBuffer(device, description);
    if (!input)
    {
        return std::unexpected(input.error());
    }
    if (!seed)
    {
        return std::unexpected(seed.error());
    }
    if (!arena)
    {
        return std::unexpected(arena.error());
    }
    if (!readback)
    {
        return std::unexpected(readback.error());
    }
    slot.input = std::move(*input);
    slot.seed = std::move(*seed);
    slot.arena = std::move(*arena);
    slot.readback = std::move(*readback);
    for (std::uint32_t offset = 0U; offset < kArenaBytes; offset += 4U)
    {
        std::memcpy(slot.seed.mapped_data() + offset, &kGuard, sizeof(kGuard));
    }
    return slot;
}

lgp::framework::Status UploadScene(Slot &slot, Scene const &scene)
{
    auto valid = ValidateScene(scene);
    if (!valid)
    {
        return std::unexpected(lgp::framework::MakeError("PBD scene", std::string(ErrorName(valid.error()))));
    }
    std::memcpy(slot.input.mapped_data(), scene.particles.data(), scene.particles.size() * sizeof(Particle));
    std::memcpy(slot.input.mapped_data() + kConstraintInput, scene.constraints.data(),
                scene.constraints.size() * sizeof(Constraint));
    return {};
}

lgp::framework::Result<Microsoft::WRL::ComPtr<ID3D12RootSignature>> CreateRootSignature(ID3D12Device &device)
{
    std::array<D3D12_ROOT_PARAMETER1, 4U> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0U, 0U, 16U};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor = {0U, 0U, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[2].Descriptor = {0U, 0U, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[3].Descriptor = {1U, 0U, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC description{};
    description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    description.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    description.Desc_1_1.pParameters = parameters.data();
    Microsoft::WRL::ComPtr<ID3DBlob> blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> errors{};
    HRESULT hr = D3D12SerializeVersionedRootSignature(&description, &blob, &errors);
    if (FAILED(hr))
    {
        return std::unexpected(lgp::framework::MakeHResultError("PBD root serialization", hr));
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root{};
    hr = device.CreateRootSignature(0U, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
    if (FAILED(hr))
    {
        return std::unexpected(lgp::framework::MakeHResultError("PBD root", hr));
    }
    return root;
}

lgp::framework::Result<Microsoft::WRL::ComPtr<ID3D12PipelineState>> CreateComputePipeline(
    ID3D12Device &device, ID3D12RootSignature &root, lgp::framework::CompiledShader const &shader)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &root;
    description.CS = shader.Bytecode();
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline{};
    HRESULT const hr = device.CreateComputePipelineState(&description, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr))
    {
        return std::unexpected(lgp::framework::MakeHResultError("PBD compute pipeline", hr));
    }
    return pipeline;
}

lgp::framework::Result<Microsoft::WRL::ComPtr<ID3D12PipelineState>> CreateGraphicsPipeline(
    ID3D12Device &device, ID3D12RootSignature &root, lgp::framework::CompiledShader const &vertex,
    lgp::framework::CompiledShader const &pixel, DXGI_FORMAT format)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &root;
    description.VS = vertex.Bytecode();
    description.PS = pixel.Bytecode();
    description.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    description.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    description.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    description.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    description.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    description.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1U;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline{};
    HRESULT const hr = device.CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr))
    {
        return std::unexpected(lgp::framework::MakeHResultError("PBD diagnostic pipeline", hr));
    }
    return pipeline;
}

void BufferBarrier(ID3D12GraphicsCommandList7 &list, ID3D12Resource &resource, D3D12_BARRIER_SYNC beforeSync,
                   D3D12_BARRIER_ACCESS beforeAccess, D3D12_BARRIER_SYNC afterSync,
                   D3D12_BARRIER_ACCESS afterAccess) noexcept
{
    D3D12_BUFFER_BARRIER barrier{};
    barrier.SyncBefore = beforeSync;
    barrier.SyncAfter = afterSync;
    barrier.AccessBefore = beforeAccess;
    barrier.AccessAfter = afterAccess;
    barrier.pResource = &resource;
    barrier.Size = UINT64_MAX;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_BUFFER;
    group.NumBarriers = 1U;
    group.pBufferBarriers = &barrier;
    list.Barrier(1U, &group);
}

lgp::framework::Result<FrameReadback> DecodeReadback(Slot const &slot, std::uint32_t frameSlot)
{
    D3D12_RANGE const range{0U, kArenaBytes};
    void *mapped{};
    HRESULT const hr = slot.readback.resource()->Map(0U, &range, &mapped);
    if (FAILED(hr))
    {
        return std::unexpected(lgp::framework::MakeHResultError("PBD readback Map", hr));
    }
    auto const *bytes = static_cast<std::byte const *>(mapped);
    FrameReadback output{};
    auto const &constants = slot.constants;
    output.frameSlot = frameSlot;
    std::uint32_t const status = Read<std::uint32_t>(bytes, kStatus);
    if (status != 0U)
    {
        output.error = static_cast<Error>(status - 1U);
    }
    output.guardsIntact = Read<std::uint32_t>(bytes, kStatus + 4U) == 1U;
    output.executedIterations = Read<std::uint32_t>(bytes, kStatus + 8U);
    for (std::uint32_t index = 0U; index < constants.particles; ++index)
    {
        output.step.particles.push_back(Read<Particle>(bytes, constants.inputOffset + index * 32U));
        output.step.predicted.push_back(Read<Particle>(bytes, kPredicted + index * 32U));
    }
    for (std::uint32_t index = 0U; index < constants.constraints; ++index)
    {
        output.step.lambdas.push_back(Read<float>(bytes, kLambdas + index * 4U));
        output.corrections.push_back(Read<Correction>(bytes, kCorrections + index * 32U));
    }
    output.step.beforeProjection = Read<Metrics>(bytes, kMetrics);
    output.step.final = Read<Metrics>(bytes, kMetrics + 65U * 32U);
    for (std::uint32_t index = 0U; index < output.executedIterations; ++index)
    {
        output.step.iterations.push_back(Read<Metrics>(bytes, kMetrics + (index + 1U) * 32U));
    }
    // Verify the GPU guard verdict independently rather than trusting its status word.
    std::array<std::array<std::uint32_t, 3U>, 6U> const regions{{
        {kA, constants.particles * 32U, 1056U},
        {kB, constants.particles * 32U, 1056U},
        {kPredicted, constants.particles * 32U, 1056U},
        {kLambdas, constants.constraints * 4U, 144U},
        {kCorrections, constants.constraints * 32U, 1056U},
        {kIntegers, constants.particles * 16U, 528U},
    }};
    for (auto const &region : regions)
    {
        for (std::uint32_t offset = region[1]; offset < region[2]; offset += 4U)
        {
            output.guardsIntact &= Read<std::uint32_t>(bytes, region[0] + offset) == kGuard;
        }
    }
    for (std::uint32_t record = 1U; record < 67U; ++record)
    {
        if (record <= output.executedIterations || record == 65U)
        {
            continue;
        }
        for (std::uint32_t offset = 0U; offset < 32U; offset += 4U)
        {
            output.guardsIntact &= Read<std::uint32_t>(bytes, kMetrics + record * 32U + offset) == kGuard;
        }
    }
    for (std::uint32_t offset = kArenaTail; offset < kArenaBytes; offset += 4U)
    {
        output.guardsIntact &= Read<std::uint32_t>(bytes, offset) == kGuard;
    }
    D3D12_RANGE const written{0U, 0U};
    slot.readback.resource()->Unmap(0U, &written);
    if (!output.guardsIntact && !output.error)
    {
        output.error = Error::NonFiniteResult;
    }
    return output;
}

} // namespace ch40::pbd::gpu
