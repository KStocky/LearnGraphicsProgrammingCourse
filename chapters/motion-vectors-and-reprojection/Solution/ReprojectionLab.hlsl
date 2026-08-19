struct SurfaceConstants
{
    float4 currentRectUv;
    float4 currentColor;
    uint4 metadata;
    float4 depths;
    float4 currentClipRows[4];
    float4 previousClipRows[4];
};

cbuffer FrameConstants : register(b0)
{
    uint4 gHeader0;
    float4 gCurrentJitterExposure;
    float4 gPreviousJitterExposure;
    float4 gDepthSettings;
    SurfaceConstants gSurfaces[4];
};

Texture2D<float4> gCurrentColor : register(t0);
Texture2D<float> gCurrentDepth : register(t1);
Texture2D<uint> gCurrentIdentity : register(t2);
Texture2D<float4> gMotionClipDepth : register(t3);
Texture2D<float2> gPreviousHistoryUv : register(t4);
Texture2D<float4> gReprojectedHistoryColor : register(t5);
Texture2D<uint> gRejectionReasons : register(t6);
Texture2D<float> gExposureScale : register(t7);
Texture2D<float4> gHistoryColor : register(t8);
Texture2D<float> gHistoryDepth : register(t9);
Texture2D<uint> gHistoryIdentity : register(t10);

RWTexture2D<float4> gCurrentColorUav : register(u0);
RWTexture2D<float> gCurrentDepthUav : register(u1);
RWTexture2D<uint> gCurrentIdentityUav : register(u2);
RWTexture2D<float4> gMotionClipDepthUav : register(u3);
RWTexture2D<float2> gPreviousHistoryUvUav : register(u4);
RWTexture2D<float4> gReprojectedHistoryColorUav : register(u5);
RWTexture2D<uint> gRejectionReasonsUav : register(u6);
RWTexture2D<float> gExposureScaleUav : register(u7);

SamplerState gLinearClamp : register(s0);
SamplerState gPointClamp : register(s1);

static const uint kRejectNoHistory = 1u << 0u;
static const uint kRejectReset = 1u << 1u;
static const uint kRejectNonFinite = 1u << 2u;
static const uint kRejectPreviousW = 1u << 3u;
static const uint kRejectUvOutOfBounds = 1u << 4u;
static const uint kRejectDepthMismatch = 1u << 5u;
static const uint kRejectIdentityMismatch = 1u << 6u;
static const uint kRejectExposure = 1u << 7u;

float2 RasterUv(uint2 pixel)
{
    return (float2(pixel) + 0.5f) / float2(gHeader0.xy);
}

bool TryResolveLocal(float4 rectUv, float2 rasterUv, float2 jitterUv, out float2 localUv)
{
    float2 minUv = rectUv.xy + jitterUv;
    float2 maxUv = rectUv.zw + jitterUv;
    float2 extent = maxUv - minUv;
    if (extent.x <= 0.0f || extent.y <= 0.0f)
    {
        localUv = 0.0f.xx;
        return false;
    }

    localUv = (rasterUv - minUv) / extent;
    return all(localUv >= 0.0f.xx) && all(localUv <= 1.0f.xx);
}

float4 TransformRows(float4 rows[4], float2 localUv)
{
    float4 localPoint = float4(localUv, 0.0f, 1.0f);
    return float4(dot(rows[0], localPoint), dot(rows[1], localPoint), dot(rows[2], localPoint), dot(rows[3], localPoint));
}

bool IsFinite4(float4 value)
{
    return all(isfinite(value));
}

bool IsFinite2(float2 value)
{
    return all(isfinite(value));
}

float2 ProjectUnjitteredUv(float4 clipPosition)
{
    float inverseW = 1.0f / clipPosition.w;
    float ndcX = clipPosition.x * inverseW;
    float ndcY = clipPosition.y * inverseW;
    return float2((ndcX * 0.5f) + 0.5f, (-ndcY * 0.5f) + 0.5f);
}

float DepthGradient(float2 uv)
{
    float2 texel = 1.0f / float2(gHeader0.xy);
    float center = gHistoryDepth.SampleLevel(gPointClamp, uv, 0.0f);
    float left = gHistoryDepth.SampleLevel(gPointClamp, uv + float2(-texel.x, 0.0f), 0.0f);
    float right = gHistoryDepth.SampleLevel(gPointClamp, uv + float2(texel.x, 0.0f), 0.0f);
    float up = gHistoryDepth.SampleLevel(gPointClamp, uv + float2(0.0f, -texel.y), 0.0f);
    float down = gHistoryDepth.SampleLevel(gPointClamp, uv + float2(0.0f, texel.y), 0.0f);
    return max(max(abs(center - left), abs(right - center)), max(abs(center - up), abs(down - center)));
}

