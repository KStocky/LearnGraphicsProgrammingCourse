// Chapter 29 Solution: screen-space reflections and one-bounce screen-space diffuse indirect lighting.
//
// Everything here reads the screen and nothing else. The analytic scene was consumed by the G-buffer pass; from
// this point on the only evidence available is device depth, the surface record, and the previous frame's
// accumulated contribution. That restriction is the whole point: the code below is written so that every place
// where the screen cannot answer produces a typed miss and a decomposed confidence factor instead of a plausible
// colour.
//
// Traversal is deterministic perspective-correct linear marching: UV and 1/viewDepth are both linear in the same
// segment parameter, so interpolating them together is exact, not an approximation. The step schedule is uniform in
// screen space and every step, refinement, geometry sample, and thickness rejection is published. A hierarchical
// depth pyramid would let the same ray skip empty screen regions; that acceleration is a separate contract and is
// deliberately not running here, so no diagnostic in this shader may be read as evidence of it.
#include "../Common/ScreenSpaceReflectionShared.hlsli"

struct ReflectionRay
{
    float3 origin;
    float3 direction;
    float3 viewDirection;
    float nDotV;
    float appliedNormalBias;
    float maximumDistance;
    float towardCameraCosine;
    bool facingValid;
};

// R = 2 * dot(N, V) * N - V, renormalised, with the origin displaced along the normal by
// constantNormalBias + depthProportionalNormalBias * viewDepth. The depth-proportional term keeps the displacement
// roughly constant in screen space as the surface recedes.
ReflectionRay BuildReflectionRay(float3 viewPosition, float3 normal, float3 direction, float maximumDistance)
{
    ReflectionRay ray;
    ray.viewDirection = normalize(-viewPosition);
    ray.nDotV = dot(normal, ray.viewDirection);
    ray.facingValid = ray.nDotV > MinimumFacingCosine;
    ray.appliedNormalBias = ConstantNormalBias + DepthProportionalNormalBias * viewPosition.z;
    ray.origin = viewPosition + normal * ray.appliedNormalBias;
    ray.direction = direction;
    ray.maximumDistance = maximumDistance;
    ray.towardCameraCosine = dot(ray.direction, ray.viewDirection);
    return ray;
}

struct ClippedSegment
{
    bool intersectsFrustum;
    float enterDistance;
    float exitDistance;
    float3 enterPosition;
    float3 exitPosition;
    uint enterLimit;
    uint exitLimit;
    uint rejectionLimit;
};

struct FrustumPlane
{
    float3 normal;
    float offset;
    uint limit;
};

// Clips [0, maximumDistance] against the six view-frustum half-spaces in the fixed order near, far, left, right,
// bottom, top. The screen domain and the frustum are the same region: a point inside the frustum has UV inside the
// unit square and a representable device depth.
ClippedSegment ClipRayToFrustum(ReflectionRay ray)
{
    FrustumPlane planes[6];
    planes[0].normal = float3(0.0f, 0.0f, 1.0f);
    planes[0].offset = -NearPlane;
    planes[0].limit = ClipNearPlane;
    planes[1].normal = float3(0.0f, 0.0f, -1.0f);
    planes[1].offset = FarPlane;
    planes[1].limit = ClipFarPlane;
    planes[2].normal = float3(1.0f, 0.0f, TanHalfHorizontalFov);
    planes[2].offset = 0.0f;
    planes[2].limit = ClipLeftPlane;
    planes[3].normal = float3(-1.0f, 0.0f, TanHalfHorizontalFov);
    planes[3].offset = 0.0f;
    planes[3].limit = ClipRightPlane;
    planes[4].normal = float3(0.0f, 1.0f, TanHalfVerticalFov);
    planes[4].offset = 0.0f;
    planes[4].limit = ClipBottomPlane;
    planes[5].normal = float3(0.0f, -1.0f, TanHalfVerticalFov);
    planes[5].offset = 0.0f;
    planes[5].limit = ClipTopPlane;

    ClippedSegment segment;
    segment.intersectsFrustum = true;
    segment.enterDistance = 0.0f;
    segment.exitDistance = ray.maximumDistance;
    segment.enterLimit = ClipRayStart;
    segment.exitLimit = ClipRayEnd;
    segment.rejectionLimit = ClipRayStart;

    [unroll] for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
    {
        if (!segment.intersectsFrustum)
        {
            continue;
        }
        float distanceAtOrigin = dot(planes[planeIndex].normal, ray.origin) + planes[planeIndex].offset;
        float rate = dot(planes[planeIndex].normal, ray.direction);
        if (rate == 0.0f)
        {
            if (distanceAtOrigin < 0.0f)
            {
                segment.intersectsFrustum = false;
                segment.rejectionLimit = planes[planeIndex].limit;
            }
            continue;
        }
        float crossing = -distanceAtOrigin / rate;
        if (rate > 0.0f)
        {
            if (crossing > segment.enterDistance)
            {
                segment.enterDistance = crossing;
                segment.enterLimit = planes[planeIndex].limit;
            }
        }
        else if (crossing < segment.exitDistance)
        {
            segment.exitDistance = crossing;
            segment.exitLimit = planes[planeIndex].limit;
        }
        if (segment.enterDistance > segment.exitDistance)
        {
            segment.intersectsFrustum = false;
            segment.rejectionLimit = planes[planeIndex].limit;
        }
    }

    segment.enterPosition = ray.origin + ray.direction * segment.enterDistance;
    segment.exitPosition = ray.origin + ray.direction * segment.exitDistance;
    return segment;
}

