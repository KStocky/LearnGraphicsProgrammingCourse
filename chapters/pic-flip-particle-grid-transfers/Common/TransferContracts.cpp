#include "TransferContracts.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace ch43::transfers
{
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
bool Finite(Vec3 a) noexcept
{
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
float Component(Vec3 a, std::uint32_t axis) noexcept
{
    if (axis == 0U)
    {
        return a.x;
    }
    return axis == 1U ? a.y : a.z;
}
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
    case Error::NoSupport:
        return "no-support";
    case Error::Topology:
        return "topology";
    case Error::Identity:
        return "identity";
    case Error::AtomicLimit:
        return "atomic-limit";
    case Error::Guard:
        return "guard";
    }
    return "unknown-error";
}
Scene MakeBlock()
{
    Scene result{};
    for (std::uint32_t i = 0U; i < 32U; ++i)
    {
        float const x = -0.35F + 0.1F * static_cast<float>(i % 4U);
        float const y = -0.3F + 0.13F * static_cast<float>((i / 4U) % 4U);
        float const z = -0.1F + 0.17F * static_cast<float>(i / 16U);
        float const noise = i % 2U == 0U ? 1.0F : -1.0F;
        result.push_back(
            {{x, y, z}, 1.0F + 0.25F * static_cast<float>(i % 3U), {noise, 0.5F * noise, -0.25F * noise}, i});
    }
    return result;
}
Vec3 FacePosition(std::uint32_t face) noexcept
{
    std::uint32_t const axis = face / 80U;
    std::uint32_t const nx = axis == 0U ? 5U : 4U;
    std::uint32_t const ny = axis == 1U ? 5U : 4U;
    std::uint32_t const key = face % 80U;
    return {-1.0F + 0.5F * (static_cast<float>(key % nx) + (axis == 0U ? 0.0F : 0.5F)),
            -1.0F + 0.5F * (static_cast<float>((key / nx) % ny) + (axis == 1U ? 0.0F : 0.5F)),
            -1.0F + 0.5F * (static_cast<float>(key / (nx * ny)) + (axis == 2U ? 0.0F : 0.5F))};
}
std::expected<Stencil, Error> Support(Vec3 position, std::uint32_t axis)
{
    if (!Finite(position))
    {
        return std::unexpected(Error::NonFinite);
    }
    if (axis > 2U)
    {
        return std::unexpected(Error::Parameters);
    }
    // Independent double-precision oracle: enumerate every face, not the shader's eight-corner loop.
    Stencil result{};
    double sum{};
    for (std::uint32_t face = axis * 80U; face < (axis + 1U) * 80U; ++face)
    {
        Vec3 const center = FacePosition(face);
        double weight = 1.0;
        for (std::uint32_t d = 0U; d < 3U; ++d)
        {
            double const distance = std::abs(static_cast<double>(Component(position, d)) - Component(center, d));
            weight *= std::max(0.0, 1.0 - distance / 0.5);
        }
        if (weight > 0.0)
        {
            result.push_back({face, weight});
            sum += weight;
        }
    }
    if (sum <= 0.0)
    {
        return std::unexpected(Error::NoSupport);
    }
    for (auto &entry : result)
    {
        entry.weight /= sum;
    }
    return result;
}
std::expected<void, Error> Validate(std::span<Particle const> particles, Configuration const &c)
{
    if (particles.empty() || particles.size() > kCapacity)
    {
        return std::unexpected(Error::Capacity);
    }
    if (!std::isfinite(c.alpha) || !Finite(c.increment))
    {
        return std::unexpected(Error::NonFinite);
    }
    if (c.alpha < 0 || c.alpha > 1 || std::abs(c.increment.x) > 10 || std::abs(c.increment.y) > 10 ||
        std::abs(c.increment.z) > 10 || c.field > 4U || c.view > 3U)
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
        if (p.mass < 0.0001F || p.mass > 1000.0F)
        {
            return std::unexpected(Error::Mass);
        }
        if (p.position.x < -1 || p.position.x >= 1 || p.position.y < -1 || p.position.y >= 1 || p.position.z < -1 ||
            p.position.z >= 1)
        {
            return std::unexpected(Error::Domain);
        }
        if (std::abs(p.velocity.x) > 100 || std::abs(p.velocity.y) > 100 || std::abs(p.velocity.z) > 100)
        {
            return std::unexpected(Error::Parameters);
        }
        if (p.identity != i)
        {
            return std::unexpected(Error::Identity);
        }
    }
    return {};
}
State Reset(Scene const &scene)
{
    State result{};
    result.particles = scene;
    result.snapshot = scene;
    result.samples.resize(scene.size());
    return result;
}
std::expected<double, Error> Interpolate(Stencil const &stencil, std::span<Face const> faces, bool delta)
{
    double sum{};
    double value{};
    for (auto const &entry : stencil)
    {
        if (entry.face >= faces.size())
        {
            return std::unexpected(Error::Parameters);
        }
        auto const &f = faces[entry.face];
        if (delta && f.beforeValid != f.afterValid)
        {
            return std::unexpected(Error::Topology);
        }
        if (f.afterValid == 0U || (delta && f.beforeValid == 0U))
        {
            continue;
        }
        double const v = delta ? static_cast<double>(f.after) - f.before : f.after;
        if (!std::isfinite(v))
        {
            return std::unexpected(Error::NonFinite);
        }
        sum += entry.weight;
        value += entry.weight * v;
    }
    if (sum <= 0.0)
    {
        return std::unexpected(Error::NoSupport);
    }
    return value / sum;
}
std::expected<void, Error> Advance(Configuration const &c, Stage stage, State &s)
{
    auto valid = Validate(s.particles, c);
    if (!valid)
    {
        return valid;
    }
    if (stage > Stage::Flip)
    {
        return std::unexpected(Error::Parameters);
    }
    s.snapshot = s.particles;
    s.faces = {};
    s.metrics = {};
    s.metrics.supportMin = stage >= Stage::Support ? 1.0F : 0.0F;
    std::array<double, kFaces> masses{};
    std::array<double, kFaces> momenta{};
    for (std::size_t i = 0U; i < s.particles.size(); ++i)
    {
        auto const &p = s.snapshot[i];
        s.samples[i] = {};
        if (stage < Stage::Support)
        {
            continue;
        }
        s.samples[i].support = 1.0F;
        for (std::uint32_t axis = 0U; axis < 3U; ++axis)
        {
            auto stencil = Support(p.position, axis);
            if (!stencil)
            {
                return std::unexpected(stencil.error());
            }
            s.samples[i].entries += static_cast<std::uint32_t>(stencil->size());
            if (stage < Stage::Scatter)
            {
                continue;
            }
            for (auto const &entry : *stencil)
            {
                double const m = p.mass * entry.weight;
                masses[entry.face] += m;
                momenta[entry.face] += m * Component(p.velocity, axis);
            }
        }
    }
    for (std::uint32_t face = 0U; face < kFaces; ++face)
    {
        auto &f = s.faces[face];
        f.mass = static_cast<float>(masses[face]);
        f.momentum = static_cast<float>(momenta[face]);
        if (stage < Stage::Normalize || f.mass <= 0)
        {
            continue;
        }
        std::uint32_t const axis = face / 80U;
        Vec3 const position = FacePosition(face);
        float const constant = static_cast<float>(axis + 1U);
        f.before = static_cast<float>(momenta[face] / masses[face]);
        if (c.field == 1U)
        {
            f.before = constant;
        }
        if (c.field == 2U)
        {
            f.before = constant + 0.3F * position.x - 0.2F * position.y + 0.1F * position.z;
        }
        f.after = f.before + Component(c.increment, axis);
        f.beforeValid = c.field == 4U ? 0U : 1U;
        f.afterValid = c.field >= 3U ? 0U : 1U;
    }
    if (stage >= Stage::Pic)
    {
        for (std::size_t i = 0U; i < s.particles.size(); ++i)
        {
            std::array<float, 3U> pic{};
            std::array<float, 3U> delta{};
            for (std::uint32_t axis = 0U; axis < 3U; ++axis)
            {
                auto stencil = Support(s.snapshot[i].position, axis);
                if (!stencil)
                {
                    return std::unexpected(stencil.error());
                }
                // FLIP topology validation precedes PIC's no-support rejection.
                if (stage == Stage::Flip)
                {
                    auto change = Interpolate(*stencil, s.faces, true);
                    if (!change)
                    {
                        return std::unexpected(change.error());
                    }
                    delta[axis] = static_cast<float>(*change);
                }
                auto value = Interpolate(*stencil, s.faces, false);
                if (!value)
                {
                    return std::unexpected(value.error());
                }
                pic[axis] = static_cast<float>(*value);
            }
            auto &p = s.particles[i];
            s.samples[i].pic = {pic[0], pic[1], pic[2]};
            s.samples[i].delta = {delta[0], delta[1], delta[2]};
            p.velocity = stage == Stage::Flip ? s.samples[i].pic * (1 - c.alpha) +
                                                    (s.snapshot[i].velocity + s.samples[i].delta) * c.alpha
                                              : s.samples[i].pic;
            if (!Finite(p.velocity))
            {
                return std::unexpected(Error::NonFinite);
            }
            if (std::abs(p.velocity.x) > 100 || std::abs(p.velocity.y) > 100 || std::abs(p.velocity.z) > 100)
            {
                return std::unexpected(Error::Parameters);
            }
        }
    }
    std::array<float, 3U> mass{};
    std::array<float, 3U> momentum{};
    for (std::uint32_t f = 0U; f < kFaces; ++f)
    {
        mass[f / 80U] += s.faces[f].mass;
        momentum[f / 80U] += s.faces[f].momentum;
        s.metrics.occupied += s.faces[f].beforeValid;
    }
    s.metrics.familyMass = {mass[0], mass[1], mass[2]};
    s.metrics.familyMomentum = {momentum[0], momentum[1], momentum[2]};
    for (std::size_t i = 0U; i < s.particles.size(); ++i)
    {
        auto const &p = s.particles[i];
        s.metrics.kinetic += 0.5F * p.mass * Dot(p.velocity, p.velocity);
        s.metrics.inputKinetic += 0.5F * p.mass * Dot(s.snapshot[i].velocity, s.snapshot[i].velocity);
        s.metrics.particleMomentum = s.metrics.particleMomentum + p.velocity * p.mass;
        s.metrics.entries += s.samples[i].entries;
    }
    s.metrics.round = ++s.rounds;
    return {};
}
} // namespace ch43::transfers
