#include "GpuLabSupport.hpp"

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace ch10::pass_scheduling::gpu
{
namespace
{

using namespace ch08::frame_graph;

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] std::string DescribeGraphDiagnostics(std::vector<PassGraphDiagnostic> const &diagnostics)
{
    std::string message{};
    for (PassGraphDiagnostic const &diagnostic : diagnostics)
    {
        if (!message.empty())
        {
            message += '\n';
        }
        message += diagnostic.message;
    }
    return message;
}

[[nodiscard]] std::string DescribeScheduleDiagnostics(std::vector<ScheduleDiagnostic> const &diagnostics)
{
    std::string message{};
    for (ScheduleDiagnostic const &diagnostic : diagnostics)
    {
        if (!message.empty())
        {
            message += '\n';
        }
        message += diagnostic.message;
    }
    return message;
}

[[nodiscard]] lgp::framework::Status CompileShader(lgp::framework::ShaderCompiler &compiler,
                                                   lgp::framework::ShaderCompileOptions &options,
                                                   wchar_t const *entryPoint, wchar_t const *targetProfile,
                                                   lgp::framework::CompiledShader &shader)
{
    options.entryPoint = entryPoint;
    options.targetProfile = targetProfile;
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto result = compiler.Compile(options);
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    shader = std::move(*result);
    return {};
}

[[nodiscard]] lgp::framework::Status CreateGraphicsPipeline(ID3D12Device10 &device,
                                                            D3D12_GRAPHICS_PIPELINE_STATE_DESC &description,
                                                            lgp::framework::CompiledShader const &pixelShader,
                                                            DXGI_FORMAT format, std::string_view name,
                                                            Microsoft::WRL::ComPtr<ID3D12PipelineState> &pipeline)
{
    description.PS = pixelShader.Bytecode();
    description.RTVFormats[0] = format;
    HRESULT const result =
        device.CreateGraphicsPipelineState(&description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                             "Failed to create the Chapter 10 " + std::string{name} + " pipeline."));
    }
    return {};
}

[[nodiscard]] lgp::framework::Status CreateComputePipeline(ID3D12Device10 &device, ID3D12RootSignature &rootSignature,
                                                           lgp::framework::CompiledShader const &shader,
                                                           std::string_view name,
                                                           Microsoft::WRL::ComPtr<ID3D12PipelineState> &pipeline)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &rootSignature;
    description.CS = shader.Bytecode();
    HRESULT const result =
        device.CreateComputePipelineState(&description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState", result,
                                             "Failed to create the Chapter 10 " + std::string{name} + " pipeline."));
    }
    return {};
}

[[nodiscard]] std::uint64_t QueryAllocationSize(ID3D12Device10 &device, D3D12_RESOURCE_DESC1 const &description)
{
    D3D12_RESOURCE_ALLOCATION_INFO1 detailed{};
    D3D12_RESOURCE_ALLOCATION_INFO const allocation =
        device.GetResourceAllocationInfo2(0U, 1U, &description, &detailed);
    return allocation.SizeInBytes;
}

} // namespace

lgp::framework::TextureBarrierState UndefinedState() noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_UNDEFINED};
}

lgp::framework::TextureBarrierState ComputeUnorderedAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
            D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS};
}

lgp::framework::TextureBarrierState ComputeShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
            D3D12_BARRIER_LAYOUT_SHADER_RESOURCE};
}

lgp::framework::TextureBarrierState ComputeProducedSharedShaderResourceState() noexcept
{
    return ComputeShaderResourceState();
}

lgp::framework::TextureBarrierState PixelShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
            D3D12_BARRIER_LAYOUT_SHADER_RESOURCE};
}

lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
}

lgp::framework::TextureBarrierState FrameStartState(bool headless) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
            headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT};
}

lgp::framework::TextureBarrierState FrameEndState(bool headless) noexcept
{
    return FrameStartState(headless);
}

D3D12_RESOURCE_DESC1 ComputeTextureDescription(lgp::framework::Extent2D size) noexcept
{
    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = size.width;
    description.Height = size.height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = kTransientFormat;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return description;
}

D3D12_RESOURCE_DESC1 GraphicsTextureDescription(lgp::framework::Extent2D size) noexcept
{
    D3D12_RESOURCE_DESC1 description = ComputeTextureDescription(size);
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return description;
}

D3D12_TEXTURE_BARRIER MakeTextureBarrier(ID3D12Resource &resource, lgp::framework::TextureBarrierState before,
                                         lgp::framework::TextureBarrierState after,
                                         D3D12_TEXTURE_BARRIER_FLAGS flags) noexcept
{
    D3D12_TEXTURE_BARRIER barrier{};
    barrier.SyncBefore = before.sync;
    barrier.SyncAfter = after.sync;
    barrier.AccessBefore = before.access;
    barrier.AccessAfter = after.access;
    barrier.LayoutBefore = before.layout;
    barrier.LayoutAfter = after.layout;
    barrier.pResource = &resource;
    barrier.Subresources.IndexOrFirstMipLevel = UINT32_MAX;
    barrier.Flags = flags;
    return barrier;
}

