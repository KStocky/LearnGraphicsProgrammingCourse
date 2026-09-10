// Chapter 33 shared GPU lab support.
//
// This file owns the parts of the DXR lab that are not the lesson: the constant-buffer ABI, the resource
// declarations, the record layouts the C++ side mirrors, the analytic scene's ray generation, and the display
// path. It deliberately owns no traversal. The Starter's object-space ray transform and Moller-Trumbore
// intersection live in Starter/DxrLab.hlsl, and the ray generation, miss and closest-hit shaders of the real
// raytracing pipeline live in Solution/DxrLab.hlsl, because those are the chapter.
//
// Conventions that hold everywhere below:
//
//   * Distances are metres along the ray's direction, and every ray in this lab has a unit-length direction, so
//     `t` is a world-space distance without a scale factor to remember.
//   * The instance transform is a row-major 3x4 object-to-world matrix, stored as three `float4` rows exactly the
//     way `D3D12_RAYTRACING_INSTANCE_DESC::Transform` stores it. Its inverse is stored beside it because every
//     transform in this scene is a dyadic scale and translation whose inverse is exact.
//   * Triangle facing is decided in *object* space, which is what DXR specifies. A mirrored instance transform
//     does not flip it, and the lab measures that rather than asserting it.
//   * Barycentrics follow the DXR attribute convention: the pair (b1, b2) weights the vertices as
//     v0 * (1 - b1 - b2) + v1 * b1 + v2 * b2.

#ifndef LGP_CH33_DXR_SHARED_HLSLI
#define LGP_CH33_DXR_SHARED_HLSLI

// Mirrors ch33::dxr::gpu status bits.
static const uint StatusTraversalRan = 1u << 0u;
static const uint StatusHit = 1u << 1u;
static const uint StatusMiss = 1u << 2u;
static const uint StatusFrontFace = 1u << 3u;
static const uint StatusBackFace = 1u << 4u;
static const uint StatusClosestHitRan = 1u << 5u;
static const uint StatusMissShaderRan = 1u << 6u;
static const uint StatusLocalRootArgumentsRead = 1u << 7u;
static const uint StatusFixedFunctionTraversal = 1u << 8u;
static const uint StatusAnalyticTraversal = 1u << 9u;

// Mirrors the HLSL `HitKind()` values of the fixed-function triangle intersector.
static const uint HitKindFrontFace = 254u;
static const uint HitKindBackFace = 255u;

static const uint InvalidIndex = 0xffffffffu;
static const uint AbiMarker = 0x44785233u;

// Mirrors ch33::dxr::gpu::DebugView.
static const uint ViewFinal = 0u;
static const uint ViewHitMiss = 1u;
static const uint ViewInstanceIdentity = 2u;
static const uint ViewPrimitiveIdentity = 3u;
static const uint ViewBarycentrics = 4u;
static const uint ViewRayDistance = 5u;
static const uint ViewFaceOrientation = 6u;
static const uint ViewShaderRecord = 7u;
static const uint ViewStageLedger = 8u;

// The fixed distance window the RayDistance view maps across. It is a display constant rather than the ray
// interval so that the picture does not change when a test narrows tMin or tMax.
static const float DisplayNearMetres = 1.0f;
static const float DisplayFarMetres = 3.0f;

// Counter slots. They mirror ch33::dxr::gpu::FrameRecord field for field, as 32-bit words.
static const uint CounterAbiMarker = 0u;
static const uint CounterRayCount = 1u;
static const uint CounterHitCount = 2u;
static const uint CounterMissCount = 3u;
static const uint CounterFrontFaceCount = 4u;
static const uint CounterBackFaceCount = 5u;
static const uint CounterTraversalMask = 6u;
static const uint CounterDispatchWidth = 7u;
static const uint CounterDispatchHeight = 8u;
static const uint CounterReserved = 9u;
static const uint CounterHitGroupRecordBase = 10u;
static const uint CounterMissRecordBase = 14u;
static const uint CounterInstanceHitBase = 16u;

