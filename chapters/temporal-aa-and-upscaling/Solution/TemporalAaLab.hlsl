// Chapter 28 Solution: scene-linear temporal resolve and temporal upscaling.
#include "../Common/TemporalAaShared.hlsli"

HistoryPixel GatherHistory(float2 uv, out bool footprintValid)
{
    float2 minimumUv = 0.5f / float2(Width, Height);
    float2 maximumUv = 1.0f - minimumUv;
    footprintValid = all(uv >= minimumUv) && all(uv <= maximumUv);
    float2 coordinate = uv * float2(Width, Height) - 0.5f;
    int2 base = int2(floor(coordinate));
    float2 f = frac(coordinate);
    uint2 p00 = uint2(clamp(base, int2(0, 0), int2(Width - 1u, Height - 1u)));
    uint2 p10 = uint2(clamp(base + int2(1, 0), int2(0, 0), int2(Width - 1u, Height - 1u)));
    uint2 p01 = uint2(clamp(base + int2(0, 1), int2(0, 0), int2(Width - 1u, Height - 1u)));
    uint2 p11 = uint2(clamp(base + int2(1, 1), int2(0, 0), int2(Width - 1u, Height - 1u)));
    HistoryPixel a = HistoryRead[p00.y * Width + p00.x];
    HistoryPixel b = HistoryRead[p10.y * Width + p10.x];
    HistoryPixel c = HistoryRead[p01.y * Width + p01.x];
    HistoryPixel d = HistoryRead[p11.y * Width + p11.x];
    float4 weights = float4((1.0f - f.x) * (1.0f - f.y), f.x * (1.0f - f.y),
                            (1.0f - f.x) * f.y, f.x * f.y);
    HistoryPixel result = a;
    result.colorR = dot(float4(a.colorR, b.colorR, c.colorR, d.colorR), weights);
    result.colorG = dot(float4(a.colorG, b.colorG, c.colorG, d.colorG), weights);
    result.colorB = dot(float4(a.colorB, b.colorB, c.colorB, d.colorB), weights);
    result.luminance = dot(float4(a.luminance, b.luminance, c.luminance, d.luminance), weights);
    int2 nearest = int2(round(coordinate));
    nearest = clamp(nearest, int2(0, 0), int2(Width - 1u, Height - 1u));
    HistoryPixel metadata = HistoryRead[uint(nearest.y) * Width + uint(nearest.x)];
    result.depth = metadata.depth;
    result.normalX = metadata.normalX;
    result.normalY = metadata.normalY;
    result.normalZ = metadata.normalZ;
    result.objectId = metadata.objectId;
    result.sampleCount = metadata.sampleCount;
    return result;
}

