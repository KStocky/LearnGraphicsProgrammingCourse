struct CandidateData
{
    float2 center;
    float2 halfExtent;
    float4 color;
    uint visible;
    uint3 padding;
};

StructuredBuffer<CandidateData> gCandidates : register(t0);
StructuredBuffer<uint> gFlags : register(t1);
RWStructuredBuffer<uint> gFlagsUav : register(u0);
RWStructuredBuffer<uint> gIndices : register(u1);
RWByteAddressBuffer gCommands : register(u2);
RWStructuredBuffer<uint> gCount : register(u3);
RWStructuredBuffer<uint> gStats : register(u4);

cbuffer DrawConstants : register(b0)
{
    uint gCandidateIndex;
};

cbuffer DispatchConstants : register(b0)
{
    uint gCapacity;
    uint gCandidateCount;
    uint gVertexCountPerQuad;
    uint gReserved;
};

static const uint kThreadCount = 64;
groupshared uint gScan[kThreadCount];

float2 QuadCorner(uint vertexId)
{
    switch (vertexId)
    {
    case 0:
        return float2(-1.0, 1.0);
    case 1:
        return float2(1.0, 1.0);
    case 2:
        return float2(-1.0, -1.0);
    case 3:
        return float2(1.0, 1.0);
    case 4:
        return float2(1.0, -1.0);
    default:
        return float2(-1.0, -1.0);
    }
}

struct VsOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VsOutput IndirectVS(uint vertexId : SV_VertexID)
{
    CandidateData candidate = gCandidates[gCandidateIndex];
    float2 corner = QuadCorner(vertexId);

    VsOutput output;
    output.position = float4(candidate.center + (corner * candidate.halfExtent), 0.0, 1.0);
    output.color = candidate.color;
    return output;
}

float4 ColorPS(VsOutput input) : SV_Target0
{
    return input.color;
}

[numthreads(kThreadCount, 1, 1)]
void ResetCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index < gCandidateCount)
    {
        gFlagsUav[index] = 0;
    }

    if (index < gCapacity)
    {
        gIndices[index] = 0xffffffffu;
        uint commandOffset = index * 20u;
        gCommands.Store(commandOffset + 0u, 0u);
        gCommands.Store(commandOffset + 4u, 0u);
        gCommands.Store(commandOffset + 8u, 0u);
        gCommands.Store(commandOffset + 12u, 0u);
        gCommands.Store(commandOffset + 16u, 0u);
    }

    if (index < 4u)
    {
        gStats[index] = 0u;
    }

    if (index == 0u)
    {
        gCount[0] = 0u;
    }
}

[numthreads(kThreadCount, 1, 1)]
void ClassifyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= gCandidateCount)
    {
        return;
    }

    gFlagsUav[index] = gCandidates[index].visible != 0u ? 1u : 0u;
}

[numthreads(kThreadCount, 1, 1)]
void StableCompactCS(uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    uint index = dispatchThreadId.x;
    uint flag = index < gCandidateCount ? gFlags[index] : 0u;
    gScan[groupIndex] = flag;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint offset = 1u; offset < kThreadCount; offset <<= 1u)
    {
        uint addend = groupIndex >= offset ? gScan[groupIndex - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gScan[groupIndex] += addend;
        GroupMemoryBarrierWithGroupSync();
    }

    uint visibleCount = gScan[gCandidateCount - 1u];
    uint exclusiveOffset = gScan[groupIndex] - flag;
    if (index < gCandidateCount && flag != 0u && exclusiveOffset < gCapacity)
    {
        gIndices[exclusiveOffset] = index;
        uint commandOffset = exclusiveOffset * 20u;
        gCommands.Store(commandOffset + 0u, index);
        gCommands.Store(commandOffset + 4u, gVertexCountPerQuad);
        gCommands.Store(commandOffset + 8u, 1u);
        gCommands.Store(commandOffset + 12u, 0u);
        gCommands.Store(commandOffset + 16u, 0u);
    }

    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0u)
    {
        uint emittedCount = min(visibleCount, gCapacity);
        gCount[0] = visibleCount;
        gStats[0] = gCandidateCount;
        gStats[1] = visibleCount;
        gStats[2] = emittedCount;
        gStats[3] = visibleCount - emittedCount;
    }
}

[numthreads(kThreadCount, 1, 1)]
void AtomicAppendCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint reverseIndex = (gCandidateCount - 1u) - dispatchThreadId.x;
    if (reverseIndex < gCandidateCount && gFlags[reverseIndex] != 0u)
    {
        uint appendIndex;
        InterlockedAdd(gCount[0], 1u, appendIndex);
        if (appendIndex < gCapacity)
        {
            gIndices[appendIndex] = reverseIndex;
            uint commandOffset = appendIndex * 20u;
            gCommands.Store(commandOffset + 0u, reverseIndex);
            gCommands.Store(commandOffset + 4u, gVertexCountPerQuad);
            gCommands.Store(commandOffset + 8u, 1u);
            gCommands.Store(commandOffset + 12u, 0u);
            gCommands.Store(commandOffset + 16u, 0u);
        }
    }

    GroupMemoryBarrierWithGroupSync();
    if (dispatchThreadId.x == 0u)
    {
        uint visibleCount = gCount[0];
        uint emittedCount = min(visibleCount, gCapacity);
        gStats[0] = gCandidateCount;
        gStats[1] = visibleCount;
        gStats[2] = emittedCount;
        gStats[3] = visibleCount - emittedCount;
    }
}