struct ScreenSegment
{
    float2 startUv;
    float2 endUv;
    float startReciprocalViewDepth;
    float endReciprocalViewDepth;
    float screenLengthTexels;
    bool valid;
};

// A straight view-space segment is not linear in screen space, but 1/viewDepth is. Storing both endpoints in UV and
// in reciprocal view depth and interpolating both in the same parameter is exact perspective-correct traversal.
ScreenSegment ProjectRaySegment(ClippedSegment clipped)
{
    ScreenSegment segment;
    float2 startUv;
    float2 endUv;
    bool startValid = ProjectViewPosition(clipped.enterPosition, startUv);
    bool endValid = ProjectViewPosition(clipped.exitPosition, endUv);
    // Clipping guarantees both endpoints lie in the closed unit square, so saturating only removes the last
    // rounding bit at the boundary.
    segment.startUv = saturate(startUv);
    segment.endUv = saturate(endUv);
    segment.startReciprocalViewDepth = 1.0f / max(clipped.enterPosition.z, NearPlane);
    segment.endReciprocalViewDepth = 1.0f / max(clipped.exitPosition.z, NearPlane);
    float2 texels = (segment.endUv - segment.startUv) * float2(Width, Height);
    segment.screenLengthTexels = length(texels);
    segment.valid = startValid && endValid && clipped.intersectsFrustum;
    return segment;
}

struct TraceResult
{
    bool hit;
    uint missReason;
    float2 hitUv;
    float hitRayViewDepth;
    float hitSceneViewDepth;
    float hitDepthDelta;
    float hitThicknessInterval;
    float hitRayDistance;
    float hitParameter;
    uint stepCount;
    uint refinementCount;
    uint geometrySampleCount;
    uint thicknessRejectionCount;
    uint requiredStepCount;
    bool stepBudgetExhausted;
    ScreenSegment segment;
    float segmentExitDistance;
};

struct TraversalSample
{
    float2 uv;
    float viewDepth;
    float3 viewPosition;
    float rayDistance;
};

TraversalSample SampleScreenRay(ScreenSegment segment, ReflectionRay ray, float parameter)
{
    TraversalSample result;
    result.uv = lerp(segment.startUv, segment.endUv, parameter);
    float reciprocal = lerp(segment.startReciprocalViewDepth, segment.endReciprocalViewDepth, parameter);
    result.viewDepth = 1.0f / reciprocal;
    result.viewPosition = ViewPositionFromUv(result.uv, result.viewDepth);
    result.rayDistance = dot(result.viewPosition - ray.origin, ray.direction);
    return result;
}

struct ScenePoint
{
    bool hasGeometry;
    float sceneViewDepth;
    float depthDelta;
    float thicknessInterval;
};