struct RayRecord
{
    uint status;
    uint instanceIndex;
    uint instanceId;
    uint primitiveIndex;
    uint hitKind;
    uint hitGroupRecordIndex;
    uint materialId;
    uint missShaderIndex;
    float tHit;
    float barycentricB1;
    float barycentricB2;
    float worldPositionX;
    float worldPositionY;
    float worldPositionZ;
    float colorR;
    float colorG;
    float colorB;
    uint stageMask;
    uint reservedA;
    uint reservedB;
};

struct InstanceRecord
{
    float4 objectToWorldRow0;
    float4 objectToWorldRow1;
    float4 objectToWorldRow2;
    float4 worldToObjectRow0;
    float4 worldToObjectRow1;
    float4 worldToObjectRow2;
    uint instanceId;
    uint instanceMask;
    uint instanceContributionToHitGroupIndex;
    uint instanceFlags;
};

// The same twenty bytes the Solution stores in a shader-table record's local root arguments. The Starter reads
// them from a structured buffer instead, which is precisely the difference the chapter is about.
struct MaterialRecord
{
    uint materialId;
    uint recordIndex;
    float colorR;
    float colorG;
    float colorB;
};

// The trace-side bindings exist only for the two shaders that actually trace. The display pass owns register b0
// and t0 for itself, so declaring both sets in one compilation would be a register collision rather than a
// convenience.
#if defined(LGP_CH33_ANALYTIC) || defined(LGP_CH33_RAYTRACING)
#define LGP_CH33_TRACE 1
#endif

#ifdef LGP_CH33_TRACE

cbuffer DispatchConstants : register(b0)
{
    uint Width;
    uint Height;
    uint InstanceCount;
    uint InstanceInclusionMask;
    uint RayContributionToHitGroupIndex;
    uint MultiplierForGeometryContribution;
    uint MissShaderIndex;
    uint ConfiguredRayFlags;
    float WindowHalfExtentX;
    float WindowHalfExtentY;
    float CameraOriginZ;
    float RayTMin;
    float RayTMax;
    uint HitGroupRecordCount;
    uint MaterialCount;
    uint FrameIndex;
};

RWStructuredBuffer<RayRecord> RayRecords : register(u0);
RWStructuredBuffer<uint> Counters : register(u1);

#ifdef LGP_CH33_ANALYTIC
StructuredBuffer<InstanceRecord> Instances : register(t0);
StructuredBuffer<float3> Vertices : register(t1);
StructuredBuffer<MaterialRecord> Materials : register(t2);
#endif

#ifdef LGP_CH33_RAYTRACING
RaytracingAccelerationStructure Scene : register(t0);
#endif

// The primary ray. Both variants must form exactly this ray, or nothing downstream is comparable.
float3 LabRayOrigin(uint2 pixel)
{
    float u = (float(pixel.x) + 0.5f) / float(Width);
    float v = (float(pixel.y) + 0.5f) / float(Height);
    return float3(-WindowHalfExtentX + 2.0f * WindowHalfExtentX * u,
                  WindowHalfExtentY - 2.0f * WindowHalfExtentY * v, CameraOriginZ);
}

float3 LabRayDirection()
{
    return float3(0.0f, 0.0f, -1.0f);
}

// The hit-group record index D3D12 computes at trace time. The Solution never calls this to shade with - the
// traversal already selected a record and the record reports its own index - but both variants publish the index
// the arithmetic predicts, so a disagreement is visible instead of invisible.
uint LabHitGroupRecordIndex(uint geometryContribution, uint instanceContribution)
{
    return RayContributionToHitGroupIndex + MultiplierForGeometryContribution * geometryContribution +
           instanceContribution;
}

