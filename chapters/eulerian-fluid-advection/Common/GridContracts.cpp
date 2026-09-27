#include "GridContracts.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <utility>

namespace ch36::fluid
{
namespace
{
[[nodiscard]] auto ValidLayout(Layout layout) noexcept -> bool
{
    return layout == Layout::Cell || layout == Layout::XFace || layout == Layout::YFace;
}

[[nodiscard]] auto ValidBoundary(Boundary boundary) noexcept -> bool
{
    return boundary == Boundary::Solid || boundary == Boundary::Open || boundary == Boundary::Periodic;
}

[[nodiscard]] auto Offset(Extent extent, std::int32_t x, std::int32_t y) noexcept -> std::size_t
{
    auto const stride = static_cast<std::size_t>(extent.width) + 2U * kGhostWidth;
    return (static_cast<std::size_t>(y + 1) * stride) + static_cast<std::size_t>(x + 1);
}

[[nodiscard]] auto InsideGhosts(Extent extent, std::int32_t x, std::int32_t y) noexcept -> bool
{
    return x >= -1 && y >= -1 && x <= static_cast<std::int32_t>(extent.width) &&
           y <= static_cast<std::int32_t>(extent.height);
}
} // namespace

auto ValidateGrid(Grid grid) noexcept -> std::expected<void, ContractError>
{
    if (!std::isfinite(grid.cellSize))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (grid.width < 2U || grid.height < 2U || grid.width > kMaximumAxis || grid.height > kMaximumAxis ||
        grid.cellSize < 1.0e-6 || grid.cellSize > 1.0e6)
    {
        return std::unexpected(ContractError::InvalidGrid);
    }
    return {};
}

auto InteriorExtent(Grid grid, Layout layout) noexcept -> std::expected<Extent, ContractError>
{
    if (auto const valid = ValidateGrid(grid); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (!ValidLayout(layout))
    {
        return std::unexpected(ContractError::InvalidLayout);
    }
    return Extent{grid.width + static_cast<std::uint32_t>(layout == Layout::XFace),
                  grid.height + static_cast<std::uint32_t>(layout == Layout::YFace)};
}

auto MakeField(Grid grid, Layout layout, double initial) -> std::expected<Field, ContractError>
{
    auto const extent = InteriorExtent(grid, layout);
    if (!extent)
    {
        return std::unexpected(extent.error());
    }
    if (!std::isfinite(initial))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    auto const count = static_cast<std::size_t>(extent->width + 2U) * (extent->height + 2U);
    return Field{layout, std::vector<double>(count, initial)};
}

auto ValidateField(Grid grid, Field const &field) noexcept -> std::expected<void, ContractError>
{
    auto const extent = InteriorExtent(grid, field.layout);
    if (!extent)
    {
        return std::unexpected(extent.error());
    }
    auto const count = static_cast<std::size_t>(extent->width + 2U) * (extent->height + 2U);
    if (field.values.size() != count)
    {
        return std::unexpected(ContractError::InvalidFieldSize);
    }
    if (!std::all_of(field.values.begin(), field.values.end(), [](double value) { return std::isfinite(value); }))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return {};
}

auto Cell(Grid grid, Field const &field, std::int32_t x, std::int32_t y) -> std::expected<double, ContractError>
{
    if (auto const valid = ValidateField(grid, field); !valid)
    {
        return std::unexpected(valid.error());
    }
    auto const extent = *InteriorExtent(grid, field.layout);
    if (!InsideGhosts(extent, x, y))
    {
        return std::unexpected(ContractError::OutsideField);
    }
    return field.values[Offset(extent, x, y)];
}

auto SetCell(Grid grid, Field &field, std::int32_t x, std::int32_t y, double value)
    -> std::expected<void, ContractError>
{
    if (auto const valid = ValidateField(grid, field); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (!std::isfinite(value))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    auto const extent = *InteriorExtent(grid, field.layout);
    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(extent.width) || y >= static_cast<std::int32_t>(extent.height))
    {
        return std::unexpected(ContractError::OutsideField);
    }
    field.values[Offset(extent, x, y)] = value;
    return {};
}

auto FillGhosts(Grid grid, Field &field, Boundary boundary) -> std::expected<void, ContractError>
{
    if (auto const valid = ValidateField(grid, field); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (!ValidBoundary(boundary))
    {
        return std::unexpected(ContractError::InvalidBoundary);
    }
    auto const extent = *InteriorExtent(grid, field.layout);
    auto const width = static_cast<std::int32_t>(extent.width);
    auto const height = static_cast<std::int32_t>(extent.height);
    auto at = [&](std::int32_t x, std::int32_t y) -> double & { return field.values[Offset(extent, x, y)]; };

    if (boundary == Boundary::Solid && field.layout == Layout::XFace)
    {
        for (std::int32_t y = 0; y < height; ++y)
        {
            at(0, y) = 0.0;
            at(width - 1, y) = 0.0;
        }
    }
    if (boundary == Boundary::Solid && field.layout == Layout::YFace)
    {
        for (std::int32_t x = 0; x < width; ++x)
        {
            at(x, 0) = 0.0;
            at(x, height - 1) = 0.0;
        }
    }
    if (boundary == Boundary::Periodic && field.layout == Layout::XFace)
    {
        for (std::int32_t y = 0; y < height; ++y)
        {
            at(width - 1, y) = at(0, y);
        }
    }
    if (boundary == Boundary::Periodic && field.layout == Layout::YFace)
    {
        for (std::int32_t x = 0; x < width; ++x)
        {
            at(x, height - 1) = at(x, 0);
        }
    }

    auto const xPeriod = static_cast<std::int32_t>(grid.width);
    auto const yPeriod = static_cast<std::int32_t>(grid.height);
    for (std::int32_t y = 0; y < height; ++y)
    {
        at(-1, y) = at(boundary == Boundary::Periodic ? xPeriod - 1 : 0, y);
        at(width, y) = at(
            boundary == Boundary::Periodic ? static_cast<std::int32_t>(field.layout == Layout::XFace) : width - 1, y);
    }
    for (std::int32_t x = -1; x <= width; ++x)
    {
        at(x, -1) = at(x, boundary == Boundary::Periodic ? yPeriod - 1 : 0);
        at(x, height) = at(x, boundary == Boundary::Periodic ? static_cast<std::int32_t>(field.layout == Layout::YFace)
                                                             : height - 1);
    }
    return {};
}

namespace
{
[[nodiscard]] auto SamplePrepared(Grid grid, Field const &field, double worldX, double worldY)
    -> std::expected<double, ContractError>
{
    if (!std::isfinite(worldX) || !std::isfinite(worldY))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    auto const extent = *InteriorExtent(grid, field.layout);
    auto const x = worldX / grid.cellSize - (field.layout == Layout::XFace ? 0.0 : 0.5);
    auto const y = worldY / grid.cellSize - (field.layout == Layout::YFace ? 0.0 : 0.5);
    if (x < -1.0 || y < -1.0 || x > static_cast<double>(extent.width) || y > static_cast<double>(extent.height))
    {
        return std::unexpected(ContractError::OutsideSamplingDomain);
    }
    auto const x0 = static_cast<std::int32_t>(std::floor(x));
    auto const y0 = static_cast<std::int32_t>(std::floor(y));
    auto const x1 = std::min(x0 + 1, static_cast<std::int32_t>(extent.width));
    auto const y1 = std::min(y0 + 1, static_cast<std::int32_t>(extent.height));
    auto const fx = x - static_cast<double>(x0);
    auto const fy = y - static_cast<double>(y0);
    auto at = [&](std::int32_t i, std::int32_t j) { return field.values[Offset(extent, i, j)]; };
    auto const lower = std::lerp(at(x0, y0), at(x1, y0), fx);
    auto const upper = std::lerp(at(x0, y1), at(x1, y1), fx);
    return std::lerp(lower, upper, fy);
}
} // namespace

auto Sample(Grid grid, Field const &field, double worldX, double worldY) -> std::expected<double, ContractError>
{
    if (auto const valid = ValidateField(grid, field); !valid)
    {
        return std::unexpected(valid.error());
    }
    return SamplePrepared(grid, field, worldX, worldY);
}

auto MeasureFieldError(Grid grid, Field const &reference, Field const &observed)
    -> std::expected<FieldError, ContractError>
{
    for (auto const *field : {&reference, &observed})
    {
        if (auto const valid = ValidateField(grid, *field); !valid)
        {
            return std::unexpected(valid.error());
        }
    }
    if (reference.layout != observed.layout)
    {
        return std::unexpected(ContractError::InvalidLayout);
    }
    auto const extent = *InteriorExtent(grid, reference.layout);
    FieldError error{};
    double squaredSum = 0.0;
    for (std::int32_t y = 0; y < static_cast<std::int32_t>(extent.height); ++y)
    {
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(extent.width); ++x)
        {
            auto const index = Offset(extent, x, y);
            auto const difference = std::abs(reference.values[index] - observed.values[index]);
            error.meanAbsolute += difference;
            squaredSum += difference * difference;
            error.maximumAbsolute = std::max(error.maximumAbsolute, difference);
        }
    }
    auto const count = static_cast<double>(extent.width) * extent.height;
    error.meanAbsolute /= count;
    error.rootMeanSquare = std::sqrt(squaredSum / count);
    if (!std::isfinite(error.meanAbsolute) || !std::isfinite(error.rootMeanSquare) ||
        !std::isfinite(error.maximumAbsolute))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return error;
}

namespace
{
[[nodiscard]] auto Transport(Grid grid, Field const &source, Field const &velocityX, Field const &velocityY,
                             double timeStep) -> std::expected<Field, ContractError>
{
    auto result = *MakeField(grid, source.layout);
    auto const extent = *InteriorExtent(grid, source.layout);
    for (std::int32_t y = 0; y < static_cast<std::int32_t>(extent.height); ++y)
    {
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(extent.width); ++x)
        {
            auto const worldX = (static_cast<double>(x) + (source.layout == Layout::XFace ? 0.0 : 0.5)) * grid.cellSize;
            auto const worldY = (static_cast<double>(y) + (source.layout == Layout::YFace ? 0.0 : 0.5)) * grid.cellSize;
            auto const u = SamplePrepared(grid, velocityX, worldX, worldY);
            auto const v = SamplePrepared(grid, velocityY, worldX, worldY);
            if (!u || !v)
            {
                return std::unexpected(!u ? u.error() : v.error());
            }
            auto const transported = SamplePrepared(grid, source, worldX - timeStep * *u, worldY - timeStep * *v);
            if (!transported)
            {
                return std::unexpected(transported.error());
            }
            result.values[Offset(extent, x, y)] = *transported;
        }
    }
    return result;
}

[[nodiscard]] auto BoundedStep(Grid grid, Field const &velocityX, Field const &velocityY, double timeStep)
    -> std::expected<void, ContractError>
{
    if (!std::isfinite(timeStep))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (timeStep < 0.0)
    {
        return std::unexpected(ContractError::InvalidTimeStep);
    }
    for (auto const *field : {&velocityX, &velocityY})
    {
        auto const extent = *InteriorExtent(grid, field->layout);
        for (std::int32_t y = 0; y < static_cast<std::int32_t>(extent.height); ++y)
        {
            for (std::int32_t x = 0; x < static_cast<std::int32_t>(extent.width); ++x)
            {
                if (std::abs(field->values[Offset(extent, x, y)]) * timeStep > grid.cellSize)
                {
                    return std::unexpected(ContractError::InvalidTimeStep);
                }
            }
        }
    }
    return {};
}

[[nodiscard]] auto DepartureRange(Grid grid, Field const &scalar, double worldX, double worldY)
    -> std::pair<double, double>
{
    auto const extent = *InteriorExtent(grid, Layout::Cell);
    auto const x = worldX / grid.cellSize - 0.5;
    auto const y = worldY / grid.cellSize - 0.5;
    auto const x0 = static_cast<std::int32_t>(std::floor(x));
    auto const y0 = static_cast<std::int32_t>(std::floor(y));
    auto const x1 = std::min(x0 + 1, static_cast<std::int32_t>(extent.width));
    auto const y1 = std::min(y0 + 1, static_cast<std::int32_t>(extent.height));
    auto const values = {scalar.values[Offset(extent, x0, y0)], scalar.values[Offset(extent, x1, y0)],
                         scalar.values[Offset(extent, x0, y1)], scalar.values[Offset(extent, x1, y1)]};
    return {*std::min_element(values.begin(), values.end()), *std::max_element(values.begin(), values.end())};
}
} // namespace

auto AdvectScalar(Grid grid, Field const &scalar, Field const &velocityX, Field const &velocityY, double timeStep,
                  Boundary boundary, AdvectionMethod method) -> std::expected<AdvectionResult, ContractError>
{
    for (auto const *field : {&scalar, &velocityX, &velocityY})
    {
        if (auto const valid = ValidateField(grid, *field); !valid)
        {
            return std::unexpected(valid.error());
        }
    }
    if (scalar.layout != Layout::Cell || velocityX.layout != Layout::XFace || velocityY.layout != Layout::YFace)
    {
        return std::unexpected(ContractError::InvalidLayout);
    }
    if (!ValidBoundary(boundary))
    {
        return std::unexpected(ContractError::InvalidBoundary);
    }
    if (method != AdvectionMethod::SemiLagrangian && method != AdvectionMethod::Corrected)
    {
        return std::unexpected(ContractError::InvalidMethod);
    }
    auto source = scalar;
    auto u = velocityX;
    auto v = velocityY;
    for (auto *field : {&source, &u, &v})
    {
        if (auto const filled = FillGhosts(grid, *field, boundary); !filled)
        {
            return std::unexpected(filled.error());
        }
    }
    if (auto const bounded = BoundedStep(grid, u, v, timeStep); !bounded)
    {
        return std::unexpected(bounded.error());
    }

    auto forward = Transport(grid, source, u, v, timeStep);
    if (!forward)
    {
        return std::unexpected(forward.error());
    }
    AdvectionResult result{std::move(*forward)};
    auto const extent = *InteriorExtent(grid, Layout::Cell);
    if (method == AdvectionMethod::Corrected)
    {
        auto predicted = result.scalar;
        if (auto const filled = FillGhosts(grid, predicted, boundary); !filled)
        {
            return std::unexpected(filled.error());
        }
        auto reverse = Transport(grid, predicted, u, v, -timeStep);
        if (!reverse)
        {
            return std::unexpected(reverse.error());
        }
        for (std::int32_t y = 0; y < static_cast<std::int32_t>(grid.height); ++y)
        {
            for (std::int32_t x = 0; x < static_cast<std::int32_t>(grid.width); ++x)
            {
                auto const worldX = (static_cast<double>(x) + 0.5) * grid.cellSize;
                auto const worldY = (static_cast<double>(y) + 0.5) * grid.cellSize;
                auto const uAtCell = SamplePrepared(grid, u, worldX, worldY);
                auto const vAtCell = SamplePrepared(grid, v, worldX, worldY);
                if (!uAtCell || !vAtCell)
                {
                    return std::unexpected(!uAtCell ? uAtCell.error() : vAtCell.error());
                }
                auto const [minimum, maximum] =
                    DepartureRange(grid, source, worldX - timeStep * *uAtCell, worldY - timeStep * *vAtCell);
                auto const index = Offset(extent, x, y);
                auto const corrected =
                    result.scalar.values[index] + 0.5 * (source.values[index] - reverse->values[index]);
                auto const bounded = std::clamp(corrected, minimum, maximum);
                result.clampedCells += static_cast<std::uint32_t>(bounded != corrected);
                result.scalar.values[index] = bounded;
            }
        }
    }
    if (auto const filled = FillGhosts(grid, result.scalar, boundary); !filled)
    {
        return std::unexpected(filled.error());
    }
    for (std::int32_t y = 0; y < static_cast<std::int32_t>(grid.height); ++y)
    {
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(grid.width); ++x)
        {
            auto const index = Offset(extent, x, y);
            auto const a = source.values[index];
            auto const b = result.scalar.values[index];
            auto const area = grid.cellSize * grid.cellSize;
            result.sourceMass += a * area;
            result.outputMass += b * area;
            result.sourceEnergy += a * a * area;
            result.outputEnergy += b * b * area;
        }
    }
    if (!std::isfinite(result.sourceMass) || !std::isfinite(result.outputMass) || !std::isfinite(result.sourceEnergy) ||
        !std::isfinite(result.outputEnergy))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    return result;
}

