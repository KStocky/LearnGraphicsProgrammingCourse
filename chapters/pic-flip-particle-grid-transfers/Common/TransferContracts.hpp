#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace ch43::transfers
{
inline constexpr std::uint32_t kCapacity = 64U, kFaces = 240U;
struct Vec3 final
{
    float x{}, y{}, z{};
};
Vec3 operator+(Vec3 a, Vec3 b) noexcept;
Vec3 operator-(Vec3 a, Vec3 b) noexcept;
Vec3 operator*(Vec3 a, float b) noexcept;
float Dot(Vec3 a, Vec3 b) noexcept;
bool Finite(Vec3 a) noexcept;
float Component(Vec3 a, std::uint32_t axis) noexcept;
struct Particle final
{
    Vec3 position{};
    float mass{1.0F};
    Vec3 velocity{};
    std::uint32_t identity{};
};
struct Face final
{
    float mass{}, momentum{}, before{}, after{};
    std::uint32_t beforeValid{}, afterValid{}, padding0{}, padding1{};
};
struct Sample final
{
    Vec3 pic{};
    float support{};
    Vec3 delta{};
    std::uint32_t entries{};
};
struct Metrics final
{
    Vec3 familyMass{};
    float kinetic{};
    Vec3 familyMomentum{};
    float inputKinetic{};
    Vec3 particleMomentum{};
    float supportMin{};
    std::uint32_t occupied{}, entries{}, attempts{}, round{};
};
static_assert(sizeof(Particle) == 32U && sizeof(Face) == 32U && sizeof(Sample) == 32U && sizeof(Metrics) == 64U);
static_assert(offsetof(Particle, velocity) == 16U && offsetof(Particle, identity) == 28U);
enum class Error : std::uint32_t
{
    Capacity = 1U,
    Parameters,
    NonFinite,
    Mass,
    Domain,
    NoSupport,
    Topology,
    Identity,
    AtomicLimit,
    Guard
};
std::string_view ErrorName(Error error) noexcept;
enum class Stage : std::uint32_t
{
    Identity,
    Support,
    Scatter,
    Normalize,
    Pic,
    Flip
};
struct Configuration final
{
    float alpha{0.95F};
    Vec3 increment{};
    // 0: transfer, 1: manufactured constant, 2: manufactured affine,
    // 3: remove after-validity (topology error), 4: remove both (no support).
    std::uint32_t field{}, view{};
    bool paused{};
};
struct Entry final
{
    std::uint32_t face{};
    double weight{};
};
using Stencil = std::vector<Entry>;
using Scene = std::vector<Particle>;
struct State final
{
    Scene particles{}, snapshot{};
    std::array<Face, kFaces> faces{};
    std::vector<Sample> samples{};
    Metrics metrics{};
    std::uint32_t rounds{};
};
Scene MakeBlock();
Vec3 FacePosition(std::uint32_t face) noexcept;
std::expected<Stencil, Error> Support(Vec3 position, std::uint32_t axis);
std::expected<void, Error> Validate(std::span<Particle const> particles, Configuration const &configuration = {});
State Reset(Scene const &scene);
std::expected<void, Error> Advance(Configuration const &configuration, Stage stage, State &state);
std::expected<double, Error> Interpolate(Stencil const &stencil, std::span<Face const> faces, bool delta);
} // namespace ch43::transfers
