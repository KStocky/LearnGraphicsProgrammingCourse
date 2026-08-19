#include "MathTransforms.hpp"

#include <algorithm>
#include <cmath>

namespace ch01::graphics_math::solution
{
namespace
{

using DirectX::XMMatrixIdentity;
using DirectX::XMMatrixInverse;
using DirectX::XMMatrixOrthographicLH;
using DirectX::XMMatrixPerspectiveFovLH;
using DirectX::XMMatrixRotationQuaternion;
using DirectX::XMMatrixRotationRollPitchYaw;
using DirectX::XMMatrixScaling;
using DirectX::XMMatrixTranslation;
using DirectX::XMMatrixTranspose;
using DirectX::XMQuaternionNormalize;
using DirectX::XMQuaternionRotationRollPitchYaw;
using DirectX::XMStoreFloat3;
using DirectX::XMStoreFloat4;
using DirectX::XMStoreFloat4x4;
using DirectX::XMVector3Normalize;
using DirectX::XMVector4Transform;
using DirectX::XMVectorSet;

} // namespace

DirectX::XMFLOAT3 DegreesVectorToRadians(DirectX::XMFLOAT3 degrees) noexcept
{
    return {
        DegreesToRadians(degrees.x),
        DegreesToRadians(degrees.y),
        DegreesToRadians(degrees.z),
    };
}

DirectX::XMMATRIX BuildEulerRotationMatrixFromDegrees(DirectX::XMFLOAT3 degrees) noexcept
{
    DirectX::XMFLOAT3 const radians = DegreesVectorToRadians(degrees);
    return XMMatrixRotationRollPitchYaw(radians.x, radians.y, radians.z);
}

DirectX::XMVECTOR BuildQuaternionFromDegrees(DirectX::XMFLOAT3 degrees) noexcept
{
    DirectX::XMFLOAT3 const radians = DegreesVectorToRadians(degrees);
    return XMQuaternionNormalize(XMQuaternionRotationRollPitchYaw(radians.x, radians.y, radians.z));
}

DirectX::XMMATRIX BuildQuaternionRotationMatrixFromDegrees(DirectX::XMFLOAT3 degrees) noexcept
{
    return XMMatrixRotationQuaternion(BuildQuaternionFromDegrees(degrees));
}

DirectX::XMVECTOR SlerpQuaternion(DirectX::XMVECTOR start, DirectX::XMVECTOR end, float interpolation) noexcept
{
    float const clampedInterpolation = std::clamp(interpolation, 0.0F, 1.0F);
    return DirectX::XMQuaternionSlerp(start, end, clampedInterpolation);
}

DirectX::XMMATRIX BuildSlerpRotationMatrixFromDegrees(DirectX::XMFLOAT3 startDegrees, DirectX::XMFLOAT3 endDegrees,
                                                      float interpolation) noexcept
{
    return XMMatrixRotationQuaternion(SlerpQuaternion(BuildQuaternionFromDegrees(startDegrees),
                                                      BuildQuaternionFromDegrees(endDegrees), interpolation));
}

DirectX::XMMATRIX ComposeModelMatrix(DirectX::XMFLOAT3 translation, DirectX::XMFLOAT3 scale,
                                     DirectX::XMMATRIX rotationMatrix) noexcept
{
    return XMMatrixScaling(scale.x, scale.y, scale.z) * rotationMatrix *
           XMMatrixTranslation(translation.x, translation.y, translation.z);
}

DirectX::XMMATRIX ComposeWrongOrderModelMatrix(DirectX::XMFLOAT3 translation, DirectX::XMFLOAT3 scale,
                                               DirectX::XMMATRIX rotationMatrix) noexcept
{
    return XMMatrixTranslation(translation.x, translation.y, translation.z) * rotationMatrix *
           XMMatrixScaling(scale.x, scale.y, scale.z);
}

DirectX::XMMATRIX BuildPerspectiveProjection(float verticalFieldOfViewDegrees, float aspectRatio, float nearPlane,
                                             float farPlane) noexcept
{
    float const clampedAspect = aspectRatio > 0.0F ? aspectRatio : 1.0F;
    float const clampedNear = std::max(nearPlane, 0.001F);
    float const clampedFar = std::max(farPlane, clampedNear + 0.001F);
    float const clampedFieldOfView = std::clamp(verticalFieldOfViewDegrees, 5.0F, 160.0F);
    return XMMatrixPerspectiveFovLH(DegreesToRadians(clampedFieldOfView), clampedAspect, clampedNear, clampedFar);
}

DirectX::XMMATRIX BuildOrthographicProjection(float height, float aspectRatio, float nearPlane, float farPlane) noexcept
{
    float const clampedHeight = std::max(height, 0.05F);
    float const clampedAspect = aspectRatio > 0.0F ? aspectRatio : 1.0F;
    float const clampedNear = std::max(nearPlane, 0.001F);
    float const clampedFar = std::max(farPlane, clampedNear + 0.001F);
    return XMMatrixOrthographicLH(clampedHeight * clampedAspect, clampedHeight, clampedNear, clampedFar);
}

DirectX::XMFLOAT4 TransformPoint(DirectX::XMFLOAT3 point, DirectX::XMMATRIX transform) noexcept
{
    DirectX::XMFLOAT4 transformedPoint{};
    XMStoreFloat4(&transformedPoint, XMVector4Transform(XMVectorSet(point.x, point.y, point.z, 1.0F), transform));
    return transformedPoint;
}

DirectX::XMFLOAT3 TransformDirection(DirectX::XMFLOAT3 direction, DirectX::XMMATRIX transform) noexcept
{
    DirectX::XMFLOAT3 transformedDirection{};
    XMStoreFloat3(&transformedDirection,
                  XMVector4Transform(XMVectorSet(direction.x, direction.y, direction.z, 0.0F), transform));
    return transformedDirection;
}

DirectX::XMFLOAT3 HomogeneousDivide(DirectX::XMFLOAT4 clipCoordinates) noexcept
{
    float const reciprocalW = (std::fabs(clipCoordinates.w) > 1.0e-6F) ? (1.0F / clipCoordinates.w) : 0.0F;
    return {
        clipCoordinates.x * reciprocalW,
        clipCoordinates.y * reciprocalW,
        clipCoordinates.z * reciprocalW,
    };
}

DirectX::XMFLOAT2 ViewportMap(DirectX::XMFLOAT3 ndcCoordinates, lgp::framework::Extent2D viewport) noexcept
{
    if (viewport.width == 0U || viewport.height == 0U)
    {
        return {};
    }

    return {
        (ndcCoordinates.x * 0.5F + 0.5F) * static_cast<float>(viewport.width),
        (1.0F - (ndcCoordinates.y * 0.5F + 0.5F)) * static_cast<float>(viewport.height),
    };
}

DirectX::XMMATRIX BuildNormalMatrix(DirectX::XMMATRIX modelMatrix) noexcept
{
    DirectX::XMMATRIX linearTransform = modelMatrix;
    linearTransform.r[3] = XMVectorSet(0.0F, 0.0F, 0.0F, 1.0F);
    return XMMatrixTranspose(XMMatrixInverse(nullptr, linearTransform));
}

DirectX::XMFLOAT4X4 StoreMatrix(DirectX::XMMATRIX matrix) noexcept
{
    DirectX::XMFLOAT4X4 storedMatrix{};
    XMStoreFloat4x4(&storedMatrix, matrix);
    return storedMatrix;
}

} // namespace ch01::graphics_math::solution
