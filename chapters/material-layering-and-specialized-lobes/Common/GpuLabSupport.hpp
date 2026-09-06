#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "MaterialLayering.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <DirectXMath.h>

#include <lgp/framework/barriers.hpp>
#include <lgp/framework/buffer.hpp>
#include <lgp/framework/device_resources.hpp>
#include <lgp/framework/error.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

// Chapter 23 - Material Layering and Specialized BRDF Lobes (shared GPU support).
//
// This support layer is chapter owned but shared by the Starter and Solution so
// the pedagogy - deterministic scene, HLSL that mirrors the CPU contract, and
// honest CPU/GPU parity probes - stays visible in one place. It deliberately does
// not hide chapter-owned resources, pipelines, barriers, or draws; those remain
// in each renderer. It only removes duplicated scaffolding (constant packing,
// geometry, readback, and the CPU reference used by the tests).

namespace ch23::material_layering::gpu
{

// The runnable lab renders into a full-precision linear HDR target so the WARP
// readback tests can compare radiance against the CPU contract without losing
// precision to tone mapping or an 8-bit swap-chain surface.
inline constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
inline constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

// Material configuration a preset applies. Mirrors PresetXxx in MaterialLab.hlsli.
enum class ScenePreset : std::uint32_t
{
    Baseline = 0U,    // Isotropic metal/dielectric base only; identical to the Starter.
    Anisotropic = 1U, // Anisotropic base lobe with a meaningful tangent orientation.
    Clearcoat = 2U,   // Dielectric clearcoat over a rougher base.
    Emissive = 3U,    // Additive emission layered on the base.
};

// Diagnostic contribution view. Mirrors OutputViewXxx in MaterialLab.hlsli.
enum class OutputView : std::uint32_t
{
    Final = 0U,       // base + coat + emission (before display mapping).
    Base = 1U,        // irradiance * coat-attenuated base BRDF.
    Coat = 2U,        // irradiance * coat reflection lobe.
    Emission = 3U,    // additive emitted radiance only.
    Attenuation = 4U, // coat transmission (energy budget), shown without tone mapping.
};

// Which geometry the frame draws. Mirrors SceneGeometryXxx in MaterialLab.hlsli.
enum class SceneGeometry : std::uint32_t
{
    Sphere = 0U,    // Tangent-framed material sphere for the visual scene and smoke runs.
    ProbeCard = 1U, // Screen-filling analytic surface for CPU/GPU parity and anisotropy probes.
};

// Per-frame lighting/material constants. Field order and 16-byte packing must
// match the LabConstants cbuffer in MaterialLab.hlsli exactly; the static_asserts
// below guard every offset so the CPU and HLSL layouts never drift.
struct LabConstants final
{
    DirectX::XMFLOAT4X4 viewProjection{};

    DirectX::XMFLOAT3 cameraPosition{};
    std::uint32_t sceneGeometry{};

    DirectX::XMFLOAT3 directionToLight{};
    float lightIntensity{};

    DirectX::XMFLOAT3 lightColor{1.0F, 1.0F, 1.0F};
    std::uint32_t outputView{};

    DirectX::XMFLOAT3 baseColor{};
    float metallic{};

    DirectX::XMFLOAT3 baseDielectricF0{0.04F, 0.04F, 0.04F};
    float perceptualRoughness{};

    DirectX::XMFLOAT3 emissiveColor{};
    float anisotropy{};

    float clearcoatWeight{};
    float clearcoatRoughness{};
    float emissiveIntensity{};
    std::uint32_t preset{};

    DirectX::XMFLOAT3 probeNormal{0.0F, 0.0F, 1.0F};
    float probePad0{};

    DirectX::XMFLOAT3 probeTangent{1.0F, 0.0F, 0.0F};
    float probePad1{};

    DirectX::XMFLOAT3 probeViewDirection{0.0F, 0.0F, 1.0F};
    float probePad2{};

