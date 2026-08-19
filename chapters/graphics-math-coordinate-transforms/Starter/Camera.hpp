#pragma once

#include "../Common/ChapterTypes.hpp"

#include <DirectXMath.h>

namespace ch01::graphics_math::starter
{

struct CameraFrame final
{
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 right{};
    DirectX::XMFLOAT3 up{};
    DirectX::XMFLOAT3 forward{};
    DirectX::XMFLOAT4X4 viewMatrix{};
};

[[nodiscard]] CameraFrame BuildCameraFrame(ChapterControls const &controls) noexcept;

} // namespace ch01::graphics_math::starter
