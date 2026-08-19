#pragma once

#include "../Common/ChapterTypes.hpp"

#include <DirectXMath.h>

namespace ch01::graphics_math::solution
{

[[nodiscard]] DirectX::XMFLOAT3 DegreesVectorToRadians(DirectX::XMFLOAT3 degrees) noexcept;
[[nodiscard]] DirectX::XMMATRIX BuildEulerRotationMatrixFromDegrees(DirectX::XMFLOAT3 degrees) noexcept;
[[nodiscard]] DirectX::XMVECTOR BuildQuaternionFromDegrees(DirectX::XMFLOAT3 degrees) noexcept;
[[nodiscard]] DirectX::XMMATRIX BuildQuaternionRotationMatrixFromDegrees(DirectX::XMFLOAT3 degrees) noexcept;
[[nodiscard]] DirectX::XMVECTOR SlerpQuaternion(DirectX::XMVECTOR start, DirectX::XMVECTOR end,
                                                float interpolation) noexcept;
[[nodiscard]] DirectX::XMMATRIX BuildSlerpRotationMatrixFromDegrees(DirectX::XMFLOAT3 startDegrees,
                                                                    DirectX::XMFLOAT3 endDegrees,
                                                                    float interpolation) noexcept;
[[nodiscard]] DirectX::XMMATRIX ComposeModelMatrix(DirectX::XMFLOAT3 translation, DirectX::XMFLOAT3 scale,
                                                   DirectX::XMMATRIX rotationMatrix) noexcept;
[[nodiscard]] DirectX::XMMATRIX ComposeWrongOrderModelMatrix(DirectX::XMFLOAT3 translation, DirectX::XMFLOAT3 scale,
                                                             DirectX::XMMATRIX rotationMatrix) noexcept;
[[nodiscard]] DirectX::XMMATRIX BuildPerspectiveProjection(float verticalFieldOfViewDegrees, float aspectRatio,
                                                           float nearPlane, float farPlane) noexcept;
[[nodiscard]] DirectX::XMMATRIX BuildOrthographicProjection(float height, float aspectRatio, float nearPlane,
                                                            float farPlane) noexcept;
[[nodiscard]] DirectX::XMFLOAT4 TransformPoint(DirectX::XMFLOAT3 point, DirectX::XMMATRIX transform) noexcept;
[[nodiscard]] DirectX::XMFLOAT3 TransformDirection(DirectX::XMFLOAT3 direction, DirectX::XMMATRIX transform) noexcept;
[[nodiscard]] DirectX::XMFLOAT3 HomogeneousDivide(DirectX::XMFLOAT4 clipCoordinates) noexcept;
[[nodiscard]] DirectX::XMFLOAT2 ViewportMap(DirectX::XMFLOAT3 ndcCoordinates,
                                            lgp::framework::Extent2D viewport) noexcept;
[[nodiscard]] DirectX::XMMATRIX BuildNormalMatrix(DirectX::XMMATRIX modelMatrix) noexcept;
[[nodiscard]] DirectX::XMFLOAT4X4 StoreMatrix(DirectX::XMMATRIX matrix) noexcept;

} // namespace ch01::graphics_math::solution
