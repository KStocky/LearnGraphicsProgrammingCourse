#include "Renderer.hpp"

#include <d3d12.h>

namespace ch04::color_pipeline::starter
{
namespace
{

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

} // namespace

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &)
{
    return {};
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &, lgp::framework::Extent2D)
{
    return {};
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &)
{
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr)
    {
        return {};
    }

    lgp::framework::TransitionTexture(*frameContext.commandList, *frameContext.renderTarget,
                                      FrameStartState(frameContext), RenderTargetState());

    float const clearColor[]{0.02F, 0.025F, 0.035F, 1.0F};
    frameContext.commandList->OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    frameContext.commandList->ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);

    lgp::framework::TransitionTexture(*frameContext.commandList, *frameContext.renderTarget, RenderTargetState(),
                                      FrameEndState(frameContext));
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept {}

} // namespace ch04::color_pipeline::starter
