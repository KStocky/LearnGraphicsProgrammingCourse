#include "GpuLabSupport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace ch23::material_layering::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr float kPi = 3.14159265358979323846F;

[[nodiscard]] DirectX::XMFLOAT3 NormalizedOrDefault(Float3 value, DirectX::XMFLOAT3 fallback) noexcept
{
    float const lengthSquared = (value.x * value.x) + (value.y * value.y) + (value.z * value.z);
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12F)
    {
        return fallback;
    }
    float const inverseLength = 1.0F / std::sqrt(lengthSquared);
    return {value.x * inverseLength, value.y * inverseLength, value.z * inverseLength};
}

[[nodiscard]] UnitVector3 ToUnit(DirectX::XMFLOAT3 value) noexcept
{
    return {value.x, value.y, value.z};
}

} // namespace

LayeredMaterial BaselineMaterial() noexcept
{
    // A moderately glossy copper: a full metal so the base specular lobe dominates
    // and the isotropic highlight is obvious. No coat, emission, or anisotropy.
    LayeredMaterial material{};
    material.baseColor = {0.95F, 0.64F, 0.54F};
    material.metallic = 1.0F;
    material.perceptualRoughness = 0.35F;
    material.anisotropy = 0.0F;
    material.baseDielectricF0 = {0.04F, 0.04F, 0.04F};
    material.clearcoatWeight = 0.0F;
    material.clearcoatRoughness = 0.05F;
    material.emissiveColor = {0.0F, 0.0F, 0.0F};
    material.emissiveIntensity = 0.0F;
    return material;
}

LayeredMaterial AnisotropicMaterial() noexcept
{
    LayeredMaterial material = BaselineMaterial();
    material.anisotropy = 0.85F;
    return material;
}

LayeredMaterial ClearcoatMaterial() noexcept
{
    // A rough red dielectric under a sharp dielectric coat, so the coat reflection
    // is clearly separate from the diffuse base beneath it.
    LayeredMaterial material{};
    material.baseColor = {0.55F, 0.06F, 0.05F};
    material.metallic = 0.0F;
    material.perceptualRoughness = 0.6F;
    material.anisotropy = 0.0F;
    material.baseDielectricF0 = {0.04F, 0.04F, 0.04F};
    material.clearcoatWeight = 1.0F;
    material.clearcoatRoughness = 0.05F;
    material.emissiveColor = {0.0F, 0.0F, 0.0F};
    material.emissiveIntensity = 0.0F;
    return material;
}

LayeredMaterial EmissiveMaterial() noexcept
{
    // A dark base with an additive blue emitter, so emission is unmistakably a
    // separate additive term rather than reflectance.
    LayeredMaterial material{};
    material.baseColor = {0.05F, 0.05F, 0.06F};
    material.metallic = 0.0F;
    material.perceptualRoughness = 0.5F;
    material.anisotropy = 0.0F;
    material.baseDielectricF0 = {0.04F, 0.04F, 0.04F};
    material.clearcoatWeight = 0.0F;
    material.clearcoatRoughness = 0.05F;
    material.emissiveColor = {0.10F, 0.42F, 0.92F};
    material.emissiveIntensity = 3.0F;
    return material;
}

LabConfiguration BaselineConfiguration() noexcept
{
    LabConfiguration configuration{};
    configuration.preset = ScenePreset::Baseline;
    configuration.outputView = OutputView::Final;
    configuration.geometry = SceneGeometry::Sphere;
    configuration.material = BaselineMaterial();
    configuration.directionToLight = {-0.4F, 0.7F, -0.6F};
    configuration.lightIntensity = 3.0F;
    configuration.lightColor = {1.0F, 1.0F, 1.0F};
    configuration.exposure = 0.0F;
    return configuration;
}

