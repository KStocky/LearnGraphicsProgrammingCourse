#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12sdklayers.h>

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/device_resources.hpp>

#include <cstring>
#include <string>
#include <utility>

namespace lgp::framework
{
namespace
{

[[nodiscard]] Status SetObjectName(ID3D12Object *object, std::wstring_view name)
{
    if (object == nullptr || name.empty())
    {
        return {};
    }

    std::wstring const objectName{name};
    HRESULT const result = object->SetName(objectName.c_str());
    if (FAILED(result))
    {
        return std::unexpected(MakeHResultError("ID3D12Object::SetName", result, "Failed to set a D3D12 object name."));
    }

    return {};
}

[[nodiscard]] std::wstring MakeFrameResourceName(UINT frameIndex, wchar_t const *suffix)
{
    std::wstring name = L"LGP Frame ";
    name += std::to_wstring(frameIndex);
    name += L" ";
    name += suffix;
    return name;
}

[[nodiscard]] D3D12_VIEWPORT MakeViewport(Extent2D const size)
{
    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0F;
    viewport.TopLeftY = 0.0F;
    viewport.Width = static_cast<float>(size.width);
    viewport.Height = static_cast<float>(size.height);
    viewport.MinDepth = D3D12_MIN_DEPTH;
    viewport.MaxDepth = D3D12_MAX_DEPTH;
    return viewport;
}

[[nodiscard]] D3D12_RECT MakeScissorRect(Extent2D const size)
{
    D3D12_RECT scissorRect{};
    scissorRect.left = 0;
    scissorRect.top = 0;
    scissorRect.right = static_cast<LONG>(size.width);
    scissorRect.bottom = static_cast<LONG>(size.height);
    return scissorRect;
}

} // namespace

DeviceResources::DeviceResources(DeviceResourcesConfiguration configuration) noexcept
    : configuration_{std::move(configuration)}, drawableSize_{configuration_.width, configuration_.height}
{
}

DeviceResources::~DeviceResources()
{
    (void)WaitForGpuIdle();
    ReleaseBackBuffers();
    commandList_.Reset();

    if (fenceEvent_ != nullptr)
    {
        ::CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
}

Result<std::unique_ptr<DeviceResources>> DeviceResources::Create(DeviceResourcesConfiguration const &configuration)
{
    std::unique_ptr<DeviceResources> deviceResources{new DeviceResources(configuration)};
    auto const initialization = deviceResources->Initialize();
    if (!initialization)
    {
        return std::unexpected(std::move(initialization.error()));
    }

    return deviceResources;
}

Status DeviceResources::Initialize()
{
    if (configuration_.width == 0U || configuration_.height == 0U)
    {
        return std::unexpected(
            MakeError("DeviceResources::Create", "Drawable width and height must be greater than zero."));
    }

    if (configuration_.backBufferCount == 0U)
    {
        return std::unexpected(MakeError("DeviceResources::Create", "At least one frame context is required."));
    }

    if (!configuration_.headless && configuration_.backBufferCount < 2U)
    {
        return std::unexpected(
            MakeError("DeviceResources::Create", "Flip-discard swap chains require at least two back buffers."));
    }

    if (configuration_.shaderVisibleDescriptorCount == 0U)
    {
        return std::unexpected(MakeError("DeviceResources::Create",
                                         "The CBV/SRV/UAV descriptor heap must contain at least one descriptor."));
    }

    if (!configuration_.headless && configuration_.windowHandle == nullptr)
    {
        return std::unexpected(
            MakeError("DeviceResources::Create", "A Win32 window handle is required when not running headless."));
    }

    if (configuration_.rtvDescriptorCount < configuration_.backBufferCount)
    {
        configuration_.rtvDescriptorCount = configuration_.backBufferCount;
    }

    auto const debugFeatures = EnableDebugFeatures();
    if (!debugFeatures)
    {
        return std::unexpected(std::move(debugFeatures.error()));
    }

    auto const factoryAndAdapter = CreateFactoryAndAdapter();
    if (!factoryAndAdapter)
    {
        return std::unexpected(std::move(factoryAndAdapter.error()));
    }

    auto const deviceObjects = CreateDeviceObjects();
    if (!deviceObjects)
    {
        return std::unexpected(std::move(deviceObjects.error()));
    }

    auto const descriptorHeaps = CreateDescriptorHeaps();
    if (!descriptorHeaps)
    {
        return std::unexpected(std::move(descriptorHeaps.error()));
    }

    auto const frameResources = CreateFrameResources();
    if (!frameResources)
    {
        return std::unexpected(std::move(frameResources.error()));
    }

    auto const swapChain = CreateSwapChain();
    if (!swapChain)
    {
        return std::unexpected(std::move(swapChain.error()));
    }

    return {};
}

Status DeviceResources::EnableDebugFeatures()
{
    if (configuration_.enableDebugLayer)
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        HRESULT const debugResult = ::D3D12GetDebugInterface(IID_PPV_ARGS(debugController.ReleaseAndGetAddressOf()));
        if (FAILED(debugResult))
        {
            return std::unexpected(
                MakeHResultError("D3D12GetDebugInterface", debugResult, "Failed to acquire the D3D12 debug layer."));
        }

        debugController->EnableDebugLayer();
    }

    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings;
    HRESULT const dredResult = ::D3D12GetDebugInterface(IID_PPV_ARGS(dredSettings.ReleaseAndGetAddressOf()));
    if (FAILED(dredResult))
    {
        if (configuration_.enableDebugLayer)
        {
            return std::unexpected(
                MakeHResultError("D3D12GetDebugInterface", dredResult, "Failed to acquire D3D12 DRED settings."));
        }

        return {};
    }

    dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    dredSettings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    return {};
}

Status DeviceResources::CreateFactoryAndAdapter()
{
    UINT const factoryFlags = configuration_.enableDebugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0U;
    HRESULT const factoryResult = ::CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf()));
    if (FAILED(factoryResult))
    {
        return std::unexpected(
            MakeHResultError("CreateDXGIFactory2", factoryResult, "Failed to create a DXGI factory."));
    }

    if (configuration_.useWarpAdapter)
    {
        HRESULT const warpResult = factory_->EnumWarpAdapter(IID_PPV_ARGS(adapter_.ReleaseAndGetAddressOf()));
        if (FAILED(warpResult))
        {
            return std::unexpected(MakeHResultError("IDXGIFactory7::EnumWarpAdapter", warpResult,
                                                    "Failed to enumerate the WARP adapter."));
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        HRESULT const descResult = adapter_->GetDesc1(&adapterDesc);
        if (FAILED(descResult))
        {
            return std::unexpected(MakeHResultError("IDXGIAdapter1::GetDesc1", descResult,
                                                    "Failed to query the WARP adapter description."));
        }

        adapterInfo_.description = std::wstring{adapterDesc.Description};
        adapterInfo_.dedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
        adapterInfo_.isWarp = true;
        return {};
    }

    for (UINT adapterIndex = 0U;; ++adapterIndex)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidateAdapter;
        HRESULT const enumResult =
            factory_->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                 IID_PPV_ARGS(candidateAdapter.ReleaseAndGetAddressOf()));

        if (enumResult == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }

        if (FAILED(enumResult))
        {
            return std::unexpected(MakeHResultError("IDXGIFactory7::EnumAdapterByGpuPreference", enumResult,
                                                    "Failed while enumerating D3D12 adapters."));
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        HRESULT const descResult = candidateAdapter->GetDesc1(&adapterDesc);
        if (FAILED(descResult))
        {
            return std::unexpected(
                MakeHResultError("IDXGIAdapter1::GetDesc1", descResult, "Failed to query a DXGI adapter description."));
        }

        if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U)
        {
            continue;
        }

        HRESULT const probeResult =
            ::D3D12CreateDevice(candidateAdapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr);
        if (SUCCEEDED(probeResult))
        {
            adapter_ = std::move(candidateAdapter);
            adapterInfo_.description = std::wstring{adapterDesc.Description};
            adapterInfo_.dedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
            adapterInfo_.isWarp = false;
            return {};
        }
    }

    return std::unexpected(MakeError("IDXGIFactory7::EnumAdapterByGpuPreference",
                                     "No hardware D3D12 adapter was suitable. Use --warp to force the WARP adapter."));
}

