#include "GpuLabSupport.hpp"

#include <lgp/framework/error.hpp>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace ch33::dxr::gpu
{

using Microsoft::WRL::ComPtr;

namespace
{

// Export names. They are namespace-scope constants rather than locals because every pointer a state-object
// subobject holds has to stay valid until `CreateStateObject` returns, and because the shader table and the
// contract description have to name exactly the same exports the DXIL library does.
constexpr wchar_t const kRayGenerationExport[] = L"RayGenerationMain";
constexpr wchar_t const kMissBackgroundExport[] = L"MissBackground";
constexpr wchar_t const kMissVoidExport[] = L"MissVoid";
constexpr wchar_t const kClosestHitExport[] = L"ClosestHitMain";
constexpr wchar_t const kHitGroupExport[] = L"TriangleHitGroup";

enum GlobalRootParameter : UINT
{
    GlobalConstants = 0U,
    GlobalSceneSrv = 1U,
    GlobalRecordsUav = 2U,
    GlobalCountersUav = 3U,
};

enum AnalyticRootParameter : UINT
{
    AnalyticConstants = 0U,
    AnalyticInstancesSrv = 1U,
    AnalyticVerticesSrv = 2U,
    AnalyticMaterialsSrv = 3U,
    AnalyticRecordsUav = 4U,
    AnalyticCountersUav = 5U,
};

enum GraphicsRootParameter : UINT
{
    GraphicsConstants = 0U,
    GraphicsRecordsTable = 1U,
};

// Slots inside each frame slot's shader-visible descriptor allocation.
constexpr UINT kRecordsSrvIndex = 0U;
constexpr UINT kSceneSrvIndex = 1U;

// HLSL RAY_FLAG values the configuration can request. They are mirrored here because the constant buffer carries
// them to the Starter's analytic traversal, which has to apply them itself.
constexpr std::uint32_t kHlslRayFlagCullBackFacing = 0x10U;
constexpr std::uint32_t kHlslRayFlagCullFrontFacing = 0x20U;

constexpr std::size_t kFrameRecordWordCount = sizeof(FrameRecord) / sizeof(std::uint32_t);

[[nodiscard]] lgp::framework::Error LabContractFailure(std::string operation, ContractError error)
{
    return lgp::framework::MakeError(std::move(operation),
                                     "Chapter 33 refused the request: " + std::string{ContractErrorName(error)} + ".");
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), blob->GetBufferSize()};
}

[[nodiscard]] std::uint64_t AlignUpBytes(std::uint64_t value, std::uint64_t alignment) noexcept
{
    return ((value + alignment - 1U) / alignment) * alignment;
}

// Applies a row-major 3x4 affine transform to a point.
[[nodiscard]] std::array<double, 3U> TransformPointRowMajor(std::array<double, 12U> const &transform,
                                                            std::array<double, 3U> const &point) noexcept
{
    return {
        transform[0] * point[0] + transform[1] * point[1] + transform[2] * point[2] + transform[3],
        transform[4] * point[0] + transform[5] * point[1] + transform[6] * point[2] + transform[7],
        transform[8] * point[0] + transform[9] * point[1] + transform[10] * point[2] + transform[11],
    };
}

[[nodiscard]] std::array<double, 3U> TransformDirectionRowMajor(std::array<double, 12U> const &transform,
                                                                std::array<double, 3U> const &direction) noexcept
{
    return {
        transform[0] * direction[0] + transform[1] * direction[1] + transform[2] * direction[2],
        transform[4] * direction[0] + transform[5] * direction[1] + transform[6] * direction[2],
        transform[8] * direction[0] + transform[9] * direction[1] + transform[10] * direction[2],
    };
}

// The exact inverse of an affine 3x4 transform, by cofactors. Every transform this lab authors is a dyadic scale
// and a dyadic translation, so the division below is exact and the object-space ray the Starter forms is the
// object-space ray the hardware forms.
[[nodiscard]] std::array<double, 12U> InvertAffine3x4(std::array<double, 12U> const &transform) noexcept
{
    double const a = transform[0];
    double const b = transform[1];
    double const c = transform[2];
    double const d = transform[4];
    double const e = transform[5];
    double const f = transform[6];
    double const g = transform[8];
    double const h = transform[9];
    double const i = transform[10];

    double const determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    std::array<double, 12U> inverse{};
    if (determinant == 0.0)
    {
        return inverse;
    }
    double const inverseDeterminant = 1.0 / determinant;

    inverse[0] = (e * i - f * h) * inverseDeterminant;
    inverse[1] = (c * h - b * i) * inverseDeterminant;
    inverse[2] = (b * f - c * e) * inverseDeterminant;
    inverse[4] = (f * g - d * i) * inverseDeterminant;
    inverse[5] = (a * i - c * g) * inverseDeterminant;
    inverse[6] = (c * d - a * f) * inverseDeterminant;
    inverse[8] = (d * h - e * g) * inverseDeterminant;
    inverse[9] = (b * g - a * h) * inverseDeterminant;
    inverse[10] = (a * e - b * d) * inverseDeterminant;

    std::array<double, 3U> const translation{transform[3], transform[7], transform[11]};
    std::array<double, 3U> const inverseTranslation = TransformDirectionRowMajor(inverse, translation);
    inverse[3] = -inverseTranslation[0];
    inverse[7] = -inverseTranslation[1];
    inverse[11] = -inverseTranslation[2];
    return inverse;
}

struct AuthoredInstance final
{
    std::array<double, 12U> objectToWorld{};
    std::uint32_t instanceId{};
    std::uint32_t instanceMask{};
    std::uint32_t contribution{};
};

// The three authored instances. The transforms are diagonal scales and translations chosen so that every scale and
// every translation is a dyadic rational: the inverses are exact, the panels do not overlap on screen except where
// the overlay is meant to, and the three plane depths land on t = 1.5, t = 2 and t = 2.5 exactly.
[[nodiscard]] AuthoredInstance AuthoredLeftPanel() noexcept
{
    return {
        .objectToWorld = {0.5, 0.0, 0.0, -0.75, 0.0, 0.5, 0.0, 0.25, 0.0, 0.0, 1.0, 0.0},
        .instanceId = 7U,
        .instanceMask = 0x1U,
        .contribution = 0U,
    };
}

// The right panel of the `Paired` scene. Its x scale is negative, so the instance mirrors. DXR decides triangle
// facing in object space, so this instance is still front facing; a world-space winding test would say the
// opposite, and the difference between those two answers is exactly the sign of this determinant.
[[nodiscard]] AuthoredInstance AuthoredMirroredRightPanel() noexcept
{
    return {
        .objectToWorld = {-0.5, 0.0, 0.0, 0.75, 0.0, 0.5, 0.0, -0.25, 0.0, 0.0, 1.0, -0.5},
        .instanceId = 11U,
        .instanceMask = 0x2U,
        .contribution = 1U,
    };
}

// The right panel of the `FacingPair` scene: the same footprint and the same distance, reached by a
// positive-determinant transform that turns the triangles away from the camera. This one really is back facing.
[[nodiscard]] AuthoredInstance AuthoredFlippedRightPanel() noexcept
{
    return {
        .objectToWorld = {-0.5, 0.0, 0.0, 0.75, 0.0, 0.5, 0.0, -0.25, 0.0, 0.0, -1.0, -0.5},
        .instanceId = 11U,
        .instanceMask = 0x2U,
        .contribution = 1U,
    };
}

[[nodiscard]] AuthoredInstance AuthoredNearOverlay() noexcept
{
    return {
        .objectToWorld = {0.25, 0.0, 0.0, -0.75, 0.0, 0.25, 0.0, 0.25, 0.0, 0.0, 1.0, 0.5},
        .instanceId = 23U,
        .instanceMask = 0x4U,
        .contribution = 2U,
    };
}

[[nodiscard]] std::vector<AuthoredInstance> AuthoredInstances(SceneVariant scene)
{
    switch (scene)
    {
    case SceneVariant::Paired:
        return {AuthoredLeftPanel(), AuthoredMirroredRightPanel(), AuthoredNearOverlay()};
    case SceneVariant::FacingPair:
        return {AuthoredLeftPanel(), AuthoredFlippedRightPanel(), AuthoredNearOverlay()};
    case SceneVariant::SingleInstance:
        return {AuthoredLeftPanel()};
    case SceneVariant::NoOverlay:
        return {AuthoredLeftPanel(), AuthoredMirroredRightPanel()};
    default:
        return {};
    }
}

struct ModelCandidate final
{
    bool accepted{};
    double t{};
    double b1{};
    double b2{};
    bool frontFace{};
    // The smallest absolute barycentric coordinate of this candidate, whether or not it was accepted. It is the
    // evidence that the scene keeps its rays away from triangle edges.
    double edgeMargin{std::numeric_limits<double>::infinity()};
};

// Moller-Trumbore in double precision, written from the same formulation the two shaders use so that a
// disagreement is the GPU's arithmetic rather than a second algorithm.
[[nodiscard]] ModelCandidate IntersectTriangleModel(std::array<double, 3U> const &origin,
                                                    std::array<double, 3U> const &direction,
                                                    std::array<double, 3U> const &v0, std::array<double, 3U> const &v1,
                                                    std::array<double, 3U> const &v2, double tMin, double tMax) noexcept
{
    auto const subtract = [](std::array<double, 3U> const &left, std::array<double, 3U> const &right)
    { return std::array<double, 3U>{left[0] - right[0], left[1] - right[1], left[2] - right[2]}; };
    auto const cross = [](std::array<double, 3U> const &left, std::array<double, 3U> const &right)
    {
        return std::array<double, 3U>{
            left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0],
        };
    };
    auto const dot = [](std::array<double, 3U> const &left, std::array<double, 3U> const &right)
    { return left[0] * right[0] + left[1] * right[1] + left[2] * right[2]; };

    ModelCandidate candidate{};
    std::array<double, 3U> const edge1 = subtract(v1, v0);
    std::array<double, 3U> const edge2 = subtract(v2, v0);
    std::array<double, 3U> const pvec = cross(direction, edge2);
    double const determinant = dot(edge1, pvec);
    if (std::abs(determinant) < kParallelRayDeterminantEpsilon)
    {
        return candidate;
    }

    double const inverseDeterminant = 1.0 / determinant;
    std::array<double, 3U> const tvec = subtract(origin, v0);
    double const b1 = dot(tvec, pvec) * inverseDeterminant;
    std::array<double, 3U> const qvec = cross(tvec, edge1);
    double const b2 = dot(direction, qvec) * inverseDeterminant;
    double const b0 = 1.0 - b1 - b2;
    // Only rays that came close to the triangle contribute to the margin. A ray that happens to lie on the
    // extension of an edge but far outside the triangle is not a rounding hazard, and counting it would understate
    // how much room the scene actually leaves.
    double const extent = std::max({std::abs(b0), std::abs(b1), std::abs(b2)});
    if (extent <= 1.5)
    {
        candidate.edgeMargin = std::min({std::abs(b0), std::abs(b1), std::abs(b2)});
    }

    if (b1 < 0.0 || b1 > 1.0 || b2 < 0.0 || b1 + b2 > 1.0)
    {
        return candidate;
    }

    double const t = dot(edge2, qvec) * inverseDeterminant;
    // The interval is strict at both ends, so the rejection is written as the negation of `tMin < t < tMax`
    // directly: a candidate exactly on either endpoint is refused.
    if (t <= tMin || t >= tMax)
    {
        return candidate;
    }

    candidate.accepted = true;
    candidate.t = t;
    candidate.b1 = b1;
    candidate.b2 = b2;
    // A positive determinant is a triangle whose object-space vertices appear clockwise from the ray origin, which
    // is D3D12's front-facing definition.
    candidate.frontFace = determinant > 0.0;
    return candidate;
}

[[nodiscard]] lgp::framework::Status CreateComputePipeline(ID3D12Device10 &device, ID3D12RootSignature &rootSignature,
                                                           lgp::framework::CompiledShader const &shader,
                                                           std::string_view name, ComPtr<ID3D12PipelineState> &pipeline)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &rootSignature;
    description.CS = shader.Bytecode();
    HRESULT const result =
        device.CreateComputePipelineState(&description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState", result,
                                                                "Failed to create the Chapter 33 " + std::string{name} +
                                                                    " compute pipeline."));
    }
    return {};
}

[[nodiscard]] lgp::framework::Status CompileShader(lgp::framework::ShaderCompiler const &compiler,
                                                   lgp::framework::ShaderCompileOptions options,
                                                   wchar_t const *entryPoint, wchar_t const *profile,
                                                   std::vector<lgp::framework::ShaderDefine> defines,
                                                   lgp::framework::CompiledShader &shader)
{
    options.entryPoint = entryPoint == nullptr ? std::wstring{} : std::wstring{entryPoint};
    options.targetProfile = profile;
    options.defines = std::move(defines);
    // A library has no entry point, so it is compiled with a target and nothing else. Passing `-E` here would be
    // rejected by DXC, and passing `-T` twice would be a duplicate the framework already forbids.
    if (entryPoint == nullptr)
    {
        options.additionalArguments = {L"-T", profile};
    }
    else
    {
        options.additionalArguments = {L"-E", entryPoint, L"-T", profile};
    }

    auto compiled = compiler.Compile(options);
    if (!compiled)
    {
        return std::unexpected(std::move(compiled.error()));
    }
    shader = std::move(*compiled);
    return {};
}

[[nodiscard]] BufferBarrierState NoAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS};
}

[[nodiscard]] BufferBarrierState BuildWriteState() noexcept
{
    return {D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
            D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE};
}

[[nodiscard]] BufferBarrierState BuildReadState() noexcept
{
    return {D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
            D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ};
}

[[nodiscard]] BufferBarrierState TraceReadState() noexcept
{
    return {D3D12_BARRIER_SYNC_RAYTRACING, D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ};
}

[[nodiscard]] BufferBarrierState BuildScratchState() noexcept
{
    return {D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
}

[[nodiscard]] BufferBarrierState RaytracingUnorderedAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_RAYTRACING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
}

[[nodiscard]] BufferBarrierState ComputeUnorderedAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
}

[[nodiscard]] BufferBarrierState PixelShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
}

[[nodiscard]] BufferBarrierState CopySourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE};
}

[[nodiscard]] BufferBarrierState CopyDestinationState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST};
}

[[nodiscard]] lgp::framework::TextureBarrierState FrameStartState(
    lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, frameContext.renderTargetInitialLayout};
}

[[nodiscard]] lgp::framework::TextureBarrierState FrameEndState(
    lgp::framework::FrameContext const &frameContext) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
            frameContext.headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT};
}

[[nodiscard]] lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
}

[[nodiscard]] D3D12_TEXTURE_BARRIER MakeTextureBarrier(ID3D12Resource &resource,
                                                       lgp::framework::TextureBarrierState before,
                                                       lgp::framework::TextureBarrierState after) noexcept
{
    D3D12_TEXTURE_BARRIER barrier{};
    barrier.SyncBefore = before.sync;
    barrier.SyncAfter = after.sync;
    barrier.AccessBefore = before.access;
    barrier.AccessAfter = after.access;
    barrier.LayoutBefore = before.layout;
    barrier.LayoutAfter = after.layout;
    barrier.pResource = &resource;
    barrier.Subresources.IndexOrFirstMipLevel = UINT32_MAX;
    barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
    return barrier;
}

[[nodiscard]] D3D12_BUFFER_BARRIER MakeBufferBarrier(ID3D12Resource &resource, BufferBarrierState before,
                                                     BufferBarrierState after) noexcept
{
    D3D12_BUFFER_BARRIER barrier{};
    barrier.SyncBefore = before.sync;
    barrier.SyncAfter = after.sync;
    barrier.AccessBefore = before.access;
    barrier.AccessAfter = after.access;
    barrier.pResource = &resource;
    barrier.Offset = 0U;
    barrier.Size = UINT64_MAX;
    return barrier;
}

void SubmitBufferBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_BUFFER_BARRIER> const &barriers)
{
    if (barriers.empty())
    {
        return;
    }
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_BUFFER;
    group.NumBarriers = static_cast<UINT>(barriers.size());
    group.pBufferBarriers = barriers.data();
    commandList.Barrier(1U, &group);
}

void SubmitTextureBarrier(ID3D12GraphicsCommandList7 &commandList, D3D12_TEXTURE_BARRIER const &barrier)
{
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1U;
    group.pTextureBarriers = &barrier;
    commandList.Barrier(1U, &group);
}

} // namespace

// -------------------------------------------------------------------------------------------------------------
// Names and diagnostics
// -------------------------------------------------------------------------------------------------------------

