// Chapter 29 shared GPU contract for screen-space reflections and screen-space diffuse indirect lighting.
//
// Everything in this file is infrastructure that both the Starter and the Solution are entitled to: the analytic
// scene, the G-buffer pass that publishes screen data, the projection and depth conversions, the environment, the
// split-sum specular weight, deterministic sampling, and the diagnostic display. The screen-space transport itself
// lives in the per-variant shader, because that is the algorithm the chapter teaches.
//
// Colours are scene-linear watts per square metre per steradian everywhere except the final display conversion.
// View space is left-handed with +X right, +Y up, +Z forward, and the eye at the origin, so a visible point has
// viewPosition.z > 0. Texture UV has its origin at the top-left, so the UV-Y flip lives in NdcFromUv/UvFromNdc and
// nowhere else.

static const float Pi = 3.14159265358979323846f;

static const uint StatusGBuffer = 1u;
static const uint StatusReconstruction = 2u;
static const uint StatusBaseline = 4u;
static const uint StatusRay = 8u;
static const uint StatusTraversal = 16u;
static const uint StatusConfidence = 32u;
static const uint StatusComposition = 64u;
static const uint StatusIndirect = 128u;
static const uint StatusTemporal = 256u;

// Mirrors ch29::screen_space_reflections::MissReason.
static const uint MissNone = 0u;
static const uint MissInvalidInput = 1u;
static const uint MissBackFacing = 2u;
static const uint MissBehindCamera = 3u;
static const uint MissOffScreen = 4u;
static const uint MissMaximumSteps = 5u;
static const uint MissThicknessExceeded = 6u;
static const uint MissNoCrossing = 7u;
static const uint MissMaximumDistance = 8u;

// Mirrors ch29::screen_space_reflections::ClipLimit.
static const uint ClipRayStart = 0u;
static const uint ClipRayEnd = 1u;
static const uint ClipNearPlane = 2u;
static const uint ClipFarPlane = 3u;
static const uint ClipLeftPlane = 4u;
static const uint ClipRightPlane = 5u;
static const uint ClipBottomPlane = 6u;
static const uint ClipTopPlane = 7u;

static const uint FlagReversedDepth = 1u;
static const uint FlagConstantEnvironment = 2u;
static const uint FlagScreenTracing = 4u;
static const uint FlagIndirect = 8u;
static const uint FlagTemporal = 16u;
static const uint FlagHistoryValid = 32u;
static const uint FlagReset = 64u;

static const uint RecordHit = 1u;
static const uint RecordStepBudgetExhausted = 2u;
static const uint RecordSegmentValid = 4u;
static const uint RecordHistoryUsable = 8u;
static const uint RecordBackground = 16u;

// Mirrors ch29::screen_space_reflections::HistoryRejection. A history sample is reused only when no bit is set.
static const uint HistoryRejectNoHistory = 1u;
static const uint HistoryRejectOffScreen = 2u;
static const uint HistoryRejectNoSamples = 4u;
static const uint HistoryRejectMaterial = 8u;
static const uint HistoryRejectDepth = 16u;
// Lab-only: temporal accumulation was switched off, so no history was consulted at all.
static const uint HistoryRejectDisabled = 32u;

static const uint AbiMarker = 0x53535229u;

static const uint MaterialBackground = 0u;
static const uint MaterialFloor = 1u;
static const uint MaterialBox = 2u;
static const uint MaterialWall = 3u;
static const uint MaterialPanel = 4u;

