#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "GpuDrivenContracts.hpp"

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
#include <span>
#include <string_view>
#include <vector>

namespace ch21::gpu_driven::gpu
{

inline constexpr std::uint32_t kMaximumInstanceCount = 16U;
inline constexpr std::uint32_t kDrawTemplateCount = 3U;
inline constexpr std::uint32_t kIndirectCommandStride = 32U;
inline constexpr std::uint32_t kGuardValue = 0xCDCDCDCDU;
inline constexpr std::uint32_t kViewportHeight = 720U;
inline constexpr float kProjectionScale = 1.0F;
inline constexpr float kNearPlane = 0.5F;

enum class ScenePreset : std::uint8_t
{
    Default = 0U,
    Permuted,
    Overflow,
    Empty,
};

struct LabConfiguration final
{
    ScenePreset scene{ScenePreset::Default};
    std::uint32_t capacity{kMaximumInstanceCount};
};

struct Float4 final
{
    float x{};
    float y{};
    float z{};
    float w{};
};

struct GpuInstance final
{
    Float4 bounds{};
    Float4 display{};
    std::uint32_t stableId{};
    std::uint32_t instanceDataIndex{};
    std::uint32_t firstDrawTemplate{};
    std::uint32_t lodCount{};
    std::uint32_t previousLod{};
    std::array<std::uint32_t, 3U> padding{};
};

static_assert(sizeof(GpuInstance) == 64U);

struct GpuDrawTemplate final
{
    std::uint32_t indexCount{};
    std::uint32_t startIndex{};
    std::int32_t baseVertex{};
    std::uint32_t materialIndex{};
};

static_assert(sizeof(GpuDrawTemplate) == 16U);

struct GpuIndirectCommand final
{
    std::uint32_t stableId{};
    std::uint32_t lod{};
    std::uint32_t instanceDataIndex{};
    std::uint32_t indexCountPerInstance{};
    std::uint32_t instanceCount{};
    std::uint32_t startIndexLocation{};
    std::int32_t baseVertexLocation{};
    std::uint32_t startInstanceLocation{};
};

static_assert(sizeof(GpuIndirectCommand) == kIndirectCommandStride);

struct CpuReference final
{
    LabConfiguration configuration{};
    std::vector<GpuInstance> gpuInstances{};
    std::vector<InstanceRecord> instances{};
    std::vector<DrawTemplate> drawTemplates{};
    std::vector<VisibilityDecision> decisions{};
    std::vector<IndirectCommand> commands{};
    std::uint32_t visibleCount{};
};

struct ReadbackEvidence final
{
    std::vector<IndirectCommand> commands{};
    std::uint32_t visibleCount{};
    std::uint32_t executedCount{};
    std::array<std::uint32_t, 8U> guard{};
};

struct BufferBarrierState final
{
    D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};
    D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};
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

    [[nodiscard]] ID3D12Resource *Get() const noexcept;
    [[nodiscard]] std::uint64_t size_in_bytes() const noexcept;
    [[nodiscard]] std::byte *mapped_data() noexcept;
    [[nodiscard]] std::byte const *mapped_data() const noexcept;

  private:
    friend std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
        ID3D12Device10 &device, std::uint64_t sizeInBytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
        std::wstring_view name, bool mapPersistently);

    void Reset() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::uint64_t sizeInBytes_{};
    std::byte *mappedData_{};
};

[[nodiscard]] constexpr std::uint32_t NormalizeCapacity(std::uint32_t capacity) noexcept
{
    return capacity < kMaximumInstanceCount ? capacity : kMaximumInstanceCount;
}

[[nodiscard]] std::expected<CpuReference, lgp::framework::Error> BuildCpuReference(LabConfiguration configuration);
[[nodiscard]] IndirectCommand ToContractCommand(GpuIndirectCommand const &command,
                                                std::span<InstanceRecord const> instances,
                                                std::span<DrawTemplate const> drawTemplates);

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] BufferBarrierState NoAccessState() noexcept;
[[nodiscard]] BufferBarrierState ComputeUnorderedAccessState() noexcept;
[[nodiscard]] BufferBarrierState ExecuteIndirectState() noexcept;
[[nodiscard]] BufferBarrierState CopySourceState() noexcept;

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

[[nodiscard]] lgp::framework::Status CompileShader(lgp::framework::ShaderCompiler &compiler,
                                                   lgp::framework::ShaderCompileOptions &options,
                                                   wchar_t const *entryPoint, wchar_t const *targetProfile,
                                                   lgp::framework::CompiledShader &shader);

} // namespace ch21::gpu_driven::gpu
