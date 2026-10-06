#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace ch42::sph
{
inline constexpr std::uint32_t kCapacity = 64U;
enum class Error : std::uint32_t
{
    Capacity = 1U,
    Parameters,
    NonFinite,
    Mass,
    Domain,
    Density,
    UnsafeMotion,
    Identity,
    Guard
};
std::string_view ErrorName(Error error) noexcept;
struct Vec3 final
{
    float x{}, y{}, z{};
};
Vec3 operator+(Vec3 a, Vec3 b) noexcept;
Vec3 operator-(Vec3 a, Vec3 b) noexcept;
Vec3 operator*(Vec3 a, float b) noexcept;
float Dot(Vec3 a, Vec3 b) noexcept;
float Length(Vec3 a) noexcept;
bool Finite(Vec3 a) noexcept;
struct Particle final
{
    Vec3 position{};
    float mass{1.953125F};
    Vec3 velocity{};
    std::uint32_t identity{};
};
struct Sample final
{
    float density{}, pressure{};
    Vec3 acceleration{};
    std::uint32_t candidates{}, accepted{}, coincident{};
};
struct Metrics final
{
    float densityMin{}, densityMax{}, pressureMin{}, pressureMax{};
    float speed{}, acceleration{}, soundRatio{}, motionRatio{};
    float kernelProbe{}, deficiency{}, kinetic{}, momentumMagnitude{};
    std::uint32_t candidates{}, accepted{}, coincident{}, logicalLoads{};
    std::uint32_t candidateMin{}, candidateMax{}, warnings{}, contacts{};
};
static_assert(sizeof(Particle) == 32U && sizeof(Sample) == 32U && sizeof(Metrics) == 80U);
static_assert(offsetof(Particle, mass) == 12U && offsetof(Particle, velocity) == 16U &&
              offsetof(Particle, identity) == 28U);
enum class Stage : std::uint32_t
{
    Gravity,
    Kernels,
    Density,
    Forces,
    Contact,
    Grid
};
struct Configuration final
{
    float radius{0.25F}, restDensity{1000.0F}, soundSpeed{4.0F}, dynamicViscosity{1.0F};
    float tick{1.0F / 120.0F}, restitution{0.25F}, motionLimit{0.5F};
    Vec3 gravity{0.0F, -9.81F, 0.0F};
    std::uint32_t substeps{4U}, view{};
    bool grid{true}, signedPressure{}, walls{true}, paused{};
};
using Scene = std::vector<Particle>;
struct State final
{
    Scene particles{};
    std::vector<Sample> samples{};
    std::vector<std::array<std::uint32_t, 2U>> acceptedMasks{};
    Metrics metrics{};
    std::uint32_t ticks{}, completedSubsteps{};
};
Scene MakeBlock();
std::expected<void, Error> Validate(std::span<Particle const> particles, Configuration const &configuration = {});
std::expected<float, Error> Poly6(float r, float radius) noexcept;
std::expected<Vec3, Error> Poly6Gradient(Vec3 displacement, float radius) noexcept;
std::expected<Vec3, Error> SpikyGradient(Vec3 displacement, float radius) noexcept;
std::expected<float, Error> ViscosityLaplacian(float r, float radius) noexcept;
std::expected<float, Error> Pressure(float density, Configuration const &configuration) noexcept;
std::expected<Vec3, Error> PairForce(Particle const &a, Particle const &b, Sample const &sa, Sample const &sb,
                                     Configuration const &configuration) noexcept;
State Reset(Scene const &scene);
// Independent O(N^2) reference: never uses GPU keys or uploaded neighbor lists.
std::expected<void, Error> Advance(Configuration const &configuration, Stage stage, State &state);
} // namespace ch42::sph
