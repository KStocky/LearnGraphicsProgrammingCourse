#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <expected>
#include <optional>

namespace ch34::particles::solution
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    Renderer() = default;
    Renderer(Renderer &&) noexcept = default;
    Renderer &operator=(Renderer &&) noexcept = default;
    Renderer(Renderer const &) = delete;
    Renderer &operator=(Renderer const &) = delete;
    ~Renderer() override = default;

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void ConfigureHeadlessTest(gpu::LabConfiguration const &configuration) noexcept;
    [[nodiscard]] std::expected<gpu::FrameReadback, lgp::framework::Error> ReadBackOutputs();

  private:
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateRootSignatures();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateCommandSignature();
    [[nodiscard]] lgp::framework::Status CreateResources();
    void DestroyResources(lgp::framework::DeviceResources &deviceResources) noexcept;
    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;

    void SetLabConstants(ID3D12GraphicsCommandList7 &commandList, gpu::LabConfiguration const &configuration,
                         std::uint32_t readSlot, std::uint32_t writeSlot, std::uint32_t requested) const noexcept;

    lgp::framework::DeviceResources *deviceResources_{};

    lgp::framework::CompiledShader initShader_{};
    lgp::framework::CompiledShader emitShader_{};
    lgp::framework::CompiledShader simulateShader_{};
    lgp::framework::CompiledShader compactShader_{};
    lgp::framework::CompiledShader indirectArgsShader_{};
    lgp::framework::CompiledShader checksumShader_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};

    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> initPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> emitPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> simulatePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> compactPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> indirectArgsPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> checksumPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> commandSignature_{};

    gpu::BufferResource stateA_{};
    gpu::BufferResource stateB_{};
    gpu::BufferResource counters_{};
    gpu::BufferResource compactedSlots_{};
    gpu::BufferResource compactedIdentities_{};
    gpu::BufferResource indirectArgs_{};
    gpu::BufferResource indirectCount_{};
    gpu::BufferResource checksum_{};
    gpu::BufferResource stateReadback_{};
    gpu::BufferResource countersReadback_{};
    gpu::BufferResource compactedSlotsReadback_{};
    gpu::BufferResource compactedIdentitiesReadback_{};
    gpu::BufferResource indirectArgsReadback_{};
    gpu::BufferResource indirectCountReadback_{};
    gpu::BufferResource checksumReadback_{};
    lgp::framework::DescriptorAllocation descriptors_{};

    std::uint32_t allocatedCapacity_{};
    bool resourcesInitialized_{false};

    std::optional<gpu::LabConfiguration> headlessConfiguration_{};
    bool headless_{false};

    FixedStepPlan lastPlan_{};
    FrameSlots slotsUsed_{};
    BufferSlot resultSlot_{BufferSlot::A};
    bool anySubsteps_{false};
};

} // namespace ch34::particles::solution
