#pragma once

#include <lgp/framework/application.hpp>
#include <lgp/framework/shader_compiler.hpp>
#include <wrl/client.h>

#include "FogContracts.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <vector>

namespace ch39::fog::gpu
{
struct Configuration final
{
    Medium medium{};
    float distance{8.F};
    float light{2.F};
    // Additional scene/camera epoch; medium, distance and light are also
    // fingerprinted automatically for temporal-history compatibility.
    float signature{1.F};
    // Diagnostic displacement only. Fixed-camera synthetic rays have no
    // world-space reprojection; any nonzero shift rejects history.
    float historyShiftX{};
    bool resetHistory{};
};

struct Readback final
{
    std::array<float, kWidth * kHeight * 4> pixels{};
    std::vector<float> froxels{};
    [[nodiscard]] float Pixel(std::uint32_t x, std::uint32_t y, std::uint32_t channel) const noexcept
    {
        return pixels[(y * kWidth + x) * 4 + channel];
    }
    [[nodiscard]] float Froxel(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t channel) const noexcept
    {
        return froxels[((z * kHeight + y) * kWidth + x) * 4 + channel];
    }
};

[[nodiscard]] lgp::framework::Status Validate(Configuration const &);

class Buffer final
{
  public:
    Buffer() = default;
    Buffer(Buffer &&) noexcept;
    Buffer &operator=(Buffer &&) noexcept;
    Buffer(Buffer const &) = delete;
    Buffer &operator=(Buffer const &) = delete;
    ~Buffer();
    [[nodiscard]] ID3D12Resource *Get() const noexcept
    {
        return resource_.Get();
    }
    [[nodiscard]] std::byte *Data() const noexcept
    {
        return data_;
    }

  private:
    friend std::expected<Buffer, lgp::framework::Error> MakeBuffer(ID3D12Device10 &, std::uint64_t, D3D12_HEAP_TYPE,
                                                                   D3D12_RESOURCE_FLAGS, bool);
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::byte *data_{};
};

[[nodiscard]] std::expected<Buffer, lgp::framework::Error> MakeBuffer(ID3D12Device10 &, std::uint64_t, D3D12_HEAP_TYPE,
                                                                      D3D12_RESOURCE_FLAGS, bool);
void Barrier(ID3D12GraphicsCommandList7 &, ID3D12Resource &, D3D12_BARRIER_SYNC, D3D12_BARRIER_ACCESS,
             D3D12_BARRIER_SYNC, D3D12_BARRIER_ACCESS) noexcept;
[[nodiscard]] std::expected<lgp::framework::CompiledShader, lgp::framework::Error> Compile(
    lgp::framework::ShaderCompiler &, std::filesystem::path const &, wchar_t const *, wchar_t const *);
[[nodiscard]] std::expected<Microsoft::WRL::ComPtr<ID3D12RootSignature>, lgp::framework::Error> MakeRoot(
    ID3D12Device10 &, D3D12_ROOT_SIGNATURE_DESC const &);
[[nodiscard]] std::expected<Microsoft::WRL::ComPtr<ID3D12PipelineState>, lgp::framework::Error> MakeCompute(
    ID3D12Device10 &, ID3D12RootSignature &, lgp::framework::CompiledShader const &);
[[nodiscard]] std::expected<Microsoft::WRL::ComPtr<ID3D12PipelineState>, lgp::framework::Error> MakeGraphics(
    ID3D12Device10 &, ID3D12RootSignature &, lgp::framework::CompiledShader const &,
    lgp::framework::CompiledShader const &, DXGI_FORMAT);
void BeginDraw(lgp::framework::FrameContext const &);
void EndDraw(lgp::framework::FrameContext const &);

// Synthetic 32x32 orthographic rays through a 16-slice bounded medium;
// no scene mesh, atmospheric perspective or general shadow map is represented.
class SyntheticVolume final
{
  public:
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::DeviceResources &, std::filesystem::path const &);
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &, Configuration const &);
    [[nodiscard]] std::expected<Readback, lgp::framework::Error> ReadBack(lgp::framework::DeviceResources &);
    void Shutdown() noexcept;

  private:
    struct Slot final
    {
        Buffer froxel{}, result{}, readback{}, froxelReadback{};
        bool used{};
    };
    std::vector<Slot> slots_{};
    std::array<Buffer, 2> history_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRoot_{}, graphicsRoot_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> inject_{}, integrate_{}, graphics_{};
    std::uint32_t lastSlot_{}, frameIndex_{};
    Configuration lastConfig_{};
    std::array<bool, 2> historyUsed_{};
    bool rendered_{};
};
} // namespace ch39::fog::gpu
