// Shared Chapter 28 GPU contract. Colors remain scene-linear throughout.
static const uint RejectNoHistory = 1u;
static const uint RejectReset = 2u;
static const uint RejectFootprint = 4u;
static const uint RejectDepth = 8u;
static const uint RejectNormal = 16u;
static const uint RejectObject = 32u;
static const uint RejectReactive = 64u;
static const uint RejectDisocclusion = 128u;
static const uint RejectExposure = 256u;
static const uint RejectLuminance = 512u;

struct PixelStatistics
{
    float currentR, currentG, currentB, currentLuminance;
    float spatialR, spatialG, spatialB, currentDepth;
    float motionX, motionY, currentJitterX, currentJitterY;
    float previousJitterX, previousJitterY, previousHistoryX, previousHistoryY;
    float historyStorageX, historyStorageY;
    float historyR, historyG, historyB, historyLuminance;
    float constrainedR, constrainedG, constrainedB, varianceClipScale;
    float validityFactor, sampleCountFactor, lockStatus, motionFactor;
    float reactiveFactor, disocclusionFactor, historyFeedback, resolvedR;
    float resolvedG, resolvedB, sharpenedR, sharpenedG;
    float sharpenedB, sharpeningDeltaR, sharpeningDeltaG, sharpeningDeltaB;
    float overshootObserved;
    uint rejectionReasons, previousSampleCount, nextSampleCount, currentObjectId;
    uint sampledObjectId, status, renderWidth, renderHeight;
};

struct HistoryPixel
{
    float colorR, colorG, colorB, luminance;
    float depth, normalX, normalY, normalZ;
    uint objectId, sampleCount;
};

cbuffer DispatchConstants : register(b0)
{
    uint Width;
    uint Height;
    uint RenderWidth;
    uint RenderHeight;
    uint AnimationFrame;
    uint PreviousAnimationFrame;
    uint JitterPhasePeriod;
    uint Flags;
    float CurrentPreExposure;
    float PreviousPreExposure;
    float SharpeningStrength;
    float ConstantsPadding;
};

RWStructuredBuffer<PixelStatistics> Statistics : register(u0);
RWStructuredBuffer<HistoryPixel> HistoryWrite : register(u1);
StructuredBuffer<HistoryPixel> HistoryRead : register(t0);

float RadicalInverse(uint index, uint base)
{
    float result = 0.0f;
    float inverseBase = 1.0f / float(base);
    float fraction = inverseBase;
    uint value = index + 1u;
    while (value > 0u)
    {
        result += float(value % base) * fraction;
        value /= base;
        fraction *= inverseBase;
    }
    return result;
}