std::string_view LabStageName(LabStage const stage) noexcept
{
    switch (stage)
    {
    case LabStage::SceneUpload:
        return "SceneUpload";
    case LabStage::BottomLevelBuild:
        return "BottomLevelBuild";
    case LabStage::BottomLevelBarrier:
        return "BottomLevelBarrier";
    case LabStage::TopLevelBuild:
        return "TopLevelBuild";
    case LabStage::TopLevelBarrier:
        return "TopLevelBarrier";
    case LabStage::StateObjectResolved:
        return "StateObjectResolved";
    case LabStage::ShaderTableRecorded:
        return "ShaderTableRecorded";
    case LabStage::RayDispatch:
        return "RayDispatch";
    case LabStage::OutputReadback:
        return "OutputReadback";
    default:
        return "UnknownStage";
    }
}

std::string_view RaytracingSupportDiagnostic(RaytracingSupportStatus const status) noexcept
{
    switch (status)
    {
    case RaytracingSupportStatus::Supported:
        return "DXR 1.0 is available: the adapter reports at least D3D12_RAYTRACING_TIER_1_0 and Shader Model 6.3.";
    case RaytracingSupportStatus::OptionsQueryFailed:
        return "D3D12_FEATURE_D3D12_OPTIONS5 could not be queried, so the raytracing tier is unknown.";
    case RaytracingSupportStatus::TierUnsupported:
        return "The adapter reports D3D12_RAYTRACING_TIER_NOT_SUPPORTED.";
    case RaytracingSupportStatus::ShaderModelQueryFailed:
        return "D3D12_FEATURE_SHADER_MODEL could not be queried, so the highest shader model is unknown.";
    case RaytracingSupportStatus::ShaderModelUnsupported:
        return "The adapter's highest shader model is below 6.3, which lib_6_3 raytracing shaders require.";
    case RaytracingSupportStatus::StateObjectUnavailable:
        return "The raytracing state object could not be created.";
    case RaytracingSupportStatus::ShaderIdentifierUnavailable:
        return "The state object returned no usable shader identifier for a declared export.";
    default:
        return "Unknown raytracing support status.";
    }
}

std::string DescribeShaderModel(std::uint32_t const shaderModel)
{
    // `D3D_SHADER_MODEL` packs the version as one nibble per component, so 0x63 is Shader Model 6.3. Printing the
    // raw value in decimal, or in decimal behind an `0x`, is how a capability diagnostic ends up claiming "99".
    if (shaderModel == 0U)
    {
        return "unknown";
    }
    return std::to_string((shaderModel >> 4U) & 0xFU) + "." + std::to_string(shaderModel & 0xFU);
}

// -------------------------------------------------------------------------------------------------------------
// Configuration
// -------------------------------------------------------------------------------------------------------------

LabConfiguration DefaultConfiguration(LabVariant const variant) noexcept
{
    LabConfiguration configuration{};
    if (variant == LabVariant::Starter)
    {
        configuration.topLevelBuild = TopLevelBuildMode::Rebuild;
    }
    return configuration;
}

RayFlags MakeRayFlags(LabConfiguration const &configuration) noexcept
{
    RayFlags flags = RayFlags::None;
    if (configuration.cullBackFacingTriangles)
    {
        flags = flags | RayFlags::CullBackFacingTriangles;
    }
    if (configuration.cullFrontFacingTriangles)
    {
        flags = flags | RayFlags::CullFrontFacingTriangles;
    }
    return flags;
}

std::uint32_t MakeHlslRayFlags(LabConfiguration const &configuration) noexcept
{
    std::uint32_t flags = 0U;
    if (configuration.cullBackFacingTriangles)
    {
        flags |= kHlslRayFlagCullBackFacing;
    }
    if (configuration.cullFrontFacingTriangles)
    {
        flags |= kHlslRayFlagCullFrontFacing;
    }
    return flags;
}

lgp::framework::Status ValidateLabConfiguration(LabConfiguration const &configuration, LabVariant const variant)
{
    if (static_cast<std::uint32_t>(configuration.debugView) >= kDebugViewCount)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "The Chapter 33 debug view is out of range."));
    }
    if (static_cast<std::uint32_t>(configuration.scene) >= kSceneVariantCount)
    {
        return std::unexpected(
            lgp::framework::MakeError("ValidateLabConfiguration", "The Chapter 33 scene variant is out of range."));
    }

    if (auto const mask = ValidateInstanceInclusionMask(configuration.instanceInclusionMask); !mask)
    {
        return std::unexpected(LabContractFailure("ValidateInstanceInclusionMask", mask.error()));
    }
    if (auto const flags = ValidateRayFlags(MakeRayFlags(configuration)); !flags)
    {
        return std::unexpected(LabContractFailure("ValidateRayFlags", flags.error()));
    }

    RayDescription const ray{
        .origin = {0.0, 0.0, kCameraOriginZ},
        .direction = {0.0, 0.0, -1.0},
        .tMinMetres = static_cast<double>(configuration.rayTMin),
        .tMaxMetres = static_cast<double>(configuration.rayTMax),
    };
    if (auto const validation = ValidateRay(ray); !validation)
    {
        return std::unexpected(LabContractFailure("ValidateRay", validation.error()));
    }

    // Every surface in this lab is a plane perpendicular to the ray, so the frame's distances are known before it
    // runs. An endpoint placed on one of them is an endpoint whose answer is the driver's rather than DXR's, and
    // the chapter refuses to ask that question rather than shipping a test that measures WARP.
    for (double const distance : ScenePlaneDistances(configuration.scene))
    {
        double const toMinimum = std::abs(distance - static_cast<double>(configuration.rayTMin));
        double const toMaximum = std::abs(distance - static_cast<double>(configuration.rayTMax));
        if (toMinimum < kIntervalEndpointMarginMetres || toMaximum < kIntervalEndpointMarginMetres)
        {
            return std::unexpected(lgp::framework::MakeError(
                "ValidateLabConfiguration",
                "Chapter 33 keeps every ray-interval endpoint at least " +
                    std::to_string(kIntervalEndpointMarginMetres) + " m away from a surface, and this scene has one " +
                    "at " + std::to_string(distance) +
                    " m. D3D12 specifies a strict interval, implementations "
                    "disagree about the endpoint itself, and the chapter refuses to build evidence on that "
                    "disagreement."));
        }
    }

    if (configuration.missShaderIndex >= kMissRecordCount)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidateLabConfiguration", "Chapter 33 publishes " + std::to_string(kMissRecordCount) +
                                            " miss records, so the miss shader index must be below that."));
    }

    // Every instance's record index has to land inside the hit-group section. The three terms are the contract's,
    // so a caller that pushes the ray contribution past the table's end is refused with the contract's own name
    // for the mistake rather than with a silent out-of-bounds table read on the GPU.
    for (AuthoredInstance const &instance : AuthoredInstances(configuration.scene))
    {
        HitGroupIndexParameters const parameters{
            .rayContributionToHitGroupIndex = configuration.rayContributionToHitGroupIndex,
            .multiplierForGeometryContributionToHitGroupIndex = configuration.multiplierForGeometryContribution,
            .geometryContributionToHitGroupIndex = 0U,
            .instanceContributionToHitGroupIndex = instance.contribution,
        };
        auto const index = ComputeHitGroupRecordIndex(parameters);
        if (!index)
        {
            return std::unexpected(LabContractFailure("ComputeHitGroupRecordIndex", index.error()));
        }
        if (*index >= kHitGroupRecordCount)
        {
            return std::unexpected(lgp::framework::MakeError(
                "ValidateLabConfiguration",
                "Chapter 33 lays out " + std::to_string(kHitGroupRecordCount) + " hit-group records, and instance " +
                    std::to_string(instance.instanceId) + " would select record " + std::to_string(*index) + "."));
        }
    }

    if (variant == LabVariant::Starter)
    {
        if (configuration.topLevelBuild == TopLevelBuildMode::Update)
        {
            return std::unexpected(lgp::framework::MakeError(
                "ValidateLabConfiguration",
                "The Chapter 33 Starter builds no acceleration structure, so it cannot refit one."));
        }
    }
    return {};
}

// -------------------------------------------------------------------------------------------------------------
// The analytic scene
// -------------------------------------------------------------------------------------------------------------

std::span<VertexPosition const> GeometryVertices() noexcept
{
    // Two disjoint triangles in object space, wound so that they are front facing to a ray travelling along -Z.
    // The gap between them is what makes a miss observable inside the silhouette of an instance.
    static constexpr std::array<VertexPosition, kVertexCount> kVertices{{
        {-0.90F, -0.60F, 0.0F},
        {-0.10F, -0.60F, 0.0F},
        {-0.50F, 0.60F, 0.0F},
        {0.10F, -0.60F, 0.0F},
        {0.90F, -0.60F, 0.0F},
        {0.50F, 0.60F, 0.0F},
    }};
    return kVertices;
}

std::span<HitGroupLocalConstants const> MaterialTable() noexcept
{
    // Four hit-group records. Each one reports the index it believes it occupies, and every colour is a dyadic
    // rational so that a float and a double agree about it exactly.
    static constexpr std::array<HitGroupLocalConstants, kMaterialCount> kMaterials{{
        {0U, 0U, 0.875F, 0.375F, 0.250F},
        {1U, 1U, 0.250F, 0.625F, 0.9375F},
        {2U, 2U, 0.375F, 0.875F, 0.4375F},
        {3U, 3U, 0.875F, 0.750F, 0.250F},
    }};
    return kMaterials;
}

std::vector<InstanceRecord> BuildInstances(SceneVariant const scene)
{
    std::vector<InstanceRecord> instances{};
    for (AuthoredInstance const &authored : AuthoredInstances(scene))
    {
        std::array<double, 12U> const inverse = InvertAffine3x4(authored.objectToWorld);
        InstanceRecord record{};
        for (std::size_t index = 0U; index < 12U; ++index)
        {
            record.objectToWorld[index] = static_cast<float>(authored.objectToWorld[index]);
            record.worldToObject[index] = static_cast<float>(inverse[index]);
        }
        record.instanceId = authored.instanceId;
        record.instanceMask = authored.instanceMask;
        record.instanceContributionToHitGroupIndex = authored.contribution;
        record.flags = 0U;
        instances.push_back(record);
    }
    return instances;
}

std::vector<double> ScenePlaneDistances(SceneVariant const scene)
{
    std::vector<double> distances{};
    for (AuthoredInstance const &authored : AuthoredInstances(scene))
    {
        // Every instance's geometry lies in its own object-space z = 0 plane, so its world plane is the transform's
        // z translation and the distance along a -Z ray is the camera's z minus it.
        distances.push_back(kCameraOriginZ - authored.objectToWorld[11]);
    }
    return distances;
}

std::vector<InstanceDescription> BuildInstanceDescriptions(SceneVariant const scene,
                                                           std::uint64_t const bottomLevelAddress)
{
    std::vector<InstanceDescription> descriptions{};
    for (AuthoredInstance const &authored : AuthoredInstances(scene))
    {
        InstanceDescription description{};
        description.objectToWorld.values = authored.objectToWorld;
        description.instanceId = authored.instanceId;
        description.instanceMask = authored.instanceMask;
        description.instanceContributionToHitGroupIndex = authored.contribution;
        description.flags = InstanceFlags::None;
        description.bottomLevelAddress = bottomLevelAddress;
        descriptions.push_back(description);
    }
    return descriptions;
}

std::vector<ReferenceInstance> BuildReferenceInstances(SceneVariant const scene)
{
    std::span<VertexPosition const> const vertices = GeometryVertices();
    ReferenceGeometry geometry{};
    geometry.flags = GeometryFlags::Opaque;
    for (std::uint32_t triangle = 0U; triangle < kTrianglesPerGeometry; ++triangle)
    {
        auto const at = [&vertices](std::size_t index)
        {
            return Float3{
                static_cast<double>(vertices[index].x),
                static_cast<double>(vertices[index].y),
                static_cast<double>(vertices[index].z),
            };
        };
        std::size_t const base = static_cast<std::size_t>(triangle) * 3U;
        geometry.triangles.push_back({at(base + 0U), at(base + 1U), at(base + 2U)});
    }

    std::vector<ReferenceInstance> instances{};
    for (AuthoredInstance const &authored : AuthoredInstances(scene))
    {
        ReferenceInstance instance{};
        instance.objectToWorld.values = authored.objectToWorld;
        instance.instanceId = authored.instanceId;
        instance.instanceMask = authored.instanceMask;
        instance.instanceContributionToHitGroupIndex = authored.contribution;
        instance.flags = InstanceFlags::None;
        instance.geometries.push_back(geometry);
        instances.push_back(std::move(instance));
    }
    return instances;
}

std::uint32_t BuildFrameStages(LabConfiguration const &configuration, LabVariant const variant,
                               std::span<LabStage> stages) noexcept
{
    (void)configuration;
    std::uint32_t count = 0U;
    auto const push = [&stages, &count](LabStage stage)
    {
        if (count < stages.size())
        {
            stages[count] = stage;
            ++count;
        }
    };

    push(LabStage::SceneUpload);
    if (variant == LabVariant::Solution)
    {
        push(LabStage::BottomLevelBuild);
        push(LabStage::BottomLevelBarrier);
        push(LabStage::TopLevelBuild);
        push(LabStage::TopLevelBarrier);
        push(LabStage::StateObjectResolved);
        push(LabStage::ShaderTableRecorded);
    }
    push(LabStage::RayDispatch);
    push(LabStage::OutputReadback);
    return count;
}

std::uint64_t EncodeStageOrder(std::span<LabStage const> stages) noexcept
{
    std::uint64_t word = 0U;
    std::size_t const count = std::min<std::size_t>(stages.size(), 16U);
    for (std::size_t index = 0U; index < count; ++index)
    {
        std::uint64_t const nibble = static_cast<std::uint64_t>(stages[index]) + 1U;
        word |= (nibble & 0xFULL) << (index * 4U);
    }
    return word;
}

std::uint32_t StageMask(std::span<LabStage const> stages) noexcept
{
    std::uint32_t mask = 0U;
    for (LabStage const stage : stages)
    {
        mask |= 1U << static_cast<std::uint32_t>(stage);
    }
    return mask;
}

std::uint32_t UnavailableStageMask(LabVariant const variant) noexcept
{
    if (variant == LabVariant::Solution)
    {
        return 0U;
    }
    return (1U << static_cast<std::uint32_t>(LabStage::BottomLevelBuild)) |
           (1U << static_cast<std::uint32_t>(LabStage::BottomLevelBarrier)) |
           (1U << static_cast<std::uint32_t>(LabStage::TopLevelBuild)) |
           (1U << static_cast<std::uint32_t>(LabStage::TopLevelBarrier)) |
           (1U << static_cast<std::uint32_t>(LabStage::StateObjectResolved)) |
           (1U << static_cast<std::uint32_t>(LabStage::ShaderTableRecorded));
}

// -------------------------------------------------------------------------------------------------------------
// The deterministic CPU model
// -------------------------------------------------------------------------------------------------------------

