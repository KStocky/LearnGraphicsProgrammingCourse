#pragma once

#include <cstdint>
#include <expected>
#include <vector>

namespace ch37::projection
{

enum class Error : std::uint8_t
{
    NonFinite,
    InvalidGrid,
    InvalidLayout,
    InvalidFieldSize,
    InvalidBoundary,
    InvalidSettings,
    InvalidMethod,
    IncompatibleSource,
    UnsupportedColouring,
    Breakdown,
    NotConverged,
};

enum class Layout : std::uint8_t
{
    Cell,
    XFace,
    YFace,
};

enum class Boundary : std::uint8_t
{
    Solid,
    Open,
    Periodic,
};

enum class Method : std::uint8_t
{
    Jacobi,
    RedBlack,
    ConjugateGradient,
};

enum class Stop : std::uint8_t
{
    FixedIterations,
    Tolerance,
};

struct Grid final
{
    std::uint32_t nx{};
    std::uint32_t ny{};
    double h{};
};

// Interior coordinates are zero-based; one ghost ring surrounds each field.
// Storage is x-fastest: (y + 1) * (interiorWidth + 2) + x + 1.
struct Field final
{
    Layout layout{Layout::Cell};
    std::vector<double> values{};
};

struct Velocity final
{
    Field x{};
    Field y{};
};

struct Settings final
{
    Method method{Method::ConjugateGradient};
    Stop stop{Stop::Tolerance};
    std::uint32_t maxIterations{1000U};
    // For tolerance stopping, residual RMS <= tolerance * initial residual RMS.
    // Fixed Jacobi/red-black run maxIterations (unless the initial RHS is zero);
    // CG may terminate early on convergence to avoid a zero search direction.
    double tolerance{1.0e-8};
};

struct SolveResult final
{
    Field pressure{};
    std::uint32_t iterations{};
    bool converged{};
    // Includes the initial residual (index zero), then one entry per iteration.
    std::vector<double> residualHistory{};
};

struct ProjectionResult final
{
    Field pressure{};
    Velocity corrected{};
    std::uint32_t iterations{};
    bool converged{};
    std::vector<double> residualHistory{};
    double divergenceBefore{};
    double divergenceAfter{};
};

[[nodiscard]] auto ValidateGrid(Grid grid) noexcept -> std::expected<void, Error>;
[[nodiscard]] auto MakeField(Grid grid, Layout layout, double initial = 0.0) -> std::expected<Field, Error>;
[[nodiscard]] auto ValidateField(Grid grid, Field const &field) noexcept -> std::expected<void, Error>;
[[nodiscard]] auto NormalizeVelocity(Grid grid, Velocity const &velocity, Boundary boundary)
    -> std::expected<Velocity, Error>;
[[nodiscard]] auto Divergence(Grid grid, Velocity const &velocity, Boundary boundary) -> std::expected<Field, Error>;
// A = -Laplacian. Solid: zero normal derivative; open: pressure ghost = 0;
// periodic: opposite pressure cell. Pressure ghosts in the input are ignored.
[[nodiscard]] auto ApplyOperator(Grid grid, Field const &pressure, Boundary boundary) -> std::expected<Field, Error>;
[[nodiscard]] auto Residual(Grid grid, Field const &pressure, Field const &rhs, Boundary boundary)
    -> std::expected<Field, Error>;
[[nodiscard]] auto RootMeanSquare(Grid grid, Field const &cells) -> std::expected<double, Error>;
// Closed systems require |mean(rhs)| <= 1e-10 * RMS(rhs), covering rounding
// in the conservative face differences without accepting a meaningful source.
// Their pressure gauge is fixed by subtracting its interior arithmetic mean.
[[nodiscard]] auto Solve(Grid grid, Field const &rhs, Boundary boundary, Settings settings)
    -> std::expected<SolveResult, Error>;
[[nodiscard]] auto ApplyGradient(Grid grid, Velocity const &velocity, Field const &pressure, double dt,
                                 Boundary boundary) -> std::expected<Velocity, Error>;
// b = -div(u)/dt and u' = u - dt grad(p).
[[nodiscard]] auto Project(Grid grid, Velocity const &velocity, double dt, Boundary boundary, Settings settings)
    -> std::expected<ProjectionResult, Error>;

} // namespace ch37::projection
