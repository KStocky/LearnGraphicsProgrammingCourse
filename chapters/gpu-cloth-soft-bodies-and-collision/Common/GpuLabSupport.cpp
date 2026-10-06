#include "GpuLabSupport.hpp"
#include <cstring>
#include <utility>

namespace ch41::cloth::gpu
{
namespace
{
template <typename T> T Read(std::byte const *bytes, std::uint32_t offset)
{
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(T));
    return value;
}
template <typename T> void Write(std::byte *bytes, std::uint32_t offset, T const &value)
{
    std::memcpy(bytes + offset, &value, sizeof(T));
}
} // namespace
lgp::framework::Result<Slot> CreateSlot(ID3D12Device &device)
{
    using namespace lgp::framework;
    Slot slot{};
    auto input = CreateUploadBuffer(device, kInputBytes, L"Cloth fenced scene upload");
    auto seed = CreateUploadBuffer(device, kArenaBytes, L"Cloth fenced reset upload");
    BufferCreateDesc description{};
    description.sizeInBytes = kArenaBytes;
    description.heapType = D3D12_HEAP_TYPE_READBACK;
    description.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    description.name = L"Cloth fenced readback";
    auto readback = CreateCommittedBuffer(device, description);
    if (!input)
    {
        return std::unexpected(input.error());
    }
    if (!seed)
    {
        return std::unexpected(seed.error());
    }
    if (!readback)
    {
        return std::unexpected(readback.error());
    }
    slot.input = std::move(*input);
    slot.seed = std::move(*seed);
    slot.readback = std::move(*readback);
    return slot;
}
lgp::framework::Status UploadScene(Slot &slot, Scene const &scene, bool resetRest)
{
    auto valid = Validate(scene);
    if (!valid)
    {
        return std::unexpected(lgp::framework::MakeError("Cloth scene", std::string(ErrorName(valid.error()))));
    }
    auto *input = slot.input.mapped_data();
    for (std::uint32_t offset = 0U; offset < kInputBytes; offset += 4U)
    {
        Write(input, offset, kGuard);
    }
    std::memcpy(input + kConstraintInput, scene.constraints.data(), scene.constraints.size() * sizeof(Constraint));
    std::memcpy(input + kTriangleInput, scene.triangles.data(), scene.triangles.size() * sizeof(Triangle));
    std::memcpy(input + kTetInput, scene.tetrahedra.data(), scene.tetrahedra.size() * sizeof(Tetrahedron));
    if (!resetRest)
    {
        return {};
    }
    auto *seed = slot.seed.mapped_data();
    for (std::uint32_t offset = 0U; offset < kArenaBytes; offset += 4U)
    {
        Write(seed, offset, kGuard);
    }
    for (std::uint32_t i = 0U; i < scene.particles.size(); ++i)
    {
        Write(seed, kP + i * 32U, scene.particles[i]);
        Write(seed, kStart + i * 32U, scene.particles[i]);
        Write(seed, kSnapshot + i * 32U, scene.particles[i]);
        Write(seed, kKeys + i * 8U, std::array<std::uint32_t, 2U>{0U, i});
    }
    for (std::uint32_t i = 0U; i < scene.constraints.size(); ++i)
    {
        Write(seed, kLambda + i * 4U, 0.0F);
        Write(seed, kActive + i * 4U, 1U);
    }
    for (std::uint32_t i = 0U; i < scene.triangles.size(); ++i)
    {
        Write(seed, kFaces + i * 4U, 1U);
    }
    for (std::uint32_t i = 0U; i < scene.tetrahedra.size(); ++i)
    {
        Write(seed, kTets + i * 4U, 1U);
    }
    Write(seed, kMetrics, Metrics{});
    for (std::uint32_t offset = kStatus; offset < kTail; offset += 4U)
    {
        Write(seed, offset, 0U);
    }
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
        return std::unexpected(lgp::framework::MakeHResultError("Cloth root serialization", hr));
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root{};
    hr = device.CreateRootSignature(0U, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
    if (FAILED(hr))
    {
        return std::unexpected(lgp::framework::MakeHResultError("Cloth root", hr));
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
        return std::unexpected(lgp::framework::MakeHResultError("Cloth compute pipeline", hr));
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
    auto &blend = description.BlendState.RenderTarget[0];
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
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
        return std::unexpected(lgp::framework::MakeHResultError("Cloth diagnostic pipeline", hr));
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
        return std::unexpected(lgp::framework::MakeHResultError("Cloth readback Map", hr));
    }
    auto const *bytes = static_cast<std::byte const *>(mapped);
    FrameReadback result{};
    result.frameSlot = frameSlot;
    auto const &c = slot.constants;
    result.constants = c;
    auto &s = result.state;
    auto const error = Read<std::uint32_t>(bytes, kStatus);
    if (error != 0U)
    {
        result.error = static_cast<Error>(error);
    }
    for (std::uint32_t i = 0U; i < c.particles; ++i)
    {
        s.particles.push_back(Read<Particle>(bytes, kP + i * 32U));
    }
    for (std::uint32_t i = 0U; i < c.constraints; ++i)
    {
        s.lambdas.push_back(Read<float>(bytes, kLambda + i * 4U));
        s.active.push_back(Read<std::uint32_t>(bytes, kActive + i * 4U));
    }
    for (std::uint32_t i = 0U; i < c.triangles; ++i)
    {
        s.faces.push_back(Read<std::uint32_t>(bytes, kFaces + i * 4U));
    }
    for (std::uint32_t i = 0U; i < c.tetrahedra; ++i)
    {
        s.tetrahedra.push_back(Read<std::uint32_t>(bytes, kTets + i * 4U));
    }
    s.metrics = Read<Metrics>(bytes, kMetrics);
    s.ticks = Read<std::uint32_t>(bytes, kStatus + 4U);
    std::array<std::array<std::uint32_t, 3U>, 8U> const regions{{{kP, c.particles * 32U, kStart},
                                                                 {kStart, c.particles * 32U, kSnapshot},
                                                                 {kSnapshot, c.particles * 32U, kLambda},
                                                                 {kLambda, c.constraints * 4U, kActive},
                                                                 {kActive, c.constraints * 4U, kFaces},
                                                                 {kFaces, c.triangles * 4U, kTets},
                                                                 {kTets, c.tetrahedra * 4U, kKeys},
                                                                 {kKeys, c.particles * 8U, kMetrics}}};
    result.guardsIntact = true;
    for (auto const &region : regions)
    {
        for (std::uint32_t offset = region[0] + region[1]; offset < region[2]; offset += 4U)
        {
            result.guardsIntact &= Read<std::uint32_t>(bytes, offset) == kGuard;
        }
    }
    for (std::uint32_t offset = kTail; offset < kArenaBytes; offset += 4U)
    {
        result.guardsIntact &= Read<std::uint32_t>(bytes, offset) == kGuard;
    }
    if (!result.guardsIntact)
    {
        result.error = Error::Guard;
    }
    D3D12_RANGE const written{0U, 0U};
    slot.readback.resource()->Unmap(0U, &written);
    return result;
}
} // namespace ch41::cloth::gpu
