#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CascadedShadowContracts.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <DirectXMath.h>

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/buffer.hpp>
#include <lgp/framework/device_resources.hpp>
#include <lgp/framework/error.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

// Chapter 24 - Cascaded Shadow Maps (shared GPU support).
//
// This layer is chapter owned but shared by the Starter and Solution so the
// deterministic scene, camera, and the CPU->GPU constant packing stay in one
// place. It deliberately does not own the shadow depth resources, the pipeline
// and root-signature setup, the barriers, the per-slice depth passes, or the
// cascade selection / blend / bias shader code: those stay in each renderer and
// each HLSL file because they are exactly what the chapter teaches. Common only
// removes duplicated scaffolding - scene geometry, the camera, the wrapper that
// drives the CPU cascade contract, texture readback, and shader compilation.

namespace ch24::cascaded_shadows::gpu
{

// The scene renders straight to the swap-chain surface with tone mapping in the
// pixel shader, so there is no separate HDR target. The camera view still needs
// a depth buffer, and the shadow depth array is a typeless R32 so it can be both
// a depth target (per slice) and a Texture2DArray shader resource.
inline constexpr DXGI_FORMAT kSceneDepthFormat = DXGI_FORMAT_D32_FLOAT;
inline constexpr DXGI_FORMAT kShadowTypelessFormat = DXGI_FORMAT_R32_TYPELESS;
inline constexpr DXGI_FORMAT kShadowDepthFormat = DXGI_FORMAT_D32_FLOAT;
inline constexpr DXGI_FORMAT kShadowResourceFormat = DXGI_FORMAT_R32_FLOAT;

// A fixed shadow resolution keeps the WARP tests deterministic and keeps the
// stabilization test's sub-texel vs super-texel offsets well defined.
inline constexpr std::uint32_t kShadowResolution = 1024U;
inline constexpr std::uint32_t kMaxCascades = kMaxCascadeCount;

// Which diagnostic the lighting pass writes. Mirrors DebugViewXxx in the HLSL.
enum class DebugView : std::uint32_t
{
    Shaded = 0U,        // Normal shaded scene with shadows.
    CascadeColor = 1U,  // Flat per-cascade tint (blended when blending is on).
    CascadeShaded = 2U, // Shaded scene tinted by the selected cascade.
    ShadowFactor = 3U,  // Grayscale shadow visibility only.
};

// Per-frame lighting constants. Field order and 16-byte packing must match the
// LabConstants cbuffer in CascadedShadowLab.hlsli exactly; the static_asserts
// below guard every offset so the CPU and HLSL layouts never drift.
struct LabConstants final
{
    DirectX::XMFLOAT4X4 cameraViewProjection{};
    std::array<DirectX::XMFLOAT4X4, kMaxCascades> cascadeViewProjection{};

    DirectX::XMFLOAT4 cascadeSplitViewDepth{};
    DirectX::XMFLOAT4 cascadeBlendStartViewDepth{};
    DirectX::XMFLOAT4 cascadeTexelBiasScale{};

    DirectX::XMFLOAT3 cameraPosition{};
    float exposure{};

    DirectX::XMFLOAT3 cameraForward{};
    float receiverDepthBias{};

    DirectX::XMFLOAT3 directionToLight{};
    float normalOffsetWorld{};

    DirectX::XMFLOAT3 lightColor{};
    float lightIntensity{};

    std::uint32_t cascadeCount{};
    std::uint32_t debugView{};
    std::uint32_t blendEnabled{};
    std::uint32_t shadowsEnabled{};
};

static_assert(sizeof(LabConstants) == 448U, "LabConstants must match the HLSL cbuffer footprint.");
static_assert(offsetof(LabConstants, cameraViewProjection) == 0U);
static_assert(offsetof(LabConstants, cascadeViewProjection) == 64U);
static_assert(offsetof(LabConstants, cascadeSplitViewDepth) == 320U);
static_assert(offsetof(LabConstants, cascadeBlendStartViewDepth) == 336U);
static_assert(offsetof(LabConstants, cascadeTexelBiasScale) == 352U);
static_assert(offsetof(LabConstants, cameraPosition) == 368U);
static_assert(offsetof(LabConstants, exposure) == 380U);
static_assert(offsetof(LabConstants, cameraForward) == 384U);
static_assert(offsetof(LabConstants, receiverDepthBias) == 396U);
static_assert(offsetof(LabConstants, directionToLight) == 400U);
static_assert(offsetof(LabConstants, normalOffsetWorld) == 412U);
static_assert(offsetof(LabConstants, lightColor) == 416U);
static_assert(offsetof(LabConstants, lightIntensity) == 428U);
static_assert(offsetof(LabConstants, cascadeCount) == 432U);
static_assert(offsetof(LabConstants, debugView) == 436U);
static_assert(offsetof(LabConstants, blendEnabled) == 440U);
static_assert(offsetof(LabConstants, shadowsEnabled) == 444U);

// Depth-pass constants: one cascade's light view-projection per draw. Matches the
// ShadowPassConstants cbuffer in the HLSL.
struct ShadowPassConstants final
{
    DirectX::XMFLOAT4X4 lightViewProjection{};
};

static_assert(sizeof(ShadowPassConstants) == 64U, "ShadowPassConstants must match the HLSL cbuffer footprint.");

// Interleaved scene vertex: POSITION (12) + NORMAL (12). Geometry is baked into
// world space so both the depth and lighting passes draw it with no per-object
// transform, which keeps the frame deterministic and the draw loop simple.
struct MeshVertex final
{
    float positionX{};
    float positionY{};
    float positionZ{};
    float normalX{};
    float normalY{};
    float normalZ{};
};

static_assert(sizeof(MeshVertex) == 24U, "MeshVertex must match the scene input layout stride.");

struct MeshGeometry final
{
    std::vector<MeshVertex> vertices{};
    std::vector<std::uint32_t> indices{};
};

// The runnable configuration a test or the interactive UI applies. The cascade
// math parameters live in the contract CascadeConfig; the booleans below let the
// UI and tests flip stabilization, blending, and shadows without rebuilding it.
struct LabConfiguration final
{
    CascadeConfig cascadeConfig{};
    DebugView debugView{DebugView::Shaded};
    bool stabilizationEnabled{true};
    bool blendEnabled{true};
    bool shadowsEnabled{true};
    float exposure{0.0F};