float2 CentroidCorrectedHalton(uint frame, uint period)
{
    float2 phaseMean = 0.0f;
    for (uint phase = 0u; phase < period; ++phase)
    {
        phaseMean += float2(RadicalInverse(phase, 2u), RadicalInverse(phase, 3u)) - 0.5f;
    }
    phaseMean /= float(period);
    uint phaseIndex = frame % period;
    return float2(RadicalInverse(phaseIndex, 2u), RadicalInverse(phaseIndex, 3u)) - 0.5f - phaseMean;
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 RgbToYCoCg(float3 color)
{
    return float3(0.25f * color.r + 0.5f * color.g + 0.25f * color.b,
                  0.5f * color.r - 0.5f * color.b,
                  -0.25f * color.r + 0.5f * color.g - 0.25f * color.b);
}

float3 YCoCgToRgb(float3 color)
{
    return float3(color.x + color.y - color.z, color.x + color.z, color.x - color.y - color.z);
}

struct SceneValue
{
    float3 color;
    float depth;
    float3 normal;
    uint objectId;
    float reactive;
    float disocclusion;
};

float2 MovingCenter(uint frame)
{
    float t = float(frame) * 0.17f;
    return float2(0.48f + 0.16f * sin(t), 0.57f + 0.035f * cos(t * 0.7f));
}

SceneValue EvaluateScene(float2 uv, uint frame)
{
    SceneValue value;
    value.color = float3(0.08f, 0.11f, 0.16f);
    value.depth = 20.0f;
    value.normal = float3(0.0f, 0.0f, 1.0f);
    value.objectId = 1u;
    value.reactive = 0.0f;
    value.disocclusion = 0.0f;

    if (uv.x < 0.20f && uv.y < 0.22f)
    {
        value.color = float3(0.25f, 0.5f, 1.0f);
        return value;
    }

    float checker = 2.0f * frac(0.5f * (floor(uv.x * 79.0f) + floor(uv.y * 53.0f)));
    float lineMask = (abs(frac(uv.x * 31.0f) - 0.5f) < 0.055f ||
                      abs(frac(uv.y * 23.0f) - 0.5f) < 0.055f) ? 1.0f : 0.0f;
    value.color = lerp(float3(0.025f, 0.035f, 0.05f), float3(0.72f, 0.62f, 0.18f), checker);
    value.color += lineMask * float3(1.6f, 1.2f, 0.35f);

    float2 center = MovingCenter(frame);
    float2 local = uv - center;
    if (abs(local.x) < 0.105f && abs(local.y) < 0.13f)
    {
        value.objectId = 2u;
        value.depth = 6.0f;
        value.normal = normalize(float3(local.x * 2.5f, local.y * 2.0f, 1.0f));
        if (local.x > 0.045f && local.y < -0.035f)
        {
            value.normal = (frame & 1u) != 0u ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
        }
        float thin = abs(frac((local.x + 0.105f) * 95.0f) - 0.5f) < 0.12f ? 1.0f : 0.0f;
        value.color = lerp(float3(0.08f, 0.18f, 0.75f), float3(4.0f, 0.15f, 0.05f), thin);
        value.reactive = abs(local.y) < 0.018f ? 1.0f : 0.15f;
    }

    float2 previousLocal = uv - MovingCenter(frame > 0u ? frame - 1u : 0u);
    bool wasObject = abs(previousLocal.x) < 0.105f && abs(previousLocal.y) < 0.13f;
    bool isObject = value.objectId == 2u;
    value.disocclusion = wasObject != isObject ? 1.0f : 0.0f;
    return value;
}

float3 SpatialBilinearAtExtent(uint2 displayPixel, uint frame, float2 jitterPixels, uint2 sourceExtent)
{
    float2 displayUv = (float2(displayPixel) + 0.5f) / float2(Width, Height);
    float2 center = displayUv * float2(sourceExtent) - 0.5f;
    int2 base = int2(floor(center));
    float2 f = frac(center);
    float3 sum = 0.0f;
    [unroll] for (uint oy = 0u; oy < 2u; ++oy)
    {
        [unroll] for (uint ox = 0u; ox < 2u; ++ox)
        {
            int2 texel = clamp(base + int2(ox, oy), int2(0, 0), int2(sourceExtent) - 1);
            float2 uv = (float2(texel) + 0.5f + jitterPixels) / float2(sourceExtent);
            float weight = (ox == 0u ? 1.0f - f.x : f.x) * (oy == 0u ? 1.0f - f.y : f.y);
            sum += EvaluateScene(uv, frame).color * weight;
        }
    }
    return sum;
}

float3 SpatialBilinear(uint2 displayPixel, uint frame, float2 jitterPixels)
{
    return SpatialBilinearAtExtent(displayPixel, frame, jitterPixels, uint2(RenderWidth, RenderHeight));
}

void Neighborhood(uint2 pixel, uint frame, float2 jitterPixels, out float3 minimumColor, out float3 maximumColor,
                  out float3 mean, out float3 variance, out float3 neighborAverage)
{
    minimumColor = float3(1.0e6f, 1.0e6f, 1.0e6f);
    maximumColor = 0.0f;
    mean = 0.0f;
    variance = 0.0f;
    neighborAverage = 0.0f;
    uint count = 0u;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            uint2 p = uint2(clamp(int2(pixel) + int2(x, y), int2(0, 0), int2(Width - 1u, Height - 1u)));
            float3 rgb = SpatialBilinear(p, frame, jitterPixels) * CurrentPreExposure;
            float3 converted = RgbToYCoCg(rgb);
            minimumColor = min(minimumColor, rgb);
            maximumColor = max(maximumColor, rgb);
            ++count;
            float3 delta = converted - mean;
            mean += delta / float(count);
            variance += delta * (converted - mean);
            neighborAverage += rgb;
        }
    }
    variance = max(0.0f, variance / float(count));
    neighborAverage /= float(count);
}

float ClipAxis(float center, float value, float radius)
{
    float distance = abs(value - center);
    return distance == 0.0f ? 1.0f : min(1.0f, radius / distance);
}

