// Chapter 33 Solution: the DXR 1.0 raytracing pipeline itself.
//
// This library is compiled at runtime as `lib_6_3` and linked into a raytracing state object with one triangle hit
// group, two miss shaders, a global root signature and a hit-group-local root signature. Everything the Starter
// had to do by hand happens here in fixed-function traversal:
//
//   * The instance inclusion mask, the strict `TMin < t < TMax` interval, the nearest-hit search and the
//     object-space facing test are all arguments to a single `TraceRay`, not a loop.
//   * The identity the closest-hit shader reports - `InstanceIndex`, `InstanceID`, `PrimitiveIndex`, `HitKind`,
//     `RayTCurrent` and the barycentric attributes - comes from the traversal, not from the shader's own search.
//   * The material comes from the *record the traversal selected*. `HitGroupRecordIndex` and the colour below are
//     local root arguments living inside a shader-table record; the shader cannot compute which record it is in,
//     so a record that reports its own index is direct evidence of the shader-table indexing arithmetic.
//
// Recursion depth is one and there is exactly one `TraceRay` in the library, so the pipeline config the state
// object declares is the pipeline the shaders actually need.

#include "../Common/DxrShared.hlsli"

#ifdef LGP_CH33_RAYTRACING

// The payload, declared as scalars so that its 56 bytes are unambiguous and match the `MaxPayloadSizeInBytes` the
// state object declares. It carries facts, not shading state: what was hit, which record answered, how far away it
// was, and which stages ran.
struct LabPayload
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
    float colorR;
    float colorG;
    float colorB;
};

// The hit group's local root signature. These five 32-bit values are stored inside each hit-group shader-table
// record, immediately after its 32-byte shader identifier. Two records that name the same hit group and carry
// different values here are two different materials, and that is the whole mechanism.
cbuffer HitGroupConstants : register(b1)
{
    uint HitGroupMaterialId;
    uint HitGroupRecordIndex;
    float HitGroupColorR;
    float HitGroupColorG;
    float HitGroupColorB;
};

[shader("raygeneration")] void RayGenerationMain()
{
    uint2 pixel = DispatchRaysIndex().xy;

    RayDesc ray;
    ray.Origin = LabRayOrigin(pixel);
    ray.Direction = LabRayDirection();
    ray.TMin = RayTMin;
    ray.TMax = RayTMax;

    LabPayload payload;
    payload.status = StatusTraversalRan | StatusFixedFunctionTraversal;
    payload.instanceIndex = InvalidIndex;
    payload.instanceId = InvalidIndex;
    payload.primitiveIndex = InvalidIndex;
    payload.hitKind = 0u;
    payload.hitGroupRecordIndex = InvalidIndex;
    payload.materialId = InvalidIndex;
    payload.missShaderIndex = InvalidIndex;
    payload.tHit = 0.0f;
    payload.barycentricB1 = 0.0f;
    payload.barycentricB2 = 0.0f;
    payload.colorR = 0.0f;
    payload.colorG = 0.0f;
    payload.colorB = 0.0f;

    // The three record-selection terms are passed exactly as the configuration supplies them, so the record the
    // traversal picks is the record the chapter's own arithmetic predicts.
    TraceRay(Scene, ConfiguredRayFlags, InstanceInclusionMask, RayContributionToHitGroupIndex,
             MultiplierForGeometryContribution, MissShaderIndex, ray, payload);

    RayRecord record = LabMakeMissRecord();
    record.status = payload.status;
    record.instanceIndex = payload.instanceIndex;
    record.instanceId = payload.instanceId;
    record.primitiveIndex = payload.primitiveIndex;
    record.hitKind = payload.hitKind;
    record.hitGroupRecordIndex = payload.hitGroupRecordIndex;
    record.materialId = payload.materialId;
    record.missShaderIndex = payload.missShaderIndex;
    record.tHit = payload.tHit;
    record.barycentricB1 = payload.barycentricB1;
    record.barycentricB2 = payload.barycentricB2;
    record.colorR = payload.colorR;
    record.colorG = payload.colorG;
    record.colorB = payload.colorB;
    if ((payload.status & StatusHit) != 0u)
    {
        float3 position = ray.Origin + payload.tHit * ray.Direction;
        record.worldPositionX = position.x;
        record.worldPositionY = position.y;
        record.worldPositionZ = position.z;
    }

    LabStoreRecord(pixel, record);
}

[shader("closesthit")] void ClosestHitMain(inout LabPayload payload,
                                           in BuiltInTriangleIntersectionAttributes attributes)
{
    bool frontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
    payload.status |= StatusHit | StatusClosestHitRan | StatusLocalRootArgumentsRead |
                      (frontFace ? StatusFrontFace : StatusBackFace);
    payload.instanceIndex = InstanceIndex();
    payload.instanceId = InstanceID();
    payload.primitiveIndex = PrimitiveIndex();
    payload.hitKind = HitKind();
    payload.hitGroupRecordIndex = HitGroupRecordIndex;
    payload.materialId = HitGroupMaterialId;
    payload.missShaderIndex = InvalidIndex;
    payload.tHit = RayTCurrent();
    payload.barycentricB1 = attributes.barycentrics.x;
    payload.barycentricB2 = attributes.barycentrics.y;

    float3 shaded = LabShadeHit(float3(HitGroupColorR, HitGroupColorG, HitGroupColorB), attributes.barycentrics.x,
                                attributes.barycentrics.y, frontFace);
    payload.colorR = shaded.r;
    payload.colorG = shaded.g;
    payload.colorB = shaded.b;
}

// Miss record 0. It reports the index of the record it occupies, which is how a readback proves that
// `MissShaderIndex` selected a record rather than that "some miss shader ran".
[shader("miss")] void MissBackground(inout LabPayload payload)
{
    payload.status |= StatusMiss | StatusMissShaderRan;
    payload.missShaderIndex = 0u;
    float3 background = LabMissColor(0u, DispatchRaysIndex().xy);
    payload.colorR = background.r;
    payload.colorG = background.g;
    payload.colorB = background.b;
}

// Miss record 1. Deliberately black, so that switching miss records is visible in the picture and not only in the
// records buffer.
[shader("miss")] void MissVoid(inout LabPayload payload)
{
    payload.status |= StatusMiss | StatusMissShaderRan;
    payload.missShaderIndex = 1u;
    float3 background = LabMissColor(1u, DispatchRaysIndex().xy);
    payload.colorR = background.r;
    payload.colorG = background.g;
    payload.colorB = background.b;
}

#endif // LGP_CH33_RAYTRACING
