#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include "../Common/GpuLabSupport.hpp"

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace ch09::transient_aliasing::solution
{
namespace
{

using namespace ch08::frame_graph;
using namespace transient_allocation;
using gpu::MakeTextureBarrier;
using gpu::RenderTargetState;
using gpu::ShaderResourceState;
using gpu::UndefinedState;

inline constexpr HeapCompatibilityKey kRtDsTextureHeap{
    static_cast<std::uint32_t>(D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES)};

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "TransientAliasingLab.hlsl";
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

[[nodiscard]] std::string DescribeAllocationDiagnostics(std::vector<AllocationDiagnostic> const &diagnostics)
{
    std::string message{};
    for (AllocationDiagnostic const &diagnostic : diagnostics)
    {
        if (!message.empty())
        {
            message += '\n';
        }
        message += diagnostic.message;
    }
    return message;
}

[[nodiscard]] ResourceLifetime const *FindLifetime(CompiledPassGraph const &graph,
                                                   TextureResourceId resourceId) noexcept
{
    auto const lifetime = std::ranges::find(graph.resourceLifetimes, resourceId, &ResourceLifetime::resourceId);
    return lifetime == graph.resourceLifetimes.end() ? nullptr : &*lifetime;
}

[[nodiscard]] TexturePlacement const *FindPlacement(TransientTextureAllocationPlan const &plan,
                                                    TextureResourceId resourceId) noexcept
{
    auto const placement = std::ranges::find(plan.placements, resourceId, &TexturePlacement::resourceId);
    return placement == plan.placements.end() ? nullptr : &*placement;
}

[[nodiscard]] std::string_view ResourceName(CompiledPassGraph const &graph, TextureResourceId resourceId) noexcept
{
    if (resourceId.value >= graph.textureResources.size())
    {
        return "unknown";
    }
    return graph.textureResources[resourceId.value].name;
}

[[nodiscard]] std::string LifetimeLabel(ResourceLifetime const &lifetime)
{
    if (!lifetime.firstExecutionIndex || !lifetime.lastExecutionIndex)
    {
        return "unused";
    }
    return "[" + std::to_string(*lifetime.firstExecutionIndex) + ", " + std::to_string(*lifetime.lastExecutionIndex) +
           "]";
}

} // namespace

lgp::framework::Status Renderer::CreateShaders()
{
    return gpu::CreateLabShaders(ShaderPath(), fullscreenVertexShader_, analyticPixelShader_, copyPixelShader_,
                                 accentPixelShader_, compositePixelShader_);
}

lgp::framework::Status Renderer::CreateRootSignature()
{
    return gpu::CreateLabRootSignature(*deviceResources_->device(), rootSignature_);
}

lgp::framework::Status Renderer::CreatePipelineStates()
{
    return gpu::CreateLabPipelineStates(*deviceResources_->device(), deviceResources_->back_buffer_format(),
                                        *rootSignature_.Get(), fullscreenVertexShader_, analyticPixelShader_,
                                        copyPixelShader_, accentPixelShader_, compositePixelShader_, analyticPipeline_,
                                        copyPipeline_, accentPipeline_, compositePipeline_);
}

std::expected<Renderer::GraphExecutionPlan, lgp::framework::Error> Renderer::CompileGraph() const
{
    PassGraph graph{};
    TextureResourceId const textureA = graph.AddTransientTexture("A");
    TextureResourceId const textureB = graph.AddTransientTexture("B");
    TextureResourceId const textureC = graph.AddTransientTexture("C");
    TextureResourceId const frameTarget = graph.AddImportedTexture("frameTarget", gpu::FrameStartState(headless_));

    PassId const analyticPass = graph.AddPass("analytic-hdr");
    PassId const copyPass = graph.AddPass("copy-hdr");
    PassId const accentPass = graph.AddPass("accent-mask");
    PassId const compositePass = graph.AddPass("composite");
    PassId const boundaryPass = graph.AddPass("frame-boundary");

    graph.DeclareTextureWrite(analyticPass, textureA, RenderTargetState());
    graph.DeclareTextureRead(copyPass, textureA, ShaderResourceState());
    graph.DeclareTextureWrite(copyPass, textureB, RenderTargetState());
    graph.DeclareTextureWrite(accentPass, textureC, RenderTargetState());
    graph.DeclareTextureRead(compositePass, textureB, ShaderResourceState());
    graph.DeclareTextureRead(compositePass, textureC, ShaderResourceState());
    graph.DeclareTextureWrite(compositePass, frameTarget, RenderTargetState());
    graph.DeclareTextureRead(boundaryPass, textureB, UndefinedState());
    graph.DeclareTextureRead(boundaryPass, textureC, UndefinedState());
    graph.DeclareTextureRead(boundaryPass, frameTarget, gpu::FrameEndState(headless_));

    PassGraphCompileResult result = graph.Compile();
    if (!result)
    {
        return std::unexpected(
            lgp::framework::MakeError("PassGraph::Compile", DescribeGraphDiagnostics(result.error())));
    }
    return GraphExecutionPlan{std::move(*result), textureA, textureB,   textureC,      frameTarget,
                              analyticPass,       copyPass, accentPass, compositePass, boundaryPass};
}

lgp::framework::Status Renderer::CreateTransientTextures(lgp::framework::Extent2D size)
{
    for (auto &texture : transientTextures_)
    {
        texture.Reset();
    }
    transientHeap_.Reset();
    graphPlan_.reset();
    allocationPlan_.reset();
    lastAliasHandoffTrace_.clear();
    if (size.empty())
    {
        return {};
    }

    if (!transientRtvs_)
    {
        auto result = deviceResources_->rtv_heap().Allocate(3U);
        if (!result)
        {
            return std::unexpected(std::move(result.error()));
        }
        transientRtvs_ = *result;
    }
    if (!transientSrvs_)
    {
        auto result = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(3U);
        if (!result)
        {
            return std::unexpected(std::move(result.error()));
        }
        transientSrvs_ = *result;
    }

    auto graphResult = CompileGraph();
    if (!graphResult)
    {
        return std::unexpected(std::move(graphResult.error()));
    }
    GraphExecutionPlan graphPlan = std::move(*graphResult);

    D3D12_RESOURCE_DESC1 const description = gpu::TextureDescription(size);
    D3D12_RESOURCE_ALLOCATION_INFO1 detailedAllocation{};
    D3D12_RESOURCE_ALLOCATION_INFO const allocation =
        deviceResources_->device()->GetResourceAllocationInfo2(0U, 1U, &description, &detailedAllocation);
    if (allocation.SizeInBytes == UINT64_MAX || allocation.SizeInBytes == 0U || allocation.Alignment == 0U)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ID3D12Device10::GetResourceAllocationInfo2",
            "The adapter returned invalid allocation requirements for the Chapter 9 transient texture."));
    }

    std::array<TextureResourceId, 3U> const resourceIds{graphPlan.textureA, graphPlan.textureB, graphPlan.textureC};
    std::array<std::string_view, 3U> const names{"A", "B", "C"};
    std::vector<TransientTextureRequest> requests{};
    requests.reserve(resourceIds.size());
    for (std::size_t index = 0U; index < resourceIds.size(); ++index)
    {
        ResourceLifetime const *lifetime = FindLifetime(graphPlan.graph, resourceIds[index]);
        if (lifetime == nullptr)
        {
            return std::unexpected(lgp::framework::MakeError(
                "Renderer::CreateTransientTextures", "The compiled graph omitted a transient resource lifetime."));
        }
        requests.push_back({resourceIds[index], std::string{names[index]}, allocation.SizeInBytes, allocation.Alignment,
                            kRtDsTextureHeap, *lifetime});
    }

    TransientTextureAllocationResult planResult = PlanTransientTextureAllocations(requests);
    if (!planResult)
    {
        return std::unexpected(lgp::framework::MakeError("PlanTransientTextureAllocations",
                                                         DescribeAllocationDiagnostics(planResult.error())));
    }
    TransientTextureAllocationPlan allocationPlan = std::move(*planResult);
    if (allocationPlan.heaps.size() != 1U)
    {
        return std::unexpected(lgp::framework::MakeError(
            "PlanTransientTextureAllocations", "The Chapter 9 lab requires exactly one compatible physical heap."));
    }

    HeapAllocation const &plannedHeap = allocationPlan.heaps.front();
    D3D12_HEAP_DESC heapDescription{};
    heapDescription.SizeInBytes = plannedHeap.size;
    heapDescription.Alignment = plannedHeap.alignment;
    heapDescription.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDescription.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    HRESULT const heapResult =
        deviceResources_->device()->CreateHeap(&heapDescription, IID_PPV_ARGS(transientHeap_.ReleaseAndGetAddressOf()));
    if (FAILED(heapResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device::CreateHeap", heapResult, "Failed to create the planned Chapter 9 transient heap."));
    }

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = gpu::kTransientFormat;
    clearValue.Color[3] = 1.0F;
    for (std::size_t index = 0U; index < resourceIds.size(); ++index)
    {
        TexturePlacement const *placement = FindPlacement(allocationPlan, resourceIds[index]);
        if (placement == nullptr)
        {
            return std::unexpected(lgp::framework::MakeError(
                "Renderer::CreateTransientTextures", "The allocation plan omitted a transient texture placement."));
        }
        HRESULT const placedResult = deviceResources_->device()->CreatePlacedResource2(
            transientHeap_.Get(), placement->offset, &description, D3D12_BARRIER_LAYOUT_UNDEFINED, &clearValue, 0U,
            nullptr, IID_PPV_ARGS(transientTextures_[index].ReleaseAndGetAddressOf()));
        if (FAILED(placedResult))
        {
            return std::unexpected(
                lgp::framework::MakeHResultError("ID3D12Device10::CreatePlacedResource2", placedResult,
                                                 "Failed to create a placed Chapter 9 transient texture."));
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = gpu::kTransientFormat;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        deviceResources_->device()->CreateRenderTargetView(transientTextures_[index].Get(), &rtv,
                                                           transientRtvs_.CpuHandle(static_cast<UINT>(index)));

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = gpu::kTransientFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1U;
        deviceResources_->device()->CreateShaderResourceView(transientTextures_[index].Get(), &srv,
                                                             transientSrvs_.CpuHandle(static_cast<UINT>(index)));
    }

    graphPlan_ = std::move(graphPlan);
    allocationPlan_ = std::move(allocationPlan);
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
            lgp::framework::MakeError("ImGui_ImplDX12_Init", "Failed to initialize Chapter 9 diagnostics."));
    }
    imguiInitialized_ = true;
    return {};
}

