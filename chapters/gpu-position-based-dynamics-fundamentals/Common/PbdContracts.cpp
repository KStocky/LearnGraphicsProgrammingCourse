#include "PbdContracts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace ch40::pbd
{
namespace
{

bool InBox(Vector3 value, float extent) noexcept
{
    return IsFinite(value) && std::abs(value.x) <= extent && std::abs(value.y) <= extent && std::abs(value.z) <= extent;
}

std::expected<void, Error> ValidateConstraint(std::span<Particle const> particles, Constraint const &constraint)
{
    if (constraint.first >= particles.size() || constraint.second >= particles.size() ||
        constraint.color >= kMaximumConstraints || !std::isfinite(constraint.restLength) ||
        !std::isfinite(constraint.compliance) || constraint.compliance < 0.0F || constraint.compliance > 0.1F ||
        !InBox(constraint.anchor, 2.0F) || constraint.padding0 != 0.0F || constraint.padding1 != 0.0F ||
        constraint.padding2 != 0.0F)
    {
        return std::unexpected(Error::InvalidConstraint);
    }
    if (constraint.kind == ConstraintKind::Distance)
    {
        if (constraint.first == constraint.second || constraint.restLength < 0.01F || constraint.restLength > 2.0F)
        {
            return std::unexpected(Error::InvalidConstraint);
        }
    }
    else if (constraint.kind == ConstraintKind::Attachment)
    {
        if (constraint.first != constraint.second || constraint.restLength != 0.0F)
        {
            return std::unexpected(Error::InvalidConstraint);
        }
    }
    else
    {
        return std::unexpected(Error::InvalidConstraint);
    }
    return {};
}

std::uint32_t MaximumDegree(Scene const &scene)
{
    std::array<std::uint32_t, kMaximumParticles> degrees{};
    for (auto const &constraint : scene.constraints)
    {
        ++degrees[constraint.first];
        if (constraint.kind == ConstraintKind::Distance)
        {
            ++degrees[constraint.second];
        }
    }
    return *std::max_element(degrees.begin(), degrees.end());
}

} // namespace

Vector3 Add(Vector3 first, Vector3 second) noexcept
{
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Vector3 Subtract(Vector3 first, Vector3 second) noexcept
{
    return {first.x - second.x, first.y - second.y, first.z - second.z};
}

Vector3 Scale(Vector3 value, float scale) noexcept
{
    return {value.x * scale, value.y * scale, value.z * scale};
}

float Length(Vector3 value) noexcept
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

bool IsFinite(Vector3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::string_view ErrorName(Error error) noexcept
{
    switch (error)
    {
    case Error::InvalidCount:
        return "Particle or constraint count is outside the bounded lab";
    case Error::NonFiniteInput:
        return "Particle state is non-finite or outside the input bounds";
    case Error::InvalidMass:
        return "Inverse mass must be finite and between zero and one hundred";
    case Error::InvalidConfiguration:
        return "Invalid time step, iterations, solver, gravity or relaxation";
    case Error::InvalidConstraint:
        return "Invalid constraint indices, type, rest length, compliance or anchor";
    case Error::DegenerateDistance:
        return "A nonzero distance constraint has coincident endpoints";
    case Error::NoMovableParticle:
        return "An unsatisfied constraint has no movable endpoint";
    case Error::UnsafeColor:
        return "Two constraints in a color share a mutable particle";
    case Error::ExcessiveDegree:
        return "Particle degree exceeds the bounded accumulation contract";
    case Error::AtomicRange:
        return "A correction exceeds the fixed-point accumulation range";
    case Error::NonFiniteResult:
        return "The solver produced non-finite state or evidence";
    }
    return "Unknown PBD contract error";
}

std::expected<void, Error> ValidateScene(Scene const &scene)
{
    if (scene.particles.empty() || scene.particles.size() > kMaximumParticles || scene.constraints.empty() ||
        scene.constraints.size() > kMaximumConstraints)
    {
        return std::unexpected(Error::InvalidCount);
    }
    for (auto const &particle : scene.particles)
    {
        if (!InBox(particle.position, 2.0F) || !InBox(particle.velocity, 8.0F) || particle.padding != 0U)
        {
            return std::unexpected(Error::NonFiniteInput);
        }
        if (!std::isfinite(particle.inverseMass) || particle.inverseMass < 0.0F || particle.inverseMass > 100.0F)
        {
            return std::unexpected(Error::InvalidMass);
        }
    }
    std::array<std::array<bool, kMaximumParticles>, kMaximumConstraints> writes{};
    for (auto const &constraint : scene.constraints)
    {
        auto valid = ValidateConstraint(scene.particles, constraint);
        if (!valid)
        {
            return valid;
        }
        auto correction = ProjectConstraint(scene.particles, constraint, 0.0F, 1.0F / 60.0F);
        if (!correction)
        {
            return std::unexpected(correction.error());
        }
        std::array<std::uint32_t, 2U> const endpoints{constraint.first, constraint.second};
        std::uint32_t const count = constraint.kind == ConstraintKind::Distance ? 2U : 1U;
        for (std::uint32_t index = 0U; index < count; ++index)
        {
            std::uint32_t const particle = endpoints[index];
            if (scene.particles[particle].inverseMass == 0.0F)
            {
                continue;
            }
            if (writes[constraint.color][particle])
            {
                return std::unexpected(Error::UnsafeColor);
            }
            writes[constraint.color][particle] = true;
        }
    }
    if (MaximumDegree(scene) > kMaximumDegree)
    {
        return std::unexpected(Error::ExcessiveDegree);
    }
    return {};
}

std::expected<void, Error> ValidateConfiguration(Configuration const &configuration, Scene const &scene)
{
    auto valid = ValidateScene(scene);
    if (!valid)
    {
        return valid;
    }
    if (!std::isfinite(configuration.timeStep) || configuration.timeStep < 1.0F / 240.0F ||
        configuration.timeStep > 1.0F / 15.0F || configuration.iterations == 0U ||
        configuration.iterations > kMaximumIterations || !InBox(configuration.gravity, 20.0F) ||
        !std::isfinite(configuration.relaxation) || configuration.relaxation <= 0.0F ||
        configuration.relaxation > 1.0F ||
        (configuration.strategy != Strategy::Colored && configuration.strategy != Strategy::Jacobi &&
         configuration.strategy != Strategy::Atomic))
    {
        return std::unexpected(Error::InvalidConfiguration);
    }
    if (configuration.strategy != Strategy::Colored &&
        configuration.relaxation > 1.0F / static_cast<float>(MaximumDegree(scene)))
    {
        return std::unexpected(Error::InvalidConfiguration);
    }
    return {};
}

std::expected<Correction, Error> ProjectConstraint(std::span<Particle const> particles, Constraint const &constraint,
                                                   float lambda, float timeStep, float relaxation)
{
    auto valid = ValidateConstraint(particles, constraint);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    if (!std::isfinite(lambda) || !std::isfinite(timeStep) || timeStep < 1.0F / 240.0F || timeStep > 1.0F / 15.0F ||
        !std::isfinite(relaxation) || relaxation <= 0.0F || relaxation > 1.0F)
    {
        return std::unexpected(Error::InvalidConfiguration);
    }
    auto const &first = particles[constraint.first];
    bool const distance = constraint.kind == ConstraintKind::Distance;
    Vector3 const other = distance ? particles[constraint.second].position : constraint.anchor;
    float const secondWeight = distance ? particles[constraint.second].inverseMass : 0.0F;
    if (!IsFinite(first.position) || !IsFinite(other) || !std::isfinite(first.inverseMass) ||
        first.inverseMass < 0.0F || first.inverseMass > 100.0F || !std::isfinite(secondWeight) || secondWeight < 0.0F ||
        secondWeight > 100.0F)
    {
        return std::unexpected(Error::NonFiniteInput);
    }
    Vector3 const separation = Subtract(first.position, other);
    float const length = Length(separation);
    float const value = length - constraint.restLength;
    if (distance && length <= kLengthEpsilon)
    {
        return std::unexpected(Error::DegenerateDistance);
    }
    float const weight = first.inverseMass + secondWeight;
    if (weight == 0.0F)
    {
        if (std::abs(value) > kLengthEpsilon)
        {
            return std::unexpected(Error::NoMovableParticle);
        }
        return Correction{};
    }
    float const alphaTilde = constraint.compliance / (timeStep * timeStep);
    float const deltaLambda = -relaxation * (value + alphaTilde * lambda) / (weight + alphaTilde);
    Vector3 const gradient = length > kLengthEpsilon ? Scale(separation, 1.0F / length) : Vector3{};
    Correction const correction{Scale(gradient, first.inverseMass * deltaLambda), deltaLambda,
                                Scale(gradient, -secondWeight * deltaLambda), 0.0F};
    if (!IsFinite(correction.first) || !IsFinite(correction.second) || !std::isfinite(deltaLambda))
    {
        return std::unexpected(Error::NonFiniteResult);
    }
    return correction;
}

std::expected<std::int32_t, Error> QuantizeCorrection(float correction)
{
    if (!std::isfinite(correction) || std::abs(correction) > kMaximumCorrection)
    {
        return std::unexpected(Error::AtomicRange);
    }
    return static_cast<std::int32_t>(std::round(correction * static_cast<float>(kAtomicScale)));
}

std::expected<Metrics, Error> Measure(std::span<Particle const> particles, std::span<Constraint const> constraints,
                                      std::span<float const> lambdas, float timeStep)
{
    if (particles.empty() || particles.size() > kMaximumParticles || constraints.empty() ||
        constraints.size() > kMaximumConstraints || lambdas.size() != constraints.size())
    {
        return std::unexpected(Error::InvalidCount);
    }
    if (!std::isfinite(timeStep) || timeStep < 1.0F / 240.0F || timeStep > 1.0F / 15.0F)
    {
        return std::unexpected(Error::InvalidConfiguration);
    }
    Metrics metrics{};
    for (std::size_t index = 0U; index < constraints.size(); ++index)
    {
        auto const &constraint = constraints[index];
        auto valid = ValidateConstraint(particles, constraint);
        if (!valid || !std::isfinite(lambdas[index]))
        {
            return std::unexpected(valid ? Error::NonFiniteInput : valid.error());
        }
        Vector3 const other =
            constraint.kind == ConstraintKind::Distance ? particles[constraint.second].position : constraint.anchor;
        float const value = Length(Subtract(particles[constraint.first].position, other)) - constraint.restLength;
        float const residual = std::abs(value + constraint.compliance / (timeStep * timeStep) * lambdas[index]);
        if (!std::isfinite(value) || !std::isfinite(residual))
        {
            return std::unexpected(Error::NonFiniteResult);
        }
        if (constraint.kind == ConstraintKind::Distance)
        {
            metrics.maximumDistanceError = std::max(metrics.maximumDistanceError, std::abs(value));
        }
        else
        {
            metrics.maximumAttachmentError = std::max(metrics.maximumAttachmentError, std::abs(value));
        }
        metrics.maximumCompliantResidual = std::max(metrics.maximumCompliantResidual, residual);
    }
    for (auto const &particle : particles)
    {
        if (!IsFinite(particle.position) || !IsFinite(particle.velocity) || !std::isfinite(particle.inverseMass) ||
            particle.inverseMass < 0.0F || particle.inverseMass > 100.0F)
        {
            return std::unexpected(Error::NonFiniteResult);
        }
        float const speed = Length(particle.velocity);
        metrics.maximumSpeed = std::max(metrics.maximumSpeed, speed);
        if (particle.inverseMass > 0.0F)
        {
            metrics.kineticEnergy += 0.5F * speed * speed / particle.inverseMass;
        }
    }
    if (!std::isfinite(metrics.maximumSpeed) || !std::isfinite(metrics.kineticEnergy))
    {
        return std::unexpected(Error::NonFiniteResult);
    }
    return metrics;
}

std::expected<StepResult, Error> SolveStep(Scene const &scene, Configuration const &configuration, bool project)
{
    auto valid = ValidateConfiguration(configuration, scene);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    StepResult result{};
    result.particles = scene.particles;
    result.lambdas.assign(scene.constraints.size(), 0.0F);
    for (auto &particle : result.particles)
    {
        if (particle.inverseMass == 0.0F)
        {
            particle.velocity = {};
            continue;
        }
        particle.velocity = Add(particle.velocity, Scale(configuration.gravity, configuration.timeStep));
        particle.position = Add(particle.position, Scale(particle.velocity, configuration.timeStep));
    }
    result.predicted = result.particles;
    auto before = Measure(result.particles, scene.constraints, result.lambdas, configuration.timeStep);
    if (!before)
    {
        return std::unexpected(before.error());
    }
    result.beforeProjection = *before;
    std::uint32_t maximumColor{};
    for (auto const &constraint : scene.constraints)
    {
        maximumColor = std::max(maximumColor, constraint.color);
    }
    for (std::uint32_t iteration = 0U; project && iteration < configuration.iterations; ++iteration)
    {
        if (configuration.strategy == Strategy::Colored)
        {
            for (std::uint32_t color = 0U; color <= maximumColor; ++color)
            {
                for (std::size_t index = 0U; index < scene.constraints.size(); ++index)
                {
                    auto const &constraint = scene.constraints[index];
                    if (constraint.color != color)
                    {
                        continue;
                    }
                    auto correction =
                        ProjectConstraint(result.particles, constraint, result.lambdas[index], configuration.timeStep);
                    if (!correction)
                    {
                        return std::unexpected(correction.error());
                    }
                    result.lambdas[index] += correction->deltaLambda;
                    if (result.particles[constraint.first].inverseMass > 0.0F)
                    {
                        auto &position = result.particles[constraint.first].position;
                        position = Add(position, correction->first);
                    }
                    if (constraint.kind == ConstraintKind::Distance &&
                        result.particles[constraint.second].inverseMass > 0.0F)
                    {
                        auto &position = result.particles[constraint.second].position;
                        position = Add(position, correction->second);
                    }
                }
            }
        }
        else
        {
            std::vector<Vector3> sums(result.particles.size());
            std::vector<std::array<std::int32_t, 3U>> integers(result.particles.size());
            for (std::size_t index = 0U; index < scene.constraints.size(); ++index)
            {
                auto const &constraint = scene.constraints[index];
                auto correction = ProjectConstraint(result.particles, constraint, result.lambdas[index],
                                                    configuration.timeStep, configuration.relaxation);
                if (!correction)
                {
                    return std::unexpected(correction.error());
                }
                result.lambdas[index] += correction->deltaLambda;
                std::array<std::uint32_t, 2U> const endpoints{constraint.first, constraint.second};
                std::array<Vector3, 2U> const values{correction->first, correction->second};
                std::uint32_t const count = constraint.kind == ConstraintKind::Distance ? 2U : 1U;
                for (std::uint32_t endpoint = 0U; endpoint < count; ++endpoint)
                {
                    auto const particle = endpoints[endpoint];
                    auto const value = values[endpoint];
                    if (configuration.strategy == Strategy::Jacobi)
                    {
                        sums[particle] = Add(sums[particle], value);
                        continue;
                    }
                    std::array<float, 3U> const components{value.x, value.y, value.z};
                    for (std::size_t component = 0U; component < components.size(); ++component)
                    {
                        auto quantized = QuantizeCorrection(components[component]);
                        if (!quantized)
                        {
                            return std::unexpected(quantized.error());
                        }
                        integers[particle][component] += *quantized;
                    }
                }
            }
            for (std::size_t particle = 0U; particle < result.particles.size(); ++particle)
            {
                if (configuration.strategy == Strategy::Atomic)
                {
                    auto const &value = integers[particle];
                    sums[particle] = {static_cast<float>(value[0]) / static_cast<float>(kAtomicScale),
                                      static_cast<float>(value[1]) / static_cast<float>(kAtomicScale),
                                      static_cast<float>(value[2]) / static_cast<float>(kAtomicScale)};
                }
                result.particles[particle].position = Add(result.particles[particle].position, sums[particle]);
            }
        }
        auto evidence = Measure(result.particles, scene.constraints, result.lambdas, configuration.timeStep);
        if (!evidence)
        {
            return std::unexpected(evidence.error());
        }
        result.iterations.push_back(*evidence);
    }
    for (std::size_t index = 0U; index < result.particles.size(); ++index)
    {
        auto &particle = result.particles[index];
        particle.velocity =
            particle.inverseMass == 0.0F
                ? Vector3{}
                : Scale(Subtract(particle.position, scene.particles[index].position), 1.0F / configuration.timeStep);
    }
    auto final = Measure(result.particles, scene.constraints, result.lambdas, configuration.timeStep);
    if (!final)
    {
        return std::unexpected(final.error());
    }
    result.final = *final;
    return result;
}

std::expected<Scene, Error> MakeDefaultScene(bool rootPinned, float massRatio, float compliance)
{
    if (!std::isfinite(massRatio) || massRatio < 1.0F || massRatio > 100.0F || !std::isfinite(compliance) ||
        compliance < 0.0F || compliance > 0.1F)
    {
        return std::unexpected(Error::InvalidConfiguration);
    }
    Scene scene{};
    for (std::uint32_t index = 0U; index < 12U; ++index)
    {
        float const x = -0.65F + static_cast<float>(index) * 0.115F;
        float const y = 0.35F + (index % 2U == 0U ? 0.02F : -0.02F);
        float inverseMass = index % 3U == 0U ? 1.0F / massRatio : 1.0F;
        if (index == 0U && rootPinned)
        {
            inverseMass = 0.0F;
        }
        scene.particles.push_back({{x, y, 0.0F}, inverseMass, {0.0F, index % 2U == 0U ? 0.2F : -1.0F, 0.0F}, 0U});
        if (index > 0U)
        {
            float const rest = Length(Subtract(scene.particles[index - 1U].position, scene.particles[index].position));
            scene.constraints.push_back({index - 1U,
                                         index,
                                         ConstraintKind::Distance,
                                         (index - 1U) % 2U,
                                         rest,
                                         compliance,
                                         0.0F,
                                         0.0F,
                                         {},
                                         0.0F});
        }
    }
    scene.constraints.push_back(
        {0U, 0U, ConstraintKind::Attachment, 2U, 0.0F, compliance, 0.0F, 0.0F, scene.particles.front().position, 0.0F});
    auto valid = ValidateScene(scene);
    if (!valid)
    {
        return std::unexpected(valid.error());
    }
    return scene;
}

} // namespace ch40::pbd
