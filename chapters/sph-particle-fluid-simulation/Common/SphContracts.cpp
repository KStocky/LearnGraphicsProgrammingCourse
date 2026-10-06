#include "SphContracts.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ch42::sph
{
std::string_view ErrorName(Error error) noexcept
{
    switch (error)
    {
    case Error::Capacity:
        return "capacity";
    case Error::Parameters:
        return "parameters";
    case Error::NonFinite:
        return "nonfinite";
    case Error::Mass:
        return "mass";
    case Error::Domain:
        return "domain";
    case Error::Density:
        return "density";
    case Error::UnsafeMotion:
        return "unsafe-motion";
    case Error::Identity:
        return "identity";
    case Error::Guard:
        return "guard";
    }
    return "unknown";
}
Vec3 operator+(Vec3 a, Vec3 b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 operator-(Vec3 a, Vec3 b) noexcept
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 operator*(Vec3 a, float b) noexcept
{
    return {a.x * b, a.y * b, a.z * b};
}
float Dot(Vec3 a, Vec3 b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
float Length(Vec3 a) noexcept
{
    return std::sqrt(Dot(a, a));
}
bool Finite(Vec3 a) noexcept
{
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
namespace
{
bool Domain(Vec3 p) noexcept
{
    return p.x >= -2.0F && p.y >= -2.0F && p.z >= -2.0F && p.x < 2.0F && p.y < 2.0F && p.z < 2.0F;
}
std::expected<void, Error> KernelArguments(float r, float radius) noexcept
{
    if (!std::isfinite(r) || !std::isfinite(radius))
    {
        return std::unexpected(Error::NonFinite);
    }
    if (r < 0.0F || radius < 0.125F || radius > 0.5F)
    {
        return std::unexpected(Error::Parameters);
    }
    return {};
}
} // namespace
Scene MakeBlock()
{
    Scene scene{};
    for (std::uint32_t z = 0U; z < 4U; ++z)
    {
        for (std::uint32_t y = 0U; y < 4U; ++y)
        {
            for (std::uint32_t x = 0U; x < 4U; ++x)
            {
                scene.push_back({{(static_cast<float>(x) - 1.5F) * 0.125F, 0.8F + static_cast<float>(y) * 0.125F,
                                  (static_cast<float>(z) - 1.5F) * 0.125F},
                                 1.953125F,
                                 {},
                                 static_cast<std::uint32_t>(scene.size())});
            }
        }
    }
    return scene;
}
std::expected<void, Error> Validate(std::span<Particle const> particles, Configuration const &c)
{
    if (particles.empty() || particles.size() > kCapacity)
    {
        return std::unexpected(Error::Capacity);
    }
    if (!std::isfinite(c.radius) || !std::isfinite(c.restDensity) || !std::isfinite(c.soundSpeed) ||
        !std::isfinite(c.dynamicViscosity) || !std::isfinite(c.tick) || !std::isfinite(c.restitution) ||
        !std::isfinite(c.motionLimit) || !Finite(c.gravity))
    {
        return std::unexpected(Error::NonFinite);
    }
    if ((c.radius != 0.125F && c.radius != 0.25F && c.radius != 0.5F) || c.restDensity < 1.0F ||
        c.restDensity > 10000.0F || c.soundSpeed <= 0.0F || c.soundSpeed > 100.0F || c.dynamicViscosity < 0.0F ||
        c.dynamicViscosity > 100.0F || c.tick < 1.0e-6F || c.tick > 0.1F || c.restitution < 0.0F ||
        c.restitution > 1.0F || c.motionLimit <= 0.0F || c.motionLimit > 1.0F || c.substeps == 0U || c.substeps > 16U ||
        c.view > 3U || Length(c.gravity) > 100.0F)
    {
        return std::unexpected(Error::Parameters);
    }
    for (std::size_t i = 0U; i < particles.size(); ++i)
    {
        auto const &p = particles[i];
        if (!Finite(p.position) || !Finite(p.velocity) || !std::isfinite(p.mass))
        {
            return std::unexpected(Error::NonFinite);
        }
        if (p.mass < 1.0e-6F || p.mass > 1000.0F)
        {
            return std::unexpected(Error::Mass);
        }
        if (!Domain(p.position))
        {
            return std::unexpected(Error::Domain);
        }
        if (p.identity != i)
        {
            return std::unexpected(Error::Identity);
        }
    }
    return {};
}
std::expected<float, Error> Poly6(float r, float radius) noexcept
{
    auto valid = KernelArguments(r, radius);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (r >= radius)
    {
        return 0.0F;
    }
    float const q = 1.0F - (r * r) / (radius * radius);
    return 315.0F / (64.0F * std::numbers::pi_v<float> * radius * radius * radius) * q * q * q;
}
std::expected<Vec3, Error> Poly6Gradient(Vec3 d, float radius) noexcept
{
    if (!Finite(d))
    {
        return std::unexpected(Error::NonFinite);
    }
    auto w = Poly6(Length(d), radius);
    if (!w)
    {
        return std::unexpected(w.error());
    }
    float const r = Length(d);
    if (r >= radius)
    {
        return Vec3{};
    }
    float const q = 1.0F - Dot(d, d) / (radius * radius);
    float const scale = -945.0F / (32.0F * std::numbers::pi_v<float> * std::pow(radius, 5.0F));
    return d * (scale * q * q);
}
std::expected<Vec3, Error> SpikyGradient(Vec3 d, float radius) noexcept
{
    if (!Finite(d))
    {
        return std::unexpected(Error::NonFinite);
    }
    float const r = Length(d);
    auto valid = KernelArguments(r, radius);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (r == 0.0F || r >= radius)
    {
        return Vec3{};
    }
    float const q = 1.0F - r / radius;
    return d * (-45.0F / (std::numbers::pi_v<float> * std::pow(radius, 4.0F)) * q * q / r);
}
std::expected<float, Error> ViscosityLaplacian(float r, float radius) noexcept
{
    auto valid = KernelArguments(r, radius);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (r >= radius)
    {
        return 0.0F;
    }
    return 45.0F / (std::numbers::pi_v<float> * std::pow(radius, 5.0F)) * (1.0F - r / radius);
}
std::expected<float, Error> Pressure(float density, Configuration const &c) noexcept
{
    if (!std::isfinite(density))
    {
        return std::unexpected(Error::NonFinite);
    }
    if (density <= 0.0F)
    {
        return std::unexpected(Error::Density);
    }
    if (!std::isfinite(c.soundSpeed) || !std::isfinite(c.restDensity) || c.soundSpeed <= 0.0F || c.restDensity <= 0.0F)
    {
        return std::unexpected(Error::Parameters);
    }
    float const p = c.soundSpeed * c.soundSpeed * (density - c.restDensity);
    if (!std::isfinite(p))
    {
        return std::unexpected(Error::NonFinite);
    }
    return c.signedPressure ? p : std::max(p, 0.0F);
}
std::expected<Vec3, Error> PairForce(Particle const &a, Particle const &b, Sample const &sa, Sample const &sb,
                                     Configuration const &c) noexcept
{
    if (!Finite(a.position) || !Finite(b.position) || !Finite(a.velocity) || !Finite(b.velocity) ||
        !std::isfinite(a.mass) || !std::isfinite(b.mass) || !std::isfinite(sa.density) || !std::isfinite(sb.density) ||
        !std::isfinite(sa.pressure) || !std::isfinite(sb.pressure))
    {
        return std::unexpected(Error::NonFinite);
    }
    if (a.mass <= 0.0F || b.mass <= 0.0F)
    {
        return std::unexpected(Error::Mass);
    }
    if (sa.density <= 0.0F || sb.density <= 0.0F)
    {
        return std::unexpected(Error::Density);
    }
    if (!std::isfinite(c.dynamicViscosity) || c.dynamicViscosity < 0.0F)
    {
        return std::unexpected(Error::Parameters);
    }
    if (!std::isfinite(sa.density * sa.density) || !std::isfinite(sb.density * sb.density) ||
        !std::isfinite(sa.density * sb.density))
    {
        return std::unexpected(Error::NonFinite);
    }
    Vec3 const d = a.position - b.position;
    float const r = Length(d);
    auto gradient = SpikyGradient(d, c.radius);
    auto lap = ViscosityLaplacian(r, c.radius);
    if (!gradient)
    {
        return std::unexpected(gradient.error());
    }
    if (!lap)
    {
        return std::unexpected(lap.error());
    }
    // Coincident pressure has zero direction; viscosity still damps a velocity difference.
    if (a.identity == b.identity)
    {
        return Vec3{};
    }
    Vec3 const pressure =
        *gradient *
        (-a.mass * b.mass * (sa.pressure / (sa.density * sa.density) + sb.pressure / (sb.density * sb.density)));
    Vec3 const viscosity =
        (b.velocity - a.velocity) * (c.dynamicViscosity * a.mass * b.mass / (sa.density * sb.density) * *lap);
    Vec3 const force = pressure + viscosity;
    if (!Finite(force))
    {
        return std::unexpected(Error::NonFinite);
    }
    return force;
}
State Reset(Scene const &scene)
{
    State s{};
    s.particles = scene;
    s.samples.resize(scene.size());
    s.acceptedMasks.resize(scene.size());
    return s;
}
std::expected<void, Error> Advance(Configuration const &c, Stage stage, State &s)
{
    if (static_cast<std::uint32_t>(stage) > static_cast<std::uint32_t>(Stage::Grid))
    {
        return std::unexpected(Error::Parameters);
    }
    auto valid = Validate(s.particles, c);
    if (!valid)
    {
        return valid;
    }
    if (c.paused)
    {
        return {};
    }
    float const dt = c.tick / static_cast<float>(c.substeps);
    for (std::uint32_t step = 0U; step < c.substeps; ++step)
    {
        auto const snapshot = s.particles;
        s.samples.assign(snapshot.size(), {});
        s.acceptedMasks.assign(snapshot.size(), {});
        for (std::size_t i = 0U; i < snapshot.size(); ++i)
        {
            auto &sample = s.samples[i];
            for (std::size_t j = 0U; j < snapshot.size(); ++j)
            {
                ++sample.candidates;
                float const r = Length(snapshot[i].position - snapshot[j].position);
                if (r >= c.radius)
                {
                    continue;
                }
                ++sample.accepted;
                s.acceptedMasks[i][j / 32U] |= 1U << (j % 32U);
                if (i != j && r == 0.0F)
                {
                    ++sample.coincident;
                }
                if (stage >= Stage::Density)
                {
                    sample.density += snapshot[j].mass * *Poly6(r, c.radius);
                }
            }
            if (stage >= Stage::Density)
            {
                auto p = Pressure(sample.density, c);
                if (!p)
                {
                    return std::unexpected(p.error());
                }
                sample.pressure = *p;
            }
        }
        if (stage >= Stage::Forces)
        {
            for (std::size_t i = 0U; i < snapshot.size(); ++i)
            {
                for (std::size_t j = 0U; j < snapshot.size(); ++j)
                {
                    auto force = PairForce(snapshot[i], snapshot[j], s.samples[i], s.samples[j], c);
                    if (!force)
                    {
                        return std::unexpected(force.error());
                    }
                    s.samples[i].acceleration = s.samples[i].acceleration + *force * (1.0F / snapshot[i].mass);
                }
            }
        }
        std::uint32_t contacts{};
        float motion{};
        for (std::size_t i = 0U; i < snapshot.size(); ++i)
        {
            auto p = snapshot[i];
            p.velocity = p.velocity + (s.samples[i].acceleration + c.gravity) * dt;
            Vec3 const displacement = p.velocity * dt;
            if (!Finite(p.velocity) || !Finite(displacement))
            {
                return std::unexpected(Error::NonFinite);
            }
            if (Length(displacement) > c.motionLimit * c.radius)
            {
                return std::unexpected(Error::UnsafeMotion);
            }
            motion = std::max(motion, Length(displacement) / c.radius);
            p.position = p.position + displacement;
            if (stage >= Stage::Contact && c.walls)
            {
                std::array<float *, 3U> coordinates{&p.position.x, &p.position.y, &p.position.z};
                std::array<float *, 3U> velocities{&p.velocity.x, &p.velocity.y, &p.velocity.z};
                for (std::size_t axis = 0U; axis < 3U; ++axis)
                {
                    float &x = *coordinates[axis];
                    float &v = *velocities[axis];
                    if (x < -1.99F || x > 1.99F)
                    {
                        x = std::clamp(x, -1.99F, 1.99F);
                        if ((x < 0.0F && v < 0.0F) || (x > 0.0F && v > 0.0F))
                        {
                            v *= -c.restitution;
                        }
                        ++contacts;
                    }
                }
            }
            if (!Domain(p.position))
            {
                return std::unexpected(Error::Domain);
            }
            s.particles[i] = p;
        }
        ++s.completedSubsteps;
        auto &m = s.metrics;
        m = {};
        m.densityMin = s.samples[0].density;
        m.pressureMin = s.samples[0].pressure;
        m.candidateMin = static_cast<std::uint32_t>(snapshot.size());
        Vec3 momentum{};
        for (std::size_t i = 0U; i < snapshot.size(); ++i)
        {
            auto const &sample = s.samples[i];
            auto const &p = s.particles[i];
            m.densityMin = std::min(m.densityMin, sample.density);
            m.densityMax = std::max(m.densityMax, sample.density);
            m.pressureMin = std::min(m.pressureMin, sample.pressure);
            m.pressureMax = std::max(m.pressureMax, sample.pressure);
            m.speed = std::max(m.speed, Length(p.velocity));
            m.acceleration = std::max(m.acceleration, Length(sample.acceleration));
            m.candidates += sample.candidates;
            m.accepted += sample.accepted;
            m.coincident += sample.coincident;
            m.candidateMax = std::max(m.candidateMax, sample.candidates);
            m.kinetic += 0.5F * p.mass * Dot(p.velocity, p.velocity);
            momentum = momentum + p.velocity * p.mass;
        }
        m.kernelProbe = stage >= Stage::Kernels ? *Poly6(0.0F, c.radius) : 0.0F;
        m.deficiency = stage >= Stage::Density ? std::max(0.0F, 1.0F - m.densityMin / c.restDensity) : 0.0F;
        m.soundRatio = c.soundSpeed * dt / c.radius;
        m.motionRatio = motion;
        m.contacts = contacts;
        m.warnings = (m.soundRatio > 0.1F ? 1U : 0U) | (motion > 0.1F ? 2U : 0U);
        m.logicalLoads = m.candidates * (stage >= Stage::Forces ? 2U : 1U);
        m.momentumMagnitude = Length(momentum);
    }
    ++s.ticks;
    return {};
}
} // namespace ch42::sph