// Depth buffers store one surface, not a solid. A crossing is only a contact when the ray passes behind the sampled
// surface by no more than the thickness interval; everything deeper is information the screen does not have.
ScenePoint EvaluateScenePoint(TraversalSample traversalSample)
{
    ScenePoint scenePoint;
    scenePoint.hasGeometry = false;
    scenePoint.sceneViewDepth = 0.0f;
    scenePoint.depthDelta = 0.0f;
    scenePoint.thicknessInterval = 0.0f;
    float deviceDepth = SampleDeviceDepthNearest(traversalSample.uv);
    if (IsBackgroundDeviceDepth(deviceDepth))
    {
        return scenePoint;
    }
    scenePoint.hasGeometry = true;
    scenePoint.sceneViewDepth = ViewDepthFromDeviceDepth(deviceDepth);
    scenePoint.depthDelta = traversalSample.viewDepth - scenePoint.sceneViewDepth;
    scenePoint.thicknessInterval = ConstantThickness + DepthProportionalThickness * scenePoint.sceneViewDepth;
    return scenePoint;
}

TraceResult MakeTraceMiss(uint missReason)
{
    TraceResult result = (TraceResult)0;
    result.hit = false;
    result.missReason = missReason;
    return result;
}

// Mirrors TraceScreenSpaceRay in the CPU contracts, including the order in which the mutually exclusive miss
// reasons are decided.
TraceResult TraceScreenSpaceRay(ReflectionRay ray, uint maximumStepCount)
{
    if (!ray.facingValid)
    {
        return MakeTraceMiss(MissBackFacing);
    }
    ClippedSegment clipped = ClipRayToFrustum(ray);
    if (!clipped.intersectsFrustum)
    {
        return MakeTraceMiss(clipped.rejectionLimit == ClipNearPlane ? MissBehindCamera : MissOffScreen);
    }
    ScreenSegment segment = ProjectRaySegment(clipped);
    if (!segment.valid)
    {
        return MakeTraceMiss(MissBehindCamera);
    }

    float parameterPerStep = segment.screenLengthTexels > 0.0f ? StepLengthTexels / segment.screenLengthTexels : 1.0f;
    float required =
        segment.screenLengthTexels > 0.0f ? ceil(segment.screenLengthTexels / StepLengthTexels) : 1.0f;
    uint requiredStepCount = uint(clamp(required, 1.0f, 257.0f));
    uint stepCount = min(requiredStepCount, maximumStepCount);

    TraceResult result = (TraceResult)0;
    result.missReason = MissNone;
    result.requiredStepCount = requiredStepCount;
    result.stepBudgetExhausted = requiredStepCount > maximumStepCount;
    result.segment = segment;
    result.segmentExitDistance = clipped.exitDistance;

    float lowParameter = 0.0f;
    bool previousBehind = false;
    bool reachedSegmentEnd = false;
    [loop] for (uint stepIndex = 1u; stepIndex <= stepCount; ++stepIndex)
    {
        float parameter = min(1.0f, (float(stepIndex) + StartOffsetFraction) * parameterPerStep);
        TraversalSample traversalSample = SampleScreenRay(segment, ray, parameter);
        ScenePoint scenePoint = EvaluateScenePoint(traversalSample);
        ++result.stepCount;

        if (!scenePoint.hasGeometry)
        {
            lowParameter = parameter;
            previousBehind = false;
            if (parameter >= 1.0f)
            {
                reachedSegmentEnd = true;
                break;
            }
            continue;
        }
        ++result.geometrySampleCount;

        bool behind = scenePoint.depthDelta >= 0.0f;
        if (behind && !previousBehind)
        {
            float refinedLow = lowParameter;
            float refinedHigh = parameter;
            [loop] for (uint refinement = 0u; refinement < RefinementStepCount; ++refinement)
            {
                float middle = 0.5f * (refinedLow + refinedHigh);
                ScenePoint middlePoint = EvaluateScenePoint(SampleScreenRay(segment, ray, middle));
                ++result.refinementCount;
                if (middlePoint.hasGeometry && middlePoint.depthDelta >= 0.0f)
                {
                    refinedHigh = middle;
                }
                else
                {
                    refinedLow = middle;
                }
            }

            TraversalSample hitSample = SampleScreenRay(segment, ray, refinedHigh);
            ScenePoint hitPoint = EvaluateScenePoint(hitSample);
            if (hitPoint.hasGeometry && hitPoint.depthDelta >= 0.0f &&
                hitPoint.depthDelta <= hitPoint.thicknessInterval)
            {
                result.hit = true;
                result.missReason = MissNone;
                result.hitUv = hitSample.uv;
                result.hitRayViewDepth = hitSample.viewDepth;
                result.hitSceneViewDepth = hitPoint.sceneViewDepth;
                result.hitDepthDelta = hitPoint.depthDelta;
                result.hitThicknessInterval = hitPoint.thicknessInterval;
                result.hitRayDistance = hitSample.rayDistance;
                result.hitParameter = refinedHigh;
                return result;
            }
            ++result.thicknessRejectionCount;
        }

        lowParameter = parameter;
        previousBehind = behind;
        if (parameter >= 1.0f)
        {
            reachedSegmentEnd = true;
            break;
        }
    }

    if (result.thicknessRejectionCount > 0u)
    {
        result.missReason = MissThicknessExceeded;
        return result;
    }
    if (result.stepBudgetExhausted || !reachedSegmentEnd)
    {
        result.stepBudgetExhausted = true;
        result.missReason = MissMaximumSteps;
        return result;
    }
    if (result.geometrySampleCount == 0u)
    {
        result.missReason = MissNoCrossing;
        return result;
    }
    if (clipped.exitLimit == ClipRayEnd)
    {
        result.missReason = MissMaximumDistance;
    }
    else if (clipped.exitLimit == ClipNearPlane)
    {
        result.missReason = MissBehindCamera;
    }
    else
    {
        result.missReason = MissOffScreen;
    }
    return result;
}

