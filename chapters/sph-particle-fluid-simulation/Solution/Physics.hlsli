#define SPH_STAGE 5
static const float PI = 3.14159265358979323846;

// 3D support radius R; units W=m^-3, gradient=m^-4, Laplacian=m^-5.
float Poly6(float r)
{
    if (r >= radius) return 0;
    float q = 1 - r * r / (radius * radius);
    return 315 / (64 * PI * radius * radius * radius) * q * q * q;
}
float3 Poly6Gradient(float3 d)
{
    float r = length(d);
    if (r >= radius) return 0;
    float q = 1 - dot(d, d) / (radius * radius);
    return -945 / (32 * PI * pow(radius, 5)) * q * q * d;
}
float3 SpikyGradient(float3 d)
{
    float r = length(d);
    if (r == 0 || r >= radius) return 0;
    float q = 1 - r / radius;
    return -45 / (PI * pow(radius, 4)) * q * q * d / r;
}
float ViscosityLaplacian(float r)
{
    return r < radius ? 45 / (PI * pow(radius, 5)) * (1 - r / radius) : 0;
}

float DensityContribution(float mass, float r)
{
    return mass * Poly6(r);
}
float EquationOfState(float rho)
{
    float p = soundSpeed * soundSpeed * (rho - restDensity);
    return signedPressure != 0 ? p : max(p, 0);
}

float3 PairAcceleration(Particle a, Particle b, Sample sa, Sample sb)
{
    float3 d = a.position - b.position;
    float r = length(d);
    if (a.identity == b.identity || r >= radius) return 0;
    float3 pressure = -b.mass * (sa.pressure / (sa.density * sa.density) +
                               sb.pressure / (sb.density * sb.density)) * SpikyGradient(d);
    // Dynamic viscosity mu [kg/(m*s)]; F_ij=mu*m_i*m_j/(rho_i*rho_j)*(v_j-v_i)*lap.
    float3 viscosity = dynamicViscosity * b.mass / (sa.density * sb.density) *
                       (b.velocity - a.velocity) * ViscosityLaplacian(r);
    return pressure + viscosity;
}

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
    if (walls != 0)
    {
        for (uint axis = 0; axis < 3; ++axis)
        {
            if (p.position[axis] < -1.99 || p.position[axis] > 1.99)
            {
                p.position[axis] = clamp(p.position[axis], -1.99, 1.99);
                if ((p.position[axis] < 0 && p.velocity[axis] < 0) ||
                    (p.position[axis] > 0 && p.velocity[axis] > 0))
                    p.velocity[axis] *= -restitution;
                ++contacts;
            }
        }
    }
    if (any(p.position < -2) || any(p.position >= 2)) Fail(5);
}

// Serial cursor per owner, binary lower bound for each of the complete 27 neighboring cells.
bool NextCandidate(float3 position, inout uint cursor, inout uint record, out uint identity)
{
    identity = 0;
    if (useGrid == 0)
    {
        if (record >= count) return false;
        identity = record++;
        return true;
    }
    int3 home = int3(floor((position + 2) / radius));
    uint dimensions = uint(4 / radius);
    // The snapshot already rejected invalid positions; match the last-cell rounding policy.
    home = min(home, int3(dimensions - 1, dimensions - 1, dimensions - 1));
    while (cursor < 27)
    {
        int3 offset = int3(int(cursor % 3) - 1, int((cursor / 3) % 3) - 1, int(cursor / 9) - 1);
        int3 cell = home + offset;
        if (any(cell < 0) || any(cell >= int(dimensions)))
        {
            ++cursor;
            record = 0xffffffff;
            continue;
        }
        uint key = uint(cell.x) + dimensions * (uint(cell.y) + dimensions * uint(cell.z));
        if (record == 0xffffffff)
        {
            uint lo = 0, hi = count;
            while (lo < hi)
            {
                uint mid = (lo + hi) / 2;
                if (arena.Load(KEYS + mid * 8) < key) lo = mid + 1;
                else hi = mid;
            }
            record = lo;
        }
        if (record < count && arena.Load(KEYS + record * 8) == key)
        {
            identity = arena.Load(KEYS + record++ * 8 + 4);
            return true;
        }
        ++cursor;
        record = 0xffffffff;
    }
    return false;
}
