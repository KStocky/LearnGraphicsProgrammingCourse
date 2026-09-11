#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ParticleContracts.hpp"

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

namespace ch34::particles::gpu
{

// The single-thread-group budget. Emission and compaction scan their whole particle array in one group, so the
// lab caps capacity here and the host refuses anything larger rather than silently scanning a fraction of it.
inline constexpr std::uint32_t kThreadGroupSize = 256U;
inline constexpr std::uint32_t kMaximumLabCapacity = kThreadGroupSize;

// One quad per particle: two triangles, six vertices.
inline constexpr std::uint32_t kVertexCountPerParticle = 6U;

// The GPU particle ABI. Ten 32-bit scalars, matching the HLSL `GpuParticle` field for field, so a structured
// view has stride 40 and a readback can be reinterpreted directly.
struct GpuParticle final
{
    std::uint32_t identity{kInvalidParticleId};
    std::uint32_t alive{};
    float px{};
    float py{};
    float pz{};
    float vx{};
    float vy{};
    float vz{};
    float age{};
    float lifetime{};

    [[nodiscard]] bool operator==(GpuParticle const &) const noexcept = default;
};

static_assert(sizeof(GpuParticle) == 40U);

// A headless scenario. One Render advances `frameCount` frames from a fresh reset, so a test names a whole run and
// reads back the final frame's evidence plus the counters accumulated across every substep of every frame.
struct LabConfiguration final
{
    std::uint32_t capacity{64U};
    std::uint64_t seed{1U};
    FrameSettings frame{};
    double frameDeltaSeconds{1.0 / 60.0};
    std::uint32_t frameCount{1U};
    // Zero means "use capacity".
    std::uint32_t drawCapacity{0U};
    std::uint32_t outputCapacity{0U};
    float viewScale{0.5F};
    float quadHalfExtent{0.04F};
};

// A valid, visibly non-empty scenario used when no headless configuration is supplied (the windowed and smoke
// runs). Both variants share it so their default pictures match.
[[nodiscard]] LabConfiguration DefaultLabConfiguration() noexcept;

[[nodiscard]] std::uint32_t ResolveDrawCapacity(LabConfiguration const &configuration) noexcept;
[[nodiscard]] std::uint32_t ResolveOutputCapacity(LabConfiguration const &configuration) noexcept;

// The evidence a frame publishes. Both the Starter (from its uploaded CPU reference) and the Solution (from GPU
// readback) fill the same structure, so one harness compares either against an independently derived reference.
struct FrameReadback final
{
    FixedStepPlan lastPlan{};
    std::uint32_t requested{};
    std::uint32_t spawned{};
    std::uint32_t dropped{};
    std::uint32_t died{};
    std::uint32_t contact{};
    std::uint32_t liveCount{};
    std::uint32_t emittedCount{};
    std::vector<std::uint32_t> compactedSlots{};
    std::vector<std::uint32_t> compactedIdentities{};
    DrawInstancedArguments indirectArguments{};
    std::uint32_t executedCommandCount{};
    std::uint64_t checksum{};
    std::uint64_t readbackChecksum{};
    FrameSlots slotsUsed{};
    BufferSlot resultSlot{BufferSlot::A};
    StagePartition stagePartition{};
    bool guardsIntact{true};
    bool barriersModelled{true};
    std::vector<GpuParticle> state{};

    [[nodiscard]] bool operator==(FrameReadback const &) const noexcept = default;
};

// The independently derived expectation, produced by stepping the pure CPU contract `frameCount` frames. A test
// compares a GPU `FrameReadback` against this; the two share only the contract, never a GPU value.
[[nodiscard]] std::expected<FrameReadback, lgp::framework::Error> BuildReferenceReadback(
    LabConfiguration const &configuration);

// The stage partition both variants publish for a run. `emissionRan` is true when any substep ran across the
// frames, `drawRan` when the indirect draw executed a command, `readbackRan` when the frame copied its evidence
// out. Centralised so the reference and the renderers can never disagree about which stages a run recorded.
[[nodiscard]] StagePartition BuildStagePartition(bool emissionRan, bool drawRan, bool readbackRan) noexcept;

// The float32 FNV-1a image of a GPU state array, computed exactly as `ChecksumParticleState`. Used to recompute a
// checksum from a readback and to build the reference for a byte-exact comparison.
[[nodiscard]] std::uint64_t ChecksumGpuState(std::span<GpuParticle const> state) noexcept;

// The per-substep spawn requests and the frame plan for one frame, given the carrying scheduler state. These are
// emission-scheduler inputs the host owns; the GPU consumes the counts and owns the placement.
struct FramePlanResult final
{
    FixedStepPlan plan{};
    std::vector<std::uint32_t> requestedPerSubstep{};
    double nextAccumulatorSeconds{};
    double nextEmissionCarry{};
};

[[nodiscard]] std::expected<FramePlanResult, ContractError> PlanFrameEmission(FrameSettings const &settings,
                                                                              double accumulatorSeconds,
                                                                              double emissionCarry,
                                                                              double frameDeltaSeconds);

// ---------------------------------------------------------------------------------------------------------------
// D3D12 buffer and enhanced-barrier helpers (shared by both variants).
// ---------------------------------------------------------------------------------------------------------------

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
[[nodiscard]] BufferBarrierState VertexShaderResourceState() noexcept;
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

[[nodiscard]] lgp::framework::Status CompileShader(lgp::framework::ShaderCompiler &compiler,
                                                   lgp::framework::ShaderCompileOptions &options,
                                                   wchar_t const *entryPoint, wchar_t const *targetProfile,
                                                   lgp::framework::CompiledShader &shader);

} // namespace ch34::particles::gpu
