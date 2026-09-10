// Chapter 33 Starter: the same question, answered without a raytracing pipeline.
//
// This shader is the honest baseline the chapter starts from. It generates the same primary ray as the Solution,
// walks the instance array itself, transforms the ray into each instance's object space, intersects the two
// triangles with Moller-Trumbore, applies the same instance inclusion mask, the same strict `TMin < t < TMax`
// interval and the same object-space facing rule, computes the same hit-group record index with the same three
// terms, and writes the same per-pixel record. It is everything a learner can already do with a compute shader.
//
// What it cannot do, and does not pretend to:
//
//   * It builds no acceleration structure. The traversal below is a linear scan over three instances and two
//     triangles, which is exactly why it is a baseline and not a renderer: its cost is the scene's size.
//   * It has no shader table. It reads a material out of a structured buffer with an index it computed itself,
//     and it never sets `StatusLocalRootArgumentsRead`, because nothing selected a record for it.
//   * It runs no closest-hit or miss shader, so it never sets `StatusClosestHitRan` or `StatusMissShaderRan`.
//     Those bits are the Solution's to set, and a test reads them to prove that neither variant is quietly
//     running the other's mechanism.
//
// Everything it does publish - the identity, the distance, the barycentrics, the facing and the record index - is
// the same ABI the Solution publishes, so the two can be compared record for record.

#include "../Common/DxrShared.hlsli"

#ifdef LGP_CH33_ANALYTIC

// Mirrors the HLSL RAY_FLAG bits the configuration can set. The Starter reads them from the constant buffer
// because the analytic traversal has to apply them itself; the Solution passes the same bits to `TraceRay`.
static const uint LabRayFlagCullBackFacing = 0x10u;
static const uint LabRayFlagCullFrontFacing = 0x20u;

// The magnitude below which the ray is treated as parallel to the triangle's plane. It is a rejection threshold,
// not a fudge factor added to a hit: a candidate this close to parallel has no usable barycentrics.
static const float LabParallelDeterminantEpsilon = 1e-12f;

float3 LabTransformPoint(float4 row0, float4 row1, float4 row2, float3 position)
{
    return float3(dot(row0.xyz, position) + row0.w, dot(row1.xyz, position) + row1.w,
                  dot(row2.xyz, position) + row2.w);
}

float3 LabTransformDirection(float4 row0, float4 row1, float4 row2, float3 direction)
{
    return float3(dot(row0.xyz, direction), dot(row1.xyz, direction), dot(row2.xyz, direction));
}

struct TriangleCandidate
{
    bool accepted;
    float t;
    float b1;
    float b2;
    bool frontFace;
};

// Moller-Trumbore, with the two decisions this chapter cares about spelled out rather than folded into the
// arithmetic: the interval is strict at both ends, and facing is the sign of the determinant, which is the
// object-space winding test D3D12 specifies.
TriangleCandidate LabIntersectTriangle(float3 origin, float3 direction, float3 v0, float3 v1, float3 v2, float tMin,
                                       float tMax)
{
    TriangleCandidate candidate;
    candidate.accepted = false;
    candidate.t = 0.0f;
    candidate.b1 = 0.0f;
    candidate.b2 = 0.0f;
    candidate.frontFace = true;

    float3 edge1 = v1 - v0;
    float3 edge2 = v2 - v0;
    float3 pvec = cross(direction, edge2);
    float determinant = dot(edge1, pvec);
    if (abs(determinant) < LabParallelDeterminantEpsilon)
    {
        return candidate;
    }

    float inverseDeterminant = 1.0f / determinant;
    float3 tvec = origin - v0;
    float b1 = dot(tvec, pvec) * inverseDeterminant;
    if (b1 < 0.0f || b1 > 1.0f)
    {
        return candidate;
    }

    float3 qvec = cross(tvec, edge1);
    float b2 = dot(direction, qvec) * inverseDeterminant;
    if (b2 < 0.0f || b1 + b2 > 1.0f)
    {
        return candidate;
    }

    float t = dot(edge2, qvec) * inverseDeterminant;
    if (!(t > tMin && t < tMax))
    {
        return candidate;
    }

    candidate.accepted = true;
    candidate.t = t;
    candidate.b1 = b1;
    candidate.b2 = b2;
    // The determinant is the negated triple product of the direction with the winding normal, so a positive
    // determinant is a triangle whose vertices appear clockwise from the ray origin: front facing, in D3D12's
    // terms. It is computed on object-space vertices with an object-space ray. The instance transform carries the
    // ray into that space but never rewinds the vertices; this scene's x mirror therefore preserves facing.
    candidate.frontFace = determinant > 0.0f;
    return candidate;
}

