#define SPH_STAGE 0
static const float PI = 3.14159265358979323846;

// Stage 1: replace the zero probes with the support-radius kernels.
float Poly6(float r)
{
    return 0;
}
float3 Poly6Gradient(float3 d)
{
    return 0;
}
float3 SpikyGradient(float3 d)
{
    return 0;
}
float ViscosityLaplacian(float r)
{
    return 0;
}

// Stage 2: include each accepted mass, including self, and convert density to pressure.
float DensityContribution(float mass, float r)
{
    return 0;
}
float EquationOfState(float rho)
{
    return 0;
}

// Stage 3: gather pair pressure and dynamic-viscosity acceleration.
float3 PairAcceleration(Particle a, Particle b, Sample sa, Sample sb)
{
    return 0;
}

// Supplied gravity-only baseline. Stage 4 adds explicit projection/restitution.
void IntegrateParticle(inout Particle p, float3 acceleration, out float motion, out uint contacts)
{
    p.velocity += (acceleration + gravity) * dt;
    float3 displacement = p.velocity * dt;
    motion = length(displacement) / radius;
    contacts = 0;
    if (!all(isfinite(p.velocity)) || !all(isfinite(displacement)))
    {
        Fail(3);
        return;
    }
    if (motion > motionLimit)
    {
        Fail(7);
        return;
    }
    p.position += displacement;
    if (any(p.position < -2) || any(p.position >= 2)) Fail(5);
}

// Stage 5: replace this complete O(N) query with the sorted-cell cursor.
bool NextCandidate(float3 position, inout uint cursor, inout uint record, out uint identity)
{
    identity = 0;
    if (record >= count) return false;
    identity = record++;
    return true;
}
