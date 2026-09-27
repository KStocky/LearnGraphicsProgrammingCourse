#pragma once

#include <cstdint>
#include <expected>
#include <vector>

namespace ch36::fluid
{

inline constexpr std::uint32_t kMaximumAxis = 64U;
inline constexpr std::uint32_t kGhostWidth = 1U;

enum class ContractError : std::uint8_t
{
    NonFinite,
    InvalidGrid,
    InvalidLayout,
    InvalidBoundary,
    InvalidFieldSize,
    OutsideField,
    OutsideSamplingDomain,
    InvalidTimeStep,
    InvalidMethod,
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

enum class AdvectionMethod : std::uint8_t
{
    SemiLagrangian,
    Corrected,
};

struct Grid final
{
    std::uint32_t width{};
    std::uint32_t height{};
    double cellSize{};
};

struct Field final
{
    Layout layout{Layout::Cell};
    // One ghost cell on each side of the interior, stored with x varying fastest.
    std::vector<double> values{};
};

struct Extent final
{
    std::uint32_t width{};
    std::uint32_t height{};
};

[[nodiscard]] auto ValidateGrid(Grid grid) noexcept -> std::expected<void, ContractError>;
[[nodiscard]] auto InteriorExtent(Grid grid, Layout layout) noexcept -> std::expected<Extent, ContractError>;
[[nodiscard]] auto MakeField(Grid grid, Layout layout, double initial = 0.0) -> std::expected<Field, ContractError>;
[[nodiscard]] auto ValidateField(Grid grid, Field const &field) noexcept -> std::expected<void, ContractError>;
[[nodiscard]] auto Cell(Grid grid, Field const &field, std::int32_t x, std::int32_t y)
    -> std::expected<double, ContractError>;
[[nodiscard]] auto SetCell(Grid grid, Field &field, std::int32_t x, std::int32_t y, double value)
    -> std::expected<void, ContractError>;

// Solid boundaries zero only the normal face velocity; tangential faces and
// scalar cells have zero normal gradient. Open boundaries extrapolate every
// component; periodic boundaries identify opposite face values.
[[nodiscard]] auto FillGhosts(Grid grid, Field &field, Boundary boundary) -> std::expected<void, ContractError>;
// World coordinates use cell centres at (i+1/2)h, x-faces at (i,j+1/2)h,
// and y-faces at (i+1/2,j)h. Sampling supports one ghost width beyond a face.
[[nodiscard]] auto Sample(Grid grid, Field const &field, double worldX, double worldY)
    -> std::expected<double, ContractError>;

struct AdvectionResult final
{
    Field scalar{};
    double sourceMass{};
    double outputMass{};
    double sourceEnergy{};
    double outputEnergy{};
    std::uint32_t clampedCells{};
};

struct FieldError final
{
    double meanAbsolute{};
    double rootMeanSquare{};
    double maximumAbsolute{};
};

// Compare interior samples at matching physical locations; ghost cells do
// not contribute to analytic or GPU-vs-CPU field error.
[[nodiscard]] auto MeasureFieldError(Grid grid, Field const &reference, Field const &observed)
    -> std::expected<FieldError, ContractError>;

// Each component moves at most one cell per step, so departures fit the ghost
// ring. Correction uses a reverse pass and clamps to the departure stencil.
// This transports a scalar only; pressure projection is a separate chapter.
[[nodiscard]] auto AdvectScalar(Grid grid, Field const &scalar, Field const &velocityX, Field const &velocityY,
                                double timeStep, Boundary boundary, AdvectionMethod method)
    -> std::expected<AdvectionResult, ContractError>;

struct VelocityFields final
{
    Field x{};
    Field y{};
};

// Semi-Lagrangian transport evaluates each component at its own MAC face,
// using separate source and destination fields. Correction of the transported
// scalar is independent of this velocity step.
[[nodiscard]] auto AdvectVelocity(Grid grid, Field const &velocityX, Field const &velocityY, double timeStep,
                                  Boundary boundary) -> std::expected<VelocityFields, ContractError>;

// Acceleration has world-units/s^2; the timestep has seconds. Source fields
// remain unchanged so each pass has unambiguous input/output ownership.
[[nodiscard]] auto ApplyUniformForce(Grid grid, Field const &velocityX, Field const &velocityY, double accelerationX,
                                     double accelerationY, double timeStep, Boundary boundary)
    -> std::expected<VelocityFields, ContractError>;

} // namespace ch36::fluid
