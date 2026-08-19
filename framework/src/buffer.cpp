#include <lgp/framework/buffer.hpp>

#include <cstring>
#include <string>
#include <utility>

namespace lgp::framework
{
namespace
{

[[nodiscard]] Status ValidateBufferCreateDesc(BufferCreateDesc const &description)
{
    if (description.sizeInBytes == 0U)
    {
        return std::unexpected(MakeError("CreateCommittedBuffer", "Buffer size must be greater than zero."));
    }

    if (description.heapType == D3D12_HEAP_TYPE_UPLOAD && description.initialState != D3D12_RESOURCE_STATE_GENERIC_READ)
    {
        return std::unexpected(
            MakeError("CreateCommittedBuffer", "Upload buffers must use D3D12_RESOURCE_STATE_GENERIC_READ."));
    }

    if (description.heapType == D3D12_HEAP_TYPE_READBACK && description.initialState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        return std::unexpected(
            MakeError("CreateCommittedBuffer", "Readback buffers must use D3D12_RESOURCE_STATE_COPY_DEST."));
    }

    return {};
}

[[nodiscard]] Status SetResourceName(ID3D12Resource &resource, std::wstring_view name)
{
    if (name.empty())
    {
        return {};
    }

    std::wstring const resourceName{name};
    HRESULT const result = resource.SetName(resourceName.c_str());
    if (FAILED(result))
    {
        return std::unexpected(MakeHResultError("ID3D12Object::SetName", result, "Failed to set a buffer debug name."));
    }

    return {};
}

} // namespace

Buffer::Buffer(Buffer &&other) noexcept
{
    *this = std::move(other);
}

Buffer &Buffer::operator=(Buffer &&other) noexcept
{
    if (this != &other)
    {
        Reset();

        resource_ = std::move(other.resource_);
        sizeInBytes_ = std::exchange(other.sizeInBytes_, 0U);
        heapType_ = std::exchange(other.heapType_, D3D12_HEAP_TYPE_DEFAULT);
        initialState_ = std::exchange(other.initialState_, D3D12_RESOURCE_STATE_COMMON);
        mappedData_ = std::exchange(other.mappedData_, nullptr);
    }

    return *this;
}

Buffer::~Buffer()
{
    Reset();
}

D3D12_GPU_VIRTUAL_ADDRESS Buffer::gpu_virtual_address() const noexcept
{
    return resource_ != nullptr ? resource_->GetGPUVirtualAddress() : 0U;
}

std::span<std::byte> Buffer::mapped_bytes() noexcept
{
    if (mappedData_ == nullptr)
    {
        return {};
    }

    return {mappedData_, static_cast<std::size_t>(sizeInBytes_)};
}

std::span<std::byte const> Buffer::mapped_bytes() const noexcept
{
    if (mappedData_ == nullptr)
    {
        return {};
    }

    return {mappedData_, static_cast<std::size_t>(sizeInBytes_)};
}

void Buffer::Reset() noexcept
{
    if (resource_ != nullptr && mappedData_ != nullptr)
    {
        D3D12_RANGE const writtenRange{0U, 0U};
        resource_->Unmap(0U, &writtenRange);
    }

    resource_.Reset();
    sizeInBytes_ = 0U;
    heapType_ = D3D12_HEAP_TYPE_DEFAULT;
    initialState_ = D3D12_RESOURCE_STATE_COMMON;
    mappedData_ = nullptr;
}

Result<Buffer> CreateCommittedBuffer(ID3D12Device &device, BufferCreateDesc const &description)
{
    auto const validation = ValidateBufferCreateDesc(description);
    if (!validation)
    {
        return std::unexpected(std::move(validation.error()));
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = description.heapType;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1U;
    heapProperties.VisibleNodeMask = 1U;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Alignment = 0U;
    resourceDesc.Width = description.sizeInBytes;
    resourceDesc.Height = 1U;
    resourceDesc.DepthOrArraySize = 1U;
    resourceDesc.MipLevels = 1U;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1U;
    resourceDesc.SampleDesc.Quality = 0U;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = description.flags;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    HRESULT const createResult =
        device.CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc, description.initialState,
                                       nullptr, IID_PPV_ARGS(resource.ReleaseAndGetAddressOf()));

    if (FAILED(createResult))
    {
        return std::unexpected(MakeHResultError("ID3D12Device::CreateCommittedResource", createResult,
                                                "Failed to create a buffer resource."));
    }

    auto const nameStatus = SetResourceName(*resource.Get(), description.name);
    if (!nameStatus)
    {
        return std::unexpected(std::move(nameStatus.error()));
    }

    std::byte *mappedData = nullptr;
    if (description.heapType == D3D12_HEAP_TYPE_UPLOAD)
    {
        D3D12_RANGE const readRange{0U, 0U};
        void *mappedPointer = nullptr;
        HRESULT const mapResult = resource->Map(0U, &readRange, &mappedPointer);
        if (FAILED(mapResult))
        {
            return std::unexpected(
                MakeHResultError("ID3D12Resource::Map", mapResult, "Failed to map an upload buffer."));
        }

        mappedData = static_cast<std::byte *>(mappedPointer);
    }

    Buffer buffer;
    buffer.resource_ = std::move(resource);
    buffer.sizeInBytes_ = description.sizeInBytes;
    buffer.heapType_ = description.heapType;
    buffer.initialState_ = description.initialState;
    buffer.mappedData_ = mappedData;
    return buffer;
}

Result<Buffer> CreateUploadBuffer(ID3D12Device &device, std::uint64_t sizeInBytes, std::wstring_view name)
{
    BufferCreateDesc description{};
    description.sizeInBytes = sizeInBytes;
    description.heapType = D3D12_HEAP_TYPE_UPLOAD;
    description.initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
    description.name = std::wstring{name};
    return CreateCommittedBuffer(device, description);
}

Result<Buffer> CreateDefaultBuffer(ID3D12Device &device, std::uint64_t sizeInBytes, D3D12_RESOURCE_STATES initialState,
                                   std::wstring_view name, D3D12_RESOURCE_FLAGS flags)
{
    BufferCreateDesc description{};
    description.sizeInBytes = sizeInBytes;
    description.heapType = D3D12_HEAP_TYPE_DEFAULT;
    description.initialState = initialState;
    description.flags = flags;
    description.name = std::wstring{name};
    return CreateCommittedBuffer(device, description);
}

Status WriteBuffer(Buffer &buffer, std::span<std::byte const> bytes, std::uint64_t destinationOffset)
{
    if (bytes.empty())
    {
        return {};
    }

    if (buffer.mapped_data() == nullptr)
    {
        return std::unexpected(MakeError("WriteBuffer", "The buffer is not CPU mapped."));
    }

    if (destinationOffset > buffer.size_in_bytes() || bytes.size_bytes() > (buffer.size_in_bytes() - destinationOffset))
    {
        return std::unexpected(MakeError("WriteBuffer", "The requested write exceeds the buffer size."));
    }

    std::memcpy(buffer.mapped_data() + destinationOffset, bytes.data(), bytes.size_bytes());
    return {};
}

} // namespace lgp::framework