uint PointSampleIdentity(float2 uv)
{
    float2 clampedUv = clamp(uv, 0.0f.xx, asfloat(0x3F7FFFFFu).xx);
    uint2 pixel = uint2(clampedUv * float2(gHeader0.xy));
    pixel = min(pixel, gHeader0.xy - uint2(1u, 1u));
    return gHistoryIdentity.Load(int3(pixel, 0));
}

uint PickRejectColor(uint reasons)
{
    if ((reasons & (kRejectNoHistory | kRejectReset)) != 0u)
    {
        return 0u;
    }
    if ((reasons & kRejectNonFinite) != 0u)
    {
        return 1u;
    }
    if ((reasons & kRejectPreviousW) != 0u)
    {
        return 2u;
    }
    if ((reasons & kRejectUvOutOfBounds) != 0u)
    {
        return 3u;
    }
    if ((reasons & kRejectDepthMismatch) != 0u)
    {
        return 4u;
    }
    if ((reasons & kRejectIdentityMismatch) != 0u)
    {
        return 5u;
    }
    if ((reasons & kRejectExposure) != 0u)
    {
        return 6u;
    }
    return 7u;
}

float3 RejectTint(uint reasons)
{
    switch (PickRejectColor(reasons))
    {
    case 0u:
        return float3(1.0f, 0.0f, 1.0f);
    case 1u:
        return float3(1.0f, 1.0f, 1.0f);
    case 2u:
        return float3(0.0f, 0.5f, 1.0f);
    case 3u:
        return float3(1.0f, 1.0f, 0.0f);
    case 4u:
        return float3(1.0f, 0.0f, 0.0f);
    case 5u:
        return float3(0.0f, 1.0f, 1.0f);
    case 6u:
        return float3(1.0f, 0.5f, 0.0f);
    default:
        return float3(0.125f, 0.125f, 0.125f);
    }
}

[numthreads(8, 8, 1)]
void GenerateCurrentFrameCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gHeader0.x || dispatchThreadId.y >= gHeader0.y)
    {
        return;
    }

    float2 rasterUv = RasterUv(dispatchThreadId.xy);
    float nearestDepth = 3.402823466e38f;
    float4 currentColor = 0.0f.xxxx;
    float currentDepth = nearestDepth;
    uint currentIdentity = 0u;
    float4 motionClipDepth = float4(0.0f, 0.0f, 1.0f, 0.0f);
    bool found = false;

    [unroll(4)]
    for (uint surfaceIndex = 0u; surfaceIndex < gHeader0.z; ++surfaceIndex)
    {
        float2 localUv;
        if (!TryResolveLocal(gSurfaces[surfaceIndex].currentRectUv, rasterUv, gCurrentJitterExposure.xy, localUv))
        {
            continue;
        }

        float surfaceDepth = gSurfaces[surfaceIndex].depths.x;
        if (!found || surfaceDepth < nearestDepth)
        {
            float4 currentClip = TransformRows(gSurfaces[surfaceIndex].currentClipRows, localUv);
            float4 previousClip = TransformRows(gSurfaces[surfaceIndex].previousClipRows, localUv);
            float2 currentUv = ProjectUnjitteredUv(currentClip);
            float2 previousUv = ProjectUnjitteredUv(previousClip);

            nearestDepth = surfaceDepth;
            currentColor = gSurfaces[surfaceIndex].currentColor;
            currentDepth = surfaceDepth;
            currentIdentity = gSurfaces[surfaceIndex].metadata.x;
            motionClipDepth = float4(previousUv - currentUv, previousClip.w, gSurfaces[surfaceIndex].depths.y);
            found = true;
        }
    }

    gCurrentColorUav[dispatchThreadId.xy] = currentColor;
    gCurrentDepthUav[dispatchThreadId.xy] = currentDepth;
    gCurrentIdentityUav[dispatchThreadId.xy] = currentIdentity;
    gMotionClipDepthUav[dispatchThreadId.xy] = motionClipDepth;
}