LabConfiguration ConfigurationForPreset(ScenePreset preset) noexcept
{
    LabConfiguration configuration = BaselineConfiguration();
    configuration.preset = preset;
    switch (preset)
    {
    case ScenePreset::Baseline:
        configuration.material = BaselineMaterial();
        break;
    case ScenePreset::Anisotropic:
        configuration.material = AnisotropicMaterial();
        break;
    case ScenePreset::Clearcoat:
        configuration.material = ClearcoatMaterial();
        break;
    case ScenePreset::Emissive:
        configuration.material = EmissiveMaterial();
        break;
    }
    return configuration;
}

std::expected<MeshGeometry, MaterialError> GenerateSphere(float radius, std::uint32_t latitudeSegments,
                                                          std::uint32_t longitudeSegments)
{
    if (!std::isfinite(radius) || radius <= 0.0F)
    {
        return std::unexpected(MaterialError::NonFiniteInput);
    }
    if (latitudeSegments < 2U || longitudeSegments < 3U)
    {
        return std::unexpected(MaterialError::InvalidQuadrature);
    }

    auto const ringVertex = [longitudeSegments](std::uint32_t ring, std::uint32_t longitude) -> std::uint32_t
    { return 1U + (ring * longitudeSegments) + longitude; };

    MeshGeometry mesh{};

    auto const pushVertex = [&mesh](Float3 normal, float scale, Float3 tangent)
    {
        MeshVertex vertex{};
        vertex.positionX = normal.x * scale;
        vertex.positionY = normal.y * scale;
        vertex.positionZ = normal.z * scale;
        vertex.normalX = normal.x;
        vertex.normalY = normal.y;
        vertex.normalZ = normal.z;
        vertex.tangentX = tangent.x;
        vertex.tangentY = tangent.y;
        vertex.tangentZ = tangent.z;
        vertex.tangentW = 1.0F;
        mesh.vertices.push_back(vertex);
    };

    pushVertex({0.0F, 1.0F, 0.0F}, radius, {1.0F, 0.0F, 0.0F});

    for (std::uint32_t latitude = 1U; latitude < latitudeSegments; ++latitude)
    {
        float const theta = kPi * static_cast<float>(latitude) / static_cast<float>(latitudeSegments);
        float const sinTheta = std::sin(theta);
        float const cosTheta = std::cos(theta);
        for (std::uint32_t longitude = 0U; longitude < longitudeSegments; ++longitude)
        {
            float const phi = 2.0F * kPi * static_cast<float>(longitude) / static_cast<float>(longitudeSegments);
            float const cosPhi = std::cos(phi);
            float const sinPhi = std::sin(phi);
            Float3 const normal{sinTheta * cosPhi, cosTheta, sinTheta * sinPhi};
            // Tangent along increasing longitude, so anisotropy stretches the
            // highlight around the sphere consistently.
            Float3 const tangent{-sinPhi, 0.0F, cosPhi};
            pushVertex(normal, radius, tangent);
        }
    }

    std::uint32_t const bottomVertex = static_cast<std::uint32_t>(mesh.vertices.size());
    pushVertex({0.0F, -1.0F, 0.0F}, radius, {1.0F, 0.0F, 0.0F});

    for (std::uint32_t longitude = 0U; longitude < longitudeSegments; ++longitude)
    {
        std::uint32_t const nextLongitude = (longitude + 1U) % longitudeSegments;
        mesh.indices.push_back(0U);
        mesh.indices.push_back(ringVertex(0U, nextLongitude));
        mesh.indices.push_back(ringVertex(0U, longitude));
    }

    for (std::uint32_t ring = 0U; ring + 1U < latitudeSegments - 1U; ++ring)
    {
        for (std::uint32_t longitude = 0U; longitude < longitudeSegments; ++longitude)
        {
            std::uint32_t const nextLongitude = (longitude + 1U) % longitudeSegments;
            std::uint32_t const upper = ringVertex(ring, longitude);
            std::uint32_t const upperNext = ringVertex(ring, nextLongitude);
            std::uint32_t const lower = ringVertex(ring + 1U, longitude);
            std::uint32_t const lowerNext = ringVertex(ring + 1U, nextLongitude);
            mesh.indices.insert(mesh.indices.end(), {upper, upperNext, lower, upperNext, lowerNext, lower});
        }
    }

    std::uint32_t const lastRing = latitudeSegments - 2U;
    for (std::uint32_t longitude = 0U; longitude < longitudeSegments; ++longitude)
    {
        std::uint32_t const nextLongitude = (longitude + 1U) % longitudeSegments;
        mesh.indices.push_back(ringVertex(lastRing, longitude));
        mesh.indices.push_back(ringVertex(lastRing, nextLongitude));
        mesh.indices.push_back(bottomVertex);
    }

    return mesh;
}

