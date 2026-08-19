#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "WorkDistribution.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/device_resources.hpp>
#include <lgp/framework/error.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace ch13::work_distribution::gpu
{

inline constexpr std::uint32_t kCandidateGridWidth = 8U;
inline constexpr std::uint32_t kCandidateGridHeight = 8U;
inline constexpr std::uint32_t kCandidateCount = kCandidateGridWidth * kCandidateGridHeight;
inline constexpr std::uint32_t kVertexCountPerQuad = 6U;

struct Float2 final
{
    float x{};
    float y{};

    [[nodiscard]] bool operator==(Float2 const &) const noexcept = default;
};

struct Float4 final
{
    float x{};
    float y{};
    float z{};
    float w{};

    [[nodiscard]] bool operator==(Float4 const &) const noexcept = default;
};

struct CandidateData final
{
    Float2 center{};
    Float2 halfExtent{};
    Float4 color{};
    std::uint32_t visible{};
    std::array<std::uint32_t, 3U> padding{};

    [[nodiscard]] bool operator==(CandidateData const &) const noexcept = default;
};

static_assert(sizeof(CandidateData) == 48U);

enum class ScenePreset : std::uint8_t
{
    Default = 0U,
    Empty,
    Overflow,
};

enum class ExecutionMode : std::uint8_t
{
    Stable = 0U,
    AtomicAppend,
};

struct LabConfiguration final
{
    ScenePreset scene{ScenePreset::Default};
    std::uint32_t capacity{kCandidateCount};
    ExecutionMode mode{ExecutionMode::Stable};
};

[[nodiscard]] constexpr std::uint32_t NormalizeCapacity(std::uint32_t capacity) noexcept
{
    return std::min(capacity, kCandidateCount);
}

struct CpuReference final
{
    std::vector<CandidateData> candidates{};
    std::vector<std::uint32_t> visibilityFlags{};
    DistributionStatistics statistics{};
    std::vector<std::uint32_t> emittedCandidateIndices{};
    std::vector<IndirectDrawCommand> indirectCommands{};
    std::uint32_t gpuCount{};
    std::uint32_t executionCount{};
    ExecutionMode mode{ExecutionMode::Stable};
};

struct BufferBarrierState final
{
    D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};
    D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};

    [[nodiscard]] constexpr bool operator==(BufferBarrierState const &) const noexcept = default;
};

class BufferResource final
{
  public:
    BufferResource() = default;
    BufferResource(BufferResource &&other) noexcept;
    BufferResource &operator=(BufferResource &&other) noexcept;
    BufferResource(BufferResource const &) = delete;
    BufferResource &operator=(BufferResource const &) = delete;
    ~BufferResource();

    [[nodiscard]] ID3D12Resource *Get() const noexcept
    {
        return resource_.Get();
    }

    [[nodiscard]] std::uint64_t size_in_bytes() const noexcept
    {
        return sizeInBytes_;
    }

    [[nodiscard]] std::byte *mapped_data() noexcept
    {
        return mappedData_;
    }

    [[nodiscard]] std::byte const *mapped_data() const noexcept
    {
        return mappedData_;
    }

  private:
    friend std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
        ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
        std::wstring_view name, bool mapPersistently);

    void Reset() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::uint64_t sizeInBytes_{};
    std::byte *mappedData_{};
};

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;

[[nodiscard]] BufferBarrierState NoAccessState() noexcept;
[[nodiscard]] BufferBarrierState ComputeShaderResourceState() noexcept;
[[nodiscard]] BufferBarrierState ComputeUnorderedAccessState() noexcept;
[[nodiscard]] BufferBarrierState ExecuteIndirectState() noexcept;
[[nodiscard]] BufferBarrierState CopySourceState() noexcept;
[[nodiscard]] BufferBarrierState CopyDestState() noexcept;

[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(
    ID3D12Resource &resource, lgp::framework::TextureBarrierState before, lgp::framework::TextureBarrierState after,
    D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) noexcept;
void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers);

[[nodiscard]] D3D12_BUFFER_BARRIER MakeBufferBarrier(ID3D12Resource &resource, BufferBarrierState before,
                                                     BufferBarrierState after, std::uint64_t offset = 0U,
                                                     std::uint64_t size = UINT64_MAX) noexcept;
void SubmitBufferBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_BUFFER_BARRIER> &barriers);

[[nodiscard]] std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
    ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
    std::wstring_view name, bool mapPersistently = false);
[[nodiscard]] lgp::framework::Status WriteBuffer(BufferResource &buffer, std::span<std::byte const> bytes,
                                                 std::uint64_t destinationOffset = 0U);

template <typename T>
[[nodiscard]] inline lgp::framework::Status WriteBuffer(BufferResource &buffer, std::span<T const> values,
                                                        std::uint64_t destinationOffset = 0U)
{
    return WriteBuffer(buffer, std::as_bytes(values), destinationOffset);
}

[[nodiscard]] std::vector<CandidateData> BuildCandidates(ScenePreset scene);
[[nodiscard]] std::vector<std::uint32_t> ExtractVisibilityFlags(std::span<CandidateData const> candidates);
[[nodiscard]] std::expected<CpuReference, lgp::framework::Error> BuildCpuReference(LabConfiguration configuration);

[[nodiscard]] lgp::framework::Status CompileShader(lgp::framework::ShaderCompiler &compiler,
                                                   lgp::framework::ShaderCompileOptions &options,
                                                   wchar_t const *entryPoint, wchar_t const *targetProfile,
                                                   lgp::framework::CompiledShader &shader);

} // namespace ch13::work_distribution::gpu
