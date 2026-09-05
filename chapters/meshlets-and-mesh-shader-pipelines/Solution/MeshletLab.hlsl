#include "MeshletLab.hlsli"

// The amplification/mesh path consumes the same meshlet tables the CPU validated:
// global positions, per-meshlet descriptors, the vertex-remap table, and packed
// local primitives. It reproduces the classic image at meshlet granularity.

#define CH22_MAX_MESHLETS 64
#define CH22_MAX_MESHLET_VERTICES 64
#define CH22_MAX_MESHLET_PRIMITIVES 64
#define CH22_AS_GROUP_SIZE 64
#define CH22_MS_GROUP_SIZE 64

struct MeshletDescriptor
{
    uint vertexOffset;
    uint vertexCount;
    uint primitiveOffset;
    uint primitiveCount;
};

StructuredBuffer<float4> gPositions : register(t1);
StructuredBuffer<MeshletDescriptor> gMeshlets : register(t2);
StructuredBuffer<uint> gMeshletVertices : register(t3);
StructuredBuffer<uint> gMeshletPrimitives : register(t4);
RWByteAddressBuffer gStats : register(u0);

cbuffer MeshRootConstants : register(b0)
{
    uint gMeshletCount;
};

struct MeshletPayload
{
    uint meshletIndices[CH22_MAX_MESHLETS];
};

groupshared MeshletPayload s_payload;
groupshared uint s_visibleCount;

// Conservative clip-space bounds test: keep any meshlet whose vertex extent could
// touch the view volume. It never rejects a meshlet that might be visible.
bool MeshletPotentiallyVisible(MeshletDescriptor meshlet)
{
    float3 minimum = float3(1e30, 1e30, 1e30);
    float3 maximum = float3(-1e30, -1e30, -1e30);
    uint vertexCount = min(meshlet.vertexCount, CH22_MAX_MESHLET_VERTICES);
    for (uint i = 0; i < vertexCount; ++i)
    {
        uint global = gMeshletVertices[meshlet.vertexOffset + i];
        float3 position = gPositions[global].xyz;
        minimum = min(minimum, position);
        maximum = max(maximum, position);
    }
    bool outside = maximum.x < -1.0 || minimum.x > 1.0 || maximum.y < -1.0 || minimum.y > 1.0 ||
                   maximum.z < 0.0 || minimum.z > 1.0;
    return !outside;
}

[numthreads(CH22_AS_GROUP_SIZE, 1, 1)]
void MeshletAS(uint threadId : SV_DispatchThreadID)
{
    if (threadId == 0)
    {
        s_visibleCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    bool visible = false;
    if (threadId < gMeshletCount)
    {
        visible = MeshletPotentiallyVisible(gMeshlets[threadId]);
    }
    if (visible)
    {
        uint slot;
        InterlockedAdd(s_visibleCount, 1u, slot);
        if (slot < CH22_MAX_MESHLETS)
        {
            s_payload.meshletIndices[slot] = threadId;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (threadId == 0)
    {
        gStats.InterlockedAdd(0, s_visibleCount);
    }
    DispatchMesh(s_visibleCount, 1, 1, s_payload);
}

[outputtopology("triangle")]
[numthreads(CH22_MS_GROUP_SIZE, 1, 1)]
void MeshletMS(uint groupId : SV_GroupID, uint threadId : SV_GroupThreadID, in payload MeshletPayload payload,
               out vertices SceneVertexOutput outVertices[CH22_MAX_MESHLET_VERTICES],
               out indices uint3 outTriangles[CH22_MAX_MESHLET_PRIMITIVES])
{
    uint meshletIndex = payload.meshletIndices[min(groupId, CH22_MAX_MESHLETS - 1u)];
    MeshletDescriptor meshlet = gMeshlets[meshletIndex];
    uint vertexCount = min(meshlet.vertexCount, CH22_MAX_MESHLET_VERTICES);
    uint primitiveCount = min(meshlet.primitiveCount, CH22_MAX_MESHLET_PRIMITIVES);
    SetMeshOutputCounts(vertexCount, primitiveCount);

    if (threadId < vertexCount)
    {
        uint global = gMeshletVertices[meshlet.vertexOffset + threadId];
        SceneVertexOutput output;
        output.position = float4(gPositions[global].xyz, 1.0);
        output.meshletId = meshletIndex;
        outVertices[threadId] = output;
    }
    if (threadId < primitiveCount)
    {
        uint packed = gMeshletPrimitives[meshlet.primitiveOffset + threadId];
        uint safeVertexCount = max(vertexCount, 1u);
        uint a = min(packed & 0x3ffu, safeVertexCount - 1u);
        uint b = min((packed >> 10u) & 0x3ffu, safeVertexCount - 1u);
        uint c = min((packed >> 20u) & 0x3ffu, safeVertexCount - 1u);
        outTriangles[threadId] = uint3(a, b, c);
    }
    if (threadId == 0)
    {
        gStats.InterlockedAdd(4, vertexCount);
        gStats.InterlockedAdd(8, primitiveCount);
    }
}