    DirectX::XMFLOAT3 probeLightDirection{0.0F, 0.0F, 1.0F};
    float probePad3{};
};

static_assert(sizeof(LabConstants) == 240U, "LabConstants must match the HLSL cbuffer footprint.");
static_assert(offsetof(LabConstants, viewProjection) == 0U);
static_assert(offsetof(LabConstants, cameraPosition) == 64U);
static_assert(offsetof(LabConstants, sceneGeometry) == 76U);
static_assert(offsetof(LabConstants, directionToLight) == 80U);
static_assert(offsetof(LabConstants, lightIntensity) == 92U);
static_assert(offsetof(LabConstants, lightColor) == 96U);
static_assert(offsetof(LabConstants, outputView) == 108U);
static_assert(offsetof(LabConstants, baseColor) == 112U);
static_assert(offsetof(LabConstants, metallic) == 124U);
static_assert(offsetof(LabConstants, baseDielectricF0) == 128U);
static_assert(offsetof(LabConstants, perceptualRoughness) == 140U);
static_assert(offsetof(LabConstants, emissiveColor) == 144U);
static_assert(offsetof(LabConstants, anisotropy) == 156U);
static_assert(offsetof(LabConstants, clearcoatWeight) == 160U);
static_assert(offsetof(LabConstants, clearcoatRoughness) == 164U);
static_assert(offsetof(LabConstants, emissiveIntensity) == 168U);
static_assert(offsetof(LabConstants, preset) == 172U);
static_assert(offsetof(LabConstants, probeNormal) == 176U);
static_assert(offsetof(LabConstants, probeTangent) == 192U);
static_assert(offsetof(LabConstants, probeViewDirection) == 208U);
static_assert(offsetof(LabConstants, probeLightDirection) == 224U);

// Display-pass constants. Field order and packing match the DisplayConstants
// cbuffer in MaterialLab.hlsli.
struct DisplayConstants final
{
    float exposure{};
    std::uint32_t outputView{};
    DirectX::XMFLOAT2 displayPad{};
};

static_assert(sizeof(DisplayConstants) == 16U, "DisplayConstants must match the HLSL cbuffer footprint.");
static_assert(offsetof(DisplayConstants, exposure) == 0U);
static_assert(offsetof(DisplayConstants, outputView) == 4U);

// Interleaved vertex consumed by the sphere input layout: POSITION (12) + NORMAL
// (12) + TANGENT (16, with canonical w = +1). The tangent lets the anisotropic
// highlight wrap the sphere consistently so the orientation is visually meaningful.
struct MeshVertex final
{
    float positionX{};
    float positionY{};
    float positionZ{};
    float normalX{};
    float normalY{};
    float normalZ{};
    float tangentX{};
    float tangentY{};
    float tangentZ{};
    float tangentW{1.0F};
};

static_assert(sizeof(MeshVertex) == 40U, "MeshVertex must match the sphere input layout stride.");

struct MeshGeometry final
{
    std::vector<MeshVertex> vertices{};
    std::vector<std::uint32_t> indices{};
};

// The runnable configuration a test or the interactive UI applies. It carries the
// contract material verbatim so the CPU reference probe can evaluate the exact
// same LayeredMaterial the GPU shades.
struct LabConfiguration final
{
    ScenePreset preset{ScenePreset::Baseline};
    OutputView outputView{OutputView::Final};
    SceneGeometry geometry{SceneGeometry::Sphere};

    LayeredMaterial material{};

    Float3 directionToLight{-0.4F, 0.7F, -0.6F};
    float lightIntensity{3.0F};
    LinearRgb lightColor{1.0F, 1.0F, 1.0F};
    float exposure{0.0F};

