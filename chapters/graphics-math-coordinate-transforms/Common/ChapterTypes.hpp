#pragma once

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <lgp/framework/device_resources.hpp>

namespace ch01::graphics_math
{

using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;
using DirectX::XMFLOAT4X4;

inline constexpr std::size_t kObjectVertexCount = 8U;
inline constexpr float kPi = 3.14159265358979323846F;

enum class InspectionSpace : std::uint8_t
{
    Object = 0,
    World,
    View,
    Clip,
    Ndc,
    Screen,
};

enum class ProjectionMode : std::uint8_t
{
    Perspective = 0,
    Orthographic,
};

enum class OrientationMode : std::uint8_t
{
    Euler = 0,
    Quaternion,
    Slerp,
};

struct VariantCapabilities final
{
    bool transformComposition{};
    bool projectionModes{};
    bool cameraBasis{};
    bool homogeneousDivide{};
    bool viewportMapping{};
    bool normalMatrix{};
    bool quaternionOrientation{};
    bool quaternionSlerp{};
    bool frustumOverlay{};
    bool wrongOrderDemo{};
};

struct ChapterControls final
{
    XMFLOAT3 translation{0.0F, 0.0F, 0.0F};
    XMFLOAT3 scale{1.0F, 1.0F, 1.0F};
    XMFLOAT3 eulerDegrees{20.0F, 35.0F, 0.0F};
    XMFLOAT3 quaternionDegrees{20.0F, 35.0F, 0.0F};
    XMFLOAT3 slerpStartDegrees{0.0F, 0.0F, 0.0F};
    XMFLOAT3 slerpEndDegrees{0.0F, 135.0F, 0.0F};
    float slerpT{0.35F};
    bool animateSlerp{true};
    float slerpCyclesPerSecond{0.125F};
    ProjectionMode projectionMode{ProjectionMode::Perspective};
    float verticalFieldOfViewDegrees{60.0F};
    float orthographicHeight{4.0F};
    float nearPlane{0.1F};
    float farPlane{20.0F};
    float cameraYawDegrees{30.0F};
    float cameraPitchDegrees{18.0F};
    float cameraDistance{6.0F};
    XMFLOAT3 cameraTarget{0.0F, 0.0F, 0.0F};
    OrientationMode orientationMode{OrientationMode::Euler};
    InspectionSpace inspectionSpace{InspectionSpace::World};
    int selectedVertex{6};
    bool showObjectAxes{true};
    bool showWorldAxes{true};
    bool showCameraAxes{true};
    bool showFrustum{true};
    bool showWrongOrder{true};
    bool showComparisonOrientation{true};
    bool showNormalVectors{true};
    bool demonstrateIncorrectNormal{true};
    bool omitPerspectiveDivide{true};
};

struct InspectionCoordinates final
{
    XMFLOAT4 object{};
    XMFLOAT4 world{};
    XMFLOAT4 view{};
    XMFLOAT4 clip{};
    XMFLOAT3 ndc{};
    XMFLOAT2 screen{};
    XMFLOAT2 screenWithoutDivide{};
};

struct ChapterDerivedScene final
{
    VariantCapabilities capabilities{};
    std::string implementationNote;
    XMFLOAT4X4 model{};
    XMFLOAT4X4 comparisonModel{};
    XMFLOAT4X4 wrongOrderModel{};
    XMFLOAT4X4 view{};
    XMFLOAT4X4 projection{};
    XMFLOAT4X4 viewProjection{};
    XMFLOAT4X4 inverseViewProjection{};
    XMFLOAT4X4 normalMatrix{};
    XMFLOAT4X4 incorrectNormalMatrix{};
    XMFLOAT3 cameraPosition{};
    XMFLOAT3 cameraRight{};
    XMFLOAT3 cameraUp{};
    XMFLOAT3 cameraForward{};
    std::array<XMFLOAT4, kObjectVertexCount> worldVertices{};
    std::array<XMFLOAT4, kObjectVertexCount> viewVertices{};
    std::array<XMFLOAT4, kObjectVertexCount> clipVertices{};
    std::array<XMFLOAT3, kObjectVertexCount> ndcVertices{};
    std::array<XMFLOAT2, kObjectVertexCount> screenVertices{};
    InspectionCoordinates inspection{};
    XMFLOAT3 normalOriginWorld{};
    XMFLOAT3 correctNormalWorld{};
    XMFLOAT3 incorrectNormalWorld{};
    bool hasComparisonModel{};
    bool hasWrongOrderModel{};
};

[[nodiscard]] constexpr float DegreesToRadians(float degrees) noexcept
{
    return degrees * (kPi / 180.0F);
}

[[nodiscard]] constexpr int ClampVertexIndex(int vertexIndex) noexcept
{
    if (vertexIndex < 0)
    {
        return 0;
    }

    if (vertexIndex >= static_cast<int>(kObjectVertexCount))
    {
        return static_cast<int>(kObjectVertexCount - 1U);
    }

    return vertexIndex;
}

[[nodiscard]] std::string_view InspectionSpaceName(InspectionSpace space) noexcept;
[[nodiscard]] std::string_view ProjectionModeName(ProjectionMode mode) noexcept;
[[nodiscard]] std::string_view OrientationModeName(OrientationMode mode) noexcept;

} // namespace ch01::graphics_math