lgp::framework::Status Renderer::ApplyBarrierGroup(ID3D12GraphicsCommandList7 &commandList,
                                                   TextureBarrierGroup const &group,
                                                   std::vector<ID3D12Resource *> const &resources) const
{
    std::vector<D3D12_TEXTURE_BARRIER> barriers{};
    barriers.reserve(group.records.size());
    for (TextureBarrierRecord const &record : group.records)
    {
        if (record.resourceId.value >= resources.size() || resources[record.resourceId.value] == nullptr)
        {
            return std::unexpected(lgp::framework::MakeError("Renderer::ApplyBarrierGroup",
                                                             "A compiled texture ID has no bound D3D12 resource."));
        }
        D3D12_TEXTURE_BARRIER_FLAGS const flags = record.before.layout == D3D12_BARRIER_LAYOUT_UNDEFINED &&
                                                          record.after.layout == D3D12_BARRIER_LAYOUT_RENDER_TARGET
                                                      ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD
                                                      : D3D12_TEXTURE_BARRIER_FLAG_NONE;
        barriers.push_back(MakeTextureBarrier(*resources[record.resourceId.value], record.before, record.after, flags));
    }
    if (!barriers.empty())
    {
        D3D12_BARRIER_GROUP barrierGroup{};
        barrierGroup.Type = D3D12_BARRIER_TYPE_TEXTURE;
        barrierGroup.NumBarriers = static_cast<UINT>(barriers.size());
        barrierGroup.pTextureBarriers = barriers.data();
        commandList.Barrier(1U, &barrierGroup);
    }
    return {};
}