std::expected<ReferenceFrame, lgp::framework::Error> BuildReferenceFrame(LabConfiguration const &configuration,
                                                                         LabVariant const variant,
                                                                         lgp::framework::Extent2D const extent)
{
    if (extent.empty())
    {
        return std::unexpected(
            lgp::framework::MakeError("BuildReferenceFrame", "The Chapter 33 reference frame needs a non-empty size."));
    }

    std::vector<AuthoredInstance> const authored = AuthoredInstances(configuration.scene);
    std::vector<std::array<double, 12U>> inverses{};
    inverses.reserve(authored.size());
    for (AuthoredInstance const &instance : authored)
    {
        inverses.push_back(InvertAffine3x4(instance.objectToWorld));
    }

    std::span<VertexPosition const> const vertices = GeometryVertices();
    std::span<HitGroupLocalConstants const> const materials = MaterialTable();

    ReferenceFrame reference{};
    reference.records.resize(static_cast<std::size_t>(extent.width) * static_cast<std::size_t>(extent.height));
    reference.minimumEdgeMargin = std::numeric_limits<double>::infinity();
    reference.frame.abiMarker = kAbiMarker;
    reference.frame.dispatchWidth = extent.width;
    reference.frame.dispatchHeight = extent.height;
    reference.frame.rayCount = extent.width * extent.height;

    double const tMin = static_cast<double>(configuration.rayTMin);
    double const tMax = static_cast<double>(configuration.rayTMax);
    bool const cullBack = configuration.cullBackFacingTriangles;
    bool const cullFront = configuration.cullFrontFacingTriangles;

    for (std::uint32_t pixelY = 0U; pixelY < extent.height; ++pixelY)
    {
        for (std::uint32_t pixelX = 0U; pixelX < extent.width; ++pixelX)
        {
            double const u = (static_cast<double>(pixelX) + 0.5) / static_cast<double>(extent.width);
            double const v = (static_cast<double>(pixelY) + 0.5) / static_cast<double>(extent.height);
            std::array<double, 3U> const worldOrigin{
                -kWindowHalfExtentX + 2.0 * kWindowHalfExtentX * u,
                kWindowHalfExtentY - 2.0 * kWindowHalfExtentY * v,
                kCameraOriginZ,
            };
            std::array<double, 3U> const worldDirection{0.0, 0.0, -1.0};

            bool found = false;
            double nearest = tMax;
            double bestT = 0.0;
            double bestB1 = 0.0;
            double bestB2 = 0.0;
            bool bestFrontFace = true;
            std::uint32_t bestInstance = kInvalidIndex;
            std::uint32_t bestPrimitive = kInvalidIndex;

            for (std::size_t instanceIndex = 0U; instanceIndex < authored.size(); ++instanceIndex)
            {
                if ((authored[instanceIndex].instanceMask & configuration.instanceInclusionMask) == 0U)
                {
                    continue;
                }
                std::array<double, 3U> const objectOrigin =
                    TransformPointRowMajor(inverses[instanceIndex], worldOrigin);
                std::array<double, 3U> const objectDirection =
                    TransformDirectionRowMajor(inverses[instanceIndex], worldDirection);

                for (std::uint32_t primitive = 0U; primitive < kTrianglesPerGeometry; ++primitive)
                {
                    auto const at = [&vertices](std::size_t index)
                    {
                        return std::array<double, 3U>{
                            static_cast<double>(vertices[index].x),
                            static_cast<double>(vertices[index].y),
                            static_cast<double>(vertices[index].z),
                        };
                    };
                    std::size_t const base = static_cast<std::size_t>(primitive) * 3U;
                    ModelCandidate const candidate = IntersectTriangleModel(
                        objectOrigin, objectDirection, at(base + 0U), at(base + 1U), at(base + 2U), tMin, nearest);
                    reference.minimumEdgeMargin = std::min(reference.minimumEdgeMargin, candidate.edgeMargin);
                    if (!candidate.accepted)
                    {
                        continue;
                    }
                    if ((candidate.frontFace && cullFront) || (!candidate.frontFace && cullBack))
                    {
                        continue;
                    }
                    found = true;
                    nearest = candidate.t;
                    bestT = candidate.t;
                    bestB1 = candidate.b1;
                    bestB2 = candidate.b2;
                    bestFrontFace = candidate.frontFace;
                    bestInstance = static_cast<std::uint32_t>(instanceIndex);
                    bestPrimitive = primitive;
                }
            }

            RayRecord record{};
            std::uint32_t const traversalBit =
                variant == LabVariant::Solution ? kRayStatusFixedFunctionTraversal : kRayStatusAnalyticTraversal;
            if (found)
            {
                AuthoredInstance const &instance = authored[bestInstance];
                HitGroupIndexParameters const parameters{
                    .rayContributionToHitGroupIndex = configuration.rayContributionToHitGroupIndex,
                    .multiplierForGeometryContributionToHitGroupIndex = configuration.multiplierForGeometryContribution,
                    .geometryContributionToHitGroupIndex = 0U,
                    .instanceContributionToHitGroupIndex = instance.contribution,
                };
                auto const recordIndex = ComputeHitGroupRecordIndex(parameters);
                if (!recordIndex)
                {
                    return std::unexpected(LabContractFailure("ComputeHitGroupRecordIndex", recordIndex.error()));
                }
                std::size_t const materialIndex =
                    std::min<std::size_t>(static_cast<std::size_t>(*recordIndex), materials.size() - 1U);
                HitGroupLocalConstants const &material = materials[materialIndex];

                record.status = kRayStatusTraversalRan | traversalBit | kRayStatusHit |
                                (bestFrontFace ? kRayStatusFrontFace : kRayStatusBackFace);
                if (variant == LabVariant::Solution)
                {
                    record.status |= kRayStatusClosestHitRan | kRayStatusLocalRootArgumentsRead;
                }
                record.instanceIndex = bestInstance;
                record.instanceId = instance.instanceId;
                record.primitiveIndex = bestPrimitive;
                record.hitKind = bestFrontFace ? kHitKindTriangleFrontFace : kHitKindTriangleBackFace;
                record.hitGroupRecordIndex = static_cast<std::uint32_t>(*recordIndex);
                record.materialId = material.materialId;
                record.missShaderIndex = kInvalidIndex;
                record.tHit = static_cast<float>(bestT);
                record.barycentricB1 = static_cast<float>(bestB1);
                record.barycentricB2 = static_cast<float>(bestB2);
                record.worldPositionX = static_cast<float>(worldOrigin[0]);
                record.worldPositionY = static_cast<float>(worldOrigin[1]);
                record.worldPositionZ = static_cast<float>(worldOrigin[2] - bestT);

                double const weight = 0.45 + 0.55 * std::clamp(1.0 - bestB1 - bestB2, 0.0, 1.0);
                double const facing = bestFrontFace ? 1.0 : 0.45;
                record.colorR = static_cast<float>(static_cast<double>(material.colorR) * weight * facing);
                record.colorG = static_cast<float>(static_cast<double>(material.colorG) * weight * facing);
                record.colorB = static_cast<float>(static_cast<double>(material.colorB) * weight * facing);
            }
            else
            {
                record.status = kRayStatusTraversalRan | traversalBit | kRayStatusMiss;
                if (variant == LabVariant::Solution)
                {
                    record.status |= kRayStatusMissShaderRan;
                }
                record.instanceIndex = kInvalidIndex;
                record.instanceId = kInvalidIndex;
                record.primitiveIndex = kInvalidIndex;
                record.hitKind = 0U;
                record.hitGroupRecordIndex = kInvalidIndex;
                record.materialId = kInvalidIndex;
                record.missShaderIndex = configuration.missShaderIndex;
                if (configuration.missShaderIndex == 0U)
                {
                    record.colorR = static_cast<float>(0.04 + 0.10 * v);
                    record.colorG = static_cast<float>(0.05 + 0.13 * v);
                    record.colorB = static_cast<float>(0.12 + 0.20 * v);
                }
            }

            reference.records[static_cast<std::size_t>(pixelY) * extent.width + pixelX] = record;

            reference.frame.traversalMask |= record.status;
            if ((record.status & kRayStatusHit) != 0U)
            {
                ++reference.frame.hitCount;
                if ((record.status & kRayStatusFrontFace) != 0U)
                {
                    ++reference.frame.frontFaceCount;
                }
                else
                {
                    ++reference.frame.backFaceCount;
                }
                if (record.hitGroupRecordIndex < kHitGroupRecordCount)
                {
                    ++reference.frame.hitGroupRecordCounts[record.hitGroupRecordIndex];
                }
                if (record.instanceIndex < authored.size())
                {
                    ++reference.frame.instanceHitCounts[record.instanceIndex];
                }
            }
            else
            {
                ++reference.frame.missCount;
                if (record.missShaderIndex < kMissRecordCount)
                {
                    ++reference.frame.missRecordCounts[record.missShaderIndex];
                }
            }
        }
    }

    reference.hitCount = reference.frame.hitCount;
    reference.missCount = reference.frame.missCount;
    if (!std::isfinite(reference.minimumEdgeMargin))
    {
        reference.minimumEdgeMargin = 1.0;
    }
    return reference;
}

// -------------------------------------------------------------------------------------------------------------
// Buffers
// -------------------------------------------------------------------------------------------------------------

BufferResource::BufferResource(BufferResource &&other) noexcept
{
    *this = std::move(other);
}

BufferResource &BufferResource::operator=(BufferResource &&other) noexcept
{
    if (this != &other)
    {
        Reset();
        resource_ = std::move(other.resource_);
        sizeInBytes_ = std::exchange(other.sizeInBytes_, 0U);
        mappedData_ = std::exchange(other.mappedData_, nullptr);
    }
    return *this;
}

BufferResource::~BufferResource()
{
    Reset();
}

void BufferResource::Reset() noexcept
{
    if (resource_ != nullptr && mappedData_ != nullptr)
    {
        D3D12_RANGE const writtenRange{0U, 0U};
        resource_->Unmap(0U, &writtenRange);
    }
    resource_.Reset();
    sizeInBytes_ = 0U;
    mappedData_ = nullptr;
}

std::uint64_t BufferResource::gpu_virtual_address() const noexcept
{
    return resource_ == nullptr ? 0U : resource_->GetGPUVirtualAddress();
}

std::expected<BufferResource, lgp::framework::Error> CreateBuffer(
    ID3D12Device10 &device, std::uint64_t const sizeInBytes, D3D12_HEAP_TYPE const heapType,
    D3D12_RESOURCE_FLAGS const flags, std::wstring_view const name, bool const mapPersistently)
{
    if (sizeInBytes == 0U)
    {
        return std::unexpected(lgp::framework::MakeError("CreateBuffer", "Chapter 33 buffers must be non-empty."));
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = heapType;
    heapProperties.CreationNodeMask = 1U;
    heapProperties.VisibleNodeMask = 1U;

    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = sizeInBytes;
    description.Height = 1U;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;

    BufferResource buffer{};
    // Buffers have no layout, so an enhanced-barrier buffer is always created with `UNDEFINED`. That is true of
    // the acceleration structures too: the `RAYTRACING_ACCELERATION_STRUCTURE` flag describes what may access the
    // memory, not a layout the runtime has to track.
    HRESULT const result = device.CreateCommittedResource3(
        &heapProperties, D3D12_HEAP_FLAG_NONE, &description, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0U,
        nullptr, IID_PPV_ARGS(buffer.resource_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device10::CreateCommittedResource3", result,
                                                                "Failed to create a Chapter 33 buffer."));
    }
    if (!name.empty())
    {
        std::wstring const objectName{name};
        HRESULT const nameResult = buffer.resource_->SetName(objectName.c_str());
        if (FAILED(nameResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Object::SetName", nameResult,
                                                                    "Failed to name a Chapter 33 buffer."));
        }
    }
    if (mapPersistently)
    {
        D3D12_RANGE const readRange{0U, heapType == D3D12_HEAP_TYPE_READBACK ? sizeInBytes : 0U};
        void *mapped = nullptr;
        HRESULT const mapResult = buffer.resource_->Map(0U, &readRange, &mapped);
        if (FAILED(mapResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                    "Failed to map a Chapter 33 buffer."));
        }
        buffer.mappedData_ = static_cast<std::byte *>(mapped);
    }
    buffer.sizeInBytes_ = sizeInBytes;
    return buffer;
}

// -------------------------------------------------------------------------------------------------------------
// RendererCore
// -------------------------------------------------------------------------------------------------------------

RendererCore::RendererCore(std::filesystem::path shaderPath, LabVariant const variant)
    : shaderPath_{std::move(shaderPath)}, variant_{variant}
{
    interactiveConfiguration_ = DefaultConfiguration(variant);
}

RaytracingCapability RendererCore::capability() const noexcept
{
    return capability_;
}

std::span<ShaderArtifact const> RendererCore::shader_artifacts() const noexcept
{
    return shaderArtifacts_;
}

LabConfiguration RendererCore::ActiveConfiguration() const noexcept
{
    if (headless_)
    {
        return headlessConfiguration_.value_or(DefaultConfiguration(variant_));
    }
    return interactiveConfiguration_;
}

lgp::framework::Status RendererCore::QueryCapability()
{
    capability_ = {};

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    HRESULT const optionsResult =
        deviceResources_->device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options));
    if (FAILED(optionsResult))
    {
        capability_.status = RaytracingSupportStatus::OptionsQueryFailed;
        return {};
    }
    capability_.reportedTier = static_cast<std::uint32_t>(options.RaytracingTier);
    capability_.tier10Available = options.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0;
    if (!capability_.tier10Available)
    {
        capability_.status = RaytracingSupportStatus::TierUnsupported;
        return {};
    }

    // `CheckFeatureSupport` for shader models answers "the highest you may use, capped by what you asked for", and
    // it fails outright when the runtime does not know the model the caller named. Walking down from the newest
    // model the course knows about is the only way to get a truthful answer on every runtime.
    static constexpr std::array<D3D_SHADER_MODEL, 6U> kCandidateModels{
        D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
        D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3,
    };
    bool queried = false;
    for (D3D_SHADER_MODEL const candidate : kCandidateModels)
    {
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{candidate};
        if (SUCCEEDED(deviceResources_->device()->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel,
                                                                      sizeof(shaderModel))))
        {
            capability_.reportedShaderModel = static_cast<std::uint32_t>(shaderModel.HighestShaderModel);
            queried = true;
            break;
        }
    }
    if (!queried)
    {
        capability_.status = RaytracingSupportStatus::ShaderModelQueryFailed;
        return {};
    }

    capability_.shaderModel63Available =
        capability_.reportedShaderModel >= static_cast<std::uint32_t>(D3D_SHADER_MODEL_6_3);
    if (!capability_.shaderModel63Available)
    {
        capability_.status = RaytracingSupportStatus::ShaderModelUnsupported;
        return {};
    }

    capability_.status = RaytracingSupportStatus::Supported;
    capability_.dispatchable = true;
    return {};
}

lgp::framework::Status RendererCore::CreateShaders()
{
    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }
    lgp::framework::ShaderCompiler const compiler = std::move(*compilerResult);

    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = shaderPath_;
    options.includeDirectories = {shaderPath_.parent_path(), shaderPath_.parent_path().parent_path() / "Common"};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif

    shaderArtifacts_.clear();
    auto const publish = [this](lgp::framework::CompiledShader const &shader)
    {
        shaderArtifacts_.push_back({
            .entryPoint = shader.entryPoint,
            .targetProfile = shader.targetProfile,
            .bytecodeSizeBytes = shader.bytecode.size(),
            .diagnosticsEmpty = shader.diagnostics.empty(),
        });
    };

    if (variant_ == LabVariant::Starter)
    {
        if (auto status = CompileShader(compiler, options, L"AnalyticTraceCS", L"cs_6_0",
                                        {{L"LGP_CH33_ANALYTIC", L"1"}}, analyticShader_);
            !status)
        {
            return status;
        }
        publish(analyticShader_);
    }
    else if (capability_.dispatchable)
    {
        // A DXIL library has no entry point. Shader Model 6.3 is the floor DXR 1.0 requires, and the chapter
        // compiles exactly that rather than the newest model available, so the shader the learner reads is the
        // shader the minimum-specification device runs.
        if (auto status = CompileShader(compiler, options, nullptr, L"lib_6_3", {{L"LGP_CH33_RAYTRACING", L"1"}},
                                        raytracingLibrary_);
            !status)
        {
            return status;
        }
        publish(raytracingLibrary_);
    }

    if (auto status =
            CompileShader(compiler, options, L"FullscreenVS", L"vs_6_0", {{L"LGP_CH33_DISPLAY", L"1"}}, vertexShader_);
        !status)
    {
        return status;
    }
    publish(vertexShader_);

    if (auto status =
            CompileShader(compiler, options, L"DisplayPS", L"ps_6_0", {{L"LGP_CH33_DISPLAY", L"1"}}, pixelShader_);
        !status)
    {
        return status;
    }
    publish(pixelShader_);
    return {};
}

