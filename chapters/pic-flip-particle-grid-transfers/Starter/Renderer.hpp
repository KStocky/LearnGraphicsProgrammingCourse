#define CH43_VARIANT_starter 1
#define CH43_VARIANT_solution 2
#define CH43_VARIANT_IMPL(x) CH43_VARIANT_##x
#define CH43_VARIANT(x) CH43_VARIANT_IMPL(x)
#if (CH43_VARIANT(LGP_TRANSFER_VARIANT) == 1 && !defined(CH43_STARTER_HPP)) ||                                         \
    (CH43_VARIANT(LGP_TRANSFER_VARIANT) == 2 && !defined(CH43_SOLUTION_HPP))
#if CH43_VARIANT(LGP_TRANSFER_VARIANT) == 1
#define CH43_STARTER_HPP
#else
#define CH43_SOLUTION_HPP
#endif
#include "../Common/TransferContracts.hpp"
#include <lgp/framework.hpp>
#include <optional>

namespace ch43::transfers::LGP_TRANSFER_VARIANT
{
#include "Stage.hpp"
struct Constants final
{
    std::uint32_t count{}, stage{}, field{}, view{};
    float alpha{};
    Vec3 increment{};
};
static_assert(sizeof(Constants) == 32U && offsetof(Constants, increment) == 20U);
inline constexpr std::uint32_t kP = 0U, kSnapshot = 2064U, kFaceArena = 4128U, kSamples = 11824U;
inline constexpr std::uint32_t kMetrics = 13888U, kStatus = 13968U, kBytes = 14000U, kGuard = 0x5A17C0DEU;
struct Slot final
{
    lgp::framework::Buffer seed{}, readback{};
    Constants constants{};
    std::uint64_t epoch{};
    bool used{};
};
struct Readback final
{
    State state{};
    std::optional<Error> error{};
    Constants constants{};
    bool guardsIntact{};
    std::uint32_t frameSlot{};
    std::uint64_t epoch{};
};
class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    lgp::framework::Status OnResize(lgp::framework::DeviceResources &resources, lgp::framework::Extent2D size) override;
    lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    lgp::framework::Status Render(lgp::framework::FrameContext const &frame) override;
    void Shutdown(lgp::framework::DeviceResources &resources) noexcept override;
    std::expected<void, Error> Configure(Scene scene, Configuration configuration);
    void ResetExperiment() noexcept
    {
        resetPending_ = true;
        ++epoch_;
        diagnosticReported_ = false;
    }
    void Pause(bool paused) noexcept
    {
        configuration_.paused = paused;
    }
    static constexpr Stage ImplementedStage() noexcept
    {
        return kStage;
    }
    lgp::framework::Result<Readback> ReadBackOutputs();
    lgp::framework::Result<Readback> Decode(Slot const &slot, std::uint32_t index) const;
    std::expected<void, Error> InjectFault(Error fault) noexcept
    {
        if (fault != Error::Capacity && fault != Error::Parameters && fault != Error::NonFinite &&
            fault != Error::Mass && fault != Error::Domain && fault != Error::Identity)
            return std::unexpected(Error::Parameters);
        fault_ = fault;
        ResetExperiment();
        return {};
    }
    enum class Pass : std::size_t
    {
        Preflight,
        Snapshot,
        Clear,
        Scatter,
        Normalize,
        Transfer,
        Measure,
        Count
    };
    void Dispatch(ID3D12GraphicsCommandList7 &list, Constants const &constants, Pass pass);
    void Barrier(ID3D12GraphicsCommandList7 &list, D3D12_BARRIER_SYNC beforeSync, D3D12_BARRIER_ACCESS beforeAccess,
                 D3D12_BARRIER_SYNC afterSync, D3D12_BARRIER_ACCESS afterAccess) const noexcept;
    lgp::framework::Status Upload(Slot &slot);
    lgp::framework::DeviceResources *resources_{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, static_cast<std::size_t>(Pass::Count)> compute_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> graphics_{};
    lgp::framework::Buffer arena_{};
    std::vector<Slot> slots_{};
    Scene scene_{MakeBlock()};
    Configuration configuration_{};
    std::optional<std::uint32_t> lastSlot_{};
    std::optional<Error> fault_{};
    std::uint64_t epoch_{1U};
    bool resetPending_{true}, arenaUsed_{}, diagnosticReported_{};
    HWND windowHandle_{};
    std::string settingsText_{};
};
} // namespace ch43::transfers::LGP_TRANSFER_VARIANT
#endif
#undef CH43_VARIANT
#undef CH43_VARIANT_IMPL
#undef CH43_VARIANT_starter
#undef CH43_VARIANT_solution
