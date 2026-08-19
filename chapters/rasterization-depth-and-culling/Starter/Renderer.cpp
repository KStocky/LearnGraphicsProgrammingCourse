#include "Renderer.hpp"

#include <d3d12.h>

namespace ch02::rasterization::starter
{

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
    if (frameContext.renderTarget == nullptr)
    {
        return {};
    }

    lgp::framework::TextureBarrierState const initialState{
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        frameContext.renderTargetInitialLayout,
    };
    lgp::framework::TextureBarrierState constexpr renderTargetState{
        D3D12_BARRIER_SYNC_RENDER_TARGET,
        D3D12_BARRIER_ACCESS_RENDER_TARGET,
        D3D12_BARRIER_LAYOUT_RENDER_TARGET,
    };
    lgp::framework::TransitionTexture(*frameContext.commandList, *frameContext.renderTarget, initialState,
                                      renderTargetState);

    float const clearColor[]{0.035F, 0.055F, 0.085F, 1.0F};
    frameContext.commandList->OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    frameContext.commandList->ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);

    lgp::framework::TextureBarrierState constexpr presentState{
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        D3D12_BARRIER_LAYOUT_PRESENT,
    };
    lgp::framework::TransitionTexture(*frameContext.commandList, *frameContext.renderTarget, renderTargetState,
                                      presentState);
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept {}

} // namespace ch02::rasterization::starter
