#include "MathTransforms.hpp"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

namespace ch01::graphics_math::starter
{
namespace
{

using DirectX::XMMatrixIdentity;
using DirectX::XMMatrixPerspectiveFovLH;
using DirectX::XMStoreFloat4;
using DirectX::XMStoreFloat4x4;
using DirectX::XMVector4Transform;
using DirectX::XMVectorSet;

} // namespace

DirectX::XMMATRIX BuildModelMatrixStub() noexcept
{
    // TODO: Build translation, rotation, and scale matrices from the UI state.
    return XMMatrixIdentity();
}

DirectX::XMMATRIX BuildPerspectiveProjectionStub(float verticalFieldOfViewDegrees, float aspectRatio, float nearPlane,
                                                 float farPlane) noexcept
{
    float const clampedAspect = aspectRatio > 0.0F ? aspectRatio : 1.0F;
    float const clampedNear = std::max(nearPlane, 0.1F);
    float const clampedFar = std::max(farPlane, clampedNear + 0.1F);
    return XMMatrixPerspectiveFovLH(DegreesToRadians(verticalFieldOfViewDegrees), clampedAspect, clampedNear,
                                    clampedFar);
}

DirectX::XMFLOAT4 TransformPointStub(DirectX::XMFLOAT3 point, DirectX::XMMATRIX transform) noexcept
{
    DirectX::XMFLOAT4 transformedPoint{};
    XMStoreFloat4(&transformedPoint, XMVector4Transform(XMVectorSet(point.x, point.y, point.z, 1.0F), transform));
    return transformedPoint;
}

DirectX::XMFLOAT3 HomogeneousDivideStub(DirectX::XMFLOAT4 clipCoordinates) noexcept
{
    float const reciprocalW = (std::fabs(clipCoordinates.w) > 1.0e-6F) ? (1.0F / clipCoordinates.w) : 0.0F;
    return {
        clipCoordinates.x * reciprocalW,
        clipCoordinates.y * reciprocalW,
        clipCoordinates.z * reciprocalW,
    };
}

DirectX::XMFLOAT2 ViewportMapStub(DirectX::XMFLOAT3 ndcCoordinates, lgp::framework::Extent2D viewport) noexcept
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

DirectX::XMFLOAT4X4 StoreMatrixStub(DirectX::XMMATRIX matrix) noexcept
{
    DirectX::XMFLOAT4X4 storedMatrix{};
    XMStoreFloat4x4(&storedMatrix, matrix);
    return storedMatrix;
}

} // namespace ch01::graphics_math::starter
