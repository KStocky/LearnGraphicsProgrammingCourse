#pragma once

#include "../Common/GpuLabSupport.hpp"

namespace ch40::pbd::LGP_PBD_VARIANT
{

inline constexpr gpu::Stage kStage = gpu::Stage::All;

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &resources,
                                                  lgp::framework::Extent2D size) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frame) override;
    void Shutdown(lgp::framework::DeviceResources &resources) noexcept override;
    [[nodiscard]] std::expected<void, Error> Configure(Scene scene, Configuration configuration,
                                                       bool project = DefaultProjectionEnabled());
    [[nodiscard]] static constexpr gpu::Stage ImplementedStage() noexcept
    {
        return kStage;
    }
    [[nodiscard]] static constexpr bool DefaultProjectionEnabled() noexcept
    {
        return kStage != gpu::Stage::FreeMotion;
    }
    [[nodiscard]] bool ProjectionEnabled() const noexcept
    {
        return projectionEnabled_;
    }
    [[nodiscard]] lgp::framework::Result<gpu::FrameReadback> ReadBackOutputs();

  private:
    enum class Pass : std::size_t
    {
        Predict,
        Colored,
        Corrections,
        Gather,
        ResetIntegers,
        Accumulate,
        ApplyIntegers,
        Velocity,
        Measure,
        Guards,
        Count
    };
    void Dispatch(ID3D12GraphicsCommandList7 &list, gpu::Slot &slot, gpu::Constants const &constants, Pass pass);
    lgp::framework::DeviceResources *resources_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, static_cast<std::size_t>(Pass::Count)> compute_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphics_{};
    std::vector<gpu::Slot> slots_{};
    Scene scene_{};
    Configuration configuration_{};
    std::optional<std::uint32_t> lastSlot_{};
    std::uint32_t iterationChoice_{3U};
    std::uint32_t dtChoice_{1U};
    std::uint32_t complianceChoice_{};
    std::uint32_t massChoice_{};
    std::uint32_t view_{};
    bool pinned_{true};
    bool projectionEnabled_{DefaultProjectionEnabled()};
    HWND windowHandle_{};
    std::string settingsText_{};
    bool diagnosticReported_{};
};

} // namespace ch40::pbd::LGP_PBD_VARIANT