lgp::framework::Status RendererCore::CreateRootSignatures()
{
    auto const serializeAndCreate = [this](D3D12_ROOT_SIGNATURE_DESC const &description, std::string_view name,
                                           ComPtr<ID3D12RootSignature> &rootSignature) -> lgp::framework::Status
    {
        ComPtr<ID3DBlob> serialized{};
        ComPtr<ID3DBlob> errors{};
        HRESULT const serializeResult =
            D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, serialized.ReleaseAndGetAddressOf(),
                                        errors.ReleaseAndGetAddressOf());
        if (FAILED(serializeResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult,
                                                                    BlobText(errors.Get())));
        }
        HRESULT const createResult = deviceResources_->device()->CreateRootSignature(
            0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(rootSignature.ReleaseAndGetAddressOf()));
        if (FAILED(createResult))
        {
            return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                                                    "Failed to create the Chapter 33 " +
                                                                        std::string{name} + " root signature."));
        }
        return {};
    };

    constexpr UINT kDispatchConstantCount = sizeof(DispatchConstants) / sizeof(std::uint32_t);
    if (variant_ == LabVariant::Starter)
    {
        std::array<D3D12_ROOT_PARAMETER, 6U> parameters{};
        parameters[AnalyticConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[AnalyticConstants].Constants.ShaderRegister = 0U;
        parameters[AnalyticConstants].Constants.Num32BitValues = kDispatchConstantCount;
        auto const setRootView = [&parameters](UINT slot, D3D12_ROOT_PARAMETER_TYPE type, UINT shaderRegister)
        {
            parameters[slot].ParameterType = type;
            parameters[slot].Descriptor.ShaderRegister = shaderRegister;
        };
        setRootView(AnalyticInstancesSrv, D3D12_ROOT_PARAMETER_TYPE_SRV, 0U);
        setRootView(AnalyticVerticesSrv, D3D12_ROOT_PARAMETER_TYPE_SRV, 1U);
        setRootView(AnalyticMaterialsSrv, D3D12_ROOT_PARAMETER_TYPE_SRV, 2U);
        setRootView(AnalyticRecordsUav, D3D12_ROOT_PARAMETER_TYPE_UAV, 0U);
        setRootView(AnalyticCountersUav, D3D12_ROOT_PARAMETER_TYPE_UAV, 1U);

        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = static_cast<UINT>(parameters.size());
        description.pParameters = parameters.data();
        if (auto status = serializeAndCreate(description, "analytic traversal", globalRootSignature_); !status)
        {
            return status;
        }
    }
    else
    {
        // The global root signature every raytracing shader in the state object shares. The acceleration structure
        // is a descriptor-table entry rather than a root SRV because that is how an engine with more than one
        // structure binds one, and because a descriptor is the only binding for which D3D12 defines a *null*
        // acceleration structure at all. The lab does not use that null - WARP removes the device when a dispatch
        // reaches one - but the binding shape is the one the rule is written for.
        D3D12_DESCRIPTOR_RANGE sceneRange{};
        sceneRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sceneRange.NumDescriptors = 1U;
        sceneRange.BaseShaderRegister = 0U;
        sceneRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        std::array<D3D12_ROOT_PARAMETER, 4U> parameters{};
        parameters[GlobalConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[GlobalConstants].Constants.ShaderRegister = 0U;
        parameters[GlobalConstants].Constants.Num32BitValues = kDispatchConstantCount;
        parameters[GlobalSceneSrv].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[GlobalSceneSrv].DescriptorTable.NumDescriptorRanges = 1U;
        parameters[GlobalSceneSrv].DescriptorTable.pDescriptorRanges = &sceneRange;
        parameters[GlobalRecordsUav].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[GlobalRecordsUav].Descriptor.ShaderRegister = 0U;
        parameters[GlobalCountersUav].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[GlobalCountersUav].Descriptor.ShaderRegister = 1U;

        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = static_cast<UINT>(parameters.size());
        description.pParameters = parameters.data();
        if (auto status = serializeAndCreate(description, "raytracing global", globalRootSignature_); !status)
        {
            return status;
        }

        // The hit group's local root signature. Its arguments live inside a shader-table record, which is why it
        // is declared LOCAL: the runtime must not expect them to be bound from the command list.
        D3D12_ROOT_PARAMETER localParameter{};
        localParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        localParameter.Constants.ShaderRegister = 1U;
        localParameter.Constants.Num32BitValues = kHitGroupLocalConstantCount;
        D3D12_ROOT_SIGNATURE_DESC localDescription{};
        localDescription.NumParameters = 1U;
        localDescription.pParameters = &localParameter;
        localDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
        if (auto status = serializeAndCreate(localDescription, "hit-group local", hitGroupLocalRootSignature_); !status)
        {
            return status;
        }
    }

    D3D12_DESCRIPTOR_RANGE recordsRange{};
    recordsRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    recordsRange.NumDescriptors = 1U;
    recordsRange.BaseShaderRegister = 0U;
    recordsRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 2U> graphicsParameters{};
    graphicsParameters[GraphicsConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[GraphicsConstants].Constants.ShaderRegister = 0U;
    graphicsParameters[GraphicsConstants].Constants.Num32BitValues = sizeof(DisplayConstants) / sizeof(std::uint32_t);
    graphicsParameters[GraphicsConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    graphicsParameters[GraphicsRecordsTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParameters[GraphicsRecordsTable].DescriptorTable.NumDescriptorRanges = 1U;
    graphicsParameters[GraphicsRecordsTable].DescriptorTable.pDescriptorRanges = &recordsRange;
    graphicsParameters[GraphicsRecordsTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC graphicsDescription{};
    graphicsDescription.NumParameters = static_cast<UINT>(graphicsParameters.size());
    graphicsDescription.pParameters = graphicsParameters.data();
    return serializeAndCreate(graphicsDescription, "display", graphicsRootSignature_);
}

lgp::framework::Status RendererCore::CreatePipelines()
{
    if (variant_ == LabVariant::Starter)
    {
        if (auto status = CreateComputePipeline(*deviceResources_->device(), *globalRootSignature_.Get(),
                                                analyticShader_, "analytic traversal", analyticPipeline_);
            !status)
        {
            return status;
        }
    }

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = graphicsRootSignature_.Get();
    description.VS = vertexShader_.Bytecode();
    description.PS = pixelShader_.Bytecode();
    description.BlendState = blend;
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizer;
    description.DepthStencilState = depth;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1U;
    description.RTVFormats[0] = deviceResources_->back_buffer_format();
    description.SampleDesc.Count = 1U;
    HRESULT const result = deviceResources_->device()->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(graphicsPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState", result,
                                                                "Failed to create the Chapter 33 display pipeline."));
    }
    return {};
}

lgp::framework::Status RendererCore::ValidateStateObjectDescription()
{
    StateObjectDescription description{};
    description.exports = {
        {.name = "RayGenerationMain", .stage = ShaderStage::RayGeneration},
        {.name = "MissBackground", .stage = ShaderStage::Miss},
        {.name = "MissVoid", .stage = ShaderStage::Miss},
        {.name = "ClosestHitMain", .stage = ShaderStage::ClosestHit},
    };
    description.hitGroups = {{
        .name = "TriangleHitGroup",
        .type = HitGroupType::Triangles,
        .closestHitExport = "ClosestHitMain",
        .anyHitExport = {},
        .intersectionExport = {},
    }};
    description.shaderConfig = {.maxPayloadSizeBytes = kPayloadSizeBytes, .maxAttributeSizeBytes = kAttributeSizeBytes};
    description.pipelineConfig = {.maxTraceRecursionDepth = kTraceRecursionDepth};
    description.localRootSignatures = {{
        .name = "HitGroupLocal",
        .rootArgumentSizeBytes = kHitGroupLocalArgumentSizeBytes,
    }};
    description.associations = {{
        .localRootSignatureName = "HitGroupLocal",
        .exportNames = {"TriangleHitGroup"},
    }};

    auto validation = ValidateStateObject(description);
    if (!validation)
    {
        return std::unexpected(LabContractFailure("ValidateStateObject", validation.error()));
    }

    // The payload the HLSL declares, field for field. The contract lays it out the way HLSL would and reports the
    // size the shader config has to declare, so `MaxPayloadSizeInBytes` is derived rather than guessed.
    std::array<PayloadFieldDescription, 14U> const payloadFields{{
        {.name = "status", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "instanceIndex", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "instanceId", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "primitiveIndex", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "hitKind", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "hitGroupRecordIndex", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "materialId", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "missShaderIndex", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "tHit", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "barycentricB1", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "barycentricB2", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "colorR", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "colorG", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
        {.name = "colorB", .scalarSizeBytes = 4U, .scalarAlignmentBytes = 4U, .elementCount = 1U},
    }};
    auto payload = ValidatePayloadLayout(payloadFields, kMaximumPayloadSizeBytes);
    if (!payload)
    {
        return std::unexpected(LabContractFailure("ValidatePayloadLayout", payload.error()));
    }
    if (payload->sizeBytes != kPayloadSizeBytes)
    {
        return std::unexpected(lgp::framework::MakeError("ValidatePayloadLayout",
                                                         "Chapter 33 declares a " + std::to_string(kPayloadSizeBytes) +
                                                             " byte payload and the field layout needs " +
                                                             std::to_string(payload->sizeBytes) + "."));
    }

    stateObjectEvidence_ = {};
    stateObjectEvidence_.validation = std::move(*validation);
    stateObjectEvidence_.payload = std::move(*payload);
    stateObjectEvidence_.exportCount = static_cast<std::uint32_t>(description.exports.size());
    stateObjectEvidence_.hitGroupCount = static_cast<std::uint32_t>(description.hitGroups.size());

    // Whether the local root signature is really associated with the hit group and only the hit group is a fact
    // the contract's validation already carries: it resolves each export's association and reports the local root
    // argument size the resulting shader-table entry needs. Reading that back is evidence; assigning `true`
    // because the description asked for an association would be a restatement of the request.
    bool associated =
        stateObjectEvidence_.validation.maximumLocalRootArgumentSizeBytes == kHitGroupLocalArgumentSizeBytes;
    std::uint32_t associatedHitGroupEntries = 0U;
    for (ShaderTableEntryRequirement const &entry : stateObjectEvidence_.validation.entries)
    {
        if (entry.section == ShaderTableSectionKind::HitGroup)
        {
            associated = associated && entry.localRootArgumentSizeBytes == kHitGroupLocalArgumentSizeBytes &&
                         entry.minimumRecordStrideBytes >= kShaderIdentifierSizeBytes + kHitGroupLocalArgumentSizeBytes;
            ++associatedHitGroupEntries;
        }
        else
        {
            // A ray generation or miss export that acquired local root arguments would mean the association named
            // more exports than the chapter intended, and its records would silently grow.
            associated = associated && entry.localRootArgumentSizeBytes == 0U;
        }
    }
    stateObjectEvidence_.localRootSignatureAssociated =
        associated && associatedHitGroupEntries == stateObjectEvidence_.hitGroupCount &&
        associatedHitGroupEntries != 0U;
    return {};
}

lgp::framework::Status RendererCore::CreateStateObject()
{
    if (variant_ != LabVariant::Solution || !capability_.dispatchable)
    {
        return {};
    }
    if (auto status = ValidateStateObjectDescription(); !status)
    {
        return status;
    }

    // Every pointer below has to stay valid until `CreateStateObject` returns, which is why the descriptions are
    // locals in one function rather than temporaries built inside the subobject array.
    std::array<D3D12_EXPORT_DESC, 4U> exports{{
        {kRayGenerationExport, nullptr, D3D12_EXPORT_FLAG_NONE},
        {kMissBackgroundExport, nullptr, D3D12_EXPORT_FLAG_NONE},
        {kMissVoidExport, nullptr, D3D12_EXPORT_FLAG_NONE},
        {kClosestHitExport, nullptr, D3D12_EXPORT_FLAG_NONE},
    }};
    D3D12_DXIL_LIBRARY_DESC library{};
    library.DXILLibrary = raytracingLibrary_.Bytecode();
    library.NumExports = static_cast<UINT>(exports.size());
    library.pExports = exports.data();

    D3D12_HIT_GROUP_DESC hitGroup{};
    hitGroup.HitGroupExport = kHitGroupExport;
    hitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroup.ClosestHitShaderImport = kClosestHitExport;

    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
    shaderConfig.MaxPayloadSizeInBytes = kPayloadSizeBytes;
    shaderConfig.MaxAttributeSizeInBytes = kAttributeSizeBytes;

    // The DXR 1.0 pipeline config, not CONFIG1: this chapter declares a recursion depth and nothing else, and a
    // subobject that carries optional 1.1 flags would be a promise the lab does not need to make.
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
    pipelineConfig.MaxTraceRecursionDepth = kTraceRecursionDepth;

    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSignature{};
    globalRootSignature.pGlobalRootSignature = globalRootSignature_.Get();

    D3D12_LOCAL_ROOT_SIGNATURE localRootSignature{};
    localRootSignature.pLocalRootSignature = hitGroupLocalRootSignature_.Get();

    std::array<D3D12_STATE_SUBOBJECT, 7U> subobjects{{
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
        {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hitGroup},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRootSignature},
        {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &localRootSignature},
        {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, nullptr},
    }};

    // The association names the subobject it applies to by pointer, so it has to be filled in after the array
    // exists. Associating the local root signature with the hit group and nothing else is what makes the record
    // stride of the hit-group section different from the miss section's.
    wchar_t const *associatedExport = kHitGroupExport;
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{};
    association.pSubobjectToAssociate = &subobjects[5];
    association.NumExports = 1U;
    association.pExports = &associatedExport;
    subobjects[6].pDesc = &association;

    D3D12_STATE_OBJECT_DESC description{};
    description.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    description.NumSubobjects = static_cast<UINT>(subobjects.size());
    description.pSubobjects = subobjects.data();

    HRESULT const result = deviceResources_->device()->CreateStateObject(
        &description, IID_PPV_ARGS(stateObject_.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        capability_.status = RaytracingSupportStatus::StateObjectUnavailable;
        capability_.dispatchable = false;
        return std::unexpected(lgp::framework::MakeHResultError(
            "ID3D12Device5::CreateStateObject", result,
            "Failed to create the Chapter 33 raytracing state object on an adapter that reported raytracing tier " +
                std::to_string(capability_.reportedTier) + "."));
    }

    ComPtr<ID3D12StateObjectProperties> properties{};
    if (FAILED(stateObject_.As(&properties)))
    {
        capability_.status = RaytracingSupportStatus::StateObjectUnavailable;
        capability_.dispatchable = false;
        stateObject_.Reset();
        return std::unexpected(
            lgp::framework::MakeError("ID3D12StateObject::QueryInterface",
                                      "The Chapter 33 state object exposes no ID3D12StateObjectProperties."));
    }

    // Shader identifiers are only meaningful while the state object that produced them is alive, so they are
    // copied here, once, and every frame's table is written from the copies.
    auto const copyIdentifier = [&properties](wchar_t const *exportName,
                                              std::array<std::byte, kShaderIdentifierSizeBytes> &destination)
        -> std::expected<void, lgp::framework::Error>
    {
        void const *identifier = properties->GetShaderIdentifier(exportName);
        if (identifier == nullptr)
        {
            return std::unexpected(lgp::framework::MakeError("ID3D12StateObjectProperties::GetShaderIdentifier",
                                                             "A Chapter 33 export has no shader identifier."));
        }
        std::memcpy(destination.data(), identifier, destination.size());
        auto const validated = MakeShaderIdentifier(std::span<std::byte const>{destination});
        if (!validated)
        {
            return std::unexpected(LabContractFailure("MakeShaderIdentifier", validated.error()));
        }
        return {};
    };

    if (auto status = copyIdentifier(kRayGenerationExport, rayGenerationIdentifier_); !status)
    {
        capability_.status = RaytracingSupportStatus::ShaderIdentifierUnavailable;
        capability_.dispatchable = false;
        return std::unexpected(std::move(status.error()));
    }
    if (auto status = copyIdentifier(kMissBackgroundExport, missIdentifiers_[0]); !status)
    {
        capability_.status = RaytracingSupportStatus::ShaderIdentifierUnavailable;
        capability_.dispatchable = false;
        return std::unexpected(std::move(status.error()));
    }
    if (auto status = copyIdentifier(kMissVoidExport, missIdentifiers_[1]); !status)
    {
        capability_.status = RaytracingSupportStatus::ShaderIdentifierUnavailable;
        capability_.dispatchable = false;
        return std::unexpected(std::move(status.error()));
    }
    if (auto status = copyIdentifier(kHitGroupExport, hitGroupIdentifier_); !status)
    {
        capability_.status = RaytracingSupportStatus::ShaderIdentifierUnavailable;
        capability_.dispatchable = false;
        return std::unexpected(std::move(status.error()));
    }

    // The table layout is the contract's, not this file's: sections at 64-byte aligned starts, strides set by the
    // widest record in each section and rounded up to 32 bytes, and every addition checked.
    std::vector<ShaderTableSectionRequest> sections{};
    ShaderIdentifier rayGeneration{};
    std::memcpy(rayGeneration.bytes.data(), rayGenerationIdentifier_.data(), rayGeneration.bytes.size());
    sections.push_back({
        .kind = ShaderTableSectionKind::RayGeneration,
        .records = {{.identifier = rayGeneration, .localRootArgumentSizeBytes = 0U}},
    });

    ShaderTableSectionRequest missSection{};
    missSection.kind = ShaderTableSectionKind::Miss;
    for (std::size_t index = 0U; index < kMissRecordCount; ++index)
    {
        ShaderIdentifier identifier{};
        std::memcpy(identifier.bytes.data(), missIdentifiers_[index].data(), identifier.bytes.size());
        missSection.records.push_back({.identifier = identifier, .localRootArgumentSizeBytes = 0U});
    }
    sections.push_back(std::move(missSection));

    ShaderTableSectionRequest hitGroupSection{};
    hitGroupSection.kind = ShaderTableSectionKind::HitGroup;
    ShaderIdentifier hitGroupIdentifier{};
    std::memcpy(hitGroupIdentifier.bytes.data(), hitGroupIdentifier_.data(), hitGroupIdentifier.bytes.size());
    for (std::uint32_t index = 0U; index < kHitGroupRecordCount; ++index)
    {
        hitGroupSection.records.push_back({
            .identifier = hitGroupIdentifier,
            .localRootArgumentSizeBytes = kHitGroupLocalArgumentSizeBytes,
        });
    }
    sections.push_back(std::move(hitGroupSection));

    auto layout = BuildShaderTableLayout(sections);
    if (!layout)
    {
        return std::unexpected(LabContractFailure("BuildShaderTableLayout", layout.error()));
    }
    shaderTableLayout_ = std::move(*layout);

    // The table the association implies, checked against the table that was actually laid out: the hit-group
    // records have to be wide enough for the identifier plus the local arguments, and the miss records - which the
    // association deliberately excludes - have to be exactly one identifier wide. A stride that disagrees means
    // the association reached exports it should not have, or none at all.
    auto const hitGroupLayout = FindShaderTableSection(shaderTableLayout_, ShaderTableSectionKind::HitGroup);
    auto const missLayout = FindShaderTableSection(shaderTableLayout_, ShaderTableSectionKind::Miss);
    if (!hitGroupLayout || !missLayout)
    {
        return std::unexpected(LabContractFailure("FindShaderTableSection", ContractError::ShaderTableSectionMissing));
    }
    stateObjectEvidence_.localRootSignatureAssociated =
        stateObjectEvidence_.localRootSignatureAssociated &&
        hitGroupLayout->strideBytes >= kShaderIdentifierSizeBytes + kHitGroupLocalArgumentSizeBytes &&
        missLayout->strideBytes == kShaderIdentifierSizeBytes;

    stateObjectEvidence_.subobjectCount = static_cast<std::uint32_t>(subobjects.size());
    stateObjectEvidence_.created = true;
    return {};
}

lgp::framework::Status RendererCore::CreateSlotDescriptors(FrameSlotResources &slot)
{
    bool const raytracing = variant_ == LabVariant::Solution && capability_.dispatchable;
    auto allocation = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(raytracing ? 2U : 1U);
    if (!allocation)
    {
        return std::unexpected(std::move(allocation.error()));
    }
    slot.descriptors = *allocation;

    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.Buffer.NumElements = static_cast<UINT>(slot.rayRecords.size_in_bytes() / sizeof(RayRecord));
    description.Buffer.StructureByteStride = sizeof(RayRecord);
    deviceResources_->device()->CreateShaderResourceView(slot.rayRecords.Get(), &description,
                                                         slot.descriptors.CpuHandle(kRecordsSrvIndex));

    if (!raytracing)
    {
        return {};
    }

    // An acceleration-structure view names its structure by GPU virtual address and has no resource pointer, which
    // is why the location is the only field that matters. A location of zero would be D3D12's defined null
    // acceleration structure; this lab never creates one, because WARP 1.0.20 removes the device rather than
    // missing every ray when a dispatch reaches it.
    D3D12_SHADER_RESOURCE_VIEW_DESC sceneDescription{};
    sceneDescription.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    sceneDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sceneDescription.Format = DXGI_FORMAT_UNKNOWN;
    sceneDescription.RaytracingAccelerationStructure.Location = slot.topLevel.gpu_virtual_address();
    deviceResources_->device()->CreateShaderResourceView(nullptr, &sceneDescription,
                                                         slot.descriptors.CpuHandle(kSceneSrvIndex));
    return {};
}

lgp::framework::Status RendererCore::UploadShaderTable(FrameSlotResources &slot)
{
    if (variant_ != LabVariant::Solution || !capability_.dispatchable)
    {
        return {};
    }
    std::byte *const table = slot.shaderTable.mapped_data();
    if (table == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("UploadShaderTable", "The Chapter 33 shader table is not mapped."));
    }
    std::memset(table, 0, static_cast<std::size_t>(slot.shaderTable.size_in_bytes()));

    auto const section = [this](ShaderTableSectionKind kind) -> std::expected<ShaderTableSectionLayout, ContractError>
    { return FindShaderTableSection(shaderTableLayout_, kind); };

    auto rayGeneration = section(ShaderTableSectionKind::RayGeneration);
    auto miss = section(ShaderTableSectionKind::Miss);
    auto hitGroups = section(ShaderTableSectionKind::HitGroup);
    if (!rayGeneration || !miss || !hitGroups)
    {
        return std::unexpected(LabContractFailure("FindShaderTableSection", ContractError::ShaderTableSectionMissing));
    }

    std::memcpy(table + rayGeneration->records[0].offsetBytes, rayGenerationIdentifier_.data(),
                rayGenerationIdentifier_.size());
    for (std::size_t index = 0U; index < miss->records.size(); ++index)
    {
        std::memcpy(table + miss->records[index].offsetBytes, missIdentifiers_[index].data(),
                    missIdentifiers_[index].size());
    }

    std::span<HitGroupLocalConstants const> const materials = MaterialTable();
    for (std::size_t index = 0U; index < hitGroups->records.size(); ++index)
    {
        ShaderRecordLayout const &record = hitGroups->records[index];
        std::memcpy(table + record.offsetBytes, hitGroupIdentifier_.data(), hitGroupIdentifier_.size());
        std::memcpy(table + record.localRootArgumentOffsetBytes, &materials[index], sizeof(HitGroupLocalConstants));
    }
    return {};
}

lgp::framework::Status RendererCore::UploadScene(FrameSlotResources &slot, LabConfiguration const &configuration)
{
    std::vector<InstanceRecord> const instances = BuildInstances(configuration.scene);
    lastInstances_ = instances;

    if (std::byte *const destination = slot.instances.mapped_data(); destination != nullptr)
    {
        std::memcpy(destination, instances.data(), instances.size() * sizeof(InstanceRecord));
    }
    else
    {
        return std::unexpected(
            lgp::framework::MakeError("UploadScene", "The Chapter 33 instance table is not mapped."));
    }

    if (!slot.structuresInitialized)
    {
        std::span<VertexPosition const> const vertices = GeometryVertices();
        std::span<HitGroupLocalConstants const> const materials = MaterialTable();
        if (slot.vertices.mapped_data() == nullptr || slot.materials.mapped_data() == nullptr)
        {
            return std::unexpected(
                lgp::framework::MakeError("UploadScene", "The Chapter 33 geometry tables are not mapped."));
        }
        std::memcpy(slot.vertices.mapped_data(), vertices.data(), vertices.size_bytes());
        std::memcpy(slot.materials.mapped_data(), materials.data(), materials.size_bytes());
        if (auto status = UploadShaderTable(slot); !status)
        {
            return status;
        }
        slot.structuresInitialized = true;
    }

    if (variant_ != LabVariant::Solution)
    {
        return {};
    }

    std::vector<InstanceDescription> const descriptions =
        BuildInstanceDescriptions(configuration.scene, slot.bottomLevel.gpu_virtual_address());
    auto const packing = ValidateInstances(descriptions);
    if (!packing)
    {
        return std::unexpected(LabContractFailure("ValidateInstances", packing.error()));
    }
    BufferRange const instanceBuffer{
        .address = slot.instanceDescriptions.gpu_virtual_address(),
        .sizeBytes = slot.instanceDescriptions.size_in_bytes(),
    };
    if (auto const validated = ValidateInstanceBuffer(instanceBuffer, static_cast<std::uint32_t>(descriptions.size()));
        !validated)
    {
        return std::unexpected(LabContractFailure("ValidateInstanceBuffer", validated.error()));
    }
    lastAccelerationEvidence_.packing = *packing;
    lastAccelerationEvidence_.instanceBufferAddress = instanceBuffer.address;

    std::byte *const destination = slot.instanceDescriptions.mapped_data();
    if (destination == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("UploadScene", "The Chapter 33 instance description buffer is not mapped."));
    }
    for (std::size_t index = 0U; index < descriptions.size(); ++index)
    {
        InstanceDescription const &source = descriptions[index];
        auto const packed = PackInstanceHeader(source);
        if (!packed)
        {
            return std::unexpected(LabContractFailure("PackInstanceHeader", packed.error()));
        }
        D3D12_RAYTRACING_INSTANCE_DESC instance{};
        for (std::size_t row = 0U; row < 3U; ++row)
        {
            for (std::size_t column = 0U; column < 4U; ++column)
            {
                instance.Transform[row][column] = static_cast<FLOAT>(source.objectToWorld.values[row * 4U + column]);
            }
        }
        instance.InstanceID = source.instanceId;
        instance.InstanceMask = source.instanceMask;
        instance.InstanceContributionToHitGroupIndex = source.instanceContributionToHitGroupIndex;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        instance.AccelerationStructure = source.bottomLevelAddress;
        std::memcpy(destination + index * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), &instance, sizeof(instance));
    }
    return {};
}

D3D12_BUFFER_BARRIER RendererCore::TrackBufferBarrier(LabResource const resource, ID3D12Resource &d3dResource,
                                                      BufferBarrierState const before, BufferBarrierState const after)
{
    lastBarriers_.push_back({
        .resource = resource,
        .syncBefore = static_cast<std::uint32_t>(before.sync),
        .syncAfter = static_cast<std::uint32_t>(after.sync),
        .accessBefore = static_cast<std::uint32_t>(before.access),
        .accessAfter = static_cast<std::uint32_t>(after.access),
    });
    return MakeBufferBarrier(d3dResource, before, after);
}

void RendererCore::RecordCounterReset(ID3D12GraphicsCommandList7 &commandList, FrameSlotResources &slot)
{
    BufferBarrierState const dispatchState =
        variant_ == LabVariant::Solution ? RaytracingUnorderedAccessState() : ComputeUnorderedAccessState();
    // The last thing that touched this slot's counters was the readback copy at the end of the previous frame that
    // used the slot, so the access this frame is transitioning away from is `COPY_SOURCE` and not the unordered
    // access the dispatch left. Naming the dispatch state here would be a barrier that describes a state the
    // resource has not been in since two frames ago.
    BufferBarrierState const before = slot.countersInitialized ? CopySourceState() : NoAccessState();
    std::vector<D3D12_BUFFER_BARRIER> barriers{
        TrackBufferBarrier(LabResource::FrameCounters, *slot.frameCounters.Get(), before, CopyDestinationState()),
    };
    SubmitBufferBarriers(commandList, barriers);
    commandList.CopyBufferRegion(slot.frameCounters.Get(), 0U, slot.frameCountersZero.Get(), 0U,
                                 slot.frameCounters.size_in_bytes());
    barriers = {
        TrackBufferBarrier(LabResource::FrameCounters, *slot.frameCounters.Get(), CopyDestinationState(),
                           dispatchState),
    };
    SubmitBufferBarriers(commandList, barriers);
    slot.countersInitialized = true;
}

namespace
{

[[nodiscard]] D3D12_RAYTRACING_GEOMETRY_DESC MakeGeometryDescription(std::uint64_t const vertexAddress) noexcept
{
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    // The geometry is opaque, so no any-hit shader can run for it and the hit group needs no any-hit export. That
    // is a promise about the geometry, not a performance hint.
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.Transform3x4 = 0U;
    geometry.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.IndexCount = 0U;
    geometry.Triangles.VertexCount = kVertexCount;
    geometry.Triangles.IndexBuffer = 0U;
    geometry.Triangles.VertexBuffer.StartAddress = vertexAddress;
    geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(VertexPosition);
    return geometry;
}

constexpr D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS kBottomLevelBuildFlags =
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
// The top-level structure declares `ALLOW_UPDATE` at build time because a structure that was not built with it can
// never be refitted: the permission is chosen when the driver picks the data layout, not when the refit is asked
// for.
constexpr D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS kTopLevelBuildFlags =
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

constexpr BuildFlags kBottomLevelContractFlags = BuildFlags::PreferFastTrace;
constexpr BuildFlags kTopLevelContractFlags = BuildFlags::PreferFastTrace | BuildFlags::AllowUpdate;

[[nodiscard]] PrebuildInfo ToReportedPrebuild(
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO const &prebuild) noexcept
{
    // The driver's three numbers, copied and not otherwise touched. Everything downstream that adjusts a size
    // does so into a separate value, because a diagnostic that prints an adjusted number as if the driver said it
    // is worse than no diagnostic at all.
    return {
        .resultDataMaxSizeBytes = prebuild.ResultDataMaxSizeInBytes,
        .scratchDataSizeBytes = prebuild.ScratchDataSizeInBytes,
        .updateScratchDataSizeBytes = prebuild.UpdateScratchDataSizeInBytes,
    };
}

// The sizes the lab allocates buffers for and validates its build requests against. Two adjustments, both of them
// the lab's own decision and both published beside the untouched report:
//
//   * The result size is rounded up to the acceleration-structure alignment. D3D12 requires the destination
//     *address* to be a multiple of 256 bytes and requires the caller to suballocate accordingly; it does not
//     require the driver's reported size to already be a multiple of 256, and WARP 1.0.20's is not. Packing two
//     structures at the unrounded size would produce a misaligned second structure.
//   * A build without `ALLOW_UPDATE` has no update budget, even if the driver leaves a non-zero value in that
//     report field. The untouched report remains available for diagnostics.
//   * When the build declares `ALLOW_UPDATE`, the refit scratch is raised to at least the build scratch. A driver
//     may report less, including zero, and D3D12 permits that; the chapter refuses to plan around it, because a
//     caller that reserves nothing for a refit and then meets a driver that needs some has an out-of-bounds write
//     with no symptom. The lab allocates the larger of the two either way, so this is the budget the refit is
//     really given rather than a claim about what the driver asked for.
[[nodiscard]] PrebuildInfo ToAllocationBudget(PrebuildInfo const &reported, bool const allowUpdate) noexcept
{
    return {
        .resultDataMaxSizeBytes = AlignUpBytes(reported.resultDataMaxSizeBytes, kAccelerationStructureAlignmentBytes),
        .scratchDataSizeBytes = reported.scratchDataSizeBytes,
        .updateScratchDataSizeBytes =
            allowUpdate ? std::max(reported.updateScratchDataSizeBytes, reported.scratchDataSizeBytes) : 0U,
    };
}

[[nodiscard]] std::string DescribePrebuild(std::string_view name, PrebuildEvidence const &evidence)
{
    return std::string{name} + ": driver reported result=" + std::to_string(evidence.reported.resultDataMaxSizeBytes) +
           " scratch=" + std::to_string(evidence.reported.scratchDataSizeBytes) +
           " updateScratch=" + std::to_string(evidence.reported.updateScratchDataSizeBytes) +
           "; the lab allocated result=" + std::to_string(evidence.allocationBudget.resultDataMaxSizeBytes) +
           " scratch=" + std::to_string(evidence.allocationBudget.scratchDataSizeBytes) +
           " updateScratch=" + std::to_string(evidence.allocationBudget.updateScratchDataSizeBytes);
}

[[nodiscard]] D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO QueryBottomLevelPrebuild(
    ID3D12Device10 &device, D3D12_RAYTRACING_GEOMETRY_DESC const &geometry) noexcept
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = kBottomLevelBuildFlags;
    inputs.NumDescs = kGeometryCount;
    inputs.pGeometryDescs = &geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device.GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    return prebuild;
}

[[nodiscard]] D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO QueryTopLevelPrebuild(
    ID3D12Device10 &device, UINT const instanceCount) noexcept
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags = kTopLevelBuildFlags;
    inputs.NumDescs = instanceCount;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device.GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    return prebuild;
}

} // namespace