lgp::framework::Status Renderer::ExecuteAliasHandoff(ID3D12GraphicsCommandList7 &commandList)
{
    if (!graphPlan_ || !allocationPlan_)
    {
        return std::unexpected(lgp::framework::MakeError(
            "Renderer::ExecuteAliasHandoff", "The compiled graph and allocation plan must both be available."));
    }

    auto const accentPass =
        std::ranges::find(graphPlan_->graph.scheduledPasses, graphPlan_->accentPass, &CompiledPass::id);
    if (accentPass == graphPlan_->graph.scheduledPasses.end())
    {
        return std::unexpected(lgp::framework::MakeError(
            "Renderer::ExecuteAliasHandoff", "The compiled schedule does not contain the accent-mask pass."));
    }

    auto const activation =
        std::ranges::find(allocationPlan_->aliasActivations, graphPlan_->textureC, &AliasActivation::resourceId);
    if (activation == allocationPlan_->aliasActivations.end() ||
        std::ranges::count(allocationPlan_->aliasActivations, graphPlan_->textureC, &AliasActivation::resourceId) != 1)
    {
        return std::unexpected(lgp::framework::MakeError(
            "Renderer::ExecuteAliasHandoff",
            "The concrete lab plan must contain exactly one alias activation for texture C."));
    }
    if (activation->passIndex != accentPass->executionIndex ||
        activation->predecessorResourceIds != std::vector<TextureResourceId>{graphPlan_->textureA})
    {
        return std::unexpected(lgp::framework::MakeError(
            "Renderer::ExecuteAliasHandoff",
            "Texture C must activate at accent-mask with texture A as its only immediate predecessor."));
    }

    TexturePlacement const *placementA = FindPlacement(*allocationPlan_, graphPlan_->textureA);
    TexturePlacement const *placementC = FindPlacement(*allocationPlan_, graphPlan_->textureC);
    if (placementA == nullptr || placementC == nullptr || placementA->heapIndex != placementC->heapIndex ||
        placementA->offset >= placementC->offset + placementC->allocationSize ||
        placementC->offset >= placementA->offset + placementA->allocationSize)
    {
        return std::unexpected(lgp::framework::MakeError(
            "Renderer::ExecuteAliasHandoff",
            "The planned A-to-C activation requires overlapping byte ranges in the same physical heap."));
    }

    std::vector<ID3D12Resource *> resources(graphPlan_->graph.textureResources.size(), nullptr);
    resources[graphPlan_->textureA.value] = transientTextures_[0].Get();
    resources[graphPlan_->textureB.value] = transientTextures_[1].Get();
    resources[graphPlan_->textureC.value] = transientTextures_[2].Get();
    TextureResourceId const predecessorId = activation->predecessorResourceIds.front();
    TextureResourceId const activatedId = activation->resourceId;
    if (predecessorId.value >= resources.size() || activatedId.value >= resources.size() ||
        resources[predecessorId.value] == nullptr || resources[activatedId.value] == nullptr)
    {
        return std::unexpected(lgp::framework::MakeError(
            "Renderer::ExecuteAliasHandoff",
            "The planned alias activation resource IDs are not bound to concrete D3D12 resources."));
    }

    lgp::framework::TextureBarrierState const deactivateAfter{
        D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_UNDEFINED};
    lgp::framework::TextureBarrierState const activateBefore{
        D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_UNDEFINED};
    std::array<D3D12_TEXTURE_BARRIER, 2U> barriers{
        MakeTextureBarrier(*resources[predecessorId.value], ShaderResourceState(), deactivateAfter),
        MakeTextureBarrier(*resources[activatedId.value], activateBefore, RenderTargetState(),
                           D3D12_TEXTURE_BARRIER_FLAG_DISCARD),
    };
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = static_cast<UINT>(barriers.size());
    group.pTextureBarriers = barriers.data();
    commandList.Barrier(1U, &group);

    lastAliasHandoffTrace_ = {
        {predecessorId, ShaderResourceState(), deactivateAfter, D3D12_TEXTURE_BARRIER_FLAG_NONE},
        {activatedId, activateBefore, RenderTargetState(), D3D12_TEXTURE_BARRIER_FLAG_DISCARD},
    };
    return {};
}

