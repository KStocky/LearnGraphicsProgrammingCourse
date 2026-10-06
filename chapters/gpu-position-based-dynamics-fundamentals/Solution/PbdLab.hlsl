// Raw ABI matches Common/GpuLabSupport.hpp and the unchanged CPU contracts.
cbuffer Lab : register(b0)
{
    uint particleCount;
    uint constraintCount;
    uint inputOffset;
    uint outputOffset;
    float dt;
    float relaxation;
    uint activeColor;
    uint metricIndex;
    float3 gravity;
    uint strategy;
    uint iterationCount;
    uint project;
    uint view;
    uint complianceEnabled;
};

ByteAddressBuffer Scene : register(t0);
RWByteAddressBuffer Arena : register(u0);
ByteAddressBuffer Display : register(t1);

static const uint A = 0;
static const uint B = 1056;
static const uint Predicted = 2112;
static const uint Lambdas = 3168;
static const uint CorrectionBase = 3312;
static const uint Integers = 4368;
static const uint MetricsBase = 4896;
static const uint Status = 7040;
static const uint Guard = 0x5A17C0DE;
static const float Scale = 1048576.0;
static const float Epsilon = 1.0e-6;

struct Particle
{
    float3 position;
    float weight;
    float3 velocity;
    uint padding;
};
struct Constraint
{
    uint first;
    uint second;
    uint kind;
    uint color;
    float rest;
    float compliance;
    float3 anchor;
};
struct Correction
{
    float3 first;
    float deltaLambda;
    float3 second;
};

Particle LoadParticle(uint base, uint index)
{
    uint address = base + index * 32;
    Particle p;
    p.position = asfloat(Arena.Load3(address));
    p.weight = asfloat(Arena.Load(address + 12));
    p.velocity = asfloat(Arena.Load3(address + 16));
    p.padding = Arena.Load(address + 28);
    return p;
}
void StoreParticle(uint base, uint index, Particle p)
{
    uint address = base + index * 32;
    Arena.Store4(address, asuint(float4(p.position, p.weight)));
    Arena.Store3(address + 16, asuint(p.velocity));
    Arena.Store(address + 28, p.padding);
}
Constraint LoadConstraint(uint index)
{
    uint address = 1024 + index * 48;
    uint4 metadata = Scene.Load4(address);
    Constraint c;
    c.first = metadata.x;
    c.second = metadata.y;
    c.kind = metadata.z;
    c.color = metadata.w;
    c.rest = asfloat(Scene.Load(address + 16));
    c.compliance = asfloat(Scene.Load(address + 20));
    c.anchor = asfloat(Scene.Load3(address + 32));
    return c;
}
void Fail(uint errorPlusOne)
{
    uint original;
    Arena.InterlockedCompareExchange(Status, 0, errorPlusOne, original);
}
Correction ProjectConstraint(uint index, float damping)
{
    Constraint c = LoadConstraint(index);
    Particle first = LoadParticle(inputOffset, c.first);
    float3 other = c.anchor;
    float secondWeight = 0;
    if (c.kind == 0)
    {
        Particle second = LoadParticle(inputOffset, c.second);
        other = second.position;
        secondWeight = second.weight;
    }
    float3 separation = first.position - other;
    float magnitude = length(separation);
    float value = magnitude - c.rest;
    Correction correction = (Correction)0;
    if (!all(isfinite(separation)) || !isfinite(magnitude))
    {
        Fail(11); // NonFiniteResult
        return correction;
    }
    if (c.kind == 0 && magnitude <= Epsilon)
    {
        Fail(6); // DegenerateDistance; no undocumented direction fallback.
        return correction;
    }
    float weight = first.weight + secondWeight;
    if (weight == 0)
    {
        if (abs(value) > Epsilon)
            Fail(7); // NoMovableParticle
        return correction;
    }
    float alphaTilde = complianceEnabled != 0 ? c.compliance / (dt * dt) : 0;
    float lambda = asfloat(Arena.Load(Lambdas + index * 4));
    correction.deltaLambda = -damping * (value + alphaTilde * lambda) / (weight + alphaTilde);
    // A satisfied zero-length attachment has no gradient, not a divide by zero.
    float3 gradient = 0;
    if (magnitude > Epsilon)
        gradient = separation / magnitude;
    correction.first = first.weight * gradient * correction.deltaLambda;
    correction.second = -secondWeight * gradient * correction.deltaLambda;
    if (!all(isfinite(correction.first)) || !all(isfinite(correction.second)) ||
        !isfinite(correction.deltaLambda))
    {
        Fail(11);
        return (Correction)0;
    }
    return correction;
}
void StoreCorrection(uint index, Correction correction)
{
    Arena.Store4(CorrectionBase + index * 32, asuint(float4(correction.first, correction.deltaLambda)));
    Arena.Store4(CorrectionBase + index * 32 + 16, asuint(float4(correction.second, 0)));
    float lambda = asfloat(Arena.Load(Lambdas + index * 4));
    Arena.Store(Lambdas + index * 4, asuint(lambda + correction.deltaLambda));
}

