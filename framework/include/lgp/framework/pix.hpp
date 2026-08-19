#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>

#include <cstdint>

namespace lgp::framework
{

[[nodiscard]] constexpr std::uint64_t PixColor(std::uint8_t red, std::uint8_t green, std::uint8_t blue) noexcept
{
    return 0xff000000ULL | (static_cast<std::uint64_t>(red) << 16U) | (static_cast<std::uint64_t>(green) << 8U) |
           static_cast<std::uint64_t>(blue);
}

[[nodiscard]] constexpr std::uint64_t PixColorIndex(std::uint8_t index) noexcept
{
    return static_cast<std::uint64_t>(index);
}

inline constexpr std::uint64_t kPixDefaultColor = 0U;

class PixEventScope final
{
  public:
    PixEventScope(ID3D12CommandQueue &queue, std::uint64_t color, wchar_t const *label) noexcept;
    PixEventScope(ID3D12GraphicsCommandList &commandList, std::uint64_t color, wchar_t const *label) noexcept;
    PixEventScope(PixEventScope &&other) noexcept;
    PixEventScope &operator=(PixEventScope &&other) noexcept;
    PixEventScope(PixEventScope const &) = delete;
    PixEventScope &operator=(PixEventScope const &) = delete;
    ~PixEventScope();

  private:
    void End() noexcept;

    ID3D12CommandQueue *queue_{};
    ID3D12GraphicsCommandList *commandList_{};
};

void SetPixMarker(ID3D12CommandQueue &queue, std::uint64_t color, wchar_t const *label) noexcept;
void SetPixMarker(ID3D12GraphicsCommandList &commandList, std::uint64_t color, wchar_t const *label) noexcept;

} // namespace lgp::framework
