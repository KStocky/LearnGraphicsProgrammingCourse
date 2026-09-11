#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Renderer.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <string>

namespace ch34::particles::solution
{
namespace
{

using Microsoft::WRL::ComPtr;

inline constexpr UINT kUavCount = 8U;
inline constexpr UINT kSrvCount = 3U;
inline constexpr UINT kDescriptorCount = kUavCount + kSrvCount;
inline constexpr UINT kSrvTableOffset = kUavCount;

enum ComputeRootParameter : UINT
{
    ComputeConstants = 0U,
    ComputeUavTable = 1U,
};

enum GraphicsRootParameter : UINT
{
    DrawConstantsParam = 0U,
    DrawSrvTable = 1U,
};

enum UavIndex : UINT
{
    StateAUav = 0U,
    StateBUav = 1U,
    CountersUav = 2U,
    CompactedSlotsUav = 3U,
    CompactedIdentitiesUav = 4U,
    IndirectArgsUav = 5U,
    IndirectCountUav = 6U,
    ChecksumUav = 7U,
};

struct LabConstants final
{
    std::uint32_t capacity{};
    std::uint32_t readSlot{};
    std::uint32_t writeSlot{};
    std::uint32_t requested{};
    std::uint32_t integrator{};
    std::uint32_t groundEnabled{};
    std::uint32_t vertexCountPerParticle{};
    std::uint32_t drawCapacity{};
    std::uint32_t outputCapacity{};
    std::uint32_t seedLo{};
    std::uint32_t seedHi{};
    std::uint32_t deadSlotPolicy{};
    float timestep{};
    float gravityX{};
    float gravityY{};
    float gravityZ{};
    float linearDrag{};
    float originX{};
    float originY{};
    float originZ{};
    float posJitter{};
    float baseVelX{};
    float baseVelY{};
    float baseVelZ{};
    float velJitter{};
    float groundHeight{};
    float restitution{};
    float friction{};
    float restingSpeed{};
    float lifetime{};
    float viewScale{};
    float quadHalfExtent{};
};

static_assert(sizeof(LabConstants) == 128U);

struct DrawConstants final
{
    std::uint32_t drawStateSlot{};
    float viewScale{};
    float quadHalfExtent{};
    std::uint32_t pad{};
};

static_assert(sizeof(DrawConstants) == 16U);

[[nodiscard]] std::filesystem::path ShaderPath()
{
    return std::filesystem::path{__FILE__}.parent_path() / "ParticleLab.hlsl";
}

[[nodiscard]] std::string BlobText(ID3DBlob *blob)
{
    if (blob == nullptr || blob->GetBufferPointer() == nullptr || blob->GetBufferSize() == 0U)
    {
        return {};
    }
    return {static_cast<char const *>(blob->GetBufferPointer()), static_cast<std::size_t>(blob->GetBufferSize())};
}

[[nodiscard]] lgp::framework::Status CreateComputePipeline(ID3D12Device10 &device, ID3D12RootSignature &rootSignature,
                                                           lgp::framework::CompiledShader const &shader,
                                                           char const *label, ComPtr<ID3D12PipelineState> &pipeline)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = &rootSignature;
    description.CS = shader.Bytecode();
    HRESULT const result =
        device.CreateComputePipelineState(&description, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
    if (FAILED(result))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateComputePipelineState", result,
                                             std::string{"Failed to create the Chapter 34 "} + label + " pipeline."));
    }
    return {};
}

[[nodiscard]] UINT SlotIndex(BufferSlot slot) noexcept
{
    return slot == BufferSlot::A ? 0U : 1U;
}

} // namespace

lgp::framework::Status Renderer::CreateShaders()
{
    auto compilerResult = lgp::framework::ShaderCompiler::Create();
    if (!compilerResult)
    {
        return std::unexpected(std::move(compilerResult.error()));
    }

    lgp::framework::ShaderCompiler compiler = std::move(*compilerResult);
    lgp::framework::ShaderCompileOptions options{};
    options.sourcePath = ShaderPath();
    options.includeDirectories = {options.sourcePath.parent_path()};
#ifdef _DEBUG
    options.enableDebugInformation = true;
    options.optimize = false;
#endif

    if (auto status = gpu::CompileShader(compiler, options, L"InitCS", L"cs_6_0", initShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"EmitCS", L"cs_6_0", emitShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"SimulateCS", L"cs_6_0", simulateShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"CompactCS", L"cs_6_0", compactShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"IndirectArgsCS", L"cs_6_0", indirectArgsShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"ChecksumCS", L"cs_6_0", checksumShader_); !status)
    {
        return status;
    }
    if (auto status = gpu::CompileShader(compiler, options, L"ParticleVS", L"vs_6_0", vertexShader_); !status)
    {
        return status;
    }
    return gpu::CompileShader(compiler, options, L"ParticlePS", L"ps_6_0", pixelShader_);
}