    // Forces a specific cascade count regardless of cascadeConfig.cascadeCount.
    // The Starter uses 1 to render a single directional shadow map; the Solution
    // uses it in the equivalence test. Zero keeps cascadeConfig.cascadeCount.
    std::uint32_t cascadeCountOverride{0U};

    // World-space camera translation applied before the cascade fit. The
    // stabilization test nudges the camera by sub-texel and super-texel amounts
    // along the light's right axis to show texel snapping absorbs the former.
    Float3 cameraWorldOffset{0.0F, 0.0F, 0.0F};
};

// The canonical deterministic configuration shared by the smoke runs and tests.
[[nodiscard]] LabConfiguration BaselineConfiguration() noexcept;

// The deterministic left-handed camera framing the occluder row, plus the values
// the cascade contract needs (forward/up/fov/aspect). The world offset shifts the
// camera position only, leaving its orientation fixed.
struct CameraFrame final
{
    DirectX::XMFLOAT4X4 viewProjection{};
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 forward{};
    DirectX::XMFLOAT3 up{};
    float verticalFovRadians{};
    float aspectRatio{};
    float nearPlane{};
    float farPlane{};
};

[[nodiscard]] CameraFrame ComputeCamera(lgp::framework::Extent2D size, Float3 worldOffset) noexcept;

// Deterministic analytic scene: a checkerboard ground plane and a row of box
// occluders at increasing depth so shadows fall across every cascade.
[[nodiscard]] MeshGeometry BuildScene();

// The packed frame data: the lighting constants and the per-cascade light
// view-projection matrices, ready to feed the depth passes and the lighting pass.
struct FrameData final
{
    LabConstants lab{};
    std::uint32_t cascadeCount{};
    std::array<ShadowPassConstants, kMaxCascades> shadowPasses{};

    // Per-cascade world-units-per-texel from the CPU contract. The stabilization
    // test uses cascade 0's value to size its sub-texel and super-texel offsets.
    std::array<Float2, kMaxCascades> worldUnitsPerTexel{};
};

// Drives the CPU cascade contract (BuildCascadedShadowSetup) for the supplied
// configuration and camera, then packs the result into GPU constants. Fails with
// the contract's typed error if the configuration is invalid.
[[nodiscard]] std::expected<FrameData, CascadeError> BuildFrameData(LabConfiguration const &configuration,
                                                                    CameraFrame const &camera) noexcept;

// Full-precision readback of one array slice of a depth resource. The stabilization
// and slice-behavior tests compare these bytes directly.
struct DepthReadback final
{
    lgp::framework::Extent2D size{};
    std::uint32_t rowPitch{};
    std::vector<std::byte> pixels{};

    [[nodiscard]] float Depth(std::uint32_t x, std::uint32_t y) const noexcept;
};

// Copies one array slice (mip 0) of a shadow resource into a readback buffer. The
// source is transitioned from SHADER_RESOURCE to COPY_SOURCE and back with
// enhanced barriers, matching the repo convention.
[[nodiscard]] std::expected<DepthReadback, lgp::framework::Error> ReadBackDepthSlice(
    lgp::framework::DeviceResources &deviceResources, ID3D12Resource &source, lgp::framework::Extent2D size,
    std::uint32_t arraySlice, std::uint32_t arraySize);

// Enhanced-barrier states shared by both renderers.
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState ShaderResourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState DepthWriteState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept;

// Compiles one entry point of a chapter HLSL file with the framework's DXC wrapper
// (row-major matrices, warnings as errors, HLSL 2021).
[[nodiscard]] lgp::framework::Status CompileShaderEntry(lgp::framework::ShaderCompiler const &compiler,
                                                        lgp::framework::ShaderCompileOptions options,
                                                        wchar_t const *entryPoint, wchar_t const *targetProfile,
                                                        lgp::framework::CompiledShader &shader);

} // namespace ch24::cascaded_shadows::gpu