[numthreads(8, 8, 1)]
void ValidateReprojectionCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gHeader0.x || dispatchThreadId.y >= gHeader0.y)
    {
        return;
    }

    float2 rasterUv = RasterUv(dispatchThreadId.xy);
    float4 motionClipDepth = gMotionClipDepth.Load(int3(dispatchThreadId.xy, 0));
    float2 previousHistoryUv =
        rasterUv + motionClipDepth.xy + gPreviousJitterExposure.xy - gCurrentJitterExposure.xy;

    uint reasons = 0u;
    if ((gHeader0.w & 0x1u) == 0u)
    {
        reasons |= kRejectNoHistory;
    }
    if ((gHeader0.w & 0x2u) != 0u)
    {
        reasons |= kRejectReset;
    }
    if (!IsFinite2(previousHistoryUv) || !isfinite(motionClipDepth.z) || !isfinite(motionClipDepth.w))
    {
        reasons |= kRejectNonFinite;
    }
    if (isfinite(motionClipDepth.z) && motionClipDepth.z <= 0.0f)
    {
        reasons |= kRejectPreviousW;
    }

    float2 halfTexel = 0.5f / float2(gHeader0.xy);
    if (previousHistoryUv.x < halfTexel.x || previousHistoryUv.x > (1.0f - halfTexel.x) ||
        previousHistoryUv.y < halfTexel.y || previousHistoryUv.y > (1.0f - halfTexel.y))
    {
        reasons |= kRejectUvOutOfBounds;
    }

    float sampledPreviousDepth = gHistoryDepth.SampleLevel(gPointClamp, previousHistoryUv, 0.0f);
    float localGradient = DepthGradient(previousHistoryUv);
    float depthScale = max(abs(motionClipDepth.w), abs(sampledPreviousDepth));
    float tolerance = max(max(gDepthSettings.x, gDepthSettings.y * depthScale), gDepthSettings.z * abs(localGradient));
    if (abs(sampledPreviousDepth - motionClipDepth.w) > tolerance)
    {
        reasons |= kRejectDepthMismatch;
    }

    uint currentIdentity = gCurrentIdentity.Load(int3(dispatchThreadId.xy, 0));
    uint previousIdentity = PointSampleIdentity(previousHistoryUv);
    if (currentIdentity != previousIdentity)
    {
        reasons |= kRejectIdentityMismatch;
    }

    float scale = gCurrentJitterExposure.z / gPreviousJitterExposure.z;
    float symmetricRatio = max(scale, 1.0f / scale);
    if (!isfinite(scale) || gCurrentJitterExposure.z <= 0.0f || gPreviousJitterExposure.z <= 0.0f ||
        !isfinite(symmetricRatio) || symmetricRatio > gPreviousJitterExposure.w)
    {
        reasons |= kRejectExposure;
        if (gCurrentJitterExposure.z <= 0.0f || gPreviousJitterExposure.z <= 0.0f || !isfinite(scale))
        {
            scale = asfloat(0x7FC00000u);
        }
    }

    gPreviousHistoryUvUav[dispatchThreadId.xy] = previousHistoryUv;
    gReprojectedHistoryColorUav[dispatchThreadId.xy] = gHistoryColor.SampleLevel(gLinearClamp, previousHistoryUv, 0.0f) * scale;
    gRejectionReasonsUav[dispatchThreadId.xy] = reasons;
    gExposureScaleUav[dispatchThreadId.xy] = scale;
}

struct FullscreenVertexOutput
{
    float4 position : SV_Position;
};

FullscreenVertexOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2(-1.0f, 3.0f),
        float2(3.0f, -1.0f),
    };

    FullscreenVertexOutput output;
    output.position = float4(positions[vertexId], 0.0f, 1.0f);
    return output;
}

float4 CompositePS(FullscreenVertexOutput input) : SV_Target0
{
    int2 pixel = int2(input.position.xy);
    float4 currentColor = gCurrentColor.Load(int3(pixel, 0));
    float4 historyColor = gReprojectedHistoryColor.Load(int3(pixel, 0));
    uint reasons = gRejectionReasons.Load(int3(pixel, 0));

    if (reasons == 0u)
    {
        return saturate(float4((0.5f * currentColor.rgb) + (0.5f * historyColor.rgb) + float3(0.0f, 0.125f, 0.0f),
                               1.0f));
    }

    float3 tint = RejectTint(reasons);
    return saturate(float4((0.25f * currentColor.rgb) + (0.75f * tint), 1.0f));
}
