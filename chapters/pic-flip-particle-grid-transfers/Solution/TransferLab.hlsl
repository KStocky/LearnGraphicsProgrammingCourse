#include "Stage.hpp"
cbuffer Constants : register(b0)
{
    uint count, stage, field, view;
    float alpha; float3 increment;
};
RWByteAddressBuffer arena : register(u0);
ByteAddressBuffer display : register(t0);
static const uint P = 0, SNAPSHOT = 2064, FACES = 4128, SAMPLES = 11824;
static const uint METRICS = 13888, STATUS = 13968;
struct Particle { float3 position; float mass; float3 velocity; uint identity; };
uint AtomicLoad(uint address)
{
    uint value;
    arena.InterlockedOr(address, 0, value);
    return value;
}
void Fail(uint error)
{
    uint previous;
    arena.InterlockedCompareExchange(STATUS, 0, error, previous);
}
bool Failed() { return AtomicLoad(STATUS) != 0; }
Particle LoadParticle(uint start, uint i)
{
    uint4 a = arena.Load4(start + i * 32), b = arena.Load4(start + i * 32 + 16);
    Particle p;
    p.position = asfloat(a.xyz); p.mass = asfloat(a.w);
    p.velocity = asfloat(b.xyz); p.identity = b.w;
    return p;
}
float3 FacePosition(uint face)
{
    uint axis = face / 80, key = face % 80;
    uint3 shape = 4; shape[axis] = 5;
    float3 offset = 0.5; offset[axis] = 0;
    uint3 index = uint3(key % shape.x, (key / shape.x) % shape.y, key / (shape.x * shape.y));
    return -1 + 0.5 * (float3(index) + offset);
}
#include "Transfers.hlsli"
[numthreads(1, 1, 1)]
void Preflight()
{
    if (Failed()) return;
    if (count == 0 || count > 64) { Fail(1); return; }
    if (!isfinite(alpha) || !all(isfinite(increment))) { Fail(3); return; }
    if (alpha < 0 || alpha > 1 || any(abs(increment) > 10) || field > 4 || view > 3 || stage != TRANSFER_STAGE)
        Fail(2);
}
[numthreads(64, 1, 1)]
void Snapshot(uint i : SV_DispatchThreadID)
{
    if (i >= min(count, 64) || Failed()) return;
    Particle p = LoadParticle(P, i);
    if (!all(isfinite(p.position)) || !all(isfinite(p.velocity)) || !isfinite(p.mass))
        { Fail(3); return; }
    if (p.mass < 0.0001 || p.mass > 1000) { Fail(4); return; }
    if (any(p.position < -1) || any(p.position >= 1)) { Fail(5); return; }
    if (any(abs(p.velocity) > 100)) { Fail(2); return; }
    if (p.identity != i) { Fail(8); return; }
    arena.Store4(SNAPSHOT + i * 32, arena.Load4(P + i * 32));
    arena.Store4(SNAPSHOT + i * 32 + 16, arena.Load4(P + i * 32 + 16));
}
[numthreads(64, 1, 1)]
void Clear(uint i : SV_DispatchThreadID)
{
    if (Failed()) return;
    if (i < 240)
    {
        arena.Store4(FACES + i * 32, 0);
        arena.Store4(FACES + i * 32 + 16, 0);
    }
    if (i < min(count, 64))
    {
        arena.Store4(SAMPLES + i * 32, 0);
        arena.Store4(SAMPLES + i * 32 + 16, 0);
    }
    if (i == 0) arena.Store(STATUS + 12, 0);
}
[numthreads(64, 1, 1)]
void Scatter(uint i : SV_DispatchThreadID)
{
    if (i >= min(count, 64) || Failed() || TRANSFER_STAGE < 1) return;
    Particle p = LoadParticle(SNAPSHOT, i);
    uint entries = 0;
    float support = 1;
    for (uint axis = 0; axis < 3; ++axis)
    {
        Stencil s = Support(p.position, axis);
        entries += s.count;
        float sum = 0;
        for (uint j = 0; j < s.count; ++j) sum += s.weight[j];
        support = min(support, sum);
        if (TRANSFER_STAGE >= 2 && !Failed()) ScatterParticle(p, axis, s);
    }
    arena.Store(SAMPLES + i * 32 + 12, asuint(support));
    arena.Store(SAMPLES + i * 32 + 28, entries);
}
[numthreads(64, 1, 1)]
void Normalize(uint face : SV_DispatchThreadID)
{
    if (face >= 240 || Failed() || TRANSFER_STAGE < 3) return;
    NormalizeFace(face);
}
[numthreads(64, 1, 1)]
void Transfer(uint i : SV_DispatchThreadID)
{
    if (i >= min(count, 64) || Failed() || TRANSFER_STAGE < 4) return;
    Particle p = LoadParticle(SNAPSHOT, i);
    float3 pic = 0, delta = 0;
    for (uint axis = 0; axis < 3; ++axis)
    {
        Stencil s = Support(p.position, axis);
        if (TRANSFER_STAGE >= 5) delta[axis] = Gather(s, true);
        if (Failed()) return;
        pic[axis] = Gather(s, false);
        if (Failed()) return;
    }
    float3 velocity = TRANSFER_STAGE >= 5 ? TransferVelocity(p, pic, delta) : pic;
    if (!all(isfinite(velocity)) || any(abs(velocity) > 100)) { Fail(2); return; }
    arena.Store3(P + i * 32 + 16, asuint(velocity));
    arena.Store3(SAMPLES + i * 32, asuint(pic));
    arena.Store3(SAMPLES + i * 32 + 16, asuint(delta));
}
[numthreads(1, 1, 1)]
void Measure()
{
    if (Failed()) return;
    float3 mass = 0, momentum = 0, particleMomentum = 0;
    float kinetic = 0, inputKinetic = 0, support = TRANSFER_STAGE >= 1 ? 1 : 0;
    uint occupied = 0, entries = 0;
    for (uint f = 0; f < 240; ++f)
    {
        float2 data = asfloat(arena.Load2(FACES + f * 32));
        mass[f / 80] += data.x; momentum[f / 80] += data.y;
        occupied += arena.Load(FACES + f * 32 + 16);
    }
    for (uint i = 0; i < count; ++i)
    {
        Particle p = LoadParticle(P, i), old = LoadParticle(SNAPSHOT, i);
        kinetic += 0.5 * p.mass * dot(p.velocity, p.velocity);
        inputKinetic += 0.5 * old.mass * dot(old.velocity, old.velocity);
        particleMomentum += p.mass * p.velocity;
        support = min(support, asfloat(arena.Load(SAMPLES + i * 32 + 12)));
        entries += arena.Load(SAMPLES + i * 32 + 28);
    }
    uint round = arena.Load(STATUS + 4) + 1;
    arena.Store(STATUS + 4, round);
    arena.Store4(METRICS, asuint(float4(mass, kinetic)));
    arena.Store4(METRICS + 16, asuint(float4(momentum, inputKinetic)));
    arena.Store4(METRICS + 32, asuint(float4(particleMomentum, support)));
    arena.Store4(METRICS + 48, uint4(occupied, entries, arena.Load(STATUS + 12), round));
}
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD; };
Vertex Fullscreen(uint i : SV_VertexID)
{
    Vertex v; v.uv = float2((i << 1) & 2, i & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float2 Project(float3 p) { return float2(0.5 + p.x * 0.3 + p.z * 0.08, 0.5 - p.y * 0.3 + p.z * 0.04); }
float4 Diagnostic(Vertex input) : SV_Target
{
    float3 color = float3(0.015, 0.025, 0.045);
    for (uint f = 0; f < 240; ++f)
    {
        if (f / 80 != view % 3 || display.Load(FACES + f * 32 + 16) == 0) continue;
        float velocity = asfloat(display.Load(FACES + f * 32 + 12));
        if (length(input.uv - Project(FacePosition(f))) < 0.006)
            color = velocity >= 0 ? float3(0.1, 0.7, 0.4) : float3(0.9, 0.3, 0.1);
    }
    for (uint i = 0; i < min(count, 64); ++i)
    {
        float3 p = asfloat(display.Load3(P + i * 32));
        float3 velocity = asfloat(display.Load3(P + i * 32 + 16));
        float value = view == 3 ? length(asfloat(display.Load3(SAMPLES + i * 32 + 16))) : length(velocity);
        if (length(input.uv - Project(p)) < 0.009)
            color = lerp(float3(0.1, 0.3, 1), float3(1, 0.2, 0.05), saturate(value));
    }
    if (input.uv.y < 0.025) color = display.Load(STATUS) != 0 ? float3(1, 0, 0) : float3(0, 0.5, 0.2);
    return float4(color, 1);
}
