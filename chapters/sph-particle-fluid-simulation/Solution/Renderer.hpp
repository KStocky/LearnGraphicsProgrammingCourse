#define CH42_VARIANT_starter 1
#define CH42_VARIANT_solution 2
#define CH42_VARIANT_IMPL(x) CH42_VARIANT_##x
#define CH42_VARIANT(x) CH42_VARIANT_IMPL(x)
#if (CH42_VARIANT(LGP_SPH_VARIANT) == 1 && !defined(CH42_STARTER_HPP)) ||                                              \
    (CH42_VARIANT(LGP_SPH_VARIANT) == 2 && !defined(CH42_SOLUTION_HPP))
#if CH42_VARIANT(LGP_SPH_VARIANT) == 1
#define CH42_STARTER_HPP
#else
#define CH42_SOLUTION_HPP
#endif
#include "../Common/SphContracts.hpp"
#include "Stage.hpp"
#include <lgp/framework.hpp>
#include <optional>

namespace ch42::sph::LGP_SPH_VARIANT
{
struct Constants final
{
    std::uint32_t count{}, stage{}, useGrid{}, signedPressure{};
    float radius{}, restDensity{}, soundSpeed{}, dynamicViscosity{};
    float dt{}, restitution{}, motionLimit{};
    std::uint32_t view{};
    Vec3 gravity{};
    std::uint32_t walls{};
};
static_assert(sizeof(Constants) == 64U && offsetof(Constants, gravity) == 48U);
inline constexpr std::uint32_t kP = 0U, kSnapshot = 2064U, kSamples = 4128U, kKeys = 6192U;
inline constexpr std::uint32_t kMasks = 6720U, kStep = 7248U, kMetrics = 8288U, kStatus = 8384U, kBytes = 8416U;
inline constexpr std::uint32_t kGuard = 0x5A17C0DEU;
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
    std::vector<std::array<std::uint32_t, 2U>> keys{};
    std::array<float, 4U> kernelEvidence{};
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
    void ResetSimulation() noexcept
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
    // Controlled GPU validation experiments. Does not bypass application Configure.
    void InjectFault(Error fault) noexcept
    {
        fault_ = fault;
        ResetSimulation();
    }
    enum class Pass : std::size_t
    {
        Preflight,
        Snapshot,
        Sort,
        Density,
        Forces,
        Integrate,
        Measure,
        EndTick,
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
} // namespace ch42::sph::LGP_SPH_VARIANT
#endif
#undef CH42_VARIANT
#undef CH42_VARIANT_IMPL
#undef CH42_VARIANT_starter
#undef CH42_VARIANT_solution