struct PixelRecord
{
    float deviceDepth;
    float viewDepth;
    float viewPositionX;
    float viewPositionY;
    float viewPositionZ;
    float normalX;
    float normalY;
    float normalZ;
    float roughness;
    float albedoR;
    float albedoG;
    float albedoB;
    float directR;
    float directG;
    float directB;
    float motionX;
    float motionY;
    float rayDirectionX;
    float rayDirectionY;
    float rayDirectionZ;
    float nDotV;
    float appliedNormalBias;
    float towardCameraCosine;
    float segmentStartUvX;
    float segmentStartUvY;
    float segmentEndUvX;
    float segmentEndUvY;
    float segmentScreenLengthTexels;
    float segmentExitDistance;
    float hitUvX;
    float hitUvY;
    float hitRayViewDepth;
    float hitSceneViewDepth;
    float hitDepthDelta;
    float hitThicknessInterval;
    float hitRayDistance;
    float hitParameter;
    float confidenceValidity;
    float confidenceScreenEdge;
    float confidenceRayDistance;
    float confidenceThickness;
    float confidenceRoughness;
    float confidenceTowardCamera;
    float confidenceCombined;
    float screenWeight;
    float environmentWeight;
    float screenRadianceR;
    float screenRadianceG;
    float screenRadianceB;
    float environmentRadianceR;
    float environmentRadianceG;
    float environmentRadianceB;
    float incomingRadianceR;
    float incomingRadianceG;
    float incomingRadianceB;
    float splitSumA;
    float splitSumB;
    float specularWeightR;
    float specularWeightG;
    float specularWeightB;
    float reflectionR;
    float reflectionG;
    float reflectionB;
    float baselineReflectionR;
    float baselineReflectionG;
    float baselineReflectionB;
    float indirectMeanIncomingR;
    float indirectMeanIncomingG;
    float indirectMeanIncomingB;
    float indirectIrradianceR;
    float indirectIrradianceG;
    float indirectIrradianceB;
    float indirectOutgoingR;
    float indirectOutgoingG;
    float indirectOutgoingB;
    float indirectAverageCosineOverPdf;
    float indirectConfidence;
    float currentConfidence;
    float previousUvX;
    float previousUvY;
    float previousViewDepth;
    float currentWeight;
    float historyWeight;
    float historyR;
    float historyG;
    float historyB;
    float historyConfidence;
    float historyViewDepth;
    float historyDepthDifference;
    float historyDepthTolerance;
    float outputConfidence;
    float temporalR;
    float temporalG;
    float temporalB;
    float baselineR;
    float baselineG;
    float baselineB;
    float finalR;
    float finalG;
    float finalB;
    uint materialId;
    uint missReason;
    uint stepCount;
    uint refinementCount;
    uint geometrySampleCount;
    uint thicknessRejectionCount;
    uint requiredStepCount;
    uint indirectHitCount;
    uint indirectMissCount;
    uint indirectClampedCount;
    uint indirectSampleCount;
    uint previousSampleCount;
    uint nextSampleCount;
    uint historyMaterialId;
    uint historyRejectionReasons;
    uint flags;
    uint status;
    uint abiMarker;
};

struct SurfaceRecord
{
    float normalX;
    float normalY;
    float normalZ;
    float roughness;
    float albedoR;
    float albedoG;
    float albedoB;
    float viewDepth;
    float radianceR;
    float radianceG;
    float radianceB;
    float motionX;
    float motionY;
    float previousViewDepth;
    uint materialId;
    uint padding;
};

struct HistoryPixel
{
    float colorR;
    float colorG;
    float colorB;
    float confidence;
    float viewDepth;
    uint materialId;
    uint sampleCount;
    uint padding;
};

cbuffer DispatchConstants : register(b0)
{
    uint Width;
    uint Height;
    uint Flags;
    uint MaximumStepCount;
    uint RefinementStepCount;
    uint IndirectSampleCount;
    uint IndirectMaximumStepCount;
    uint SplitSumSampleCount;
    uint MaximumTemporalSampleCount;
    uint SolutionMode;
    float NearPlane;
    float FarPlane;
    float TanHalfVerticalFov;
    float TanHalfHorizontalFov;
    float StepLengthTexels;
    float StartOffsetFraction;
    float ConstantThickness;
    float DepthProportionalThickness;
    float ConstantNormalBias;
    float DepthProportionalNormalBias;
    float MinimumFacingCosine;
    float MaximumRayDistance;
    float IndirectMaximumRayDistance;
    float IndirectMaximumRadiance;
    float ScreenEdgeFadeUv;
    float DistanceFadeStartFraction;
    float ThicknessFadeFraction;
    float RoughnessFadeStart;
    float RoughnessFadeEnd;
    float TowardCameraFadeStart;
    float TowardCameraFadeEnd;
    float MaximumHistoryWeight;
    float AbsoluteDepthTolerance;
    float RelativeDepthTolerance;
    float BoxCenterX;
    float BoxCenterZ;
    float PreviousBoxCenterX;
    float PreviousBoxCenterZ;
    uint MaterialIdentityRequired;
    float ConstantsPadding;
};

