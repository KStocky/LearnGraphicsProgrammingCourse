#include "ProjectionContracts.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <limits>
#include <utility>

namespace ch37::projection
{
namespace
{
[[nodiscard]] auto ValidBoundary(Boundary boundary) noexcept -> bool
{
    return boundary == Boundary::Solid || boundary == Boundary::Open || boundary == Boundary::Periodic;
}

[[nodiscard]] auto Width(Grid grid, Layout layout) noexcept -> std::uint32_t
{
    return grid.nx + static_cast<std::uint32_t>(layout == Layout::XFace);
}

[[nodiscard]] auto Height(Grid grid, Layout layout) noexcept -> std::uint32_t
{
    return grid.ny + static_cast<std::uint32_t>(layout == Layout::YFace);
}

[[nodiscard]] auto Index(std::uint32_t width, int x, int y) noexcept -> std::size_t
{
    return static_cast<std::size_t>(y + 1) * (width + 2U) + static_cast<std::size_t>(x + 1);
}

[[nodiscard]] auto At(Field const &field, Grid grid, int x, int y) noexcept -> double
{
    return field.values[Index(Width(grid, field.layout), x, y)];
}

auto At(Field &field, Grid grid, int x, int y) noexcept -> double &
{
    return field.values[Index(Width(grid, field.layout), x, y)];
}

[[nodiscard]] auto ValidateAs(Grid grid, Field const &field, Layout layout) noexcept -> std::expected<void, Error>
{
    if (auto const valid = ValidateField(grid, field); !valid)
    {
        return valid;
    }
    if (field.layout != layout)
    {
        return std::unexpected(Error::InvalidLayout);
    }
    return {};
}

[[nodiscard]] auto ValidateVelocity(Grid grid, Velocity const &velocity) noexcept -> std::expected<void, Error>
{
    if (auto const valid = ValidateAs(grid, velocity.x, Layout::XFace); !valid)
    {
        return valid;
    }
    return ValidateAs(grid, velocity.y, Layout::YFace);
}

[[nodiscard]] auto MakeCells(Grid grid) -> Field
{
    return Field{Layout::Cell, std::vector<double>(static_cast<std::size_t>(grid.nx + 2U) * (grid.ny + 2U), 0.0)};
}

template <class F> void EachCell(Grid grid, F &&fn)
{
    for (int y = 0; y < static_cast<int>(grid.ny); ++y)
    {
        for (int x = 0; x < static_cast<int>(grid.nx); ++x)
        {
            fn(x, y);
        }
    }
}

[[nodiscard]] auto Finite(Field const &field) noexcept -> bool
{
    return std::all_of(field.values.begin(), field.values.end(), [](double v) { return std::isfinite(v); });
}

[[nodiscard]] auto Rms(Grid grid, Field const &field) noexcept -> double
{
    double sum = 0.0;
    EachCell(grid,
             [&](int x, int y)
             {
                 auto const v = At(field, grid, x, y);
                 sum += v * v;
             });
    return std::sqrt(sum / static_cast<double>(grid.nx * grid.ny));
}

[[nodiscard]] auto Mean(Grid grid, Field const &field) noexcept -> double
{
    double sum = 0.0;
    EachCell(grid, [&](int x, int y) { sum += At(field, grid, x, y); });
    return sum / static_cast<double>(grid.nx * grid.ny);
}

void RemoveMean(Grid grid, Field &field)
{
    auto const mean = Mean(grid, field);
    EachCell(grid, [&](int x, int y) { At(field, grid, x, y) -= mean; });
}

void FillPressureGhosts(Grid grid, Field &pressure, Boundary boundary)
{
    auto const nx = static_cast<int>(grid.nx);
    auto const ny = static_cast<int>(grid.ny);
    for (int y = 0; y < ny; ++y)
    {
        At(pressure, grid, -1, y) =
            boundary == Boundary::Open ? 0.0 : At(pressure, grid, boundary == Boundary::Periodic ? nx - 1 : 0, y);
        At(pressure, grid, nx, y) =
            boundary == Boundary::Open ? 0.0 : At(pressure, grid, boundary == Boundary::Periodic ? 0 : nx - 1, y);
    }
    for (int x = -1; x <= nx; ++x)
    {
        At(pressure, grid, x, -1) =
            boundary == Boundary::Open ? 0.0 : At(pressure, grid, x, boundary == Boundary::Periodic ? ny - 1 : 0);
        At(pressure, grid, x, ny) =
            boundary == Boundary::Open ? 0.0 : At(pressure, grid, x, boundary == Boundary::Periodic ? 0 : ny - 1);
    }
}

[[nodiscard]] auto Dot(Grid grid, Field const &a, Field const &b) noexcept -> double
{
    double sum = 0.0;
    EachCell(grid, [&](int x, int y) { sum += At(a, grid, x, y) * At(b, grid, x, y); });
    return sum;
}

[[nodiscard]] auto Neighbor(Grid grid, Field const &p, Boundary boundary, int x, int y, int cx, int cy) noexcept
    -> double
{
    if (x >= 0 && x < static_cast<int>(grid.nx) && y >= 0 && y < static_cast<int>(grid.ny))
    {
        return At(p, grid, x, y);
    }
    if (boundary == Boundary::Solid)
    {
        return At(p, grid, cx, cy);
    }
    if (boundary == Boundary::Open)
    {
        return 0.0;
    }
    auto const wrapX = (x + static_cast<int>(grid.nx)) % static_cast<int>(grid.nx);
    auto const wrapY = (y + static_cast<int>(grid.ny)) % static_cast<int>(grid.ny);
    return At(p, grid, wrapX, wrapY);
}

[[nodiscard]] auto Stencil(Grid grid, Field const &p, Boundary boundary, int x, int y) noexcept -> double
{
    auto const sum = Neighbor(grid, p, boundary, x - 1, y, x, y) + Neighbor(grid, p, boundary, x + 1, y, x, y) +
                     Neighbor(grid, p, boundary, x, y - 1, x, y) + Neighbor(grid, p, boundary, x, y + 1, x, y);
    return (4.0 * At(p, grid, x, y) - sum) / (grid.h * grid.h);
}

[[nodiscard]] auto Diagonal(Grid grid, Boundary boundary, int x, int y) noexcept -> double
{
    auto const walls = static_cast<int>(x == 0) + static_cast<int>(y == 0) +
                       static_cast<int>(x + 1 == static_cast<int>(grid.nx)) +
                       static_cast<int>(y + 1 == static_cast<int>(grid.ny));
    return static_cast<double>(4 - (boundary == Boundary::Solid ? walls : 0)) / (grid.h * grid.h);
}

[[nodiscard]] auto Operator(Grid grid, Field const &p, Boundary boundary) -> Field
{
    auto out = MakeCells(grid);
    EachCell(grid, [&](int x, int y) { At(out, grid, x, y) = Stencil(grid, p, boundary, x, y); });
    return out;
}

[[nodiscard]] auto ValidSettings(Grid grid, Boundary boundary, Settings settings) noexcept -> std::expected<void, Error>
{
    if (settings.method != Method::Jacobi && settings.method != Method::RedBlack &&
        settings.method != Method::ConjugateGradient)
    {
        return std::unexpected(Error::InvalidMethod);
    }
    if (settings.stop != Stop::FixedIterations && settings.stop != Stop::Tolerance)
    {
        return std::unexpected(Error::InvalidSettings);
    }
    if (!std::isfinite(settings.tolerance))
    {
        return std::unexpected(Error::NonFinite);
    }
    if (settings.maxIterations == 0U || settings.maxIterations > 100000U || settings.tolerance <= 0.0 ||
        settings.tolerance >= 1.0)
    {
        return std::unexpected(Error::InvalidSettings);
    }
    if (settings.method == Method::RedBlack && boundary == Boundary::Periodic &&
        (grid.nx % 2U != 0U || grid.ny % 2U != 0U))
    {
        return std::unexpected(Error::UnsupportedColouring);
    }
    return {};
}
} // namespace

auto ValidateGrid(Grid grid) noexcept -> std::expected<void, Error>
{
    if (!std::isfinite(grid.h))
    {
        return std::unexpected(Error::NonFinite);
    }
    if (grid.nx < 2U || grid.ny < 2U || grid.nx > 64U || grid.ny > 64U || grid.h < 1.0e-6 || grid.h > 1.0e6)
    {
        return std::unexpected(Error::InvalidGrid);
    }
    return {};
}

auto MakeField(Grid grid, Layout layout, double initial) -> std::expected<Field, Error>
{
    if (auto const valid = ValidateGrid(grid); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (layout != Layout::Cell && layout != Layout::XFace && layout != Layout::YFace)
    {
        return std::unexpected(Error::InvalidLayout);
    }
    if (!std::isfinite(initial))
    {
        return std::unexpected(Error::NonFinite);
    }
    return Field{
        layout,
        std::vector<double>(static_cast<std::size_t>(Width(grid, layout) + 2U) * (Height(grid, layout) + 2U), initial)};
}

auto ValidateField(Grid grid, Field const &field) noexcept -> std::expected<void, Error>
{
    if (auto const valid = ValidateGrid(grid); !valid)
    {
        return valid;
    }
    if (field.layout != Layout::Cell && field.layout != Layout::XFace && field.layout != Layout::YFace)
    {
        return std::unexpected(Error::InvalidLayout);
    }
    if (field.values.size() !=
        static_cast<std::size_t>(Width(grid, field.layout) + 2U) * (Height(grid, field.layout) + 2U))
    {
        return std::unexpected(Error::InvalidFieldSize);
    }
    if (!Finite(field))
    {
        return std::unexpected(Error::NonFinite);
    }
    return {};
}

auto NormalizeVelocity(Grid grid, Velocity const &velocity, Boundary boundary) -> std::expected<Velocity, Error>
{
    if (auto const valid = ValidateVelocity(grid, velocity); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (!ValidBoundary(boundary))
    {
        return std::unexpected(Error::InvalidBoundary);
    }
    auto result = velocity;
    if (boundary == Boundary::Solid)
    {
        for (int y = 0; y < static_cast<int>(grid.ny); ++y)
        {
            At(result.x, grid, 0, y) = 0.0;
            At(result.x, grid, static_cast<int>(grid.nx), y) = 0.0;
        }
        for (int x = 0; x < static_cast<int>(grid.nx); ++x)
        {
            At(result.y, grid, x, 0) = 0.0;
            At(result.y, grid, x, static_cast<int>(grid.ny)) = 0.0;
        }
    }
    else if (boundary == Boundary::Periodic)
    {
        for (int y = 0; y < static_cast<int>(grid.ny); ++y)
        {
            At(result.x, grid, static_cast<int>(grid.nx), y) = At(result.x, grid, 0, y);
        }
        for (int x = 0; x < static_cast<int>(grid.nx); ++x)
        {
            At(result.y, grid, x, static_cast<int>(grid.ny)) = At(result.y, grid, x, 0);
        }
    }
    for (auto *field : {&result.x, &result.y})
    {
        auto const width = static_cast<int>(Width(grid, field->layout));
        auto const height = static_cast<int>(Height(grid, field->layout));
        for (int y = 0; y < height; ++y)
        {
            At(*field, grid, -1, y) =
                At(*field, grid, boundary == Boundary::Periodic ? static_cast<int>(grid.nx) - 1 : 0, y);
            At(*field, grid, width, y) =
                At(*field, grid,
                   boundary == Boundary::Periodic ? static_cast<int>(field->layout == Layout::XFace) : width - 1, y);
        }
        for (int x = -1; x <= width; ++x)
        {
            At(*field, grid, x, -1) =
                At(*field, grid, x, boundary == Boundary::Periodic ? static_cast<int>(grid.ny) - 1 : 0);
            At(*field, grid, x, height) =
                At(*field, grid, x,
                   boundary == Boundary::Periodic ? static_cast<int>(field->layout == Layout::YFace) : height - 1);
        }
    }
    return result;
}

auto Divergence(Grid grid, Velocity const &velocity, Boundary boundary) -> std::expected<Field, Error>
{
    auto const normalized = NormalizeVelocity(grid, velocity, boundary);
    if (!normalized)
    {
        return std::unexpected(normalized.error());
    }
    auto result = MakeCells(grid);
    EachCell(grid,
             [&](int x, int y)
             {
                 At(result, grid, x, y) = (At(normalized->x, grid, x + 1, y) - At(normalized->x, grid, x, y) +
                                           At(normalized->y, grid, x, y + 1) - At(normalized->y, grid, x, y)) /
                                          grid.h;
             });
    if (!Finite(result))
    {
        return std::unexpected(Error::NonFinite);
    }
    return result;
}

auto ApplyOperator(Grid grid, Field const &pressure, Boundary boundary) -> std::expected<Field, Error>
{
    if (auto const valid = ValidateAs(grid, pressure, Layout::Cell); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (!ValidBoundary(boundary))
    {
        return std::unexpected(Error::InvalidBoundary);
    }
    auto result = Operator(grid, pressure, boundary);
    if (!Finite(result))
    {
        return std::unexpected(Error::NonFinite);
    }
    return result;
}

auto Residual(Grid grid, Field const &pressure, Field const &rhs, Boundary boundary) -> std::expected<Field, Error>
{
    if (auto const valid = ValidateAs(grid, rhs, Layout::Cell); !valid)
    {
        return std::unexpected(valid.error());
    }
    auto result = ApplyOperator(grid, pressure, boundary);
    if (!result)
    {
        return std::unexpected(result.error());
    }
    EachCell(grid, [&](int x, int y) { At(*result, grid, x, y) = At(rhs, grid, x, y) - At(*result, grid, x, y); });
    if (!Finite(*result))
    {
        return std::unexpected(Error::NonFinite);
    }
    return result;
}

auto RootMeanSquare(Grid grid, Field const &cells) -> std::expected<double, Error>
{
    if (auto const valid = ValidateAs(grid, cells, Layout::Cell); !valid)
    {
        return std::unexpected(valid.error());
    }
    auto const rms = Rms(grid, cells);
    if (!std::isfinite(rms))
    {
        return std::unexpected(Error::NonFinite);
    }
    return rms;
}

auto Solve(Grid grid, Field const &rhs, Boundary boundary, Settings settings) -> std::expected<SolveResult, Error>
{
    if (auto const valid = ValidateAs(grid, rhs, Layout::Cell); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (!ValidBoundary(boundary))
    {
        return std::unexpected(Error::InvalidBoundary);
    }
    if (auto const valid = ValidSettings(grid, boundary, settings); !valid)
    {
        return std::unexpected(valid.error());
    }
    auto const initial = Rms(grid, rhs);
    if (!std::isfinite(initial))
    {
        return std::unexpected(Error::NonFinite);
    }
    if (boundary != Boundary::Open && initial != 0.0 && std::abs(Mean(grid, rhs)) > 1.0e-10 * initial)
    {
        return std::unexpected(Error::IncompatibleSource);
    }
    SolveResult result{MakeCells(grid), 0U, initial == 0.0, {initial}};
    if (initial == 0.0)
    {
        FillPressureGhosts(grid, result.pressure, boundary);
        return result;
    }
    // Remove representational mean before iteration, to keep the closed-system
    // residual orthogonal to the constant nullspace even at roundoff scale.
    auto source = rhs;
    if (boundary != Boundary::Open)
    {
        RemoveMean(grid, source);
    }
    auto residual = source;
    auto cgResidual = source;
    auto direction = source;
    auto const threshold = settings.tolerance * initial;
    double previousNorm = Dot(grid, residual, residual);
    for (std::uint32_t iteration = 0; iteration < settings.maxIterations; ++iteration)
    {
        if (settings.method == Method::ConjugateGradient)
        {
            auto const applied = Operator(grid, direction, boundary);
            auto const denominator = Dot(grid, direction, applied);
            if (!std::isfinite(denominator) || denominator <= 0.0 || !std::isfinite(previousNorm))
            {
                return std::unexpected(Error::Breakdown);
            }
            auto const alpha = previousNorm / denominator;
            EachCell(grid,
                     [&](int x, int y)
                     {
                         At(result.pressure, grid, x, y) += alpha * At(direction, grid, x, y);
                         At(cgResidual, grid, x, y) -= alpha * At(applied, grid, x, y);
                     });
            auto const nextNorm = Dot(grid, cgResidual, cgResidual);
            if (!std::isfinite(nextNorm))
            {
                return std::unexpected(Error::NonFinite);
            }
            if (nextNorm > 0.0)
            {
                auto const beta = nextNorm / previousNorm;
                EachCell(
                    grid, [&](int x, int y)
                    { At(direction, grid, x, y) = At(cgResidual, grid, x, y) + beta * At(direction, grid, x, y); });
            }
            previousNorm = nextNorm;
        }
        else if (settings.method == Method::Jacobi)
        {
            auto next = result.pressure;
            EachCell(grid,
                     [&](int x, int y)
                     {
                         At(next, grid, x, y) =
                             At(result.pressure, grid, x, y) +
                             (At(source, grid, x, y) - Stencil(grid, result.pressure, boundary, x, y)) /
                                 Diagonal(grid, boundary, x, y);
                     });
            result.pressure = std::move(next);
        }
        else
        {
            for (int colour = 0; colour < 2; ++colour)
            {
                EachCell(grid,
                         [&](int x, int y)
                         {
                             if ((x + y) % 2 == colour)
                             {
                                 At(result.pressure, grid, x, y) +=
                                     (At(source, grid, x, y) - Stencil(grid, result.pressure, boundary, x, y)) /
                                     Diagonal(grid, boundary, x, y);
                             }
                         });
            }
        }
        if (boundary != Boundary::Open)
        {
            RemoveMean(grid, result.pressure);
        }
        if (!Finite(result.pressure))
        {
            return std::unexpected(Error::NonFinite);
        }
        // Measure the true (rather than recursively estimated CG) residual.
        auto const applied = Operator(grid, result.pressure, boundary);
        EachCell(grid,
                 [&](int x, int y) { At(residual, grid, x, y) = At(source, grid, x, y) - At(applied, grid, x, y); });
        auto const norm = Rms(grid, residual);
        if (!std::isfinite(norm))
        {
            return std::unexpected(Error::NonFinite);
        }
        result.residualHistory.push_back(norm);
        ++result.iterations;
        result.converged = norm <= threshold;
        if (result.converged)
        {
            if (settings.stop == Stop::Tolerance || settings.method == Method::ConjugateGradient)
            {
                break;
            }
        }
    }
    if (settings.stop == Stop::Tolerance && !result.converged)
    {
        return std::unexpected(Error::NotConverged);
    }
    FillPressureGhosts(grid, result.pressure, boundary);
    return result;
}

auto ApplyGradient(Grid grid, Velocity const &velocity, Field const &pressure, double dt, Boundary boundary)
    -> std::expected<Velocity, Error>
{
    auto normalized = NormalizeVelocity(grid, velocity, boundary);
    if (!normalized)
    {
        return std::unexpected(normalized.error());
    }
    if (auto const valid = ValidateAs(grid, pressure, Layout::Cell); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (!std::isfinite(dt))
    {
        return std::unexpected(Error::NonFinite);
    }
    if (dt <= 0.0)
    {
        return std::unexpected(Error::InvalidSettings);
    }
    for (int y = 0; y < static_cast<int>(grid.ny); ++y)
    {
        for (int x = 0; x <= static_cast<int>(grid.nx); ++x)
        {
            if (boundary == Boundary::Solid && (x == 0 || x == static_cast<int>(grid.nx)))
            {
                continue;
            }
            auto const left =
                Neighbor(grid, pressure, boundary, x - 1, y, std::clamp(x - 1, 0, static_cast<int>(grid.nx) - 1), y);
            auto const right =
                Neighbor(grid, pressure, boundary, x, y, std::clamp(x, 0, static_cast<int>(grid.nx) - 1), y);
            At(normalized->x, grid, x, y) -= dt * (right - left) / grid.h;
        }
    }
    for (int y = 0; y <= static_cast<int>(grid.ny); ++y)
    {
        for (int x = 0; x < static_cast<int>(grid.nx); ++x)
        {
            if (boundary == Boundary::Solid && (y == 0 || y == static_cast<int>(grid.ny)))
            {
                continue;
            }
            auto const bottom =
                Neighbor(grid, pressure, boundary, x, y - 1, x, std::clamp(y - 1, 0, static_cast<int>(grid.ny) - 1));
            auto const top =
                Neighbor(grid, pressure, boundary, x, y, x, std::clamp(y, 0, static_cast<int>(grid.ny) - 1));
            At(normalized->y, grid, x, y) -= dt * (top - bottom) / grid.h;
        }
    }
    if (!Finite(normalized->x) || !Finite(normalized->y))
    {
        return std::unexpected(Error::NonFinite);
    }
    return NormalizeVelocity(grid, *normalized, boundary);
}

auto Project(Grid grid, Velocity const &velocity, double dt, Boundary boundary, Settings settings)
    -> std::expected<ProjectionResult, Error>
{
    if (!std::isfinite(dt))
    {
        return std::unexpected(Error::NonFinite);
    }
    if (dt <= 0.0)
    {
        return std::unexpected(Error::InvalidSettings);
    }
    auto divergence = Divergence(grid, velocity, boundary);
    if (!divergence)
    {
        return std::unexpected(divergence.error());
    }
    auto const before = Rms(grid, *divergence);
    EachCell(grid, [&](int x, int y) { At(*divergence, grid, x, y) = -At(*divergence, grid, x, y) / dt; });
    if (!Finite(*divergence))
    {
        return std::unexpected(Error::NonFinite);
    }
    auto solved = Solve(grid, *divergence, boundary, settings);
    if (!solved)
    {
        return std::unexpected(solved.error());
    }
    auto corrected = ApplyGradient(grid, velocity, solved->pressure, dt, boundary);
    if (!corrected)
    {
        return std::unexpected(corrected.error());
    }
    auto after = Divergence(grid, *corrected, boundary);
    if (!after)
    {
        return std::unexpected(after.error());
    }
    auto const afterRms = Rms(grid, *after);
    if (!std::isfinite(afterRms))
    {
        return std::unexpected(Error::NonFinite);
    }
    return ProjectionResult{std::move(solved->pressure),
                            std::move(*corrected),
                            solved->iterations,
                            solved->converged,
                            std::move(solved->residualHistory),
                            before,
                            afterRms};
}
} // namespace ch37::projection