Status DeviceResources::CreateDeviceObjects()
{
    HRESULT const deviceResult =
        ::D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device_.ReleaseAndGetAddressOf()));
    if (FAILED(deviceResult))
    {
        return std::unexpected(
            MakeHResultError("D3D12CreateDevice", deviceResult, "Failed to create the D3D12 device."));
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    HRESULT const optionsResult =
        device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12));
    if (FAILED(optionsResult))
    {
        return std::unexpected(MakeHResultError("ID3D12Device::CheckFeatureSupport", optionsResult,
                                                "Failed to query enhanced barrier support."));
    }
    if (!options12.EnhancedBarriersSupported)
    {
        return std::unexpected(
            MakeError("ID3D12Device::CheckFeatureSupport", "The selected adapter does not support enhanced barriers."));
    }

    if (configuration_.enableDebugLayer)
    {
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(device_.As(&infoQueue)) && infoQueue != nullptr)
        {
            (void)infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            (void)infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        }
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0U;

    HRESULT const queueResult =
        device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(graphicsQueue_.ReleaseAndGetAddressOf()));
    if (FAILED(queueResult))
    {
        return std::unexpected(MakeHResultError("ID3D12Device::CreateCommandQueue", queueResult,
                                                "Failed to create the direct command queue."));
    }

    HRESULT const fenceResult =
        device_->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.ReleaseAndGetAddressOf()));
    if (FAILED(fenceResult))
    {
        return std::unexpected(
            MakeHResultError("ID3D12Device::CreateFence", fenceResult, "Failed to create the frame fence."));
    }

    fenceEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent_ == nullptr)
    {
        return std::unexpected(MakeLastError("CreateEventW", "Failed to create a fence completion event."));
    }

    auto deviceName = SetObjectName(device_.Get(), L"LGP Device");
    if (!deviceName)
    {
        return std::unexpected(std::move(deviceName.error()));
    }

    auto queueName = SetObjectName(graphicsQueue_.Get(), L"LGP Graphics Queue");
    if (!queueName)
    {
        return std::unexpected(std::move(queueName.error()));
    }

    auto fenceName = SetObjectName(fence_.Get(), L"LGP Frame Fence");
    if (!fenceName)
    {
        return std::unexpected(std::move(fenceName.error()));
    }

    nextFenceValue_ = 0U;
    return {};
}