float3 ConstrainHistoryColor(float3 history, float3 rgbMinimum, float3 rgbMaximum, float3 mean, float3 variance,
                             out float clipScale)
{
    float3 yCoCg = RgbToYCoCg(history);
    float3 radius = float3(1.5f, 1.0f, 1.0f) * sqrt(variance);
    clipScale = min(ClipAxis(mean.x, yCoCg.x, radius.x),
                    min(ClipAxis(mean.y, yCoCg.y, radius.y), ClipAxis(mean.z, yCoCg.z, radius.z)));
    float3 varianceClipped = clipScale < 1.0f ? YCoCgToRgb(mean + (yCoCg - mean) * clipScale) : history;
    return clamp(varianceClipped, rgbMinimum, rgbMaximum);
}

float3 BoundedSharpen(float3 center, float3 average, float3 minimumColor, float3 maximumColor, float strength,
                      out float3 appliedDetail, out float overshoot)
{
    float3 requested = strength * (center - average);
    float3 range = maximumColor - minimumColor;
    float3 lower = max(0.0f, minimumColor);
    float3 upper = maximumColor + 0.05f * range;
    float3 output = clamp(center + requested, lower, upper);
    appliedDetail = output - center;
    float3 overshootRatio = max(0.0f, output - maximumColor) / max(range, 1.0e-6f);
    overshoot = max(overshootRatio.r, max(overshootRatio.g, overshootRatio.b));
    return output;
}

struct FullscreenVertex
{
    float4 position : SV_Position;
};

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenVertex output;
    output.position = float4(vertexId == 2u ? 3.0f : -1.0f, vertexId == 1u ? 3.0f : -1.0f, 0.0f, 1.0f);
    return output;
}

cbuffer DisplayConstants : register(b0)
{
    uint DisplayWidth;
    uint DisplayHeight;
    uint SelectedDebugView;
    uint ExpectedStatus;
};

StructuredBuffer<PixelStatistics> DisplayStatistics : register(t0);

float3 ReasonColor(uint reasons)
{
    if (reasons == 0u) return float3(0.05f, 0.75f, 0.15f);
    float3 result = 0.0f;
    if ((reasons & (RejectNoHistory | RejectReset)) != 0u) result += float3(0.15f, 0.25f, 1.0f);
    if ((reasons & RejectFootprint) != 0u) result += float3(1.0f, 0.0f, 1.0f);
    if ((reasons & (RejectDepth | RejectNormal)) != 0u) result += float3(1.0f, 0.55f, 0.0f);
    if ((reasons & RejectObject) != 0u) result += float3(1.0f, 0.0f, 0.0f);
    if ((reasons & (RejectReactive | RejectDisocclusion)) != 0u) result += float3(0.0f, 0.8f, 1.0f);
    if ((reasons & (RejectExposure | RejectLuminance)) != 0u) result += float3(0.75f, 0.0f, 0.75f);
    return saturate(result);
}

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
    uint2 pixel = min(uint2(position.xy), uint2(DisplayWidth - 1u, DisplayHeight - 1u));
    PixelStatistics p = DisplayStatistics[pixel.y * DisplayWidth + pixel.x];
    if ((p.status & ExpectedStatus) != ExpectedStatus) return float4(1.0f, 0.0f, 1.0f, 1.0f);
    float3 spatial = float3(p.spatialR, p.spatialG, p.spatialB);
    float3 history = float3(p.historyR, p.historyG, p.historyB);
    float3 constrained = float3(p.constrainedR, p.constrainedG, p.constrainedB);
    float3 resolved = float3(p.resolvedR, p.resolvedG, p.resolvedB);
    float3 sharpened = float3(p.sharpenedR, p.sharpenedG, p.sharpenedB);
    float3 result = spatial;
    if (SelectedDebugView == 1u) result = float3(0.5f + p.motionX * 8.0f, 0.5f + p.motionY * 8.0f, 0.5f);
    else if (SelectedDebugView == 2u) result = ReasonColor(p.rejectionReasons);
    else if (SelectedDebugView == 3u)
    {
        bool left = pixel.x * 2u < DisplayWidth;
        result = left ? history : constrained;
    }
    else if (SelectedDebugView == 4u) result = float3(p.historyFeedback, float(p.nextSampleCount) / 64.0f, p.lockStatus);
    else if (SelectedDebugView == 5u) result = resolved;
    else if (SelectedDebugView == 6u)
    {
        bool left = pixel.x * 2u < DisplayWidth;
        result = left ? float3(p.currentR, p.currentG, p.currentB) : resolved;
    }
    else if (SelectedDebugView == 7u)
    {
        bool left = pixel.x * 2u < DisplayWidth;
        result = left ? resolved : sharpened;
    }
    return float4(result / (1.0f + result), 1.0f);
}