CameraMatrices ComputeSphereCamera(lgp::framework::Extent2D size, float azimuth, float elevation, float radius) noexcept
{
    float const horizontalRadius = radius * std::cos(elevation);
    DirectX::XMFLOAT3 const eyePosition{
        horizontalRadius * std::sin(azimuth),
        radius * std::sin(elevation),
        -horizontalRadius * std::cos(azimuth),
    };
    DirectX::XMVECTOR const eye = DirectX::XMLoadFloat3(&eyePosition);
    DirectX::XMVECTOR const target = DirectX::XMVectorSet(0.0F, 0.0F, 0.0F, 1.0F);
    DirectX::XMVECTOR const up = DirectX::XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F);
    DirectX::XMMATRIX const view = DirectX::XMMatrixLookAtLH(eye, target, up);
    float const aspect = size.height == 0U ? 1.0F : static_cast<float>(size.width) / static_cast<float>(size.height);
    DirectX::XMMATRIX const projection = DirectX::XMMatrixPerspectiveFovLH(0.72F, aspect, 0.1F, 50.0F);

    CameraMatrices camera{};
    DirectX::XMStoreFloat4x4(&camera.viewProjection, view * projection);
    camera.position = eyePosition;
    return camera;
}

LabConstants MakeLabConstants(LabConfiguration const &configuration, CameraMatrices const &camera) noexcept
{
    LabConstants constants{};
    constants.viewProjection = camera.viewProjection;
    constants.cameraPosition = camera.position;
    constants.sceneGeometry = static_cast<std::uint32_t>(configuration.geometry);
    constants.directionToLight = NormalizedOrDefault(configuration.directionToLight, {-0.4F, 0.7F, -0.6F});
    constants.lightIntensity = configuration.lightIntensity;
    constants.lightColor = {configuration.lightColor.r, configuration.lightColor.g, configuration.lightColor.b};
    constants.outputView = static_cast<std::uint32_t>(configuration.outputView);

    LayeredMaterial const &material = configuration.material;
    constants.baseColor = {material.baseColor.r, material.baseColor.g, material.baseColor.b};
    constants.metallic = material.metallic;
    constants.baseDielectricF0 = {material.baseDielectricF0.r, material.baseDielectricF0.g,
                                  material.baseDielectricF0.b};
    constants.perceptualRoughness = material.perceptualRoughness;
    constants.emissiveColor = {material.emissiveColor.r, material.emissiveColor.g, material.emissiveColor.b};
    constants.anisotropy = material.anisotropy;
    constants.clearcoatWeight = material.clearcoatWeight;
    constants.clearcoatRoughness = material.clearcoatRoughness;
    constants.emissiveIntensity = material.emissiveIntensity;
    constants.preset = static_cast<std::uint32_t>(configuration.preset);

    constants.probeNormal = NormalizedOrDefault(configuration.probeNormal, {0.0F, 0.0F, 1.0F});
    constants.probeTangent = NormalizedOrDefault(configuration.probeTangent, {1.0F, 0.0F, 0.0F});
    constants.probeViewDirection = NormalizedOrDefault(configuration.probeViewDirection, {0.0F, 0.0F, 1.0F});
    constants.probeLightDirection = NormalizedOrDefault(configuration.probeLightDirection, {0.0F, 0.0F, 1.0F});
    return constants;
}