void Renderer::DrawTo(ID3D12GraphicsCommandList7 &commandList, ID3D12PipelineState &pipeline,
                      D3D12_CPU_DESCRIPTOR_HANDLE renderTarget) const
{
    commandList.OMSetRenderTargets(1U, &renderTarget, FALSE, nullptr);
    commandList.SetPipelineState(&pipeline);
    commandList.DrawInstanced(3U, 1U, 0U, 0U);
}

void Renderer::BuildDiagnosticsWindow() const
{
    if (!graphPlan_ || !allocationPlan_)
    {
        return;
    }
    ImGui::Begin("Transient texture placement");
    ImGui::Text("Requested: %llu bytes", static_cast<unsigned long long>(allocationPlan_->totalRequestedBytes));
    ImGui::Text("Heap: %llu bytes", static_cast<unsigned long long>(allocationPlan_->totalHeapBytes));
    ImGui::Text("Saved: %llu bytes", static_cast<unsigned long long>(allocationPlan_->savedBytes));
    ImGui::Separator();

    float const availableWidth = (std::max)(ImGui::GetContentRegionAvail().x, 1.0F);
    for (TexturePlacement const &placement : allocationPlan_->placements)
    {
        ImGui::Text("%s lifetime %s heap %zu offset %llu range [%llu, %llu)", placement.name.c_str(),
                    LifetimeLabel(placement.lifetime).c_str(), placement.heapIndex,
                    static_cast<unsigned long long>(placement.offset),
                    static_cast<unsigned long long>(placement.offset),
                    static_cast<unsigned long long>(placement.offset + placement.allocationSize));
        float const start =
            availableWidth * static_cast<float>(placement.offset) / static_cast<float>(allocationPlan_->totalHeapBytes);
        float const width = availableWidth * static_cast<float>(placement.allocationSize) /
                            static_cast<float>(allocationPlan_->totalHeapBytes);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + start);
        ImGui::ProgressBar(width / availableWidth, {availableWidth - start, 10.0F}, "");
    }

    ImGui::Separator();
    for (AliasActivation const &activation : allocationPlan_->aliasActivations)
    {
        std::string predecessors{};
        std::string const resourceName{ResourceName(graphPlan_->graph, activation.resourceId)};
        for (TextureResourceId const predecessor : activation.predecessorResourceIds)
        {
            if (!predecessors.empty())
            {
                predecessors += ", ";
            }
            predecessors += ResourceName(graphPlan_->graph, predecessor);
        }
        ImGui::Text("Pass %u activates %s after %s", activation.passIndex, resourceName.c_str(), predecessors.c_str());
    }
    ImGui::End();
}

