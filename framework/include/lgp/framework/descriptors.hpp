#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string_view>
#include <vector>

#include <lgp/framework/error.hpp>

namespace lgp::framework
{

struct DescriptorAllocation final
{
    D3D12_DESCRIPTOR_HEAP_TYPE type{D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES};
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
    UINT descriptorSize{};
    UINT offset{};
    UINT count{};
    bool shaderVisible{false};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return count != 0U;
    }
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT index = 0) const noexcept;
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT index = 0) const noexcept;
};

class DescriptorAllocator final
{
  public:
    DescriptorAllocator() noexcept = default;
    DescriptorAllocator(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity, UINT descriptorSize,
                        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart, D3D12_GPU_DESCRIPTOR_HANDLE gpuStart,
                        bool shaderVisible) noexcept;

    void Reset(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity, UINT descriptorSize,
               D3D12_CPU_DESCRIPTOR_HANDLE cpuStart, D3D12_GPU_DESCRIPTOR_HANDLE gpuStart, bool shaderVisible) noexcept;

    [[nodiscard]] Result<DescriptorAllocation> Allocate(UINT count = 1);
    void Free(DescriptorAllocation allocation) noexcept;

    [[nodiscard]] UINT capacity() const noexcept
    {
        return capacity_;
    }
    [[nodiscard]] UINT available() const noexcept;

  private:
    struct FreeBlock final
    {
        UINT offset{};
        UINT count{};
    };

    void CoalesceFreeBlocks() noexcept;

    D3D12_DESCRIPTOR_HEAP_TYPE type_{D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES};
    UINT capacity_{0};
    UINT descriptorSize_{0};
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart_{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart_{};
    bool shaderVisible_{false};
    std::vector<FreeBlock> freeBlocks_{};
};

class DescriptorHeap final
{
  public:
    DescriptorHeap() = default;
    DescriptorHeap(DescriptorHeap &&) noexcept = default;
    DescriptorHeap &operator=(DescriptorHeap &&) noexcept = default;
    DescriptorHeap(DescriptorHeap const &) = delete;
    DescriptorHeap &operator=(DescriptorHeap const &) = delete;

    [[nodiscard]] ID3D12DescriptorHeap *Get() const noexcept
    {
        return heap_.Get();
    }
    [[nodiscard]] UINT descriptor_size() const noexcept
    {
        return descriptorSize_;
    }
    [[nodiscard]] bool shader_visible() const noexcept
    {
        return shaderVisible_;
    }
    [[nodiscard]] D3D12_DESCRIPTOR_HEAP_TYPE type() const noexcept
    {
        return type_;
    }

    [[nodiscard]] Result<DescriptorAllocation> Allocate(UINT count = 1);
    void Free(DescriptorAllocation allocation) noexcept;

  private:
    friend Result<DescriptorHeap> CreateDescriptorHeap(ID3D12Device &device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                       UINT capacity, bool shaderVisible, std::wstring_view name);

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_{};
    DescriptorAllocator allocator_{};
    D3D12_DESCRIPTOR_HEAP_TYPE type_{D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES};
    UINT descriptorSize_{0};
    bool shaderVisible_{false};
};

[[nodiscard]] Result<DescriptorHeap> CreateDescriptorHeap(ID3D12Device &device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                                          UINT capacity, bool shaderVisible,
                                                          std::wstring_view name = {});

} // namespace lgp::framework
