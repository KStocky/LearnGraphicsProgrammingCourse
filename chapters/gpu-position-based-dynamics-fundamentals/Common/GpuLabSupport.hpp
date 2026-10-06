#pragma once

#include "PbdContracts.hpp"

#include <lgp/framework.hpp>

#include <array>
#include <filesystem>
#include <optional>

namespace ch40::pbd::gpu
{

// Every region includes a capacity sentinel; unused records retain the same bits.
inline constexpr std::uint32_t kGuard = 0x5A17C0DEU;
inline constexpr std::uint32_t kA = 0U;
inline constexpr std::uint32_t kB = 1056U;
inline constexpr std::uint32_t kPredicted = 2112U;
inline constexpr std::uint32_t kLambdas = 3168U;
inline constexpr std::uint32_t kCorrections = 3312U;
inline constexpr std::uint32_t kIntegers = 4368U;
inline constexpr std::uint32_t kMetrics = 4896U;
inline constexpr std::uint32_t kStatus = 7040U;
inline constexpr std::uint32_t kArenaTail = 7056U;
inline constexpr std::uint32_t kArenaBytes = 7072U;
inline constexpr std::uint32_t kConstraintInput = 1024U;
inline constexpr std::uint32_t kInputBytes = 2560U;

enum class Stage : std::uint32_t
{
    FreeMotion,
    Colored,
    Compliance,
    Jacobi,
    Atomic,
    All
};

[[nodiscard]] constexpr std::uint32_t StrategyCount(Stage stage) noexcept
{
    if (stage == Stage::FreeMotion || stage >= Stage::Atomic)
    {
        return 3U;
    }
    if (stage == Stage::Jacobi)
    {
        return 2U;
    }
    return 1U;
}

struct Constants final
{
    std::uint32_t particles{};
    std::uint32_t constraints{};
    std::uint32_t inputOffset{kA};
    std::uint32_t outputOffset{kB};
    float dt{};
    float relaxation{};
    std::uint32_t color{};
    std::uint32_t metricIndex{};
    Vector3 gravity{};
    std::uint32_t strategy{};
    std::uint32_t iterations{};
    std::uint32_t project{};
    std::uint32_t view{};
    std::uint32_t complianceEnabled{};
};
static_assert(sizeof(Constants) == 64U);

struct FrameReadback final
{
    StepResult step{};
    std::vector<Correction> corrections{};
    std::optional<Error> error{};
    bool guardsIntact{};
    std::uint32_t executedIterations{};
    std::uint32_t frameSlot{};
};

struct Slot final
{
    lgp::framework::Buffer input{};
    lgp::framework::Buffer seed{};
    lgp::framework::Buffer arena{};
    lgp::framework::Buffer readback{};
    bool used{};
    Constants constants{};
};

[[nodiscard]] lgp::framework::Result<Slot> CreateSlot(ID3D12Device &device);
[[nodiscard]] lgp::framework::Status UploadScene(Slot &slot, Scene const &scene);
[[nodiscard]] lgp::framework::Result<Microsoft::WRL::ComPtr<ID3D12RootSignature>> CreateRootSignature(
    ID3D12Device &device);
[[nodiscard]] lgp::framework::Result<Microsoft::WRL::ComPtr<ID3D12PipelineState>> CreateComputePipeline(
    ID3D12Device &device, ID3D12RootSignature &root, lgp::framework::CompiledShader const &shader);
[[nodiscard]] lgp::framework::Result<Microsoft::WRL::ComPtr<ID3D12PipelineState>> CreateGraphicsPipeline(
    ID3D12Device &device, ID3D12RootSignature &root, lgp::framework::CompiledShader const &vertex,
    lgp::framework::CompiledShader const &pixel, DXGI_FORMAT format);
void BufferBarrier(ID3D12GraphicsCommandList7 &list, ID3D12Resource &resource, D3D12_BARRIER_SYNC beforeSync,
                   D3D12_BARRIER_ACCESS beforeAccess, D3D12_BARRIER_SYNC afterSync,
                   D3D12_BARRIER_ACCESS afterAccess) noexcept;
[[nodiscard]] lgp::framework::Result<FrameReadback> DecodeReadback(Slot const &slot, std::uint32_t frameSlot);

} // namespace ch40::pbd::gpu