    // Probe-card surface. Used only when geometry == ProbeCard.
    Float3 probeNormal{0.0F, 0.0F, 1.0F};
    Float3 probeTangent{1.0F, 0.0F, 0.0F};
    Float3 probeViewDirection{0.3F, 0.1F, 0.9F};
    Float3 probeLightDirection{-0.2F, 0.4F, 0.8F};
};

// The canonical deterministic material the Starter and the Solution's Baseline
// preset both render. Keeping it in one place is what lets the byte-identical
// baseline test configure both apps identically.
[[nodiscard]] LayeredMaterial BaselineMaterial() noexcept;

// Preset material factories used by the interactive UI and the tests.
[[nodiscard]] LayeredMaterial AnisotropicMaterial() noexcept;
[[nodiscard]] LayeredMaterial ClearcoatMaterial() noexcept;
[[nodiscard]] LayeredMaterial EmissiveMaterial() noexcept;

// The headless baseline configuration shared by the Starter and Solution smoke
// runs and the byte-identical equivalence test.
[[nodiscard]] LabConfiguration BaselineConfiguration() noexcept;

// Applies a preset's material to a configuration, leaving light/camera untouched.
[[nodiscard]] LabConfiguration ConfigurationForPreset(ScenePreset preset) noexcept;

// Deterministic tangent-framed UV sphere centered at the origin.
[[nodiscard]] std::expected<MeshGeometry, MaterialError> GenerateSphere(float radius, std::uint32_t latitudeSegments,
                                                                        std::uint32_t longitudeSegments);

struct CameraMatrices final
{
    DirectX::XMFLOAT4X4 viewProjection{};
    DirectX::XMFLOAT3 position{};
};

// Left-handed orbit camera framing the unit sphere. Headless callers pass fixed
// orbit parameters so the camera stays deterministic.
[[nodiscard]] CameraMatrices ComputeSphereCamera(lgp::framework::Extent2D size, float azimuth, float elevation,
                                                 float radius) noexcept;

// Packs a configuration and camera into the GPU constant layout.
[[nodiscard]] LabConstants MakeLabConstants(LabConfiguration const &configuration,
                                            CameraMatrices const &camera) noexcept;

// The radiance breakdown the shader produces for a probe surface, evaluated on
// the CPU through the MaterialLayering contract. baseRadiance and coatRadiance
// already include the incident irradiance so their sum with emission matches the
// shader's Final view.
struct ProbeReference final
{
    LinearRgb finalRadiance{};
    LinearRgb baseRadiance{};
    LinearRgb coatRadiance{};
    LinearRgb emissionRadiance{};
    float coatTransmission{};
    float nDotL{};
    float nDotV{};
};

// Evaluates the CPU reference for a probe configuration. Fails with the contract's
// typed error if the material, frame, or directions are invalid.
[[nodiscard]] std::expected<ProbeReference, MaterialError> EvaluateProbeReference(
    LabConfiguration const &configuration) noexcept;

// Full-precision linear readback of a 2D texture subresource.
struct TextureReadback final
{
    lgp::framework::Extent2D size{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    std::uint32_t rowPitch{};
    std::vector<std::byte> pixels{};

    // Returns the RGBA float texel at (x, y) for R32G32B32A32_FLOAT readbacks.
    [[nodiscard]] DirectX::XMFLOAT4 TexelRgba(std::uint32_t x, std::uint32_t y) const noexcept;
};

// Copies a shader-resource-layout texture into a readback buffer and returns its
// linear contents. The source is transitioned to COPY_SOURCE and back to
// SHADER_RESOURCE with enhanced barriers.
[[nodiscard]] std::expected<TextureReadback, lgp::framework::Error> ReadBackTexture(
    lgp::framework::DeviceResources &deviceResources, ID3D12Resource &source, lgp::framework::Extent2D size,
    DXGI_FORMAT format);

// Enhanced-barrier states shared by both renderers.
[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState ShaderResourceState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState DepthWriteState() noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept;
[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept;

// Compiles one entry point of a chapter HLSL file with the framework's DXC
// wrapper (row-major matrices, warnings as errors, HLSL 2021).
[[nodiscard]] lgp::framework::Status CompileShaderEntry(lgp::framework::ShaderCompiler const &compiler,
                                                        lgp::framework::ShaderCompileOptions options,
                                                        wchar_t const *entryPoint, wchar_t const *targetProfile,
                                                        lgp::framework::CompiledShader &shader);

} // namespace ch23::material_layering::gpu
