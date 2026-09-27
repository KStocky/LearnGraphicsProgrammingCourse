#include "Renderer.hpp"

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/error.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace ch35::spatial::starter
{
namespace
{
using lgp::framework::MakeError;
using lgp::framework::MakeHResultError;
using lgp::framework::Status;
using Microsoft::WRL::ComPtr;

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
    if (auto status = Compile(*compiler, path, L"PointVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    return Compile(*compiler, path, L"PointPS", L"ps_6_0", pixelShader_);
}

auto Renderer::CreatePipeline() -> Status
{
    auto &device = *resources_->device();
    std::array<D3D12_ROOT_PARAMETER, 2U> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 1U;
    parameters[0].Constants.Num32BitValues = 2U;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor.ShaderRegister = 0U;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    if (auto status = Root(device, description, graphicsRoot_); !status)
    {
        return status;
    }
    return GraphicsPipeline(device, *graphicsRoot_.Get(), vertexShader_, pixelShader_, resources_->back_buffer_format(),
                            graphicsPipeline_);
}

auto Renderer::CreateResources() -> Status
{
    drawSlots_.resize(resources_->back_buffer_count());
    for (auto &draw : drawSlots_)
    {
        auto buffer = gpu::CreateBuffer(*resources_->device(), sizeof(gpu::GpuRecord) * kMaximumRecords,
                                        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, true);
        if (!buffer)
        {
            return std::unexpected(std::move(buffer.error()));
        }
        draw = std::move(*buffer);
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
    if (auto status = CreatePipeline(); !status)
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
    auto reference = gpu::BuildReference(ActiveConfiguration());
    if (!reference)
    {
        return std::unexpected(std::move(reference.error()));
    }
    reference_ = std::move(*reference);
    return {};
}

auto Renderer::Render(lgp::framework::FrameContext const &frame) -> Status
{
    if (frame.frameSlot >= drawSlots_.size())
    {
        return std::unexpected(MakeError("Render", "Chapter 35 frame slot is out of range."));
    }
    auto const configuration = ActiveConfiguration();
    auto &draw = drawSlots_[frame.frameSlot];
    std::array<gpu::GpuRecord, kMaximumRecords> records{};
    std::copy(reference_.records.begin(), reference_.records.end(), records.begin());
    std::memcpy(draw.mapped_data(), records.data(), sizeof(records));

    auto &list = *frame.commandList;
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
    list.SetGraphicsRootShaderResourceView(1U, draw.Get()->GetGPUVirtualAddress());
    list.SetPipelineState(graphicsPipeline_.Get());
    if (reference_.stats.emittedCount != 0U)
    {
        list.DrawInstanced(6U, reference_.stats.emittedCount, 0U, 0U);
    }
    lgp::framework::TransitionTexture(
        list, *frame.renderTarget,
        {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET},
        {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
         frame.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT});
    return {};
}

auto Renderer::ReadBackOutputs() -> std::expected<gpu::FrameReadback, lgp::framework::Error>
{
    return reference_; // Baseline evidence is CPU-computed, not a GPU readback.
}

void Renderer::ConfigureHeadlessTest(gpu::LabConfiguration const &configuration)
{
    headless_ = configuration;
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    drawSlots_.clear();
    graphicsPipeline_.Reset();
    graphicsRoot_.Reset();
    resources_ = nullptr;
}
} // namespace ch35::spatial::starter