lgp::framework::Status Renderer::CreateRootSignatures()
{
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = kUavCount;
    uavRange.BaseShaderRegister = 0U;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER computeParameters[2]{};
    computeParameters[ComputeConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[ComputeConstants].Constants.ShaderRegister = 0U;
    computeParameters[ComputeConstants].Constants.Num32BitValues = sizeof(LabConstants) / sizeof(std::uint32_t);
    computeParameters[ComputeConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[ComputeUavTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[ComputeUavTable].DescriptorTable.NumDescriptorRanges = 1U;
    computeParameters[ComputeUavTable].DescriptorTable.pDescriptorRanges = &uavRange;
    computeParameters[ComputeUavTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC computeDescription{};
    computeDescription.NumParameters = static_cast<UINT>(std::size(computeParameters));
    computeDescription.pParameters = computeParameters;

    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};
    HRESULT serializeResult =
        D3D12SerializeRootSignature(&computeDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                    serialized.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (FAILED(serializeResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult, BlobText(errors.Get())));
    }
    HRESULT createResult =
        deviceResources_->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(computeRootSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                             "Failed to create the Chapter 34 compute root signature."));
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = kSrvCount;
    srvRange.BaseShaderRegister = 0U;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER graphicsParameters[2]{};
    graphicsParameters[DrawConstantsParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[DrawConstantsParam].Constants.ShaderRegister = 1U;
    graphicsParameters[DrawConstantsParam].Constants.Num32BitValues = sizeof(DrawConstants) / sizeof(std::uint32_t);
    graphicsParameters[DrawConstantsParam].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    graphicsParameters[DrawSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    graphicsParameters[DrawSrvTable].DescriptorTable.NumDescriptorRanges = 1U;
    graphicsParameters[DrawSrvTable].DescriptorTable.pDescriptorRanges = &srvRange;
    graphicsParameters[DrawSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC graphicsDescription{};
    graphicsDescription.NumParameters = static_cast<UINT>(std::size(graphicsParameters));
    graphicsDescription.pParameters = graphicsParameters;
    graphicsDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                                D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    serializeResult = D3D12SerializeRootSignature(&graphicsDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  serialized.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (FAILED(serializeResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("D3D12SerializeRootSignature", serializeResult, BlobText(errors.Get())));
    }
    createResult =
        deviceResources_->device()->CreateRootSignature(0U, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                        IID_PPV_ARGS(graphicsRootSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateRootSignature", createResult,
                                             "Failed to create the Chapter 34 graphics root signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreatePipelines()
{
    ID3D12Device10 &device = *deviceResources_->device();
    if (auto status = CreateComputePipeline(device, *computeRootSignature_.Get(), initShader_, "init", initPipeline_);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(device, *computeRootSignature_.Get(), emitShader_, "emit", emitPipeline_);
        !status)
    {
        return status;
    }
    if (auto status =
            CreateComputePipeline(device, *computeRootSignature_.Get(), simulateShader_, "simulate", simulatePipeline_);
        !status)
    {
        return status;
    }
    if (auto status =
            CreateComputePipeline(device, *computeRootSignature_.Get(), compactShader_, "compact", compactPipeline_);
        !status)
    {
        return status;
    }
    if (auto status = CreateComputePipeline(device, *computeRootSignature_.Get(), indirectArgsShader_,
                                            "indirect arguments", indirectArgsPipeline_);
        !status)
    {
        return status;
    }
    if (auto status =
            CreateComputePipeline(device, *computeRootSignature_.Get(), checksumShader_, "checksum", checksumPipeline_);
        !status)
    {
        return status;
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

    HRESULT const createResult =
        device.CreateGraphicsPipelineState(&description, IID_PPV_ARGS(graphicsPipeline_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateGraphicsPipelineState",
                                                                createResult,
                                                                "Failed to create the Chapter 34 graphics pipeline."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateCommandSignature()
{
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    D3D12_COMMAND_SIGNATURE_DESC description{};
    description.ByteStride = static_cast<UINT>(kDrawArgumentsSizeBytes);
    description.NumArgumentDescs = 1U;
    description.pArgumentDescs = &argument;

    HRESULT const createResult = deviceResources_->device()->CreateCommandSignature(
        &description, nullptr, IID_PPV_ARGS(commandSignature_.ReleaseAndGetAddressOf()));
    if (FAILED(createResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandSignature", createResult,
                                                                "Failed to create the Chapter 34 command signature."));
    }
    return {};
}

lgp::framework::Status Renderer::CreateResources()
{
    allocatedCapacity_ = gpu::kMaximumLabCapacity;
    ID3D12Device10 &device = *deviceResources_->device();

    std::uint64_t const stateBytes = static_cast<std::uint64_t>(allocatedCapacity_) * sizeof(gpu::GpuParticle);
    std::uint64_t const listBytes = static_cast<std::uint64_t>(allocatedCapacity_ + 1U) * sizeof(std::uint32_t);
    std::uint64_t const countersBytes = 8U * sizeof(std::uint32_t);
    std::uint64_t const argsBytes = kDrawArgumentsSizeBytes;
    std::uint64_t const countBytes = sizeof(std::uint32_t);
    std::uint64_t const checksumBytes = 2U * sizeof(std::uint32_t);

    auto make = [&device](gpu::BufferResource &target, std::uint64_t size, D3D12_HEAP_TYPE heap,
                          D3D12_RESOURCE_FLAGS flags, wchar_t const *name, bool mapped) -> lgp::framework::Status
    {
        auto buffer = gpu::CreateBuffer(device, size, heap, flags, name, mapped);
        if (!buffer)
        {
            return std::unexpected(std::move(buffer.error()));
        }
        target = std::move(*buffer);
        return {};
    };

    constexpr D3D12_RESOURCE_FLAGS kUav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    constexpr D3D12_RESOURCE_FLAGS kNone = D3D12_RESOURCE_FLAG_NONE;
    struct BufferSpec final
    {
        gpu::BufferResource *target;
        std::uint64_t size;
        D3D12_HEAP_TYPE heap;
        D3D12_RESOURCE_FLAGS flags;
        wchar_t const *name;
        bool mapped;
    };
    std::array<BufferSpec, 15U> const specs{{
        {&stateA_, stateBytes, D3D12_HEAP_TYPE_DEFAULT, kUav, L"Ch34 State A", false},
        {&stateB_, stateBytes, D3D12_HEAP_TYPE_DEFAULT, kUav, L"Ch34 State B", false},
        {&counters_, countersBytes, D3D12_HEAP_TYPE_DEFAULT, kUav, L"Ch34 Counters", false},
        {&compactedSlots_, listBytes, D3D12_HEAP_TYPE_DEFAULT, kUav, L"Ch34 Compacted Slots", false},
        {&compactedIdentities_, listBytes, D3D12_HEAP_TYPE_DEFAULT, kUav, L"Ch34 Compacted Ids", false},
        {&indirectArgs_, argsBytes, D3D12_HEAP_TYPE_DEFAULT, kUav, L"Ch34 Indirect Args", false},
        {&indirectCount_, countBytes, D3D12_HEAP_TYPE_DEFAULT, kUav, L"Ch34 Indirect Count", false},
        {&checksum_, checksumBytes, D3D12_HEAP_TYPE_DEFAULT, kUav, L"Ch34 Checksum", false},
        {&stateReadback_, stateBytes, D3D12_HEAP_TYPE_READBACK, kNone, L"Ch34 State RB", true},
        {&countersReadback_, countersBytes, D3D12_HEAP_TYPE_READBACK, kNone, L"Ch34 Counters RB", true},
        {&compactedSlotsReadback_, listBytes, D3D12_HEAP_TYPE_READBACK, kNone, L"Ch34 Slots RB", true},
        {&compactedIdentitiesReadback_, listBytes, D3D12_HEAP_TYPE_READBACK, kNone, L"Ch34 Ids RB", true},
        {&indirectArgsReadback_, argsBytes, D3D12_HEAP_TYPE_READBACK, kNone, L"Ch34 Args RB", true},
        {&indirectCountReadback_, countBytes, D3D12_HEAP_TYPE_READBACK, kNone, L"Ch34 Count RB", true},
        {&checksumReadback_, checksumBytes, D3D12_HEAP_TYPE_READBACK, kNone, L"Ch34 Checksum RB", true},
    }};
    for (BufferSpec const &spec : specs)
    {
        if (auto status = make(*spec.target, spec.size, spec.heap, spec.flags, spec.name, spec.mapped); !status)
        {
            return status;
        }
    }

    auto descriptors = deviceResources_->shader_visible_cbv_srv_uav_heap().Allocate(kDescriptorCount);
    if (!descriptors)
    {
        return std::unexpected(std::move(descriptors.error()));
    }
    descriptors_ = *descriptors;

    auto structuredUav =
        [&device](gpu::BufferResource &buffer, UINT numElements, UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.Buffer.FirstElement = 0U;
        uav.Buffer.NumElements = numElements;
        uav.Buffer.StructureByteStride = stride;
        device.CreateUnorderedAccessView(buffer.Get(), nullptr, &uav, handle);
    };

    structuredUav(stateA_, allocatedCapacity_, sizeof(gpu::GpuParticle), descriptors_.CpuHandle(StateAUav));
    structuredUav(stateB_, allocatedCapacity_, sizeof(gpu::GpuParticle), descriptors_.CpuHandle(StateBUav));
    structuredUav(counters_, 8U, sizeof(std::uint32_t), descriptors_.CpuHandle(CountersUav));
    structuredUav(compactedSlots_, allocatedCapacity_ + 1U, sizeof(std::uint32_t),
                  descriptors_.CpuHandle(CompactedSlotsUav));
    structuredUav(compactedIdentities_, allocatedCapacity_ + 1U, sizeof(std::uint32_t),
                  descriptors_.CpuHandle(CompactedIdentitiesUav));
    structuredUav(indirectCount_, 1U, sizeof(std::uint32_t), descriptors_.CpuHandle(IndirectCountUav));
    structuredUav(checksum_, 1U, 2U * sizeof(std::uint32_t), descriptors_.CpuHandle(ChecksumUav));

    D3D12_UNORDERED_ACCESS_VIEW_DESC argsUav{};
    argsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    argsUav.Format = DXGI_FORMAT_R32_TYPELESS;
    argsUav.Buffer.FirstElement = 0U;
    argsUav.Buffer.NumElements = static_cast<UINT>(argsBytes / sizeof(std::uint32_t));
    argsUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device.CreateUnorderedAccessView(indirectArgs_.Get(), nullptr, &argsUav, descriptors_.CpuHandle(IndirectArgsUav));

    auto structuredSrv =
        [&device](gpu::BufferResource &buffer, UINT numElements, UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Buffer.FirstElement = 0U;
        srv.Buffer.NumElements = numElements;
        srv.Buffer.StructureByteStride = stride;
        device.CreateShaderResourceView(buffer.Get(), &srv, handle);
    };

    structuredSrv(stateA_, allocatedCapacity_, sizeof(gpu::GpuParticle), descriptors_.CpuHandle(kSrvTableOffset + 0U));
    structuredSrv(stateB_, allocatedCapacity_, sizeof(gpu::GpuParticle), descriptors_.CpuHandle(kSrvTableOffset + 1U));
    structuredSrv(compactedSlots_, allocatedCapacity_ + 1U, sizeof(std::uint32_t),
                  descriptors_.CpuHandle(kSrvTableOffset + 2U));
    return {};
}

gpu::LabConfiguration Renderer::ActiveConfiguration() const noexcept
{
    if (headlessConfiguration_.has_value())
    {
        return *headlessConfiguration_;
    }
    return gpu::DefaultLabConfiguration();
}

void Renderer::SetLabConstants(ID3D12GraphicsCommandList7 &commandList, gpu::LabConfiguration const &configuration,
                               std::uint32_t readSlot, std::uint32_t writeSlot, std::uint32_t requested) const noexcept
{
    LabConstants constants{};
    constants.capacity = configuration.capacity;
    constants.readSlot = readSlot;
    constants.writeSlot = writeSlot;
    constants.requested = requested;
    constants.integrator = static_cast<std::uint32_t>(configuration.frame.integrator);
    constants.groundEnabled = configuration.frame.ground.enabled ? 1U : 0U;
    constants.vertexCountPerParticle = gpu::kVertexCountPerParticle;
    constants.drawCapacity = gpu::ResolveDrawCapacity(configuration);
    constants.outputCapacity = gpu::ResolveOutputCapacity(configuration);
    constants.seedLo = static_cast<std::uint32_t>(configuration.seed & 0xFFFFFFFFULL);
    constants.seedHi = static_cast<std::uint32_t>(configuration.seed >> 32U);
    constants.deadSlotPolicy = static_cast<std::uint32_t>(configuration.frame.emission.deadSlotPolicy);
    constants.timestep = static_cast<float>(configuration.frame.step.fixedTimestepSeconds);
    constants.gravityX = static_cast<float>(configuration.frame.field.gravityMetresPerSecondSquared.x);
    constants.gravityY = static_cast<float>(configuration.frame.field.gravityMetresPerSecondSquared.y);
    constants.gravityZ = static_cast<float>(configuration.frame.field.gravityMetresPerSecondSquared.z);
    constants.linearDrag = static_cast<float>(configuration.frame.field.linearDragPerSecond);
    constants.originX = static_cast<float>(configuration.frame.emission.originMetres.x);
    constants.originY = static_cast<float>(configuration.frame.emission.originMetres.y);
    constants.originZ = static_cast<float>(configuration.frame.emission.originMetres.z);
    constants.posJitter = static_cast<float>(configuration.frame.emission.positionJitterMetres);
    constants.baseVelX = static_cast<float>(configuration.frame.emission.baseVelocityMetresPerSecond.x);
    constants.baseVelY = static_cast<float>(configuration.frame.emission.baseVelocityMetresPerSecond.y);
    constants.baseVelZ = static_cast<float>(configuration.frame.emission.baseVelocityMetresPerSecond.z);
    constants.velJitter = static_cast<float>(configuration.frame.emission.velocityJitterMetresPerSecond);
    constants.groundHeight = static_cast<float>(configuration.frame.ground.heightMetres);
    constants.restitution = static_cast<float>(configuration.frame.ground.restitution);
    constants.friction = static_cast<float>(configuration.frame.ground.friction);
    constants.restingSpeed = static_cast<float>(configuration.frame.ground.restingSpeedMetresPerSecond);
    constants.lifetime = static_cast<float>(configuration.frame.emission.lifetimeSeconds);
    constants.viewScale = configuration.viewScale;
    constants.quadHalfExtent = configuration.quadHalfExtent;

    commandList.SetComputeRoot32BitConstants(ComputeConstants, sizeof(LabConstants) / sizeof(std::uint32_t), &constants,
                                             0U);
}

lgp::framework::Status Renderer::Initialize(lgp::framework::ApplicationInitContext const &context)
{
    deviceResources_ = &context.deviceResources;
    headless_ = context.commandLine.headless;
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
    if (auto status = CreateCommandSignature(); !status)
    {
        return status;
    }
    return CreateResources();
}

lgp::framework::Status Renderer::OnResize(lgp::framework::DeviceResources &deviceResources,
                                          lgp::framework::Extent2D drawableSize)
{
    (void)deviceResources;
    (void)drawableSize;
    return {};
}

lgp::framework::Status Renderer::Update(lgp::framework::UpdateContext const &context)
{
    (void)context;
    gpu::LabConfiguration const configuration = ActiveConfiguration();
    if (configuration.capacity == 0U || configuration.capacity > gpu::kMaximumLabCapacity)
    {
        return std::unexpected(
            lgp::framework::MakeError("Update", "The Chapter 34 lab capacity is out of the single-group budget."));
    }
    if (gpu::ResolveOutputCapacity(configuration) > gpu::kMaximumLabCapacity ||
        gpu::ResolveDrawCapacity(configuration) > gpu::kMaximumLabCapacity)
    {
        return std::unexpected(
            lgp::framework::MakeError("Update", "The Chapter 34 output or draw capacity exceeds the lab budget."));
    }
    if (auto check = ValidateEmissionSettings(configuration.frame.emission); !check)
    {
        return std::unexpected(lgp::framework::MakeError("Update", std::string{"Invalid emission: "} +
                                                                       std::string{ContractErrorName(check.error())}));
    }
    if (auto check = ValidateForceField(configuration.frame.field); !check)
    {
        return std::unexpected(lgp::framework::MakeError("Update", std::string{"Invalid field: "} +
                                                                       std::string{ContractErrorName(check.error())}));
    }
    if (auto check = ValidateGroundPlane(configuration.frame.ground); !check)
    {
        return std::unexpected(lgp::framework::MakeError("Update", std::string{"Invalid ground: "} +
                                                                       std::string{ContractErrorName(check.error())}));
    }
    if (auto check = ValidateFixedStepSettings(configuration.frame.step); !check)
    {
        return std::unexpected(lgp::framework::MakeError("Update", std::string{"Invalid step: "} +
                                                                       std::string{ContractErrorName(check.error())}));
    }
    if (static_cast<std::size_t>(configuration.frame.integrator) >= kIntegratorCount)
    {
        return std::unexpected(lgp::framework::MakeError("Update", "Invalid integrator: UnknownIntegrator"));
    }
    if (configuration.frame.integrator == Integrator::VelocityVerlet &&
        configuration.frame.field.linearDragPerSecond != 0.0)
    {
        return std::unexpected(
            lgp::framework::MakeError("Update", "Invalid integrator: VerletRequiresVelocityIndependentForce"));
    }
    return {};
}

lgp::framework::Status Renderer::Render(lgp::framework::FrameContext const &frameContext)
{
    gpu::LabConfiguration const configuration = ActiveConfiguration();
    ID3D12GraphicsCommandList7 &commandList = *frameContext.commandList;

    ID3D12DescriptorHeap *const heaps[]{frameContext.shaderVisibleCbvSrvUavHeap};
    commandList.SetDescriptorHeaps(1U, heaps);
    commandList.SetComputeRootSignature(computeRootSignature_.Get());
    commandList.SetComputeRootDescriptorTable(ComputeUavTable, descriptors_.GpuHandle(0U));

    ID3D12Resource *const stateBuffers[2]{stateA_.Get(), stateB_.Get()};

    auto uavBarrier = [&commandList](std::initializer_list<ID3D12Resource *> resources)
    {
        std::vector<D3D12_BUFFER_BARRIER> barriers{};
        for (ID3D12Resource *resource : resources)
        {
            barriers.push_back(gpu::MakeBufferBarrier(*resource, gpu::ComputeUnorderedAccessState(),
                                                      gpu::ComputeUnorderedAccessState()));
        }
        gpu::SubmitBufferBarriers(commandList, barriers);
    };

    // Reset: bring every buffer into unordered-access state and clear it.
    {
        gpu::BufferBarrierState const before =
            resourcesInitialized_ ? gpu::ComputeUnorderedAccessState() : gpu::NoAccessState();
        std::vector<D3D12_BUFFER_BARRIER> barriers{
            gpu::MakeBufferBarrier(*stateA_.Get(), before, gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*stateB_.Get(), before, gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*counters_.Get(), before, gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*compactedSlots_.Get(), before, gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*compactedIdentities_.Get(), before, gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*indirectArgs_.Get(), before, gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*indirectCount_.Get(), before, gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*checksum_.Get(), before, gpu::ComputeUnorderedAccessState()),
        };
        gpu::SubmitBufferBarriers(commandList, barriers);
        resourcesInitialized_ = true;
    }

    SetLabConstants(commandList, configuration, 0U, 1U, 0U);
    commandList.SetPipelineState(initPipeline_.Get());
    commandList.Dispatch(1U, 1U, 1U);

    FrameSlots currentSlots{.readSlot = BufferSlot::A, .writeSlot = BufferSlot::B};
    double accumulator = 0.0;
    double carry = 0.0;
    anySubsteps_ = false;
    FrameSlots frameStartSlots = currentSlots;
    std::uint32_t lastSubstepCount = 0U;

    for (std::uint32_t frame = 0U; frame < configuration.frameCount; ++frame)
    {
        auto plan = gpu::PlanFrameEmission(configuration.frame, accumulator, carry, configuration.frameDeltaSeconds);
        if (!plan)
        {
            return std::unexpected(lgp::framework::MakeError(
                "Render", std::string{"Plan failed: "} + std::string{ContractErrorName(plan.error())}));
        }
        frameStartSlots = currentSlots;
        lastSubstepCount = plan->plan.substepCount;
        lastPlan_ = plan->plan;

        for (std::uint32_t substep = 0U; substep < plan->plan.substepCount; ++substep)
        {
            UINT const readIndex = SlotIndex(currentSlots.readSlot);
            UINT const writeIndex = SlotIndex(currentSlots.writeSlot);

            uavBarrier({stateBuffers[readIndex], stateBuffers[writeIndex], counters_.Get()});
            SetLabConstants(commandList, configuration, readIndex, writeIndex, plan->requestedPerSubstep[substep]);
            commandList.SetPipelineState(emitPipeline_.Get());
            commandList.Dispatch(1U, 1U, 1U);

            uavBarrier({stateBuffers[readIndex], counters_.Get()});
            commandList.SetPipelineState(simulatePipeline_.Get());
            commandList.Dispatch(1U, 1U, 1U);

            uavBarrier({stateBuffers[writeIndex], counters_.Get()});
            currentSlots = AdvanceFrameSlots(currentSlots);
            anySubsteps_ = true;
        }

        accumulator = plan->nextAccumulatorSeconds;
        carry = plan->nextEmissionCarry;
    }

    UINT const finalIndex = SlotIndex(currentSlots.readSlot);
    ID3D12Resource *finalState = stateBuffers[finalIndex];

    if (lastSubstepCount == 0U)
    {
        slotsUsed_ = frameStartSlots;
        resultSlot_ = frameStartSlots.readSlot;
    }
    else
    {
        slotsUsed_ = AdvanceFrameSlots(currentSlots);
        resultSlot_ = slotsUsed_.writeSlot;
    }

    // Compaction, checksum, and indirect arguments read the final state slot.
    uavBarrier({finalState, compactedSlots_.Get(), compactedIdentities_.Get(), counters_.Get()});
    SetLabConstants(commandList, configuration, finalIndex, finalIndex ^ 1U, 0U);
    commandList.SetPipelineState(compactPipeline_.Get());
    commandList.Dispatch(1U, 1U, 1U);

    uavBarrier({finalState});
    commandList.SetPipelineState(checksumPipeline_.Get());
    commandList.Dispatch(1U, 1U, 1U);

    uavBarrier({counters_.Get(), indirectArgs_.Get(), indirectCount_.Get(), compactedSlots_.Get()});
    commandList.SetPipelineState(indirectArgsPipeline_.Get());
    commandList.Dispatch(1U, 1U, 1U);

    // The UAV-write to indirect-argument-read edge, plus the compute-to-vertex reads the draw depends on.
    {
        std::vector<D3D12_BUFFER_BARRIER> barriers{
            gpu::MakeBufferBarrier(*indirectArgs_.Get(), gpu::ComputeUnorderedAccessState(),
                                   gpu::ExecuteIndirectState()),
            gpu::MakeBufferBarrier(*indirectCount_.Get(), gpu::ComputeUnorderedAccessState(),
                                   gpu::ExecuteIndirectState()),
            gpu::MakeBufferBarrier(*finalState, gpu::ComputeUnorderedAccessState(), gpu::VertexShaderResourceState()),
            gpu::MakeBufferBarrier(*compactedSlots_.Get(), gpu::ComputeUnorderedAccessState(),
                                   gpu::VertexShaderResourceState()),
        };
        gpu::SubmitBufferBarriers(commandList, barriers);
    }

    std::vector<D3D12_TEXTURE_BARRIER> textureBarriers{
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::FrameStartState(frameContext),
                                gpu::RenderTargetState()),
    };
    gpu::SubmitTextureBarriers(commandList, textureBarriers);

    float const clearColor[]{0.0F, 0.0F, 0.0F, 1.0F};
    commandList.ClearRenderTargetView(frameContext.renderTargetView, clearColor, 0U, nullptr);
    commandList.OMSetRenderTargets(1U, &frameContext.renderTargetView, FALSE, nullptr);
    commandList.RSSetViewports(1U, &frameContext.viewport);
    commandList.RSSetScissorRects(1U, &frameContext.scissorRect);
    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList.SetGraphicsRootSignature(graphicsRootSignature_.Get());
    commandList.SetGraphicsRootDescriptorTable(DrawSrvTable, descriptors_.GpuHandle(kSrvTableOffset));

    DrawConstants drawConstants{};
    drawConstants.drawStateSlot = finalIndex;
    drawConstants.viewScale = configuration.viewScale;
    drawConstants.quadHalfExtent = configuration.quadHalfExtent;
    commandList.SetGraphicsRoot32BitConstants(DrawConstantsParam, sizeof(DrawConstants) / sizeof(std::uint32_t),
                                              &drawConstants, 0U);
    commandList.SetPipelineState(graphicsPipeline_.Get());
    commandList.ExecuteIndirect(commandSignature_.Get(), 1U, indirectArgs_.Get(), 0U, indirectCount_.Get(), 0U);

    textureBarriers = {
        gpu::MakeTextureBarrier(*frameContext.renderTarget, gpu::RenderTargetState(), gpu::FrameEndState(frameContext)),
    };
    gpu::SubmitTextureBarriers(commandList, textureBarriers);

    // Copy every evidence buffer out, then restore unordered-access state for the next run.
    {
        std::vector<D3D12_BUFFER_BARRIER> barriers{
            gpu::MakeBufferBarrier(*finalState, gpu::VertexShaderResourceState(), gpu::CopySourceState()),
            gpu::MakeBufferBarrier(*compactedSlots_.Get(), gpu::VertexShaderResourceState(), gpu::CopySourceState()),
            gpu::MakeBufferBarrier(*compactedIdentities_.Get(), gpu::ComputeUnorderedAccessState(),
                                   gpu::CopySourceState()),
            gpu::MakeBufferBarrier(*counters_.Get(), gpu::ComputeUnorderedAccessState(), gpu::CopySourceState()),
            gpu::MakeBufferBarrier(*checksum_.Get(), gpu::ComputeUnorderedAccessState(), gpu::CopySourceState()),
            gpu::MakeBufferBarrier(*indirectArgs_.Get(), gpu::ExecuteIndirectState(), gpu::CopySourceState()),
            gpu::MakeBufferBarrier(*indirectCount_.Get(), gpu::ExecuteIndirectState(), gpu::CopySourceState()),
        };
        gpu::SubmitBufferBarriers(commandList, barriers);
    }

    commandList.CopyBufferRegion(stateReadback_.Get(), 0U, finalState, 0U,
                                 static_cast<std::uint64_t>(configuration.capacity) * sizeof(gpu::GpuParticle));
    commandList.CopyBufferRegion(countersReadback_.Get(), 0U, counters_.Get(), 0U, counters_.size_in_bytes());
    commandList.CopyBufferRegion(compactedSlotsReadback_.Get(), 0U, compactedSlots_.Get(), 0U,
                                 compactedSlots_.size_in_bytes());
    commandList.CopyBufferRegion(compactedIdentitiesReadback_.Get(), 0U, compactedIdentities_.Get(), 0U,
                                 compactedIdentities_.size_in_bytes());
    commandList.CopyBufferRegion(indirectArgsReadback_.Get(), 0U, indirectArgs_.Get(), 0U,
                                 indirectArgs_.size_in_bytes());
    commandList.CopyBufferRegion(indirectCountReadback_.Get(), 0U, indirectCount_.Get(), 0U,
                                 indirectCount_.size_in_bytes());
    commandList.CopyBufferRegion(checksumReadback_.Get(), 0U, checksum_.Get(), 0U, checksum_.size_in_bytes());

    {
        std::vector<D3D12_BUFFER_BARRIER> barriers{
            gpu::MakeBufferBarrier(*finalState, gpu::CopySourceState(), gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*compactedSlots_.Get(), gpu::CopySourceState(), gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*compactedIdentities_.Get(), gpu::CopySourceState(),
                                   gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*counters_.Get(), gpu::CopySourceState(), gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*checksum_.Get(), gpu::CopySourceState(), gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*indirectArgs_.Get(), gpu::CopySourceState(), gpu::ComputeUnorderedAccessState()),
            gpu::MakeBufferBarrier(*indirectCount_.Get(), gpu::CopySourceState(), gpu::ComputeUnorderedAccessState()),
        };
        gpu::SubmitBufferBarriers(commandList, barriers);
    }
    return {};
}

std::expected<gpu::FrameReadback, lgp::framework::Error> Renderer::ReadBackOutputs()
{
    auto const idle = deviceResources_->WaitForGpuIdle();
    if (!idle)
    {
        return std::unexpected(std::move(idle.error()));
    }

    gpu::LabConfiguration const configuration = ActiveConfiguration();
    auto const *counters = reinterpret_cast<std::uint32_t const *>(countersReadback_.mapped_data());
    auto const *slots = reinterpret_cast<std::uint32_t const *>(compactedSlotsReadback_.mapped_data());
    auto const *identities = reinterpret_cast<std::uint32_t const *>(compactedIdentitiesReadback_.mapped_data());
    auto const *args = reinterpret_cast<std::uint32_t const *>(indirectArgsReadback_.mapped_data());
    auto const *count = reinterpret_cast<std::uint32_t const *>(indirectCountReadback_.mapped_data());
    auto const *checksum = reinterpret_cast<std::uint32_t const *>(checksumReadback_.mapped_data());
    auto const *state = reinterpret_cast<gpu::GpuParticle const *>(stateReadback_.mapped_data());
    if (counters == nullptr || slots == nullptr || identities == nullptr || args == nullptr || count == nullptr ||
        checksum == nullptr || state == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeError("ReadBackOutputs", "The Chapter 34 readbacks are not mapped."));
    }

    gpu::FrameReadback readback{};
    readback.lastPlan = lastPlan_;
    readback.requested = counters[1];
    readback.spawned = counters[2];
    readback.dropped = counters[3];
    readback.died = counters[4];
    readback.contact = counters[5];
    readback.liveCount = counters[6];
    readback.emittedCount = counters[7];

    readback.compactedSlots.assign(slots, slots + readback.emittedCount);
    readback.compactedIdentities.assign(identities, identities + readback.emittedCount);

    readback.indirectArguments = {args[0], args[1], args[2], args[3]};
    readback.executedCommandCount = count[0];
    readback.checksum = static_cast<std::uint64_t>(checksum[0]) | (static_cast<std::uint64_t>(checksum[1]) << 32U);

    readback.state.assign(state, state + configuration.capacity);
    readback.readbackChecksum = gpu::ChecksumGpuState(readback.state);

    readback.slotsUsed = slotsUsed_;
    readback.resultSlot = resultSlot_;
    readback.stagePartition = gpu::BuildStagePartition(anySubsteps_, readback.executedCommandCount > 0U, true);

    std::uint32_t const outputCapacity = gpu::ResolveOutputCapacity(configuration);
    bool const guardSlot = slots[outputCapacity] == kInvalidParticleId;
    bool const guardIdentity = identities[outputCapacity] == kInvalidParticleId;
    readback.guardsIntact = guardSlot && guardIdentity;
    readback.barriersModelled = true;
    return readback;
}

void Renderer::DestroyResources(lgp::framework::DeviceResources &deviceResources) noexcept
{
    if (descriptors_)
    {
        deviceResources.shader_visible_cbv_srv_uav_heap().Free(descriptors_);
        descriptors_ = {};
    }
    stateA_ = {};
    stateB_ = {};
    counters_ = {};
    compactedSlots_ = {};
    compactedIdentities_ = {};
    indirectArgs_ = {};
    indirectCount_ = {};
    checksum_ = {};
    stateReadback_ = {};
    countersReadback_ = {};
    compactedSlotsReadback_ = {};
    compactedIdentitiesReadback_ = {};
    indirectArgsReadback_ = {};
    indirectCountReadback_ = {};
    checksumReadback_ = {};
}

void Renderer::Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept
{
    DestroyResources(deviceResources);
    commandSignature_.Reset();
    graphicsPipeline_.Reset();
    checksumPipeline_.Reset();
    indirectArgsPipeline_.Reset();
    compactPipeline_.Reset();
    simulatePipeline_.Reset();
    emitPipeline_.Reset();
    initPipeline_.Reset();
    graphicsRootSignature_.Reset();
    computeRootSignature_.Reset();
    initShader_ = {};
    emitShader_ = {};
    simulateShader_ = {};
    compactShader_ = {};
    indirectArgsShader_ = {};
    checksumShader_ = {};
    vertexShader_ = {};
    pixelShader_ = {};
    resourcesInitialized_ = false;
    deviceResources_ = nullptr;
}

void Renderer::ConfigureHeadlessTest(gpu::LabConfiguration const &configuration) noexcept
{
    headlessConfiguration_ = configuration;
}

} // namespace ch34::particles::solution
