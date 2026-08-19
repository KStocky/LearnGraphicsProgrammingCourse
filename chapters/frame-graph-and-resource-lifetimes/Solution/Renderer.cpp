#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <lgp/framework/barriers.hpp>

namespace ch08::frame_graph::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

enum RootParameter : UINT
{
    HdrTexture = 0U,
};

[[nodiscard]] std::string D3D12BlobToUtf8(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }

    return {
        static_cast<char const *>(blob->GetBufferPointer()),
        static_cast<std::size_t>(blob->GetBufferSize()),
    };
}

[[nodiscard]] std::filesystem::path ResolveShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "PassGraphLab.hlsl";
}

[[nodiscard]] lgp::framework::TextureBarrierState ShaderResourceState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_PIXEL_SHADING,
        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
        D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
    };
}

[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {
        D3D12_BARRIER_SYNC_RENDER_TARGET,
        D3D12_BARRIER_ACCESS_RENDER_TARGET,
        D3D12_BARRIER_LAYOUT_RENDER_TARGET,
    };
}

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frameContext.renderTargetInitialLayout};
}

[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept
{
    return {
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT,
    };
}

[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(ID3D12Resource &resource,
                                                       lgp::framework::TextureBarrierState before,
                                                       lgp::framework::TextureBarrierState after) noexcept
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
    return barrier;
}

[[nodiscard]] std::string DescribeDiagnostics(std::vector<PassGraphDiagnostic> const &diagnostics)
{
    std::string message{};
    for (std::size_t diagnosticIndex = 0U; diagnosticIndex < diagnostics.size(); ++diagnosticIndex)
    {
        if (diagnosticIndex > 0U)
        {
            message += "\n";
        }
        message += diagnostics[diagnosticIndex].message;
    }
    return message;
}

[[nodiscard]] std::string_view DependencyKindLabel(DependencyKind kind) noexcept
{
    switch (kind)
    {
    case DependencyKind::ReadAfterWrite:
        return "RAW";
    case DependencyKind::WriteAfterRead:
        return "WAR";
    case DependencyKind::WriteAfterWrite:
        return "WAW";
    case DependencyKind::Explicit:
        return "Explicit";
    default:
        return "Unknown";
    }
}

[[nodiscard]] std::string_view BarrierSyncLabel(D3D12_BARRIER_SYNC sync) noexcept
{
    switch (sync)
    {
    case D3D12_BARRIER_SYNC_NONE:
        return "None";
    case D3D12_BARRIER_SYNC_PIXEL_SHADING:
        return "PixelShading";
    case D3D12_BARRIER_SYNC_RENDER_TARGET:
        return "RenderTarget";
    default:
        return "Unknown";
    }
}

[[nodiscard]] std::string_view BarrierAccessLabel(D3D12_BARRIER_ACCESS access) noexcept
{
    switch (access)
    {
    case D3D12_BARRIER_ACCESS_NO_ACCESS:
        return "NoAccess";
    case D3D12_BARRIER_ACCESS_SHADER_RESOURCE:
        return "ShaderResource";
    case D3D12_BARRIER_ACCESS_RENDER_TARGET:
        return "RenderTarget";
    default:
        return "Unknown";
    }
}

[[nodiscard]] std::string_view BarrierLayoutLabel(D3D12_BARRIER_LAYOUT layout) noexcept
{
    if (layout == D3D12_BARRIER_LAYOUT_COMMON)
    {
        return "Present/Common";
    }
    if (layout == D3D12_BARRIER_LAYOUT_SHADER_RESOURCE)
    {
        return "ShaderResource";
    }
    if (layout == D3D12_BARRIER_LAYOUT_RENDER_TARGET)
    {
        return "RenderTarget";
    }

    return "Unknown";
}

[[nodiscard]] std::string FormatBarrierState(lgp::framework::TextureBarrierState const &state)
{
    return "sync=" + std::string{BarrierSyncLabel(state.sync)} +
           ", access=" + std::string{BarrierAccessLabel(state.access)} +
           ", layout=" + std::string{BarrierLayoutLabel(state.layout)};
}

[[nodiscard]] std::string FormatLifetimeInterval(ResourceLifetime const &lifetime)
{
    if (!lifetime.firstExecutionIndex.has_value() || !lifetime.lastExecutionIndex.has_value())
    {
        return "unused";
    }

    return "[" + std::to_string(*lifetime.firstExecutionIndex) + ", " + std::to_string(*lifetime.lastExecutionIndex) +
           "]";
}

[[nodiscard]] bool LifetimeUsesPassSlot(ResourceLifetime const &lifetime, std::uint32_t slot) noexcept
{
    return lifetime.firstExecutionIndex.has_value() && lifetime.lastExecutionIndex.has_value() &&
           *lifetime.firstExecutionIndex <= slot && slot <= *lifetime.lastExecutionIndex;
}

[[nodiscard]] std::string_view ResourceName(CompiledPassGraph const &graph, TextureResourceId resourceId) noexcept
{
    if (resourceId.value >= graph.textureResources.size())
    {
        return "Unknown";
    }

    return graph.textureResources[resourceId.value].name;
}

[[nodiscard]] std::string_view PassName(CompiledPassGraph const &graph, PassId passId) noexcept
{
    auto const pass = std::find_if(graph.scheduledPasses.begin(), graph.scheduledPasses.end(),
                                   [passId](CompiledPass const &candidate) { return candidate.id == passId; });
    if (pass != graph.scheduledPasses.end())
    {
        return pass->name;
    }

    return "Unknown";
}

} // namespace

