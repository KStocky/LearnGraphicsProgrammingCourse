#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>

#include <lgp/framework/error.hpp>

namespace lgp::framework
{

struct BufferCreateDesc final
{
    std::uint64_t sizeInBytes{};
    D3D12_HEAP_TYPE heapType{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_STATES initialState{D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_FLAGS flags{D3D12_RESOURCE_FLAG_NONE};
    std::wstring name{};
};

class Buffer final
{
  public:
    Buffer() = default;
    Buffer(Buffer &&other) noexcept;
    Buffer &operator=(Buffer &&other) noexcept;
    Buffer(Buffer const &) = delete;
    Buffer &operator=(Buffer const &) = delete;
    ~Buffer();

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return resource_ != nullptr;
    }

    [[nodiscard]] ID3D12Resource *resource() const noexcept
    {
        return resource_.Get();
    }
    [[nodiscard]] std::uint64_t size_in_bytes() const noexcept
    {
        return sizeInBytes_;
    }
    [[nodiscard]] D3D12_HEAP_TYPE heap_type() const noexcept
    {
        return heapType_;
    }
    [[nodiscard]] D3D12_RESOURCE_STATES initial_state() const noexcept
    {
        return initialState_;
    }
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS gpu_virtual_address() const noexcept;

    [[nodiscard]] std::byte *mapped_data() noexcept
    {
        return mappedData_;
    }
    [[nodiscard]] std::byte const *mapped_data() const noexcept
    {
        return mappedData_;
    }
    [[nodiscard]] std::span<std::byte> mapped_bytes() noexcept;
    [[nodiscard]] std::span<std::byte const> mapped_bytes() const noexcept;

  private:
    friend Result<Buffer> CreateCommittedBuffer(ID3D12Device &device, BufferCreateDesc const &description);

    void Reset() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource_{};
    std::uint64_t sizeInBytes_{0};
    D3D12_HEAP_TYPE heapType_{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_STATES initialState_{D3D12_RESOURCE_STATE_COMMON};
    std::byte *mappedData_{nullptr};
};

[[nodiscard]] Result<Buffer> CreateCommittedBuffer(ID3D12Device &device, BufferCreateDesc const &description);
[[nodiscard]] Result<Buffer> CreateUploadBuffer(ID3D12Device &device, std::uint64_t sizeInBytes,
                                                std::wstring_view name = {});
[[nodiscard]] Result<Buffer> CreateDefaultBuffer(ID3D12Device &device, std::uint64_t sizeInBytes,
                                                 D3D12_RESOURCE_STATES initialState, std::wstring_view name = {},
                                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);

[[nodiscard]] Status WriteBuffer(Buffer &buffer, std::span<std::byte const> bytes, std::uint64_t destinationOffset = 0);

template <typename T>
[[nodiscard]] Status WriteBuffer(Buffer &buffer, std::span<T const> elements, std::uint64_t destinationOffset = 0)
{
    static_assert(std::is_trivially_copyable_v<T>);
    return WriteBuffer(buffer, std::as_bytes(elements), destinationOffset);
}

[[nodiscard]] constexpr std::uint32_t AlignConstantBufferSize(std::uint32_t sizeInBytes) noexcept
{
    return (sizeInBytes + 255U) & ~255U;
}

} // namespace lgp::framework
