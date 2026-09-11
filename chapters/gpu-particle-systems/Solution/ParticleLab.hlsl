// Chapter 34 Solution GPU particle lab.
//
// The whole particle frame runs on the GPU: emission allocates free slots from a scanned dead list, issues
// monotone identities and seeds each particle from (seed, identity); a fixed-step simulation integrates, resolves
// the ground plane, ages, and retires; a stable exclusive-scan-and-scatter compaction produces the live index
// list; a single-thread pass writes the D3D12_DRAW_ARGUMENTS the command signature reads; and a checksum pass
// hashes the float32 state image exactly as the CPU contract does, so a readback can be compared bit-for-bit in a
// dyadic scenario.
//
// One teaching simplification is pinned here and mirrored by the host: the lab caps capacity at the thread-group
// size so emission and compaction are single-thread-group passes whose scans live in group-shared memory. A
// shipping system scans across groups; a lab that fits in one group keeps every ordering decision visible.

#define LGP_PARTICLE_THREAD_GROUP_SIZE 256

struct GpuParticle
{
    uint identity;
    uint alive;
    float px;
    float py;
    float pz;
    float vx;
    float vy;
    float vz;
    float age;
    float lifetime;
};

RWStructuredBuffer<GpuParticle> gStateA : register(u0);
RWStructuredBuffer<GpuParticle> gStateB : register(u1);
RWStructuredBuffer<uint> gCounters : register(u2);
RWStructuredBuffer<uint> gCompactedSlots : register(u3);
RWStructuredBuffer<uint> gCompactedIdentities : register(u4);
RWByteAddressBuffer gIndirectArgs : register(u5);
RWStructuredBuffer<uint> gIndirectCount : register(u6);
RWStructuredBuffer<uint2> gChecksum : register(u7);

StructuredBuffer<GpuParticle> gDrawStateA : register(t0);
StructuredBuffer<GpuParticle> gDrawStateB : register(t1);
StructuredBuffer<uint> gDrawSlots : register(t2);

cbuffer LabConstants : register(b0)
{
    uint gCapacity;
    uint gReadSlot;
    uint gWriteSlot;
    uint gRequested;
    uint gIntegrator;
    uint gGroundEnabled;
    uint gVertexCountPerParticle;
    uint gDrawCapacity;
    uint gOutputCapacity;
    uint gSeedLo;
    uint gSeedHi;
    uint gDeadSlotPolicy;
    float gTimestep;
    float gGravityX;
    float gGravityY;
    float gGravityZ;
    float gLinearDrag;
    float gOriginX;
    float gOriginY;
    float gOriginZ;
    float gPosJitter;
    float gBaseVelX;
    float gBaseVelY;
    float gBaseVelZ;
    float gVelJitter;
    float gGroundHeight;
    float gRestitution;
    float gFriction;
    float gRestingSpeed;
    float gLifetime;
    float gViewScale;
    float gQuadHalfExtent;
};

cbuffer DrawConstants : register(b1)
{
    uint gDrawStateSlot;
    float gDrawViewScale;
    float gDrawQuadHalfExtent;
    uint gDrawPad;
};

// Counter buffer indices. The dead list is a scanned free list rather than an append counter, so the only counter
// that must persist across substeps is the monotone identity source and the lifetime accounting.
static const uint kCounterNextIdentity = 0u;
static const uint kCounterRequested = 1u;
static const uint kCounterSpawned = 2u;
static const uint kCounterDropped = 3u;
static const uint kCounterDied = 4u;
static const uint kCounterContact = 5u;
static const uint kCounterLive = 6u;
static const uint kCounterEmitted = 7u;

static const uint kInvalidId = 0xFFFFFFFFu;

groupshared uint gScan[LGP_PARTICLE_THREAD_GROUP_SIZE];

uint Finalize32(uint value)
{
    value ^= value >> 16u;
    value *= 0x7FEB352Du;
    value ^= value >> 15u;
    value *= 0x846CA68Bu;
    value ^= value >> 16u;
    return value;
}

uint MixSeed(uint seedLo, uint seedHi, uint identity, uint stream)
{
    uint state = Finalize32(seedLo ^ 0x9E3779B9u);
    state = Finalize32(state ^ seedHi);
    state = Finalize32(state ^ identity);
    state = Finalize32(state ^ stream);
    return state;
}

