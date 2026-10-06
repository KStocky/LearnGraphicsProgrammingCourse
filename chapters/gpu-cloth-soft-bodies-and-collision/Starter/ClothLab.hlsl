cbuffer Settings : register(b0)
{
    uint particles, constraints, triangles, tetrahedra;
    uint stage, color, iterations, flags;
    float dt, friction, complianceScale, motionLimit;
    float3 gravity;
    uint padding;
};
ByteAddressBuffer Input : register(t0);
RWByteAddressBuffer Arena : register(u0);
ByteAddressBuffer Display : register(t1);
static const uint P = 0, Start = 1056, Snapshot = 2112, Lambda = 3168, Active = 3696;
static const uint Faces = 4224, Tets = 4496, Keys = 4544, Metrics = 4816, Status = 4880;
static const uint ConstraintInput = 1024, TriangleInput = 7168, TetInput = 8192;
struct Particle { float3 position; float weight; float3 velocity; float radius; };
struct Constraint { uint4 v; uint kind; uint color; float rest; float compliance; float ratio; };
Particle LoadParticle(uint region, uint i)
{
    uint offset = region + i * 32;
    Particle p;
    p.position = asfloat(Arena.Load3(offset));
    p.weight = asfloat(Arena.Load(offset + 12));
    p.velocity = asfloat(Arena.Load3(offset + 16));
    p.radius = asfloat(Arena.Load(offset + 28));
    return p;
}
void StoreParticle(uint region, uint i, Particle p)
{
    uint offset = region + i * 32;
    Arena.Store3(offset, asuint(p.position));
    Arena.Store(offset + 12, asuint(p.weight));
    Arena.Store3(offset + 16, asuint(p.velocity));
    Arena.Store(offset + 28, asuint(p.radius));
}
Constraint LoadConstraint(uint i)
{
    uint offset = ConstraintInput + i * 48;
    Constraint c;
    c.v = Input.Load4(offset);
    c.kind = Input.Load(offset + 16);
    c.color = Input.Load(offset + 20);
    c.rest = asfloat(Input.Load(offset + 24));
    c.compliance = asfloat(Input.Load(offset + 28));
    c.ratio = asfloat(Input.Load(offset + 32));
    return c;
}
uint Arity(Constraint c) { return c.kind == 4 ? 4 : c.kind == 3 ? 3 : 2; }
void Error(uint code) { uint old; Arena.InterlockedCompareExchange(Status, 0, code, old); }
bool Domain(float3 p) { return all(isfinite(p)) && all(p >= -4.0) && all(p < 4.0); }
bool Evaluate(Constraint c, out float value, out float3 g[4])
{
    value = 0;
    [unroll] for (uint i = 0; i < 4; ++i) g[i] = 0;
    float3 a = LoadParticle(P, c.v.x).position;
    float3 b = LoadParticle(P, c.v.y).position;
    if (c.kind <= 2)
    {
        float3 delta = a - b;
        value = length(delta);
        if (value < 1e-7) { Error(5); return false; }
        g[0] = delta / value; g[1] = -g[0];
    }
    else if (c.kind == 3)
    {
        float3 d = LoadParticle(P, c.v.z).position;
        float3 n = cross(b - a, d - a);
        float magnitude = length(n);
        if (magnitude < 1e-7) { Error(5); return false; }
        value = magnitude * 0.5;
        n /= magnitude;
        g[1] = cross(d - a, n) * 0.5;
        g[2] = cross(n, b - a) * 0.5;
        g[0] = -g[1] - g[2];
    }
    else
    {
        float3 d = LoadParticle(P, c.v.z).position;
        float3 f = LoadParticle(P, c.v.w).position;
        value = dot(b - a, cross(d - a, f - a)) / 6.0;
        if (value <= 1e-8) { Error(6); return false; }
        g[1] = cross(d - a, f - a) / 6.0;
        g[2] = cross(f - a, b - a) / 6.0;
        g[3] = cross(b - a, d - a) / 6.0;
        g[0] = -g[1] - g[2] - g[3];
    }
    if (!isfinite(value)) { Error(9); return false; }
    return true;
}
void ProjectDistance(uint i, Constraint c)
{
    // Learner checkpoint: this physics stage is disabled by the trusted host kStage.
}
void ProjectShape(uint i, Constraint c)
{
    // Learner checkpoint: this physics stage is disabled by the trusted host kStage.
}
[numthreads(1, 1, 1)]
void BeginTick(uint3 id : SV_DispatchThreadID)
{
    if (Arena.Load(Status) != 0) return;
    for (uint offset = 0; offset < 64; offset += 4) Arena.Store(Metrics + offset, 0);
    Arena.Store(Status + 8, 0); Arena.Store(Status + 12, 0);
}
[numthreads(128, 1, 1)]
void ResetLambda(uint3 id : SV_DispatchThreadID)
{
    if (id.x < constraints && Arena.Load(Status) == 0) Arena.Store(Lambda + id.x * 4, 0);
}
[numthreads(32, 1, 1)]
void Predict(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= particles || Arena.Load(Status) != 0) return;
    Particle p = LoadParticle(P, id.x);
    StoreParticle(Start, id.x, p);
    if (p.weight > 0)
    {
        p.velocity += gravity * dt;
        float3 movement = p.velocity * dt;
        if (!all(isfinite(movement))) { Error(9); return; }
        if (length(movement) > motionLimit) { Error(7); return; }
        p.position += movement;
    }
    else p.velocity = 0;
    if (!Domain(p.position)) { Error(8); return; }
    StoreParticle(P, id.x, p);
}
[numthreads(128, 1, 1)]
void Colored(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= constraints || stage == 0 || Arena.Load(Status) != 0 || Arena.Load(Active + i * 4) == 0) return;
    Constraint c = LoadConstraint(i);
    if (c.color != color) return;
    if (c.kind <= 2) ProjectDistance(i, c);
    else if (stage >= 2) ProjectShape(i, c);
}
[numthreads(1, 1, 1)]
void ValidateVolumes(uint3 id : SV_DispatchThreadID)
{
    if (stage < 2 || Arena.Load(Status) != 0) return;
    for (uint t = 0; t < tetrahedra; ++t)
    {
        if (Arena.Load(Tets + t * 4) == 0) continue;
        Constraint c;
        c.v = Input.Load4(TetInput + t * 16);
        c.kind = 4; c.color = 0; c.rest = 0; c.compliance = 0; c.ratio = 0;
        float value; float3 gradients[4];
        if (!Evaluate(c, value, gradients)) return;
    }
}
bool Has(uint4 v, uint count, uint a)
{
    for (uint i = 0; i < count; ++i) if (v[i] == a) return true;
    return false;
}
void TearTopology()
{
    // Learner checkpoint: this physics stage is disabled by the trusted host kStage.
}
[numthreads(1, 1, 1)]
void Tear(uint3 id : SV_DispatchThreadID)
{
    if (stage == 5 && (flags & 4) != 0 && Arena.Load(Status) == 0) TearTopology();
}
void ContactParticle(uint i)
{
    // Learner checkpoint: this physics stage is disabled by the trusted host kStage.
}
[numthreads(32, 1, 1)]
void Environment(uint3 id : SV_DispatchThreadID)
{
    if (id.x < particles && stage >= 3 && (flags & 1) != 0 && Arena.Load(Status) == 0) ContactParticle(id.x);
}
int3 Cell(float3 p) { return (int3)floor((p + 4.0) / 0.25); }
uint Key(int3 c) { return (uint)(c.x + 32 * c.y + 1024 * c.z); }
bool Excluded(uint a, uint b)
{
    for (uint i = 0; i < constraints; ++i)
    {
        Constraint c = LoadConstraint(i);
        if (c.kind <= 2 && Arena.Load(Active + i * 4) != 0 && Has(c.v, 2, a) && Has(c.v, 2, b)) return true;
    }
    for (uint t = 0; t < triangles; ++t)
    {
        uint4 v = Input.Load4(TriangleInput + t * 16);
        if (Arena.Load(Faces + t * 4) != 0 && Has(v, 3, a) && Has(v, 3, b)) return true;
    }
    for (uint t = 0; t < tetrahedra; ++t)
    {
        uint4 v = Input.Load4(TetInput + t * 16);
        if (Arena.Load(Tets + t * 4) != 0 && Has(v, 4, a) && Has(v, 4, b)) return true;
    }
    return false;
}
[numthreads(1, 1, 1)]
void BinSort(uint3 id : SV_DispatchThreadID)
{
    // Learner checkpoint: this physics stage is disabled by the trusted host kStage.
}
void SelfGather(uint i)
{
    // Learner checkpoint: this physics stage is disabled by the trusted host kStage.
}
[numthreads(32, 1, 1)]
void SelfContact(uint3 id : SV_DispatchThreadID)
{
    if (id.x < particles && stage >= 4 && (flags & 2) != 0 && Arena.Load(Status) == 0) SelfGather(id.x);
}
[numthreads(1, 1, 1)]
void Velocity(uint3 id : SV_DispatchThreadID)
{
    if (Arena.Load(Status) != 0) return;
    float motion = asfloat(Arena.Load(Metrics + 28));
    for (uint i = 0; i < particles; ++i)
    {
        Particle p = LoadParticle(P, i);
        float3 displacement = p.position - LoadParticle(Start, i).position;
        motion = max(motion, length(displacement));
        if (!all(isfinite(p.position)) || !all(isfinite(p.velocity))) { Error(9); return; }
        if (!Domain(p.position)) { Error(8); return; }
        if (length(displacement) > motionLimit) { Error(7); return; }
        p.velocity = p.weight > 0 ? displacement / dt : float3(0, 0, 0);
        StoreParticle(P, i, p);
    }
    Arena.Store(Metrics + 28, asuint(motion));
}
[numthreads(1, 1, 1)]
void Measure(uint3 id : SV_DispatchThreadID)
{
    if (Arena.Load(Status) != 0) return;
    float distance = 0, area = 0, volume = 0, compliant = 0, penetration = 0, speed = 0, kinetic = 0;
    uint broken = 0;
    for (uint i = 0; i < constraints; ++i)
    {
        if (Arena.Load(Active + i * 4) == 0) { ++broken; continue; }
        Constraint c = LoadConstraint(i);
        if (stage == 0 || (stage == 1 && c.kind > 2)) continue;
        float value; float3 g[4];
        if (!Evaluate(c, value, g)) return;
        float error = abs(value - c.rest);
        if (c.kind <= 2) distance = max(distance, error);
        else if (c.kind == 3) area = max(area, error);
        else volume = max(volume, error);
        compliant = max(compliant, abs(value - c.rest +
            c.compliance * complianceScale / (dt * dt) * asfloat(Arena.Load(Lambda + i * 4))));
    }
    for (uint i = 0; i < particles; ++i)
    {
        Particle p = LoadParticle(P, i);
        float magnitude = length(p.velocity);
        speed = max(speed, magnitude);
        if (p.weight > 0) kinetic += 0.5 * magnitude * magnitude / p.weight;
        if (stage >= 3 && (flags & 1) != 0)
        {
            penetration = max(penetration, max(0.0, p.radius - p.position.y));
            penetration = max(penetration, max(0.0, p.radius + 0.35 - length(p.position - float3(0, 0.55, 0))));
        }
        if (stage >= 4 && (flags & 2) != 0)
        {
            for (uint j = i + 1; j < particles; ++j)
            {
                if (!Excluded(i, j))
                {
                    Particle q = LoadParticle(P, j);
                    penetration = max(penetration, max(0.0, p.radius + q.radius - length(p.position - q.position)));
                }
            }
        }
    }
    Arena.Store4(Metrics, asuint(float4(distance, area, volume, compliant)));
    Arena.Store3(Metrics + 16, asuint(float3(penetration, speed, kinetic)));
    uint ticks = Arena.Load(Status + 4) + 1;
    Arena.Store(Status + 4, ticks);
    Arena.Store4(Metrics + 32, uint4(Arena.Load(Status + 8), Arena.Load(Status + 12), broken, ticks));
    if (!all(isfinite(float4(distance, area, volume, compliant))) ||
        !all(isfinite(float3(penetration, speed, kinetic)))) Error(9);
}
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex Fullscreen(uint id : SV_VertexID)
{
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float2 Project(float3 p) { return float2(0.5 + p.x * 0.25 + p.z * 0.07, 0.85 - p.y * 0.35 + p.z * 0.04); }
float Segment(float2 p, float2 a, float2 b)
{
    float2 d = b - a; float squared = dot(d, d);
    float t = squared > 1e-12 ? saturate(dot(p - a, d) / squared) : 0;
    return length(p - a - t * d);
}
float3 DisplayPosition(uint i) { return asfloat(Display.Load3(P + i * 32)); }
float4 Diagnostic(Vertex v) : SV_Target
{
    float3 result = float3(0.025, 0.035, 0.055);
    if (abs(v.uv.y - 0.85) < 0.002) result = float3(0.6, 0.6, 0.6);
    float sphere = length((v.uv - Project(float3(0, 0.55, 0))) / float2(0.25, 0.35));
    if (abs(sphere - 0.35) < 0.008) result = float3(0.4, 0.45, 0.5);
    for (uint t = 0; t < triangles; ++t)
    {
        uint3 ids = Input.Load3(TriangleInput + t * 16);
        float2 a = Project(DisplayPosition(ids.x)), b = Project(DisplayPosition(ids.y)), c = Project(DisplayPosition(ids.z));
        bool active = Display.Load(Faces + t * 4) != 0;
        float2 ab = b - a, ac = c - a, ap = v.uv - a;
        float determinant = ab.x * ac.y - ab.y * ac.x;
        if (active && abs(determinant) > 1e-8)
        {
            float u = (ap.x * ac.y - ap.y * ac.x) / determinant;
            float w = (ab.x * ap.y - ab.y * ap.x) / determinant;
            if (u >= 0 && w >= 0 && u + w <= 1) result = float3(0.06, 0.14, 0.22);
        }
        float edge = min(Segment(v.uv, a, b), min(Segment(v.uv, b, c), Segment(v.uv, c, a)));
        if (edge < 0.0018) result = active ? float3(0.25, 0.65, 0.9) : float3(0.8, 0.15, 0.08);
    }
    for (uint i = 0; i < constraints; ++i)
    {
        Constraint c = LoadConstraint(i);
        if (c.kind > 1) continue;
        if (Display.Load(Active + i * 4) == 0 &&
            Segment(v.uv, Project(DisplayPosition(c.v.x)), Project(DisplayPosition(c.v.y))) < 0.0025)
            result = float3(1, 0.15, 0.05);
    }
    for (uint i = 0; i < particles; ++i)
    {
        float3 p = DisplayPosition(i);
        float weight = asfloat(Display.Load(P + i * 32 + 12));
        float radius = asfloat(Display.Load(P + i * 32 + 28));
        if (length(v.uv - Project(p)) < 0.005)
        {
            bool contact = p.y <= radius + 0.002 || length(p - float3(0, 0.55, 0)) <= radius + 0.352;
            result = weight == 0 ? float3(1, 0.85, 0.1) : contact ? float3(0.2, 1, 0.35) : float3(0.85, 0.9, 1);
        }
    }
    float residual = asfloat(Display.Load(Metrics));
    float penetration = asfloat(Display.Load(Metrics + 16));
    if (v.uv.y < 0.025 && v.uv.x < saturate(residual * 100)) result = float3(1, 0.6, 0.1);
    if (v.uv.y > 0.03 && v.uv.y < 0.05 && v.uv.x < saturate(penetration * 100)) result = float3(1, 0.1, 0.5);
    if (Display.Load(Status) != 0 && v.uv.y < 0.08) result = float3(1, 0, 0);
    return float4(result, 1);
}