RWTexture2D<float> GBufferDepthWrite : register(u0);
RWStructuredBuffer<SurfaceRecord> GBufferSurfaceWrite : register(u1);
RWStructuredBuffer<PixelRecord> Diagnostics : register(u2);
RWStructuredBuffer<HistoryPixel> HistoryWrite : register(u3);
Texture2D<float> GBufferDepth : register(t0);
StructuredBuffer<SurfaceRecord> GBufferSurface : register(t1);
StructuredBuffer<HistoryPixel> HistoryRead : register(t2);

bool ReversedDepth()
{
    return (Flags & FlagReversedDepth) != 0u;
}

float DepthClearValue()
{
    return ReversedDepth() ? 0.0f : 1.0f;
}

// A texel still holding the clear value is background: the depth clear value is the farthest representable device
// depth in both conventions, so background and far-plane geometry are indistinguishable and neither can be hit.
bool IsBackgroundDeviceDepth(float deviceDepth)
{
    return deviceDepth == DepthClearValue();
}

float2 NdcFromUv(float2 uv)
{
    return float2(2.0f * uv.x - 1.0f, 1.0f - 2.0f * uv.y);
}

float2 UvFromNdc(float2 ndc)
{
    return float2((ndc.x + 1.0f) * 0.5f, (1.0f - ndc.y) * 0.5f);
}

float2 PixelCenterUv(uint2 pixel)
{
    return (float2(pixel) + 0.5f) / float2(Width, Height);
}

float3 ViewPositionFromUv(float2 uv, float viewDepth)
{
    float2 ndc = NdcFromUv(uv);
    return float3(ndc.x * viewDepth * TanHalfHorizontalFov, ndc.y * viewDepth * TanHalfVerticalFov, viewDepth);
}

// Points at or behind the eye have no screen position; the caller must not use the returned UV when this is false.
bool ProjectViewPosition(float3 viewPosition, out float2 uv)
{
    if (viewPosition.z <= 0.0f)
    {
        uv = float2(0.0f, 0.0f);
        return false;
    }
    float2 ndc = float2(viewPosition.x / (viewPosition.z * TanHalfHorizontalFov),
                        viewPosition.y / (viewPosition.z * TanHalfVerticalFov));
    uv = UvFromNdc(ndc);
    return true;
}

// Both conventions are one ratio with denominator z * (f - n). Keeping the plane difference in the numerator makes
// the near and far planes encode exactly instead of cancelling two nearly equal terms.
float DeviceDepthFromViewDepth(float viewDepth)
{
    float range = FarPlane - NearPlane;
    float clamped = clamp(viewDepth, NearPlane, FarPlane);
    float numerator = ReversedDepth() ? NearPlane * (FarPlane - clamped) : FarPlane * (clamped - NearPlane);
    return saturate(numerator / (clamped * range));
}

// z = n * f / lerp(planes). The plane encodings decode back to the plane distances exactly.
float ViewDepthFromDeviceDepth(float deviceDepth)
{
    float clamped = saturate(deviceDepth);
    if (clamped <= 0.0f)
    {
        return ReversedDepth() ? FarPlane : NearPlane;
    }
    if (clamped >= 1.0f)
    {
        return ReversedDepth() ? NearPlane : FarPlane;
    }
    float complement = 1.0f - clamped;
    float denominator = ReversedDepth() ? (NearPlane * complement + FarPlane * clamped)
                                        : (FarPlane * complement + NearPlane * clamped);
    return clamp((NearPlane * FarPlane) / denominator, NearPlane, FarPlane);
}