float SignedUnitFromBits(uint bits)
{
    float unit = float(bits >> 8u) * (1.0 / 16777216.0);
    return (unit * 2.0) - 1.0;
}

GpuParticle DeadParticle()
{
    GpuParticle p;
    p.identity = kInvalidId;
    p.alive = 0u;
    p.px = 0.0;
    p.py = 0.0;
    p.pz = 0.0;
    p.vx = 0.0;
    p.vy = 0.0;
    p.vz = 0.0;
    p.age = 0.0;
    p.lifetime = 0.0;
    return p;
}

GpuParticle LoadState(uint slot, uint index)
{
    if (slot == 0u)
    {
        return gStateA[index];
    }
    return gStateB[index];
}

void StoreState(uint slot, uint index, GpuParticle value)
{
    if (slot == 0u)
    {
        gStateA[index] = value;
    }
    else
    {
        gStateB[index] = value;
    }
}

// Inclusive Hillis-Steele scan of gScan over the whole group. Returns the total and leaves gScan holding the
// inclusive prefix so a caller recovers the exclusive prefix by subtracting its own input.
uint InclusiveScanGroup(uint groupIndex)
{
    [unroll]
    for (uint offset = 1u; offset < LGP_PARTICLE_THREAD_GROUP_SIZE; offset <<= 1u)
    {
        uint addend = (groupIndex >= offset) ? gScan[groupIndex - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gScan[groupIndex] += addend;
        GroupMemoryBarrierWithGroupSync();
    }
    return gScan[LGP_PARTICLE_THREAD_GROUP_SIZE - 1u];
}

[numthreads(LGP_PARTICLE_THREAD_GROUP_SIZE, 1, 1)]
void InitCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index < gCapacity)
    {
        gStateA[index] = DeadParticle();
        gStateB[index] = DeadParticle();
    }
    if (index < 8u)
    {
        gCounters[index] = 0u;
    }
    if (index < gCapacity)
    {
        gCompactedSlots[index] = kInvalidId;
        gCompactedIdentities[index] = kInvalidId;
    }
    // The sentinel sits at the configured output boundary. Without the scatter clamp, the first excess live
    // particle overwrites it, including when the boundary equals the maximum 256-particle capacity.
    if (index == 0u)
    {
        gCompactedSlots[gOutputCapacity] = kInvalidId;
        gCompactedIdentities[gOutputCapacity] = kInvalidId;
    }
}

[numthreads(LGP_PARTICLE_THREAD_GROUP_SIZE, 1, 1)]
void EmitCS(uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    uint index = dispatchThreadId.x;
    uint dead = 0u;
    if (index < gCapacity)
    {
        dead = (LoadState(gReadSlot, index).alive == 0u) ? 1u : 0u;
    }
    gScan[groupIndex] = dead;
    GroupMemoryBarrierWithGroupSync();

    uint totalDead = InclusiveScanGroup(groupIndex);
    uint exclusiveDead = gScan[groupIndex] - dead;

    uint spawnCount = min(gRequested, totalDead);
    uint nextIdentity = gCounters[kCounterNextIdentity];

    uint spawnIndex = (gDeadSlotPolicy == 0u) ? exclusiveDead : (totalDead - 1u - exclusiveDead);
    if (index < gCapacity && dead == 1u && spawnIndex < spawnCount)
    {
        uint identity = nextIdentity + spawnIndex;

        GpuParticle p;
        p.identity = identity;
        p.alive = 1u;
        p.px = gOriginX + (gPosJitter * SignedUnitFromBits(MixSeed(gSeedLo, gSeedHi, identity, 0u)));
        p.py = gOriginY + (gPosJitter * SignedUnitFromBits(MixSeed(gSeedLo, gSeedHi, identity, 1u)));
        p.pz = gOriginZ + (gPosJitter * SignedUnitFromBits(MixSeed(gSeedLo, gSeedHi, identity, 2u)));
        p.vx = gBaseVelX + (gVelJitter * SignedUnitFromBits(MixSeed(gSeedLo, gSeedHi, identity, 3u)));
        p.vy = gBaseVelY + (gVelJitter * SignedUnitFromBits(MixSeed(gSeedLo, gSeedHi, identity, 4u)));
        p.vz = gBaseVelZ + (gVelJitter * SignedUnitFromBits(MixSeed(gSeedLo, gSeedHi, identity, 5u)));
        p.age = 0.0;
        p.lifetime = gLifetime;
        StoreState(gReadSlot, index, p);
    }

    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0u)
    {
        uint dropped = gRequested - spawnCount;
        gCounters[kCounterNextIdentity] = nextIdentity + spawnCount;
        gCounters[kCounterRequested] += gRequested;
        gCounters[kCounterSpawned] += spawnCount;
        gCounters[kCounterDropped] += dropped;
    }
}

