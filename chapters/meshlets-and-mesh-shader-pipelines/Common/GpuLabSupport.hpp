#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "MeshletContracts.hpp"

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

namespace ch22::meshlets::gpu
{

// The deterministic teaching scene is a small planar grid drawn straight in clip
// space. Each grid cell becomes exactly one meshlet under the limits below, so the
// meshlet partition is visually obvious: a distinct flat colour per rectangle.
inline constexpr std::uint32_t kGridColumns = 3U;
inline constexpr std::uint32_t kGridRows = 2U;
inline constexpr std::uint32_t kExpectedMeshletCount = kGridColumns * kGridRows;

// Four distinct corners and two triangles per cell force the greedy builder to
// close a meshlet at every cell boundary.
inline constexpr std::uint32_t kSceneMaxVertices = 4U;
inline constexpr std::uint32_t kSceneMaxPrimitives = 2U;

// Compile-time capacities shared with MeshletLab.hlsli. They bound the amplification
// payload and mesh-shader output arrays and must not be exceeded by the scene limits.
inline constexpr std::uint32_t kShaderMaxMeshlets = 64U;
inline constexpr std::uint32_t kShaderMaxMeshletVertices = 64U;
inline constexpr std::uint32_t kShaderMaxMeshletPrimitives = 64U;

static_assert(kExpectedMeshletCount <= kShaderMaxMeshlets);
static_assert(kSceneMaxVertices <= kShaderMaxMeshletVertices);
static_assert(kSceneMaxPrimitives <= kShaderMaxMeshletPrimitives);

// gStats byte-addressed layout: dispatched amplification survivors, emitted mesh
// vertices, emitted mesh primitives, and one padding dword.
inline constexpr std::uint32_t kStatsDwordCount = 4U;
inline constexpr std::uint32_t kStatsBytes = kStatsDwordCount * sizeof(std::uint32_t);
inline constexpr std::uint32_t kStatsDispatchedGroupsOffset = 0U;
inline constexpr std::uint32_t kStatsEmittedVerticesOffset = 4U;
inline constexpr std::uint32_t kStatsEmittedPrimitivesOffset = 8U;

// Which pipeline the caller wants. The default drives the amplification/mesh path so
// the plain headless smoke run exercises it on capable adapters.
enum class MeshPathRequest : std::uint8_t
{
    Mesh = 0U,
    Classic,
};

// The pipeline the renderer actually recorded. MeshUnsupported is a first-class,
// intentional outcome: the mesh path was requested but the adapter lacks tier 1, so
// the classic pipeline drew the frame and no mesh dispatch is claimed.
enum class ExecutedPath : std::uint8_t
{
    Classic = 0U,
    MeshShader,
    MeshUnsupported,
};

struct LabConfiguration final
{
    MeshPathRequest request{MeshPathRequest::Mesh};
};

struct MeshShaderCapabilities final
{
    bool supported{};
    bool shaderModel65Supported{};
    D3D12_MESH_SHADER_TIER tier{D3D12_MESH_SHADER_TIER_NOT_SUPPORTED};
};

// Per-vertex payload of the flattened, meshlet-ordered vertex stream consumed by the
// classic indexed pipeline. meshletId is carried flat so classic and mesh raster
// produce identical per-cell colours.
struct SceneVertex final
{
    float px{};
    float py{};
    float pz{};
    std::uint32_t meshletId{};
};

static_assert(sizeof(SceneVertex) == 16U);

// Global vertex positions (w == 1) consumed by the amplification/mesh path.
struct PositionVertex final
{
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

static_assert(sizeof(PositionVertex) == 16U);

// One meshlet's slices into the vertex-remap and packed-primitive tables.
struct MeshletDescriptor final
{
    std::uint32_t vertexOffset{};
    std::uint32_t vertexCount{};
    std::uint32_t primitiveOffset{};
    std::uint32_t primitiveCount{};
};

static_assert(sizeof(MeshletDescriptor) == 16U);

// CPU-resident, upload-ready view of the scene. The classic stream (vertices +
// indices) and the mesh-path tables (positions + descriptors + remap + primitives)
// describe the same triangles, so every pipeline rasterizes the same image.
struct GpuScene final
{
    std::vector<Float3> positions{};
    std::vector<GlobalIndex> sourceIndices{};
    MeshletBuild build{};
    MeshletLimits limits{};

    std::vector<SceneVertex> classicVertices{};
    std::vector<std::uint32_t> classicIndices{};

    std::vector<PositionVertex> meshPositions{};
    std::vector<MeshletDescriptor> meshletDescriptors{};
    std::vector<std::uint32_t> meshletVertices{};
    std::vector<std::uint32_t> meshletPrimitives{};

    std::uint32_t meshletCount{};
    std::uint32_t emittedVertexReferences{};
    std::uint32_t emittedPrimitives{};
};

// Evidence structure exposed to tests and future prose.
struct MeshEvidence final
{
    ExecutedPath executedPath{ExecutedPath::Classic};
    MeshShaderCapabilities capabilities{};
    std::uint32_t meshletCount{};
    std::uint32_t dispatchedMeshletGroups{};
    std::uint32_t emittedVertices{};
    std::uint32_t emittedPrimitives{};
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

// Builds the deterministic grid mesh, meshletizes it through MeshletContracts, and
// fails if ValidateMeshletBuild rejects the result before any GPU upload.
[[nodiscard]] std::expected<GpuScene, lgp::framework::Error> BuildGpuScene();

[[nodiscard]] MeshShaderCapabilities QueryMeshShaderCapabilities(ID3D12Device10 &device);

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] BufferBarrierState NoAccessState() noexcept;
[[nodiscard]] BufferBarrierState CopyDestState() noexcept;
[[nodiscard]] BufferBarrierState MeshUnorderedAccessState() noexcept;
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

} // namespace ch22::meshlets::gpu
