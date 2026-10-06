cbuffer Constants : register(b0)
{
    uint count, stage, useGrid, signedPressure;
    float radius, restDensity, soundSpeed, dynamicViscosity;
    float dt, restitution, motionLimit;
    uint view;
    float3 gravity;
    uint walls;
};
RWByteAddressBuffer arena : register(u0);
ByteAddressBuffer display : register(t0);
static const uint P = 0, SNAPSHOT = 2064, SAMPLES = 4128, KEYS = 6192;
static const uint MASKS = 6720, STEP = 7248, METRICS = 8288, STATUS = 8384, BYTES = 8416;
struct Particle { float3 position; float mass; float3 velocity; uint identity; };
struct Sample { float density, pressure; float3 acceleration; uint candidates, accepted, coincident; };
void Fail(uint error)
{
    uint previous;
    arena.InterlockedCompareExchange(STATUS, 0, error, previous);
}
bool Failed() { return arena.Load(STATUS) != 0; }
Particle LoadParticle(uint start, uint i)
{
    uint4 a = arena.Load4(start + i * 32);
    uint4 b = arena.Load4(start + i * 32 + 16);
    Particle p;
    p.position = asfloat(a.xyz); p.mass = asfloat(a.w);
    p.velocity = asfloat(b.xyz); p.identity = b.w;
    return p;
}
void StoreParticle(uint start, uint i, Particle p)
{
    arena.Store4(start + i * 32, uint4(asuint(p.position), asuint(p.mass)));
    arena.Store4(start + i * 32 + 16, uint4(asuint(p.velocity), p.identity));
}
Sample LoadSample(uint i)
{
    uint4 a = arena.Load4(SAMPLES + i * 32);
    uint4 b = arena.Load4(SAMPLES + i * 32 + 16);
    Sample s;
    s.density = asfloat(a.x); s.pressure = asfloat(a.y);
    s.acceleration = asfloat(uint3(a.zw, b.x));
    s.candidates = b.y; s.accepted = b.z; s.coincident = b.w;
    return s;
}
void StoreSample(uint i, Sample s)
{
    arena.Store4(SAMPLES + i * 32, asuint(float4(s.density, s.pressure, s.acceleration.xy)));
    arena.Store4(SAMPLES + i * 32 + 16, uint4(asuint(s.acceleration.z), s.candidates, s.accepted, s.coincident));
}
#include "Physics.hlsli"

