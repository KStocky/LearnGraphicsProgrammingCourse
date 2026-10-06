// Reader-owned transfer math. Addressing never precedes geometric clipping.
struct Stencil { uint face[8]; float weight[8]; uint count; };
Stencil Support(float3 position, uint axis)
{
    Stencil s = (Stencil)0;
    float3 offset = 0.5;
    offset[axis] = 0;
    float3 q = (position + 1) / 0.5 - offset;
    int3 base = int3(floor(q));
    float3 fraction = q - floor(q);
    int3 shape = 4;
    shape[axis] = 5;
    float sum = 0;
    for (uint corner = 0; corner < 8; ++corner)
    {
        int3 bit = int3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
        int3 index = base + bit;
        float3 tent = lerp(1 - fraction, fraction, float3(bit));
        float w = tent.x * tent.y * tent.z;
        if (any(index < 0) || any(index >= shape) || w <= 0) continue;
        s.face[s.count] = axis * 80 + uint(index.x + shape.x * (index.y + shape.y * index.z));
        s.weight[s.count++] = w;
        sum += w;
    }
    if (sum <= 0) { Fail(6); return s; }
    for (uint j = 0; j < s.count; ++j) s.weight[j] /= sum;
    return s;
}
void AddFloat(uint address, float value)
{
    uint expected = AtomicLoad(address);
    for (uint retry = 0; retry < 4096; ++retry)
    {
        float sum = asfloat(expected) + value;
        if (!isfinite(sum)) { Fail(3); return; }
        uint observed, unused;
        arena.InterlockedAdd(STATUS + 12, 1, unused);
        arena.InterlockedCompareExchange(address, expected, asuint(sum), observed);
        if (observed == expected) return;
        expected = observed;
    }
    Fail(9);
}
void ScatterParticle(Particle p, uint axis, Stencil s)
{
    for (uint j = 0; j < s.count; ++j)
    {
        float m = p.mass * s.weight[j];
        AddFloat(FACES + s.face[j] * 32, m);
        AddFloat(FACES + s.face[j] * 32 + 4, m * p.velocity[axis]);
    }
}
void NormalizeFace(uint face)
{
    uint address = FACES + face * 32;
    float mass = asfloat(arena.Load(address));
    if (mass <= 0) return; // Clear explicitly marked this face invalid.
    float momentum = asfloat(arena.Load(address + 4));
    uint axis = face / 80;
    float before = momentum / mass;
    if (field == 1) before = float(axis + 1);
    if (field == 2)
    {
        float3 p = FacePosition(face);
        before = float(axis + 1) + 0.3 * p.x - 0.2 * p.y + 0.1 * p.z;
    }
    float after = before + increment[axis];
    if (!isfinite(before) || !isfinite(after)) { Fail(3); return; }
    arena.Store2(address + 8, asuint(float2(before, after)));
    arena.Store2(address + 16, uint2(field == 4 ? 0 : 1, field >= 3 ? 0 : 1));
}
float Gather(Stencil s, bool delta)
{
    float value = 0, sum = 0;
    for (uint j = 0; j < s.count; ++j)
    {
        uint address = FACES + s.face[j] * 32;
        uint2 valid = arena.Load2(address + 16);
        if (delta && valid.x != valid.y) { Fail(7); return 0; }
        if (valid.y == 0 || (delta && valid.x == 0)) continue;
        float2 velocity = asfloat(arena.Load2(address + 8));
        value += s.weight[j] * (delta ? velocity.y - velocity.x : velocity.y);
        sum += s.weight[j];
    }
    if (sum <= 0) { Fail(6); return 0; }
    return value / sum;
}
float3 TransferVelocity(Particle p, float3 pic, float3 delta)
{
    return (1 - alpha) * pic + alpha * (p.velocity + delta);
}