Status DeviceResources::CreateDescriptorHeaps()
{
    auto rtvHeapResult = CreateDescriptorHeap(*device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                              configuration_.rtvDescriptorCount, false, L"LGP RTV Heap");
    if (!rtvHeapResult)
    {
        return std::unexpected(std::move(rtvHeapResult.error()));
    }

    auto shaderVisibleHeapResult =
        CreateDescriptorHeap(*device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                             configuration_.shaderVisibleDescriptorCount, true, L"LGP CBV/SRV/UAV Heap");
    if (!shaderVisibleHeapResult)
    {
        return std::unexpected(std::move(shaderVisibleHeapResult.error()));
    }

    rtvHeap_ = std::move(rtvHeapResult.value());
    shaderVisibleCbvSrvUavHeap_ = std::move(shaderVisibleHeapResult.value());
    return {};
}

Status DeviceResources::CreateFrameResources()
{
    frames_.clear();
    frames_.resize(configuration_.backBufferCount);

    for (UINT frameIndex = 0U; frameIndex < configuration_.backBufferCount; ++frameIndex)
    {
        FrameResources &frame = frames_[frameIndex];

        HRESULT const allocatorResult = device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(frame.commandAllocator.ReleaseAndGetAddressOf()));

        if (FAILED(allocatorResult))
        {
            return std::unexpected(MakeHResultError("ID3D12Device::CreateCommandAllocator", allocatorResult,
                                                    "Failed to create a frame command allocator."));
        }

        auto allocatorName =
            SetObjectName(frame.commandAllocator.Get(), MakeFrameResourceName(frameIndex, L"Command Allocator"));
        if (!allocatorName)
        {
            return std::unexpected(std::move(allocatorName.error()));
        }

        auto descriptorAllocation = rtvHeap_.Allocate(1U);
        if (!descriptorAllocation)
        {
            return std::unexpected(std::move(descriptorAllocation.error()));
        }
        frame.renderTargetView = descriptorAllocation.value();

        if (configuration_.headless)
        {
            D3D12_HEAP_PROPERTIES heapProperties{};
            heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC1 renderTargetDescription{};
            renderTargetDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            renderTargetDescription.Width = configuration_.width;
            renderTargetDescription.Height = configuration_.height;
            renderTargetDescription.DepthOrArraySize = 1U;
            renderTargetDescription.MipLevels = 1U;
            renderTargetDescription.Format = configuration_.backBufferFormat;
            renderTargetDescription.SampleDesc.Count = 1U;
            renderTargetDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            renderTargetDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            HRESULT const targetResult = device_->CreateCommittedResource3(
                &heapProperties, D3D12_HEAP_FLAG_NONE, &renderTargetDescription, D3D12_BARRIER_LAYOUT_COMMON, nullptr,
                nullptr, 0U, nullptr, IID_PPV_ARGS(frame.renderTarget.ReleaseAndGetAddressOf()));
            if (FAILED(targetResult))
            {
                return std::unexpected(MakeHResultError("ID3D12Device10::CreateCommittedResource3", targetResult,
                                                        "Failed to create a headless render target."));
            }

            device_->CreateRenderTargetView(frame.renderTarget.Get(), nullptr, frame.renderTargetView.cpuHandle);
            auto targetName =
                SetObjectName(frame.renderTarget.Get(), MakeFrameResourceName(frameIndex, L"Headless Render Target"));
            if (!targetName)
            {
                return std::unexpected(std::move(targetName.error()));
            }
        }
    }

    HRESULT const listResult =
        device_->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_.front().commandAllocator.Get(), nullptr,
                                   IID_PPV_ARGS(commandList_.ReleaseAndGetAddressOf()));

    if (FAILED(listResult))
    {
        return std::unexpected(MakeHResultError("ID3D12Device::CreateCommandList", listResult,
                                                "Failed to create the graphics command list."));
    }

    auto commandListName = SetObjectName(commandList_.Get(), L"LGP Graphics Command List");
    if (!commandListName)
    {
        return std::unexpected(std::move(commandListName.error()));
    }

    HRESULT const closeResult = commandList_->Close();
    if (FAILED(closeResult))
    {
        return std::unexpected(MakeHResultError("ID3D12GraphicsCommandList::Close", closeResult,
                                                "Failed to close the initial graphics command list."));
    }

    return {};
}