[numthreads(8, 8, 1)] void TemporalCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Width || dispatchThreadId.y >= Height) return;
    uint2 pixel = dispatchThreadId.xy;
    uint index = pixel.y * Width + pixel.x;
    bool canSampleHistory = (Flags & 1u) != 0u;
    bool hasHistory = (Flags & 8u) != 0u;
    bool reset = (Flags & 2u) != 0u;
    float2 jitterPixels = (Flags & 4u) != 0u ? CentroidCorrectedHalton(AnimationFrame, JitterPhasePeriod) : 0.0f;
    float2 priorJitterPixels =
        (Flags & 4u) != 0u ? CentroidCorrectedHalton(PreviousAnimationFrame, JitterPhasePeriod) : 0.0f;
    float2 displayUv = (float2(pixel) + 0.5f) / float2(Width, Height);
    float2 currentJitterUv = jitterPixels / float2(RenderWidth, RenderHeight);
    float2 previousJitterUv = priorJitterPixels / float2(RenderWidth, RenderHeight);
    float2 currentJitteredUv = displayUv + currentJitterUv;
    SceneValue scene = EvaluateScene(currentJitteredUv, AnimationFrame);
    float3 spatial = SpatialBilinear(pixel, AnimationFrame, jitterPixels) * CurrentPreExposure;
    float3 nativeSpatial =
        SpatialBilinearAtExtent(pixel, AnimationFrame, jitterPixels, uint2(Width, Height)) * CurrentPreExposure;

    float2 motion = 0.0f;
    if (scene.objectId == 2u)
    {
        motion = MovingCenter(PreviousAnimationFrame) - MovingCenter(AnimationFrame);
    }
    float2 previousUv = currentJitteredUv + motion + previousJitterUv - currentJitterUv;
    float2 historyStorageUv = previousUv - previousJitterUv;

    float2 minimumHistoryUv = 0.5f / float2(Width, Height);
    bool footprintValid =
        all(historyStorageUv >= minimumHistoryUv) && all(historyStorageUv <= 1.0f - minimumHistoryUv);
    HistoryPixel history = (HistoryPixel)0;
    history.colorR = spatial.r;
    history.colorG = spatial.g;
    history.colorB = spatial.b;
    history.luminance = Luminance(spatial);
    history.depth = scene.depth;
    history.normalX = scene.normal.x;
    history.normalY = scene.normal.y;
    history.normalZ = scene.normal.z;
    history.objectId = scene.objectId;
    if (canSampleHistory)
    {
        history = GatherHistory(historyStorageUv, footprintValid);
    }

    uint reasons = 0u;
    if (!hasHistory) reasons |= RejectNoHistory;
    if (reset) reasons |= RejectReset;
    if (!footprintValid) reasons |= RejectFootprint;
    float depthTolerance = max(0.001f, 0.005f * max(scene.depth, history.depth));
    if (abs(history.depth - scene.depth) > depthTolerance) reasons |= RejectDepth;
    if (dot(scene.normal, float3(history.normalX, history.normalY, history.normalZ)) < 0.8f) reasons |= RejectNormal;
    if (scene.objectId != history.objectId) reasons |= RejectObject;
    if (scene.reactive >= 0.95f) reasons |= RejectReactive;
    if (scene.disocclusion >= 0.5f) reasons |= RejectDisocclusion;
    float exposureScale = CurrentPreExposure / PreviousPreExposure;
    float exposureRatio = max(exposureScale, 1.0f / exposureScale);
    if (exposureRatio > 4.0f) reasons |= RejectExposure;
    float historyLuminance = history.luminance * exposureScale;
    float luminanceRatio =
        (max(Luminance(spatial), historyLuminance) + 1.0e-4f) /
        (min(Luminance(spatial), historyLuminance) + 1.0e-4f);
    if (luminanceRatio > 8.0f) reasons |= RejectLuminance;

    float3 minimumColor, maximumColor, mean, variance, neighborAverage;
    Neighborhood(pixel, AnimationFrame, jitterPixels, minimumColor, maximumColor, mean, variance, neighborAverage);
    float3 historyColor = float3(history.colorR, history.colorG, history.colorB) * exposureScale;
    float clipScale = 1.0f;
    float3 constrained = spatial;
    if (reasons == 0u)
    {
        constrained = ConstrainHistoryColor(historyColor, minimumColor, maximumColor, mean, variance, clipScale);
    }

    float validityFactor = reasons == 0u ? 1.0f : 0.0f;
    float sampleCountFactor = min(1.0f, float(history.sampleCount) / 16.0f);
    float motionFactor = max(0.0f, 1.0f - length(motion) / 0.1f);
    float reactiveFactor = 1.0f - scene.reactive;
    float disocclusionFactor = 1.0f - scene.disocclusion;
    float unconstrainedFeedback = lerp(0.05f, 0.95f, sampleCountFactor);
    float feedback = validityFactor * unconstrainedFeedback * motionFactor * reactiveFactor * disocclusionFactor;
    float3 resolved = reasons == 0u ? lerp(spatial, constrained, feedback) : spatial;
    uint nextCount = reasons == 0u ? min(history.sampleCount + 1u, 64u) : 1u;
    float3 appliedDetail;
    float overshoot;
    float3 sharpened =
        BoundedSharpen(resolved, neighborAverage, minimumColor, maximumColor, SharpeningStrength, appliedDetail, overshoot);

    HistoryPixel next;
    next.colorR = resolved.r;
    next.colorG = resolved.g;
    next.colorB = resolved.b;
    next.luminance = Luminance(resolved);
    next.depth = scene.depth;
    next.normalX = scene.normal.x;
    next.normalY = scene.normal.y;
    next.normalZ = scene.normal.z;
    next.objectId = scene.objectId;
    next.sampleCount = nextCount;
    HistoryWrite[index] = next;

    PixelStatistics output = (PixelStatistics)0;
    output.currentR = nativeSpatial.r;
    output.currentG = nativeSpatial.g;
    output.currentB = nativeSpatial.b;
    output.currentLuminance = Luminance(spatial);
    output.spatialR = spatial.r;
    output.spatialG = spatial.g;
    output.spatialB = spatial.b;
    output.currentDepth = scene.depth;
    output.motionX = motion.x;
    output.motionY = motion.y;
    output.currentJitterX = jitterPixels.x;
    output.currentJitterY = jitterPixels.y;
    output.previousJitterX = priorJitterPixels.x;
    output.previousJitterY = priorJitterPixels.y;
    output.previousHistoryX = previousUv.x;
    output.previousHistoryY = previousUv.y;
    output.historyStorageX = historyStorageUv.x;
    output.historyStorageY = historyStorageUv.y;
    output.historyR = historyColor.r;
    output.historyG = historyColor.g;
    output.historyB = historyColor.b;
    output.historyLuminance = historyLuminance;
    output.constrainedR = constrained.r;
    output.constrainedG = constrained.g;
    output.constrainedB = constrained.b;
    output.varianceClipScale = clipScale;
    output.validityFactor = validityFactor;
    output.sampleCountFactor = sampleCountFactor;
    output.lockStatus = validityFactor * sampleCountFactor * reactiveFactor * disocclusionFactor;
    output.motionFactor = motionFactor;
    output.reactiveFactor = reactiveFactor;
    output.disocclusionFactor = disocclusionFactor;
    output.historyFeedback = feedback;
    output.resolvedR = resolved.r;
    output.resolvedG = resolved.g;
    output.resolvedB = resolved.b;
    output.sharpenedR = sharpened.r;
    output.sharpenedG = sharpened.g;
    output.sharpenedB = sharpened.b;
    output.sharpeningDeltaR = appliedDetail.r;
    output.sharpeningDeltaG = appliedDetail.g;
    output.sharpeningDeltaB = appliedDetail.b;
    output.overshootObserved = overshoot;
    output.rejectionReasons = reasons;
    output.previousSampleCount = history.sampleCount;
    output.nextSampleCount = nextCount;
    output.currentObjectId = scene.objectId;
    output.sampledObjectId = history.objectId;
    output.status = 511u;
    output.renderWidth = RenderWidth;
    output.renderHeight = RenderHeight;
    Statistics[index] = output;
}
