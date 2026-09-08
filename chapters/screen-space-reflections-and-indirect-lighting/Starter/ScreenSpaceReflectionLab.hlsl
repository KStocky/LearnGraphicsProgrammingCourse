// Chapter 29 Starter: the honest deferred baseline.
//
// The G-buffer pass has already published depth, normals, roughness, albedo, material identity, and the
// scene-linear outgoing radiance of every visible surface. This pass shades exactly what a deferred renderer with a
// distant environment can say on its own:
//
//     outgoing = directRadiance + environmentRadiance(mirrorDirection) * (F0 * A + B)
//
// It never asks the depth buffer what is actually in the mirror direction, so a bright object standing on the floor
// leaves no reflection at all. Every screen-space field in the diagnostic record stays zero and the miss reason
// stays InvalidInput, because no ray was traced. There is no hidden Solution logic here to switch on later: the
// screen-space traversal, confidence, composition, indirect estimate, and temporal reuse are the work the chapter
// asks the learner to add.
#include "../Common/ScreenSpaceReflectionShared.hlsli"

[numthreads(8, 8, 1)] void ScreenSpaceCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Width || dispatchThreadId.y >= Height)
    {
        return;
    }
    uint2 pixel = dispatchThreadId.xy;
    uint index = pixel.y * Width + pixel.x;
    float2 uv = PixelCenterUv(pixel);
    float deviceDepth = SampleDeviceDepthNearest(uv);
    SurfaceRecord surface = LoadSurface(pixel);

    PixelRecord record = MakeEmptyRecord();
    record.deviceDepth = deviceDepth;
    record.materialId = surface.materialId;
    record.motionX = surface.motionX;
    record.motionY = surface.motionY;
    record.status = StatusGBuffer;

    float3 direct = SurfaceRadiance(surface);
    record.directR = direct.r;
    record.directG = direct.g;
    record.directB = direct.b;

    bool background = surface.materialId == MaterialBackground || IsBackgroundDeviceDepth(deviceDepth);
    if (background)
    {
        // Background pixels carry the environment they already show. They have no surface, so there is nothing to
        // reconstruct and nothing to reflect.
        record.flags |= RecordBackground;
        record.baselineR = direct.r;
        record.baselineG = direct.g;
        record.baselineB = direct.b;
        record.finalR = direct.r;
        record.finalG = direct.g;
        record.finalB = direct.b;
        record.temporalR = direct.r;
        record.temporalG = direct.g;
        record.temporalB = direct.b;
        record.status |= StatusReconstruction | StatusBaseline;
        Diagnostics[index] = record;
        return;
    }

    float viewDepth = ViewDepthFromDeviceDepth(deviceDepth);
    float3 viewPosition = ViewPositionFromUv(uv, viewDepth);
    float3 normal = normalize(float3(surface.normalX, surface.normalY, surface.normalZ));
    float3 viewDirection = normalize(-viewPosition);
    float nDotV = dot(normal, viewDirection);
    record.viewDepth = viewDepth;
    record.viewPositionX = viewPosition.x;
    record.viewPositionY = viewPosition.y;
    record.viewPositionZ = viewPosition.z;
    record.normalX = normal.x;
    record.normalY = normal.y;
    record.normalZ = normal.z;
    record.roughness = surface.roughness;
    record.albedoR = surface.albedoR;
    record.albedoG = surface.albedoG;
    record.albedoB = surface.albedoB;
    record.nDotV = nDotV;
    record.status |= StatusReconstruction;

    float3 mirrorDirection = MirrorDirection(normal, viewDirection);
    float3 environmentRadiance = EvaluateEnvironment(mirrorDirection);
    float2 splitSum = IntegrateSplitSum(nDotV, surface.roughness, SplitSumSampleCount);
    float3 normalIncidenceReflectance = float3(0.04f, 0.04f, 0.04f);
    float3 specularWeight = normalIncidenceReflectance * splitSum.x + splitSum.y;
    float3 baselineReflection = environmentRadiance * specularWeight;
    float3 baseline = direct + baselineReflection;

    record.rayDirectionX = mirrorDirection.x;
    record.rayDirectionY = mirrorDirection.y;
    record.rayDirectionZ = mirrorDirection.z;
    record.towardCameraCosine = dot(mirrorDirection, viewDirection);
    record.environmentRadianceR = environmentRadiance.r;
    record.environmentRadianceG = environmentRadiance.g;
    record.environmentRadianceB = environmentRadiance.b;
    record.splitSumA = splitSum.x;
    record.splitSumB = splitSum.y;
    record.specularWeightR = specularWeight.r;
    record.specularWeightG = specularWeight.g;
    record.specularWeightB = specularWeight.b;
    record.baselineReflectionR = baselineReflection.r;
    record.baselineReflectionG = baselineReflection.g;
    record.baselineReflectionB = baselineReflection.b;
    record.baselineR = baseline.r;
    record.baselineG = baseline.g;
    record.baselineB = baseline.b;
    record.finalR = baseline.r;
    record.finalG = baseline.g;
    record.finalB = baseline.b;
    record.temporalR = baseline.r;
    record.temporalG = baseline.g;
    record.temporalB = baseline.b;
    if (Finite3(baseline) && Finite3(viewPosition))
    {
        record.status |= StatusBaseline;
    }
    Diagnostics[index] = record;
}
