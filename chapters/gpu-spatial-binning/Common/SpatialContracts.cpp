#include "SpatialContracts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace ch35::spatial
{
namespace
{
[[nodiscard]] auto Components(Float3 value) noexcept -> std::array<double, 3>
{
    return {value.x, value.y, value.z};
}

[[nodiscard]] auto Components(Cell3 value) noexcept -> std::array<std::uint32_t, 3>
{
    return {value.x, value.y, value.z};
}

[[nodiscard]] auto ValidateRadius(Grid const &grid, double radius) noexcept -> std::expected<void, ContractError>
{
    if (!std::isfinite(radius))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (radius < 0.0 || radius > grid.cellSize)
    {
        return std::unexpected(ContractError::InvalidRadius);
    }
    return {};
}

[[nodiscard]] auto InRadius(Float3 a, Float3 b, double radius) noexcept -> bool
{
    return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z) <= radius;
}

[[nodiscard]] auto ValidateRecords(Grid const &grid, std::span<Record const> records, bool requireSorted)
    -> std::expected<void, ContractError>
{
    auto const cells = ValidateGrid(grid);
    if (!cells)
    {
        return std::unexpected(cells.error());
    }
    if (records.size() > kMaximumRecords)
    {
        return std::unexpected(ContractError::CapacityExceeded);
    }
    std::unordered_set<std::uint32_t> identities{};
    std::unordered_set<std::uint32_t> sources{};
    for (std::size_t index = 0; index < records.size(); ++index)
    {
        auto const &record = records[index];
        if (record.identity == kInvalidParticleId || record.sourceIndex >= kMaximumRecords)
        {
            return std::unexpected(ContractError::InvalidIdentity);
        }
        if (!identities.insert(record.identity).second)
        {
            return std::unexpected(ContractError::DuplicateIdentity);
        }
        if (!sources.insert(record.sourceIndex).second)
        {
            return std::unexpected(ContractError::DuplicateSourceIndex);
        }
        auto const key = CellForPosition(grid, record.position);
        if (!key)
        {
            return std::unexpected(key.error());
        }
        if (!key->has_value() || **key != record.key || record.key >= *cells)
        {
            return std::unexpected(ContractError::InvalidCell);
        }
        if (requireSorted && index != 0U)
        {
            auto const &previous = records[index - 1U];
            if (previous.key > record.key || (previous.key == record.key && previous.identity > record.identity))
            {
                return std::unexpected(ContractError::InvalidRecordOrder);
            }
        }
    }
    return {};
}

[[nodiscard]] auto FindQuery(std::span<Record const> records, std::uint32_t identity) noexcept
    -> std::expected<Record const *, ContractError>
{
    for (auto const &record : records)
    {
        if (record.identity == identity)
        {
            return &record;
        }
    }
    return std::unexpected(ContractError::UnknownQueryIdentity);
}

[[nodiscard]] auto ValidateRanges(std::span<Record const> records, std::span<CellRange const> ranges) noexcept
    -> std::expected<void, ContractError>
{
    if (ranges.empty())
    {
        return std::unexpected(ContractError::InvalidRanges);
    }
    std::uint32_t offset = 0U;
    auto const count = static_cast<std::uint32_t>(records.size());
    for (std::uint32_t key = 0U; key < ranges.size(); ++key)
    {
        auto const &range = ranges[key];
        if (range.start > range.end || range.end > count)
        {
            return std::unexpected(ContractError::InvalidRanges);
        }
        if (offset < count && records[offset].key == key)
        {
            if (range.start != offset || range.end == offset)
            {
                return std::unexpected(ContractError::InvalidRanges);
            }
            while (offset < range.end)
            {
                if (records[offset].key != key)
                {
                    return std::unexpected(ContractError::InvalidRanges);
                }
                ++offset;
            }
        }
        else if (range.start != count || range.end != count)
        {
            return std::unexpected(ContractError::InvalidRanges);
        }
    }
    if (offset != count)
    {
        return std::unexpected(ContractError::InvalidRanges);
    }
    return {};
}
} // namespace

auto ValidateGrid(Grid const &grid) noexcept -> std::expected<std::uint32_t, ContractError>
{
    auto const origins = Components(grid.origin);
    auto const dimensions = Components(grid.dimensions);
    if (!std::isfinite(grid.cellSize) ||
        !std::all_of(origins.begin(), origins.end(), [](double value) { return std::isfinite(value); }))
    {
        return std::unexpected(ContractError::NonFinite);
    }
    if (grid.cellSize < kMinimumCellSize || grid.cellSize > kMaximumCellSize)
    {
        return std::unexpected(ContractError::InvalidGrid);
    }
    std::uint64_t count = 1U;
    for (std::size_t axis = 0U; axis < 3U; ++axis)
    {
        if (dimensions[axis] == 0U || std::abs(origins[axis]) > kMaximumWorldCoordinate ||
            !std::isfinite(origins[axis] + grid.cellSize * dimensions[axis]) ||
            std::abs(origins[axis] + grid.cellSize * dimensions[axis]) > kMaximumWorldCoordinate)
        {
            return std::unexpected(ContractError::InvalidGrid);
        }
        if (count > std::numeric_limits<std::uint32_t>::max() / dimensions[axis])
        {
            return std::unexpected(ContractError::ArithmeticOverflow);
        }
        count *= dimensions[axis];
    }
    if (count > kMaximumCells)
    {
        return std::unexpected(ContractError::GridTooLarge);
    }
    return static_cast<std::uint32_t>(count);
}