Status DeviceResources::CreateSwapChain()
{
    if (configuration_.headless)
    {
        return {};
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width = configuration_.width;
    swapChainDesc.Height = configuration_.height;
    swapChainDesc.Format = configuration_.backBufferFormat;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1U;
    swapChainDesc.SampleDesc.Quality = 0U;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = configuration_.backBufferCount;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapChainDesc.Flags = 0U;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT const createResult =
        factory_->CreateSwapChainForHwnd(graphicsQueue_.Get(), configuration_.windowHandle, &swapChainDesc, nullptr,
                                         nullptr, swapChain1.ReleaseAndGetAddressOf());

    if (FAILED(createResult))
    {
        return std::unexpected(MakeHResultError("IDXGIFactory7::CreateSwapChainForHwnd", createResult,
                                                "Failed to create the flip-discard swap chain."));
    }

    HRESULT const associationResult =
        factory_->MakeWindowAssociation(configuration_.windowHandle, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(associationResult))
    {
        return std::unexpected(MakeHResultError("IDXGIFactory7::MakeWindowAssociation", associationResult,
                                                "Failed to disable DXGI Alt+Enter handling."));
    }

    HRESULT const queryResult = swapChain1.As(&swapChain_);
    if (FAILED(queryResult))
    {
        return std::unexpected(
            MakeHResultError("IDXGISwapChain1::QueryInterface", queryResult, "Failed to query IDXGISwapChain4."));
    }

    return RecreateBackBuffers();
}

Status DeviceResources::RecreateBackBuffers()
{
    if (configuration_.headless)
    {
        return {};
    }

    for (UINT frameIndex = 0U; frameIndex < configuration_.backBufferCount; ++frameIndex)
    {
        FrameResources &frame = frames_[frameIndex];

        HRESULT const bufferResult =
            swapChain_->GetBuffer(frameIndex, IID_PPV_ARGS(frame.renderTarget.ReleaseAndGetAddressOf()));
        if (FAILED(bufferResult))
        {
            return std::unexpected(MakeHResultError("IDXGISwapChain4::GetBuffer", bufferResult,
                                                    "Failed to acquire a swap-chain back buffer."));
        }

        device_->CreateRenderTargetView(frame.renderTarget.Get(), nullptr, frame.renderTargetView.cpuHandle);

        auto renderTargetName =
            SetObjectName(frame.renderTarget.Get(), MakeFrameResourceName(frameIndex, L"Back Buffer"));
        if (!renderTargetName)
        {
            return std::unexpected(std::move(renderTargetName.error()));
        }

        frame.fenceValue = 0U;
    }

    drawableSize_ = {configuration_.width, configuration_.height};
    return {};
}

void DeviceResources::ReleaseBackBuffers() noexcept
{
    for (FrameResources &frame : frames_)
    {
        frame.renderTarget.Reset();
        frame.fenceValue = 0U;
    }
}

Status DeviceResources::WaitForFenceValue(std::uint64_t value)
{
    if (value == 0U)
    {
        return {};
    }

    if (fence_ == nullptr || fenceEvent_ == nullptr)
    {
        return std::unexpected(
            MakeError("DeviceResources::WaitForFenceValue", "The frame fence has not been initialized."));
    }

    if (fence_->GetCompletedValue() >= value)
    {
        return {};
    }

    HRESULT const setEventResult = fence_->SetEventOnCompletion(value, fenceEvent_);
    if (FAILED(setEventResult))
    {
        return std::unexpected(MakeHResultError("ID3D12Fence::SetEventOnCompletion", setEventResult,
                                                "Failed to arm the fence completion event."));
    }

    DWORD const waitResult = ::WaitForSingleObjectEx(fenceEvent_, INFINITE, FALSE);
    if (waitResult != WAIT_OBJECT_0)
    {
        if (waitResult == WAIT_FAILED)
        {
            return std::unexpected(MakeLastError("WaitForSingleObjectEx", "Failed while waiting for GPU completion."));
        }

        return std::unexpected(
            MakeError("WaitForSingleObjectEx", "Unexpected wait result while waiting for GPU completion."));
    }

    return {};
}

Result<std::uint64_t> DeviceResources::Signal()
{
    if (graphicsQueue_ == nullptr || fence_ == nullptr)
    {
        return std::unexpected(
            MakeError("DeviceResources::Signal", "The command queue or fence has not been initialized."));
    }

    ++nextFenceValue_;

    HRESULT const signalResult = graphicsQueue_->Signal(fence_.Get(), nextFenceValue_);
    if (FAILED(signalResult))
    {
        return std::unexpected(
            MakeHResultError("ID3D12CommandQueue::Signal", signalResult, "Failed to signal the frame fence."));
    }

    return nextFenceValue_;
}

Status DeviceResources::WaitForGpuIdle()
{
    if (graphicsQueue_ == nullptr || fence_ == nullptr)
    {
        return {};
    }

    auto const signalValue = Signal();
    if (!signalValue)
    {
        return std::unexpected(std::move(signalValue.error()));
    }

    return WaitForFenceValue(signalValue.value());
}

Result<FrameContext> DeviceResources::BeginFrame(std::uint64_t frameIndex)
{
    if (frames_.empty())
    {
        return std::unexpected(MakeError("DeviceResources::BeginFrame", "No frame resources have been created."));
    }

    UINT frameSlot = 0U;
    if (configuration_.headless)
    {
        frameSlot = static_cast<UINT>(frameIndex % frames_.size());
    }
    else
    {
        frameSlot = swapChain_->GetCurrentBackBufferIndex();
    }

    FrameResources &frame = frames_[frameSlot];

    auto const waitStatus = WaitForFenceValue(frame.fenceValue);
    if (!waitStatus)
    {
        return std::unexpected(std::move(waitStatus.error()));
    }

    HRESULT const allocatorReset = frame.commandAllocator->Reset();
    if (FAILED(allocatorReset))
    {
        return std::unexpected(MakeHResultError("ID3D12CommandAllocator::Reset", allocatorReset,
                                                "Failed to reset the frame command allocator."));
    }

    HRESULT const listReset = commandList_->Reset(frame.commandAllocator.Get(), nullptr);
    if (FAILED(listReset))
    {
        return std::unexpected(MakeHResultError("ID3D12GraphicsCommandList::Reset", listReset,
                                                "Failed to reset the graphics command list."));
    }

    FrameContext context{
        *this,
        frameIndex,
        frameSlot,
        frame.commandAllocator.Get(),
        commandList_.Get(),
        frame.renderTarget.Get(),
        frame.renderTargetView.cpuHandle,
        shaderVisibleCbvSrvUavHeap_.Get(),
        configuration_.backBufferFormat,
        configuration_.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT,
        MakeViewport(drawableSize_),
        MakeScissorRect(drawableSize_),
        drawableSize_,
        configuration_.headless,
    };

    return context;
}

Status DeviceResources::EndFrame(FrameContext const &frameContext)
{
    HRESULT const closeResult = commandList_->Close();
    if (FAILED(closeResult))
    {
        return std::unexpected(MakeHResultError("ID3D12GraphicsCommandList::Close", closeResult,
                                                "Failed to close the graphics command list."));
    }

    ID3D12CommandList *const commandLists[] = {commandList_.Get()};
    graphicsQueue_->ExecuteCommandLists(1U, commandLists);

    auto const fenceValue = Signal();
    if (!fenceValue)
    {
        return std::unexpected(std::move(fenceValue.error()));
    }

    frames_[frameContext.frameSlot].fenceValue = fenceValue.value();

    if (!configuration_.headless)
    {
        HRESULT const presentResult = swapChain_->Present(0U, 0U);
        if (presentResult != DXGI_STATUS_OCCLUDED && FAILED(presentResult))
        {
            return std::unexpected(
                MakeHResultError("IDXGISwapChain4::Present", presentResult, "Failed to present the swap chain."));
        }
    }

    return {};
}

Status DeviceResources::Resize(std::uint32_t width, std::uint32_t height)
{
    configuration_.width = width;
    configuration_.height = height;
    drawableSize_ = {width, height};

    if (configuration_.headless)
    {
        return {};
    }

    if (width == 0U || height == 0U)
    {
        return {};
    }

    auto const idleStatus = WaitForGpuIdle();
    if (!idleStatus)
    {
        return std::unexpected(std::move(idleStatus.error()));
    }

    ReleaseBackBuffers();

    HRESULT const resizeResult =
        swapChain_->ResizeBuffers(configuration_.backBufferCount, width, height, configuration_.backBufferFormat, 0U);
    if (FAILED(resizeResult))
    {
        return std::unexpected(MakeHResultError("IDXGISwapChain4::ResizeBuffers", resizeResult,
                                                "Failed to resize the flip-discard swap chain."));
    }

    return RecreateBackBuffers();
}

Result<RenderTargetReadback> DeviceResources::ReadBackRenderTarget(UINT frameSlot)
{
    if (frameSlot >= frames_.size() || frames_[frameSlot].renderTarget == nullptr)
    {
        return std::unexpected(
            MakeError("DeviceResources::ReadBackRenderTarget", "The requested frame does not have a render target."));
    }

    auto idleStatus = WaitForGpuIdle();
    if (!idleStatus)
    {
        return std::unexpected(std::move(idleStatus.error()));
    }

    ID3D12Resource *const source = frames_[frameSlot].renderTarget.Get();
    D3D12_RESOURCE_DESC const sourceDescription = source->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0U;
    UINT64 rowSize = 0U;
    UINT64 totalBytes = 0U;
    device_->GetCopyableFootprints(&sourceDescription, 0U, 1U, 0U, &footprint, &rowCount, &rowSize, &totalBytes);

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readbackDescription{};
    readbackDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDescription.Width = totalBytes;
    readbackDescription.Height = 1U;
    readbackDescription.DepthOrArraySize = 1U;
    readbackDescription.MipLevels = 1U;
    readbackDescription.SampleDesc.Count = 1U;
    readbackDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
    HRESULT const bufferResult = device_->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(readbackBuffer.ReleaseAndGetAddressOf()));
    if (FAILED(bufferResult))
    {
        return std::unexpected(MakeHResultError("ID3D12Device::CreateCommittedResource", bufferResult,
                                                "Failed to create a render-target readback buffer."));
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    HRESULT const allocatorResult = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                    IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf()));
    if (FAILED(allocatorResult))
    {
        return std::unexpected(MakeHResultError("ID3D12Device::CreateCommandAllocator", allocatorResult,
                                                "Failed to create a readback command allocator."));
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> list;
    HRESULT const listResult = device_->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                          IID_PPV_ARGS(list.ReleaseAndGetAddressOf()));
    if (FAILED(listResult))
    {
        return std::unexpected(MakeHResultError("ID3D12Device::CreateCommandList", listResult,
                                                "Failed to create a readback command list."));
    }

    TextureBarrierState constexpr common{
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        D3D12_BARRIER_LAYOUT_COMMON,
    };
    TextureBarrierState constexpr copySource{
        D3D12_BARRIER_SYNC_COPY,
        D3D12_BARRIER_ACCESS_COPY_SOURCE,
        D3D12_BARRIER_LAYOUT_COPY_SOURCE,
    };
    TransitionTexture(*list.Get(), *source, common, copySource);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readbackBuffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource = source;
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sourceLocation.SubresourceIndex = 0U;
    list->CopyTextureRegion(&destination, 0U, 0U, 0U, &sourceLocation, nullptr);

    TransitionTexture(*list.Get(), *source, copySource, common);

    HRESULT const closeResult = list->Close();
    if (FAILED(closeResult))
    {
        return std::unexpected(MakeHResultError("ID3D12GraphicsCommandList::Close", closeResult,
                                                "Failed to close the readback command list."));
    }
    ID3D12CommandList *const lists[]{list.Get()};
    graphicsQueue_->ExecuteCommandLists(1U, lists);
    auto signalResult = Signal();
    if (!signalResult)
    {
        return std::unexpected(std::move(signalResult.error()));
    }
    auto waitStatus = WaitForFenceValue(signalResult.value());
    if (!waitStatus)
    {
        return std::unexpected(std::move(waitStatus.error()));
    }

    RenderTargetReadback readback{};
    readback.size = drawableSize_;
    readback.format = sourceDescription.Format;
    readback.rowPitch = footprint.Footprint.RowPitch;
    readback.pixels.resize(static_cast<std::size_t>(totalBytes));

    void *mappedData = nullptr;
    D3D12_RANGE const readRange{0U, static_cast<SIZE_T>(totalBytes)};
    HRESULT const mapResult = readbackBuffer->Map(0U, &readRange, &mappedData);
    if (FAILED(mapResult))
    {
        return std::unexpected(
            MakeHResultError("ID3D12Resource::Map", mapResult, "Failed to map the render-target readback buffer."));
    }
    std::memcpy(readback.pixels.data(), mappedData, static_cast<std::size_t>(totalBytes));
    D3D12_RANGE const writtenRange{0U, 0U};
    readbackBuffer->Unmap(0U, &writtenRange);
    return readback;
}

} // namespace lgp::framework