lgp::framework::Status RendererCore::CreateFrameSlotResources(lgp::framework::Extent2D const size)
{
    if (size.empty())
    {
        return std::unexpected(
            lgp::framework::MakeError("CreateFrameSlotResources", "Chapter 33 needs a non-empty drawable size."));
    }
    // The ray grid is bounded so that a headless WARP frame reads back quickly. A larger window magnifies the grid
    // rather than tracing more rays, which keeps the scene - and every number a test asserts - identical.
    size_.width = std::clamp(size.width, kMinimumWidth, kMaximumWidth);
    size_.height = std::clamp(size.height, kMinimumHeight, kMaximumHeight);

    ID3D12Device10 &device = *deviceResources_->device();
    std::uint32_t const slotCount = deviceResources_->back_buffer_count();
    frameSlots_.clear();
    frameSlots_.resize(slotCount);

    std::uint64_t const recordBytes =
        static_cast<std::uint64_t>(size_.width) * static_cast<std::uint64_t>(size_.height) * sizeof(RayRecord);

    for (std::uint32_t slotIndex = 0U; slotIndex < slotCount; ++slotIndex)
    {
        FrameSlotResources &slot = frameSlots_[slotIndex];
        std::wstring const suffix = L" (slot " + std::to_wstring(slotIndex) + L")";

        auto const makeBuffer = [&device, &suffix](BufferResource &destination, std::uint64_t bytes,
                                                   D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
                                                   std::wstring_view name, bool mapped) -> lgp::framework::Status
        {
            auto buffer = CreateBuffer(device, bytes, heapType, flags, std::wstring{name} + suffix, mapped);
            if (!buffer)
            {
                return std::unexpected(std::move(buffer.error()));
            }
            destination = std::move(*buffer);
            return {};
        };

        if (auto status = makeBuffer(slot.vertices, GeometryVertices().size_bytes(), D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_FLAG_NONE, L"Ch33 Vertices", true);
            !status)
        {
            return status;
        }
        if (auto status = makeBuffer(slot.materials, MaterialTable().size_bytes(), D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_FLAG_NONE, L"Ch33 Materials", true);
            !status)
        {
            return status;
        }
        if (auto status = makeBuffer(slot.instances, kMaximumInstanceCount * sizeof(InstanceRecord),
                                     D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch33 Instances", true);
            !status)
        {
            return status;
        }
        if (auto status = makeBuffer(slot.rayRecords, recordBytes, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch33 RayRecords", false);
            !status)
        {
            return status;
        }
        if (auto status = makeBuffer(slot.frameCounters, sizeof(FrameRecord), D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch33 FrameCounters", false);
            !status)
        {
            return status;
        }
        if (auto status = makeBuffer(slot.frameCountersZero, sizeof(FrameRecord), D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_FLAG_NONE, L"Ch33 FrameCountersZero", true);
            !status)
        {
            return status;
        }
        std::memset(slot.frameCountersZero.mapped_data(), 0, sizeof(FrameRecord));
        if (auto status = makeBuffer(slot.rayRecordsReadback, recordBytes, D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_FLAG_NONE, L"Ch33 RayRecordsReadback", true);
            !status)
        {
            return status;
        }
        if (auto status = makeBuffer(slot.frameCountersReadback, sizeof(FrameRecord), D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_FLAG_NONE, L"Ch33 FrameCountersReadback", true);
            !status)
        {
            return status;
        }

        if (variant_ == LabVariant::Solution && capability_.dispatchable)
        {
            D3D12_RAYTRACING_GEOMETRY_DESC const geometry =
                MakeGeometryDescription(slot.vertices.gpu_virtual_address());
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO const bottomLevel =
                QueryBottomLevelPrebuild(device, geometry);
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO const topLevel =
                QueryTopLevelPrebuild(device, kMaximumInstanceCount);
            if (bottomLevel.ResultDataMaxSizeInBytes == 0U || topLevel.ResultDataMaxSizeInBytes == 0U)
            {
                return std::unexpected(lgp::framework::MakeError(
                    "GetRaytracingAccelerationStructurePrebuildInfo",
                    "The adapter reported a zero-byte acceleration structure, which no build can satisfy."));
            }

            // Acceleration structures and their scratch both need 256-byte aligned addresses. A committed buffer's
            // base is far more aligned than that, and rounding the sizes keeps the whole allocation legal even if
            // a future step suballocates from it.
            std::uint64_t const scratchBytes =
                AlignUpBytes(std::max(topLevel.ScratchDataSizeInBytes, topLevel.UpdateScratchDataSizeInBytes),
                             kAccelerationStructureAlignmentBytes);
            if (auto status = makeBuffer(
                    slot.bottomLevel,
                    AlignUpBytes(bottomLevel.ResultDataMaxSizeInBytes, kAccelerationStructureAlignmentBytes),
                    D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE,
                    L"Ch33 BottomLevel", false);
                !status)
            {
                return status;
            }
            if (auto status =
                    makeBuffer(slot.bottomLevelScratch,
                               AlignUpBytes(bottomLevel.ScratchDataSizeInBytes, kAccelerationStructureAlignmentBytes),
                               D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               L"Ch33 BottomLevelScratch", false);
                !status)
            {
                return status;
            }
            if (auto status = makeBuffer(
                    slot.topLevel,
                    AlignUpBytes(topLevel.ResultDataMaxSizeInBytes, kAccelerationStructureAlignmentBytes),
                    D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE,
                    L"Ch33 TopLevel", false);
                !status)
            {
                return status;
            }
            // The two builds get their own scratch. Sharing one buffer is legal and would need a barrier between
            // the builds; keeping them separate removes that dependency instead of hiding it.
            if (auto status = makeBuffer(slot.topLevelScratch, scratchBytes, D3D12_HEAP_TYPE_DEFAULT,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Ch33 TopLevelScratch", false);
                !status)
            {
                return status;
            }
            if (auto status = makeBuffer(slot.instanceDescriptions,
                                         kMaximumInstanceCount * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                                         D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch33 InstanceDescs", true);
                !status)
            {
                return status;
            }
            if (auto status =
                    makeBuffer(slot.shaderTable, std::max<std::uint64_t>(shaderTableLayout_.totalSizeBytes, 1U),
                               D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, L"Ch33 ShaderTable", true);
                !status)
            {
                return status;
            }
        }

        if (auto status = CreateSlotDescriptors(slot); !status)
        {
            return status;
        }
    }
    return {};
}

lgp::framework::Status RendererCore::RecordAccelerationStructures(ID3D12GraphicsCommandList7 &commandList,
                                                                  FrameSlotResources &slot,
                                                                  LabConfiguration const &configuration)
{
    ID3D12Device10 &device = *deviceResources_->device();
    std::uint32_t const instanceCount = static_cast<std::uint32_t>(AuthoredInstances(configuration.scene).size());

    D3D12_RAYTRACING_GEOMETRY_DESC const geometry = MakeGeometryDescription(slot.vertices.gpu_virtual_address());
    TriangleGeometryDescription const contractGeometry{
        .vertexFormat = VertexFormat::R32G32B32Float,
        .vertexBufferAddress = slot.vertices.gpu_virtual_address(),
        .vertexStrideBytes = sizeof(VertexPosition),
        .vertexCount = kVertexCount,
        .indexFormat = IndexFormat::None,
        .indexBufferAddress = 0U,
        .indexCount = 0U,
        .transform3x4Address = 0U,
        .flags = GeometryFlags::Opaque,
    };
    auto const geometryValidation = ValidateTriangleGeometry(contractGeometry);
    if (!geometryValidation)
    {
        return std::unexpected(LabContractFailure("ValidateTriangleGeometry", geometryValidation.error()));
    }

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO const bottomLevelPrebuild =
        QueryBottomLevelPrebuild(device, geometry);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO const topLevelPrebuild =
        QueryTopLevelPrebuild(device, instanceCount);
    PrebuildEvidence bottomLevelEvidence{};
    bottomLevelEvidence.reported = ToReportedPrebuild(bottomLevelPrebuild);
    bottomLevelEvidence.allocationBudget = ToAllocationBudget(bottomLevelEvidence.reported, false);
    PrebuildEvidence topLevelEvidence{};
    topLevelEvidence.reported = ToReportedPrebuild(topLevelPrebuild);
    topLevelEvidence.allocationBudget = ToAllocationBudget(topLevelEvidence.reported, true);

    // The contract validates the budget the lab allocated, not the driver's report, because that is the triple the
    // buffers below actually satisfy. Both are published, so a failure names which of the two is at fault.
    if (auto const validated = ValidatePrebuildInfo(kBottomLevelContractFlags, bottomLevelEvidence.allocationBudget);
        !validated)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidatePrebuildInfo", "Chapter 33 refused its bottom-level allocation budget: " +
                                        std::string{ContractErrorName(validated.error())} + ". " +
                                        DescribePrebuild("bottom level", bottomLevelEvidence) + "."));
    }
    if (auto const validated = ValidatePrebuildInfo(kTopLevelContractFlags, topLevelEvidence.allocationBudget);
        !validated)
    {
        return std::unexpected(lgp::framework::MakeError(
            "ValidatePrebuildInfo",
            "Chapter 33 refused its top-level allocation budget: " + std::string{ContractErrorName(validated.error())} +
                ". " + DescribePrebuild("top level", topLevelEvidence) + "."));
    }

    // A refit is only legal when this slot already holds a structure that was built with `ALLOW_UPDATE` and with
    // the same instance count. When the configuration asks for one and the slot cannot supply it - the first frame
    // in a slot, or a scene change - the frame rebuilds and says so, because a ledger that claimed a refit here
    // would be the exact kind of comfortable lie this chapter exists to remove.
    bool const updateRequested = configuration.topLevelBuild == TopLevelBuildMode::Update;
    bool const canUpdate = slot.topLevelBuilt && slot.topLevelInstanceCount == instanceCount;
    bool const performUpdate = updateRequested && canUpdate;

    AccelerationStructureBuildRequest bottomLevelRequest{
        .kind = AccelerationStructureKind::BottomLevel,
        .mode = BuildMode::Build,
        .flags = kBottomLevelContractFlags,
        .prebuild = bottomLevelEvidence.allocationBudget,
        .destination = {slot.bottomLevel.gpu_virtual_address(), slot.bottomLevel.size_in_bytes()},
        .scratch = {slot.bottomLevelScratch.gpu_virtual_address(), slot.bottomLevelScratch.size_in_bytes()},
        .source = {},
        .elementCount = kGeometryCount,
        .sourceElementCount = 0U,
        .sourceFlags = BuildFlags::None,
    };
    auto const bottomLevelPlan = ValidateBuildRequest(bottomLevelRequest);
    if (!bottomLevelPlan)
    {
        return std::unexpected(LabContractFailure("ValidateBuildRequest", bottomLevelPlan.error()));
    }

    AccelerationStructureBuildRequest topLevelRequest{
        .kind = AccelerationStructureKind::TopLevel,
        .mode = performUpdate ? BuildMode::Update : BuildMode::Build,
        .flags = kTopLevelContractFlags,
        .prebuild = topLevelEvidence.allocationBudget,
        .destination = {slot.topLevel.gpu_virtual_address(), slot.topLevel.size_in_bytes()},
        .scratch = {slot.topLevelScratch.gpu_virtual_address(), slot.topLevelScratch.size_in_bytes()},
        .source = performUpdate ? BufferRange{slot.topLevel.gpu_virtual_address(), slot.topLevel.size_in_bytes()}
                                : BufferRange{},
        .elementCount = instanceCount,
        .sourceElementCount = performUpdate ? slot.topLevelInstanceCount : 0U,
        .sourceFlags = performUpdate ? kTopLevelContractFlags : BuildFlags::None,
    };
    auto const topLevelPlan = ValidateBuildRequest(topLevelRequest);
    if (!topLevelPlan)
    {
        return std::unexpected(LabContractFailure("ValidateBuildRequest", topLevelPlan.error()));
    }

    // The bottom-level structure is written by this frame and was read by the previous frame that used this slot,
    // so the reuse is a write-after-read that gets its own barrier. It is separate from the build/consume barrier
    // below, and the published list keeps the two distinguishable.
    std::vector<D3D12_BUFFER_BARRIER> barriers{
        TrackBufferBarrier(LabResource::BottomLevelStructure, *slot.bottomLevel.Get(),
                           slot.topLevelBuilt ? BuildReadState() : NoAccessState(), BuildWriteState()),
        TrackBufferBarrier(LabResource::BottomLevelScratch, *slot.bottomLevelScratch.Get(),
                           slot.topLevelBuilt ? BuildScratchState() : NoAccessState(), BuildScratchState()),
    };
    SubmitBufferBarriers(commandList, barriers);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottomLevelBuild{};
    bottomLevelBuild.DestAccelerationStructureData = slot.bottomLevel.gpu_virtual_address();
    bottomLevelBuild.ScratchAccelerationStructureData = slot.bottomLevelScratch.gpu_virtual_address();
    bottomLevelBuild.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottomLevelBuild.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bottomLevelBuild.Inputs.Flags = kBottomLevelBuildFlags;
    bottomLevelBuild.Inputs.NumDescs = kGeometryCount;
    bottomLevelBuild.Inputs.pGeometryDescs = &geometry;
    commandList.BuildRaytracingAccelerationStructure(&bottomLevelBuild, 0U, nullptr);
    lastBuildTimeline_.push_back({
        .kind = BuildStepKind::BuildBottomLevel,
        .resourceId = static_cast<std::uint64_t>(LabResource::BottomLevelStructure),
        .scratchId = static_cast<std::uint64_t>(LabResource::BottomLevelScratch),
        .inputs = {},
    });

    // The required build barrier. Without it the top-level build may read a bottom-level structure the driver has
    // not finished writing, and nothing in D3D12 will say so: the symptom is an image that is intermittently
    // wrong.
    barriers = {
        TrackBufferBarrier(LabResource::BottomLevelStructure, *slot.bottomLevel.Get(), BuildWriteState(),
                           BuildReadState()),
    };
    SubmitBufferBarriers(commandList, barriers);
    lastBuildTimeline_.push_back({
        .kind = BuildStepKind::UavBarrier,
        .resourceId = static_cast<std::uint64_t>(LabResource::BottomLevelStructure),
        .scratchId = 0U,
        .inputs = {},
    });

    barriers = {
        TrackBufferBarrier(LabResource::TopLevelStructure, *slot.topLevel.Get(),
                           slot.topLevelBuilt ? TraceReadState() : NoAccessState(), BuildWriteState()),
        TrackBufferBarrier(LabResource::TopLevelScratch, *slot.topLevelScratch.Get(),
                           slot.topLevelBuilt ? BuildScratchState() : NoAccessState(), BuildScratchState()),
    };
    SubmitBufferBarriers(commandList, barriers);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topLevelBuild{};
    topLevelBuild.DestAccelerationStructureData = slot.topLevel.gpu_virtual_address();
    topLevelBuild.ScratchAccelerationStructureData = slot.topLevelScratch.gpu_virtual_address();
    topLevelBuild.SourceAccelerationStructureData = performUpdate ? slot.topLevel.gpu_virtual_address() : 0U;
    topLevelBuild.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    topLevelBuild.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    topLevelBuild.Inputs.Flags =
        performUpdate ? (kTopLevelBuildFlags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE)
                      : kTopLevelBuildFlags;
    topLevelBuild.Inputs.NumDescs = instanceCount;
    topLevelBuild.Inputs.InstanceDescs = slot.instanceDescriptions.gpu_virtual_address();
    commandList.BuildRaytracingAccelerationStructure(&topLevelBuild, 0U, nullptr);
    lastBuildTimeline_.push_back({
        .kind = BuildStepKind::BuildTopLevel,
        .resourceId = static_cast<std::uint64_t>(LabResource::TopLevelStructure),
        .scratchId = static_cast<std::uint64_t>(LabResource::TopLevelScratch),
        .inputs = {static_cast<std::uint64_t>(LabResource::BottomLevelStructure)},
    });

    // The required dispatch barrier. It is the only place in the frame where the sync scope changes from the build
    // pipeline to the raytracing pipeline, which is why the two halves of a DXR frame can be reasoned about
    // separately at all.
    barriers = {
        TrackBufferBarrier(LabResource::TopLevelStructure, *slot.topLevel.Get(), BuildWriteState(), TraceReadState()),
    };
    SubmitBufferBarriers(commandList, barriers);
    lastBuildTimeline_.push_back({
        .kind = BuildStepKind::UavBarrier,
        .resourceId = static_cast<std::uint64_t>(LabResource::TopLevelStructure),
        .scratchId = 0U,
        .inputs = {},
    });

    slot.topLevelBuilt = true;
    slot.topLevelInstanceCount = instanceCount;

    lastAccelerationEvidence_.bottomLevelPrebuild = bottomLevelEvidence;
    lastAccelerationEvidence_.topLevelPrebuild = topLevelEvidence;
    lastAccelerationEvidence_.bottomLevelPlan = *bottomLevelPlan;
    lastAccelerationEvidence_.topLevelPlan = *topLevelPlan;
    lastAccelerationEvidence_.bottomLevelAddress = slot.bottomLevel.gpu_virtual_address();
    lastAccelerationEvidence_.bottomLevelScratchAddress = slot.bottomLevelScratch.gpu_virtual_address();
    lastAccelerationEvidence_.topLevelAddress = slot.topLevel.gpu_virtual_address();
    lastAccelerationEvidence_.topLevelScratchAddress = slot.topLevelScratch.gpu_virtual_address();
    lastAccelerationEvidence_.instanceCount = instanceCount;
    lastAccelerationEvidence_.geometryPrimitiveCount = geometryValidation->primitiveCount;
    lastAccelerationEvidence_.topLevelUpdateRequested = updateRequested;
    lastAccelerationEvidence_.topLevelUpdated = performUpdate;
    lastAccelerationEvidence_.scratchBuffersDistinct =
        slot.bottomLevelScratch.gpu_virtual_address() != slot.topLevelScratch.gpu_virtual_address();
    return {};
}

DispatchConstants RendererCore::MakeDispatchConstants(LabConfiguration const &configuration) const noexcept
{
    return {
        .width = size_.width,
        .height = size_.height,
        .instanceCount = static_cast<std::uint32_t>(AuthoredInstances(configuration.scene).size()),
        .instanceInclusionMask = configuration.instanceInclusionMask,
        .rayContributionToHitGroupIndex = configuration.rayContributionToHitGroupIndex,
        .multiplierForGeometryContribution = configuration.multiplierForGeometryContribution,
        .missShaderIndex = configuration.missShaderIndex,
        .rayFlags = MakeHlslRayFlags(configuration),
        .windowHalfExtentX = static_cast<float>(kWindowHalfExtentX),
        .windowHalfExtentY = static_cast<float>(kWindowHalfExtentY),
        .cameraOriginZ = static_cast<float>(kCameraOriginZ),
        .rayTMin = configuration.rayTMin,
        .rayTMax = configuration.rayTMax,
        .hitGroupRecordCount = kHitGroupRecordCount,
        .materialCount = kMaterialCount,
        .frameIndex = configuration.frameIndex,
    };
}

void RendererCore::RecordAnalyticTraversal(ID3D12GraphicsCommandList7 &commandList, FrameSlotResources &slot,
                                           LabConfiguration const &configuration)
{
    std::vector<D3D12_BUFFER_BARRIER> const barriers{
        TrackBufferBarrier(LabResource::RayRecords, *slot.rayRecords.Get(),
                           slot.recordsInitialized ? CopySourceState() : NoAccessState(),
                           ComputeUnorderedAccessState()),
    };
    SubmitBufferBarriers(commandList, barriers);

    DispatchConstants const constants = MakeDispatchConstants(configuration);
    commandList.SetComputeRootSignature(globalRootSignature_.Get());
    commandList.SetComputeRoot32BitConstants(AnalyticConstants, sizeof(constants) / sizeof(std::uint32_t), &constants,
                                             0U);
    commandList.SetComputeRootShaderResourceView(AnalyticInstancesSrv, slot.instances.gpu_virtual_address());
    commandList.SetComputeRootShaderResourceView(AnalyticVerticesSrv, slot.vertices.gpu_virtual_address());
    commandList.SetComputeRootShaderResourceView(AnalyticMaterialsSrv, slot.materials.gpu_virtual_address());
    commandList.SetComputeRootUnorderedAccessView(AnalyticRecordsUav, slot.rayRecords.gpu_virtual_address());
    commandList.SetComputeRootUnorderedAccessView(AnalyticCountersUav, slot.frameCounters.gpu_virtual_address());
    commandList.SetPipelineState(analyticPipeline_.Get());
    commandList.Dispatch((size_.width + kGroupWidth - 1U) / kGroupWidth,
                         (size_.height + kGroupHeight - 1U) / kGroupHeight, 1U);
    slot.recordsInitialized = true;
}

lgp::framework::Status RendererCore::RecordRayDispatch(ID3D12GraphicsCommandList7 &commandList,
                                                       FrameSlotResources &slot, LabConfiguration const &configuration)
{
    std::vector<D3D12_BUFFER_BARRIER> const barriers{
        TrackBufferBarrier(LabResource::RayRecords, *slot.rayRecords.Get(),
                           slot.recordsInitialized ? CopySourceState() : NoAccessState(),
                           RaytracingUnorderedAccessState()),
    };
    SubmitBufferBarriers(commandList, barriers);

    std::uint64_t const tableBase = slot.shaderTable.gpu_virtual_address();
    DispatchDimensions const dimensions{.width = size_.width, .height = size_.height, .depth = 1U};
    auto const dispatchDescription = MakeDispatchRaysDescription(shaderTableLayout_, tableBase, dimensions);
    if (!dispatchDescription)
    {
        return std::unexpected(LabContractFailure("MakeDispatchRaysDescription", dispatchDescription.error()));
    }

    lastShaderTableEvidence_ = {};
    lastShaderTableEvidence_.layout = shaderTableLayout_;
    lastShaderTableEvidence_.dispatchDescription = *dispatchDescription;
    lastShaderTableEvidence_.baseAddress = tableBase;
    lastShaderTableEvidence_.totalSizeBytes = shaderTableLayout_.totalSizeBytes;
    lastShaderTableEvidence_.rayGenerationRecordAddress = dispatchDescription->rayGeneration.startAddress;
    lastShaderTableEvidence_.identifierByteCount = static_cast<std::uint32_t>(kShaderIdentifierSizeBytes);
    lastShaderTableEvidence_.identifiersCopiedWhileStateObjectAlive = stateObject_ != nullptr;
    for (std::uint32_t index = 0U; index < kMissRecordCount; ++index)
    {
        auto const address = ResolveMissRecordAddress(shaderTableLayout_, tableBase, index);
        if (!address)
        {
            return std::unexpected(LabContractFailure("ResolveMissRecordAddress", address.error()));
        }
        lastShaderTableEvidence_.missRecordAddresses[index] = *address;
    }

    std::vector<AuthoredInstance> const authored = AuthoredInstances(configuration.scene);
    for (std::size_t index = 0U; index < lastShaderTableEvidence_.instanceRecordIndices.size(); ++index)
    {
        if (index >= authored.size())
        {
            lastShaderTableEvidence_.instanceRecordIndices[index] = kInvalidIndex;
            continue;
        }
        HitGroupIndexParameters const parameters{
            .rayContributionToHitGroupIndex = configuration.rayContributionToHitGroupIndex,
            .multiplierForGeometryContributionToHitGroupIndex = configuration.multiplierForGeometryContribution,
            .geometryContributionToHitGroupIndex = 0U,
            .instanceContributionToHitGroupIndex = authored[index].contribution,
        };
        auto const recordIndex = ComputeHitGroupRecordIndex(parameters);
        if (!recordIndex)
        {
            return std::unexpected(LabContractFailure("ComputeHitGroupRecordIndex", recordIndex.error()));
        }
        lastShaderTableEvidence_.instanceRecordIndices[index] = *recordIndex;
        auto const address = ResolveHitGroupRecordAddress(shaderTableLayout_, tableBase, parameters);
        if (!address)
        {
            return std::unexpected(LabContractFailure("ResolveHitGroupRecordAddress", address.error()));
        }
    }
    for (std::uint32_t index = 0U; index < kHitGroupRecordCount; ++index)
    {
        HitGroupIndexParameters const parameters{
            .rayContributionToHitGroupIndex = index,
            .multiplierForGeometryContributionToHitGroupIndex = 0U,
            .geometryContributionToHitGroupIndex = 0U,
            .instanceContributionToHitGroupIndex = 0U,
        };
        auto const address = ResolveHitGroupRecordAddress(shaderTableLayout_, tableBase, parameters);
        if (!address)
        {
            return std::unexpected(LabContractFailure("ResolveHitGroupRecordAddress", address.error()));
        }
        lastShaderTableEvidence_.hitGroupRecordAddresses[index] = *address;
    }

    DispatchConstants const constants = MakeDispatchConstants(configuration);
    // Raytracing root arguments are compute root arguments. There is no separate raytracing binding point, which
    // is the single most common surprise in DXR bring-up.
    commandList.SetComputeRootSignature(globalRootSignature_.Get());
    commandList.SetComputeRoot32BitConstants(GlobalConstants, sizeof(constants) / sizeof(std::uint32_t), &constants,
                                             0U);
    commandList.SetComputeRootDescriptorTable(GlobalSceneSrv, slot.descriptors.GpuHandle(kSceneSrvIndex));
    commandList.SetComputeRootUnorderedAccessView(GlobalRecordsUav, slot.rayRecords.gpu_virtual_address());
    commandList.SetComputeRootUnorderedAccessView(GlobalCountersUav, slot.frameCounters.gpu_virtual_address());

    D3D12_DISPATCH_RAYS_DESC dispatch{};
    dispatch.RayGenerationShaderRecord.StartAddress = dispatchDescription->rayGeneration.startAddress;
    dispatch.RayGenerationShaderRecord.SizeInBytes = dispatchDescription->rayGeneration.sizeBytes;
    dispatch.MissShaderTable.StartAddress = dispatchDescription->miss.startAddress;
    dispatch.MissShaderTable.SizeInBytes = dispatchDescription->miss.sizeBytes;
    dispatch.MissShaderTable.StrideInBytes = dispatchDescription->miss.strideBytes;
    dispatch.HitGroupTable.StartAddress = dispatchDescription->hitGroup.startAddress;
    dispatch.HitGroupTable.SizeInBytes = dispatchDescription->hitGroup.sizeBytes;
    dispatch.HitGroupTable.StrideInBytes = dispatchDescription->hitGroup.strideBytes;
    dispatch.Width = dispatchDescription->dimensions.width;
    dispatch.Height = dispatchDescription->dimensions.height;
    dispatch.Depth = dispatchDescription->dimensions.depth;

    commandList.SetPipelineState1(stateObject_.Get());
    commandList.DispatchRays(&dispatch);
    lastBuildTimeline_.push_back({
        .kind = BuildStepKind::DispatchRays,
        .resourceId = 0U,
        .scratchId = 0U,
        .inputs = {static_cast<std::uint64_t>(LabResource::TopLevelStructure)},
    });
    slot.recordsInitialized = true;
    return {};
}

void RendererCore::RecordDisplayAndReadback(lgp::framework::FrameContext const &frameContext, FrameSlotResources &slot,
                                            LabConfiguration const &configuration,
                                            std::span<LabStage const> const stages)
{
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    BufferBarrierState const dispatchState =
        variant_ == LabVariant::Solution ? RaytracingUnorderedAccessState() : ComputeUnorderedAccessState();

    std::vector<D3D12_BUFFER_BARRIER> barriers{
        TrackBufferBarrier(LabResource::RayRecords, *slot.rayRecords.Get(), dispatchState, PixelShaderResourceState()),
        TrackBufferBarrier(LabResource::FrameCounters, *slot.frameCounters.Get(), dispatchState, CopySourceState()),
    };
    SubmitBufferBarriers(commandList, barriers);

    SubmitTextureBarrier(commandList, MakeTextureBarrier(*frameContext.renderTarget, FrameStartState(frameContext),
                                                         RenderTargetState()));

    constexpr std::array<float, 4U> kClearColor{0.01F, 0.01F, 0.018F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, kClearColor.data(), 0U, nullptr);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // The stage list this function is handed already contains `OutputReadback`, because the copies below are the
    // stage: the caller pushes it immediately before calling, so the mask the display draws and the ordered list
    // the readback publishes are the same statement about the same frame. Deriving the mask here from the list,
    // rather than accumulating one alongside it, is what keeps them from drifting apart again.
    std::uint32_t const stageMask = StageMask(stages);
    std::uint32_t const unavailableMask = UnavailableStageMask(variant_);
    std::uint32_t const allStagesMask = (1U << kLabStageCount) - 1U;

    DisplayConstants const display{
        .width = size_.width,
        .height = size_.height,
        .surfaceWidth = frameContext.drawableSize.width,
        .surfaceHeight = frameContext.drawableSize.height,
        .debugView = static_cast<std::uint32_t>(configuration.debugView),
        .variant = static_cast<std::uint32_t>(variant_),
        .stageMask = stageMask,
        .stageSkippedMask = allStagesMask & ~stageMask & ~unavailableMask,
        .stageUnavailableMask = unavailableMask,
        .hitGroupRecordCount = kHitGroupRecordCount,
        .rayTMin = configuration.rayTMin,
        .rayTMax = configuration.rayTMax,
        .hitCount = currentReference_.hitCount,
        .missCount = currentReference_.missCount,
    };
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    commandList.SetGraphicsRoot32BitConstants(GraphicsConstants, sizeof(display) / sizeof(std::uint32_t), &display, 0U);
    commandList.SetGraphicsRootDescriptorTable(GraphicsRecordsTable, slot.descriptors.GpuHandle(kRecordsSrvIndex));
    commandList.SetPipelineState(graphicsPipeline_.Get());
    commandList.DrawInstanced(3U, 1U, 0U, 0U);

    barriers = {
        TrackBufferBarrier(LabResource::RayRecords, *slot.rayRecords.Get(), PixelShaderResourceState(),
                           CopySourceState()),
    };
    SubmitBufferBarriers(commandList, barriers);
    commandList.CopyBufferRegion(slot.rayRecordsReadback.Get(), 0U, slot.rayRecords.Get(), 0U,
                                 slot.rayRecords.size_in_bytes());
    commandList.CopyBufferRegion(slot.frameCountersReadback.Get(), 0U, slot.frameCounters.Get(), 0U,
                                 slot.frameCounters.size_in_bytes());

    SubmitTextureBarrier(
        commandList, MakeTextureBarrier(*frameContext.renderTarget, RenderTargetState(), FrameEndState(frameContext)));
}

lgp::framework::Status RendererCore::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
    interactiveConfiguration_ = DefaultConfiguration(variant_);

    if (auto status = QueryCapability(); !status)
    {
        return status;
    }
    if (auto status = CreateShaders(); !status)
    {
        return status;
    }
    if (auto status = CreateRootSignatures(); !status)
    {
        return status;
    }
    if (auto status = CreatePipelines(); !status)
    {
        return status;
    }
    return CreateStateObject();
}

lgp::framework::Status RendererCore::OnResize(lgp::framework::DeviceResources &deviceResources,
                                              lgp::framework::Extent2D const drawableSize)
{
    DestroyFrameSlotResources(deviceResources);
    return CreateFrameSlotResources(drawableSize);
}

lgp::framework::Status RendererCore::Update(lgp::framework::UpdateContext const &context)
{
    if (!headless_)
    {
        LabConfiguration &configuration = interactiveConfiguration_;
        if (context.input.WasKeyPressed('V'))
        {
            configuration.debugView =
                static_cast<DebugView>((static_cast<std::uint32_t>(configuration.debugView) + 1U) % kDebugViewCount);
        }
        if (context.input.WasKeyPressed('S'))
        {
            configuration.scene =
                static_cast<SceneVariant>((static_cast<std::uint32_t>(configuration.scene) + 1U) % kSceneVariantCount);
        }
        if (context.input.WasKeyPressed('M'))
        {
            configuration.missShaderIndex = (configuration.missShaderIndex + 1U) % kMissRecordCount;
        }
        if (context.input.WasKeyPressed('R'))
        {
            configuration.rayContributionToHitGroupIndex = (configuration.rayContributionToHitGroupIndex + 1U) % 2U;
        }
        if (context.input.WasKeyPressed('1'))
        {
            configuration.instanceInclusionMask = 0xFFU;
        }
        if (context.input.WasKeyPressed('2'))
        {
            configuration.instanceInclusionMask = 0x1U;
        }
        if (context.input.WasKeyPressed('3'))
        {
            configuration.instanceInclusionMask = 0x6U;
        }
        if (context.input.WasKeyPressed('F'))
        {
            if (!configuration.cullBackFacingTriangles && !configuration.cullFrontFacingTriangles)
            {
                configuration.cullBackFacingTriangles = true;
            }
            else if (configuration.cullBackFacingTriangles)
            {
                configuration.cullBackFacingTriangles = false;
                configuration.cullFrontFacingTriangles = true;
            }
            else
            {
                configuration.cullFrontFacingTriangles = false;
            }
        }
        if (context.input.WasKeyPressed('T'))
        {
            // Three intervals, each placed well away from the scene's three surfaces so that what a learner sees is
            // the interval doing its job rather than an implementation's endpoint policy.
            if (configuration.rayTMin == 0.0F && configuration.rayTMax == 16.0F)
            {
                configuration.rayTMin = 1.75F;
            }
            else if (configuration.rayTMin == 1.75F)
            {
                configuration.rayTMin = 0.0F;
                configuration.rayTMax = 2.25F;
            }
            else
            {
                configuration.rayTMin = 0.0F;
                configuration.rayTMax = 16.0F;
            }
        }
        if (variant_ == LabVariant::Solution)
        {
            if (context.input.WasKeyPressed('B'))
            {
                configuration.topLevelBuild = configuration.topLevelBuild == TopLevelBuildMode::Rebuild
                                                  ? TopLevelBuildMode::Update
                                                  : TopLevelBuildMode::Rebuild;
            }
        }
        configuration.frameIndex = static_cast<std::uint32_t>(context.frameIndex);
    }

    LabConfiguration const configuration = ActiveConfiguration();
    if (auto validation = ValidateLabConfiguration(configuration, variant_); !validation)
    {
        return validation;
    }
    auto reference = BuildReferenceFrame(configuration, variant_, size_);
    if (!reference)
    {
        return std::unexpected(std::move(reference.error()));
    }
    currentReference_ = std::move(*reference);
    return {};
}

lgp::framework::Status RendererCore::Render(lgp::framework::FrameContext const &frameContext)
{
    if (frameContext.frameSlot >= frameSlots_.size())
    {
        return std::unexpected(lgp::framework::MakeError("Render", "The Chapter 33 frame slot is out of range."));
    }

    LabConfiguration const configuration = ActiveConfiguration();
    if (auto validation = ValidateLabConfiguration(configuration, variant_); !validation)
    {
        return validation;
    }
    if (variant_ == LabVariant::Solution && !capability_.dispatchable)
    {
        // An adapter without DXR gets an explicit refusal that names the evidence, not a silent fallback to the
        // analytic path dressed up as raytracing.
        return std::unexpected(lgp::framework::MakeError(
            "Render", std::string{RaytracingSupportDiagnostic(capability_.status)} + " Reported raytracing tier " +
                          std::to_string(capability_.reportedTier) + " and highest shader model " +
                          DescribeShaderModel(capability_.reportedShaderModel) + "."));
    }

    FrameSlotResources &slot = frameSlots_[frameContext.frameSlot];
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;
    // Both the ray dispatch and the display draw read through the shader-visible heap, so it is set once for the
    // whole frame rather than by whichever pass happens to run first.
    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);

    lastBarriers_.clear();
    lastBuildTimeline_.clear();
    lastAccelerationEvidence_ = {};
    lastShaderTableEvidence_ = {};

    // The stage list is built into locals and only published once the frame has finished recording. A frame that
    // fails halfway therefore leaves the previously published evidence alone instead of overwriting part of it: a
    // reader that sees a stage list has a stage list from a frame that really ran to the end.
    std::array<LabStage, kLabStageCount> stages{};
    std::uint32_t stageCount = 0U;
    auto const pushStage = [&stages, &stageCount](LabStage stage)
    {
        if (stageCount < stages.size())
        {
            stages[stageCount] = stage;
            ++stageCount;
        }
    };

    if (auto status = UploadScene(slot, configuration); !status)
    {
        return status;
    }
    pushStage(LabStage::SceneUpload);
    RecordCounterReset(commandList, slot);

    PipelineStatusLedger ledger{};
    if (variant_ == LabVariant::Solution)
    {
        if (auto status = RecordAccelerationStructures(commandList, slot, configuration); !status)
        {
            return status;
        }
        pushStage(LabStage::BottomLevelBuild);
        pushStage(LabStage::BottomLevelBarrier);
        pushStage(LabStage::TopLevelBuild);
        pushStage(LabStage::TopLevelBarrier);
        // The state object was created once, at initialization. The ledger records that this frame *resolved* it
        // rather than that it created one, because a per-frame ledger that claimed a creation would be describing
        // work the frame never submitted.
        pushStage(LabStage::StateObjectResolved);
        pushStage(LabStage::ShaderTableRecorded);

        if (auto recorded =
                ledger.RecordSuccess(PipelineStage::BottomLevelBuild, lastAccelerationEvidence_.geometryPrimitiveCount);
            !recorded)
        {
            return std::unexpected(LabContractFailure("PipelineStatusLedger::RecordSuccess", recorded.error()));
        }
        if (auto recorded = ledger.RecordSuccess(PipelineStage::TopLevelBuild, lastAccelerationEvidence_.instanceCount);
            !recorded)
        {
            return std::unexpected(LabContractFailure("PipelineStatusLedger::RecordSuccess", recorded.error()));
        }
        if (auto recorded = ledger.RecordSuccess(
                PipelineStage::StateObject, static_cast<std::uint32_t>(stateObjectEvidence_.validation.entries.size()));
            !recorded)
        {
            return std::unexpected(LabContractFailure("PipelineStatusLedger::RecordSuccess", recorded.error()));
        }

        if (auto status = RecordRayDispatch(commandList, slot, configuration); !status)
        {
            return status;
        }
        pushStage(LabStage::RayDispatch);

        std::uint32_t recordCount = 0U;
        for (ShaderTableSectionLayout const &section : shaderTableLayout_.sections)
        {
            recordCount += static_cast<std::uint32_t>(section.recordCount);
        }
        if (auto recorded = ledger.RecordSuccess(PipelineStage::ShaderTable, recordCount); !recorded)
        {
            return std::unexpected(LabContractFailure("PipelineStatusLedger::RecordSuccess", recorded.error()));
        }
        if (auto recorded = ledger.RecordSuccess(PipelineStage::Dispatch, 1U); !recorded)
        {
            return std::unexpected(LabContractFailure("PipelineStatusLedger::RecordSuccess", recorded.error()));
        }

        auto const timeline = ValidateBuildTimeline(lastBuildTimeline_);
        if (!timeline)
        {
            return std::unexpected(LabContractFailure("ValidateBuildTimeline", timeline.error()));
        }
        lastAccelerationEvidence_.timeline = *timeline;
    }
    else
    {
        RecordAnalyticTraversal(commandList, slot, configuration);
        pushStage(LabStage::RayDispatch);
    }

    // `RecordDisplayAndReadback` records the display draw *and* the two readback copies, so the readback stage is
    // pushed immediately before it rather than after: the mask the display shader is handed has to describe the
    // frame the display shader is part of. Nothing below can fail, so pushing first cannot claim work that never
    // happened.
    pushStage(LabStage::OutputReadback);
    std::span<LabStage const> const submittedStages{stages.data(), stageCount};
    RecordDisplayAndReadback(frameContext, slot, configuration, submittedStages);

    for (std::size_t index = 0U; index < kPipelineStageCount; ++index)
    {
        lastLedger_[index] = ledger.Status(static_cast<PipelineStage>(index));
    }
    lastLedgerReady_ = ledger.IsReadyToDispatch();
    lastLedgerEvidence_ = ledger.TotalEvidenceCount();
    lastStages_ = stages;
    lastStageCount_ = stageCount;
    lastStageOrderWord_ = EncodeStageOrder(submittedStages);
    lastStageMask_ = StageMask(submittedStages);
    lastStageSkippedMask_ = ((1U << kLabStageCount) - 1U) & ~lastStageMask_ & ~UnavailableStageMask(variant_);
    lastRenderedConfiguration_ = configuration;
    lastRenderedFrameSlot_ = frameContext.frameSlot;
    lastRayRecordAddress_ = slot.rayRecords.gpu_virtual_address();
    lastTopLevelAddress_ = slot.topLevel.gpu_virtual_address();
    lastShaderTableAddress_ = slot.shaderTable.gpu_virtual_address();
    hasRendered_ = true;
    return {};
}