lgp::framework::Status Renderer::CreateShaders()
{
    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    lgp::framework::ShaderCompiler compiler = std::move(*compilerResult);

    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = ResolveShaderPath();
    options.includeDirectories = {options.sourcePath.parent_path()};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif

    options.entryPoint = L"FullscreenVS";
    options.targetProfile = L"vs_6_0";
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto vertexResult = compiler.Compile(options);
    if (!vertexResult)
    {
        return std::unexpected(std::move(vertexResult.error()));
    }
    fullscreenVertexShader_ = std::move(*vertexResult);

    options.entryPoint = L"HdrAnalyticPS";
    options.targetProfile = L"ps_6_0";
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto hdrResult = compiler.Compile(options);
    if (!hdrResult)
    {
        return std::unexpected(std::move(hdrResult.error()));
    }
    hdrPixelShader_ = std::move(*hdrResult);

    options.entryPoint = L"DisplayPS";
    options.targetProfile = L"ps_6_0";
    options.additionalArguments = {L"-E", options.entryPoint, L"-T", options.targetProfile};
    auto displayResult = compiler.Compile(options);
    if (!displayResult)
    {
        return std::unexpected(std::move(displayResult.error()));
    }
    displayPixelShader_ = std::move(*displayResult);
    return {};
}

lgp::framework::Status Renderer::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE descriptorRange{};
    descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange.NumDescriptors = 1U;
    descriptorRange.BaseShaderRegister = 0U;
    descriptorRange.RegisterSpace = 0U;
    descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1U;
    parameter.DescriptorTable.pDescriptorRanges = &descriptorRange;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0U;
    sampler.RegisterSpace = 0U;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = 1U;
    description.pParameters = &parameter;
    description.NumStaticSamplers = 1U;
    description.pStaticSamplers = &sampler;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};
    HRESULT const serializeResult =
        D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, serialized.ReleaseAndGetAddressOf(),
                                    errors.ReleaseAndGetAddressOf());
    if (FAILED(serializeResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult,
                                                                D3D12BlobToUtf8(errors.Get())));
    }

    HRESULT const createResult =
        deviceResources_->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(rootSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                                                "Failed to create the Chapter 8 root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreatePipelineStates()
{
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
    description.pRootSignature = rootSignature_.Get();
    description.VS = fullscreenVertexShader_.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.SampleDesc.Count = 1U;

    description.PS = hdrPixelShader_.Bytecode();
    description.RTVFormats[0] = kHdrFormat;
    HRESULT const hdrResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(hdrPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(hdrResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", hdrResult,
                                             "Failed to create the Chapter 8 analytic HDR pipeline state."));
    }

    description.PS = displayPixelShader_.Bytecode();
    description.RTVFormats[0] = deviceResources_->back_buffer_format();
    HRESULT const displayResult = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(displayPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(displayResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", displayResult,
                                             "Failed to create the Chapter 8 display pipeline state."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateHdrIntermediate(lgp::framework::Extent2D size)
{
    hdrIntermediate_.Reset();
    if (size.empty())
    {
        return {};
    }

    if (!hdrRtv_)
    {
        auto rtvResult = deviceResources_->rtv_heap().Allocate(1U);
        if (!rtvResult)
        {
            return std::unexpected(std::move(rtvResult.error()));
        }
        hdrRtv_ = *rtvResult;
    }

    if (!hdrSrv_)
    {
        auto srvResult = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(1U);
        if (!srvResult)
        {
            return std::unexpected(std::move(srvResult.error()));
        }
        hdrSrv_ = *srvResult;
    }

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = kHdrFormat;
    clearValue.Color[3] = 1.0F;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = size.width;
    description.Height = size.height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = kHdrFormat;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    HRESULT const createResult = deviceResources_->device()->CreateCommittedResource3(
        &heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &clearValue, nullptr, 0U,
        nullptr, IID_PPV_ARGS(hdrIntermediate_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3",
                                                                createResult,
                                                                "Failed to create the Chapter 8 HDR intermediate."));
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = kHdrFormat;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    deviceResources_->device()->CreateRenderTargetView(hdrIntermediate_.Get(), &rtv, hdrRtv_.cpuHandle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kHdrFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1U;
    deviceResources_->device()->CreateShaderResourceView(hdrIntermediate_.Get(), &srv, hdrSrv_.cpuHandle);
    return {};
}

lgp::framework::Status Renderer::InitializeImGui()
{
    auto descriptorResult = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(1U);
    if (!descriptorResult)
    {
        return std::unexpected(std::move(descriptorResult.error()));
    }
    imguiFontDescriptor_ = *descriptorResult;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetIO().BackendPlatformName = "LGP.ManualInput";

    ImGui_ImplDX12_InitInfo info{};
    info.Device = deviceResources_->device();
    info.CommandQueue = deviceResources_->graphics_queue();
    info.NumFramesInFlight = static_cast<int>(deviceResources_->back_buffer_count());
    info.RTVFormat = deviceResources_->back_buffer_format();
    info.SrvDescriptorHeap = deviceResources_->shader_visible_cbv_srv_uav_heap().Get();
    info.LegacySingleSrvCpuDescriptor = imguiFontDescriptor_.cpuHandle;
    info.LegacySingleSrvGpuDescriptor = imguiFontDescriptor_.gpuHandle;
    if (!ImGui_ImplDX12_Init(&info))
    {
        return std::unexpected(
            lgp::framework::MakeError("ImGui_ImplDX12_Init", "Failed to initialize the Chapter 8 diagnostics UI."));
    }

    imguiInitialized_ = true;
    return {};
}

std::expected<Renderer::FrameGraphPlan, lgp::framework::Error> Renderer::CompileFrameGraph(
    lgp::framework::FrameContext const &frameContext) const
{
    PassGraph graph{};
    TextureResourceId const hdrResourceId = graph.AddImportedTexture("hdrIntermediate", ShaderResourceState());
    TextureResourceId const frameTargetResourceId =
        graph.AddImportedTexture("frameTarget", FrameStartState(frameContext));

    PassId const analyticPassId = graph.AddPass("analytic-hdr");
    PassId const displayPassId = graph.AddPass("display");
    PassId const frameBoundaryPassId = graph.AddPass("frame-boundary");

    graph.DeclareTextureWrite(analyticPassId, hdrResourceId, RenderTargetState());
    graph.DeclareTextureRead(displayPassId, hdrResourceId, ShaderResourceState());
    graph.DeclareTextureWrite(displayPassId, frameTargetResourceId, RenderTargetState());
    graph.DeclareTextureRead(frameBoundaryPassId, frameTargetResourceId, FrameEndState(frameContext));

    PassGraphCompileResult compileResult = graph.Compile();
    if (!compileResult)
    {
        return std::unexpected(
            lgp::framework::MakeError("PassGraph::Compile", DescribeDiagnostics(compileResult.error())));
    }

    return FrameGraphPlan{
        std::move(*compileResult), hdrResourceId, frameTargetResourceId, analyticPassId, displayPassId,
        frameBoundaryPassId,
    };
}

lgp::framework::Status Renderer::ApplyBarrierGroup(ID3D12GraphicsCommandList7 &commandList,
                                                   TextureBarrierGroup const &group,
                                                   std::vector<ID3D12Resource *> const &resources) const
{
    if (group.records.empty())
    {
        return {};
    }

    std::vector<D3D12_TEXTURE_BARRIER> barriers{};
    barriers.reserve(group.records.size());
    for (TextureBarrierRecord const &record : group.records)
    {
        if (record.resourceId.value >= resources.size() || resources[record.resourceId.value] == nullptr)
        {
            return std::unexpected(lgp::framework::MakeError(
                "Renderer::ApplyBarrierGroup",
                "Compiled graph referenced a texture resource that is not mapped to an ID3D12Resource."));
        }

        barriers.push_back(MakeTextureBarrier(*resources[record.resourceId.value], record.before, record.after));
    }

    D3D12_BARRIER_GROUP barrierGroup{};
    barrierGroup.Type = D3D12_BARRIER_TYPE_TEXTURE;
    barrierGroup.NumBarriers = static_cast<UINT>(barriers.size());
    barrierGroup.pTextureBarriers = barriers.data();
    commandList.Barrier(1U, &barrierGroup);
    return {};
}

void Renderer::ExecuteAnalyticPass(ID3D12GraphicsCommandList7 &commandList) const
{
    float const hdrClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.OMSetRenderTargets(1U, &hdrRtv_.cpuHandle, FALSE, nullptr);
    commandList.ClearRenderTargetView(hdrRtv_.cpuHandle, hdrClear, 0U, nullptr);
    commandList.SetPipelineState(hdrPipeline_.Get());
    commandList.DrawInstanced(3U, 1U, 0U, 0U);
}

void Renderer::BuildDiagnosticsWindow(lgp::framework::UpdateContext const &context) const
{
    ImGui::SetNextWindowSize(ImVec2(760.0F, 560.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("Frame graph diagnostics");

    if (!lastCompiledGraph_.has_value() || !lastCompiledFrameIndex_.has_value())
    {
        ImGui::TextWrapped("Waiting for the first completed frame. The diagnostics window will display the previous "
                           "frame's compiled schedule after frame 0 finishes.");
        ImGui::End();
        return;
    }

    CompiledPassGraph const &graph = *lastCompiledGraph_;
    ImGui::Text("Showing completed frame %llu at %ux%u", static_cast<unsigned long long>(*lastCompiledFrameIndex_),
                context.drawableSize.width, context.drawableSize.height);
    ImGui::Text("Passes: %zu  Dependencies: %zu  Textures: %zu", graph.scheduledPasses.size(),
                graph.dependencyEdges.size(), graph.textureResources.size());

    if (ImGui::CollapsingHeader("Schedule", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("schedule", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Slot");
            ImGui::TableSetupColumn("Pass");
            ImGui::TableHeadersRow();
            for (CompiledPass const &pass : graph.scheduledPasses)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", pass.executionIndex);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(pass.name.c_str());
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Dependencies", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (graph.dependencyEdges.empty())
        {
            ImGui::TextUnformatted("No derived or explicit dependencies.");
        }
        else if (ImGui::BeginTable("dependencies", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("From");
            ImGui::TableSetupColumn("To");
            ImGui::TableSetupColumn("Kind");
            ImGui::TableSetupColumn("Resource");
            ImGui::TableHeadersRow();
            for (DependencyEdge const &edge : graph.dependencyEdges)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::string{PassName(graph, edge.beforePassId)}.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::string{PassName(graph, edge.afterPassId)}.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::string{DependencyKindLabel(edge.kind)}.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(edge.textureResourceId.has_value()
                                           ? graph.textureResources[edge.textureResourceId->value].name.c_str()
                                           : "-");
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Grouped enhanced barriers", ImGuiTreeNodeFlags_DefaultOpen))
    {
        std::size_t barrierRecordCount{};
        for (CompiledPass const &pass : graph.scheduledPasses)
        {
            for (TextureBarrierGroup const &group : pass.barrierGroups)
            {
                barrierRecordCount += group.records.size();
            }
        }

        if (barrierRecordCount == 0U)
        {
            ImGui::TextUnformatted("No derived barriers were needed.");
        }
        else if (ImGui::BeginTable("barriers", 5,
                                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX))
        {
            ImGui::TableSetupColumn("Pass");
            ImGui::TableSetupColumn("Group");
            ImGui::TableSetupColumn("Resource");
            ImGui::TableSetupColumn("Before");
            ImGui::TableSetupColumn("After");
            ImGui::TableHeadersRow();

            for (CompiledPass const &pass : graph.scheduledPasses)
            {
                for (std::size_t groupIndex = 0U; groupIndex < pass.barrierGroups.size(); ++groupIndex)
                {
                    TextureBarrierGroup const &group = pass.barrierGroups[groupIndex];
                    for (TextureBarrierRecord const &record : group.records)
                    {
                        std::string const beforeState = FormatBarrierState(record.before);
                        std::string const afterState = FormatBarrierState(record.after);

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(pass.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%zu", groupIndex);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(std::string{ResourceName(graph, record.resourceId)}.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(beforeState.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(afterState.c_str());
                    }
                }
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Texture lifetimes", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int const columnCount = 3 + static_cast<int>(graph.scheduledPasses.size());
        if (ImGui::BeginTable("lifetimes", columnCount,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX))
        {
            ImGui::TableSetupColumn("Texture");
            ImGui::TableSetupColumn("Owner");
            ImGui::TableSetupColumn("Range");
            for (CompiledPass const &pass : graph.scheduledPasses)
            {
                std::string const label = "P" + std::to_string(pass.executionIndex);
                ImGui::TableSetupColumn(label.c_str());
            }
            ImGui::TableHeadersRow();

            for (ResourceLifetime const &lifetime : graph.resourceLifetimes)
            {
                TextureResource const &resource = graph.textureResources[lifetime.resourceId.value];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(resource.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(resource.imported ? "Imported" : "Transient");
                ImGui::TableNextColumn();
                std::string const interval = FormatLifetimeInterval(lifetime);
                ImGui::TextUnformatted(interval.c_str());

                for (CompiledPass const &pass : graph.scheduledPasses)
                {
                    bool const active = LifetimeUsesPassSlot(lifetime, pass.executionIndex);
                    ImGui::TableNextColumn();
                    if (active)
                    {
                        ImGui::TextColored(ImVec4(0.35F, 0.85F, 0.35F, 1.0F), "##");
                    }
                    else
                    {
                        ImGui::TextUnformatted("..");
                    }
                }
            }

            ImGui::EndTable();
        }
    }

    ImGui::End();
}

void Renderer::ExecuteDisplayPass(lgp::framework::FrameContext const &frameContext)
{
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    float const frameClear[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.ClearRenderTargetView(frameContext.renderTargetView, frameClear, 0U, nullptr);
    ID3D12DescriptorHeap *const descriptorHeaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, descriptorHeaps);
    commandList.SetPipelineState(displayPipeline_.Get());
    commandList.SetGraphicsRootDescriptorTable(HdrTexture, hdrSrv_.gpuHandle);
    commandList.DrawInstanced(3U, 1U, 0U, 0U);

    if (imguiFrameBegun_)
    {
        commandList.SetDescriptorHeaps(1U, descriptorHeaps);
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), &commandList);
        imguiFrameBegun_ = false;
    }
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;

    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreateRootSignature(); !status)
    {
        return status;
    }
    if (auto status = CreatePipelineStates(); !status)
    {
        return status;
    }
    if (auto status = CreateHdrIntermediate(context.drawableSize); !status)
    {
        return status;
    }
    if (!headless_)
    {
        return InitializeImGui();
    }
    return {};
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D drawableSize)
{
    return CreateHdrIntermediate(drawableSize);
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &context)
{
    if (headless_ || !imguiInitialized_)
    {
        return {};
    }

    if (imguiFrameBegun_)
    {
        ImGui::EndFrame();
        imguiFrameBegun_ = false;
    }

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(context.drawableSize.width), static_cast<float>(context.drawableSize.height)};
    io.DeltaTime = static_cast<float>(std::max(context.deltaSeconds, 1.0 / 240.0));
    io.AddMousePosEvent(static_cast<float>(context.input.mouse.x), static_cast<float>(context.input.mouse.y));
    io.AddMouseButtonEvent(0, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Left));
    io.AddMouseButtonEvent(1, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Right));
    io.AddMouseWheelEvent(0.0F, context.input.mouse.wheelDelta);

    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    imguiFrameBegun_ = true;
    BuildDiagnosticsWindow(context);
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr || hdrIntermediate_ == nullptr)
    {
        if (imguiFrameBegun_)
        {
            ImGui::EndFrame();
            imguiFrameBegun_ = false;
        }
        return {};
    }

    std::expected<FrameGraphPlan, lgp::framework::Error> planResult = CompileFrameGraph(frameContext);
    if (!planResult)
    {
        return std::unexpected(std::move(planResult.error()));
    }
    FrameGraphPlan plan = std::move(*planResult);

    std::vector<ID3D12Resource *> resources(plan.compiledGraph.textureResources.size(), nullptr);
    resources[plan.hdrResourceId.value] = hdrIntermediate_.Get();
    resources[plan.frameTargetResourceId.value] = frameContext.renderTarget;

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.SetGraphicsRootSignature(rootSignature_.Get());

    for (CompiledPass const &pass : plan.compiledGraph.scheduledPasses)
    {
        for (TextureBarrierGroup const &barrierGroup : pass.barrierGroups)
        {
            if (auto status = ApplyBarrierGroup(commandList, barrierGroup, resources); !status)
            {
                return status;
            }
        }

        if (pass.id == plan.analyticPassId)
        {
            ExecuteAnalyticPass(commandList);
            continue;
        }

        if (pass.id == plan.displayPassId)
        {
            ExecuteDisplayPass(frameContext);
            continue;
        }

        if (pass.id == plan.frameBoundaryPassId)
        {
            continue;
        }

        return std::unexpected(lgp::framework::MakeError(
            "Renderer::Render", "Encountered an unexpected compiled pass while executing the Chapter 8 solution."));
    }

    lastCompiledFrameIndex_ = frameContext.frameIndex;
    lastCompiledGraph_ = std::move(plan.compiledGraph);
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    if (imguiFrameBegun_)
    {
        ImGui::EndFrame();
        imguiFrameBegun_ = false;
    }
    if (imguiInitialized_)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized_ = false;
    }
    lastCompiledGraph_.reset();
    lastCompiledFrameIndex_.reset();
    hdrIntermediate_.Reset();
    if (deviceResources_ != nullptr && hdrRtv_)
    {
        deviceResources_->rtv_heap().Free(hdrRtv_);
        hdrRtv_ = {};
    }
    if (deviceResources_ != nullptr && hdrSrv_)
    {
        deviceResources_->shader_visible_cbv_srv_uav_heap().Free(hdrSrv_);
        hdrSrv_ = {};
    }
    if (deviceResources_ != nullptr && imguiFontDescriptor_)
    {
        deviceResources_->shader_visible_cbv_srv_uav_heap().Free(imguiFontDescriptor_);
        imguiFontDescriptor_ = {};
    }
    displayPipeline_.Reset();
    hdrPipeline_.Reset();
    rootSignature_.Reset();
    deviceResources_ = nullptr;
}

CompiledPassGraph const *Renderer::LastCompiledGraph() const noexcept
{
    return lastCompiledGraph_.has_value() ? &*lastCompiledGraph_ : nullptr;
}

} // namespace ch08::frame_graph::solution
