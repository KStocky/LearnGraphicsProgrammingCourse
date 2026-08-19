#include "Scene.hpp"

#include "Camera.hpp"
#include "MathTransforms.hpp"

#include "../Common/ChapterGeometry.hpp"

#include <DirectXMath.h>

#include <algorithm>
#include <string>

namespace ch01::graphics_math::solution
{
namespace
{

using DirectX::XMLoadFloat4x4;
using DirectX::XMMatrixIdentity;
using DirectX::XMStoreFloat3;
using DirectX::XMVector3Normalize;
using DirectX::XMVectorScale;
using DirectX::XMVectorSet;

[[nodiscard]] XMFLOAT3 ClampScale(XMFLOAT3 scale) noexcept
{
    auto const clampAxis = [](float value) noexcept
    {
        float const sign = value < 0.0F ? -1.0F : 1.0F;
        float const magnitude = std::max(std::abs(value), 0.05F);
        return sign * magnitude;
    };

    return {clampAxis(scale.x), clampAxis(scale.y), clampAxis(scale.z)};
}

[[nodiscard]] DirectX::XMMATRIX BuildIncorrectNormalMatrix(DirectX::XMMATRIX modelMatrix) noexcept
{
    DirectX::XMMATRIX incorrectNormalMatrix = modelMatrix;
    incorrectNormalMatrix.r[3] = XMVectorSet(0.0F, 0.0F, 0.0F, 1.0F);
    return incorrectNormalMatrix;
}

[[nodiscard]] DirectX::XMMATRIX ChooseProjection(ChapterControls const &controls, float aspectRatio) noexcept
{
    if (controls.projectionMode == ProjectionMode::Orthographic)
    {
        return BuildOrthographicProjection(controls.orthographicHeight, aspectRatio, controls.nearPlane,
                                           controls.farPlane);
    }

    return BuildPerspectiveProjection(controls.verticalFieldOfViewDegrees, aspectRatio, controls.nearPlane,
                                      controls.farPlane);
}

[[nodiscard]] DirectX::XMMATRIX BuildComparisonModelMatrix(ChapterControls const &controls) noexcept
{
    switch (controls.orientationMode)
    {
    case OrientationMode::Euler:
        return ComposeModelMatrix(controls.translation, controls.scale,
                                  BuildQuaternionRotationMatrixFromDegrees(controls.quaternionDegrees));
    case OrientationMode::Quaternion:
        return ComposeModelMatrix(controls.translation, controls.scale,
                                  BuildEulerRotationMatrixFromDegrees(controls.eulerDegrees));
    case OrientationMode::Slerp:
        return ComposeModelMatrix(controls.translation, controls.scale,
                                  BuildQuaternionRotationMatrixFromDegrees(controls.slerpEndDegrees));
    }

    return XMMatrixIdentity();
}

} // namespace

lgp::framework::Result<ChapterDerivedScene> BuildScene(ChapterControls const &controls,
                                                       lgp::framework::Extent2D viewport, double)
{
    ChapterControls sanitizedControls = controls;
    sanitizedControls.selectedVertex = ClampVertexIndex(controls.selectedVertex);
    sanitizedControls.scale = ClampScale(controls.scale);

    CameraFrame const cameraFrame = BuildCameraFrame(sanitizedControls);
    DirectX::XMMATRIX const viewMatrix = XMLoadFloat4x4(&cameraFrame.viewMatrix);
    float const aspectRatio = (viewport.width != 0U && viewport.height != 0U)
                                  ? (static_cast<float>(viewport.width) / static_cast<float>(viewport.height))
                                  : 1.0F;
    DirectX::XMMATRIX const projectionMatrix = ChooseProjection(sanitizedControls, aspectRatio);

    DirectX::XMMATRIX const eulerRotationMatrix = BuildEulerRotationMatrixFromDegrees(sanitizedControls.eulerDegrees);
    DirectX::XMMATRIX const quaternionRotationMatrix =
        BuildQuaternionRotationMatrixFromDegrees(sanitizedControls.quaternionDegrees);
    DirectX::XMMATRIX const slerpRotationMatrix = BuildSlerpRotationMatrixFromDegrees(
        sanitizedControls.slerpStartDegrees, sanitizedControls.slerpEndDegrees, sanitizedControls.slerpT);

    DirectX::XMMATRIX activeRotationMatrix = eulerRotationMatrix;
    switch (sanitizedControls.orientationMode)
    {
    case OrientationMode::Euler:
        activeRotationMatrix = eulerRotationMatrix;
        break;
    case OrientationMode::Quaternion:
        activeRotationMatrix = quaternionRotationMatrix;
        break;
    case OrientationMode::Slerp:
        activeRotationMatrix = slerpRotationMatrix;
        break;
    }

    DirectX::XMMATRIX const modelMatrix =
        ComposeModelMatrix(sanitizedControls.translation, sanitizedControls.scale, activeRotationMatrix);
    DirectX::XMMATRIX const wrongOrderModelMatrix =
        ComposeWrongOrderModelMatrix(sanitizedControls.translation, sanitizedControls.scale, activeRotationMatrix);
    DirectX::XMMATRIX const comparisonModelMatrix = BuildComparisonModelMatrix(sanitizedControls);
    DirectX::XMMATRIX const viewProjectionMatrix = viewMatrix * projectionMatrix;
    DirectX::XMMATRIX const normalMatrix = BuildNormalMatrix(modelMatrix);
    DirectX::XMMATRIX const incorrectNormalMatrix = BuildIncorrectNormalMatrix(modelMatrix);

    ChapterDerivedScene scene{};
    scene.capabilities = {
        true, true, true, true, true, true, true, true, true, true,
    };
    scene.implementationNote =
        "Solution: full CPU-side transform chain, camera basis, projection modes, quaternion comparison, slerp, "
        "normal inverse-transpose, frustum overlay, and perspective-divide diagnostics are active.";
    scene.model = StoreMatrix(modelMatrix);
    scene.comparisonModel = StoreMatrix(comparisonModelMatrix);
    scene.wrongOrderModel = StoreMatrix(wrongOrderModelMatrix);
    scene.view = cameraFrame.viewMatrix;
    scene.projection = StoreMatrix(projectionMatrix);
    scene.viewProjection = StoreMatrix(viewProjectionMatrix);
    scene.inverseViewProjection = StoreMatrix(DirectX::XMMatrixInverse(nullptr, viewProjectionMatrix));
    scene.normalMatrix = StoreMatrix(normalMatrix);
    scene.incorrectNormalMatrix = StoreMatrix(incorrectNormalMatrix);
    scene.cameraPosition = cameraFrame.position;
    scene.cameraRight = cameraFrame.right;
    scene.cameraUp = cameraFrame.up;
    scene.cameraForward = cameraFrame.forward;
    scene.hasComparisonModel = sanitizedControls.showComparisonOrientation;
    scene.hasWrongOrderModel = sanitizedControls.showWrongOrder;

    std::span<XMFLOAT3 const> const objectVertices = ObjectVertices();
    DirectX::XMMATRIX const modelViewMatrix = modelMatrix * viewMatrix;
    DirectX::XMMATRIX const modelViewProjectionMatrix = modelViewMatrix * projectionMatrix;

    for (std::size_t vertexIndex = 0; vertexIndex < objectVertices.size(); ++vertexIndex)
    {
        XMFLOAT3 const objectVertex = objectVertices[vertexIndex];
        scene.worldVertices[vertexIndex] = TransformPoint(objectVertex, modelMatrix);
        scene.viewVertices[vertexIndex] =
            TransformPoint({scene.worldVertices[vertexIndex].x, scene.worldVertices[vertexIndex].y,
                            scene.worldVertices[vertexIndex].z},
                           viewMatrix);
        scene.clipVertices[vertexIndex] = TransformPoint(objectVertex, modelViewProjectionMatrix);
        scene.ndcVertices[vertexIndex] = HomogeneousDivide(scene.clipVertices[vertexIndex]);
        scene.screenVertices[vertexIndex] = ViewportMap(scene.ndcVertices[vertexIndex], viewport);
    }

    int const selectedVertex = sanitizedControls.selectedVertex;
    XMFLOAT3 const selectedObjectVertex = objectVertices[static_cast<std::size_t>(selectedVertex)];

    scene.inspection.object = {selectedObjectVertex.x, selectedObjectVertex.y, selectedObjectVertex.z, 1.0F};
    scene.inspection.world = scene.worldVertices[static_cast<std::size_t>(selectedVertex)];
    scene.inspection.view = scene.viewVertices[static_cast<std::size_t>(selectedVertex)];
    scene.inspection.clip = scene.clipVertices[static_cast<std::size_t>(selectedVertex)];
    scene.inspection.ndc = scene.ndcVertices[static_cast<std::size_t>(selectedVertex)];
    scene.inspection.screen = scene.screenVertices[static_cast<std::size_t>(selectedVertex)];
    scene.inspection.screenWithoutDivide = ViewportMap(
        {
            scene.inspection.clip.x,
            scene.inspection.clip.y,
            scene.inspection.clip.z,
        },
        viewport);

    scene.normalOriginWorld = {
        scene.inspection.world.x,
        scene.inspection.world.y,
        scene.inspection.world.z,
    };

    DirectX::XMFLOAT3 correctNormal = TransformDirection(kReferenceNormalObject, normalMatrix);
    DirectX::XMFLOAT3 incorrectNormal = TransformDirection(kReferenceNormalObject, incorrectNormalMatrix);

    DirectX::XMVECTOR const correctNormalVector =
        XMVector3Normalize(XMVectorSet(correctNormal.x, correctNormal.y, correctNormal.z, 0.0F));
    DirectX::XMVECTOR const incorrectNormalVector =
        XMVector3Normalize(XMVectorSet(incorrectNormal.x, incorrectNormal.y, incorrectNormal.z, 0.0F));

    XMStoreFloat3(&scene.correctNormalWorld, XMVectorScale(correctNormalVector, 1.25F));
    XMStoreFloat3(&scene.incorrectNormalWorld, XMVectorScale(incorrectNormalVector, 1.25F));

    return scene;
}

} // namespace ch01::graphics_math::solution