// uv exactly one lands on the first texel outside the image; the last texel owns that boundary.
uint2 TexelFromUv(float2 uv)
{
    float2 scaled = floor(saturate(uv) * float2(Width, Height));
    return uint2(min(scaled, float2(Width - 1u, Height - 1u)));
}

// Depth is never filtered: a bilinear blend of two surfaces is a depth that belongs to neither of them.
float SampleDeviceDepthNearest(float2 uv)
{
    return GBufferDepth.Load(int3(int2(TexelFromUv(uv)), 0));
}

SurfaceRecord LoadSurface(uint2 texel)
{
    return GBufferSurface[texel.y * Width + texel.x];
}

float3 SurfaceRadiance(SurfaceRecord surface)
{
    return float3(surface.radianceR, surface.radianceG, surface.radianceB);
}

// ---------------------------------------------------------------------------------------------------------------
// Deterministic analytic scene. It is evaluated once per pixel by the G-buffer pass and never consulted again:
// screen-space transport is only allowed to see what that pass published.
// ---------------------------------------------------------------------------------------------------------------

static const float3 LightDirection = float3(-0.350456f, 0.821068f, -0.450586f);
static const float3 LightIrradiance = float3(3.0f, 2.9f, 2.7f);
static const float FloorHeight = -2.0f;
static const float WallDistance = 26.0f;
static const float3 BoxHalfExtents = float3(1.5f, 1.0f, 0.8f);
static const float3 PanelNormal = float3(0.0f, 0.6f, -0.8f);
static const float PanelOffset = -8.6f;

struct SceneHit
{
    float distance;
    float3 position;
    float3 normal;
    uint materialId;
};

SceneHit MakeSceneMiss()
{
    SceneHit hit;
    hit.distance = 1.0e9f;
    hit.position = float3(0.0f, 0.0f, 0.0f);
    hit.normal = float3(0.0f, 0.0f, 1.0f);
    hit.materialId = MaterialBackground;
    return hit;
}

void IntersectBoundedPlane(float3 origin, float3 direction, float3 planeNormal, float planeOffset, float3 boundsMin,
                           float3 boundsMax, uint materialId, inout SceneHit best)
{
    float denominator = dot(planeNormal, direction);
    if (abs(denominator) < 1.0e-7f)
    {
        return;
    }
    float distance = (planeOffset - dot(planeNormal, origin)) / denominator;
    if (distance <= 1.0e-4f || distance >= best.distance)
    {
        return;
    }
    float3 position = origin + direction * distance;
    if (any(position < boundsMin) || any(position > boundsMax))
    {
        return;
    }
    best.distance = distance;
    best.position = position;
    best.normal = denominator < 0.0f ? planeNormal : -planeNormal;
    best.materialId = materialId;
}

void IntersectBox(float3 origin, float3 direction, float3 center, float3 halfExtents, uint materialId,
                  inout SceneHit best)
{
    float3 magnitude = max(abs(direction), 1.0e-8f);
    float3 signs = select(direction < 0.0f, float3(-1.0f, -1.0f, -1.0f), float3(1.0f, 1.0f, 1.0f));
    float3 inverseDirection = 1.0f / (signs * magnitude);
    float3 firstSlab = (center - halfExtents - origin) * inverseDirection;
    float3 secondSlab = (center + halfExtents - origin) * inverseDirection;
    float3 slabMinimum = min(firstSlab, secondSlab);
    float3 slabMaximum = max(firstSlab, secondSlab);
    float entryDistance = max(slabMinimum.x, max(slabMinimum.y, slabMinimum.z));
    float exitDistance = min(slabMaximum.x, min(slabMaximum.y, slabMaximum.z));
    if (exitDistance < entryDistance || entryDistance <= 1.0e-4f || entryDistance >= best.distance)
    {
        return;
    }
    float3 axis = select(slabMinimum >= entryDistance, float3(1.0f, 1.0f, 1.0f), float3(0.0f, 0.0f, 0.0f));
    best.distance = entryDistance;
    best.position = origin + direction * entryDistance;
    best.normal = normalize(-signs * axis);
    best.materialId = materialId;
}

