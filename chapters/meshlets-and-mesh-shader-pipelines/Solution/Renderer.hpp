#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <expected>
#include <optional>
#include <vector>

namespace ch22::meshlets::solution
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

    void ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept;
    [[nodiscard]] std::expected<gpu::MeshEvidence, lgp::framework::Error> ReadBackEvidence();
    [[nodiscard]] gpu::GpuScene const &LastScene() const noexcept;
    [[nodiscard]] gpu::MeshShaderCapabilities Capabilities() const noexcept;

  private:
    struct FrameSlot final
    {
        gpu::BufferResource classicVertices{};
        gpu::BufferResource classicIndices{};
        gpu::BufferResource positions{};
        gpu::BufferResource meshletDescriptors{};
        gpu::BufferResource meshletVertices{};
        gpu::BufferResource meshletPrimitives{};
        gpu::BufferResource stats{};
        gpu::BufferResource statsZero{};
        gpu::BufferResource statsReadback{};
        lgp::framework::DescriptorAllocation descriptors{};
        bool statsStateInitialized{};
    };

    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;
    [[nodiscard]] gpu::ExecutedPath ResolvePath() const noexcept;
    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateClassicPipeline();
    [[nodiscard]] lgp::framework::Status CreateMeshPipeline();
    [[nodiscard]] lgp::framework::Status CreateFrameSlots();
    void RecordClassic(lgp::framework::FrameContext const &frameContext, FrameSlot &slot);
    [[nodiscard]] lgp::framework::Status RecordMesh(lgp::framework::FrameContext const &frameContext, FrameSlot &slot);

    lgp::framework::DeviceResources *deviceResources_{};
    gpu::MeshShaderCapabilities capabilities_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    lgp::framework::CompiledShader amplificationShader_{};
    lgp::framework::CompiledShader meshShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> classicRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> meshRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> classicPipeline_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> meshPipeline_{};
    std::vector<FrameSlot> frameSlots_{};
    gpu::GpuScene scene_{};
    std::optional<HeadlessTestConfiguration> headlessConfiguration_{};
    gpu::MeshEvidence lastEvidence_{};
    UINT lastFrameSlot_{};
    bool headless_{};
};

} // namespace ch22::meshlets::solution
