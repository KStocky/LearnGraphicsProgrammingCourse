#include "CascadedShadowLab.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace ch24::cascaded_shadows::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

// Fixed light and camera framing so every headless run is deterministic.
inline constexpr Float3 kBaseCameraPosition{0.0F, 14.0F, -14.0F};
inline constexpr Float3 kCameraTarget{0.0F, 0.0F, 45.0F};
inline constexpr float kCameraFov = 1.0F;
inline constexpr float kCameraNear = 0.5F;
inline constexpr float kCameraFar = 140.0F;

// Base shadow bias in NDC depth / world units. Per-cascade scaling multiplies
// these by the cascade's texel-size ratio (ScaleBiasForCascade in the contract).
inline constexpr float kReceiverDepthBias = 0.0016F;
inline constexpr float kNormalOffsetWorld = 0.08F;

[[nodiscard]] DirectX::XMFLOAT4X4 ToXmMatrix(Matrix4 const &matrix) noexcept
{
    DirectX::XMFLOAT4X4 result{};
    for (std::size_t row = 0U; row < 4U; ++row)
    {
        for (std::size_t column = 0U; column < 4U; ++column)
        {
            result.m[row][column] = matrix.elements.at(row).at(column);
        }
    }
    return result;
}

[[nodiscard]] Float3 Normalized(Float3 value, Float3 fallback) noexcept
{
    float const lengthSquared = (value.x * value.x) + (value.y * value.y) + (value.z * value.z);
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12F)
    {
        return fallback;
    }
    float const inverseLength = 1.0F / std::sqrt(lengthSquared);
    return {value.x * inverseLength, value.y * inverseLength, value.z * inverseLength};
}