struct ConfidenceFactors
{
    float validity;
    float screenEdge;
    float rayDistance;
    float thickness;
    float roughness;
    float towardCamera;
    float combined;
};

// Confidence stays decomposed so a learner can see which assumption failed. Each factor is independently bounded to
// [0, 1] and the combined value is their product: it is a fade weight, not a probability.
ConfidenceFactors EvaluateConfidence(TraceResult trace, float roughness, float towardCameraCosine)
{
    ConfidenceFactors factors;
    // Roughness and view-facing depend only on the shaded surface and the ray, so they are reported for a miss too.
    factors.roughness = FadeOut(roughness, RoughnessFadeStart, RoughnessFadeEnd);
    factors.towardCamera = FadeOut(towardCameraCosine, TowardCameraFadeStart, TowardCameraFadeEnd);
    factors.validity = 0.0f;
    factors.screenEdge = 0.0f;
    factors.rayDistance = 0.0f;
    factors.thickness = 0.0f;
    factors.combined = 0.0f;
    if (!trace.hit)
    {
        return factors;
    }
    float borderDistance =
        min(min(trace.hitUv.x, 1.0f - trace.hitUv.x), min(trace.hitUv.y, 1.0f - trace.hitUv.y));
    float travelledFraction = saturate(trace.hitRayDistance / MaximumRayDistance);
    factors.validity = 1.0f;
    factors.screenEdge = saturate(borderDistance / ScreenEdgeFadeUv);
    factors.rayDistance = FadeOut(travelledFraction, DistanceFadeStartFraction, 1.0f);
    factors.thickness = FadeOut(trace.hitDepthDelta, 0.0f, trace.hitThicknessInterval * ThicknessFadeFraction);
    factors.combined = factors.validity * factors.screenEdge * factors.rayDistance * factors.thickness *
                       factors.roughness * factors.towardCamera;
    return factors;
}

struct IndirectEstimate
{
    float3 meanIncoming;
    float3 irradiance;
    float3 outgoing;
    float averageCosineOverPdf;
    uint hitCount;
    uint missCount;
    uint clampedCount;
    uint sampleCount;
};

