#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SpatialContracts.hpp"

#include <lgp/framework/application.hpp>

#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

namespace ch35::spatial::gpu
{

struct GpuParticle final
{
    float x{};
    float y{};
    float z{};
    std::uint32_t identity{kInvalidParticleId};
    std::uint32_t alive{};
};
static_assert(sizeof(GpuParticle) == 20U);

struct GpuRecord final
{
    std::uint32_t key{kInvalidCell};
    std::uint32_t identity{kInvalidParticleId};
    std::uint32_t sourceIndex{};
    float x{};
    float y{};
    float z{};
    [[nodiscard]] bool operator==(GpuRecord const &) const noexcept = default;
};
static_assert(sizeof(GpuRecord) == 24U);

struct GpuRange final
{
    std::uint32_t start{};
    std::uint32_t end{};
    [[nodiscard]] bool operator==(GpuRange const &) const noexcept = default;
};
static_assert(sizeof(GpuRange) == 8U);

struct GpuQuery final
{
    std::uint32_t identity{};
    std::uint32_t start{};
    std::uint32_t end{};
    std::uint32_t candidateCount{};
    std::uint32_t acceptedCount{};
    std::uint32_t overflowCount{};
    [[nodiscard]] bool operator==(GpuQuery const &) const noexcept = default;
};
static_assert(sizeof(GpuQuery) == 24U);

struct GpuStats final
{
    std::uint32_t liveCount{};
    std::uint32_t outsideCount{};
    std::uint32_t emittedCount{};
    std::uint32_t recordOverflowCount{};
    std::uint32_t candidateCount{};
    std::uint32_t acceptedCount{};
    std::uint32_t queryOverflowCount{};
    std::uint32_t outputOverflowCount{};
    std::uint32_t outputCount{};
    [[nodiscard]] bool operator==(GpuStats const &) const noexcept = default;
};
static_assert(sizeof(GpuStats) == 36U);

struct LabConfiguration final
{
    Grid grid{{-2.0, -2.0, -2.0}, 1.0, {4U, 4U, 4U}};
    std::vector<Particle> particles{{{-1.0, -1.0, -1.0}, 7U, 1U},
                                    {{0.0, -1.0, -1.0}, 2U, 1U},
                                    {{-1.0, 0.0, -1.0}, 8U, 1U},
                                    {{1.0, 1.0, 1.0}, 4U, 1U}};
    std::vector<std::uint32_t> queryIds{7U, 2U};
    double radius{1.0};
    std::uint32_t recordCapacity{kMaximumRecords};
    std::uint32_t queryCapacity{kMaximumRecords};
    std::uint32_t outputCapacity{kMaximumNeighborOutputs};
    float viewScale{0.25F};
    float pointHalfExtent{0.035F};
};

struct FrameReadback final
{
    std::vector<GpuRecord> records{};
    std::vector<GpuRange> ranges{};
    std::vector<GpuQuery> queries{};
    std::vector<std::uint32_t> neighbors{};
    GpuStats stats{};
    bool outputGuardIntact{false};
    bool paddingIntact{false};
    bool gpuComputed{false};
};

[[nodiscard]] auto BuildReference(LabConfiguration const &configuration)
    -> std::expected<FrameReadback, lgp::framework::Error>;
[[nodiscard]] auto ValidateConfiguration(LabConfiguration const &configuration) -> lgp::framework::Status;

class BufferResource final
{
  public:
    BufferResource() = default;
    BufferResource(BufferResource &&other) noexcept;
    auto operator=(BufferResource &&other) noexcept -> BufferResource &;
    BufferResource(BufferResource const &) = delete;
    auto operator=(BufferResource const &) -> BufferResource & = delete;
    ~BufferResource();

    [[nodiscard]] auto Get() const noexcept -> ID3D12Resource *
    {
        return resource_.Get();
    }
    [[nodiscard]] auto mapped_data() const noexcept -> std::byte *
    {
        return mapped_;
    }

  private:
    friend auto CreateBuffer(ID3D12Device10 &, std::uint64_t, D3D12_HEAP_TYPE, D3D12_RESOURCE_FLAGS, bool)
        -> std::expected<BufferResource, lgp::framework::Error>;
    void Reset() noexcept;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::byte *mapped_{};
};

[[nodiscard]] auto CreateBuffer(ID3D12Device10 &device, std::uint64_t bytes, D3D12_HEAP_TYPE heap,
                                D3D12_RESOURCE_FLAGS flags, bool mapped)
    -> std::expected<BufferResource, lgp::framework::Error>;

void BufferBarrier(ID3D12GraphicsCommandList7 &list, ID3D12Resource &resource, D3D12_BARRIER_SYNC beforeSync,
                   D3D12_BARRIER_ACCESS beforeAccess, D3D12_BARRIER_SYNC afterSync,
                   D3D12_BARRIER_ACCESS afterAccess) noexcept;

} // namespace ch35::spatial::gpu