void RendererCore::DestroyFrameSlotResources(lgp::framework::DeviceResources &deviceResources) noexcept
{
    for (FrameSlotResources &slot : frameSlots_)
    {
        if (slot.descriptors)
        {
            deviceResources.shader_visible_cbv_srv_uav_heap().Free(slot.descriptors);
            slot.descriptors = {};
        }
    }
    frameSlots_.clear();
    hasRendered_ = false;
}

void RendererCore::PrintHeadlessEvidence() const noexcept
{
    if (!headless_ || headlessConfiguration_.has_value() || !hasRendered_ ||
        lastRenderedFrameSlot_ >= frameSlots_.size())
    {
        return;
    }

    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    std::byte const *const frameData = slot.frameCountersReadback.mapped_data();
    std::byte const *const recordData = slot.rayRecordsReadback.mapped_data();
    if (frameData == nullptr || recordData == nullptr)
    {
        return;
    }

    FrameRecord frame{};
    std::memcpy(&frame, frameData, sizeof(frame));
    std::uint32_t statusAnd = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t statusOr = 0U;
    std::uint32_t validIdentityCount = 0U;
    std::uint32_t distanceCount = 0U;
    std::uint32_t barycentricCount = 0U;
    std::uint32_t positionCount = 0U;
    std::size_t const recordCount = static_cast<std::size_t>(size_.width) * static_cast<std::size_t>(size_.height);
    for (std::size_t index = 0U; index < recordCount; ++index)
    {
        RayRecord record{};
        std::memcpy(&record, recordData + index * sizeof(record), sizeof(record));
        statusAnd &= record.status;
        statusOr |= record.status;
        validIdentityCount += record.instanceIndex != kInvalidIndex && record.primitiveIndex != kInvalidIndex ? 1U : 0U;
        distanceCount += record.tHit != 0.0F ? 1U : 0U;
        barycentricCount += record.barycentricB1 != 0.0F || record.barycentricB2 != 0.0F ? 1U : 0U;
        positionCount +=
            record.worldPositionX != 0.0F || record.worldPositionY != 0.0F || record.worldPositionZ != 0.0F ? 1U : 0U;
    }

    std::printf("Evidence frame: variant=%s rays=%" PRIu32 " hits=%" PRIu32 " misses=%" PRIu32 " front=%" PRIu32
                " back=%" PRIu32 "\n",
                variant_ == LabVariant::Solution ? "dxr" : "analytic", frame.rayCount, frame.hitCount, frame.missCount,
                frame.frontFaceCount, frame.backFaceCount);
    std::printf("Evidence stages: count=%" PRIu32 " order=0x%" PRIX64 " submitted=0x%03" PRIX32 " skipped=0x%03" PRIX32
                " unavailable=0x%03" PRIX32 " ledger=%s items=%" PRIu32 "\n",
                lastStageCount_, lastStageOrderWord_, lastStageMask_, lastStageSkippedMask_,
                UnavailableStageMask(variant_), lastLedgerReady_ ? "ready" : "not-ready", lastLedgerEvidence_);
    std::printf("Evidence records: status-and=0x%04" PRIX32 " status-or=0x%04" PRIX32 " identity=%" PRIu32
                " distance=%" PRIu32 " barycentrics=%" PRIu32 " position=%" PRIu32 "\n",
                statusAnd, statusOr, validIdentityCount, distanceCount, barycentricCount, positionCount);
    std::printf("Evidence routing: instances=[%" PRIu32 ",%" PRIu32 ",%" PRIu32 "] hit-groups=[%" PRIu32 ",%" PRIu32
                ",%" PRIu32 ",%" PRIu32 "] miss-records=[%" PRIu32 ",%" PRIu32 "]\n",
                frame.instanceHitCounts[0], frame.instanceHitCounts[1], frame.instanceHitCounts[2],
                frame.hitGroupRecordCounts[0], frame.hitGroupRecordCounts[1], frame.hitGroupRecordCounts[2],
                frame.hitGroupRecordCounts[3], frame.missRecordCounts[0], frame.missRecordCounts[1]);

    if (variant_ == LabVariant::Solution)
    {
        PrebuildEvidence const &bottom = lastAccelerationEvidence_.bottomLevelPrebuild;
        PrebuildEvidence const &top = lastAccelerationEvidence_.topLevelPrebuild;
        auto const &sections = lastShaderTableEvidence_.layout.sections;
        std::uint64_t const rayGenerationStride = !sections.empty() ? sections[0].strideBytes : 0U;
        std::uint64_t const missStride = sections.size() > 1U ? sections[1].strideBytes : 0U;
        std::uint64_t const hitGroupStride = sections.size() > 2U ? sections[2].strideBytes : 0U;
        std::printf("Evidence prebuild: BLAS report=[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "] budget=[%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 "] TLAS report=[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "] budget=[%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 "]\n",
                    bottom.reported.resultDataMaxSizeBytes, bottom.reported.scratchDataSizeBytes,
                    bottom.reported.updateScratchDataSizeBytes, bottom.allocationBudget.resultDataMaxSizeBytes,
                    bottom.allocationBudget.scratchDataSizeBytes, bottom.allocationBudget.updateScratchDataSizeBytes,
                    top.reported.resultDataMaxSizeBytes, top.reported.scratchDataSizeBytes,
                    top.reported.updateScratchDataSizeBytes, top.allocationBudget.resultDataMaxSizeBytes,
                    top.allocationBudget.scratchDataSizeBytes, top.allocationBudget.updateScratchDataSizeBytes);
        std::printf(
            "Evidence pipeline: state-object=%s subobjects=%" PRIu32 " exports=%" PRIu32 " hit-groups=%" PRIu32
            " table-bytes=%" PRIu64 " records=[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "] strides=[%" PRIu64 ",%" PRIu64
            ",%" PRIu64 "] barriers=%zu\n",
            stateObjectEvidence_.created ? "created" : "missing", stateObjectEvidence_.subobjectCount,
            stateObjectEvidence_.exportCount, stateObjectEvidence_.hitGroupCount,
            lastShaderTableEvidence_.totalSizeBytes,
            lastShaderTableEvidence_.dispatchDescription.rayGeneration.startAddress -
                lastShaderTableEvidence_.baseAddress,
            lastShaderTableEvidence_.dispatchDescription.miss.startAddress - lastShaderTableEvidence_.baseAddress,
            lastShaderTableEvidence_.dispatchDescription.hitGroup.startAddress - lastShaderTableEvidence_.baseAddress,
            rayGenerationStride, missStride, hitGroupStride, lastBarriers_.size());
    }
}