std::expected<ProbeReference, MaterialError> EvaluateProbeReference(LabConfiguration const &configuration) noexcept
{
    // Use exactly the float-normalized directions the GPU receives so the only
    // CPU/GPU divergence is the shader's float half-vector versus the contract's
    // double intermediate, not a different set of inputs.
    DirectX::XMFLOAT3 const normal = NormalizedOrDefault(configuration.probeNormal, {0.0F, 0.0F, 1.0F});
    DirectX::XMFLOAT3 const tangent = NormalizedOrDefault(configuration.probeTangent, {1.0F, 0.0F, 0.0F});
    DirectX::XMFLOAT3 const viewDirection = NormalizedOrDefault(configuration.probeViewDirection, {0.0F, 0.0F, 1.0F});
    DirectX::XMFLOAT3 const lightDirection = NormalizedOrDefault(configuration.probeLightDirection, {0.0F, 0.0F, 1.0F});

    std::expected<ShadingFrame, MaterialError> const frame =
        MakeShadingFrame({normal.x, normal.y, normal.z}, {tangent.x, tangent.y, tangent.z});
    if (!frame)
    {
        return std::unexpected(frame.error());
    }

    std::expected<LayeredBrdfResult, MaterialError> const result =
        EvaluateLayeredBrdf(configuration.material, *frame, ToUnit(viewDirection), ToUnit(lightDirection));
    if (!result)
    {
        return std::unexpected(result.error());
    }

    float const irradianceScale = configuration.lightIntensity * result->nDotL;
    LinearRgb const irradiance{
        configuration.lightColor.r * irradianceScale,
        configuration.lightColor.g * irradianceScale,
        configuration.lightColor.b * irradianceScale,
    };

    ProbeReference reference{};
    reference.baseRadiance = {
        irradiance.r * result->attenuatedBase.r,
        irradiance.g * result->attenuatedBase.g,
        irradiance.b * result->attenuatedBase.b,
    };
    reference.coatRadiance = {
        irradiance.r * result->coatSpecular.r,
        irradiance.g * result->coatSpecular.g,
        irradiance.b * result->coatSpecular.b,
    };
    reference.emissionRadiance = {
        result->emittedRadiance.r,
        result->emittedRadiance.g,
        result->emittedRadiance.b,
    };
    reference.finalRadiance = {
        reference.baseRadiance.r + reference.coatRadiance.r + reference.emissionRadiance.r,
        reference.baseRadiance.g + reference.coatRadiance.g + reference.emissionRadiance.g,
        reference.baseRadiance.b + reference.coatRadiance.b + reference.emissionRadiance.b,
    };
    reference.coatTransmission = result->coatTransmission;
    reference.nDotL = result->nDotL;
    reference.nDotV = result->nDotV;
    return reference;
}

DirectX::XMFLOAT4 TextureReadback::TexelRgba(std::uint32_t x, std::uint32_t y) const noexcept
{
    std::size_t const offset = (static_cast<std::size_t>(y) * rowPitch) + (static_cast<std::size_t>(x) * 16U);
    if (offset + 16U > pixels.size())
    {
        return {};
    }
    DirectX::XMFLOAT4 texel{};
    std::memcpy(&texel, pixels.data() + offset, sizeof(DirectX::XMFLOAT4));
    return texel;
}

lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
}

lgp::framework::TextureBarrierState ShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
            D3D12_BARRIER_LAYOUT_SHADER_RESOURCE};
}

lgp::framework::TextureBarrierState DepthWriteState() noexcept
{
    return {D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
            D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE};
}

lgp::framework::TextureBarrierState FrameStartState(lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frameContext.renderTargetInitialLayout};
}

lgp::framework::TextureBarrierState FrameEndState(lgp::framework::FrameContext const &frameContext) noexcept
{
    return {
        D3D12_BARRIER_SYNC_NONE,
        D3D12_BARRIER_ACCESS_NO_ACCESS,
        frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT,
    };
}

