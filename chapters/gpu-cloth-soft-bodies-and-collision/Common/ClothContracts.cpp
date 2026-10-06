#include "ClothContracts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <span>

namespace ch41::cloth
{
Vector3 operator+(Vector3 a, Vector3 b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vector3 operator-(Vector3 a, Vector3 b) noexcept
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vector3 operator*(Vector3 a, float b) noexcept
{
    return {a.x * b, a.y * b, a.z * b};
}
float Dot(Vector3 a, Vector3 b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vector3 Cross(Vector3 a, Vector3 b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Length(Vector3 a) noexcept
{
    return std::sqrt(Dot(a, a));
}
bool Finite(Vector3 a) noexcept
{
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
std::expected<MechanicalEnergy, Error> MeasureMechanicalEnergy(std::span<Particle const> particles, Vector3 gravity)
{
    if (particles.empty() || particles.size() > kMaxParticles)
    {
        return std::unexpected(Error::InvalidTopology);
    }
    if (!Finite(gravity) || Length(gravity) > 100.0F)
    {
        return std::unexpected(Error::InvalidParameter);
    }
    MechanicalEnergy energy{};
    for (auto const &p : particles)
    {
        if (!Finite(p.position) || !Finite(p.velocity))
        {
            return std::unexpected(Error::NonFinite);
        }
        if (!std::isfinite(p.inverseMass) || p.inverseMass < 0.0F || p.inverseMass > 100.0F ||
            (p.inverseMass > 0.0F && p.inverseMass < std::numeric_limits<float>::min()))
        {
            return std::unexpected(Error::InvalidParameter);
        }
        if (p.inverseMass == 0.0F)
        {
            continue;
        }
        double const mass = 1.0 / static_cast<double>(p.inverseMass);
        double const vx = p.velocity.x;
        double const vy = p.velocity.y;
        double const vz = p.velocity.z;
        energy.kinetic += 0.5 * mass * (vx * vx + vy * vy + vz * vz);
        energy.gravitationalPotential -=
            mass * (static_cast<double>(gravity.x) * p.position.x + static_cast<double>(gravity.y) * p.position.y +
                    static_cast<double>(gravity.z) * p.position.z);
    }
    energy.total = energy.kinetic + energy.gravitationalPotential;
    if (!std::isfinite(energy.total))
    {
        return std::unexpected(Error::NonFinite);
    }
    return energy;
}
std::uint32_t Arity(Kind kind) noexcept
{
    if (kind == Kind::Volume)
    {
        return 4U;
    }
    return kind == Kind::Area ? 3U : 2U;
}
std::string_view ErrorName(Error error) noexcept
{
    constexpr std::array<std::string_view, 11U> names{
        "invalid topology",    "invalid rest state",        "invalid parameter",    "mutable endpoint ownership",
        "singular constraint", "signed volume inverted",    "substep motion bound", "grid domain",
        "non-finite result",   "unsolvable pinned contact", "guard corruption"};
    auto const index = static_cast<std::size_t>(error) - 1U;
    return index < names.size() ? names[index] : "unknown error";
}
std::expected<Evaluation, Error> Evaluate(Constraint const &c, std::vector<Particle> const &p)
{
    Evaluation e{};
    auto const a = p[c.vertices[0]].position;
    auto const b = p[c.vertices[1]].position;
    if (c.kind <= Kind::Bending)
    {
        auto const delta = a - b;
        float const length = Length(delta);
        if (length < 1.0e-7F)
        {
            return std::unexpected(Error::Singular);
        }
        e.value = length;
        e.gradients[0] = delta * (1.0F / length);
        e.gradients[1] = e.gradients[0] * -1.0F;
    }
    else if (c.kind == Kind::Area)
    {
        auto const d = p[c.vertices[2]].position;
        auto const cross = Cross(b - a, d - a);
        float const length = Length(cross);
        if (length < 1.0e-7F)
        {
            return std::unexpected(Error::Singular);
        }
        auto const n = cross * (1.0F / length);
        e.value = length * 0.5F;
        e.gradients[1] = Cross(d - a, n) * 0.5F;
        e.gradients[2] = Cross(n, b - a) * 0.5F;
        e.gradients[0] = (e.gradients[1] + e.gradients[2]) * -1.0F;
    }
    else
    {
        auto const d = p[c.vertices[2]].position;
        auto const f = p[c.vertices[3]].position;
        e.value = Dot(b - a, Cross(d - a, f - a)) / 6.0F;
        if (e.value <= 1.0e-8F)
        {
            return std::unexpected(Error::Inverted);
        }
        e.gradients[1] = Cross(d - a, f - a) * (1.0F / 6.0F);
        e.gradients[2] = Cross(f - a, b - a) * (1.0F / 6.0F);
        e.gradients[3] = Cross(b - a, d - a) * (1.0F / 6.0F);
        e.gradients[0] = (e.gradients[1] + e.gradients[2] + e.gradients[3]) * -1.0F;
    }
    if (!std::isfinite(e.value))
    {
        return std::unexpected(Error::NonFinite);
    }
    return e;
}
namespace
{
bool SharesMutable(Constraint const &a, Constraint const &b, Scene const &s)
{
    for (std::uint32_t i = 0U; i < Arity(a.kind); ++i)
    {
        for (std::uint32_t j = 0U; j < Arity(b.kind); ++j)
        {
            if (a.vertices[i] == b.vertices[j] && s.particles[a.vertices[i]].inverseMass > 0.0F)
            {
                return true;
            }
        }
    }
    return false;
}
template <typename T> bool Contains(T const &vertices, std::uint32_t a, std::uint32_t b)
{
    return std::ranges::find(vertices, a) != vertices.end() && std::ranges::find(vertices, b) != vertices.end();
}
void Add(Scene &s, Kind kind, std::array<std::uint32_t, 4U> vertices)
{
    Constraint c{};
    c.vertices = vertices;
    c.kind = kind;
    c.compliance = kDistanceComplianceBase;
    if (kind == Kind::Area)
    {
        c.compliance = kAreaComplianceBase;
    }
    else if (kind == Kind::Volume)
    {
        c.compliance = kVolumeComplianceBase;
    }
    c.rest = Evaluate(c, s.particles)->value;
    s.constraints.push_back(c);
}
void Topology(Scene const &s, State &state)
{
    for (std::size_t i = 0U; i < s.constraints.size(); ++i)
    {
        auto const &c = s.constraints[i];
        if (state.active[i] != 0U || c.kind > Kind::Shear)
        {
            continue;
        }
        for (std::size_t t = 0U; t < s.triangles.size(); ++t)
        {
            if (Contains(s.triangles[t].vertices, c.vertices[0], c.vertices[1]))
            {
                state.faces[t] = 0U;
            }
        }
        for (std::size_t t = 0U; t < s.tetrahedra.size(); ++t)
        {
            if (Contains(s.tetrahedra[t].vertices, c.vertices[0], c.vertices[1]))
            {
                state.tetrahedra[t] = 0U;
            }
        }
    }
    for (std::size_t i = 0U; i < s.constraints.size(); ++i)
    {
        auto const &c = s.constraints[i];
        if (c.kind == Kind::Area)
        {
            for (std::size_t t = 0U; t < s.triangles.size(); ++t)
            {
                auto const &v = s.triangles[t].vertices;
                if (state.faces[t] == 0U && Contains(v, c.vertices[0], c.vertices[1]) &&
                    std::ranges::find(v, c.vertices[2]) != v.end())
                {
                    state.active[i] = 0U;
                }
            }
        }
        if (c.kind == Kind::Volume)
        {
            for (std::size_t t = 0U; t < s.tetrahedra.size(); ++t)
            {
                auto v = s.tetrahedra[t].vertices;
                auto w = c.vertices;
                std::ranges::sort(v);
                std::ranges::sort(w);
                if (state.tetrahedra[t] == 0U && v == w)
                {
                    state.active[i] = 0U;
                }
            }
        }
        if (state.active[i] == 0U)
        {
            state.lambdas[i] = 0.0F;
        }
    }
}
std::expected<void, Error> ValidateActiveVolumes(Scene const &scene, State const &state, Stage stage)
{
    if (stage < Stage::Shape)
    {
        return {};
    }
    for (std::size_t i = 0U; i < scene.tetrahedra.size(); ++i)
    {
        if (state.tetrahedra[i] == 0U)
        {
            continue;
        }
        Constraint c{};
        c.kind = Kind::Volume;
        c.vertices = scene.tetrahedra[i].vertices;
        auto evaluated = Evaluate(c, state.particles);
        if (!evaluated)
        {
            return std::unexpected(evaluated.error());
        }
    }
    return {};
}
std::expected<void, Error> Environment(std::vector<Particle> &particles, std::vector<Particle> const &start,
                                       float friction, Metrics &metrics)
{
    for (std::size_t i = 0U; i < particles.size(); ++i)
    {
        auto &p = particles[i];
        for (std::uint32_t collider = 0U; collider < 2U; ++collider)
        {
            Vector3 normal{0.0F, 1.0F, 0.0F};
            float penetration = p.radius - p.position.y;
            if (collider == 1U)
            {
                auto const delta = p.position - Vector3{0.0F, 0.55F, 0.0F};
                float const distance = Length(delta);
                penetration = 0.35F + p.radius - distance;
                normal = distance > 1.0e-7F ? delta * (1.0F / distance) : Vector3{0.0F, 1.0F, 0.0F};
            }
            if (penetration <= 0.0F)
            {
                continue;
            }
            if (p.inverseMass == 0.0F)
            {
                return std::unexpected(Error::PinnedContact);
            }
            ++metrics.contacts;
            p.position = p.position + normal * penetration;
            auto const displacement = p.position - start[i].position;
            auto const tangent = displacement - normal * Dot(displacement, normal);
            float const length = Length(tangent);
            if (length > 1.0e-7F)
            {
                p.position = p.position - tangent * (std::min(length, friction * penetration) / length);
            }
        }
    }
    return {};
}
std::array<std::int32_t, 3U> Cell(Vector3 p)
{
    return {static_cast<std::int32_t>(std::floor((p.x + 4.0F) / 0.25F)),
            static_cast<std::int32_t>(std::floor((p.y + 4.0F) / 0.25F)),
            static_cast<std::int32_t>(std::floor((p.z + 4.0F) / 0.25F))};
}
bool Domain(Vector3 p)
{
    return Finite(p) && p.x >= -4.0F && p.x < 4.0F && p.y >= -4.0F && p.y < 4.0F && p.z >= -4.0F && p.z < 4.0F;
}
std::expected<void, Error> Self(Scene const &s, State &state)
{
    // CPU reference deliberately uses a direct neighborhood predicate, not the GPU sorted-key implementation.
    auto const snapshot = state.particles;
    for (std::size_t i = 0U; i < snapshot.size(); ++i)
    {
        auto const cell = Cell(snapshot[i].position);
        Vector3 correction{};
        for (std::size_t j = 0U; j < snapshot.size(); ++j)
        {
            if (i == j || Excluded(s, state, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j)))
            {
                continue;
            }
            auto const other = Cell(snapshot[j].position);
            if (std::abs(cell[0] - other[0]) > 1 || std::abs(cell[1] - other[1]) > 1 ||
                std::abs(cell[2] - other[2]) > 1)
            {
                continue;
            }
            if (i < j)
            {
                ++state.metrics.candidates;
            }
            auto const delta = snapshot[i].position - snapshot[j].position;
            float const distance = Length(delta);
            float const penetration = snapshot[i].radius + snapshot[j].radius - distance;
            if (penetration <= 0.0F)
            {
                continue;
            }
            float const weight = snapshot[i].inverseMass + snapshot[j].inverseMass;
            if (weight == 0.0F)
            {
                return std::unexpected(Error::PinnedContact);
            }
            if (i < j)
            {
                ++state.metrics.contacts;
            }
            auto const normal =
                distance > 1.0e-7F ? delta * (1.0F / distance) : Vector3{i < j ? -1.0F : 1.0F, 0.0F, 0.0F};
            correction = correction + normal * (penetration * snapshot[i].inverseMass / weight);
        }
        if (snapshot[i].inverseMass > 0.0F)
        {
            state.particles[i].position = snapshot[i].position + correction;
        }
    }
    return {};
}
} // namespace
std::expected<void, Error> Color(Scene &s)
{
    if (s.particles.empty() || s.particles.size() > kMaxParticles || s.constraints.size() > kMaxConstraints)
    {
        return std::unexpected(Error::InvalidTopology);
    }
    for (auto const &p : s.particles)
    {
        if (!std::isfinite(p.inverseMass) || p.inverseMass < 0.0F || p.inverseMass > 100.0F ||
            (p.inverseMass > 0.0F && p.inverseMass < std::numeric_limits<float>::min()))
        {
            return std::unexpected(Error::InvalidParameter);
        }
    }
    for (auto const &c : s.constraints)
    {
        if (c.kind > Kind::Volume)
        {
            return std::unexpected(Error::InvalidTopology);
        }
        for (std::uint32_t i = 0U; i < Arity(c.kind); ++i)
        {
            if (c.vertices[i] >= s.particles.size())
            {
                return std::unexpected(Error::InvalidTopology);
            }
            for (std::uint32_t j = 0U; j < i; ++j)
            {
                if (c.vertices[i] == c.vertices[j])
                {
                    return std::unexpected(Error::InvalidTopology);
                }
            }
        }
    }
    for (std::size_t i = 0U; i < s.constraints.size(); ++i)
    {
        auto &c = s.constraints[i];
        c.color = 0U;
        for (;; ++c.color)
        {
            bool used = false;
            for (std::size_t j = 0U; j < i; ++j)
            {
                used |= s.constraints[j].color == c.color && SharesMutable(c, s.constraints[j], s);
            }
            if (!used)
            {
                break;
            }
            if (c.color >= kMaxConstraints)
            {
                return std::unexpected(Error::Ownership);
            }
        }
    }
    return {};
}
std::expected<void, Error> Validate(Scene const &s, Configuration const &config)
{
    if (s.particles.empty() || s.particles.size() > kMaxParticles || s.constraints.size() > kMaxConstraints ||
        s.triangles.size() > kMaxTriangles || s.tetrahedra.size() > kMaxTetrahedra)
    {
        return std::unexpected(Error::InvalidTopology);
    }
    if (!std::isfinite(config.tick) || config.tick < 1.0F / 240.0F || config.tick > 1.0F / 30.0F ||
        config.substeps == 0U || config.substeps > 8U || config.iterations == 0U || config.iterations > 32U ||
        !Finite(config.gravity) || Length(config.gravity) > 100.0F || !std::isfinite(config.friction) ||
        config.friction < 0.0F || config.friction > 1.0F || !std::isfinite(config.complianceScale) ||
        config.complianceScale < 0.0F || config.complianceScale > 100.0F || !std::isfinite(config.motionLimit) ||
        config.motionLimit <= 0.0F || config.motionLimit > 1.0F)
    {
        return std::unexpected(Error::InvalidParameter);
    }
    for (auto const &p : s.particles)
    {
        if (!Domain(p.position) || !Finite(p.velocity) || !std::isfinite(p.inverseMass) || p.inverseMass < 0.0F ||
            p.inverseMass > 100.0F ||
            (p.inverseMass > 0.0F &&
             (p.inverseMass < std::numeric_limits<float>::min() || !std::isfinite(1.0F / p.inverseMass))) ||
            !std::isfinite(p.radius) || p.radius <= 0.0F || p.radius > 0.125F ||
            (p.inverseMass == 0.0F && Length(p.velocity) != 0.0F))
        {
            return std::unexpected(Error::InvalidParameter);
        }
    }
    std::set<std::array<std::uint32_t, 5U>> unique{};
    for (std::size_t i = 0U; i < s.constraints.size(); ++i)
    {
        auto const &c = s.constraints[i];
        if (c.kind > Kind::Volume || c.color >= kMaxConstraints || c.flags != 0U ||
            c.padding != std::array<std::uint32_t, 2U>{} || !std::isfinite(c.rest) || c.rest <= 0.0F ||
            !std::isfinite(c.compliance) || c.compliance < 0.0F || c.compliance > 1.0F || !std::isfinite(c.tearRatio) ||
            c.tearRatio <= 1.0F || c.tearRatio > 100.0F)
        {
            return std::unexpected(Error::InvalidRest);
        }
        std::array<std::uint32_t, 5U> key{c.kind <= Kind::Bending ? 0U : static_cast<std::uint32_t>(c.kind), 0U, 0U, 0U,
                                          0U};
        for (std::uint32_t a = 0U; a < 4U; ++a)
        {
            if (a >= Arity(c.kind))
            {
                if (c.vertices[a] != 0U)
                {
                    return std::unexpected(Error::InvalidTopology);
                }
                continue;
            }
            if (c.vertices[a] >= s.particles.size())
            {
                return std::unexpected(Error::InvalidTopology);
            }
            for (std::uint32_t b = 0U; b < a; ++b)
            {
                if (c.vertices[a] == c.vertices[b])
                {
                    return std::unexpected(Error::InvalidTopology);
                }
            }
            key[a + 1U] = c.vertices[a];
        }
        std::sort(key.begin() + 1, key.begin() + 1 + Arity(c.kind));
        if (!unique.insert(key).second)
        {
            return std::unexpected(Error::InvalidTopology);
        }
        auto const e = Evaluate(c, s.particles);
        if (!e || std::abs(e->value - c.rest) > 1.0e-5F * std::max(1.0e-7F, e->value))
        {
            return std::unexpected(Error::InvalidRest);
        }
        if (c.kind == Kind::Area)
        {
            bool found = false;
            for (auto const &face : s.triangles)
            {
                auto v = face.vertices;
                std::ranges::sort(v);
                found |= std::equal(v.begin(), v.end(), key.begin() + 1);
            }
            if (!found)
            {
                return std::unexpected(Error::InvalidTopology);
            }
        }
        if (c.kind == Kind::Volume)
        {
            bool found = false;
            for (auto const &tet : s.tetrahedra)
            {
                auto v = tet.vertices;
                std::ranges::sort(v);
                found |= std::equal(v.begin(), v.end(), key.begin() + 1);
            }
            if (!found)
            {
                return std::unexpected(Error::InvalidTopology);
            }
        }
        for (std::size_t j = 0U; j < i; ++j)
        {
            if (c.color == s.constraints[j].color && SharesMutable(c, s.constraints[j], s))
            {
                return std::unexpected(Error::Ownership);
            }
        }
    }
    std::set<std::array<std::uint32_t, 4U>> elements{};
    for (auto const &t : s.triangles)
    {
        if (t.padding != 0U)
        {
            return std::unexpected(Error::InvalidTopology);
        }
        Constraint c{};
        c.kind = Kind::Area;
        std::copy(t.vertices.begin(), t.vertices.end(), c.vertices.begin());
        auto key = c.vertices;
        for (auto v : t.vertices)
        {
            if (v >= s.particles.size())
            {
                return std::unexpected(Error::InvalidTopology);
            }
        }
        std::sort(key.begin(), key.begin() + 3);
        if (key[0] == key[1] || key[1] == key[2] || !elements.insert(key).second || !Evaluate(c, s.particles))
        {
            return std::unexpected(Error::InvalidRest);
        }
    }
    elements.clear();
    for (auto const &face : s.triangles)
    {
        for (std::uint32_t edge = 0U; edge < 3U; ++edge)
        {
            auto const a = face.vertices[edge];
            auto const b = face.vertices[(edge + 1U) % 3U];
            std::uint32_t forward{};
            std::uint32_t reverse{};
            for (auto const &other : s.triangles)
            {
                for (std::uint32_t e = 0U; e < 3U; ++e)
                {
                    auto const x = other.vertices[e];
                    auto const y = other.vertices[(e + 1U) % 3U];
                    if (x == a && y == b)
                    {
                        ++forward;
                    }
                    if (x == b && y == a)
                    {
                        ++reverse;
                    }
                }
            }
            if (forward != 1U || reverse > 1U)
            {
                return std::unexpected(Error::InvalidTopology);
            }
        }
    }
    for (auto const &t : s.tetrahedra)
    {
        Constraint c{};
        c.kind = Kind::Volume;
        c.vertices = t.vertices;
        auto key = c.vertices;
        for (auto v : key)
        {
            if (v >= s.particles.size())
            {
                return std::unexpected(Error::InvalidTopology);
            }
        }
        std::ranges::sort(key);
        if (std::adjacent_find(key.begin(), key.end()) != key.end() || !elements.insert(key).second ||
            !Evaluate(c, s.particles))
        {
            return std::unexpected(Error::InvalidRest);
        }
        for (auto const &face : s.triangles)
        {
            bool incident = true;
            for (auto vertex : face.vertices)
            {
                incident &= std::ranges::find(t.vertices, vertex) != t.vertices.end();
            }
            if (!incident)
            {
                continue;
            }
            for (auto vertex : t.vertices)
            {
                if (std::ranges::find(face.vertices, vertex) != face.vertices.end())
                {
                    continue;
                }
                auto const a = s.particles[face.vertices[0]].position;
                auto const b = s.particles[face.vertices[1]].position;
                auto const d = s.particles[face.vertices[2]].position;
                if (Dot(Cross(b - a, d - a), s.particles[vertex].position - a) >= 0.0F)
                {
                    return std::unexpected(Error::InvalidRest);
                }
            }
        }
    }
    return {};
}
Scene MakeCloth()
{
    Scene s{};
    for (std::uint32_t y = 0U; y < 4U; ++y)
    {
        for (std::uint32_t x = 0U; x < 4U; ++x)
        {
            s.particles.push_back({{(static_cast<float>(x) - 1.5F) * 0.3F, 1.6F - static_cast<float>(y) * 0.3F, 0.45F},
                                   y == 0U && (x == 0U || x == 3U) ? 0.0F : 1.0F,
                                   {},
                                   0.04F});
        }
    }
    for (std::uint32_t y = 0U; y < 4U; ++y)
    {
        for (std::uint32_t x = 0U; x < 4U; ++x)
        {
            auto const a = y * 4U + x;
            if (x < 3U)
            {
                Add(s, Kind::Structural, {a, a + 1U, 0U, 0U});
            }
            if (y < 3U)
            {
                Add(s, Kind::Structural, {a, a + 4U, 0U, 0U});
            }
            if (x < 2U)
            {
                Add(s, Kind::Bending, {a, a + 2U, 0U, 0U});
            }
            if (y < 2U)
            {
                Add(s, Kind::Bending, {a, a + 8U, 0U, 0U});
            }
            if (x < 3U && y < 3U)
            {
                Add(s, Kind::Shear, {a, a + 5U, 0U, 0U});
                Add(s, Kind::Shear, {a + 1U, a + 4U, 0U, 0U});
                s.triangles.push_back({{a, a + 4U, a + 1U}, 0U});
                s.triangles.push_back({{a + 1U, a + 4U, a + 5U}, 0U});
            }
        }
    }
    for (auto const &t : s.triangles)
    {
        Add(s, Kind::Area, {t.vertices[0], t.vertices[1], t.vertices[2], 0U});
    }
    (void)Color(s);
    return s;
}
Scene MakeSoftBody()
{
    Scene s{};
    s.particles = {{{-0.3F, 1.4F, -0.2F}, 1.0F, {}, 0.04F},
                   {{0.3F, 1.4F, -0.2F}, 1.0F, {}, 0.04F},
                   {{0.0F, 1.9F, -0.2F}, 1.0F, {}, 0.04F},
                   {{0.0F, 1.6F, 0.4F}, 1.0F, {}, 0.04F}};
    for (std::uint32_t a = 0U; a < 4U; ++a)
    {
        for (std::uint32_t b = a + 1U; b < 4U; ++b)
        {
            Add(s, Kind::Structural, {a, b, 0U, 0U});
        }
    }
    s.triangles = {{{0U, 2U, 1U}, 0U}, {{0U, 1U, 3U}, 0U}, {{0U, 3U, 2U}, 0U}, {{1U, 2U, 3U}, 0U}};
    s.tetrahedra = {{{0U, 1U, 2U, 3U}}};
    Add(s, Kind::Volume, {0U, 1U, 2U, 3U});
    (void)Color(s);
    return s;
}
State Reset(Scene const &s)
{
    State state{};
    state.particles = s.particles;
    state.lambdas.resize(s.constraints.size());
    state.active.assign(s.constraints.size(), 1U);
    state.faces.assign(s.triangles.size(), 1U);
    state.tetrahedra.assign(s.tetrahedra.size(), 1U);
    return state;
}
bool Excluded(Scene const &s, State const &state, std::uint32_t a, std::uint32_t b)
{
    for (std::size_t i = 0U; i < s.constraints.size(); ++i)
    {
        auto const &c = s.constraints[i];
        if (state.active[i] != 0U && c.kind <= Kind::Bending && Contains(std::span(c.vertices.data(), 2U), a, b))
        {
            return true;
        }
    }
    for (std::size_t i = 0U; i < s.triangles.size(); ++i)
    {
        if (state.faces[i] != 0U && Contains(s.triangles[i].vertices, a, b))
        {
            return true;
        }
    }
    for (std::size_t i = 0U; i < s.tetrahedra.size(); ++i)
    {
        if (state.tetrahedra[i] != 0U && Contains(s.tetrahedra[i].vertices, a, b))
        {
            return true;
        }
    }
    return false;
}
std::expected<void, Error> Advance(Scene const &s, Configuration const &cfg, Stage stage, State &state)
{
    auto valid = Validate(s, cfg);
    if (!valid)
    {
        return valid;
    }
    if (state.particles.size() != s.particles.size() || state.lambdas.size() != s.constraints.size() ||
        state.active.size() != s.constraints.size() || state.faces.size() != s.triangles.size() ||
        state.tetrahedra.size() != s.tetrahedra.size())
    {
        return std::unexpected(Error::InvalidTopology);
    }
    if (cfg.paused)
    {
        return {};
    }
    for (std::size_t i = 0U; i < state.particles.size(); ++i)
    {
        auto const &p = state.particles[i];
        if (!Finite(p.position) || !Finite(p.velocity))
        {
            return std::unexpected(Error::NonFinite);
        }
        if (!Domain(p.position))
        {
            return std::unexpected(Error::Domain);
        }
        if (p.inverseMass != s.particles[i].inverseMass || p.radius != s.particles[i].radius ||
            (p.inverseMass == 0.0F && Length(p.position - s.particles[i].position) != 0.0F))
        {
            return std::unexpected(Error::InvalidParameter);
        }
    }
    float const dt = cfg.tick / static_cast<float>(cfg.substeps);
    state.metrics = {};
    for (std::uint32_t step = 0U; step < cfg.substeps; ++step)
    {
        auto const start = state.particles;
        std::ranges::fill(state.lambdas, 0.0F);
        for (auto &p : state.particles)
        {
            if (p.inverseMass == 0.0F)
            {
                p.velocity = {};
                continue;
            }
            p.velocity = p.velocity + cfg.gravity * dt;
            if (!Finite(p.velocity * dt))
            {
                return std::unexpected(Error::NonFinite);
            }
            if (Length(p.velocity * dt) > cfg.motionLimit)
            {
                return std::unexpected(Error::MotionBound);
            }
            p.position = p.position + p.velocity * dt;
            if (!Domain(p.position))
            {
                return std::unexpected(Error::Domain);
            }
        }
        valid = ValidateActiveVolumes(s, state, stage);
        if (!valid)
        {
            return valid;
        }
        if (stage == Stage::All && cfg.tearing)
        {
            for (std::size_t i = 0U; i < s.constraints.size(); ++i)
            {
                auto const &c = s.constraints[i];
                if (state.active[i] == 0U || c.kind > Kind::Shear)
                {
                    continue;
                }
                if (Length(state.particles[c.vertices[0]].position - state.particles[c.vertices[1]].position) >
                    c.rest * c.tearRatio)
                {
                    state.active[i] = 0U;
                }
            }
            Topology(s, state);
        }
        for (std::uint32_t iteration = 0U; iteration < cfg.iterations; ++iteration)
        {
            for (std::uint32_t color = 0U; color < kMaxConstraints; ++color)
            {
                for (std::size_t i = 0U; i < s.constraints.size(); ++i)
                {
                    auto const &c = s.constraints[i];
                    if (stage == Stage::Prediction || (stage == Stage::Distance && c.kind > Kind::Bending) ||
                        state.active[i] == 0U || c.color != color)
                    {
                        continue;
                    }
                    auto e = Evaluate(c, state.particles);
                    if (!e)
                    {
                        return std::unexpected(e.error());
                    }
                    float const alpha = c.compliance * cfg.complianceScale / (dt * dt);
                    float denominator = alpha;
                    for (std::uint32_t a = 0U; a < Arity(c.kind); ++a)
                    {
                        denominator +=
                            state.particles[c.vertices[a]].inverseMass * Dot(e->gradients[a], e->gradients[a]);
                    }
                    float const residual = e->value - c.rest + alpha * state.lambdas[i];
                    if (denominator <= 1.0e-12F)
                    {
                        if (std::abs(residual) > 1.0e-6F)
                        {
                            return std::unexpected(Error::Singular);
                        }
                        continue;
                    }
                    float const dl = -residual / denominator;
                    state.lambdas[i] += dl;
                    for (std::uint32_t a = 0U; a < Arity(c.kind); ++a)
                    {
                        auto &p = state.particles[c.vertices[a]];
                        if (p.inverseMass > 0.0F)
                        {
                            p.position = p.position + e->gradients[a] * (p.inverseMass * dl);
                        }
                    }
                }
                valid = ValidateActiveVolumes(s, state, stage);
                if (!valid)
                {
                    return valid;
                }
            }
            if (stage >= Stage::Environment && cfg.environment)
            {
                auto result = Environment(state.particles, start, cfg.friction, state.metrics);
                if (!result)
                {
                    return result;
                }
                valid = ValidateActiveVolumes(s, state, stage);
                if (!valid)
                {
                    return valid;
                }
            }
            if (stage >= Stage::SelfContact && cfg.selfContact)
            {
                for (auto const &p : state.particles)
                {
                    if (!Domain(p.position))
                    {
                        return std::unexpected(Error::Domain);
                    }
                }
                auto result = Self(s, state);
                if (!result)
                {
                    return result;
                }
                valid = ValidateActiveVolumes(s, state, stage);
                if (!valid)
                {
                    return valid;
                }
            }
        }
        for (std::size_t i = 0U; i < state.particles.size(); ++i)
        {
            auto &p = state.particles[i];
            float const motion = Length(p.position - start[i].position);
            state.metrics.motion = std::max(state.metrics.motion, motion);
            if (!Finite(p.position) || !Finite(p.velocity))
            {
                return std::unexpected(Error::NonFinite);
            }
            if (!Domain(p.position))
            {
                return std::unexpected(Error::Domain);
            }
            if (motion > cfg.motionLimit)
            {
                return std::unexpected(Error::MotionBound);
            }
            p.velocity = p.inverseMass == 0.0F ? Vector3{} : (p.position - start[i].position) * (1.0F / dt);
        }
    }
    ++state.ticks;
    state.metrics.ticks = state.ticks;
    for (std::size_t i = 0U; i < s.constraints.size(); ++i)
    {
        auto const &c = s.constraints[i];
        if (state.active[i] == 0U)
        {
            ++state.metrics.broken;
            continue;
        }
        if (stage == Stage::Prediction || (stage == Stage::Distance && c.kind > Kind::Bending))
        {
            continue;
        }
        auto e = Evaluate(c, state.particles);
        if (!e)
        {
            return std::unexpected(e.error());
        }
        float const error = std::abs(e->value - c.rest);
        if (c.kind == Kind::Area)
        {
            state.metrics.area = std::max(state.metrics.area, error);
        }
        else if (c.kind == Kind::Volume)
        {
            state.metrics.volume = std::max(state.metrics.volume, error);
        }
        else
        {
            state.metrics.distance = std::max(state.metrics.distance, error);
        }
        state.metrics.compliant =
            std::max(state.metrics.compliant,
                     std::abs(e->value - c.rest + c.compliance * cfg.complianceScale / (dt * dt) * state.lambdas[i]));
    }
    for (std::size_t i = 0U; i < state.particles.size(); ++i)
    {
        auto const &p = state.particles[i];
        float const speed = Length(p.velocity);
        state.metrics.speed = std::max(state.metrics.speed, speed);
        if (p.inverseMass > 0.0F)
        {
            state.metrics.kinetic += 0.5F * speed * speed / p.inverseMass;
        }
        if (stage >= Stage::Environment && cfg.environment)
        {
            state.metrics.penetration = std::max(state.metrics.penetration, std::max(0.0F, p.radius - p.position.y));
            state.metrics.penetration =
                std::max(state.metrics.penetration,
                         std::max(0.0F, p.radius + 0.35F - Length(p.position - Vector3{0.0F, 0.55F, 0.0F})));
        }
        if (stage >= Stage::SelfContact && cfg.selfContact)
        {
            for (std::size_t j = i + 1U; j < state.particles.size(); ++j)
            {
                if (!Excluded(s, state, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j)))
                {
                    state.metrics.penetration =
                        std::max(state.metrics.penetration,
                                 std::max(0.0F, p.radius + state.particles[j].radius -
                                                    Length(p.position - state.particles[j].position)));
                }
            }
        }
    }
    auto const &metrics = state.metrics;
    if (!std::isfinite(metrics.distance) || !std::isfinite(metrics.area) || !std::isfinite(metrics.volume) ||
        !std::isfinite(metrics.compliant) || !std::isfinite(metrics.penetration) || !std::isfinite(metrics.speed) ||
        !std::isfinite(metrics.kinetic) || !std::isfinite(metrics.motion))
    {
        return std::unexpected(Error::NonFinite);
    }
    return {};
}
} // namespace ch41::cloth