auto AdvectVelocity(Grid grid, Field const &velocityX, Field const &velocityY, double timeStep, Boundary boundary)
    -> std::expected<VelocityFields, ContractError>
{
    for (auto const *field : {&velocityX, &velocityY})
    {
        if (auto const valid = ValidateField(grid, *field); !valid)
        {
            return std::unexpected(valid.error());
        }
    }
    if (velocityX.layout != Layout::XFace || velocityY.layout != Layout::YFace)
    {
        return std::unexpected(ContractError::InvalidLayout);
    }
    if (!ValidBoundary(boundary))
    {
        return std::unexpected(ContractError::InvalidBoundary);
    }
    VelocityFields source{velocityX, velocityY};
    for (auto *field : {&source.x, &source.y})
    {
        if (auto const filled = FillGhosts(grid, *field, boundary); !filled)
        {
            return std::unexpected(filled.error());
        }
    }
    if (auto const bounded = BoundedStep(grid, source.x, source.y, timeStep); !bounded)
    {
        return std::unexpected(bounded.error());
    }
    auto x = Transport(grid, source.x, source.x, source.y, timeStep);
    if (!x)
    {
        return std::unexpected(x.error());
    }
    auto y = Transport(grid, source.y, source.x, source.y, timeStep);
    if (!y)
    {
        return std::unexpected(y.error());
    }
    VelocityFields result{std::move(*x), std::move(*y)};
    for (auto *field : {&result.x, &result.y})
    {
        if (auto const filled = FillGhosts(grid, *field, boundary); !filled)
        {
            return std::unexpected(filled.error());
        }
    }
    return result;
}