RayRecord LabMakeMissRecord()
{
    RayRecord record;
    record.status = StatusTraversalRan | StatusMiss;
    record.instanceIndex = InvalidIndex;
    record.instanceId = InvalidIndex;
    record.primitiveIndex = InvalidIndex;
    record.hitKind = 0u;
    record.hitGroupRecordIndex = InvalidIndex;
    record.materialId = InvalidIndex;
    record.missShaderIndex = MissShaderIndex;
    record.tHit = 0.0f;
    record.barycentricB1 = 0.0f;
    record.barycentricB2 = 0.0f;
    record.worldPositionX = 0.0f;
    record.worldPositionY = 0.0f;
    record.worldPositionZ = 0.0f;
    record.colorR = 0.0f;
    record.colorG = 0.0f;
    record.colorB = 0.0f;
    record.stageMask = 0u;
    record.reservedA = 0u;
    record.reservedB = 0u;
    return record;
}

// The shade a hit publishes: the record's own colour, modulated by the barycentric coordinate so that a flat panel
// is still readable as a triangle and so that two records carrying the same colour would still be distinguishable
// by the geometry they cover.
float3 LabShadeHit(float3 materialColor, float b1, float b2, bool frontFace)
{
    float weight = 0.45f + 0.55f * saturate(1.0f - b1 - b2);
    float facing = frontFace ? 1.0f : 0.45f;
    return materialColor * weight * facing;
}

float3 LabMissColor(uint missShaderIndex, uint2 pixel)
{
    if (missShaderIndex == 0u)
    {
        float gradient = (float(pixel.y) + 0.5f) / float(Height);
        return float3(0.04f + 0.10f * gradient, 0.05f + 0.13f * gradient, 0.12f + 0.20f * gradient);
    }
    return float3(0.0f, 0.0f, 0.0f);
}

// Accumulates the frame-wide counters for one record. Every increment is an atomic because the whole point of the
// counters is that they are a second, independent statement about what the dispatch did.
void LabAccumulateCounters(RayRecord record, uint2 pixel)
{
    uint previous = 0u;
    InterlockedAdd(Counters[CounterRayCount], 1u, previous);
    InterlockedOr(Counters[CounterTraversalMask], record.status, previous);
    if ((record.status & StatusHit) != 0u)
    {
        InterlockedAdd(Counters[CounterHitCount], 1u, previous);
        if ((record.status & StatusFrontFace) != 0u)
        {
            InterlockedAdd(Counters[CounterFrontFaceCount], 1u, previous);
        }
        else
        {
            InterlockedAdd(Counters[CounterBackFaceCount], 1u, previous);
        }
        if (record.hitGroupRecordIndex < HitGroupRecordCount)
        {
            InterlockedAdd(Counters[CounterHitGroupRecordBase + record.hitGroupRecordIndex], 1u, previous);
        }
        if (record.instanceIndex < InstanceCount)
        {
            InterlockedAdd(Counters[CounterInstanceHitBase + record.instanceIndex], 1u, previous);
        }
    }
    else
    {
        InterlockedAdd(Counters[CounterMissCount], 1u, previous);
        if (record.missShaderIndex < 2u)
        {
            InterlockedAdd(Counters[CounterMissRecordBase + record.missShaderIndex], 1u, previous);
        }
    }
    if (pixel.x == 0u && pixel.y == 0u)
    {
        Counters[CounterAbiMarker] = AbiMarker;
        Counters[CounterDispatchWidth] = Width;
        Counters[CounterDispatchHeight] = Height;
    }
}

void LabStoreRecord(uint2 pixel, RayRecord record)
{
    RayRecords[pixel.y * Width + pixel.x] = record;
    LabAccumulateCounters(record, pixel);
}

#endif // LGP_CH33_TRACE

#ifdef LGP_CH33_DISPLAY

cbuffer DisplayConstants : register(b0)
{
    uint DisplayWidth;
    uint DisplayHeight;
    uint DisplaySurfaceWidth;
    uint DisplaySurfaceHeight;
    uint DisplayView;
    uint DisplayVariant;
    uint DisplayStageMask;
    uint DisplayStageSkippedMask;
    uint DisplayStageUnavailableMask;
    uint DisplayHitGroupRecordCount;
    float DisplayRayTMin;
    float DisplayRayTMax;
    uint DisplayHitCount;
    uint DisplayMissCount;
};