SceneHit IntersectScene(float3 origin, float3 direction, float3 boxCenter)
{
    SceneHit best = MakeSceneMiss();
    IntersectBoundedPlane(origin, direction, float3(0.0f, 1.0f, 0.0f), FloorHeight, float3(-40.0f, -3.0f, 1.0f),
                          float3(40.0f, -1.0f, WallDistance), MaterialFloor, best);
    IntersectBoundedPlane(origin, direction, float3(0.0f, 0.0f, -1.0f), -WallDistance,
                          float3(-40.0f, FloorHeight, WallDistance - 1.0f), float3(40.0f, 1.0f, WallDistance + 1.0f),
                          MaterialWall, best);
    IntersectBoundedPlane(origin, direction, PanelNormal, PanelOffset, float3(-7.0f, FloorHeight, 8.0f),
                          float3(-3.0f, 1.5f, 13.0f), MaterialPanel, best);
    IntersectBox(origin, direction, boxCenter, BoxHalfExtents, MaterialBox, best);
    return best;
}

void EvaluateMaterial(uint materialId, float3 position, out float3 albedo, out float roughness, out float3 emissive)
{
    albedo = float3(0.0f, 0.0f, 0.0f);
    roughness = 0.0f;
    emissive = float3(0.0f, 0.0f, 0.0f);
    if (materialId == MaterialFloor)
    {
        albedo = float3(0.35f, 0.33f, 0.30f);
        // A roughness ramp across the floor makes the single-mirror-ray assumption fail visibly from left to right.
        roughness = saturate(0.5f + position.x * 0.125f);
    }
    else if (materialId == MaterialBox)
    {
        albedo = float3(0.05f, 0.05f, 0.05f);
        roughness = 0.2f;
        emissive = float3(7.0f, 3.2f, 1.1f);
    }
    else if (materialId == MaterialWall)
    {
        float checker = 2.0f * frac(0.5f * (floor(position.x * 1.5f) + floor(position.y * 1.5f)));
        albedo = lerp(float3(0.10f, 0.12f, 0.15f), float3(0.55f, 0.50f, 0.42f), checker);
        roughness = 0.55f;
    }
    else if (materialId == MaterialPanel)
    {
        albedo = float3(0.20f, 0.22f, 0.28f);
        roughness = 0.05f;
    }
}

// One directional light and an emissive term. This is the radiance that already left the surface toward the eye,
// which is exactly what a screen-space hit is allowed to reuse: there is no second bounce anywhere in this lab.
float3 OutgoingRadiance(float3 albedo, float3 normal, float3 emissive)
{
    return emissive + (albedo / Pi) * LightIrradiance * max(0.0f, dot(normal, LightDirection));
}

// The environment is analytic and is sampled in the mirror direction only. It is deliberately not prefiltered by
// roughness: prefiltering and mip selection are Chapter 27's material, and importing them here would hide which of
// the two chapters is responsible for a given blur. Roughness therefore enters this chapter only through the
// split-sum weight and the confidence fade, and a rough surface still reads a sharp environment.
float3 EvaluateEnvironment(float3 direction)
{
    if ((Flags & FlagConstantEnvironment) != 0u)
    {
        return float3(0.35f, 0.35f, 0.35f);
    }
    float3 unitDirection = normalize(direction);
    float upward = saturate(0.5f * (unitDirection.y + 1.0f));
    float3 sky = lerp(float3(0.05f, 0.06f, 0.09f), float3(0.26f, 0.42f, 0.85f), upward);
    float sun = saturate(dot(unitDirection, LightDirection));
    return sky + float3(2.6f, 2.2f, 1.6f) * pow(sun, 96.0f);
}

// ---------------------------------------------------------------------------------------------------------------
// Deterministic sampling and the split-sum specular weight.
// ---------------------------------------------------------------------------------------------------------------

float RadicalInverseBase2(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);
    bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0f0u) >> 4u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xccccccccu) >> 2u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xaaaaaaaau) >> 1u);
    return float(bits) * (1.0f / 4294967296.0f);
}

