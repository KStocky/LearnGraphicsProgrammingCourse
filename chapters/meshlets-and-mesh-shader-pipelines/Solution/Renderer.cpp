#include "Renderer.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace ch22::meshlets::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kDescriptorCount = 6U;
inline constexpr UINT kClassicVerticesDescriptor = 0U;
inline constexpr UINT kMeshSrvTableBase = 1U;
inline constexpr UINT kStatsUavDescriptor = 5U;

// Each pipeline-state subobject must begin on a pointer-sized boundary, so the
// alignment specifier is intentional. MSVC's C4324 padding note is not actionable.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
template <typename Payload, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type> struct alignas(void *) StreamSubobject final
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{Type};
    Payload payload{};
};

struct MeshPipelineStream final
{
    StreamSubobject<ID3D12RootSignature *, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> rootSignature;
    StreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS> amplification;
    StreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> mesh;
    StreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> pixel;
    StreamSubobject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer;
    StreamSubobject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
    StreamSubobject<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depthStencil;
    StreamSubobject<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY> topology;
    StreamSubobject<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> renderTargets;
    StreamSubobject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sampleDesc;
    StreamSubobject<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> sampleMask;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "MeshletLab.hlsl";
}

[[nodiscard]] std::filesystem::path CommonShaderDirectory()
{
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "Common";
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    return blob == nullptr ? std::string{}
                           : std::string{static_cast<char const *>(blob->GetBufferPointer()), blob->GetBufferSize()};
}

[[nodiscard]] lgp::framework::Status SerializeAndCreate(ID3D12Device10 &device,
                                                        D3D12_ROOT_SIGNATURE_DESC const &description,
                                                        ComPtr<ID3D12RootSignature> &rootSignature)
{
    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};
    HRESULT const serialize =
        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, serialized.ReleaseAndGetAddressOf(),
                                    errors.ReleaseAndGetAddressOf());
    if (FAILED(serialize))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serialize, BlobText(errors.Get())));
    }
    HRESULT const create = device.CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                      IID_PPV_ARGS(rootSignature.ReleaseAndGetAddressOf()));
    if (FAILED(create))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", create,
                                                                "Failed to create a Chapter 22 root signature."));
    }
    return {};
}

[[nodiscard]] D3D12_RASTERIZER_DESC SceneRasterizer() noexcept
{
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    return rasterizer;
}

[[nodiscard]] D3D12_DEPTH_STENCIL_DESC SceneDepthStencil() noexcept
{
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    return depth;
}

[[nodiscard]] D3D12_BLEND_DESC SceneBlend() noexcept
{
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return blend;
}

} // namespace

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    return headless_ && headlessConfiguration_.has_value() ? *headlessConfiguration_ : gpu::LabConfiguration{};
}

gpu::ExecutedPath Renderer::ResolvePath() const noexcept
{
    if (ActiveConfiguration().request != gpu::MeshPathRequest::Mesh)
    {
        return gpu::ExecutedPath::Classic;
    }
    return capabilities_.supported ? gpu::ExecutedPath::MeshShader : gpu::ExecutedPath::MeshUnsupported;
}

