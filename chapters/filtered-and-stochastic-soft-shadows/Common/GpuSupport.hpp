#pragma once

#include <lgp/framework/application.hpp>
#include <lgp/framework/shader_compiler.hpp>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>

namespace ch38::soft_shadows::gpu
{
inline constexpr std::uint32_t kResolution = 64U;
inline constexpr std::uint32_t kAreaSamples = 32U;

enum class Technique : std::uint32_t
{
    Hard,
    Pcf,
    Pcss,
    Area
};

enum class VisibilityDomain : std::uint32_t
{
    ShadowMapComparison,
    AnalyticOccluderRays
};

struct Configuration final
{
    Technique technique{Technique::Hard};
    // Linear light-space distances normalized by one far plane, not perspective/clip-space depth.
    // Zero blocker depth is a valid nearest-plane occluder.
    float receiverDepth{0.7F};
    float blockerDepth{0.35F};
    float lightRadius{0.04F};
    float bias{0.001F};
    // Test/diagnostic offset of the projected receiver; the depth field stays fixed.
    float receiverUvOffsetX{};
    float receiverUvOffsetY{};
};

struct Readback final
{
    Configuration configuration{};
    VisibilityDomain domain{VisibilityDomain::ShadowMapComparison};
    // For Area: 32 radially stratified samples with uniform disk marginal
    // area density (UV squared), pdf = 1/(pi*r*r), not 32 IID samples.
    // A zero-radius point emitter has a Dirac distribution, represented by pdf 0.
    std::uint32_t lightSamples{};
    double lightSamplePdf{};
    std::uint32_t width{kResolution};
    std::uint32_t height{kResolution};
    // Unmodified float values copied from a compute UAV through a GPU readback heap.
    std::array<float, kResolution * kResolution> visibility{};
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
} // namespace ch38::soft_shadows::gpu
