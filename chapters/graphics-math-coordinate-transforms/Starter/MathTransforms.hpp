#pragma once

#include "../Common/ChapterTypes.hpp"

#include <DirectXMath.h>

namespace ch01::graphics_math::starter
{

[[nodiscard]] DirectX::XMMATRIX BuildModelMatrixStub() noexcept;
[[nodiscard]] DirectX::XMMATRIX BuildPerspectiveProjectionStub(float verticalFieldOfViewDegrees, float aspectRatio,
                                                               float nearPlane, float farPlane) noexcept;
[[nodiscard]] DirectX::XMFLOAT4 TransformPointStub(DirectX::XMFLOAT3 point, DirectX::XMMATRIX transform) noexcept;
[[nodiscard]] DirectX::XMFLOAT3 HomogeneousDivideStub(DirectX::XMFLOAT4 clipCoordinates) noexcept;
[[nodiscard]] DirectX::XMFLOAT2 ViewportMapStub(DirectX::XMFLOAT3 ndcCoordinates,
                                                lgp::framework::Extent2D viewport) noexcept;
[[nodiscard]] DirectX::XMFLOAT4X4 StoreMatrixStub(DirectX::XMMATRIX matrix) noexcept;

} // namespace ch01::graphics_math::starter
