#ifndef CH41_CLOTH_GPU_LAB_SUPPORT_HPP
#define CH41_CLOTH_GPU_LAB_SUPPORT_HPP
#include "ClothContracts.hpp"
#include <lgp/framework.hpp>
#include <optional>

namespace ch41::cloth::gpu
{
inline constexpr std::uint32_t kGuard = 0x5A17C0DEU;
inline constexpr std::uint32_t kP = 0U, kStart = 1056U, kSnapshot = 2112U, kLambda = 3168U;
inline constexpr std::uint32_t kActive = 3696U, kFaces = 4224U, kTets = 4496U, kKeys = 4544U;
inline constexpr std::uint32_t kMetrics = 4816U, kStatus = 4880U, kTail = 4912U, kArenaBytes = 4928U;
inline constexpr std::uint32_t kConstraintInput = 1024U, kTriangleInput = 7168U, kTetInput = 8192U;
inline constexpr std::uint32_t kInputBytes = 8336U;
struct Constants final
{
    std::uint32_t particles{}, constraints{}, triangles{}, tetrahedra{};
    std::uint32_t stage{}, color{}, iterations{}, flags{};
    float dt{}, friction{}, complianceScale{}, motionLimit{};
    Vector3 gravity{};
    std::uint32_t padding{};
};
static_assert(sizeof(Constants) == 64U);
struct FrameReadback final
{
    State state{};
    Constants constants{};
    std::optional<Error> error{};
    bool guardsIntact{};
    std::uint32_t frameSlot{};
};
struct Slot final
{
    lgp::framework::Buffer input{}, seed{}, readback{};
    Constants constants{};
    std::uint64_t epoch{};
    bool used{};
};
lgp::framework::Result<Slot> CreateSlot(ID3D12Device &device);
lgp::framework::Status UploadScene(Slot &slot, Scene const &scene, bool resetRest);
lgp::framework::Result<Microsoft::WRL::ComPtr<ID3D12RootSignature>> CreateRootSignature(ID3D12Device &device);
lgp::framework::Result<Microsoft::WRL::ComPtr<ID3D12PipelineState>> CreateComputePipeline(
    ID3D12Device &device, ID3D12RootSignature &root, lgp::framework::CompiledShader const &shader);
lgp::framework::Result<Microsoft::WRL::ComPtr<ID3D12PipelineState>> CreateGraphicsPipeline(
    ID3D12Device &device, ID3D12RootSignature &root, lgp::framework::CompiledShader const &vertex,
    lgp::framework::CompiledShader const &pixel, DXGI_FORMAT format);
void BufferBarrier(ID3D12GraphicsCommandList7 &list, ID3D12Resource &resource, D3D12_BARRIER_SYNC beforeSync,
                   D3D12_BARRIER_ACCESS beforeAccess, D3D12_BARRIER_SYNC afterSync,
                   D3D12_BARRIER_ACCESS afterAccess) noexcept;
lgp::framework::Result<FrameReadback> DecodeReadback(Slot const &slot, std::uint32_t frameSlot);
} // namespace ch41::cloth::gpu
#endif
