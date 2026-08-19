#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ClusteredLightingContracts.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/device_resources.hpp>
#include <lgp/framework/error.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace ch14::clustered_lighting::gpu
{

inline constexpr std::uint32_t kTileWidth = 16U;
inline constexpr std::uint32_t kTileHeight = 16U;
inline constexpr std::uint32_t kClusterSliceCount = 8U;
inline constexpr std::uint32_t kMaximumLightCount = 64U;
inline constexpr std::uint32_t kMaximumLightIndexCapacity = 262'144U;
inline constexpr std::uint32_t kBufferCanary = 0xCDCDCDCDU;
inline constexpr float kVerticalFieldOfView = 0.9F;
inline constexpr float kNearPlane = 0.5F;
inline constexpr float kFarPlane = 40.0F;
inline constexpr DXGI_FORMAT kDepthResourceFormat = DXGI_FORMAT_R32_TYPELESS;
inline constexpr DXGI_FORMAT kDepthDsvFormat = DXGI_FORMAT_D32_FLOAT;
inline constexpr DXGI_FORMAT kDepthSrvFormat = DXGI_FORMAT_R32_FLOAT;

enum class LightingMode : std::uint32_t
{
    BruteForce = 0U,
    Tiled,
    Clustered,
};

enum class ScenePreset : std::uint32_t
{
    Diagnostic = 0U,
    Empty,
};

enum class LightPreset : std::uint32_t
{
    Diagnostic = 0U,
    Offscreen,
};

enum class DebugView : std::uint32_t
{
    Final = 0U,
    Occupancy,
    Overflow,
};

struct LabConfiguration final
{
    LightingMode mode{LightingMode::Clustered};
    ScenePreset scene{ScenePreset::Diagnostic};
    LightPreset lights{LightPreset::Diagnostic};
    DepthConvention depthConvention{DepthConvention::Forward};
    DebugView debugView{DebugView::Final};
    std::uint32_t lightCount{kMaximumLightCount};
    std::uint32_t capacity{kMaximumLightIndexCapacity};
};

struct Vertex final
{
    Float3 position{};
    Float3 normal{};
};

struct ObjectData final
{
    Float3 translation{};
    float roughness{};
    Float3 scale{};
    float metalness{};
    Float3 baseColor{};
    float padding{};
};

struct PointLightData final
{
    Float3 position{};
    float radius{};
    Float3 color{};
    float intensity{};
};

struct SceneObject final
{
    ObjectData data{};
};

struct TextureReadback final
{
    lgp::framework::Extent2D size{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    std::uint32_t rowPitch{};
    std::vector<std::byte> bytes{};
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

static_assert(sizeof(Vertex) == 24U);
static_assert(sizeof(ObjectData) == 48U);
static_assert(sizeof(PointLightData) == 32U);
static_assert(sizeof(CellLightRange) == 16U);

[[nodiscard]] constexpr std::uint32_t NormalizeLightCount(std::uint32_t lightCount) noexcept
{
    return std::min(lightCount, kMaximumLightCount);
}

[[nodiscard]] constexpr std::uint32_t NormalizeCapacity(std::uint32_t capacity) noexcept
{
    return std::min(capacity, kMaximumLightIndexCapacity);
}

[[nodiscard]] std::expected<std::uint32_t, lgp::framework::Error> CellCountForMode(LightingMode mode,
                                                                                   lgp::framework::Extent2D size);
[[nodiscard]] PerspectiveProjection MakeProjection(lgp::framework::Extent2D size,
                                                   DepthConvention depthConvention) noexcept;
[[nodiscard]] LogDepthSlicing MakeSlicing() noexcept;
[[nodiscard]] std::array<Vertex, 4U> BuildUnitQuadVertices() noexcept;
[[nodiscard]] std::array<std::uint32_t, 6U> BuildUnitQuadIndices() noexcept;
[[nodiscard]] std::vector<SceneObject> BuildScene(ScenePreset scene);
[[nodiscard]] std::vector<PointLightData> BuildLights(LightPreset preset, std::uint32_t lightCount);

[[nodiscard]] lgp::framework::TextureBarrierState CommonState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState DepthWriteState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState ComputeShaderResourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState DepthReadState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState CopySourceTextureState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept;

[[nodiscard]] BufferBarrierState NoAccessState() noexcept;
[[nodiscard]] BufferBarrierState ComputeUnorderedAccessState() noexcept;
[[nodiscard]] BufferBarrierState PixelShaderResourceState() noexcept;
[[nodiscard]] BufferBarrierState CopySourceBufferState() noexcept;

[[nodiscard]] D3D12_RESOURCE_DESC1 MakeTextureDescription(lgp::framework::Extent2D size, DXGI_FORMAT format,
                                                          D3D12_RESOURCE_FLAGS flags) noexcept;
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
[[nodiscard]] std::expected<TextureReadback, lgp::framework::Error> ReadBackTexture(
    lgp::framework::DeviceResources &deviceResources, ID3D12Resource &resource,
    lgp::framework::TextureBarrierState currentState);
[[nodiscard]] std::expected<BoundedLightLists, lgp::framework::Error> BuildCpuReference(
    LabConfiguration configuration, PerspectiveProjection projection, std::span<PointLightData const> lights,
    TextureReadback const &depth);

} // namespace ch14::clustered_lighting::gpu
