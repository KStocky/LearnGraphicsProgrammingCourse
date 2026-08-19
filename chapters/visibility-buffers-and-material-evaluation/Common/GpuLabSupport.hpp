#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ClusteredLightingContracts.hpp"
#include "VisibilityBufferContracts.hpp"

#include <DirectXMath.h>
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
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ch15::visibility_buffer::gpu
{

inline constexpr std::uint32_t kTileWidth = 16U;
inline constexpr std::uint32_t kTileHeight = 16U;
inline constexpr std::uint32_t kClusterSliceCount = 8U;
inline constexpr std::uint32_t kMaximumLightCount = 8U;
inline constexpr std::uint32_t kMaximumDrawCount = 4U;
inline constexpr std::uint32_t kMaximumPrimitiveCount = 64U;
inline constexpr std::uint32_t kMaximumVertexCount = 64U;
inline constexpr std::uint32_t kMaximumIndexCount = kMaximumPrimitiveCount * 3U;
inline constexpr std::size_t kWideGBufferCount = 7U;
inline constexpr std::uint32_t kMaximumRenderWidth = 320U;
inline constexpr std::uint32_t kMaximumRenderHeight = 180U;
inline constexpr std::uint32_t kMaximumTileCountX = (kMaximumRenderWidth + kTileWidth - 1U) / kTileWidth;
inline constexpr std::uint32_t kMaximumTileCountY = (kMaximumRenderHeight + kTileHeight - 1U) / kTileHeight;
inline constexpr std::uint32_t kMaximumClusterCount = kMaximumTileCountX * kMaximumTileCountY * kClusterSliceCount;
inline constexpr std::uint32_t kMaximumLightIndexCount = kMaximumClusterCount * kMaximumLightCount;
inline constexpr std::uint32_t kMaterialTextureSize = 64U;
inline constexpr std::uint32_t kMaterialMipCount = 7U;
inline constexpr float kVerticalFieldOfView = 0.9F;
inline constexpr float kNearPlane = 0.5F;
inline constexpr float kFarPlane = 24.0F;

inline constexpr DXGI_FORMAT kVisibilityFormat = DXGI_FORMAT_R32G32_UINT;
inline constexpr DXGI_FORMAT kDepthResourceFormat = DXGI_FORMAT_R32_TYPELESS;
inline constexpr DXGI_FORMAT kDepthDsvFormat = DXGI_FORMAT_D32_FLOAT;
inline constexpr DXGI_FORMAT kDepthSrvFormat = DXGI_FORMAT_R32_FLOAT;
inline constexpr DXGI_FORMAT kSurfaceFloat4Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
inline constexpr DXGI_FORMAT kSurfaceFloat2Format = DXGI_FORMAT_R32G32_FLOAT;
inline constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
inline constexpr DXGI_FORMAT kMaterialFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

using DepthConvention = ch12::gbuffer::DepthConvention;
using PerspectiveProjection = ch12::gbuffer::PerspectiveProjection;
using CellLightRange = ch14::clustered_lighting::CellLightRange;
using BoundedLightLists = ch14::clustered_lighting::BoundedLightLists;

enum class ScenePreset : std::uint32_t
{
    Diagnostic = 0U,
    Empty,
};

enum class DebugView : std::uint32_t
{
    Final = 0U,
    ReconstructedUv,
    GradientMagnitude,
    Identifiers,
};

struct LabConfiguration final
{
    ScenePreset scene{ScenePreset::Diagnostic};
    DepthConvention depthConvention{DepthConvention::Forward};
    DebugView debugView{DebugView::Final};
    std::uint32_t lightCount{kMaximumLightCount};
};

struct Vertex final
{
    Float3 position{};
    Float3 normal{};
    Float4 tangent{};
    Float2 textureCoordinates{};
};

struct DrawData final
{
    IndexedDrawRange range{};
    Float4 baseTintAndRoughness{};
    Float4 materialParameters{};
};

struct PointLightData final
{
    Float3 position{};
    float radius{};
    Float3 color{};
    float intensity{};

    [[nodiscard]] bool operator==(PointLightData const &) const noexcept = default;
};

struct PixelDiagnostics final
{
    Float3 viewPosition{};
    std::uint32_t status{};
    Float3 normal{};
    std::uint32_t drawIdentifier{};
    Float4 tangent{};
    Float2 textureCoordinates{};
    Float2 textureDdx{};
    Float2 textureDdy{};
    std::uint32_t primitiveIdentifier{};
    std::uint32_t clusterIndex{};

    [[nodiscard]] bool operator==(PixelDiagnostics const &) const noexcept = default;
};

struct FrameConstants final
{
    DirectX::XMFLOAT4X4 projection{};
    DirectX::XMFLOAT4 projectionData{};
    DirectX::XMUINT4 dimensions{};
    DirectX::XMUINT4 counts{};
    DirectX::XMUINT4 clusters{};
    DirectX::XMFLOAT4 slicing{};
};

struct RasterDrawConstants final
{
    DirectX::XMUINT4 identifiers{};
    DirectX::XMFLOAT4 baseTintAndRoughness{};
    DirectX::XMFLOAT4 materialParameters{};
};

struct SceneData final
{
    std::vector<Vertex> vertices{};
    std::vector<std::uint32_t> indices{};
    std::vector<DrawData> draws{};
};

struct TextureReadback final
{
    lgp::framework::Extent2D size{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    std::uint32_t rowPitch{};
    std::vector<std::byte> bytes{};
};

struct FrameReadback final
{
    LabConfiguration configuration{};
    PerspectiveProjection projection{};
    BoundedLightLists lists{};
    std::vector<PointLightData> lights{};
    std::vector<PixelDiagnostics> diagnostics{};
    TextureReadback depth{};
    std::optional<TextureReadback> visibility{};
    std::uint32_t frameSlot{};
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

struct TextureReadbackBuffer final
{
    BufferResource buffer{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    std::uint64_t totalBytes{};
};

static_assert(sizeof(Vertex) == 48U);
static_assert(sizeof(DrawData) == 48U);
static_assert(sizeof(PointLightData) == 32U);
static_assert(sizeof(PixelDiagnostics) == 80U);
static_assert(sizeof(FrameConstants) == 144U);
static_assert(sizeof(RasterDrawConstants) == 48U);
static_assert(sizeof(CellLightRange) == 16U);

[[nodiscard]] constexpr std::uint32_t NormalizeLightCount(std::uint32_t lightCount) noexcept
{
    return std::min(lightCount, kMaximumLightCount);
}

[[nodiscard]] std::expected<void, lgp::framework::Error> ValidateExtent(lgp::framework::Extent2D size);
[[nodiscard]] SceneData BuildScene(ScenePreset preset);
[[nodiscard]] std::vector<PointLightData> BuildLights(std::uint32_t lightCount);
[[nodiscard]] PerspectiveProjection MakeProjection(lgp::framework::Extent2D size,
                                                   DepthConvention depthConvention) noexcept;
[[nodiscard]] std::expected<DirectX::XMFLOAT4X4, lgp::framework::Error> MakeProjectionMatrix(
    PerspectiveProjection projection);
[[nodiscard]] std::expected<std::vector<GeometryVertex>, lgp::framework::Error> BuildContractVertices(
    SceneData const &scene, lgp::framework::Extent2D size, DepthConvention depthConvention);
[[nodiscard]] std::vector<IndexedDrawRange> BuildDrawRanges(SceneData const &scene);
[[nodiscard]] std::expected<BoundedLightLists, lgp::framework::Error> BuildClusteredLightLists(
    lgp::framework::Extent2D size, PerspectiveProjection projection, std::span<PointLightData const> lights);
[[nodiscard]] std::expected<void, lgp::framework::Error> ValidateScene(SceneData const &scene);

[[nodiscard]] constexpr float DepthClearValue(DepthConvention convention) noexcept
{
    return ch12::gbuffer::DepthClearValue(convention);
}

[[nodiscard]] D3D12_COMPARISON_FUNC DepthFunction(DepthConvention convention) noexcept;
[[nodiscard]] std::uint32_t DepthModeFlag(DepthConvention convention) noexcept;

[[nodiscard]] lgp::framework::TextureBarrierState CommonState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState DepthWriteState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState ComputeShaderResourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState UnorderedAccessState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState CopySourceTextureState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState CopyDestTextureState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept;

[[nodiscard]] BufferBarrierState NoAccessState() noexcept;
[[nodiscard]] BufferBarrierState ComputeUnorderedAccessState() noexcept;
[[nodiscard]] BufferBarrierState CopySourceBufferState() noexcept;

[[nodiscard]] D3D12_RESOURCE_DESC1 MakeTextureDescription(lgp::framework::Extent2D size, DXGI_FORMAT format,
                                                          D3D12_RESOURCE_FLAGS flags,
                                                          std::uint16_t mipLevels = 1U) noexcept;
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

void ClearMappedBuffer(BufferResource &buffer) noexcept;

[[nodiscard]] lgp::framework::Status CompileShader(lgp::framework::ShaderCompiler &compiler,
                                                   lgp::framework::ShaderCompileOptions &options,
                                                   wchar_t const *entryPoint, wchar_t const *targetProfile,
                                                   lgp::framework::CompiledShader &shader);
[[nodiscard]] std::expected<Microsoft::WRL::ComPtr<ID3D12Resource>, lgp::framework::Error>
CreateDiagnosticMaterialTexture(lgp::framework::DeviceResources &deviceResources);
[[nodiscard]] std::expected<TextureReadbackBuffer, lgp::framework::Error> CreateTextureReadbackBuffer(
    ID3D12Device10 &device, D3D12_RESOURCE_DESC const &sourceDescription, std::wstring_view name);
void CopyTextureToReadback(ID3D12GraphicsCommandList7 &commandList, ID3D12Resource &source,
                           TextureReadbackBuffer const &destination) noexcept;
[[nodiscard]] std::expected<TextureReadback, lgp::framework::Error> ResolveTextureReadback(
    TextureReadbackBuffer const &readback, lgp::framework::Extent2D size, DXGI_FORMAT format);
[[nodiscard]] std::expected<std::vector<PixelDiagnostics>, lgp::framework::Error> ResolveDiagnostics(
    BufferResource const &readback, std::uint32_t pixelCount);
[[nodiscard]] std::expected<BoundedLightLists, lgp::framework::Error> ResolveUploadedLightLists(
    BufferResource const &cells, BufferResource const &indices, std::uint32_t cellCount, std::uint32_t lightIndexCount,
    std::uint32_t lightCount);
[[nodiscard]] std::filesystem::path SharedShaderPath();

} // namespace ch15::visibility_buffer::gpu