[numthreads(1, 1, 1)]
void Preflight()
{
    if (Failed()) return;
    if (count == 0 || count > 64) { Fail(1); return; }
    if (!all(isfinite(float4(radius, restDensity, soundSpeed, dynamicViscosity))) ||
        !all(isfinite(float4(dt, restitution, motionLimit, 0))) || !all(isfinite(gravity)))
        { Fail(3); return; }
    if ((radius != 0.125 && radius != 0.25 && radius != 0.5) ||
        restDensity < 1 || restDensity > 10000 || soundSpeed <= 0 || soundSpeed > 100 ||
        dynamicViscosity < 0 || dynamicViscosity > 100 || dt < 0.000001 / 16 || dt > 0.1 ||
        restitution < 0 || restitution > 1 || motionLimit <= 0 || motionLimit > 1 ||
        length(gravity) > 100 || view > 3 || stage != SPH_STAGE)
        Fail(2);
}
[numthreads(64, 1, 1)]
void Snapshot(uint i : SV_DispatchThreadID)
{
    if (i >= min(count, 64) || Failed()) return;
    Particle p = LoadParticle(P, i);
    if (!all(isfinite(p.position)) || !all(isfinite(p.velocity)) || !isfinite(p.mass))
        { Fail(3); return; }
    if (p.mass < 0.000001 || p.mass > 1000) { Fail(4); return; }
    if (any(p.position < -2) || any(p.position >= 2)) { Fail(5); return; }
    if (p.identity != i) { Fail(8); return; }
    StoreParticle(SNAPSHOT, i, p);
    int3 cell = int3(floor((p.position + 2) / radius));
    uint dimensions = uint(4 / radius);
    // Domain was validated above. Float addition can round the last IN-domain value to 4;
    // assign it to the last cell, never admit or wrap an OUT-of-domain position.
    cell = min(cell, int3(dimensions - 1, dimensions - 1, dimensions - 1));
    uint key = uint(cell.x) + dimensions * (uint(cell.y) + dimensions * uint(cell.z));
    arena.Store2(KEYS + i * 8, uint2(key, i));
}
[numthreads(1, 1, 1)]
void Sort()
{
    if (Failed()) return;
    // Bounded 64-record insertion sort, ordered by (cell key, stable identity).
    // Deliberately simple, not a production throughput sorter.
    for (uint i = 1; i < count; ++i)
    {
        uint2 value = arena.Load2(KEYS + i * 8);
        uint j = i;
        while (j > 0)
        {
            uint2 previous = arena.Load2(KEYS + (j - 1) * 8);
            if (previous.x < value.x || (previous.x == value.x && previous.y < value.y)) break;
            arena.Store2(KEYS + j * 8, previous);
            --j;
        }
        arena.Store2(KEYS + j * 8, value);
    }
}
[numthreads(64, 1, 1)]
void Density(uint i : SV_DispatchThreadID)
{
    if (i >= min(count, 64) || Failed()) return;
    Particle a = LoadParticle(SNAPSHOT, i);
    Sample s = (Sample)0;
    uint2 mask = 0;
    uint cursor = 0, record = useGrid != 0 && SPH_STAGE >= 5 ? 0xffffffff : 0, j;
    while (NextCandidate(a.position, cursor, record, j))
    {
        ++s.candidates;
        Particle b = LoadParticle(SNAPSHOT, j);
        float r = length(a.position - b.position);
        if (r >= radius) continue;
        ++s.accepted;
        mask[j / 32] |= 1u << (j % 32);
        if (i != j && r == 0) ++s.coincident;
        s.density += DensityContribution(b.mass, r);
    }
    if (SPH_STAGE >= 2)
    {
        if (!isfinite(s.density)) { Fail(3); return; }
        if (s.density <= 0) { Fail(6); return; }
        s.pressure = EquationOfState(s.density);
        if (!isfinite(s.pressure)) { Fail(3); return; }
    }
    StoreSample(i, s);
    arena.Store2(MASKS + i * 8, mask);
}
[numthreads(64, 1, 1)]
void Forces(uint i : SV_DispatchThreadID)
{
    if (i >= min(count, 64) || Failed()) return;
    Particle a = LoadParticle(SNAPSHOT, i);
    Sample sa = LoadSample(i);
    float3 acceleration = 0;
    uint cursor = 0, record = useGrid != 0 && SPH_STAGE >= 5 ? 0xffffffff : 0, j;
    if (SPH_STAGE >= 3)
    {
        while (NextCandidate(a.position, cursor, record, j))
        {
            Particle b = LoadParticle(SNAPSHOT, j);
            // Only density/pressure are read from immutable neighbor samples.
            Sample sb = (Sample)0;
            float2 densityPressure = asfloat(arena.Load2(SAMPLES + j * 32));
            sb.density = densityPressure.x;
            sb.pressure = densityPressure.y;
            acceleration += PairAcceleration(a, b, sa, sb);
        }
    }
    if (!all(isfinite(acceleration))) { Fail(3); return; }
    // One writer updates ONLY its acceleration field; density/pressure stay immutable.
    arena.Store3(SAMPLES + i * 32 + 8, asuint(acceleration));
}
[numthreads(64, 1, 1)]
void Integrate(uint i : SV_DispatchThreadID)
{
    if (i >= min(count, 64) || Failed()) return;
    Particle p = LoadParticle(SNAPSHOT, i);
    float motion;
    uint contacts;
    IntegrateParticle(p, LoadSample(i).acceleration, motion, contacts);
    arena.Store4(STEP + i * 16, uint4(asuint(motion), contacts, 0, 0));
    // Failure may leave other particles committed: intentionally NONTRANSACTIONAL.
    if (!Failed()) StoreParticle(P, i, p);
}
[numthreads(1, 1, 1)]
void Measure()
{
    if (Failed()) return;
    float rhoMin = LoadSample(0).density, rhoMax = 0;
    float pMin = LoadSample(0).pressure, pMax = pMin;
    float speed = 0, acceleration = 0, motion = 0, kinetic = 0;
    uint candidates = 0, accepted = 0, coincident = 0, cMin = count, cMax = 0, contacts = 0;
    float3 momentum = 0;
    for (uint i = 0; i < count; ++i)
    {
        Particle p = LoadParticle(P, i);
        Sample s = LoadSample(i);
        rhoMin = min(rhoMin, s.density); rhoMax = max(rhoMax, s.density);
        pMin = min(pMin, s.pressure); pMax = max(pMax, s.pressure);
        speed = max(speed, length(p.velocity)); acceleration = max(acceleration, length(s.acceleration));
        motion = max(motion, asfloat(arena.Load(STEP + i * 16)));
        contacts += arena.Load(STEP + i * 16 + 4);
        candidates += s.candidates; accepted += s.accepted; coincident += s.coincident;
        cMin = min(cMin, s.candidates); cMax = max(cMax, s.candidates);
        kinetic += 0.5 * p.mass * dot(p.velocity, p.velocity);
        momentum += p.mass * p.velocity;
    }
    float sound = soundSpeed * dt / radius;
    uint warnings = (sound > 0.1 ? 1 : 0) | (motion > 0.1 ? 2 : 0);
    arena.Store4(METRICS, asuint(float4(rhoMin, rhoMax, pMin, pMax)));
    arena.Store4(METRICS + 16, asuint(float4(speed, acceleration, sound, motion)));
    arena.Store4(METRICS + 32, asuint(float4(Poly6(0), SPH_STAGE >= 2 ? max(0, 1 - rhoMin / restDensity) : 0,
                                                kinetic, length(momentum))));
    arena.Store4(METRICS + 48, uint4(candidates, accepted, coincident, candidates * (SPH_STAGE >= 3 ? 2 : 1)));
    arena.Store4(METRICS + 64, uint4(cMin, cMax, warnings, contacts));
    arena.Store4(METRICS + 80, asuint(float4(Poly6Gradient(float3(radius * 0.5, 0, 0)).x,
        SpikyGradient(float3(radius * 0.5, 0, 0)).x, ViscosityLaplacian(radius * 0.5), Poly6(radius))));
    arena.Store(STATUS + 8, arena.Load(STATUS + 8) + 1);
}
[numthreads(1, 1, 1)]
void EndTick()
{
    if (!Failed()) arena.Store(STATUS + 4, arena.Load(STATUS + 4) + 1);
}
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD; };
Vertex Fullscreen(uint i : SV_VertexID)
{
    Vertex v;
    v.uv = float2((i << 1) & 2, i & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float4 Diagnostic(Vertex input) : SV_Target
{
    float3 color = float3(0.015, 0.025, 0.045);
    for (uint i = 0; i < min(count, 64); ++i)
    {
        float3 p = asfloat(display.Load3(P + i * 32));
        float2 center = float2(0.5 + p.x * 0.21 + p.z * 0.07, 0.5 - p.y * 0.21 + p.z * 0.04);
        float rho = asfloat(display.Load(SAMPLES + i * 32));
        float pressure = asfloat(display.Load(SAMPLES + i * 32 + 4));
        float value = view == 0 ? rho / restDensity : view == 1 ? pressure / (restDensity * soundSpeed * soundSpeed) :
                      view == 2 ? float(display.Load(SAMPLES + i * 32 + 24)) / count :
                      length(asfloat(display.Load3(P + i * 32 + 16))) / soundSpeed;
        if (length(input.uv - center) < 0.009) color = lerp(float3(0.1, 0.3, 1), float3(1, 0.2, 0.05), saturate(value));
    }
    if (input.uv.y < 0.025)
        color = display.Load(STATUS) != 0 ? float3(1, 0, 0) :
                display.Load(METRICS + 72) != 0 ? float3(1, 0.6, 0) : float3(0, 0.5, 0.2);
    return float4(color, 1);
}