lgp::framework::Status Renderer::CreateShaders()
{
    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    lgp::framework::ShaderCompiler compiler = std::move(*compilerResult);
    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = ShaderPath();
    options.includeDirectories = {CommonShaderDirectory()};
    if (auto status = gpu::CompileShader(compiler, options, L"ClassicVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"ScenePS", L"ps_6_0", pixelShader_); !status)
    {
        return status;
    }
    if (capabilities_.supported)
    {
        if (auto status = gpu::CompileShader(compiler, options, L"MeshletAS", L"as_6_5", amplificationShader_); !status)
        {
            return status;
        }
        if (auto status = gpu::CompileShader(compiler, options, L"MeshletMS", L"ms_6_5", meshShader_); !status)
        {
            return status;
        }
    }
    return {};
}

lgp::framework::Status Renderer::CreateClassicPipeline()
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1U;
    range.BaseShaderRegister = 0U;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1U;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 1U;
    description.pParameters = &parameter;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    if (auto status = SerializeAndCreate(*deviceResources_->device(), description, classicRootSignature_); !status)
    {
        return status;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = classicRootSignature_.Get();
    pipelineDescription.VS = vertexShader_.Bytecode();
    pipelineDescription.PS = pixelShader_.Bytecode();
    pipelineDescription.BlendState = SceneBlend();
    pipelineDescription.SampleMask = UINT_MAX;
    pipelineDescription.RasterizerState = SceneRasterizer();
    pipelineDescription.DepthStencilState = SceneDepthStencil();
    pipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDescription.NumRenderTargets = 1U;
    pipelineDescription.RTVFormats[0] = deviceResources_->back_buffer_format();
    pipelineDescription.SampleDesc.Count = 1U;
    HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
        &pipelineDescription, IID_PPV_ARGS(classicPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create the Chapter 22 classic pipeline."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateMeshPipeline()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 4U;
    srvRange.BaseShaderRegister = 1U;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1U;
    uavRange.BaseShaderRegister = 0U;
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0U;
    parameters[0].Constants.Num32BitValues = 1U;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1U;
    parameters[2].DescriptorTable.pDescriptorRanges = &uavRange;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(std::size(parameters));
    description.pParameters = parameters;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (auto status = SerializeAndCreate(*deviceResources_->device(), description, meshRootSignature_); !status)
    {
        return status;
    }

    MeshPipelineStream stream{};
    stream.rootSignature.payload = meshRootSignature_.Get();
    stream.amplification.payload = amplificationShader_.Bytecode();
    stream.mesh.payload = meshShader_.Bytecode();
    stream.pixel.payload = pixelShader_.Bytecode();
    stream.rasterizer.payload = SceneRasterizer();
    stream.blend.payload = SceneBlend();
    stream.depthStencil.payload = SceneDepthStencil();
    stream.topology.payload = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    stream.renderTargets.payload.NumRenderTargets = 1U;
    stream.renderTargets.payload.RTFormats[0] = deviceResources_->back_buffer_format();
    stream.sampleDesc.payload.Count = 1U;
    stream.sampleMask.payload = UINT_MAX;

    D3D12_PIPELINE_STATE_STREAM_DESC streamDescription{};
    streamDescription.SizeInBytes = sizeof(stream);
    streamDescription.pPipelineStateSubobjectStream = &stream;
    HRESULT const result = deviceResources_->device()->CreatePipelineState(
        &streamDescription, IID_PPV_ARGS(meshPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreatePipelineState", result,
                                                                "Failed to create the Chapter 22 mesh pipeline."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateFrameSlots()
{
    ID3D12Device10 &device = *deviceResources_->device();
    std::uint64_t const classicVertexBytes = scene_.classicVertices.size() * sizeof(gpu::SceneVertex);
    std::uint64_t const classicIndexBytes = scene_.classicIndices.size() * sizeof(std::uint32_t);
    std::uint64_t const positionBytes = scene_.meshPositions.size() * sizeof(gpu::PositionVertex);
    std::uint64_t const descriptorBytes = scene_.meshletDescriptors.size() * sizeof(gpu::MeshletDescriptor);
    std::uint64_t const remapBytes = scene_.meshletVertices.size() * sizeof(std::uint32_t);
    std::uint64_t const primitiveBytes = scene_.meshletPrimitives.size() * sizeof(std::uint32_t);
    std::array<std::uint32_t, gpu::kStatsDwordCount> const zeros{};

    frameSlots_.resize(deviceResources_->back_buffer_count());
    for (FrameSlot &slot : frameSlots_)
    {
        auto classicVertices = gpu::CreateBuffer(device, classicVertexBytes, D3D12_HEAP_TYPE_UPLOAD,
                                                 D3D12_RESOURCE_FLAG_NONE, L"Ch22 Classic Vertices", true);
        auto classicIndices = gpu::CreateBuffer(device, classicIndexBytes, D3D12_HEAP_TYPE_UPLOAD,
                                                D3D12_RESOURCE_FLAG_NONE, L"Ch22 Classic Indices", true);
        auto positions = gpu::CreateBuffer(device, positionBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                           L"Ch22 Positions", true);
        auto descriptors = gpu::CreateBuffer(device, descriptorBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                             L"Ch22 Meshlet Descriptors", true);
        auto meshletVertices = gpu::CreateBuffer(device, remapBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                                 L"Ch22 Meshlet Vertices", true);
        auto meshletPrimitives = gpu::CreateBuffer(device, primitiveBytes, D3D12_HEAP_TYPE_UPLOAD,
                                                   D3D12_RESOURCE_FLAG_NONE, L"Ch22 Meshlet Primitives", true);
        auto stats = gpu::CreateBuffer(device, gpu::kStatsBytes, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch22 Stats");
        auto statsZero = gpu::CreateBuffer(device, gpu::kStatsBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                           L"Ch22 Stats Zero", true);
        auto statsReadback = gpu::CreateBuffer(device, gpu::kStatsBytes, D3D12_HEAP_TYPE_READBACK,
                                               D3D12_RESOURCE_FLAG_NONE, L"Ch22 Stats Readback", true);
        auto descriptorAllocation = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorCount);
        if (!classicVertices || !classicIndices || !positions || !descriptors || !meshletVertices ||
            !meshletPrimitives || !stats || !statsZero || !statsReadback || !descriptorAllocation)
        {
            return std::unexpected(
                lgp::framework::MakeError("CreateFrameSlots", "Failed to allocate Chapter 22 Solution resources."));
        }
        slot.classicVertices = std::move(*classicVertices);
        slot.classicIndices = std::move(*classicIndices);
        slot.positions = std::move(*positions);
        slot.meshletDescriptors = std::move(*descriptors);
        slot.meshletVertices = std::move(*meshletVertices);
        slot.meshletPrimitives = std::move(*meshletPrimitives);
        slot.stats = std::move(*stats);
        slot.statsZero = std::move(*statsZero);
        slot.statsReadback = std::move(*statsReadback);
        slot.descriptors = *descriptorAllocation;
        if (auto status = gpu::WriteBuffer(slot.statsZero, std::span<std::uint32_t const>{zeros}); !status)
        {
            return status;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC classicSrv{};
        classicSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        classicSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        classicSrv.Buffer.NumElements = static_cast<UINT>(scene_.classicVertices.size());
        classicSrv.Buffer.StructureByteStride = sizeof(gpu::SceneVertex);
        device.CreateShaderResourceView(slot.classicVertices.Get(), &classicSrv,
                                        slot.descriptors.CpuHandle(kClassicVerticesDescriptor));

        D3D12_SHADER_RESOURCE_VIEW_DESC positionSrv = classicSrv;
        positionSrv.Buffer.NumElements = static_cast<UINT>(scene_.meshPositions.size());
        positionSrv.Buffer.StructureByteStride = sizeof(gpu::PositionVertex);
        device.CreateShaderResourceView(slot.positions.Get(), &positionSrv,
                                        slot.descriptors.CpuHandle(kMeshSrvTableBase));

        D3D12_SHADER_RESOURCE_VIEW_DESC descriptorSrv = classicSrv;
        descriptorSrv.Buffer.NumElements = static_cast<UINT>(scene_.meshletDescriptors.size());
        descriptorSrv.Buffer.StructureByteStride = sizeof(gpu::MeshletDescriptor);
        device.CreateShaderResourceView(slot.meshletDescriptors.Get(), &descriptorSrv,
                                        slot.descriptors.CpuHandle(kMeshSrvTableBase + 1U));

        D3D12_SHADER_RESOURCE_VIEW_DESC remapSrv = classicSrv;
        remapSrv.Buffer.NumElements = static_cast<UINT>(scene_.meshletVertices.size());
        remapSrv.Buffer.StructureByteStride = sizeof(std::uint32_t);
        device.CreateShaderResourceView(slot.meshletVertices.Get(), &remapSrv,
                                        slot.descriptors.CpuHandle(kMeshSrvTableBase + 2U));

        D3D12_SHADER_RESOURCE_VIEW_DESC primitiveSrv = classicSrv;
        primitiveSrv.Buffer.NumElements = static_cast<UINT>(scene_.meshletPrimitives.size());
        primitiveSrv.Buffer.StructureByteStride = sizeof(std::uint32_t);
        device.CreateShaderResourceView(slot.meshletPrimitives.Get(), &primitiveSrv,
                                        slot.descriptors.CpuHandle(kMeshSrvTableBase + 3U));

        D3D12_UNORDERED_ACCESS_VIEW_DESC statsUav{};
        statsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        statsUav.Format = DXGI_FORMAT_R32_TYPELESS;
        statsUav.Buffer.NumElements = gpu::kStatsDwordCount;
        statsUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device.CreateUnorderedAccessView(slot.stats.Get(), nullptr, &statsUav,
                                         slot.descriptors.CpuHandle(kStatsUavDescriptor));
    }
    return {};
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    capabilities_ = gpu::QueryMeshShaderCapabilities(*deviceResources_->device());

    auto scene = gpu::BuildGpuScene();
    if (!scene)
    {
        return std::unexpected(std::move(scene.error()));
    }
    scene_ = std::move(*scene);

    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreateClassicPipeline(); !status)
    {
        return status;
    }
    if (capabilities_.supported)
    {
        if (auto status = CreateMeshPipeline(); !status)
        {
            return status;
        }
    }
    return CreateFrameSlots();
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &deviceResources,
                                          lgp::framework::Extent2D drawableSize)
{
    (void)deviceResources;
    (void)drawableSize;
    return {};
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &context)
{
    (void)context;
    auto scene = gpu::BuildGpuScene();
    if (!scene)
    {
        return std::unexpected(std::move(scene.error()));
    }
    scene_ = std::move(*scene);
    return {};
}

void Renderer::RecordClassic(lgp::framework::FrameContext const &frameContext, FrameSlot &slot)
{
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    std::vector<D3D12_TEXTURE_BARRIER> barriers{gpu::MakeTextureBarrier(
        *frameContext.renderTarget, gpu::FrameStartState(frameContext), gpu::RenderTargetState())};
    gpu::SubmitTextureBarriers(commandList, barriers);
    float const clear[]{0.01F, 0.015F, 0.025F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clear, 0U, nullptr);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_INDEX_BUFFER_VIEW const indexView{slot.classicIndices.Get()->GetGPUVirtualAddress(),
                                            static_cast<UINT>(slot.classicIndices.size_in_bytes()),
                                            DXGI_FORMAT_R32_UINT};
    commandList.IASetIndexBuffer(&indexView);
    commandList.SetGraphicsRootSignature(classicRootSignature_.Get());
    commandList.SetPipelineState(classicPipeline_.Get());
    commandList.SetGraphicsRootDescriptorTable(0U, slot.descriptors.GpuHandle(kClassicVerticesDescriptor));
    commandList.DrawIndexedInstanced(static_cast<UINT>(scene_.classicIndices.size()), 1U, 0U, 0, 0U);
    barriers = {gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(),
                                        gpu::FrameEndState(frameContext))};
    gpu::SubmitTextureBarriers(commandList, barriers);
}

lgp::framework::Status Renderer::RecordMesh(lgp::framework::FrameContext const &frameContext, FrameSlot &slot)
{
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    gpu::BufferBarrierState const before = slot.statsStateInitialized ? gpu::CopySourceState() : gpu::NoAccessState();
    std::vector<D3D12_BUFFER_BARRIER> bufferBarriers{
        gpu::MakeBufferBarrier(*slot.stats.Get(), before, gpu::CopyDestState())};
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);
    commandList.CopyBufferRegion(slot.stats.Get(), 0U, slot.statsZero.Get(), 0U, gpu::kStatsBytes);
    bufferBarriers = {gpu::MakeBufferBarrier(*slot.stats.Get(), gpu::CopyDestState(), gpu::MeshUnorderedAccessState())};
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);

    std::vector<D3D12_TEXTURE_BARRIER> textureBarriers{gpu::MakeTextureBarrier(
        *frameContext.renderTarget, gpu::FrameStartState(frameContext), gpu::RenderTargetState())};
    gpu::SubmitTextureBarriers(commandList, textureBarriers);
    float const clear[]{0.01F, 0.015F, 0.025F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clear, 0U, nullptr);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.SetGraphicsRootSignature(meshRootSignature_.Get());
    std::uint32_t const meshletCount = scene_.meshletCount;
    commandList.SetGraphicsRoot32BitConstants(0U, 1U, &meshletCount, 0U);
    commandList.SetGraphicsRootDescriptorTable(1U, slot.descriptors.GpuHandle(kMeshSrvTableBase));
    commandList.SetGraphicsRootDescriptorTable(2U, slot.descriptors.GpuHandle(kStatsUavDescriptor));
    commandList.SetPipelineState(meshPipeline_.Get());
    commandList.DispatchMesh(1U, 1U, 1U);
    textureBarriers = {gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(),
                                               gpu::FrameEndState(frameContext))};
    gpu::SubmitTextureBarriers(commandList, textureBarriers);

    bufferBarriers = {
        gpu::MakeBufferBarrier(*slot.stats.Get(), gpu::MeshUnorderedAccessState(), gpu::CopySourceState())};
    gpu::SubmitBufferBarriers(commandList, bufferBarriers);
    commandList.CopyBufferRegion(slot.statsReadback.Get(), 0U, slot.stats.Get(), 0U, gpu::kStatsBytes);
    slot.statsStateInitialized = true;
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    FrameSlot &slot = frameSlots_.at(frameContext.frameSlot);
    if (auto status = gpu::WriteBuffer(slot.classicVertices, std::span<gpu::SceneVertex const>{scene_.classicVertices});
        !status)
    {
        return status;
    }
    if (auto status = gpu::WriteBuffer(slot.classicIndices, std::span<std::uint32_t const>{scene_.classicIndices});
        !status)
    {
        return status;
    }
    if (auto status = gpu::WriteBuffer(slot.positions, std::span<gpu::PositionVertex const>{scene_.meshPositions});
        !status)
    {
        return status;
    }
    if (auto status = gpu::WriteBuffer(slot.meshletDescriptors,
                                       std::span<gpu::MeshletDescriptor const>{scene_.meshletDescriptors});
        !status)
    {
        return status;
    }
    if (auto status = gpu::WriteBuffer(slot.meshletVertices, std::span<std::uint32_t const>{scene_.meshletVertices});
        !status)
    {
        return status;
    }
    if (auto status =
            gpu::WriteBuffer(slot.meshletPrimitives, std::span<std::uint32_t const>{scene_.meshletPrimitives});
        !status)
    {
        return status;
    }

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);

    gpu::ExecutedPath const path = ResolvePath();
    if (path == gpu::ExecutedPath::MeshShader)
    {
        if (auto status = RecordMesh(frameContext, slot); !status)
        {
            return status;
        }
    }
    else
    {
        RecordClassic(frameContext, slot);
    }

    lastEvidence_ = {};
    lastEvidence_.executedPath = path;
    lastEvidence_.capabilities = capabilities_;
    lastEvidence_.meshletCount = scene_.meshletCount;
    lastFrameSlot_ = frameContext.frameSlot;
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    for (FrameSlot &slot : frameSlots_)
    {
        if (slot.descriptors)
        {
            deviceResources.shader_visible_cbv_srv_uav_heap().Free(slot.descriptors);
        }
    }
    frameSlots_.clear();
    meshPipeline_.Reset();
    classicPipeline_.Reset();
    meshRootSignature_.Reset();
    classicRootSignature_.Reset();
    deviceResources_ = nullptr;
}

void Renderer::ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

std::expected<gpu::MeshEvidence, lgp::framework::Error> Renderer::ReadBackEvidence()
{
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }
    gpu::MeshEvidence evidence = lastEvidence_;
    if (evidence.executedPath == gpu::ExecutedPath::MeshShader)
    {
        FrameSlot const &slot = frameSlots_.at(lastFrameSlot_);
        auto const *stats = reinterpret_cast<std::uint32_t const *>(slot.statsReadback.mapped_data());
        if (stats == nullptr)
        {
            return std::unexpected(
                lgp::framework::MakeError("ReadBackEvidence", "Chapter 22 stats readback is not mapped."));
        }
        evidence.dispatchedMeshletGroups = stats[0];
        evidence.emittedVertices = stats[1];
        evidence.emittedPrimitives = stats[2];
    }
    return evidence;
}

gpu::GpuScene const &Renderer::LastScene() const noexcept
{
    return scene_;
}

gpu::MeshShaderCapabilities Renderer::Capabilities() const noexcept
{
    return capabilities_;
}

} // namespace ch22::meshlets::solution
