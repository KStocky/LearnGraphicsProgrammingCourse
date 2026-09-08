// Chapter 28 Starter: deliberately spatial-only reduced-resolution reconstruction.
#include "../Common/TemporalAaShared.hlsli"

[numthreads(8, 8, 1)] void TemporalCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Width || dispatchThreadId.y >= Height) return;
    uint2 pixel = dispatchThreadId.xy;
    uint index = pixel.y * Width + pixel.x;
    float2 jitterPixels = (Flags & 4u) != 0u ? CentroidCorrectedHalton(AnimationFrame, JitterPhasePeriod) : 0.0f;
    float2 priorJitterPixels =
        (Flags & 4u) != 0u ? CentroidCorrectedHalton(PreviousAnimationFrame, JitterPhasePeriod) : 0.0f;
    float2 displayUv = (float2(pixel) + 0.5f) / float2(Width, Height);
    SceneValue scene = EvaluateScene(displayUv + jitterPixels / float2(RenderWidth, RenderHeight), AnimationFrame);
    float3 spatial = SpatialBilinear(pixel, AnimationFrame, jitterPixels) * CurrentPreExposure;
    float3 nativeSpatial =
        SpatialBilinearAtExtent(pixel, AnimationFrame, jitterPixels, uint2(Width, Height)) * CurrentPreExposure;

    PixelStatistics output = (PixelStatistics)0;
    output.currentR = nativeSpatial.r;
    output.currentG = nativeSpatial.g;
    output.currentB = nativeSpatial.b;
    output.currentLuminance = Luminance(nativeSpatial);
    output.spatialR = spatial.r;
    output.spatialG = spatial.g;
    output.spatialB = spatial.b;
    output.currentDepth = scene.depth;
    output.currentJitterX = jitterPixels.x;
    output.currentJitterY = jitterPixels.y;
    output.previousJitterX = priorJitterPixels.x;
    output.previousJitterY = priorJitterPixels.y;
    output.rejectionReasons = RejectNoHistory;
    output.nextSampleCount = 1u;
    output.currentObjectId = scene.objectId;
    output.status = 3u;
    output.renderWidth = RenderWidth;
    output.renderHeight = RenderHeight;
    Statistics[index] = output;
}