// One bounce only: a screen hit contributes the radiance that already left that surface this frame and nothing is
// traced from it. irradiance = (1/N) * sum L * cos / pdf, and outgoing = (albedo / pi) * irradiance. With the
// cosine density cos / pdf is exactly pi, so the pi in the Lambertian BRDF and the pi in the estimator are separate
// quantities that must never be cancelled by hand.
//
// The sample set is the same deterministic Hammersley sequence for every pixel. That makes the estimate exactly
// reproducible and easy to reason about, at the cost of correlating the error across the image: neighbouring pixels
// make the same mistake, so the bias appears as structure rather than noise. Decorrelating it per pixel and then
// filtering the result is Chapter 25 and Chapter 28 material, and is not attempted here.
IndirectEstimate EstimateDiffuseIndirect(float3 viewPosition, float3 normal, float3 albedo)
{
    IndirectEstimate estimate;
    estimate.meanIncoming = float3(0.0f, 0.0f, 0.0f);
    estimate.irradiance = float3(0.0f, 0.0f, 0.0f);
    estimate.outgoing = float3(0.0f, 0.0f, 0.0f);
    estimate.averageCosineOverPdf = 0.0f;
    estimate.hitCount = 0u;
    estimate.missCount = 0u;
    estimate.clampedCount = 0u;
    estimate.sampleCount = IndirectSampleCount;

    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(normal, tangent, bitangent);
    float3 accumulatedIncoming = float3(0.0f, 0.0f, 0.0f);
    float3 accumulated = float3(0.0f, 0.0f, 0.0f);
    float accumulatedWeight = 0.0f;
    [loop] for (uint sampleIndex = 0u; sampleIndex < IndirectSampleCount; ++sampleIndex)
    {
        float cosine;
        float3 local = MapCosineHemisphere(Hammersley(sampleIndex, IndirectSampleCount), cosine);
        float3 direction = normalize(tangent * local.x + bitangent * local.y + normal * local.z);
        ReflectionRay ray = BuildReflectionRay(viewPosition, normal, direction, IndirectMaximumRayDistance);
        TraceResult trace = TraceScreenSpaceRay(ray, IndirectMaximumStepCount);

        float3 incoming;
        if (trace.hit)
        {
            incoming = SurfaceRadiance(LoadSurface(TexelFromUv(trace.hitUv)));
            ++estimate.hitCount;
        }
        else
        {
            // Explicit fallback: a screen-space miss is not zero light, it is unmeasured light.
            incoming = EvaluateEnvironment(direction);
            ++estimate.missCount;
        }
        float3 clamped = min(incoming, IndirectMaximumRadiance);
        if (any(clamped != incoming))
        {
            ++estimate.clampedCount;
        }
        float pdf = cosine / Pi;
        float weight = cosine / pdf;
        accumulatedIncoming += clamped;
        accumulated += clamped * weight;
        accumulatedWeight += weight;
    }

    float sampleCount = float(max(1u, IndirectSampleCount));
    estimate.meanIncoming = accumulatedIncoming / sampleCount;
    estimate.irradiance = accumulated / sampleCount;
    estimate.averageCosineOverPdf = accumulatedWeight / sampleCount;
    estimate.outgoing = (albedo / Pi) * estimate.irradiance;
    return estimate;
}

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
    record.indirectSampleCount = IndirectSampleCount;
    record.status = StatusGBuffer;

    float3 direct = SurfaceRadiance(surface);
    record.directR = direct.r;
    record.directG = direct.g;
    record.directB = direct.b;

    HistoryPixel emptyHistory = (HistoryPixel)0;
    HistoryWrite[index] = emptyHistory;

    bool background = surface.materialId == MaterialBackground || IsBackgroundDeviceDepth(deviceDepth);
    if (background)
    {
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
        record.currentWeight = 1.0f;
        record.historyRejectionReasons = HistoryRejectNoHistory;
        record.previousUvX = uv.x;
        record.previousUvY = uv.y;
        record.previousViewDepth = 0.0f;
        // A background pixel has nothing to reconstruct, trace, or accumulate, so every stage is complete for it.
        record.status = StatusGBuffer | StatusReconstruction | StatusBaseline | StatusRay | StatusTraversal |
                        StatusConfidence | StatusComposition | StatusIndirect | StatusTemporal;
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
    ReflectionRay ray = BuildReflectionRay(viewPosition, normal, mirrorDirection, MaximumRayDistance);
    record.rayDirectionX = ray.direction.x;
    record.rayDirectionY = ray.direction.y;
    record.rayDirectionZ = ray.direction.z;
    record.appliedNormalBias = ray.appliedNormalBias;
    record.towardCameraCosine = ray.towardCameraCosine;
    record.status |= StatusRay;

    TraceResult trace = MakeTraceMiss(MissInvalidInput);
    if ((Flags & FlagScreenTracing) != 0u)
    {
        trace = TraceScreenSpaceRay(ray, MaximumStepCount);
    }
    record.missReason = trace.missReason;
    record.hitUvX = trace.hitUv.x;
    record.hitUvY = trace.hitUv.y;
    record.hitRayViewDepth = trace.hitRayViewDepth;
    record.hitSceneViewDepth = trace.hitSceneViewDepth;
    record.hitDepthDelta = trace.hitDepthDelta;
    record.hitThicknessInterval = trace.hitThicknessInterval;
    record.hitRayDistance = trace.hitRayDistance;
    record.hitParameter = trace.hitParameter;
    record.stepCount = trace.stepCount;
    record.refinementCount = trace.refinementCount;
    record.geometrySampleCount = trace.geometrySampleCount;
    record.thicknessRejectionCount = trace.thicknessRejectionCount;
    record.requiredStepCount = trace.requiredStepCount;
    record.segmentStartUvX = trace.segment.startUv.x;
    record.segmentStartUvY = trace.segment.startUv.y;
    record.segmentEndUvX = trace.segment.endUv.x;
    record.segmentEndUvY = trace.segment.endUv.y;
    record.segmentScreenLengthTexels = trace.segment.screenLengthTexels;
    record.segmentExitDistance = trace.segmentExitDistance;
    record.flags |= trace.hit ? RecordHit : 0u;
    record.flags |= trace.stepBudgetExhausted ? RecordStepBudgetExhausted : 0u;
    record.flags |= trace.segment.valid ? RecordSegmentValid : 0u;
    record.status |= StatusTraversal;

    ConfidenceFactors confidence = EvaluateConfidence(trace, surface.roughness, ray.towardCameraCosine);
    record.confidenceValidity = confidence.validity;
    record.confidenceScreenEdge = confidence.screenEdge;
    record.confidenceRayDistance = confidence.rayDistance;
    record.confidenceThickness = confidence.thickness;
    record.confidenceRoughness = confidence.roughness;
    record.confidenceTowardCamera = confidence.towardCamera;
    record.confidenceCombined = confidence.combined;
    record.status |= StatusConfidence;

    // The screen answers with the radiance that already left the surface it found; the environment answers for
    // everything the screen could not see. The weights are confidence and 1 - confidence, so they sum to exactly
    // one and the incoming radiance is a partition of a single quantity.
    float3 screenRadiance = trace.hit ? SurfaceRadiance(LoadSurface(TexelFromUv(trace.hitUv)))
                                      : float3(0.0f, 0.0f, 0.0f);
    float3 environmentRadiance = EvaluateEnvironment(ray.direction);
    float screenWeight = confidence.combined;
    float environmentWeight = 1.0f - confidence.combined;
    float3 incomingRadiance = screenRadiance * screenWeight + environmentRadiance * environmentWeight;

    // The split-sum weight F0 * A + B is applied once, after the two sources have already been combined, so a
    // partial screen answer can never be shaded twice.
    float2 splitSum = IntegrateSplitSum(nDotV, surface.roughness, SplitSumSampleCount);
    float3 normalIncidenceReflectance = float3(0.04f, 0.04f, 0.04f);
    float3 specularWeight = normalIncidenceReflectance * splitSum.x + splitSum.y;
    float3 reflection = incomingRadiance * specularWeight;
    float3 baselineReflection = environmentRadiance * specularWeight;
    float3 baseline = direct + baselineReflection;

    record.screenWeight = screenWeight;
    record.environmentWeight = environmentWeight;
    record.screenRadianceR = screenRadiance.r;
    record.screenRadianceG = screenRadiance.g;
    record.screenRadianceB = screenRadiance.b;
    record.environmentRadianceR = environmentRadiance.r;
    record.environmentRadianceG = environmentRadiance.g;
    record.environmentRadianceB = environmentRadiance.b;
    record.incomingRadianceR = incomingRadiance.r;
    record.incomingRadianceG = incomingRadiance.g;
    record.incomingRadianceB = incomingRadiance.b;
    record.splitSumA = splitSum.x;
    record.splitSumB = splitSum.y;
    record.specularWeightR = specularWeight.r;
    record.specularWeightG = specularWeight.g;
    record.specularWeightB = specularWeight.b;
    record.reflectionR = reflection.r;
    record.reflectionG = reflection.g;
    record.reflectionB = reflection.b;
    record.baselineReflectionR = baselineReflection.r;
    record.baselineReflectionG = baselineReflection.g;
    record.baselineReflectionB = baselineReflection.b;
    record.baselineR = baseline.r;
    record.baselineG = baseline.g;
    record.baselineB = baseline.b;
    if (Finite3(baseline) && Finite3(reflection) && Finite3(incomingRadiance))
    {
        record.status |= StatusComposition | StatusBaseline;
    }

    IndirectEstimate indirect = (IndirectEstimate)0;
    if ((Flags & FlagIndirect) != 0u)
    {
        indirect = EstimateDiffuseIndirect(viewPosition, normal, float3(surface.albedoR, surface.albedoG,
                                                                       surface.albedoB));
    }
    record.indirectMeanIncomingR = indirect.meanIncoming.r;
    record.indirectMeanIncomingG = indirect.meanIncoming.g;
    record.indirectMeanIncomingB = indirect.meanIncoming.b;
    record.indirectIrradianceR = indirect.irradiance.r;
    record.indirectIrradianceG = indirect.irradiance.g;
    record.indirectIrradianceB = indirect.irradiance.b;
    record.indirectOutgoingR = indirect.outgoing.r;
    record.indirectOutgoingG = indirect.outgoing.g;
    record.indirectOutgoingB = indirect.outgoing.b;
    record.indirectAverageCosineOverPdf = indirect.averageCosineOverPdf;
    record.indirectHitCount = indirect.hitCount;
    record.indirectMissCount = indirect.missCount;
    record.indirectClampedCount = indirect.clampedCount;
    record.status |= StatusIndirect;

    float3 screenSpaceContribution = reflection + indirect.outgoing;
    float3 finalRadiance = direct + screenSpaceContribution;
    record.finalR = finalRadiance.r;
    record.finalG = finalRadiance.g;
    record.finalB = finalRadiance.b;

    // The accumulated signal is the sum of two screen-space estimates, so the confidence that drives accumulation
    // has to describe that sum rather than either half of it. The specular half is trusted by the traced
    // confidence; the diffuse half is trusted in proportion to how many of its rays the screen actually answered,
    // because a missed indirect ray falls back to the environment and is a guess. Blending the two by the radiance
    // each contributes keeps the result a convex combination: it always lies between the two confidences, and a
    // term that adds no light cannot change how much the sum is trusted. When neither term carries any radiance the
    // two halves share a zero sum equally.
    bool indirectEnabled = (Flags & FlagIndirect) != 0u;
    float indirectConfidence =
        indirectEnabled ? float(indirect.hitCount) / float(max(1u, IndirectSampleCount)) : 0.0f;
    float specularShare = Luminance(reflection);
    float indirectShare = Luminance(indirect.outgoing);
    if (specularShare + indirectShare <= 0.0f)
    {
        specularShare = 1.0f;
        indirectShare = 1.0f;
    }
    float currentConfidence =
        indirectEnabled
            ? ((specularShare * confidence.combined) + (indirectShare * indirectConfidence)) /
                  (specularShare + indirectShare)
            : confidence.combined;
    record.indirectConfidence = indirectConfidence;
    record.currentConfidence = currentConfidence;

    // Chapter 28 conventions, reused rather than re-derived: motion is previousUV - currentUV, history is
    // scene-linear, and the two display-resolution history buffers are owned by the frame sequence. Only the
    // screen-space contribution is accumulated, so the direct term can never be double counted.
    //
    // The motion vector describes the surface, not what that surface reflects. The floor is static and reports zero
    // motion, so the moving emitter's reflection is reprojected as if it were painted on the floor and smears. That
    // is a real limitation of surface motion vectors, not an accumulation bug; production renderers reproject
    // reflections through the virtual mirrored hit position instead, which this lab does not attempt.
    float2 previousUv = uv + float2(surface.motionX, surface.motionY);
    bool inBounds = all(previousUv >= 0.0f) && all(previousUv <= 1.0f);
    bool temporalEnabled = (Flags & FlagTemporal) != 0u;
    bool historyResourceValid = (Flags & FlagHistoryValid) != 0u && (Flags & FlagReset) == 0u;
    bool sampled = temporalEnabled && inBounds && historyResourceValid;
    HistoryPixel history = (HistoryPixel)0;
    if (sampled)
    {
        uint2 historyTexel = TexelFromUv(previousUv);
        history = HistoryRead[historyTexel.y * Width + historyTexel.x];
    }

    // Two independent pieces of evidence decide whether the sampled texel is the same surface, and both are
    // published. The depth comparison uses the depth this surface had in the previous frame, so a surface that
    // moved along the view axis is still recognised as itself; it is made in linear view units, so the device-depth
    // convention that stored it cannot change the answer.
    float depthDifference = sampled ? abs(surface.previousViewDepth - history.viewDepth) : 0.0f;
    float depthTolerance =
        sampled ? AbsoluteDepthTolerance +
                      RelativeDepthTolerance * max(surface.previousViewDepth, history.viewDepth)
                : 0.0f;
    uint historyRejection = 0u;
    historyRejection |= temporalEnabled ? 0u : HistoryRejectDisabled;
    historyRejection |= historyResourceValid ? 0u : HistoryRejectNoHistory;
    historyRejection |= inBounds ? 0u : HistoryRejectOffScreen;
    if (sampled)
    {
        historyRejection |= history.sampleCount > 0u ? 0u : HistoryRejectNoSamples;
        historyRejection |= (MaterialIdentityRequired != 0u && history.materialId != surface.materialId)
                                ? HistoryRejectMaterial
                                : 0u;
        historyRejection |= depthDifference > depthTolerance ? HistoryRejectDepth : 0u;
    }
    bool historyUsable = historyRejection == 0u;

    uint boundedPreviousCount = min(history.sampleCount, MaximumTemporalSampleCount);
    float historyScore = historyUsable ? history.confidence * float(boundedPreviousCount) : 0.0f;
    float currentScore = currentConfidence;
    float total = historyScore + currentScore;
    // Confidence and age decide how the two samples share the result; the ceiling decides how much of the result a
    // history sample is ever allowed to own. Without it a pixel whose current confidence is zero would take the
    // history at weight one, and both the colour and the confidence would be fixed points: stale lighting could
    // survive for as long as the surface stayed on screen. The current frame therefore always keeps at least
    // 1 - MaximumHistoryWeight, so an unconfirmed history decays geometrically instead of freezing.
    float historyWeight = min(total > 0.0f ? historyScore / total : 0.0f, MaximumHistoryWeight);
    float currentWeight = 1.0f - historyWeight;
    float3 historyColor = historyUsable ? float3(history.colorR, history.colorG, history.colorB) : 0.0f;
    float historyConfidence = historyUsable ? history.confidence : 0.0f;
    float3 temporalContribution = screenSpaceContribution * currentWeight + historyColor * historyWeight;
    float outputConfidence = currentWeight * currentConfidence + historyWeight * historyConfidence;
    uint nextSampleCount = historyUsable ? min(boundedPreviousCount + 1u, MaximumTemporalSampleCount) : 1u;

    HistoryPixel next;
    next.colorR = temporalContribution.r;
    next.colorG = temporalContribution.g;
    next.colorB = temporalContribution.b;
    next.confidence = outputConfidence;
    next.viewDepth = viewDepth;
    next.materialId = surface.materialId;
    next.sampleCount = nextSampleCount;
    next.padding = 0u;
    HistoryWrite[index] = next;

    float3 temporalRadiance = direct + temporalContribution;
    record.previousUvX = previousUv.x;
    record.previousUvY = previousUv.y;
    record.previousViewDepth = surface.previousViewDepth;
    record.currentWeight = currentWeight;
    record.historyWeight = historyWeight;
    record.historyR = historyColor.r;
    record.historyG = historyColor.g;
    record.historyB = historyColor.b;
    record.historyConfidence = historyConfidence;
    record.historyViewDepth = history.viewDepth;
    record.historyDepthDifference = depthDifference;
    record.historyDepthTolerance = depthTolerance;
    record.outputConfidence = outputConfidence;
    record.temporalR = temporalRadiance.r;
    record.temporalG = temporalRadiance.g;
    record.temporalB = temporalRadiance.b;
    record.previousSampleCount = history.sampleCount;
    record.nextSampleCount = nextSampleCount;
    record.historyMaterialId = history.materialId;
    record.historyRejectionReasons = historyRejection;
    record.flags |= historyUsable ? RecordHistoryUsable : 0u;
    if (Finite3(finalRadiance) && Finite3(temporalRadiance) && Finite3(indirect.outgoing))
    {
        record.status |= StatusTemporal;
    }
    Diagnostics[index] = record;
}