std::expected<TextureReadback, lgp::framework::Error> ReadBackTexture(lgp::framework::DeviceResources &deviceResources,
                                                                      ID3D12Resource &source,
                                                                      lgp::framework::Extent2D size, DXGI_FORMAT format)
{
    if (auto idle = deviceResources.WaitForGpuIdle(); !idle)
    {
        return std::unexpected(std::move(idle.error()));
    }

    ID3D12Device10 *const device = deviceResources.device();
    D3D12_RESOURCE_DESC const sourceDescription = source.GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0U;
    UINT64 rowSize = 0U;
    UINT64 totalBytes = 0U;
    device->GetCopyableFootprints(&sourceDescription, 0U, 1U, 0U, &footprint, &rowCount, &rowSize, &totalBytes);

    lgp::framework::BufferCreateDesc readbackDescription{};
    readbackDescription.sizeInBytes = totalBytes;
    readbackDescription.heapType = D3D12_HEAP_TYPE_READBACK;
    readbackDescription.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    readbackDescription.name = L"Ch23 material lab readback";
    std::expected<lgp::framework::Buffer, lgp::framework::Error> readbackBuffer =
        lgp::framework::CreateCommittedBuffer(*device, readbackDescription);
    if (!readbackBuffer)
    {
        return std::unexpected(std::move(readbackBuffer.error()));
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    if (HRESULT const result =
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandAllocator", result,
                                                                "Failed to create the readback allocator."));
    }

    ComPtr<ID3D12GraphicsCommandList7> commandList;
    if (HRESULT const result = device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                         IID_PPV_ARGS(commandList.GetAddressOf()));
        FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandList", result,
                                                                "Failed to create the readback command list."));
    }

    lgp::framework::TextureBarrierState constexpr copySource{
        D3D12_BARRIER_SYNC_COPY,
        D3D12_BARRIER_ACCESS_COPY_SOURCE,
        D3D12_BARRIER_LAYOUT_COPY_SOURCE,
    };
    lgp::framework::TransitionTexture(*commandList.Get(), source, ShaderResourceState(), copySource);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readbackBuffer->resource();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource = &source;
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sourceLocation.SubresourceIndex = 0U;
    commandList->CopyTextureRegion(&destination, 0U, 0U, 0U, &sourceLocation, nullptr);

    lgp::framework::TransitionTexture(*commandList.Get(), source, copySource, ShaderResourceState());

    if (HRESULT const result = commandList->Close(); FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12GraphicsCommandList::Close", result,
                                                                "Failed to close the readback command list."));
    }
    ID3D12CommandList *const lists[]{commandList.Get()};
    deviceResources.graphics_queue()->ExecuteCommandLists(1U, lists);
    if (auto idle = deviceResources.WaitForGpuIdle(); !idle)
    {
        return std::unexpected(std::move(idle.error()));
    }

    TextureReadback readback{};
    readback.size = size;
    readback.format = format;
    readback.rowPitch = footprint.Footprint.RowPitch;
    readback.pixels.resize(static_cast<std::size_t>(totalBytes));

    void *mapped = nullptr;
    D3D12_RANGE const readRange{0U, static_cast<SIZE_T>(totalBytes)};
    if (HRESULT const result = readbackBuffer->resource()->Map(0U, &readRange, &mapped); FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Resource::Map", result, "Failed to map the readback buffer."));
    }
    std::memcpy(readback.pixels.data(), mapped, static_cast<std::size_t>(totalBytes));
    D3D12_RANGE const writtenRange{0U, 0U};
    readbackBuffer->resource()->Unmap(0U, &writtenRange);
    return readback;
}

lgp::framework::Status CompileShaderEntry(lgp::framework::ShaderCompiler const &compiler,
                                          lgp::framework::ShaderCompileOptions options, wchar_t const *entryPoint,
                                          wchar_t const *targetProfile, lgp::framework::CompiledShader &shader)
{
    options.entryPoint = entryPoint;
    options.targetProfile = targetProfile;
    options.additionalArguments = {L"-E", entryPoint, L"-T", targetProfile};
    std::expected<lgp::framework::CompiledShader, lgp::framework::Error> result = compiler.Compile(options);
    if (!result)
    {
        return std::unexpected(std::move(result.error()));
    }
    shader = std::move(*result);
    return {};
}

} // namespace ch23::material_layering::gpu