void Renderer::ExecuteComposite(lgp::framework::FrameContext const &frameContext)
{
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    ID3D12DescriptorHeap *const descriptorHeaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, descriptorHeaps);
    commandList.SetGraphicsRootDescriptorTable(0U, transientSrvs_.GpuHandle(1U));
    DrawTo(commandList, *compositePipeline_.Get(), frameContext.renderTargetView);
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
    if (auto status = CreateTransientTextures(context.drawableSize); !status)
    {
        return status;
    }
    return headless_ ? lgp::framework::Status{} : InitializeImGui();
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D drawableSize)
{
    return CreateTransientTextures(drawableSize);
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
    io.DeltaTime = static_cast<float>((std::max)(context.deltaSeconds, 1.0 / 240.0));
    io.AddMousePosEvent(static_cast<float>(context.input.mouse.x), static_cast<float>(context.input.mouse.y));
    io.AddMouseButtonEvent(0, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Left));
    io.AddMouseButtonEvent(1, context.input.mouse.IsButtonDown(lgp::framework::MouseButton::Right));
    io.AddMouseWheelEvent(0.0F, context.input.mouse.wheelDelta);
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    imguiFrameBegun_ = true;
    BuildDiagnosticsWindow();
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr || !graphPlan_ ||
        !allocationPlan_ || transientTextures_[0] == nullptr || transientTextures_[1] == nullptr ||
        transientTextures_[2] == nullptr)
    {
        return {};
    }

    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    std::vector<ID3D12Resource *> resources(graphPlan_->graph.textureResources.size(), nullptr);
    resources[graphPlan_->textureA.value] = transientTextures_[0].Get();
    resources[graphPlan_->textureB.value] = transientTextures_[1].Get();
    resources[graphPlan_->textureC.value] = transientTextures_[2].Get();
    resources[graphPlan_->frameTarget.value] = frameContext.renderTarget;

    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.SetGraphicsRootSignature(rootSignature_.Get());
    ID3D12DescriptorHeap *const descriptorHeaps[]{frameContext.shaderVisibleCbvSrvUavHeap};

    for (CompiledPass const &pass : graphPlan_->graph.scheduledPasses)
    {
        if (pass.id == graphPlan_->accentPass)
        {
            if (auto status = ExecuteAliasHandoff(commandList); !status)
            {
                return status;
            }
        }
        else
        {
            for (TextureBarrierGroup const &barrierGroup : pass.barrierGroups)
            {
                if (auto status = ApplyBarrierGroup(commandList, barrierGroup, resources); !status)
                {
                    return status;
                }
            }
        }

        if (pass.id == graphPlan_->analyticPass)
        {
            DrawTo(commandList, *analyticPipeline_.Get(), transientRtvs_.CpuHandle(0U));
        }
        else if (pass.id == graphPlan_->copyPass)
        {
            commandList.SetDescriptorHeaps(1U, descriptorHeaps);
            commandList.SetGraphicsRootDescriptorTable(0U, transientSrvs_.GpuHandle(0U));
            DrawTo(commandList, *copyPipeline_.Get(), transientRtvs_.CpuHandle(1U));
        }
        else if (pass.id == graphPlan_->accentPass)
        {
            DrawTo(commandList, *accentPipeline_.Get(), transientRtvs_.CpuHandle(2U));
        }
        else if (pass.id == graphPlan_->compositePass)
        {
            ExecuteComposite(frameContext);
        }
        else if (pass.id != graphPlan_->boundaryPass)
        {
            return std::unexpected(lgp::framework::MakeError(
                "Renderer::Render", "The compiled Chapter 9 schedule contains an unknown pass."));
        }
    }
    return {};
}

