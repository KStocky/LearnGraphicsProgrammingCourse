#include "Camera.hpp"

#include "MathTransforms.hpp"

#include "../Common/ChapterGeometry.hpp"

#include <DirectXMath.h>

#include <cmath>

namespace ch01::graphics_math::solution
{
namespace
{

using DirectX::XMMatrixInverse;
using DirectX::XMStoreFloat3;
using DirectX::XMStoreFloat4x4;
using DirectX::XMVector3Cross;
using DirectX::XMVector3Dot;
using DirectX::XMVector3LengthSq;
using DirectX::XMVector3Normalize;
using DirectX::XMVectorGetX;
using DirectX::XMVectorScale;
using DirectX::XMVectorSet;
using DirectX::XMVectorSubtract;

[[nodiscard]] DirectX::XMVECTOR BuildOrbitForward(float yawDegrees, float pitchDegrees) noexcept
{
    float const yawRadians = DegreesToRadians(yawDegrees);
    float const pitchRadians = DegreesToRadians(pitchDegrees);
    float const cosPitch = std::cos(pitchRadians);

    return XMVector3Normalize(
        XMVectorSet(cosPitch * std::sin(yawRadians), std::sin(pitchRadians), cosPitch * std::cos(yawRadians), 0.0F));
}

[[nodiscard]] DirectX::XMMATRIX BuildViewMatrix(DirectX::XMVECTOR right, DirectX::XMVECTOR up,
                                                DirectX::XMVECTOR forward, DirectX::XMVECTOR position) noexcept
{
    DirectX::XMFLOAT3 rightVector{};
    DirectX::XMFLOAT3 upVector{};
    DirectX::XMFLOAT3 forwardVector{};
    XMStoreFloat3(&rightVector, right);
    XMStoreFloat3(&upVector, up);
    XMStoreFloat3(&forwardVector, forward);

    return DirectX::XMMATRIX{rightVector.x,
                             upVector.x,
                             forwardVector.x,
                             0.0F,
                             rightVector.y,
                             upVector.y,
                             forwardVector.y,
                             0.0F,
                             rightVector.z,
                             upVector.z,
                             forwardVector.z,
                             0.0F,
                             -XMVectorGetX(XMVector3Dot(position, right)),
                             -XMVectorGetX(XMVector3Dot(position, up)),
                             -XMVectorGetX(XMVector3Dot(position, forward)),
                             1.0F};
}

} // namespace

CameraFrame BuildCameraFrame(ChapterControls const &controls) noexcept
{
    DirectX::XMVECTOR const target =
        XMVectorSet(controls.cameraTarget.x, controls.cameraTarget.y, controls.cameraTarget.z, 1.0F);
    DirectX::XMVECTOR const forward = BuildOrbitForward(controls.cameraYawDegrees, controls.cameraPitchDegrees);

    DirectX::XMVECTOR right = XMVector3Cross(XMVectorSet(kWorldUp.x, kWorldUp.y, kWorldUp.z, 0.0F), forward);
    if (XMVectorGetX(XMVector3LengthSq(right)) < 1.0e-6F)
    {
        right = XMVectorSet(1.0F, 0.0F, 0.0F, 0.0F);
    }
    right = XMVector3Normalize(right);

    DirectX::XMVECTOR const up = XMVector3Normalize(XMVector3Cross(forward, right));
    DirectX::XMVECTOR const position = XMVectorSubtract(target, XMVectorScale(forward, controls.cameraDistance));

    DirectX::XMMATRIX const viewMatrix = BuildViewMatrix(right, up, forward, position);
    DirectX::XMMATRIX const inverseViewMatrix = XMMatrixInverse(nullptr, viewMatrix);

    CameraFrame frame{};
    XMStoreFloat3(&frame.position, position);
    XMStoreFloat3(&frame.right, right);
    XMStoreFloat3(&frame.up, up);
    XMStoreFloat3(&frame.forward, forward);
    XMStoreFloat4x4(&frame.viewMatrix, viewMatrix);
    XMStoreFloat4x4(&frame.inverseViewMatrix, inverseViewMatrix);
    return frame;
}

} // namespace ch01::graphics_math::solution
