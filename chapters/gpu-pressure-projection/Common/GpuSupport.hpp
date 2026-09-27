#pragma once

#include "ProjectionContracts.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/shader_compiler.hpp>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>

namespace ch37::projection::gpu
{
// A single GPU dispatch owns each solve; cap its work below the CPU contract's
// 100000-iteration allowance to avoid unbounded WARP or device execution.
inline constexpr std::uint32_t kMaxGpuIterations = 256U;

struct Configuration final
{
    Grid grid{8U, 8U, 1.0};
    Velocity velocity{};
    double dt{0.5};
    Boundary boundary{Boundary::Solid};
    Settings settings{Method::ConjugateGradient, Stop::Tolerance, 100U, 1.0e-4};
};

struct Readback final
{
    ProjectionResult projection{};
    bool gpuComputed{};
    Field divergence{};
};

[[nodiscard]] auto DefaultConfiguration() -> Configuration;
[[nodiscard]] auto Validate(Configuration const &config) -> lgp::framework::Status;

class Buffer final
{
  public:
    Buffer() = default;
    Buffer(Buffer &&other) noexcept;
    auto operator=(Buffer &&other) noexcept -> Buffer &;
    Buffer(Buffer const &) = delete;
    auto operator=(Buffer const &) -> Buffer & = delete;
    ~Buffer();
    [[nodiscard]] auto Get() const noexcept -> ID3D12Resource *
    {
        return resource_.Get();
    }
    [[nodiscard]] auto Data() const noexcept -> std::byte *
    {
        return data_;
    }

  private:
    friend auto MakeBuffer(ID3D12Device10 &, std::uint64_t, D3D12_HEAP_TYPE, D3D12_RESOURCE_FLAGS, bool)
        -> std::expected<Buffer, lgp::framework::Error>;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::byte *data_{};
};

[[nodiscard]] auto MakeBuffer(ID3D12Device10 &device, std::uint64_t bytes, D3D12_HEAP_TYPE heap,
                              D3D12_RESOURCE_FLAGS flags, bool mapped) -> std::expected<Buffer, lgp::framework::Error>;
void Barrier(ID3D12GraphicsCommandList7 &list, ID3D12Resource &resource, D3D12_BARRIER_SYNC beforeSync,
             D3D12_BARRIER_ACCESS beforeAccess, D3D12_BARRIER_SYNC afterSync,
             D3D12_BARRIER_ACCESS afterAccess) noexcept;
[[nodiscard]] auto Compile(lgp::framework::ShaderCompiler &compiler, std::filesystem::path const &path,
                           wchar_t const *entry, wchar_t const *profile)
    -> std::expected<lgp::framework::CompiledShader, lgp::framework::Error>;
[[nodiscard]] auto MakeRoot(ID3D12Device10 &device, D3D12_ROOT_SIGNATURE_DESC const &description)
    -> std::expected<Microsoft::WRL::ComPtr<ID3D12RootSignature>, lgp::framework::Error>;
[[nodiscard]] auto MakeCompute(ID3D12Device10 &device, ID3D12RootSignature &root,
                               lgp::framework::CompiledShader const &shader)
    -> std::expected<Microsoft::WRL::ComPtr<ID3D12PipelineState>, lgp::framework::Error>;
[[nodiscard]] auto MakeGraphics(ID3D12Device10 &device, ID3D12RootSignature &root,
                                lgp::framework::CompiledShader const &vertex,
                                lgp::framework::CompiledShader const &pixel, DXGI_FORMAT format)
    -> std::expected<Microsoft::WRL::ComPtr<ID3D12PipelineState>, lgp::framework::Error>;
void BeginDraw(lgp::framework::FrameContext const &frame);
void EndDraw(lgp::framework::FrameContext const &frame);
void ClearFrame(lgp::framework::FrameContext const &frame);
} // namespace ch37::projection::gpu