[numthreads(32, 1, 1)]
void Predict(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    if (index == 0)
        Arena.Store4(Status, uint4(0, 0, 0, project));
    if (index < particleCount)
    {
        uint address = index * 32;
        Particle p;
        p.position = asfloat(Scene.Load3(address));
        p.weight = asfloat(Scene.Load(address + 12));
        p.velocity = asfloat(Scene.Load3(address + 16));
        p.padding = 0;
        if (p.weight > 0)
        {
            p.velocity += gravity * dt;
            p.position += p.velocity * dt;
        }
        else
            p.velocity = 0;
        StoreParticle(A, index, p);
        StoreParticle(B, index, p);
        StoreParticle(Predicted, index, p);
        Arena.Store4(Integers + index * 16, 0);
    }
    if (index < constraintCount)
    {
        Arena.Store(Lambdas + index * 4, asuint(0.0));
        Arena.Store4(CorrectionBase + index * 32, 0);
        Arena.Store4(CorrectionBase + index * 32 + 16, 0);
    }
}

[numthreads(32, 1, 1)]
void Colored(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    if (index >= constraintCount)
        return;
    Constraint c = LoadConstraint(index);
    if (c.color != activeColor)
        return;
    Correction correction = ProjectConstraint(index, 1);
    StoreCorrection(index, correction);
    Particle first = LoadParticle(inputOffset, c.first);
    // CPU color validation permits shared immutable pins: NEVER write them.
    if (first.weight > 0)
        Arena.Store3(inputOffset + c.first * 32, asuint(first.position + correction.first));
    if (c.kind == 0)
    {
        Particle second = LoadParticle(inputOffset, c.second);
        if (second.weight > 0)
            Arena.Store3(inputOffset + c.second * 32, asuint(second.position + correction.second));
    }
}

[numthreads(32, 1, 1)]
void Corrections(uint3 id : SV_DispatchThreadID)
{
    if (id.x < constraintCount)
        StoreCorrection(id.x, ProjectConstraint(id.x, relaxation));
}

[numthreads(32, 1, 1)]
void Gather(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    if (index >= particleCount)
        return;
    Particle p = LoadParticle(inputOffset, index);
    float3 sum = 0;
    // Deterministic ascending constraint order; input positions remain immutable.
    for (uint edge = 0; edge < constraintCount; ++edge)
    {
        Constraint c = LoadConstraint(edge);
        if (c.first == index)
            sum += asfloat(Arena.Load3(CorrectionBase + edge * 32));
        if (c.kind == 0 && c.second == index)
            sum += asfloat(Arena.Load3(CorrectionBase + edge * 32 + 16));
    }
    if (p.weight > 0)
        p.position += sum;
    StoreParticle(outputOffset, index, p);
}

[numthreads(32, 1, 1)]
void ResetIntegers(uint3 id : SV_DispatchThreadID)
{
    if (id.x < particleCount)
        Arena.Store4(Integers + id.x * 16, 0);
}

int3 Quantize(float3 correction)
{
    // Adding 0.5 first can round a float just below half up to one.
    // Split the fraction before the exact bounded integer increment instead.
    float3 scaled = abs(correction * Scale);
    float3 whole = floor(scaled);
    float3 fraction = scaled - whole;
    int3 magnitude = int3(whole) + int3(fraction >= 0.5);
    return int3(sign(correction)) * magnitude;
}
void AddIntegers(uint particle, float3 correction)
{
    int3 fixed = Quantize(correction);
    uint address = Integers + particle * 16;
    Arena.InterlockedAdd(address, fixed.x);
    Arena.InterlockedAdd(address + 4, fixed.y);
    Arena.InterlockedAdd(address + 8, fixed.z);
}