void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers)
{
    if (barriers.empty())
    {
        return;
    }
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = static_cast<UINT>(barriers.size());
    group.pTextureBarriers = barriers.data();
    commandList.Barrier(1U, &group);
}

std::expected<LabGraph, lgp::framework::Error> CompileLabGraph(bool headless)
{
    PassGraph graph{};
    LabGraph lab{};
    lab.computeScratch = graph.AddTransientTexture("compute-scratch");
    lab.computeFinal = graph.AddTransientTexture("compute-final");
    lab.graphicsScratch = graph.AddTransientTexture("graphics-scratch");
    lab.graphicsFinal = graph.AddTransientTexture("graphics-final");
    lab.frameTarget = graph.AddImportedTexture("frame-target", FrameStartState(headless));

    lab.computeGenerate = graph.AddPass("compute-generate");
    lab.computeCollapse = graph.AddPass("compute-collapse");
    lab.graphicsGeometry = graph.AddPass("graphics-geometry");
    lab.graphicsResolve = graph.AddPass("graphics-resolve");
    lab.composite = graph.AddPass("composite");

    graph.DeclareTextureWrite(lab.computeGenerate, lab.computeScratch, ComputeUnorderedAccessState());
    graph.DeclareTextureRead(lab.computeCollapse, lab.computeScratch, ComputeShaderResourceState());
    graph.DeclareTextureWrite(lab.computeCollapse, lab.computeFinal, ComputeUnorderedAccessState());
    graph.DeclareTextureWrite(lab.graphicsGeometry, lab.graphicsScratch, RenderTargetState());
    graph.DeclareTextureRead(lab.graphicsResolve, lab.graphicsScratch, PixelShaderResourceState());
    graph.DeclareTextureWrite(lab.graphicsResolve, lab.graphicsFinal, RenderTargetState());
    graph.DeclareTextureRead(lab.composite, lab.computeFinal, PixelShaderResourceState());
    graph.DeclareTextureRead(lab.composite, lab.graphicsFinal, PixelShaderResourceState());
    graph.DeclareTextureWrite(lab.composite, lab.frameTarget, RenderTargetState());

    PassGraphCompileResult result = graph.Compile();
    if (!result)
    {
        return std::unexpected(
            lgp::framework::MakeError("PassGraph::Compile", DescribeGraphDiagnostics(result.error())));
    }
    lab.graph = std::move(*result);
    return lab;
}

std::expected<PhysicalTextureByteSizes, lgp::framework::Error> QueryPhysicalTextureByteSizes(
    ID3D12Device10 &device, lgp::framework::Extent2D size)
{
    PhysicalTextureByteSizes sizes{};
    D3D12_RESOURCE_DESC1 const computeDescription = ComputeTextureDescription(size);
    D3D12_RESOURCE_DESC1 const graphicsDescription = GraphicsTextureDescription(size);
    std::uint64_t const computeSize = QueryAllocationSize(device, computeDescription);
    std::uint64_t const graphicsSize = QueryAllocationSize(device, graphicsDescription);
    if (computeSize == 0U || computeSize == UINT64_MAX || graphicsSize == 0U || graphicsSize == UINT64_MAX)
    {
        return std::unexpected(
            lgp::framework::MakeError("ID3D12Device10::GetResourceAllocationInfo2",
                                      "The adapter returned invalid Chapter 10 transient texture allocation sizes."));
    }
    sizes.byTextureIndex = {computeSize, computeSize, graphicsSize, graphicsSize};
    return sizes;
}

std::expected<ScheduleComparison, lgp::framework::Error> CompileScheduleComparison(
    LabGraph const &graph, PhysicalTextureByteSizes const &byteSizes)
{
    std::vector<PassSchedulingMetadata> const metadata{
        {graph.computeGenerate, "compute-generate", 4U, PassCapability::DirectOrCompute},
        {graph.computeCollapse, "compute-collapse", 3U, PassCapability::DirectOrCompute},
        {graph.graphicsGeometry, "graphics-geometry", 5U, PassCapability::DirectOnly},
        {graph.graphicsResolve, "graphics-resolve", 2U, PassCapability::DirectOnly},
        {graph.composite, "composite", 2U, PassCapability::DirectOnly},
    };
    std::vector<TransientTextureByteSize> const sizes{
        {graph.computeScratch, byteSizes.byTextureIndex[static_cast<std::size_t>(LabTextureIndex::ComputeScratch)]},
        {graph.computeFinal, byteSizes.byTextureIndex[static_cast<std::size_t>(LabTextureIndex::ComputeFinal)]},
        {graph.graphicsScratch, byteSizes.byTextureIndex[static_cast<std::size_t>(LabTextureIndex::GraphicsScratch)]},
        {graph.graphicsFinal, byteSizes.byTextureIndex[static_cast<std::size_t>(LabTextureIndex::GraphicsFinal)]},
    };
    ScheduleCandidate const serial{{
                                       graph.computeGenerate,
                                       graph.computeCollapse,
                                       graph.graphicsGeometry,
                                       graph.graphicsResolve,
                                       graph.composite,
                                   },
                                   {}};
    ScheduleCandidate const async{{graph.graphicsGeometry, graph.graphicsResolve, graph.composite},
                                  {graph.computeGenerate, graph.computeCollapse}};
    ScheduleCompileResult serialResult = CompileSchedule(graph.graph, metadata, serial, sizes);
    if (!serialResult)
    {
        return std::unexpected(
            lgp::framework::MakeError("CompileSchedule(serial)", DescribeScheduleDiagnostics(serialResult.error())));
    }
    ScheduleCompileResult asyncResult = CompileSchedule(graph.graph, metadata, async, sizes);
    if (!asyncResult)
    {
        return std::unexpected(
            lgp::framework::MakeError("CompileSchedule(async)", DescribeScheduleDiagnostics(asyncResult.error())));
    }
    if (!serialResult->peakTransientBytes.has_value() || !asyncResult->peakTransientBytes.has_value() ||
        asyncResult->abstractMakespanTicks >= serialResult->abstractMakespanTicks ||
        *asyncResult->peakTransientBytes <= *serialResult->peakTransientBytes)
    {
        return std::unexpected(lgp::framework::MakeError(
            "CompileScheduleComparison",
            "The concrete Chapter 10 candidates must expose a shorter async abstract makespan and a higher complete "
            "async transient-byte peak."));
    }
    return ScheduleComparison{std::move(*serialResult), std::move(*asyncResult)};
}

