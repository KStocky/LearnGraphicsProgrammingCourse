#pragma once

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/device_resources.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <filesystem>

namespace ch09::transient_aliasing::gpu
{

inline constexpr DXGI_FORMAT kTransientFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

[[nodiscard]] lgp::framework::TextureBarrierState UndefinedState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState ShaderResourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(bool headless) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(bool headless) noexcept;
[[nodiscard]] D3D12_RESOURCE_DESC1 TextureDescription(lgp::framework::Extent2D size) noexcept;
[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(
    ID3D12Resource &resource, lgp::framework::TextureBarrierState before, lgp::framework::TextureBarrierState after,
    D3D12_TEXTURE_BARRIER_FLAGS flags = D3D12_TEXTURE_BARRIER_FLAG_NONE) noexcept;

[[nodiscard]] lgp::framework::Status CreateLabShaders(std::filesystem::path const &path,
                                                      lgp::framework::CompiledShader &fullscreenVertexShader,
                                                      lgp::framework::CompiledShader &analyticPixelShader,
                                                      lgp::framework::CompiledShader &copyPixelShader,
                                                      lgp::framework::CompiledShader &accentPixelShader,
                                                      lgp::framework::CompiledShader &compositePixelShader);

[[nodiscard]] lgp::framework::Status CreateLabRootSignature(ID3D12Device10 &device,
                                                            Microsoft::WRL::ComPtr<ID3D12RootSignature> &rootSignature);

[[nodiscard]] lgp::framework::Status CreateLabPipelineStates(
    ID3D12Device10 &device, DXGI_FORMAT frameFormat, ID3D12RootSignature &rootSignature,
    lgp::framework::CompiledShader const &fullscreenVertexShader,
    lgp::framework::CompiledShader const &analyticPixelShader, lgp::framework::CompiledShader const &copyPixelShader,
    lgp::framework::CompiledShader const &accentPixelShader, lgp::framework::CompiledShader const &compositePixelShader,
    Microsoft::WRL::ComPtr<ID3D12PipelineState> &analyticPipeline,
    Microsoft::WRL::ComPtr<ID3D12PipelineState> &copyPipeline,
    Microsoft::WRL::ComPtr<ID3D12PipelineState> &accentPipeline,
    Microsoft::WRL::ComPtr<ID3D12PipelineState> &compositePipeline);

} // namespace ch09::transient_aliasing::gpu