float2 Hammersley(uint index, uint count)
{
    return float2((float(index) + 0.5f) / float(count), RadicalInverseBase2(index));
}

float SmithG1(float cosine, float alpha)
{
    if (cosine <= 0.0f)
    {
        return 0.0f;
    }
    float alphaSquared = alpha * alpha;
    return (2.0f * cosine) / (cosine + sqrt(alphaSquared + (1.0f - alphaSquared) * cosine * cosine));
}

// Chapter 27's split-sum environment BRDF term. It returns A and B for F0 * A + B, both in [0, 1]. The weight is
// applied exactly once, after screen and environment radiance have already been combined.
float2 IntegrateSplitSum(float nDotView, float roughness, uint sampleCount)
{
    float nv = clamp(nDotView, 1.0e-3f, 1.0f);
    float3 view = float3(sqrt(max(0.0f, 1.0f - nv * nv)), 0.0f, nv);
    float alpha = max(roughness * roughness, 1.0e-4f);
    float alphaSquared = alpha * alpha;
    float viewVisibility = SmithG1(nv, alpha);
    float2 result = float2(0.0f, 0.0f);
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        float2 unitSample = Hammersley(sampleIndex, sampleCount);
        float cosineTheta = sqrt((1.0f - unitSample.y) / (1.0f + (alphaSquared - 1.0f) * unitSample.y));
        float sineTheta = sqrt(max(0.0f, 1.0f - cosineTheta * cosineTheta));
        float azimuth = 2.0f * Pi * unitSample.x;
        float3 halfVector = float3(sineTheta * cos(azimuth), sineTheta * sin(azimuth), cosineTheta);
        float vDotH = dot(view, halfVector);
        float3 lightDirection = 2.0f * vDotH * halfVector - view;
        if (lightDirection.z <= 0.0f || halfVector.z <= 0.0f || vDotH <= 0.0f)
        {
            continue;
        }
        float visibility = (viewVisibility * SmithG1(lightDirection.z, alpha) * vDotH) / (halfVector.z * nv);
        float fresnelComplement = pow(1.0f - vDotH, 5.0f);
        result.x += (1.0f - fresnelComplement) * visibility;
        result.y += fresnelComplement * visibility;
    }
    return saturate(result / float(sampleCount));
}

// R = 2 * dot(N, V) * N - V. The result satisfies dot(R, N) = dot(V, N), so a mirror ray never starts by pointing
// into the surface it left.
float3 MirrorDirection(float3 normal, float3 viewDirection)
{
    return normalize(2.0f * dot(normal, viewDirection) * normal - viewDirection);
}

// Branchless orthonormal basis (Duff et al.), matching BuildTangentFrame in the CPU contracts.
void BuildTangentFrame(float3 normal, out float3 tangent, out float3 bitangent)
{
    float orientation = normal.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (orientation + normal.z);
    float b = normal.x * normal.y * a;
    tangent = float3(1.0f + orientation * normal.x * normal.x * a, orientation * b, -orientation * normal.x);
    bitangent = float3(b, orientation + normal.y * normal.y * a, -normal.y);
}

// Malley's method: r = sqrt(u1), phi = 2*pi*u2, z = sqrt(1 - u1). The density is cos(theta)/pi, so cos/pdf is
// exactly pi for every accepted sample.
float3 MapCosineHemisphere(float2 unitSample, out float cosine)
{
    float radius = sqrt(unitSample.x);
    float azimuth = 2.0f * Pi * unitSample.y;
    cosine = sqrt(max(0.0f, 1.0f - unitSample.x));
    return float3(radius * cos(azimuth), radius * sin(azimuth), cosine);
}

// Linear Rec. 709 luminance of a scene-linear colour. It is used only to weigh how much each screen-space term
// contributes to the accumulated sum; nothing in this chapter blends gamma-coded values.
float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

// Linear fade from one at start to zero at end. The caller guarantees end > start.
float FadeOut(float value, float start, float end)
{
    return 1.0f - saturate((value - start) / (end - start));
}

bool Finite3(float3 value)
{
    return all(isfinite(value));
}