void AppendBox(MeshGeometry &mesh, Float3 center, Float3 halfExtents) noexcept
{
    struct FaceDef final
    {
        Float3 normal;
        Float3 uAxis;
        Float3 vAxis;
    };
    // Six faces, each spanned by two in-plane axes; normal = uAxis x vAxis order
    // chosen for outward-facing, clockwise (front-face) winding under D3D's
    // left-handed rasterizer.
    std::array<FaceDef, 6U> const faces{{
        {{0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},  // -Z
        {{0.0F, 0.0F, 1.0F}, {-1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},  // +Z
        {{-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}}, // -X
        {{1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}},   // +X
        {{0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}}, // -Y
        {{0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},   // +Y
    }};

    for (FaceDef const &face : faces)
    {
        auto const corner = [&](float u, float v) -> MeshVertex
        {
            Float3 const position{
                center.x + (face.normal.x * halfExtents.x) + (face.uAxis.x * u * halfExtents.x) +
                    (face.vAxis.x * v * halfExtents.x),
                center.y + (face.normal.y * halfExtents.y) + (face.uAxis.y * u * halfExtents.y) +
                    (face.vAxis.y * v * halfExtents.y),
                center.z + (face.normal.z * halfExtents.z) + (face.uAxis.z * u * halfExtents.z) +
                    (face.vAxis.z * v * halfExtents.z),
            };
            return MeshVertex{position.x, position.y, position.z, face.normal.x, face.normal.y, face.normal.z};
        };

        std::uint32_t const base = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(corner(-1.0F, -1.0F));
        mesh.vertices.push_back(corner(1.0F, -1.0F));
        mesh.vertices.push_back(corner(1.0F, 1.0F));
        mesh.vertices.push_back(corner(-1.0F, 1.0F));
        mesh.indices.insert(mesh.indices.end(), {base, base + 1U, base + 2U, base, base + 2U, base + 3U});
    }
}

} // namespace

LabConfiguration BaselineConfiguration() noexcept
{
    LabConfiguration configuration{};
    CascadeConfig &config = configuration.cascadeConfig;
    config.nearPlane = kCameraNear;
    config.farPlane = kCameraFar;
    config.verticalFovRadians = kCameraFov;
    config.aspectRatio = 16.0F / 9.0F;
    config.directionToLight = Float3{0.45F, 1.0F, 0.35F};
    config.cascadeCount = kMaxCascades;
    config.shadowResolution = kShadowResolution;
    config.splitLambda = 0.75F;
    config.blendFraction = 0.12F;
    config.casterDepthPadding = 2.0F;
    config.receiverDepthPadding = 2.0F;
    config.stabilization = StabilizationMode::TexelSnappedSphere;

    configuration.debugView = DebugView::Shaded;
    configuration.stabilizationEnabled = true;
    configuration.blendEnabled = true;
    configuration.shadowsEnabled = true;
    configuration.exposure = 0.0F;
    configuration.cascadeCountOverride = 0U;
    configuration.cameraWorldOffset = Float3{0.0F, 0.0F, 0.0F};
    return configuration;
}

CameraFrame ComputeCamera(lgp::framework::Extent2D size, Float3 worldOffset) noexcept
{
    // The camera orientation is fixed (forward derives from the un-offset base
    // position) so a world offset is a pure translation - exactly what the
    // stabilization test needs to nudge the frustum without rotating it.
    Float3 const forward =
        Normalized(Float3{kCameraTarget.x - kBaseCameraPosition.x, kCameraTarget.y - kBaseCameraPosition.y,
                          kCameraTarget.z - kBaseCameraPosition.z},
                   Float3{0.0F, 0.0F, 1.0F});
    Float3 const position{kBaseCameraPosition.x + worldOffset.x, kBaseCameraPosition.y + worldOffset.y,
                          kBaseCameraPosition.z + worldOffset.z};

    float const aspect =
        size.height == 0U ? (16.0F / 9.0F) : static_cast<float>(size.width) / static_cast<float>(size.height);

    DirectX::XMVECTOR const eye = DirectX::XMVectorSet(position.x, position.y, position.z, 1.0F);
    DirectX::XMVECTOR const forwardVector = DirectX::XMVectorSet(forward.x, forward.y, forward.z, 0.0F);
    DirectX::XMVECTOR const upVector = DirectX::XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F);
    DirectX::XMMATRIX const view = DirectX::XMMatrixLookToLH(eye, forwardVector, upVector);
    DirectX::XMMATRIX const projection = DirectX::XMMatrixPerspectiveFovLH(kCameraFov, aspect, kCameraNear, kCameraFar);

    CameraFrame frame{};
    DirectX::XMStoreFloat4x4(&frame.viewProjection, view * projection);
    frame.position = {position.x, position.y, position.z};
    frame.forward = {forward.x, forward.y, forward.z};
    frame.up = {0.0F, 1.0F, 0.0F};
    frame.verticalFovRadians = kCameraFov;
    frame.aspectRatio = aspect;
    frame.nearPlane = kCameraNear;
    frame.farPlane = kCameraFar;
    return frame;
}

MeshGeometry BuildScene()
{
    MeshGeometry mesh{};

    // Ground plane in world space; the checkerboard albedo is evaluated per pixel
    // in the shader from world position, so a single large quad is enough.
    float const groundMinX = -40.0F;
    float const groundMaxX = 40.0F;
    float const groundMinZ = -20.0F;
    float const groundMaxZ = 130.0F;
    std::uint32_t const groundBase = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({groundMinX, 0.0F, groundMinZ, 0.0F, 1.0F, 0.0F});
    mesh.vertices.push_back({groundMaxX, 0.0F, groundMinZ, 0.0F, 1.0F, 0.0F});
    mesh.vertices.push_back({groundMaxX, 0.0F, groundMaxZ, 0.0F, 1.0F, 0.0F});
    mesh.vertices.push_back({groundMinX, 0.0F, groundMaxZ, 0.0F, 1.0F, 0.0F});
    mesh.indices.insert(mesh.indices.end(),
                        {groundBase, groundBase + 2U, groundBase + 1U, groundBase, groundBase + 3U, groundBase + 2U});

    // A row of occluders marching away from the camera so each cascade contains
    // shadow casters at its own scale.
    AppendBox(mesh, {0.0F, 3.0F, 8.0F}, {2.0F, 3.0F, 2.0F});
    AppendBox(mesh, {-6.0F, 2.0F, 24.0F}, {2.0F, 2.0F, 2.0F});
    AppendBox(mesh, {7.0F, 4.0F, 48.0F}, {3.0F, 4.0F, 3.0F});
    AppendBox(mesh, {-5.0F, 3.5F, 82.0F}, {2.5F, 3.5F, 2.5F});
    AppendBox(mesh, {4.0F, 5.0F, 112.0F}, {3.0F, 5.0F, 3.0F});
    return mesh;
}

std::expected<FrameData, CascadeError> BuildFrameData(LabConfiguration const &configuration,
                                                      CameraFrame const &camera) noexcept
{
    CascadeConfig config = configuration.cascadeConfig;
    if (configuration.cascadeCountOverride != 0U)
    {
        config.cascadeCount = configuration.cascadeCountOverride;
    }
    config.stabilization =
        configuration.stabilizationEnabled ? StabilizationMode::TexelSnappedSphere : StabilizationMode::None;
    config.blendFraction = configuration.blendEnabled ? configuration.cascadeConfig.blendFraction : 0.0F;
    config.verticalFovRadians = camera.verticalFovRadians;
    config.aspectRatio = camera.aspectRatio;
    config.nearPlane = camera.nearPlane;
    config.farPlane = camera.farPlane;

    CascadeScene scene{};
    scene.cameraPosition = {camera.position.x, camera.position.y, camera.position.z};
    scene.cameraForward = {camera.forward.x, camera.forward.y, camera.forward.z};
    scene.cameraUp = {camera.up.x, camera.up.y, camera.up.z};
    scene.config = config;
    scene.lightUpHint = {0.0F, 1.0F, 0.0F};
    scene.splitScheme = SplitScheme::PracticalBlend;

    std::expected<CascadeSetup, CascadeError> const setup = BuildCascadedShadowSetup(scene);
    if (!setup)
    {
        return std::unexpected(setup.error());
    }

    std::uint32_t const count = setup->cascadeCount;
    float const referenceTexel = std::max(setup->cascades.at(0U).diagnostics.worldUnitsPerTexel.x,
                                          setup->cascades.at(0U).diagnostics.worldUnitsPerTexel.y);

    FrameData frame{};
    frame.cascadeCount = count;
    LabConstants &lab = frame.lab;
    lab.cameraViewProjection = camera.viewProjection;

    std::array<float, 4U> splitFar{};
    std::array<float, 4U> blendStart{};
    std::array<float, 4U> biasScale{};
    for (std::uint32_t index = 0U; index < kMaxCascades; ++index)
    {
        std::uint32_t const source = index < count ? index : count - 1U;
        CascadeData const &cascade = setup->cascades.at(source);
        DirectX::XMFLOAT4X4 const matrix = ToXmMatrix(cascade.projection.lightViewProjection);
        lab.cascadeViewProjection.at(index) = matrix;
        frame.shadowPasses.at(index).lightViewProjection = matrix;

        splitFar.at(index) = cascade.interval.farDistance;
        blendStart.at(index) = cascade.interval.farDistance - cascade.diagnostics.blendWidth;
        frame.worldUnitsPerTexel.at(index) = cascade.diagnostics.worldUnitsPerTexel;
        float const cascadeTexel =
            std::max(cascade.diagnostics.worldUnitsPerTexel.x, cascade.diagnostics.worldUnitsPerTexel.y);
        biasScale.at(index) = referenceTexel > 0.0F ? (cascadeTexel / referenceTexel) : 1.0F;
    }

    lab.cascadeSplitViewDepth = {splitFar.at(0U), splitFar.at(1U), splitFar.at(2U), splitFar.at(3U)};
    lab.cascadeBlendStartViewDepth = {blendStart.at(0U), blendStart.at(1U), blendStart.at(2U), blendStart.at(3U)};
    lab.cascadeTexelBiasScale = {biasScale.at(0U), biasScale.at(1U), biasScale.at(2U), biasScale.at(3U)};

    lab.cameraPosition = camera.position;
    lab.exposure = configuration.exposure;
    lab.cameraForward = camera.forward;
    lab.receiverDepthBias = kReceiverDepthBias;
    Float3 const lightDirection = Normalized(config.directionToLight, Float3{0.45F, 1.0F, 0.35F});
    lab.directionToLight = {lightDirection.x, lightDirection.y, lightDirection.z};
    lab.normalOffsetWorld = kNormalOffsetWorld;
    lab.lightColor = {1.0F, 1.0F, 1.0F};
    lab.lightIntensity = 3.0F;
    lab.cascadeCount = count;
    lab.debugView = static_cast<std::uint32_t>(configuration.debugView);
    lab.blendEnabled = configuration.blendEnabled ? 1U : 0U;
    lab.shadowsEnabled = configuration.shadowsEnabled ? 1U : 0U;
    return frame;
}

float DepthReadback::Depth(std::uint32_t x, std::uint32_t y) const noexcept
{
    std::size_t const offset = (static_cast<std::size_t>(y) * rowPitch) + (static_cast<std::size_t>(x) * 4U);
    if (offset + 4U > pixels.size())
    {
        return 0.0F;
    }
    float value{};
    std::memcpy(&value, pixels.data() + offset, sizeof(float));
    return value;
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

std::expected<DepthReadback, lgp::framework::Error> ReadBackDepthSlice(lgp::framework::DeviceResources &deviceResources,
                                                                       ID3D12Resource &source,
                                                                       lgp::framework::Extent2D size,
                                                                       std::uint32_t arraySlice,
                                                                       std::uint32_t arraySize)
{
    if (arraySlice >= arraySize)
    {
        return std::unexpected(lgp::framework::MakeError("ReadBackDepthSlice", "Array slice index is out of range."));
    }
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
    device->GetCopyableFootprints(&sourceDescription, arraySlice, 1U, 0U, &footprint, &rowCount, &rowSize, &totalBytes);

    lgp::framework::BufferCreateDesc readbackDescription{};
    readbackDescription.sizeInBytes = totalBytes;
    readbackDescription.heapType = D3D12_HEAP_TYPE_READBACK;
    readbackDescription.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    readbackDescription.name = L"Ch24 shadow slice readback";
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
    sourceLocation.SubresourceIndex = arraySlice;
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

    DepthReadback readback{};
    readback.size = size;
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

} // namespace ch24::cascaded_shadows::gpu