CompiledPassGraph const *Renderer::CompiledGraph() const noexcept
{
    return graphPlan_ ? &graphPlan_->graph : nullptr;
}

TransientTextureAllocationPlan const *Renderer::AllocationPlan() const noexcept
{
    return allocationPlan_ ? &*allocationPlan_ : nullptr;
}

std::vector<ExecutedAliasBarrierRecord> const &Renderer::LastAliasHandoffTrace() const noexcept
{
    return lastAliasHandoffTrace_;
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
    lastAliasHandoffTrace_.clear();
    allocationPlan_.reset();
    graphPlan_.reset();
    for (auto &texture : transientTextures_)
    {
        texture.Reset();
    }
    transientHeap_.Reset();
    if (deviceResources_ != nullptr && transientRtvs_)
    {
        deviceResources_->rtv_heap().Free(transientRtvs_);
        transientRtvs_ = {};
    }
    if (deviceResources_ != nullptr && transientSrvs_)
    {
        deviceResources_->shader_visible_cbv_srv_uav_heap().Free(transientSrvs_);
        transientSrvs_ = {};
    }
    if (deviceResources_ != nullptr && imguiFontDescriptor_)
    {
        deviceResources_->shader_visible_cbv_srv_uav_heap().Free(imguiFontDescriptor_);
        imguiFontDescriptor_ = {};
    }
    compositePipeline_.Reset();
    accentPipeline_.Reset();
    copyPipeline_.Reset();
    analyticPipeline_.Reset();
    rootSignature_.Reset();
    deviceResources_ = nullptr;
}

} // namespace ch09::transient_aliasing::solution
