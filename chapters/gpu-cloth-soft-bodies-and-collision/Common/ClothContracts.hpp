#ifndef CH41_CLOTH_CONTRACTS_HPP
#define CH41_CLOTH_CONTRACTS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace ch41::cloth
{
inline constexpr std::uint32_t kMaxParticles = 32U;
inline constexpr std::uint32_t kMaxConstraints = 128U;
inline constexpr std::uint32_t kMaxTriangles = 64U;
inline constexpr std::uint32_t kMaxTetrahedra = 8U;
inline constexpr float kDistanceComplianceBase = 1.0e-5F; // s^2/kg
inline constexpr float kAreaComplianceBase = 1.0e-6F;     // m^2*s^2/kg
inline constexpr float kVolumeComplianceBase = 1.0e-7F;   // m^4*s^2/kg
struct Vector3 final
{
    float x{}, y{}, z{};
};
Vector3 operator+(Vector3 a, Vector3 b) noexcept;
Vector3 operator-(Vector3 a, Vector3 b) noexcept;
Vector3 operator*(Vector3 a, float b) noexcept;
float Dot(Vector3 a, Vector3 b) noexcept;
Vector3 Cross(Vector3 a, Vector3 b) noexcept;
float Length(Vector3 a) noexcept;
bool Finite(Vector3 a) noexcept;

struct Particle final
{
    Vector3 position{};
    float inverseMass{1.0F};
    Vector3 velocity{};
    float radius{0.04F};
};
enum class Kind : std::uint32_t
{
    Structural,
    Shear,
    Bending,
    Area,
    Volume
};
enum class Stage : std::uint32_t
{
    Prediction,
    Distance,
    Shape,
    Environment,
    SelfContact,
    All
};
enum class Error : std::uint32_t
{
    InvalidTopology = 1U,
    InvalidRest,
    InvalidParameter,
    Ownership,
    Singular,
    Inverted,
    MotionBound,
    Domain,
    NonFinite,
    PinnedContact,
    Guard
};
std::string_view ErrorName(Error error) noexcept;
struct Constraint final
{
    std::array<std::uint32_t, 4U> vertices{};
    Kind kind{};
    std::uint32_t color{};
    float rest{};
    float compliance{};
    float tearRatio{2.0F};
    std::uint32_t flags{};
    std::array<std::uint32_t, 2U> padding{};
};
struct Triangle final
{
    std::array<std::uint32_t, 3U> vertices{};
    std::uint32_t padding{};
};
struct Tetrahedron final
{
    std::array<std::uint32_t, 4U> vertices{};
};
static_assert(sizeof(Vector3) == 12U);
static_assert(sizeof(Particle) == 32U && offsetof(Particle, inverseMass) == 12U &&
              offsetof(Particle, velocity) == 16U && offsetof(Particle, radius) == 28U);
static_assert(sizeof(Constraint) == 48U && offsetof(Constraint, kind) == 16U && offsetof(Constraint, color) == 20U &&
              offsetof(Constraint, rest) == 24U && offsetof(Constraint, compliance) == 28U &&
              offsetof(Constraint, tearRatio) == 32U && offsetof(Constraint, flags) == 36U &&
              offsetof(Constraint, padding) == 40U);
static_assert(sizeof(Triangle) == 16U && sizeof(Tetrahedron) == 16U);

struct Scene final
{
    std::vector<Particle> particles{};
    std::vector<Constraint> constraints{};
    std::vector<Triangle> triangles{};
    std::vector<Tetrahedron> tetrahedra{};
};
struct Configuration final
{
    float tick{1.0F / 60.0F};
    std::uint32_t substeps{4U};
    std::uint32_t iterations{8U};
    Vector3 gravity{0.0F, -9.81F, 0.0F};
    float friction{0.4F};
    float complianceScale{}; // Dimensionless; zero selects rigid projection.
    float motionLimit{0.5F};
    bool environment{true};
    bool selfContact{true};
    bool tearing{true};
    bool paused{};
};
struct Evaluation final
{
    float value{};
    std::array<Vector3, 4U> gradients{};
};
struct Metrics final
{
    float distance{}, area{}, volume{}, compliant{}, penetration{}, speed{}, kinetic{}, motion{};
    std::uint32_t contacts{}, candidates{}, broken{}, ticks{};
    std::array<std::uint32_t, 4U> padding{};
};
static_assert(sizeof(Metrics) == 64U);
struct State final
{
    std::vector<Particle> particles{};
    std::vector<float> lambdas{};
    std::vector<std::uint32_t> active{}, faces{}, tetrahedra{};
    Metrics metrics{};
    std::uint32_t ticks{};
};
struct MechanicalEnergy final
{
    double kinetic{}, gravitationalPotential{}, total{};
};
static_assert(sizeof(MechanicalEnergy) == 24U && offsetof(MechanicalEnergy, gravitationalPotential) == 8U &&
              offsetof(MechanicalEnergy, total) == 16U);
std::expected<MechanicalEnergy, Error> MeasureMechanicalEnergy(std::span<Particle const> particles, Vector3 gravity);
std::uint32_t Arity(Kind kind) noexcept;
std::expected<Evaluation, Error> Evaluate(Constraint const &constraint, std::vector<Particle> const &particles);
std::expected<void, Error> Color(Scene &scene);
std::expected<void, Error> Validate(Scene const &scene, Configuration const &configuration = {});
Scene MakeCloth();
Scene MakeSoftBody();
State Reset(Scene const &scene);
std::expected<void, Error> Advance(Scene const &scene, Configuration const &cfg, Stage stage, State &state);
bool Excluded(Scene const &scene, State const &state, std::uint32_t a, std::uint32_t b);
} // namespace ch41::cloth
#endif