auto FlattenCell(Grid const &grid, Cell3 cell) noexcept -> std::expected<std::uint32_t, ContractError>
{
    auto const count = ValidateGrid(grid);
    if (!count)
    {
        return std::unexpected(count.error());
    }
    if (cell.x >= grid.dimensions.x || cell.y >= grid.dimensions.y || cell.z >= grid.dimensions.z)
    {
        return std::unexpected(ContractError::InvalidCell);
    }
    auto const key = static_cast<std::uint64_t>(cell.x) +
                     static_cast<std::uint64_t>(grid.dimensions.x) *
                         (static_cast<std::uint64_t>(cell.y) + static_cast<std::uint64_t>(grid.dimensions.y) * cell.z);
    return static_cast<std::uint32_t>(key);
}

auto CellForPosition(Grid const &grid, Float3 position) noexcept
    -> std::expected<std::optional<std::uint32_t>, ContractError>
{
    auto const count = ValidateGrid(grid);
    if (!count)
    {
        return std::unexpected(count.error());
    }
    auto const positions = Components(position);
    auto const origins = Components(grid.origin);
    auto const dimensions = Components(grid.dimensions);
    Cell3 cell{};
    std::array<std::uint32_t *, 3> const axes{&cell.x, &cell.y, &cell.z};
    for (std::size_t axis = 0U; axis < 3U; ++axis)
    {
        if (!std::isfinite(positions[axis]))
        {
            return std::unexpected(ContractError::NonFinite);
        }
        auto const upper = origins[axis] + grid.cellSize * dimensions[axis];
        if (positions[axis] < origins[axis] || positions[axis] >= upper)
        {
            return std::optional<std::uint32_t>{};
        }
        // Floor, never truncation towards zero. Clamp only the last ULP of division when
        // the half-open domain check above already proved this coordinate lies inside.
        auto const quotient = std::floor((positions[axis] - origins[axis]) / grid.cellSize);
        *axes[axis] = static_cast<std::uint32_t>(std::min(quotient, static_cast<double>(dimensions[axis] - 1U)));
    }
    auto const key = FlattenCell(grid, cell);
    return std::optional<std::uint32_t>{*key};
}

auto BuildRecords(Grid const &grid, std::span<Particle const> particles, std::uint32_t capacity)
    -> std::expected<RecordBuild, ContractError>
{
    auto const cells = ValidateGrid(grid);
    if (!cells)
    {
        return std::unexpected(cells.error());
    }
    if (particles.size() > kMaximumRecords || capacity > kMaximumRecords)
    {
        return std::unexpected(ContractError::CapacityExceeded);
    }
    RecordBuild result{};
    std::unordered_set<std::uint32_t> identities{};
    for (std::size_t index = 0U; index < particles.size(); ++index)
    {
        auto const &particle = particles[index];
        if (particle.alive > 1U)
        {
            return std::unexpected(ContractError::InvalidAliveFlag);
        }
        if ((particle.alive == 0U) != (particle.identity == kInvalidParticleId))
        {
            return std::unexpected(ContractError::InvalidIdentity);
        }
        if (particle.alive == 0U)
        {
            continue;
        }
        if (!identities.insert(particle.identity).second)
        {
            return std::unexpected(ContractError::DuplicateIdentity);
        }
        ++result.liveCount;
        auto const key = CellForPosition(grid, particle.position);
        if (!key)
        {
            return std::unexpected(key.error());
        }
        if (!key->has_value())
        {
            ++result.outsideCount;
        }
        else if (result.records.size() == capacity)
        {
            ++result.overflowCount;
        }
        else
        {
            result.records.push_back({.key = **key,
                                      .identity = particle.identity,
                                      .sourceIndex = static_cast<std::uint32_t>(index),
                                      .position = particle.position});
        }
    }
    result.emittedCount = static_cast<std::uint32_t>(result.records.size());
    return result;
}

auto SortRecords(Grid const &grid, std::span<Record const> records) -> std::expected<std::vector<Record>, ContractError>
{
    if (auto const valid = ValidateRecords(grid, records, false); !valid)
    {
        return std::unexpected(valid.error());
    }
    std::vector<Record> sorted(records.begin(), records.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](Record const &a, Record const &b)
                     { return a.key < b.key || (a.key == b.key && a.identity < b.identity); });
    return sorted;
}

