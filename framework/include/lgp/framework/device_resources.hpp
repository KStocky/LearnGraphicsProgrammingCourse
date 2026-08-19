#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/error.hpp>

namespace lgp::framework
{

struct Extent2D final
{
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return width == 0U || height == 0U;
    }
    constexpr bool operator==(Extent2D const &) const noexcept = default;
};

struct AdapterInfo final
{
    std::wstring description;
    std::uint64_t dedicatedVideoMemory{};
    bool isWarp{false};
};

struct RenderTargetReadback final
{
    Extent2D size{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    std::uint32_t rowPitch{};
    std::vector<std::byte> pixels{};
};

struct DeviceResourcesConfiguration final
{
    HWND windowHandle{};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    std::uint32_t backBufferCount{2};
    std::uint32_t shaderVisibleDescriptorCount{1024};
    std::uint32_t rtvDescriptorCount{32};
    DXGI_FORMAT backBufferFormat{DXGI_FORMAT_R8G8B8A8_UNORM};
    bool enableDebugLayer{true};
    bool useWarpAdapter{false};
    bool headless{false};
};

class DeviceResources;

struct FrameContext final
{
    DeviceResources &deviceResources;
    std::uint64_t frameIndex{};
    UINT frameSlot{};
    ID3D12CommandAllocator *commandAllocator{};
    ID3D12GraphicsCommandList7 *commandList{};
    ID3D12Resource *renderTarget{};
    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView{};
    ID3D12DescriptorHeap *shaderVisibleCbvSrvUavHeap{};
    DXGI_FORMAT renderTargetFormat{DXGI_FORMAT_UNKNOWN};
    D3D12_BARRIER_LAYOUT renderTargetInitialLayout{D3D12_BARRIER_LAYOUT_COMMON};
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissorRect{};
    Extent2D drawableSize{};
    bool headless{false};
};

class DeviceResources final
{
  public:
    ~DeviceResources();

    DeviceResources(DeviceResources const &) = delete;
    DeviceResources &operator=(DeviceResources const &) = delete;

    [[nodiscard]] static Result<std::unique_ptr<DeviceResources>> Create(
        DeviceResourcesConfiguration const &configuration);

    [[nodiscard]] Result<FrameContext> BeginFrame(std::uint64_t frameIndex);
    [[nodiscard]] Status EndFrame(FrameContext const &frameContext);
    [[nodiscard]] Status Resize(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] Status WaitForGpuIdle();
    [[nodiscard]] Result<RenderTargetReadback> ReadBackRenderTarget(UINT frameSlot);

    [[nodiscard]] ID3D12Device10 *device() const noexcept
    {
        return device_.Get();
    }
    [[nodiscard]] IDXGIFactory7 *factory() const noexcept
    {
        return factory_.Get();
    }
    [[nodiscard]] IDXGIAdapter1 *adapter() const noexcept
    {
        return adapter_.Get();
    }
    [[nodiscard]] ID3D12CommandQueue *graphics_queue() const noexcept
    {
        return graphicsQueue_.Get();
    }
    [[nodiscard]] ID3D12Fence *fence() const noexcept
    {
        return fence_.Get();
    }
    [[nodiscard]] IDXGISwapChain4 *swap_chain() const noexcept
    {
        return swapChain_.Get();
    }
    [[nodiscard]] HWND window_handle() const noexcept
    {
        return configuration_.windowHandle;
    }

    [[nodiscard]] DescriptorHeap &shader_visible_cbv_srv_uav_heap() noexcept
    {
        return shaderVisibleCbvSrvUavHeap_;
    }
    [[nodiscard]] DescriptorHeap const &shader_visible_cbv_srv_uav_heap() const noexcept
    {
        return shaderVisibleCbvSrvUavHeap_;
    }
    [[nodiscard]] DescriptorHeap &rtv_heap() noexcept
    {
        return rtvHeap_;
    }
    [[nodiscard]] DescriptorHeap const &rtv_heap() const noexcept
    {
        return rtvHeap_;
    }

    [[nodiscard]] AdapterInfo const &adapter_info() const noexcept
    {
        return adapterInfo_;
    }
    [[nodiscard]] Extent2D drawable_size() const noexcept
    {
        return drawableSize_;
    }
    [[nodiscard]] DXGI_FORMAT back_buffer_format() const noexcept
    {
        return configuration_.backBufferFormat;
    }
    [[nodiscard]] std::uint32_t back_buffer_count() const noexcept
    {
        return configuration_.backBufferCount;
    }
    [[nodiscard]] bool headless() const noexcept
    {
        return configuration_.headless;
    }
    [[nodiscard]] bool has_swap_chain() const noexcept
    {
        return swapChain_ != nullptr;
    }

  private:
    struct FrameResources final
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator{};
        Microsoft::WRL::ComPtr<ID3D12Resource> renderTarget{};
        DescriptorAllocation renderTargetView{};
        std::uint64_t fenceValue{};
    };

    explicit DeviceResources(DeviceResourcesConfiguration configuration) noexcept;

    [[nodiscard]] Status Initialize();
    [[nodiscard]] Status EnableDebugFeatures();
    [[nodiscard]] Status CreateFactoryAndAdapter();
    [[nodiscard]] Status CreateDeviceObjects();
    [[nodiscard]] Status CreateDescriptorHeaps();
    [[nodiscard]] Status CreateFrameResources();
    [[nodiscard]] Status CreateSwapChain();
    [[nodiscard]] Status RecreateBackBuffers();
    void ReleaseBackBuffers() noexcept;
    [[nodiscard]] Status WaitForFenceValue(std::uint64_t value);
    [[nodiscard]] Result<std::uint64_t> Signal();

    DeviceResourcesConfiguration configuration_{};
    AdapterInfo adapterInfo_{};
    Extent2D drawableSize_{};

    Microsoft::WRL::ComPtr<IDXGIFactory7> factory_{};
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_{};
    Microsoft::WRL::ComPtr<ID3D12Device10> device_{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> graphicsQueue_{};
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_{};
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapChain_{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> commandList_{};

    DescriptorHeap rtvHeap_{};
    DescriptorHeap shaderVisibleCbvSrvUavHeap_{};
    std::vector<FrameResources> frames_{};

    HANDLE fenceEvent_{};
    std::uint64_t nextFenceValue_{0};
};

} // namespace lgp::framework