StructuredBuffer<RayRecord> DisplayRecords : register(t0);

static const float3 InstancePalette[4] = {
    float3(0.95f, 0.35f, 0.20f),
    float3(0.20f, 0.55f, 0.95f),
    float3(0.35f, 0.90f, 0.45f),
    float3(0.85f, 0.85f, 0.25f),
};

static const float3 RecordPalette[4] = {
    float3(0.55f, 0.15f, 0.85f),
    float3(0.10f, 0.80f, 0.80f),
    float3(0.95f, 0.75f, 0.10f),
    float3(0.90f, 0.20f, 0.55f),
};

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

// The stage ledger view: one column per stage, coloured by whether the frame submitted it, deliberately skipped
// it, or has no such object at all. It is the view a learner opens when the picture is black.
float3 LabStageLedgerColor(uint2 pixel)
{
    uint stage = min((pixel.x * 9u) / max(DisplayWidth, 1u), 8u);
    uint bit = 1u << stage;
    float column = float((pixel.y * 8u) / max(DisplayHeight, 1u)) / 8.0f;
    if ((DisplayStageUnavailableMask & bit) != 0u)
    {
        return float3(0.16f, 0.16f, 0.20f) * (0.6f + column);
    }
    if ((DisplayStageMask & bit) != 0u)
    {
        return float3(0.12f, 0.72f, 0.32f) * (0.55f + column);
    }
    return float3(0.85f, 0.55f, 0.10f) * (0.55f + column);
}

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
    uint2 surfaceSize = uint2(max(DisplaySurfaceWidth, 1u), max(DisplaySurfaceHeight, 1u));
    uint2 gridSize = uint2(max(DisplayWidth, 1u), max(DisplayHeight, 1u));
    uint2 surfacePixel = min(uint2(position.xy), surfaceSize - uint2(1u, 1u));
    uint2 pixel = min(surfacePixel * gridSize / surfaceSize, gridSize - uint2(1u, 1u));
    if (DisplayView == ViewStageLedger)
    {
        return float4(LabStageLedgerColor(pixel), 1.0f);
    }

    RayRecord record = DisplayRecords[pixel.y * DisplayWidth + pixel.x];
    bool hit = (record.status & StatusHit) != 0u;
    if (DisplayView == ViewFinal)
    {
        return float4(record.colorR, record.colorG, record.colorB, 1.0f);
    }
    if (DisplayView == ViewHitMiss)
    {
        return float4(hit ? float3(1.0f, 1.0f, 1.0f) : float3(0.05f, 0.05f, 0.08f), 1.0f);
    }
    if (!hit)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    if (DisplayView == ViewInstanceIdentity)
    {
        return float4(InstancePalette[record.instanceIndex & 3u], 1.0f);
    }
    if (DisplayView == ViewPrimitiveIdentity)
    {
        return float4(record.primitiveIndex == 0u ? float3(1.0f, 0.55f, 0.10f) : float3(0.10f, 0.55f, 1.0f), 1.0f);
    }
    if (DisplayView == ViewBarycentrics)
    {
        float b0 = saturate(1.0f - record.barycentricB1 - record.barycentricB2);
        return float4(record.barycentricB1, record.barycentricB2, b0, 1.0f);
    }
    if (DisplayView == ViewRayDistance)
    {
        float normalized =
            saturate((record.tHit - DisplayNearMetres) / max(DisplayFarMetres - DisplayNearMetres, 1e-6f));
        return float4(1.0f - normalized, 0.25f + 0.5f * normalized, normalized, 1.0f);
    }
    if (DisplayView == ViewFaceOrientation)
    {
        bool front = (record.status & StatusFrontFace) != 0u;
        return float4(front ? float3(0.15f, 0.85f, 0.35f) : float3(0.90f, 0.20f, 0.15f), 1.0f);
    }
    return float4(RecordPalette[record.hitGroupRecordIndex & 3u], 1.0f);
}

#endif // LGP_CH33_DISPLAY

#endif // LGP_CH33_DXR_SHARED_HLSLI
