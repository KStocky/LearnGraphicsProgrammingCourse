#pragma once

#include "../Common/GpuLabSupport.hpp"

#include <lgp/framework/application.hpp>
#include <lgp/framework/descriptors.hpp>
#include <lgp/framework/shader_compiler.hpp>

#include <wrl/client.h>

#include <expected>
#include <optional>
#include <vector>

namespace ch13::work_distribution::starter
{

struct HeadlessTestConfiguration final
{
    gpu::ScenePreset scene{gpu::ScenePreset::Default};
    std::uint32_t capacity{gpu::kCandidateCount};
};

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

    void ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept;
    [[nodiscard]] gpu::CpuReference const &LastReference() const noexcept;

  private:
    struct FrameSlotResources final
    {
        gpu::BufferResource candidateBuffer{};
        lgp::framework::DescriptorAllocation candidateSrv{};
    };

    [[nodiscard]] lgp::framework::Status CreateShaders();
    [[nodiscard]] lgp::framework::Status CreateGraphicsRootSignature();
    [[nodiscard]] lgp::framework::Status CreateGraphicsPipeline();
    [[nodiscard]] lgp::framework::Status CreateCandidateResources();
    [[nodiscard]] gpu::LabConfiguration ActiveConfiguration() const noexcept;

    lgp::framework::DeviceResources *deviceResources_{};
    lgp::framework::CompiledShader vertexShader_{};
    lgp::framework::CompiledShader pixelShader_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipeline_{};
    std::vector<FrameSlotResources> frameSlots_{};
    gpu::CpuReference currentReference_{};
    std::optional<HeadlessTestConfiguration> headlessConfiguration_{};
    bool headless_{false};
};

} // namespace ch13::work_distribution::starter