[numthreads(LGP_PARTICLE_THREAD_GROUP_SIZE, 1, 1)]
void SimulateCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= gCapacity)
    {
        return;
    }

    GpuParticle p = LoadState(gReadSlot, index);
    if (p.alive == 0u)
    {
        StoreState(gWriteSlot, index, DeadParticle());
        return;
    }

    float3 position = float3(p.px, p.py, p.pz);
    float3 velocity = float3(p.vx, p.vy, p.vz);
    float3 gravity = float3(gGravityX, gGravityY, gGravityZ);
    float h = gTimestep;
    float3 acceleration = gravity - (gLinearDrag * velocity);

    float3 newPosition = position;
    float3 newVelocity = velocity;
    if (gIntegrator == 0u)
    {
        newPosition = position + (h * velocity);
        newVelocity = velocity + (h * acceleration);
    }
    else if (gIntegrator == 1u)
    {
        newVelocity = velocity + (h * acceleration);
        newPosition = position + (h * newVelocity);
    }
    else
    {
        newPosition = position + (h * velocity) + ((0.5 * h * h) * acceleration);
        newVelocity = velocity + (h * acceleration);
    }

    bool contacted = false;
    if (gGroundEnabled != 0u)
    {
        float penetration = gGroundHeight - newPosition.y;
        if (penetration > 0.0)
        {
            contacted = true;
            newPosition.y = gGroundHeight;
            float normalSpeed = newVelocity.y;
            if (normalSpeed < 0.0)
            {
                normalSpeed = -gRestitution * normalSpeed;
                float tangentialScale = 1.0 - gFriction;
                newVelocity.x = newVelocity.x * tangentialScale;
                newVelocity.z = newVelocity.z * tangentialScale;
            }
            if (normalSpeed <= gRestingSpeed)
            {
                normalSpeed = 0.0;
            }
            newVelocity.y = normalSpeed;
        }
    }

    p.px = newPosition.x;
    p.py = newPosition.y;
    p.pz = newPosition.z;
    p.vx = newVelocity.x;
    p.vy = newVelocity.y;
    p.vz = newVelocity.z;
    p.age = p.age + h;

    if (contacted)
    {
        uint previousContact;
        InterlockedAdd(gCounters[kCounterContact], 1u, previousContact);
    }

    if (p.age >= p.lifetime)
    {
        StoreState(gWriteSlot, index, DeadParticle());
        uint previousDied;
        InterlockedAdd(gCounters[kCounterDied], 1u, previousDied);
    }
    else
    {
        StoreState(gWriteSlot, index, p);
    }
}

[numthreads(LGP_PARTICLE_THREAD_GROUP_SIZE, 1, 1)]
void CompactCS(uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    uint index = dispatchThreadId.x;
    uint alive = 0u;
    if (index < gCapacity)
    {
        alive = (LoadState(gReadSlot, index).alive != 0u) ? 1u : 0u;
    }
    gScan[groupIndex] = alive;
    GroupMemoryBarrierWithGroupSync();

    uint totalAlive = InclusiveScanGroup(groupIndex);
    uint exclusiveAlive = gScan[groupIndex] - alive;

    if (index < gCapacity && alive == 1u && exclusiveAlive < gOutputCapacity)
    {
        GpuParticle p = LoadState(gReadSlot, index);
        gCompactedSlots[exclusiveAlive] = index;
        gCompactedIdentities[exclusiveAlive] = p.identity;
    }

    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0u)
    {
        gCounters[kCounterLive] = totalAlive;
        gCounters[kCounterEmitted] = min(totalAlive, gOutputCapacity);
    }
}

