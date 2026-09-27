#include "GpuSupport.hpp"

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/error.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace ch37::projection::gpu
{
namespace
{
using lgp::framework::MakeError;
using lgp::framework::MakeHResultError;
} // namespace

auto DefaultConfiguration() -> Configuration
{
    Configuration config{};
    config.velocity.x = *MakeField(config.grid, Layout::XFace);
    config.velocity.y = *MakeField(config.grid, Layout::YFace);
    for (std::uint32_t y = 0; y < config.grid.ny; ++y)
    {
        for (std::uint32_t x = 0; x <= config.grid.nx; ++x)
        {
            config.velocity.x.values[(y + 1U) * (config.grid.nx + 3U) + x + 1U] = (x == 3U ? 0.25 : 0.0);
        }
    }
    return config;
}

auto Validate(Configuration const &config) -> lgp::framework::Status
{
    auto fail = [](char const *reason) -> lgp::framework::Status
    { return std::unexpected(MakeError("Ch37 GPU configuration", reason)); };
    if (!ValidateGrid(config.grid) || !ValidateField(config.grid, config.velocity.x) ||
        !ValidateField(config.grid, config.velocity.y) || config.velocity.x.layout != Layout::XFace ||
        config.velocity.y.layout != Layout::YFace)
    {
        return fail("Invalid grid or MAC velocity fields.");
    }
    for (auto const *field : {&config.velocity.x, &config.velocity.y})
    {
        if (!std::all_of(field->values.begin(), field->values.end(),
                         [](double v) { return std::isfinite(static_cast<float>(v)); }))
        {
            return fail("Velocity outside float32 range.");
        }
    }
    if (config.boundary != Boundary::Solid && config.boundary != Boundary::Open &&
        config.boundary != Boundary::Periodic)
    {
        return fail("Invalid boundary.");
    }
    if (!std::isfinite(static_cast<float>(config.dt)) || static_cast<float>(config.dt) <= 0.0F ||
        !std::isfinite(static_cast<float>(config.grid.h)) || config.settings.maxIterations == 0U ||
        config.settings.maxIterations > kMaxGpuIterations ||
        !std::isfinite(static_cast<float>(config.settings.tolerance)) ||
        static_cast<float>(config.settings.tolerance) <= 0.0F || config.settings.tolerance >= 1.0 ||
        (config.settings.stop != Stop::Tolerance && config.settings.stop != Stop::FixedIterations))
    {
        return fail("Invalid timestep or solver settings.");
    }
    if (config.settings.method != Method::Jacobi && config.settings.method != Method::RedBlack &&
        config.settings.method != Method::ConjugateGradient)
    {
        return fail("Invalid solver method.");
    }
    if (config.settings.method == Method::RedBlack && config.boundary == Boundary::Periodic &&
        ((config.grid.nx & 1U) != 0U || (config.grid.ny & 1U) != 0U))
    {
        return fail("Periodic red-black requires even dimensions.");
    }
    return {};
}

Buffer::Buffer(Buffer &&other) noexcept
{
    *this = std::move(other);
}
auto Buffer::operator=(Buffer &&other) noexcept -> Buffer &
{
    if (this != &other)
    {
        if (resource_ && data_)
        {
            D3D12_RANGE const range{0U, 0U};
            resource_->Unmap(0U, &range);
        }
        resource_ = std::move(other.resource_);
        data_ = std::exchange(other.data_, nullptr);
    }
    return *this;
}
Buffer::~Buffer()
{
    if (resource_ && data_)
    {
        D3D12_RANGE const range{0U, 0U};
        resource_->Unmap(0U, &range);
    }
}

auto MakeBuffer(ID3D12Device10 &device, std::uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                bool mapped) -> std::expected<Buffer, lgp::framework::Error>
{
    if (bytes == 0U)
    {
        return std::unexpected(MakeError("Ch37 buffer", "Zero size."));
    }
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1U;
    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = description.DepthOrArraySize = description.MipLevels = 1U;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    Buffer buffer{};
    auto const hr = device.CreateCommittedResource3(&properties, D3D12_HEAP_FLAG_NONE, &description,
                                                    D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0U, nullptr,
                                                    IID_PPV_ARGS(buffer.resource_.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        return std::unexpected(MakeHResultError("CreateCommittedResource3", hr));
    }
    if (mapped)
    {
        D3D12_RANGE const range{0U, 0U};
        void *pointer{};
        auto const map = buffer.resource_->Map(0U, &range, &pointer);
        if (FAILED(map))
        {
            return std::unexpected(MakeHResultError("Map", map));
        }
        buffer.data_ = static_cast<std::byte *>(pointer);
    }
    return buffer;
}

void Barrier(ID3D12GraphicsCommandList7 &list, ID3D12Resource &resource, D3D12_BARRIER_SYNC beforeSync,
             D3D12_BARRIER_ACCESS beforeAccess, D3D12_BARRIER_SYNC afterSync, D3D12_BARRIER_ACCESS afterAccess) noexcept
{
    D3D12_BUFFER_BARRIER barrier{};
    barrier.SyncBefore = beforeSync;
    barrier.AccessBefore = beforeAccess;
    barrier.SyncAfter = afterSync;
    barrier.AccessAfter = afterAccess;
    barrier.pResource = &resource;
    barrier.Size = UINT64_MAX;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_BUFFER;
    group.NumBarriers = 1U;
    group.pBufferBarriers = &barrier;
    list.Barrier(1U, &group);
}

auto Compile(lgp::framework::ShaderCompiler &compiler, std::filesystem::path const &path, wchar_t const *entry,
             wchar_t const *profile) -> std::expected<lgp::framework::CompiledShader, lgp::framework::Error>
{
    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = path;
    options.entryPoint = entry;
    options.targetProfile = profile;
    options.additionalArguments = {L"-E", entry, L"-T", profile, L"-WX"};
    return compiler.Compile(options);
}

auto MakeRoot(ID3D12Device10 &device, D3D12_ROOT_SIGNATURE_DESC const &description)
    -> std::expected<Microsoft::WRL::ComPtr<ID3D12RootSignature>, lgp::framework::Error>
{
    Microsoft::WRL::ComPtr<ID3DBlob> blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> errors{};
    auto const hr = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(),
                                                errors.GetAddressOf());
    if (FAILED(hr))
    {
        auto const text =
            errors ? std::string{static_cast<char const *>(errors->GetBufferPointer()), errors->GetBufferSize()} : "";
        return std::unexpected(MakeHResultError("D3D12SerializeRootSignature", hr, text));
    }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root{};
    auto const created = device.CreateRootSignature(0U, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                    IID_PPV_ARGS(root.GetAddressOf()));
    if (FAILED(created))
    {
        return std::unexpected(MakeHResultError("CreateRootSignature", created));
    }
    return root;
}

auto MakeCompute(ID3D12Device10 &device, ID3D12RootSignature &root, lgp::framework::CompiledShader const &shader)
    -> std::expected<Microsoft::WRL::ComPtr<ID3D12PipelineState>, lgp::framework::Error>
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &root;
    description.CS = shader.Bytecode();
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline{};
    auto const hr = device.CreateComputePipelineState(&description, IID_PPV_ARGS(pipeline.GetAddressOf()));
    if (FAILED(hr))
    {
        return std::unexpected(MakeHResultError("CreateComputePipelineState", hr));
    }
    return pipeline;
}