[numthreads(32, 1, 1)]
void Accumulate(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    if (index >= constraintCount)
        return;
    Correction correction = ProjectConstraint(index, relaxation);
    if (any(abs(correction.first) > 8) || any(abs(correction.second) > 8))
    {
        Fail(10); // AtomicRange: fail explicitly, never clamp or overflow.
        return;
    }
    StoreCorrection(index, correction);
    Constraint c = LoadConstraint(index);
    // Max degree 4, abs(component)<=8: worst signed sum 33,554,432 < INT32_MAX.
    if (LoadParticle(inputOffset, c.first).weight > 0)
        AddIntegers(c.first, correction.first);
    if (c.kind == 0 && LoadParticle(inputOffset, c.second).weight > 0)
        AddIntegers(c.second, correction.second);
}

[numthreads(32, 1, 1)]
void ApplyIntegers(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    if (index >= particleCount)
        return;
    Particle p = LoadParticle(inputOffset, index);
    int3 fixed = asint(Arena.Load3(Integers + index * 16));
    if (p.weight > 0)
        p.position += float3(fixed) / Scale;
    StoreParticle(outputOffset, index, p);
}

[numthreads(32, 1, 1)]
void Velocity(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    if (index >= particleCount)
        return;
    Particle p = LoadParticle(inputOffset, index);
    float3 old = asfloat(Scene.Load3(index * 32));
    p.velocity = p.weight > 0 ? (p.position - old) / dt : float3(0, 0, 0);
    StoreParticle(inputOffset, index, p);
}

[numthreads(1, 1, 1)]
void Measure()
{
    float distanceError = 0;
    float attachmentError = 0;
    float compliantResidual = 0;
    float maximumSpeed = 0;
    float energy = 0;
    for (uint edge = 0; edge < constraintCount; ++edge)
    {
        Constraint c = LoadConstraint(edge);
        float3 other = c.kind == 0 ? LoadParticle(inputOffset, c.second).position : c.anchor;
        float value = length(LoadParticle(inputOffset, c.first).position - other) - c.rest;
        float lambda = asfloat(Arena.Load(Lambdas + edge * 4));
        float alphaTilde = complianceEnabled != 0 ? c.compliance / (dt * dt) : 0;
        float residual = abs(value + alphaTilde * lambda);
        if (!isfinite(value) || !isfinite(residual) || !isfinite(lambda))
            Fail(11);
        if (c.kind == 0)
            distanceError = max(distanceError, abs(value));
        else
            attachmentError = max(attachmentError, abs(value));
        compliantResidual = max(compliantResidual, residual);
    }
    for (uint index = 0; index < particleCount; ++index)
    {
        Particle p = LoadParticle(inputOffset, index);
        float speed = length(p.velocity);
        if (!all(isfinite(p.position)) || !all(isfinite(p.velocity)) || !isfinite(speed))
            Fail(11);
        maximumSpeed = max(maximumSpeed, speed);
        if (p.weight > 0)
            energy += 0.5 * speed * speed / p.weight;
    }
    if (!isfinite(energy))
        Fail(11);
    uint address = MetricsBase + metricIndex * 32;
    Arena.Store4(address, asuint(float4(distanceError, attachmentError, compliantResidual, maximumSpeed)));
    Arena.Store4(address + 16, asuint(float4(energy, 0, 0, 0)));
}

bool GuardRegion(uint base, uint used, uint bytes)
{
    for (uint offset = used; offset < bytes; offset += 4)
        if (Arena.Load(base + offset) != Guard)
            return false;
    return true;
}
[numthreads(1, 1, 1)]
void Guards()
{
    bool intact = GuardRegion(A, particleCount * 32, 1056);
    intact = GuardRegion(B, particleCount * 32, 1056) && intact;
    intact = GuardRegion(Predicted, particleCount * 32, 1056) && intact;
    intact = GuardRegion(Lambdas, constraintCount * 4, 144) && intact;
    intact = GuardRegion(CorrectionBase, constraintCount * 32, 1056) && intact;
    intact = GuardRegion(Integers, particleCount * 16, 528) && intact;
    intact = GuardRegion(7056, 0, 16) && intact;
    uint executed = project != 0 ? iterationCount : 0;
    for (uint record = 1; record < 67; ++record)
        if (record > executed && record != 65)
            intact = GuardRegion(MetricsBase + record * 32, 0, 32) && intact;
    Arena.Store(Status + 4, intact ? 1 : 0);
    Arena.Store(Status + 8, executed);
    if (!intact)
        Fail(11);
}