void RendererCore::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    (void)deviceResources.WaitForGpuIdle();
    PrintHeadlessEvidence();
    DestroyFrameSlotResources(deviceResources);
    stateObject_.Reset();
    analyticPipeline_.Reset();
    graphicsPipeline_.Reset();
    globalRootSignature_.Reset();
    hitGroupLocalRootSignature_.Reset();
    graphicsRootSignature_.Reset();
    deviceResources_ = nullptr;
    hasRendered_ = false;
}

void RendererCore::ConfigureHeadlessTest(LabConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

std::expected<FrameReadback, lgp::framework::Error> RendererCore::ReadBackOutputs()
{
    if (deviceResources_ == nullptr || frameSlots_.empty() || !hasRendered_)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "No Chapter 33 frame has completed recording."));
    }
    if (auto status = deviceResources_->WaitForGpuIdle(); !status)
    {
        return std::unexpected(std::move(status.error()));
    }

    FrameSlotResources const &slot = frameSlots_[lastRenderedFrameSlot_];
    if (slot.rayRecordsReadback.mapped_data() == nullptr || slot.frameCountersReadback.mapped_data() == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "The Chapter 33 readback buffers are not mapped."));
    }

    FrameReadback readback{};
    readback.configuration = lastRenderedConfiguration_;
    readback.variant = variant_;
    readback.displaySize = size_;
    readback.records.resize(static_cast<std::size_t>(size_.width) * static_cast<std::size_t>(size_.height));
    std::memcpy(readback.records.data(), slot.rayRecordsReadback.mapped_data(),
                readback.records.size() * sizeof(RayRecord));
    std::memcpy(&readback.frame, slot.frameCountersReadback.mapped_data(), sizeof(FrameRecord));
    static_assert(kFrameRecordWordCount * sizeof(std::uint32_t) == sizeof(FrameRecord));

    readback.reference = currentReference_;
    readback.capability = capability_;
    readback.accelerationStructures = lastAccelerationEvidence_;
    readback.shaderTable = lastShaderTableEvidence_;
    readback.stateObject = stateObjectEvidence_;
    readback.barriers = lastBarriers_;
    readback.buildTimeline = lastBuildTimeline_;
    readback.ledger = lastLedger_;
    readback.ledgerReadyToDispatch = lastLedgerReady_;
    readback.ledgerEvidenceCount = lastLedgerEvidence_;
    readback.stages = lastStages_;
    readback.stageCount = lastStageCount_;
    readback.stageOrderWord = lastStageOrderWord_;
    readback.stageMask = lastStageMask_;
    readback.stageSkippedMask = lastStageSkippedMask_;
    readback.stageUnavailableMask = UnavailableStageMask(variant_);
    readback.instances = lastInstances_;
    readback.frameSlot = lastRenderedFrameSlot_;
    readback.rayRecordGpuAddress = lastRayRecordAddress_;
    readback.topLevelGpuAddress = lastTopLevelAddress_;
    readback.shaderTableGpuAddress = lastShaderTableAddress_;
    return readback;
}

} // namespace ch33::dxr::gpu