PixelRecord MakeEmptyRecord()
{
    PixelRecord record = (PixelRecord)0;
    record.missReason = MissInvalidInput;
    record.abiMarker = AbiMarker;
    return record;
}

// ---------------------------------------------------------------------------------------------------------------
// Pass one: publish the only screen data the reflection pass may read.
// ---------------------------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)] void GBufferCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Width || dispatchThreadId.y >= Height)
    {
        return;
    }
    uint2 pixel = dispatchThreadId.xy;
    uint index = pixel.y * Width + pixel.x;
    float2 uv = PixelCenterUv(pixel);
    float2 ndc = NdcFromUv(uv);
    float3 direction = normalize(float3(ndc.x * TanHalfHorizontalFov, ndc.y * TanHalfVerticalFov, 1.0f));

    SceneHit hit = IntersectScene(float3(0.0f, 0.0f, 0.0f), direction, float3(BoxCenterX, 0.0f, BoxCenterZ));
    float viewDepth = hit.materialId == MaterialBackground ? 0.0f : hit.position.z;
    bool visible = hit.materialId != MaterialBackground && viewDepth >= NearPlane && viewDepth <= FarPlane;

    SurfaceRecord surface = (SurfaceRecord)0;
    float deviceDepth = DepthClearValue();
    if (visible)
    {
        float3 albedo;
        float roughness;
        float3 emissive;
        EvaluateMaterial(hit.materialId, hit.position, albedo, roughness, emissive);
        float3 radiance = OutgoingRadiance(albedo, hit.normal, emissive);
        deviceDepth = DeviceDepthFromViewDepth(viewDepth);

        // Chapter 28 motion convention: previousUV - currentUV. Only the animated emitter moves; the camera is
        // static, so every other surface reports exactly zero motion.
        //
        // previousViewDepth is where this surface stood along the view axis in the frame that produced the history.
        // It is not the current depth: the emitter also slides toward and away from the eye, so a history check
        // that compared the current depth would reject the emitter every frame even though it reprojected
        // perfectly.
        float2 motion = float2(0.0f, 0.0f);
        float previousViewDepth = viewDepth;
        if (hit.materialId == MaterialBox)
        {
            float3 previousPosition =
                hit.position + float3(PreviousBoxCenterX - BoxCenterX, 0.0f, PreviousBoxCenterZ - BoxCenterZ);
            float2 previousUv;
            if (ProjectViewPosition(previousPosition, previousUv))
            {
                motion = previousUv - uv;
                previousViewDepth = previousPosition.z;
            }
        }

        surface.normalX = hit.normal.x;
        surface.normalY = hit.normal.y;
        surface.normalZ = hit.normal.z;
        surface.roughness = roughness;
        surface.albedoR = albedo.r;
        surface.albedoG = albedo.g;
        surface.albedoB = albedo.b;
        surface.viewDepth = viewDepth;
        surface.radianceR = radiance.r;
        surface.radianceG = radiance.g;
        surface.radianceB = radiance.b;
        surface.motionX = motion.x;
        surface.motionY = motion.y;
        surface.previousViewDepth = previousViewDepth;
        surface.materialId = hit.materialId;
    }
    else
    {
        // Background carries no surface, so it can never be intersected. It still owns a radiance: the environment
        // seen directly along the primary ray.
        float3 radiance = EvaluateEnvironment(direction);
        surface.radianceR = radiance.r;
        surface.radianceG = radiance.g;
        surface.radianceB = radiance.b;
        surface.normalZ = -1.0f;
        surface.materialId = MaterialBackground;
    }

    GBufferDepthWrite[pixel] = deviceDepth;
    GBufferSurfaceWrite[index] = surface;
}

// ---------------------------------------------------------------------------------------------------------------
// Display.
// ---------------------------------------------------------------------------------------------------------------

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
    uint DisplaySolutionMode;
    float DisplayNearPlane;
    float DisplayFarPlane;
    float DisplayPadding;
};

StructuredBuffer<PixelRecord> DisplayRecords : register(t0);

