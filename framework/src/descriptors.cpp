#include <lgp/framework/descriptors.hpp>

#include <algorithm>
#include <string>

namespace lgp::framework
{

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorAllocation::CpuHandle(UINT index) const noexcept
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpuHandle;
    handle.ptr += static_cast<SIZE_T>(descriptorSize) * index;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorAllocation::GpuHandle(UINT index) const noexcept
{
    if (!shaderVisible)
    {
        return {};
    }

    D3D12_GPU_DESCRIPTOR_HANDLE handle = gpuHandle;
    handle.ptr += static_cast<UINT64>(descriptorSize) * index;
    return handle;
}

DescriptorAllocator::DescriptorAllocator(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity, UINT descriptorSize,
                                         D3D12_CPU_DESCRIPTOR_HANDLE cpuStart, D3D12_GPU_DESCRIPTOR_HANDLE gpuStart,
                                         bool shaderVisible) noexcept
{
    Reset(type, capacity, descriptorSize, cpuStart, gpuStart, shaderVisible);
}

void DescriptorAllocator::Reset(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity, UINT descriptorSize,
                                D3D12_CPU_DESCRIPTOR_HANDLE cpuStart, D3D12_GPU_DESCRIPTOR_HANDLE gpuStart,
                                bool shaderVisible) noexcept
{
    type_ = type;
    capacity_ = capacity;
    descriptorSize_ = descriptorSize;
    cpuStart_ = cpuStart;
    gpuStart_ = gpuStart;
    shaderVisible_ = shaderVisible;

    freeBlocks_.clear();
    if (capacity_ != 0U)
    {
        freeBlocks_.push_back({0U, capacity_});
    }
}

Result<DescriptorAllocation> DescriptorAllocator::Allocate(UINT count)
{
    if (count == 0U)
    {
        return std::unexpected(
            MakeError("DescriptorAllocator::Allocate", "Descriptor count must be greater than zero."));
    }

    if (capacity_ == 0U || descriptorSize_ == 0U)
    {
        return std::unexpected(MakeError("DescriptorAllocator::Allocate", "Descriptor allocator is not initialized."));
    }

    for (auto freeBlock = freeBlocks_.begin(); freeBlock != freeBlocks_.end(); ++freeBlock)
    {
        if (freeBlock->count < count)
        {
            continue;
        }

        DescriptorAllocation allocation{};
        allocation.type = type_;
        allocation.cpuHandle = cpuStart_;
        allocation.gpuHandle = shaderVisible_ ? gpuStart_ : D3D12_GPU_DESCRIPTOR_HANDLE{};
        allocation.descriptorSize = descriptorSize_;
        allocation.offset = freeBlock->offset;
        allocation.count = count;
        allocation.shaderVisible = shaderVisible_;

        allocation.cpuHandle.ptr += static_cast<SIZE_T>(descriptorSize_) * allocation.offset;
        if (shaderVisible_)
        {
            allocation.gpuHandle.ptr += static_cast<UINT64>(descriptorSize_) * allocation.offset;
        }

        freeBlock->offset += count;
        freeBlock->count -= count;
        if (freeBlock->count == 0U)
        {
            freeBlocks_.erase(freeBlock);
        }

        return allocation;
    }

    return std::unexpected(MakeError("DescriptorAllocator::Allocate", "Descriptor heap capacity has been exhausted."));
}

void DescriptorAllocator::Free(DescriptorAllocation allocation) noexcept
{
    if (!allocation)
    {
        return;
    }

    if (allocation.type != type_ || allocation.offset > capacity_ || allocation.count > (capacity_ - allocation.offset))
    {
        return;
    }

    auto const insertPosition =
        std::lower_bound(freeBlocks_.begin(), freeBlocks_.end(), allocation.offset,
                         [](FreeBlock const &block, UINT offset) { return block.offset < offset; });

    freeBlocks_.insert(insertPosition, FreeBlock{allocation.offset, allocation.count});
    CoalesceFreeBlocks();
}

UINT DescriptorAllocator::available() const noexcept
{
    UINT descriptorCount = 0;
    for (FreeBlock const &freeBlock : freeBlocks_)
    {
        descriptorCount += freeBlock.count;
    }

    return descriptorCount;
}

void DescriptorAllocator::CoalesceFreeBlocks() noexcept
{
    if (freeBlocks_.empty())
    {
        return;
    }

    std::vector<FreeBlock> mergedBlocks;
    mergedBlocks.reserve(freeBlocks_.size());
    mergedBlocks.push_back(freeBlocks_.front());

    for (std::size_t blockIndex = 1; blockIndex < freeBlocks_.size(); ++blockIndex)
    {
        FreeBlock const currentBlock = freeBlocks_[blockIndex];
        FreeBlock &mergedBlock = mergedBlocks.back();

        UINT const mergedEnd = mergedBlock.offset + mergedBlock.count;
        if (currentBlock.offset <= mergedEnd)
        {
            UINT const currentEnd = currentBlock.offset + currentBlock.count;
            if (currentEnd > mergedEnd)
            {
                mergedBlock.count = currentEnd - mergedBlock.offset;
            }
        }
        else
        {
            mergedBlocks.push_back(currentBlock);
        }
    }

    freeBlocks_ = std::move(mergedBlocks);
}

Result<DescriptorAllocation> DescriptorHeap::Allocate(UINT count)
{
    return allocator_.Allocate(count);
}

void DescriptorHeap::Free(DescriptorAllocation allocation) noexcept
{
    allocator_.Free(allocation);
}

Result<DescriptorHeap> CreateDescriptorHeap(ID3D12Device &device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity,
                                            bool shaderVisible, std::wstring_view name)
{
    if (capacity == 0U)
    {
        return std::unexpected(
            MakeError("CreateDescriptorHeap", "Descriptor heap capacity must be greater than zero."));
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = type;
    heapDesc.NumDescriptors = capacity;
    heapDesc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    heapDesc.NodeMask = 0U;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    HRESULT const createResult = device.CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(heap.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(MakeHResultError("ID3D12Device::CreateDescriptorHeap", createResult,
                                                "Failed to create a descriptor heap."));
    }

    if (!name.empty())
    {
        std::wstring const heapName{name};
        HRESULT const nameResult = heap->SetName(heapName.c_str());
        if (FAILED(nameResult))
        {
            return std::unexpected(
                MakeHResultError("ID3D12Object::SetName", nameResult, "Failed to name a descriptor heap."));
        }
    }

    DescriptorHeap descriptorHeap;
    descriptorHeap.heap_ = std::move(heap);
    descriptorHeap.type_ = type;
    descriptorHeap.descriptorSize_ = device.GetDescriptorHandleIncrementSize(type);
    descriptorHeap.shaderVisible_ = shaderVisible;

    D3D12_CPU_DESCRIPTOR_HANDLE const cpuStart = descriptorHeap.heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart{};
    if (shaderVisible)
    {
        gpuStart = descriptorHeap.heap_->GetGPUDescriptorHandleForHeapStart();
    }

    descriptorHeap.allocator_.Reset(type, capacity, descriptorHeap.descriptorSize_, cpuStart, gpuStart, shaderVisible);
    return descriptorHeap;
}

} // namespace lgp::framework
