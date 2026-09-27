#include "GpuLabSupport.hpp"

#include <lgp/framework/error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>

namespace ch35::spatial::gpu
{
namespace
{
using lgp::framework::MakeError;
using lgp::framework::MakeHResultError;

[[nodiscard]] auto ContractFailure(char const *operation, ContractError error) -> lgp::framework::Error
{
    return MakeError(operation, "Chapter 35 spatial contract error " + std::to_string(static_cast<unsigned>(error)));
}

[[nodiscard]] auto ExactFloat(double value) noexcept -> bool
{
    return std::isfinite(value) && static_cast<double>(static_cast<float>(value)) == value;
}

} // namespace

auto ValidateConfiguration(LabConfiguration const &configuration) -> lgp::framework::Status
{
    auto const count = ValidateGrid(configuration.grid);
    if (!count)
    {
        return std::unexpected(ContractFailure("Update", count.error()));
    }
    if (configuration.particles.size() > kMaximumRecords || configuration.queryIds.size() > kMaximumRecords ||
        configuration.recordCapacity > kMaximumRecords || configuration.queryCapacity > kMaximumRecords ||
        configuration.outputCapacity > kMaximumNeighborOutputs)
    {
        return std::unexpected(MakeError("Update", "Chapter 35 capacity exceeds the GPU single-group budget."));
    }
    auto const &grid = configuration.grid;
    if (!ExactFloat(grid.origin.x) || !ExactFloat(grid.origin.y) || !ExactFloat(grid.origin.z) ||
        !ExactFloat(grid.cellSize) || !ExactFloat(configuration.radius) || configuration.radius < 0.0 ||
        configuration.radius > grid.cellSize)
    {
        return std::unexpected(MakeError("Update", "Chapter 35 requires finite, float32-exact grid and radius."));
    }
    std::array<double, 3U> const origins{grid.origin.x, grid.origin.y, grid.origin.z};
    std::array<std::uint32_t, 3U> const dimensions{grid.dimensions.x, grid.dimensions.y, grid.dimensions.z};
    for (std::size_t axis = 0U; axis < origins.size(); ++axis)
    {
        for (std::uint32_t boundary = 1U; boundary <= dimensions[axis]; ++boundary)
        {
            if (!ExactFloat(origins[axis] + grid.cellSize * boundary))
            {
                return std::unexpected(MakeError("Update", "Chapter 35 grid boundaries must be float32-exact."));
            }
        }
    }
    if (!std::isfinite(configuration.viewScale) || configuration.viewScale <= 0.0F ||
        !std::isfinite(configuration.pointHalfExtent) || configuration.pointHalfExtent <= 0.0F)
    {
        return std::unexpected(MakeError("Update", "Chapter 35 draw scale or point size is invalid."));
    }
    std::unordered_set<std::uint32_t> seen{};
    std::unordered_set<std::uint32_t> retained{};
    for (auto const &particle : configuration.particles)
    {
        if (particle.alive > 1U)
        {
            return std::unexpected(ContractFailure("Update", ContractError::InvalidAliveFlag));
        }
        if ((particle.alive == 0U) != (particle.identity == kInvalidParticleId))
        {
            return std::unexpected(ContractFailure("Update", ContractError::InvalidIdentity));
        }
        if (particle.alive == 0U)
        {
            continue;
        }
        if (!seen.insert(particle.identity).second)
        {
            return std::unexpected(ContractFailure("Update", ContractError::DuplicateIdentity));
        }
        if (!ExactFloat(particle.position.x) || !ExactFloat(particle.position.y) || !ExactFloat(particle.position.z))
        {
            return std::unexpected(MakeError("Update", "Chapter 35 live positions must be float32-exact."));
        }
        auto const key = CellForPosition(grid, particle.position);
        if (!key)
        {
            return std::unexpected(ContractFailure("Update", key.error()));
        }
        if (key->has_value() && retained.size() < configuration.recordCapacity)
        {
            retained.insert(particle.identity);
        }
    }
    for (auto const identity : configuration.queryIds)
    {
        if (!retained.contains(identity))
        {
            return std::unexpected(ContractFailure("Update", ContractError::UnknownQueryIdentity));
        }
    }
    return {};
}

auto BuildReference(LabConfiguration const &configuration) -> std::expected<FrameReadback, lgp::framework::Error>
{
    if (auto const status = ValidateConfiguration(configuration); !status)
    {
        return std::unexpected(status.error());
    }
    auto const built = BuildRecords(configuration.grid, configuration.particles, configuration.recordCapacity);
    if (!built)
    {
        return std::unexpected(ContractFailure("BuildReference", built.error()));
    }
    auto const sorted = SortRecords(configuration.grid, built->records);
    if (!sorted)
    {
        return std::unexpected(ContractFailure("BuildReference", sorted.error()));
    }
    auto const ranges = BuildCellRanges(configuration.grid, *sorted);
    if (!ranges)
    {
        return std::unexpected(ContractFailure("BuildReference", ranges.error()));
    }
    auto const query =
        EnumerateNeighbors(configuration.grid, *sorted, *ranges, configuration.queryIds, configuration.radius,
                           configuration.queryCapacity, configuration.outputCapacity);
    if (!query)
    {
        return std::unexpected(ContractFailure("BuildReference", query.error()));
    }
    FrameReadback result{};
    for (auto const &record : *sorted)
    {
        result.records.push_back({record.key, record.identity, record.sourceIndex,
                                  static_cast<float>(record.position.x), static_cast<float>(record.position.y),
                                  static_cast<float>(record.position.z)});
    }
    for (auto const &range : *ranges)
    {
        result.ranges.push_back({range.start, range.end});
    }
    for (auto const &row : query->queries)
    {
        result.queries.push_back(
            {row.identity, row.start, row.end, row.candidateCount, row.acceptedCount, row.overflowCount});
    }
    result.neighbors = query->identities;
    result.stats = {
        built->liveCount,          built->outsideCount,        built->emittedCount,
        built->overflowCount,      query->candidateCount,      query->acceptedCount,
        query->queryOverflowCount, query->outputOverflowCount, static_cast<std::uint32_t>(result.neighbors.size())};
    result.paddingIntact = true;
    return result;
}

BufferResource::BufferResource(BufferResource &&other) noexcept
{
    *this = std::move(other);
}

auto BufferResource::operator=(BufferResource &&other) noexcept -> BufferResource &
{
    if (this != &other)
    {
        Reset();
        resource_ = std::move(other.resource_);
        mapped_ = std::exchange(other.mapped_, nullptr);
    }
    return *this;
}

BufferResource::~BufferResource()
{
    Reset();
}

void BufferResource::Reset() noexcept
{
    if (resource_ != nullptr && mapped_ != nullptr)
    {
        D3D12_RANGE const written{0U, 0U};
        resource_->Unmap(0U, &written);
    }
    mapped_ = nullptr;
    resource_.Reset();
}

auto CreateBuffer(ID3D12Device10 &device, std::uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                  bool mapped) -> std::expected<BufferResource, lgp::framework::Error>
{
    if (bytes == 0U)
    {
        return std::unexpected(MakeError("CreateBuffer", "Zero-sized GPU buffer."));
    }
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    properties.CreationNodeMask = 1U;
    properties.VisibleNodeMask = 1U;
    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    BufferResource result{};
    auto const created = device.CreateCommittedResource3(&properties, D3D12_HEAP_FLAG_NONE, &description,
                                                         D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0U, nullptr,
                                                         IID_PPV_ARGS(result.resource_.ReleaseAndGetAddressOf()));
    if (FAILED(created))
    {
        return std::unexpected(MakeHResultError("ID3D12Device10::CreateCommittedResource3", created));
    }
    if (mapped)
    {
        D3D12_RANGE const read{0U, 0U};
        void *address = nullptr;
        auto const mappedResult = result.resource_->Map(0U, &read, &address);
        if (FAILED(mappedResult))
        {
            return std::unexpected(MakeHResultError("ID3D12Resource::Map", mappedResult));
        }
        result.mapped_ = static_cast<std::byte *>(address);
    }
    return result;
}

void BufferBarrier(ID3D12GraphicsCommandList7 &list, ID3D12Resource &resource, D3D12_BARRIER_SYNC beforeSync,
                   D3D12_BARRIER_ACCESS beforeAccess, D3D12_BARRIER_SYNC afterSync,
                   D3D12_BARRIER_ACCESS afterAccess) noexcept
{
    D3D12_BUFFER_BARRIER barrier{};
    barrier.SyncBefore = beforeSync;
    barrier.AccessBefore = beforeAccess;
    barrier.SyncAfter = afterSync;
    barrier.AccessAfter = afterAccess;
    barrier.pResource = &resource;
    barrier.Size = UINT64_MAX;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_BUFFER;
    group.NumBarriers = 1U;
    group.pBufferBarriers = &barrier;
    list.Barrier(1U, &group);
}
} // namespace ch35::spatial::gpu
