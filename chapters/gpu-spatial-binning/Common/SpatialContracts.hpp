#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace ch35::spatial
{

// Chapter budgets, not hardware limits. The identity sentinel agrees with Chapter 34.
inline constexpr std::uint32_t kMaximumRecords = 256U;
inline constexpr std::uint32_t kMaximumCells = 4'096U;
inline constexpr std::uint32_t kMaximumNeighborOutputs = kMaximumRecords * kMaximumRecords;
inline constexpr std::uint32_t kInvalidParticleId = 0xFFFF'FFFFU;
inline constexpr std::uint32_t kInvalidCell = 0xFFFF'FFFFU;
inline constexpr double kMaximumWorldCoordinate = 1.0e6;
inline constexpr double kMinimumCellSize = 1.0e-6;
inline constexpr double kMaximumCellSize = 1.0e6;

enum class ContractError : std::uint8_t
{
    NonFinite,
    InvalidGrid,
    GridTooLarge,
    ArithmeticOverflow,
    CapacityExceeded,
    InvalidAliveFlag,
    InvalidIdentity,
    DuplicateIdentity,
    DuplicateSourceIndex,
    InvalidCell,
    InvalidRecordOrder,
    InvalidRanges,
    UnknownQueryIdentity,
    InvalidRadius,
};

struct Float3 final
{
    double x{};
    double y{};
    double z{};
    [[nodiscard]] bool operator==(Float3 const &) const noexcept = default;
};

struct Cell3 final
{
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
    [[nodiscard]] bool operator==(Cell3 const &) const noexcept = default;
};

struct Grid final
{
    Float3 origin{};
    double cellSize{};
    Cell3 dimensions{};
};

// Cell keys are x + nx * (y + ny * z); domain is [origin, origin + dimensions * cellSize).
// A finite position outside the domain has no cell, rather than aliasing a valid key.
[[nodiscard]] auto ValidateGrid(Grid const &grid) noexcept -> std::expected<std::uint32_t, ContractError>;
[[nodiscard]] auto FlattenCell(Grid const &grid, Cell3 cell) noexcept -> std::expected<std::uint32_t, ContractError>;
[[nodiscard]] auto CellForPosition(Grid const &grid, Float3 position) noexcept
    -> std::expected<std::optional<std::uint32_t>, ContractError>;

struct Particle final
{
    Float3 position{};
    std::uint32_t identity{kInvalidParticleId};
    std::uint32_t alive{};
};

struct Record final
{
    std::uint32_t key{kInvalidCell};
    std::uint32_t identity{kInvalidParticleId};
    std::uint32_t sourceIndex{};
    Float3 position{};
    [[nodiscard]] bool operator==(Record const &) const noexcept = default;
};

struct RecordBuild final
{
    std::vector<Record> records{};
    std::uint32_t liveCount{};
    std::uint32_t outsideCount{};
    std::uint32_t emittedCount{};
    std::uint32_t overflowCount{};
};

// Source-order bounded gather; all live identities are checked even after the output fills.
[[nodiscard]] auto BuildRecords(Grid const &grid, std::span<Particle const> particles, std::uint32_t capacity)
    -> std::expected<RecordBuild, ContractError>;
// Stable sort on (cell key, stable identity), carrying source index and position unchanged.
[[nodiscard]] auto SortRecords(Grid const &grid, std::span<Record const> records)
    -> std::expected<std::vector<Record>, ContractError>;

struct CellRange final
{
    std::uint32_t start{};
    std::uint32_t end{};
    [[nodiscard]] bool operator==(CellRange const &) const noexcept = default;
};

// Empty cells use [N,N), where N is the sorted record count, including gaps and trailing cells.
[[nodiscard]] auto BuildCellRanges(Grid const &grid, std::span<Record const> sortedRecords)
    -> std::expected<std::vector<CellRange>, ContractError>;

struct QueryRange final
{
    std::uint32_t identity{};
    std::uint32_t start{};
    std::uint32_t end{};
    std::uint32_t candidateCount{}; // visited stencil records, including the query itself
    std::uint32_t acceptedCount{};  // in-radius records with a different identity
    std::uint32_t overflowCount{};  // accepted, but not written to the global output
};

struct NeighborResult final
{
    std::vector<QueryRange> queries{};
    std::vector<std::uint32_t> identities{};
    std::uint32_t candidateCount{}; // includes queries that did not fit queryCapacity
    std::uint32_t acceptedCount{};
    std::uint32_t queryOverflowCount{};
    std::uint32_t outputOverflowCount{}; // only for queries that fit queryCapacity
};

// Radius must be in [0, cellSize]: the clamped 3x3x3 stencil is then complete.
// Queries beyond queryCapacity are still evaluated for totals, but have no output range;
// accepted neighbors of those queries do not also count as output overflow.
[[nodiscard]] auto EnumerateNeighbors(Grid const &grid, std::span<Record const> sortedRecords,
                                      std::span<CellRange const> ranges, std::span<std::uint32_t const> queryIds,
                                      double radius, std::uint32_t queryCapacity, std::uint32_t outputCapacity)
    -> std::expected<NeighborResult, ContractError>;

// Independent O(N^2) oracle: scans all records rather than using keys or cell ranges.
// Returns identities in sorted-record order; self is excluded by identity.
[[nodiscard]] auto NaiveNeighbors(Grid const &grid, std::span<Record const> sortedRecords, std::uint32_t queryId,
                                  double radius) -> std::expected<std::vector<std::uint32_t>, ContractError>;

} // namespace ch35::spatial
