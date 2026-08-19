#include "Renderer.hpp"

#include <d3d12.h>

namespace ch03::texture::starter
{
namespace
{

void ClearRenderTarget(lgp::framework::FrameContext const &frameContext) noexcept
{
    if (frameContext.commandList == nullptr || frameContext.renderTarget == nullptr)
    {
        return;
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

    float const clearColor[]{0.025F, 0.040F, 0.065F, 1.0F};
    frameContext.commandList->OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    frameContext.commandList->ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);

    lgp::framework::TextureBarrierState const endState{
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT,
    };
    lgp::framework::TransitionTexture(*frameContext.commandList, *frameContext.renderTarget, renderTargetState,
                                      endState);
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
    ClearRenderTarget(frameContext);
    return {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &) noexcept {}

} // namespace ch03::texture::starter
