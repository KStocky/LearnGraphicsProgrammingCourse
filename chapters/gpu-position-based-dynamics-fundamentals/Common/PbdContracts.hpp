#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace ch40::pbd
{

inline constexpr std::uint32_t kMaximumParticles = 32U;
inline constexpr std::uint32_t kMaximumConstraints = 32U;
inline constexpr std::uint32_t kMaximumDegree = 4U;
inline constexpr std::uint32_t kMaximumIterations = 64U;
inline constexpr std::int32_t kAtomicScale = 1 << 20;
inline constexpr float kMaximumCorrection = 8.0F;
inline constexpr float kLengthEpsilon = 1.0e-6F;

struct Vector3 final
{
    float x{};
    float y{};
    float z{};
};

struct Particle final
{
    Vector3 position{};
    float inverseMass{1.0F};
    Vector3 velocity{};
    std::uint32_t padding{};
};

enum class ConstraintKind : std::uint32_t
{
    Distance = 0U,
    Attachment = 1U,
};

struct Constraint final
{
    std::uint32_t first{};
    std::uint32_t second{};
    ConstraintKind kind{ConstraintKind::Distance};
    std::uint32_t color{};
    float restLength{};
    float compliance{};
    float padding0{};
    float padding1{};
    Vector3 anchor{};
    float padding2{};
};

enum class Strategy : std::uint32_t
{
    Colored = 0U,
    Jacobi = 1U,
    Atomic = 2U,
};

struct Configuration final
{
    float timeStep{1.0F / 60.0F};
    std::uint32_t iterations{32U};
    Strategy strategy{Strategy::Colored};
    float relaxation{0.5F};
    Vector3 gravity{0.0F, -9.81F, 0.0F};
};

enum class Error : std::uint32_t
{
    InvalidCount,
    NonFiniteInput,
    InvalidMass,
    InvalidConfiguration,
    InvalidConstraint,
    DegenerateDistance,
    NoMovableParticle,
    UnsafeColor,
    ExcessiveDegree,
    AtomicRange,
    NonFiniteResult,
};

struct Scene final
{
    std::vector<Particle> particles{};
    std::vector<Constraint> constraints{};
};

struct Correction final
{
    Vector3 first{};
    float deltaLambda{};
    Vector3 second{};
    float padding{};
};

struct Metrics final
{
    float maximumDistanceError{};
    float maximumAttachmentError{};
    float maximumCompliantResidual{};
    float maximumSpeed{};
    float kineticEnergy{};
};

struct StepResult final
{
    std::vector<Particle> predicted{};
    std::vector<Particle> particles{};
    std::vector<float> lambdas{};
    std::vector<Metrics> iterations{};
    Metrics beforeProjection{};
    Metrics final{};
};

static_assert(sizeof(Vector3) == 12U);
static_assert(sizeof(Particle) == 32U);
static_assert(sizeof(Constraint) == 48U);
static_assert(sizeof(Correction) == 32U);
static_assert(kMaximumDegree * kMaximumCorrection * static_cast<float>(kAtomicScale) < 2'147'483'647.0F);

[[nodiscard]] Vector3 Add(Vector3 first, Vector3 second) noexcept;
[[nodiscard]] Vector3 Subtract(Vector3 first, Vector3 second) noexcept;
[[nodiscard]] Vector3 Scale(Vector3 value, float scale) noexcept;
[[nodiscard]] float Length(Vector3 value) noexcept;
[[nodiscard]] bool IsFinite(Vector3 value) noexcept;
[[nodiscard]] std::string_view ErrorName(Error error) noexcept;
[[nodiscard]] std::expected<void, Error> ValidateScene(Scene const &scene);
[[nodiscard]] std::expected<void, Error> ValidateConfiguration(Configuration const &configuration, Scene const &scene);
[[nodiscard]] std::expected<Correction, Error> ProjectConstraint(std::span<Particle const> particles,
                                                                 Constraint const &constraint, float lambda,
                                                                 float timeStep, float relaxation = 1.0F);
[[nodiscard]] std::expected<std::int32_t, Error> QuantizeCorrection(float correction);
[[nodiscard]] std::expected<Metrics, Error> Measure(std::span<Particle const> particles,
                                                    std::span<Constraint const> constraints,
                                                    std::span<float const> lambdas, float timeStep);
[[nodiscard]] std::expected<StepResult, Error> SolveStep(Scene const &scene, Configuration const &configuration,
                                                         bool project = true);
[[nodiscard]] std::expected<Scene, Error> MakeDefaultScene(bool rootPinned = true, float massRatio = 1.0F,
                                                           float compliance = 0.0F);

} // namespace ch40::pbd