[numthreads(1, 1, 1)]
void IndirectArgsCS()
{
    uint instanceCount = min(gCounters[kCounterEmitted], gDrawCapacity);

    gIndirectArgs.Store(0u, gVertexCountPerParticle);
    gIndirectArgs.Store(4u, instanceCount);
    gIndirectArgs.Store(8u, 0u);
    gIndirectArgs.Store(12u, 0u);

    gIndirectCount[0] = (instanceCount > 0u) ? 1u : 0u;
}

uint2 Mul32(uint a, uint b)
{
    uint aLow = a & 0xFFFFu;
    uint aHigh = a >> 16u;
    uint bLow = b & 0xFFFFu;
    uint bHigh = b >> 16u;

    uint lowLow = aLow * bLow;
    uint lowHigh = aLow * bHigh;
    uint highLow = aHigh * bLow;
    uint highHigh = aHigh * bHigh;

    uint cross = (lowLow >> 16u) + (lowHigh & 0xFFFFu) + (highLow & 0xFFFFu);
    uint low = (lowLow & 0xFFFFu) | (cross << 16u);
    uint high = highHigh + (lowHigh >> 16u) + (highLow >> 16u) + (cross >> 16u);
    return uint2(low, high);
}

uint2 Mul64(uint2 a, uint2 b)
{
    uint2 low = Mul32(a.x, b.x);
    uint high = low.y + (a.x * b.y) + (a.y * b.x);
    return uint2(low.x, high);
}

uint2 HashWord(uint2 hash, uint value)
{
    const uint2 prime = uint2(0x000001B3u, 0x00000100u);
    [unroll]
    for (uint shift = 0u; shift < 32u; shift += 8u)
    {
        hash.x ^= (value >> shift) & 0xFFu;
        hash = Mul64(hash, prime);
    }
    return hash;
}

[numthreads(1, 1, 1)]
void ChecksumCS()
{
    uint2 hash = uint2(0x84222325u, 0xCBF29CE4u);
    for (uint index = 0u; index < gCapacity; ++index)
    {
        GpuParticle p = LoadState(gReadSlot, index);
        hash = HashWord(hash, p.identity);
        hash = HashWord(hash, (p.alive != 0u) ? 1u : 0u);
        if (p.alive == 0u)
        {
            continue;
        }
        hash = HashWord(hash, asuint(p.px));
        hash = HashWord(hash, asuint(p.py));
        hash = HashWord(hash, asuint(p.pz));
        hash = HashWord(hash, asuint(p.vx));
        hash = HashWord(hash, asuint(p.vy));
        hash = HashWord(hash, asuint(p.vz));
        hash = HashWord(hash, asuint(p.age));
        hash = HashWord(hash, asuint(p.lifetime));
    }
    gChecksum[0] = hash;
}

float2 QuadCorner(uint vertexId)
{
    switch (vertexId)
    {
    case 0u:
        return float2(-1.0, 1.0);
    case 1u:
        return float2(1.0, 1.0);
    case 2u:
        return float2(-1.0, -1.0);
    case 3u:
        return float2(1.0, 1.0);
    case 4u:
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

VsOutput ParticleVS(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    uint slot = gDrawSlots[instanceId];
    GpuParticle p;
    if (gDrawStateSlot == 0u)
    {
        p = gDrawStateA[slot];
    }
    else
    {
        p = gDrawStateB[slot];
    }

    float2 center = float2(p.px, p.py) * gDrawViewScale;
    float2 corner = QuadCorner(vertexId) * gDrawQuadHalfExtent;

    uint tint = Finalize32(p.identity);
    float r = 0.35 + (0.6 * (float((tint >> 0u) & 0xFFu) / 255.0));
    float g = 0.35 + (0.6 * (float((tint >> 8u) & 0xFFu) / 255.0));
    float b = 0.35 + (0.6 * (float((tint >> 16u) & 0xFFu) / 255.0));

    VsOutput output;
    output.position = float4(center + corner, 0.0, 1.0);
    output.color = float4(r, g, b, 1.0);
    return output;
}

float4 ParticlePS(VsOutput input) : SV_Target0
{
    return input.color;
}