float3 DisplayToneMap(float3 color)
{
    float3 bounded = max(color, 0.0f);
    return pow(bounded / (1.0f + bounded), 1.0f / 2.2f);
}

float3 MissReasonColor(uint missReason, bool hit)
{
    if (hit)
    {
        return float3(0.10f, 0.85f, 0.20f);
    }
    if (missReason == MissInvalidInput)
    {
        return float3(0.08f, 0.08f, 0.10f);
    }
    if (missReason == MissBackFacing)
    {
        return float3(0.60f, 0.60f, 0.10f);
    }
    if (missReason == MissBehindCamera)
    {
        return float3(0.95f, 0.20f, 0.20f);
    }
    if (missReason == MissOffScreen)
    {
        return float3(0.20f, 0.35f, 0.95f);
    }
    if (missReason == MissMaximumSteps)
    {
        return float3(0.95f, 0.45f, 0.05f);
    }
    if (missReason == MissThicknessExceeded)
    {
        return float3(0.90f, 0.15f, 0.85f);
    }
    if (missReason == MissNoCrossing)
    {
        return float3(0.25f, 0.75f, 0.85f);
    }
    return float3(0.75f, 0.75f, 0.75f);
}

float3 DisplayColor(PixelRecord record)
{
    float3 baseline = float3(record.baselineR, record.baselineG, record.baselineB);
    uint view = SelectedDebugView;
    bool solutionOnly = view >= 3u;
    if (solutionOnly && DisplaySolutionMode == 0u)
    {
        // The Starter has no screen-space evidence to draw, so it ignores the Solution-only views instead of
        // failing configuration or inventing a picture.
        view = 0u;
    }
    if (view == 0u)
    {
        return DisplayToneMap(baseline);
    }
    if (view == 1u)
    {
        float normalized =
            saturate((record.viewDepth - DisplayNearPlane) / max(1.0e-6f, DisplayFarPlane - DisplayNearPlane));
        float encoded = pow(saturate(normalized * 6.0f), 0.45f);
        return record.materialId == 0u ? float3(0.0f, 0.0f, 0.0f) : float3(encoded, 1.0f - encoded, record.deviceDepth);
    }
    if (view == 2u)
    {
        float3 normal = float3(record.normalX, record.normalY, record.normalZ);
        return float3(0.5f * normal.x + 0.5f, 0.5f * normal.y + 0.5f, record.roughness);
    }
    if (view == 3u)
    {
        return MissReasonColor(record.missReason, (record.flags & RecordHit) != 0u);
    }
    if (view == 4u)
    {
        return float3(record.confidenceScreenEdge, record.confidenceRayDistance, record.confidenceThickness);
    }
    if (view == 5u)
    {
        return float3(record.confidenceCombined, record.confidenceRoughness, record.confidenceTowardCamera);
    }
    if (view == 6u)
    {
        return DisplayToneMap(float3(record.screenRadianceR, record.screenRadianceG, record.screenRadianceB) *
                              record.screenWeight);
    }
    if (view == 7u)
    {
        return DisplayToneMap(
            float3(record.environmentRadianceR, record.environmentRadianceG, record.environmentRadianceB) *
            record.environmentWeight);
    }
    if (view == 8u)
    {
        return DisplayToneMap(float3(record.indirectOutgoingR, record.indirectOutgoingG, record.indirectOutgoingB));
    }
    if (view == 10u)
    {
        // Red marks a rejected history sample, green how much of the result the accepted history owns, and blue the
        // confidence that survives into the next frame.
        return float3(record.historyRejectionReasons != 0u ? 1.0f : 0.0f, record.historyWeight,
                      record.outputConfidence);
    }
    return DisplayToneMap(float3(record.temporalR, record.temporalG, record.temporalB));
}

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
    uint2 pixel = min(uint2(position.xy), uint2(DisplayWidth - 1u, DisplayHeight - 1u));
    PixelRecord record = DisplayRecords[pixel.y * DisplayWidth + pixel.x];
    if ((record.status & ExpectedStatus) != ExpectedStatus || record.abiMarker != AbiMarker)
    {
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    }
    return float4(DisplayColor(record), 1.0f);
}