lgp::framework::Status CreateLabShaders(std::filesystem::path const &path, LabShaders &shaders)
{
    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    lgp::framework::ShaderCompiler compiler = std::move(*compilerResult);
    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = path;
    options.includeDirectories = {path.parent_path()};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif
    if (auto status = CompileShader(compiler, options, L"FullscreenVS", L"vs_6_0", shaders.fullscreenVertex); !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"ComputeGenerateCS", L"cs_6_0", shaders.computeGenerate);
        !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"ComputeCollapseCS", L"cs_6_0", shaders.computeCollapse);
        !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"GraphicsGeometryPS", L"ps_6_0", shaders.graphicsGeometry);
        !status)
    {
        return status;
    }
    if (auto status = CompileShader(compiler, options, L"GraphicsResolvePS", L"ps_6_0", shaders.graphicsResolve);
        !status)
    {
        return status;
    }
    return CompileShader(compiler, options, L"CompositePS", L"ps_6_0", shaders.composite);
}

lgp::framework::Status CreateLabRootSignature(ID3D12Device10 &device,
                                              Microsoft::WRL::ComPtr<ID3D12RootSignature> &rootSignature)
{
    std::array<D3D12_DESCRIPTOR_RANGE, 3U> ranges{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1U;
    ranges[0].BaseShaderRegister = 0U;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1U;
    ranges[1].BaseShaderRegister = 1U;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[2].NumDescriptors = 1U;
    ranges[2].BaseShaderRegister = 0U;

    std::array<D3D12_ROOT_PARAMETER, 3U> parameters{};
    for (std::size_t index = 0U; index < parameters.size(); ++index)
    {
        parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[index].DescriptorTable.NumDescriptorRanges = 1U;
        parameters[index].DescriptorTable.pDescriptorRanges = &ranges[index];
        parameters[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized{};
    Microsoft::WRL::ComPtr<ID3DBlob> errors{};
    HRESULT const serializeResult =
        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, serialized.ReleaseAndGetAddressOf(),
                                    errors.ReleaseAndGetAddressOf());
    if (FAILED(serializeResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult, BlobText(errors.Get())));
    }
    HRESULT const createResult =
        device.CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                   IID_PPV_ARGS(rootSignature.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                                                "Failed to create the Chapter 10 root signature."));
    }
    return {};
}

lgp::framework::Status CreateLabPipelines(ID3D12Device10 &device, DXGI_FORMAT frameFormat,
                                          ID3D12RootSignature &rootSignature, LabShaders const &shaders,
                                          LabPipelines &pipelines)
{
    if (auto status = CreateComputePipeline(device, rootSignature, shaders.computeGenerate, "compute-generate",
                                            pipelines.computeGenerate);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(device, rootSignature, shaders.computeCollapse, "compute-collapse",
                                            pipelines.computeCollapse);
        !status)
    {
        return status;
    }

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &rootSignature;
    description.VS = shaders.fullscreenVertex.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.SampleDesc.Count = 1U;

    if (auto status = CreateGraphicsPipeline(device, description, shaders.graphicsGeometry, kTransientFormat,
                                             "graphics-geometry", pipelines.graphicsGeometry);
        !status)
    {
        return status;
    }
    if (auto status = CreateGraphicsPipeline(device, description, shaders.graphicsResolve, kTransientFormat,
                                             "graphics-resolve", pipelines.graphicsResolve);
        !status)
    {
        return status;
    }
    return CreateGraphicsPipeline(device, description, shaders.composite, frameFormat, "composite",
                                  pipelines.composite);
}

} // namespace ch10::pass_scheduling::gpu
