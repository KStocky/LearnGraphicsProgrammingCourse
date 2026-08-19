#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <expected>
#include <optional>
#include <vector>

namespace ch13::work_distribution::solution
{

struct HeadlessTestConfiguration final
{
    gpu::ScenePreset scene{gpu::ScenePreset::Default};
    std::uint32_t capacity{gpu::kCandidateCount};
    gpu::ExecutionMode mode{gpu::ExecutionMode::Stable};
};

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    struct ReadbackOutputs final
    {
        DistributionStatistics statistics{};
        std::vector<std::uint32_t> emittedCandidateIndices{};
        std::vector<IndirectDrawCommand> indirectCommands{};
        std::uint32_t gpuCount{};
        std::uint32_t executionCount{};
        gpu::ExecutionMode mode{gpu::ExecutionMode::Stable};
    };

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

    void ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept;
    [[nodiscard]] std::expected<ReadbackOutputs, lgp::framework::Error> ReadBackOutputs();

  private:
    struct FrameSlotResources final
    {
        gpu::BufferResource candidateBuffer{};
        gpu::BufferResource flags{};
        gpu::BufferResource emittedIndices{};
        gpu::BufferResource indirectCommands{};
        gpu::BufferResource indirectCount{};
        gpu::BufferResource statistics{};
        gpu::BufferResource emittedIndicesReadback{};
        gpu::BufferResource indirectCommandsReadback{};
        gpu::BufferResource indirectCountReadback{};
        gpu::BufferResource statisticsReadback{};
        lgp::framework::DescriptorAllocation descriptors{};
        bool writableStateInitialized{false};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateGraphicsRootSignature();
    [[nodiscard]] lgp::framework::Status CreateComputeRootSignature();
    [[nodiscard]] lgp::framework::Status CreatePipelines();
    [[nodiscard]] lgp::framework::Status CreateCommandSignature();
    [[nodiscard]] lgp::framework::Status CreateCandidateResources();
    [[nodiscard]] lgp::framework::Status CreateFrameSlotResources();
    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;
    void DestroyFrameSlotResources(lgp::framework::DeviceResources &deviceResources) noexcept;

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader resetShader_{};
    lgp::framework::CompiledShader classifyShader_{};
    lgp::framework::CompiledShader stableShader_{};
    lgp::framework::CompiledShader atomicShader_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> resetPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> classifyPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> stablePipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> atomicPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> commandSignature_{};
    std::vector<FrameSlotResources> frameSlots_{};
    gpu::CpuReference currentReference_{};
    UINT lastRenderedFrameSlot_{0U};
    std::optional<HeadlessTestConfiguration> headlessConfiguration_{};
    bool headless_{false};
};

} // namespace ch13::work_distribution::solution