auto BuildCellRanges(Grid const &grid, std::span<Record const> sortedRecords)
    -> std::expected<std::vector<CellRange>, ContractError>
{
    if (auto const valid = ValidateRecords(grid, sortedRecords, true); !valid)
    {
        return std::unexpected(valid.error());
    }
    auto const cells = *ValidateGrid(grid);
    auto const sentinel = static_cast<std::uint32_t>(sortedRecords.size());
    std::vector<CellRange> ranges(cells, {.start = sentinel, .end = sentinel});
    for (std::uint32_t index = 0U; index < sentinel; ++index)
    {
        auto &range = ranges[sortedRecords[index].key];
        if (range.start == sentinel)
        {
            range.start = index;
        }
        range.end = index + 1U;
    }
    return ranges;
}

auto EnumerateNeighbors(Grid const &grid, std::span<Record const> sortedRecords, std::span<CellRange const> ranges,
                        std::span<std::uint32_t const> queryIds, double radius, std::uint32_t queryCapacity,
                        std::uint32_t outputCapacity) -> std::expected<NeighborResult, ContractError>
{
    if (auto const valid = ValidateRecords(grid, sortedRecords, true); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (auto const valid = ValidateRadius(grid, radius); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (queryIds.size() > kMaximumRecords || queryCapacity > kMaximumRecords ||
        outputCapacity > kMaximumNeighborOutputs)
    {
        return std::unexpected(ContractError::CapacityExceeded);
    }
    if (ranges.size() != *ValidateGrid(grid) || !ValidateRanges(sortedRecords, ranges))
    {
        return std::unexpected(ContractError::InvalidRanges);
    }
    NeighborResult result{};
    for (auto const identity : queryIds)
    {
        auto const query = FindQuery(sortedRecords, identity);
        if (!query)
        {
            return std::unexpected(query.error());
        }
        bool const stored = result.queries.size() < queryCapacity;
        QueryRange row{.identity = identity};
        if (stored)
        {
            row.start = static_cast<std::uint32_t>(result.identities.size());
        }
        else
        {
            ++result.queryOverflowCount;
        }
        auto const key = (*query)->key;
        Cell3 const cell{key % grid.dimensions.x, (key / grid.dimensions.x) % grid.dimensions.y,
                         key / (grid.dimensions.x * grid.dimensions.y)};
        for (int dz = -1; dz <= 1; ++dz)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    auto const x = static_cast<int>(cell.x) + dx;
                    auto const y = static_cast<int>(cell.y) + dy;
                    auto const z = static_cast<int>(cell.z) + dz;
                    if (x < 0 || y < 0 || z < 0 || x >= static_cast<int>(grid.dimensions.x) ||
                        y >= static_cast<int>(grid.dimensions.y) || z >= static_cast<int>(grid.dimensions.z))
                    {
                        continue;
                    }
                    auto const neighborKey = static_cast<std::uint32_t>(
                        x + static_cast<int>(grid.dimensions.x) * (y + static_cast<int>(grid.dimensions.y) * z));
                    auto const &range = ranges[neighborKey];
                    for (auto index = range.start; index < range.end; ++index)
                    {
                        auto const &other = sortedRecords[index];
                        ++row.candidateCount;
                        ++result.candidateCount;
                        if (other.identity != identity && InRadius((*query)->position, other.position, radius))
                        {
                            ++row.acceptedCount;
                            ++result.acceptedCount;
                            if (stored)
                            {
                                if (result.identities.size() < outputCapacity)
                                {
                                    result.identities.push_back(other.identity);
                                }
                                else
                                {
                                    ++row.overflowCount;
                                    ++result.outputOverflowCount;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (stored)
        {
            row.end = static_cast<std::uint32_t>(result.identities.size());
            result.queries.push_back(row);
        }
    }
    return result;
}

auto NaiveNeighbors(Grid const &grid, std::span<Record const> sortedRecords, std::uint32_t queryId, double radius)
    -> std::expected<std::vector<std::uint32_t>, ContractError>
{
    if (auto const valid = ValidateRecords(grid, sortedRecords, true); !valid)
    {
        return std::unexpected(valid.error());
    }
    if (auto const valid = ValidateRadius(grid, radius); !valid)
    {
        return std::unexpected(valid.error());
    }
    auto const query = FindQuery(sortedRecords, queryId);
    if (!query)
    {
        return std::unexpected(query.error());
    }
    std::vector<std::uint32_t> neighbors{};
    for (auto const &record : sortedRecords)
    {
        auto const dx = record.position.x - (*query)->position.x;
        auto const dy = record.position.y - (*query)->position.y;
        auto const dz = record.position.z - (*query)->position.z;
        if (record.identity != queryId && dx * dx + dy * dy + dz * dz <= radius * radius)
        {
            neighbors.push_back(record.identity);
        }
    }
    return neighbors;
}
} // namespace ch35::spatial