struct Vertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
Vertex Fullscreen(uint id : SV_VertexID)
{
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float SegmentDistance(float2 p, float2 a, float2 b)
{
    float2 delta = b - a;
    float t = saturate(dot(p - a, delta) / max(dot(delta, delta), 1.0e-12));
    return length(p - a - t * delta);
}
float2 Screen(float3 p)
{
    return float2(0.5 + p.x * 0.45, 0.62 - p.y * 0.45);
}
float4 Diagnostic(Vertex vertex) : SV_Target
{
    float2 uv = vertex.uv;
    if (Display.Load(Status) != 0 || Display.Load(Status + 4) != 1)
        return float4(0.9, 0.02, 0.03, 1);
    float3 color = project == 0 ? float3(0.04, 0.02, 0.07) : float3(0.015, 0.025, 0.04);
    // GPU metrics distinguish geometric error (orange) and compliant residual (cyan).
    float4 metrics = asfloat(Display.Load4(MetricsBase + 65 * 32));
    if (uv.y < 0.045 && uv.x < saturate(metrics.x * 20))
        color = float3(1, 0.35, 0.02);
    if (uv.y > 0.05 && uv.y < 0.09 && uv.x < saturate(metrics.z * 20))
        color = float3(0.05, 0.85, 1);
    if (uv.y > 0.095 && uv.y < 0.125 && uv.x < saturate(metrics.y * 20))
        color = float3(1, 0.15, 0.7);
    if (uv.y > 0.13 && uv.y < 0.15 && uv.x < saturate(metrics.w / 8))
        color = float3(0.4, 1, 0.1);
    if (view == 2 && project != 0)
    {
        uint iteration = min(uint(uv.x * iterationCount), iterationCount - 1);
        float4 evidence = asfloat(Display.Load4(MetricsBase + (iteration + 1) * 32));
        float graphY = 0.95 - min(evidence.z * 12, 0.2);
        if (abs(uv.y - graphY) < 0.006)
            color = float3(0.05, 0.85, 1);
        if (abs(uv.y - (0.95 - min(evidence.x * 12, 0.2))) < 0.004)
            color = float3(1, 0.35, 0.02);
    }
    for (uint edge = 0; edge < constraintCount; ++edge)
    {
        Constraint c = LoadConstraint(edge);
        float3 first = asfloat(Display.Load3(inputOffset + c.first * 32));
        float3 other = c.kind == 0 ? asfloat(Display.Load3(inputOffset + c.second * 32)) : c.anchor;
        if (SegmentDistance(uv, Screen(first), Screen(other)) < 0.003)
        {
            float3 edgeColor = c.color % 3 == 0 ? float3(1, 0.45, 0.08) :
                               c.color % 3 == 1 ? float3(0.05, 0.75, 1) : float3(1, 0.1, 0.7);
            if (view == 1)
            {
                float error = abs(length(first - other) - c.rest);
                edgeColor = lerp(float3(0.1, 0.9, 0.3), float3(1, 0.1, 0), saturate(error * 50));
            }
            color = edgeColor;
        }
    }
    for (uint index = 0; index < particleCount; ++index)
    {
        float3 current = asfloat(Display.Load3(inputOffset + index * 32));
        float3 predicted = asfloat(Display.Load3(Predicted + index * 32));
        float weight = asfloat(Display.Load(inputOffset + index * 32 + 12));
        if (SegmentDistance(uv, Screen(predicted), Screen(current)) < 0.002)
            color = float3(0.65, 0.15, 0.9);
        if (length(uv - Screen(predicted)) < 0.009)
            color = float3(0.35, 0.2, 0.45);
        if (length(uv - Screen(current)) < 0.006)
            color = weight == 0 ? float3(1, 0.1, 0.7) : float3(0.95, 0.95, 0.9);
    }
    // Strategy and actual free-motion/projected stage evidence, without a CPU image.
    if (uv.y > 0.98)
        color = project == 0 ? float3(0.5, 0.15, 0.6) :
                strategy == 0 ? float3(1, 0.45, 0.08) :
                strategy == 1 ? float3(0.05, 0.75, 1) : float3(0.3, 1, 0.2);
    return float4(color, 1);
}
