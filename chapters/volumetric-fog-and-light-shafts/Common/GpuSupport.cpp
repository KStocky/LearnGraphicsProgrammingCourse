#include "GpuSupport.hpp"

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/error.hpp>

#include <cmath>
#include <string>
#include <utility>

namespace ch39::fog::gpu
{
using lgp::framework::MakeError;
using lgp::framework::MakeHResultError;

lgp::framework::Status Validate(Configuration const &config)
{
    if (!std::isfinite(config.medium.extinction) || config.medium.extinction < 0 || config.medium.extinction > 4 ||
        !std::isfinite(config.medium.albedo) || config.medium.albedo < 0 || config.medium.albedo > 1 ||
        !std::isfinite(config.medium.anisotropy) || std::abs(config.medium.anisotropy) >= 0.95F ||
        !std::isfinite(config.distance) || config.distance <= 0 || config.distance > 8 ||
        !std::isfinite(config.light) || config.light < 0 || config.light > 10 || !std::isfinite(config.signature) ||
        !std::isfinite(config.historyShiftX) || std::abs(config.historyShiftX) > 1)
    {
        return std::unexpected(
            MakeError("Synthetic volume configuration", "Invalid bounded medium, distance, light or history."));
    }
    return {};
}

Buffer::Buffer(Buffer &&other) noexcept
{
    *this = std::move(other);
}
Buffer &Buffer::operator=(Buffer &&other) noexcept
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

std::expected<Buffer, lgp::framework::Error> MakeBuffer(ID3D12Device10 &device, std::uint64_t bytes,
                                                        D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, bool mapped)
{
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
        auto const result = buffer.resource_->Map(0U, &range, &pointer);
        if (FAILED(result))
        {
            return std::unexpected(MakeHResultError("Map", result));
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

std::expected<lgp::framework::CompiledShader, lgp::framework::Error> Compile(lgp::framework::ShaderCompiler &compiler,
                                                                             std::filesystem::path const &path,
                                                                             wchar_t const *entry,
                                                                             wchar_t const *profile)
{
    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = path;
    options.entryPoint = entry;
    options.targetProfile = profile;
    options.additionalArguments = {L"-E", entry, L"-T", profile, L"-WX"};
    return compiler.Compile(options);
}

std::expected<Microsoft::WRL::ComPtr<ID3D12RootSignature>, lgp::framework::Error> MakeRoot(
    ID3D12Device10 &device, D3D12_ROOT_SIGNATURE_DESC const &description)
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
    auto const result = device.CreateRootSignature(0U, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                   IID_PPV_ARGS(root.GetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(MakeHResultError("CreateRootSignature", result));
    }
    return root;
}

std::expected<Microsoft::WRL::ComPtr<ID3D12PipelineState>, lgp::framework::Error> MakeCompute(
    ID3D12Device10 &device, ID3D12RootSignature &root, lgp::framework::CompiledShader const &shader)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = &root;
    desc.CS = shader.Bytecode();
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline{};
    auto const hr = device.CreateComputePipelineState(&desc, IID_PPV_ARGS(pipeline.GetAddressOf()));
    if (FAILED(hr))
    {
        return std::unexpected(MakeHResultError("CreateComputePipelineState", hr));
    }
    return pipeline;
}

std::expected<Microsoft::WRL::ComPtr<ID3D12PipelineState>, lgp::framework::Error> MakeGraphics(
    ID3D12Device10 &device, ID3D12RootSignature &root, lgp::framework::CompiledShader const &vertex,
    lgp::framework::CompiledShader const &pixel, DXGI_FORMAT format)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = &root;
    desc.VS = vertex.Bytecode();
    desc.PS = pixel.Bytecode();
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1U;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1U;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline{};
    auto const hr = device.CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pipeline.GetAddressOf()));
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
    list.OMSetRenderTargets(1U, &frame.renderTargetView, FALSE, nullptr);
    list.RSSetViewports(1U, &frame.viewport);
    list.RSSetScissorRects(1U, &frame.scissorRect);
    list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void EndDraw(lgp::framework::FrameContext const &frame)
{
    lgp::framework::TransitionTexture(
        *frame.commandList, *frame.renderTarget,
        {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET},
        {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
         frame.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT});
}
} // namespace ch39::fog::gpu
