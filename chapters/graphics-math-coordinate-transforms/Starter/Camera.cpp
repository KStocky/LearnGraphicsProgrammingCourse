#include "Camera.hpp"

#include <DirectXMath.h>

namespace ch01::graphics_math::starter
{
namespace
{

using DirectX::XMMatrixLookAtLH;
using DirectX::XMStoreFloat3;
using DirectX::XMStoreFloat4x4;
using DirectX::XMVectorSet;

} // namespace

CameraFrame BuildCameraFrame(ChapterControls const &controls) noexcept
{
    // TODO: Derive right, up, and forward from yaw/pitch and rebuild the view matrix by hand.
    DirectX::XMVECTOR const target =
        XMVectorSet(controls.cameraTarget.x, controls.cameraTarget.y, controls.cameraTarget.z, 1.0F);
    DirectX::XMVECTOR const position = XMVectorSet(0.0F, 1.5F, -6.0F, 1.0F);
    DirectX::XMVECTOR const up = XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F);
    DirectX::XMVECTOR const forward = XMVectorSet(0.0F, 0.0F, 1.0F, 0.0F);
    DirectX::XMVECTOR const right = XMVectorSet(1.0F, 0.0F, 0.0F, 0.0F);

    CameraFrame frame{};
    XMStoreFloat3(&frame.position, position);
    XMStoreFloat3(&frame.right, right);
    XMStoreFloat3(&frame.up, up);
    XMStoreFloat3(&frame.forward, forward);
    XMStoreFloat4x4(&frame.viewMatrix, XMMatrixLookAtLH(position, target, up));
    return frame;
}

} // namespace ch01::graphics_math::starter