[numthreads(8, 8, 1)]
void AnalyticTraceCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= Width || pixel.y >= Height)
    {
        return;
    }

    float3 worldOrigin = LabRayOrigin(pixel);
    float3 worldDirection = LabRayDirection();

    RayRecord record = LabMakeMissRecord();
    record.status = StatusTraversalRan | StatusAnalyticTraversal | StatusMiss;

    bool cullBackFacing = (ConfiguredRayFlags & LabRayFlagCullBackFacing) != 0u;
    bool cullFrontFacing = (ConfiguredRayFlags & LabRayFlagCullFrontFacing) != 0u;

    float nearest = RayTMax;
    bool found = false;
    uint bestInstance = InvalidIndex;
    uint bestPrimitive = InvalidIndex;
    float bestT = 0.0f;
    float bestB1 = 0.0f;
    float bestB2 = 0.0f;
    bool bestFrontFace = true;

    for (uint instanceIndex = 0u; instanceIndex < InstanceCount; ++instanceIndex)
    {
        InstanceRecord instance = Instances[instanceIndex];
        if ((instance.instanceMask & InstanceInclusionMask) == 0u)
        {
            continue;
        }

        float3 objectOrigin = LabTransformPoint(instance.worldToObjectRow0, instance.worldToObjectRow1,
                                                instance.worldToObjectRow2, worldOrigin);
        float3 objectDirection = LabTransformDirection(instance.worldToObjectRow0, instance.worldToObjectRow1,
                                                       instance.worldToObjectRow2, worldDirection);

        for (uint primitiveIndex = 0u; primitiveIndex < 2u; ++primitiveIndex)
        {
            float3 v0 = Vertices[primitiveIndex * 3u + 0u];
            float3 v1 = Vertices[primitiveIndex * 3u + 1u];
            float3 v2 = Vertices[primitiveIndex * 3u + 2u];
            TriangleCandidate candidate =
                LabIntersectTriangle(objectOrigin, objectDirection, v0, v1, v2, RayTMin, nearest);
            if (!candidate.accepted)
            {
                continue;
            }
            if ((candidate.frontFace && cullFrontFacing) || (!candidate.frontFace && cullBackFacing))
            {
                continue;
            }

            found = true;
            nearest = candidate.t;
            bestInstance = instanceIndex;
            bestPrimitive = primitiveIndex;
            bestT = candidate.t;
            bestB1 = candidate.b1;
            bestB2 = candidate.b2;
            bestFrontFace = candidate.frontFace;
        }
    }

    if (found)
    {
        InstanceRecord instance = Instances[bestInstance];
        uint recordIndex = LabHitGroupRecordIndex(0u, instance.instanceContributionToHitGroupIndex);
        uint clampedRecord = min(recordIndex, MaterialCount - 1u);
        MaterialRecord material = Materials[clampedRecord];
        float3 materialColor = float3(material.colorR, material.colorG, material.colorB);

        record.status = StatusTraversalRan | StatusAnalyticTraversal | StatusHit |
                        (bestFrontFace ? StatusFrontFace : StatusBackFace);
        record.instanceIndex = bestInstance;
        record.instanceId = instance.instanceId;
        record.primitiveIndex = bestPrimitive;
        record.hitKind = bestFrontFace ? HitKindFrontFace : HitKindBackFace;
        record.hitGroupRecordIndex = recordIndex;
        record.materialId = material.materialId;
        record.missShaderIndex = InvalidIndex;
        record.tHit = bestT;
        record.barycentricB1 = bestB1;
        record.barycentricB2 = bestB2;
        float3 position = worldOrigin + bestT * worldDirection;
        record.worldPositionX = position.x;
        record.worldPositionY = position.y;
        record.worldPositionZ = position.z;
        float3 shaded = LabShadeHit(materialColor, bestB1, bestB2, bestFrontFace);
        record.colorR = shaded.r;
        record.colorG = shaded.g;
        record.colorB = shaded.b;
    }
    else
    {
        float3 background = LabMissColor(MissShaderIndex, pixel);
        record.colorR = background.r;
        record.colorG = background.g;
        record.colorB = background.b;
    }

    LabStoreRecord(pixel, record);
}

#endif // LGP_CH33_ANALYTIC
