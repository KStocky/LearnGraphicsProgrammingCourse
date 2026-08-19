#include "Scene.hpp"

#include "Camera.hpp"
#include "MathTransforms.hpp"

#include "../Common/ChapterGeometry.hpp"

#include <DirectXMath.h>

namespace ch01::graphics_math::starter
{
namespace
{

using DirectX::XMLoadFloat4x4;
using DirectX::XMMatrixInverse;

} // namespace

lgp::framework::Result<ChapterDerivedScene> BuildScene(ChapterControls const &controls,
                                                       lgp::framework::Extent2D viewport, double)
{
    CameraFrame const cameraFrame = BuildCameraFrame(controls);
    DirectX::XMMATRIX const viewMatrix = XMLoadFloat4x4(&cameraFrame.viewMatrix);
    float const aspectRatio = (viewport.width != 0U && viewport.height != 0U)
                                  ? (static_cast<float>(viewport.width) / static_cast<float>(viewport.height))
                                  : 1.0F;
    DirectX::XMMATRIX const projectionMatrix = BuildPerspectiveProjectionStub(
        controls.verticalFieldOfViewDegrees, aspectRatio, controls.nearPlane, controls.farPlane);
    DirectX::XMMATRIX const modelMatrix = BuildModelMatrixStub();
    DirectX::XMMATRIX const viewProjectionMatrix = viewMatrix * projectionMatrix;
    DirectX::XMMATRIX const modelViewProjectionMatrix = modelMatrix * viewProjectionMatrix;

    ChapterDerivedScene scene{};
    scene.capabilities = {
        false, false, false, false, false, false, false, false, false, false,
    };
    scene.implementationNote = "Starter: the host plumbing and debug view are live, but the learner-owned transform "
                               "chain is intentionally stubbed. "
                               "Implement Camera.cpp, MathTransforms.cpp, and Scene.cpp to match the Solution.";
    scene.model = StoreMatrixStub(modelMatrix);
    scene.comparisonModel = StoreMatrixStub(modelMatrix);
    scene.wrongOrderModel = StoreMatrixStub(modelMatrix);
    scene.view = cameraFrame.viewMatrix;
    scene.projection = StoreMatrixStub(projectionMatrix);
    scene.viewProjection = StoreMatrixStub(viewProjectionMatrix);
    scene.inverseViewProjection = StoreMatrixStub(XMMatrixInverse(nullptr, viewProjectionMatrix));
    scene.normalMatrix = StoreMatrixStub(modelMatrix);
    scene.incorrectNormalMatrix = StoreMatrixStub(modelMatrix);
    scene.cameraPosition = cameraFrame.position;
    scene.cameraRight = cameraFrame.right;
    scene.cameraUp = cameraFrame.up;
    scene.cameraForward = cameraFrame.forward;

    auto const objectVertices = ObjectVertices();
    for (std::size_t vertexIndex = 0; vertexIndex < objectVertices.size(); ++vertexIndex)
    {
        scene.worldVertices[vertexIndex] = TransformPointStub(objectVertices[vertexIndex], modelMatrix);
        scene.viewVertices[vertexIndex] = TransformPointStub(
            {
                scene.worldVertices[vertexIndex].x,
                scene.worldVertices[vertexIndex].y,
                scene.worldVertices[vertexIndex].z,
            },
            viewMatrix);
        scene.clipVertices[vertexIndex] = TransformPointStub(objectVertices[vertexIndex], modelViewProjectionMatrix);
        scene.ndcVertices[vertexIndex] = HomogeneousDivideStub(scene.clipVertices[vertexIndex]);
        scene.screenVertices[vertexIndex] = ViewportMapStub(scene.ndcVertices[vertexIndex], viewport);
    }

    int const selectedVertex = ClampVertexIndex(controls.selectedVertex);
    scene.inspection.object = {
        objectVertices[static_cast<std::size_t>(selectedVertex)].x,
        objectVertices[static_cast<std::size_t>(selectedVertex)].y,
        objectVertices[static_cast<std::size_t>(selectedVertex)].z,
        1.0F,
    };
    scene.inspection.world = scene.worldVertices[static_cast<std::size_t>(selectedVertex)];
    scene.inspection.view = scene.viewVertices[static_cast<std::size_t>(selectedVertex)];
    scene.inspection.clip = scene.clipVertices[static_cast<std::size_t>(selectedVertex)];
    scene.inspection.ndc = scene.ndcVertices[static_cast<std::size_t>(selectedVertex)];
    scene.inspection.screen = scene.screenVertices[static_cast<std::size_t>(selectedVertex)];
    scene.inspection.screenWithoutDivide = scene.inspection.screen;
    scene.normalOriginWorld = {
        scene.inspection.world.x,
        scene.inspection.world.y,
        scene.inspection.world.z,
    };
    scene.correctNormalWorld = kReferenceNormalObject;
    scene.incorrectNormalWorld = kReferenceNormalObject;

    return scene;
}

} // namespace ch01::graphics_math::starter
