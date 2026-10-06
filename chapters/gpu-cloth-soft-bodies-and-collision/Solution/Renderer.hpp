#define CH41_CLOTH_VARIANT_VALUE_starter 1
#define CH41_CLOTH_VARIANT_VALUE_solution 2
#define CH41_CLOTH_VARIANT_VALUE_IMPL(value) CH41_CLOTH_VARIANT_VALUE_##value
#define CH41_CLOTH_VARIANT_VALUE(value) CH41_CLOTH_VARIANT_VALUE_IMPL(value)
#if (CH41_CLOTH_VARIANT_VALUE(LGP_CLOTH_VARIANT) == 1 && !defined(CH41_CLOTH_STARTER_RENDERER_HPP)) ||                 \
    (CH41_CLOTH_VARIANT_VALUE(LGP_CLOTH_VARIANT) == 2 && !defined(CH41_CLOTH_SOLUTION_RENDERER_HPP))
#if CH41_CLOTH_VARIANT_VALUE(LGP_CLOTH_VARIANT) == 1
#define CH41_CLOTH_STARTER_RENDERER_HPP
#else
#define CH41_CLOTH_SOLUTION_RENDERER_HPP
#endif
#include "../Common/GpuLabSupport.hpp"

namespace ch41::cloth::LGP_CLOTH_VARIANT
{
inline constexpr Stage kStage = Stage::All;
class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    lgp::framework::Status OnResize(lgp::framework::DeviceResources &resources, lgp::framework::Extent2D size) override;
    lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    lgp::framework::Status Render(lgp::framework::FrameContext const &frame) override;
    void Shutdown(lgp::framework::DeviceResources &resources) noexcept override;
    std::expected<void, Error> Configure(Scene scene, Configuration configuration);
    void ResetSimulation() noexcept
    {
        resetPending_ = true;
        ++epoch_;
    }
    void Pause(bool paused) noexcept
    {
        configuration_.paused = paused;
    }
    static constexpr Stage ImplementedStage() noexcept
    {
        return kStage;
    }
    lgp::framework::Result<gpu::FrameReadback> ReadBackOutputs();

  private:
    enum class Pass : std::size_t
    {
        BeginTick,
        ResetLambda,
        Predict,
        Tear,
        Colored,
        ValidateVolumes,
        Environment,
        BinSort,
        SelfContact,
        Velocity,
        Measure,
        Count
    };
    void Dispatch(ID3D12GraphicsCommandList7 &list, gpu::Constants const &constants, Pass pass);
    lgp::framework::DeviceResources *resources_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, static_cast<std::size_t>(Pass::Count)> compute_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphics_{};
    std::vector<gpu::Slot> slots_{};
    lgp::framework::Buffer arena_{};
    Scene scene_{};
    Configuration configuration_{};
    std::optional<std::uint32_t> lastSlot_{};
    std::uint64_t epoch_{1U};
    bool resetPending_{true}, arenaUsed_{}, diagnosticReported_{}, softBody_{};
    std::uint32_t iterationChoice_{1U}, complianceChoice_{};
    HWND windowHandle_{};
    std::string settingsText_{};
};
} // namespace ch41::cloth::LGP_CLOTH_VARIANT
#endif
#undef CH41_CLOTH_VARIANT_VALUE
#undef CH41_CLOTH_VARIANT_VALUE_IMPL
#undef CH41_CLOTH_VARIANT_VALUE_solution
#undef CH41_CLOTH_VARIANT_VALUE_starter
