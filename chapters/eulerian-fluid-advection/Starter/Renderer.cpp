#include "Renderer.hpp"

#include <lgp/framework/error.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <utility>

namespace ch36::fluid::starter
{
using lgp::framework::MakeError;
using lgp::framework::Status;

auto Renderer::Active() const -> gpu::LabConfiguration
{
    return headless_.value_or(gpu::DefaultConfiguration());
}
auto Renderer::Initialize(lgp::framework::ApplicationInitContext const &context) -> Status
{
    resources_ = &context.deviceResources;
    auto compiler = lgp::framework::ShaderCompiler::Create();
    if (!compiler)
    {
        return std::unexpected(std::move(compiler.error()));
    }
    auto const path = std::filesystem::path{__FILE__}.parent_path() / "FluidLab.hlsl";
    auto vertex = gpu::Compile(*compiler, path, L"FullscreenVS", L"vs_6_0");
    if (!vertex)
    {
        return std::unexpected(std::move(vertex.error()));
    }
    auto pixel = gpu::Compile(*compiler, path, L"TracerPS", L"ps_6_0");
    if (!pixel)
    {
        return std::unexpected(std::move(pixel.error()));
    }
    std::array<D3D12_ROOT_PARAMETER, 2U> params{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0U;
    params[0].Constants.Num32BitValues = 4U;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0U;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(params.size());
    desc.pParameters = params.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    auto root = gpu::MakeRoot(*resources_->device(), desc);
    if (!root)
    {
        return std::unexpected(std::move(root.error()));
    }
    root_ = std::move(*root);
    auto pipeline =
        gpu::MakeGraphics(*resources_->device(), *root_.Get(), *vertex, *pixel, resources_->back_buffer_format());
    if (!pipeline)
    {
        return std::unexpected(std::move(pipeline.error()));
    }
    pipeline_ = std::move(*pipeline);
    for (std::uint32_t i = 0U; i < resources_->back_buffer_count(); ++i)
    {
        auto buffer = gpu::MakeBuffer(*resources_->device(), 67U * 67U * sizeof(float), D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_FLAG_NONE, true);
        if (!buffer)
        {
            return std::unexpected(std::move(buffer.error()));
        }
        drawSlots_.push_back(std::move(*buffer));
    }
    return {};
}
auto Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D) -> Status
{
    return {};
}
auto Renderer::Update(lgp::framework::UpdateContext const &) -> Status
{
    auto const config = Active();
    if (auto s = gpu::ValidateConfiguration(config); !s)
    {
        return s;
    }
    baseline_ = config.scalar;
    auto filled = FillGhosts(config.grid, baseline_, config.boundary);
    if (!filled)
    {
        return std::unexpected(MakeError("Ch36 Starter", "Could not fill baseline ghosts."));
    }
    return {};
}
auto Renderer::Render(lgp::framework::FrameContext const &frame) -> Status
{
    if (frame.frameSlot >= drawSlots_.size() || baseline_.values.empty())
    {
        return std::unexpected(MakeError("Ch36 Starter", "Update required or frame slot out of range."));
    }
    auto const config = Active();
    auto *mapped = reinterpret_cast<float *>(drawSlots_[frame.frameSlot].Data());
    std::transform(baseline_.values.begin(), baseline_.values.end(), mapped,
                   [](double value) { return static_cast<float>(value); });
    gpu::BeginDraw(frame);
    auto &list = *frame.commandList;
    list.SetGraphicsRootSignature(root_.Get());
    std::array<std::uint32_t, 4U> const parameters{config.grid.width, config.grid.height,
                                                   std::max(1U, static_cast<std::uint32_t>(frame.viewport.Width)),
                                                   std::max(1U, static_cast<std::uint32_t>(frame.viewport.Height))};
    list.SetGraphicsRoot32BitConstants(0U, 4U, parameters.data(), 0U);
    list.SetGraphicsRootShaderResourceView(1U, drawSlots_[frame.frameSlot].Get()->GetGPUVirtualAddress());
    list.SetPipelineState(pipeline_.Get());
    list.DrawInstanced(3U, 1U, 0U, 0U);
    gpu::EndDraw(frame);
    rendered_ = true;
    return {};
}
auto Renderer::ReadBackOutputs() const -> std::expected<gpu::FrameReadback, lgp::framework::Error>
{
    if (!rendered_)
    {
        return std::unexpected(MakeError("Ch36 Starter", "No baseline frame."));
    }
    auto const config = Active();
    auto result = gpu::Diagnostics(config.grid, config.scalar, baseline_, 0U, false);
    result.velocity = {config.velocityX, config.velocityY};
    for (auto *field : {&result.velocity.x, &result.velocity.y})
    {
        if (auto filled = FillGhosts(config.grid, *field, config.boundary); !filled)
        {
            return std::unexpected(MakeError("Ch36 Starter", "Invalid baseline face velocity."));
        }
    }
    return result;
}
void Renderer::ConfigureHeadlessTest(gpu::LabConfiguration const &configuration)
{
    headless_ = configuration;
}
void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept
{
    drawSlots_.clear();
    pipeline_.Reset();
    root_.Reset();
    resources_ = nullptr;
    rendered_ = false;
}
} // namespace ch36::fluid::starter