auto MakeGraphics(ID3D12Device10 &device, ID3D12RootSignature &root, lgp::framework::CompiledShader const &vertex,
                  lgp::framework::CompiledShader const &pixel, DXGI_FORMAT format)
    -> std::expected<Microsoft::WRL::ComPtr<ID3D12PipelineState>, lgp::framework::Error>
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &root;
    description.VS = vertex.Bytecode();
    description.PS = pixel.Bytecode();
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = format;
    description.SampleDesc.Count = 1U;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline{};
    auto const hr = device.CreateGraphicsPipelineState(&description, IID_PPV_ARGS(pipeline.GetAddressOf()));
    if (FAILED(hr))
    {
        return std::unexpected(MakeHResultError("CreateGraphicsPipelineState", hr));
    }
    return pipeline;
}

void BeginDraw(lgp::framework::FrameContext const &frame)
{
    auto &list = *frame.commandList;
    lgp::framework::TransitionTexture(
        list, *frame.renderTarget,
        {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frame.renderTargetInitialLayout},
        {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET});
    constexpr float clear[]{0.06F, 0.12F, 0.3F, 1.0F};
    list.ClearRenderTargetView(frame.renderTargetView, clear, 0U, nullptr);
    list.OMSetRenderTargets(1U, &frame.renderTargetView, FALSE, nullptr);
    list.RSSetViewports(1U, &frame.viewport);
    list.RSSetScissorRects(1U, &frame.scissorRect);
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void EndDraw(lgp::framework::FrameContext const &frame)
{
    auto &list = *frame.commandList;
    lgp::framework::TransitionTexture(
        list, *frame.renderTarget,
        {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET},
        {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
         frame.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT});
}

void ClearFrame(lgp::framework::FrameContext const &frame)
{
    BeginDraw(frame);
    EndDraw(frame);
}
} // namespace ch37::projection::gpu