auto ApplyUniformForce(Grid grid, Field const &velocityX, Field const &velocityY, double accelerationX,
                       double accelerationY, double timeStep, Boundary boundary)
    -> std::expected<VelocityFields, ContractError>
{
    for (auto const *field : {&velocityX, &velocityY})
    {
        if (auto const valid = ValidateField(grid, *field); !valid)
        {
            return std::unexpected(valid.error());
        }
    }
    if (velocityX.layout != Layout::XFace || velocityY.layout != Layout::YFace)
    {
        return std::unexpected(ContractError::InvalidLayout);
    }
    if (!ValidBoundary(boundary))
    {
        return std::unexpected(ContractError::InvalidBoundary);
    }
    if (!std::isfinite(accelerationX) || !std::isfinite(accelerationY) || !std::isfinite(timeStep))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (timeStep < 0.0)
    {
        return std::unexpected(ContractError::InvalidTimeStep);
    }
    VelocityFields result{velocityX, velocityY};
    for (auto [field, acceleration] : {std::pair{&result.x, accelerationX}, std::pair{&result.y, accelerationY}})
    {
        auto const extent = *InteriorExtent(grid, field->layout);
        for (std::int32_t y = 0; y < static_cast<std::int32_t>(extent.height); ++y)
        {
            for (std::int32_t x = 0; x < static_cast<std::int32_t>(extent.width); ++x)
            {
                auto &value = field->values[Offset(extent, x, y)];
                value += acceleration * timeStep;
                if (!std::isfinite(value))
                {
                    return std::unexpected(ContractError::NonFinite);
                }
            }
        }
        if (auto const filled = FillGhosts(grid, *field, boundary); !filled)
        {
            return std::unexpected(filled.error());
        }
    }
    return result;
}

} // namespace ch36::fluid
