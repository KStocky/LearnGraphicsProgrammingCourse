#include "GpuLabSupport.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace ch11::reprojection::gpu
{
namespace
{

using Microsoft::WRL::ComPtr;

struct SurfaceSample final
{
    std::uint32_t identity{};
    Float4 color{};
    float currentLinearDepth{};
    float expectedPreviousLinearDepth{};
    ClipPosition currentClip{};
    ClipPosition previousClip{};
};

[[nodiscard]] constexpr Float4 MakeColor(float r, float g, float b, float a = 1.0F) noexcept
{
    return {r, g, b, a};
}

[[nodiscard]] constexpr ClipTransformRows MakeClipTransform(Float2 uvMin, Float2 uvMax, float deviceDepth,
                                                            float clipW) noexcept
{
    float const extentX = uvMax.x - uvMin.x;
    float const extentY = uvMax.y - uvMin.y;
    ClipTransformRows transform{};
    transform.rows = {{
        {2.0F * extentX, 0.0F, 0.0F, (2.0F * uvMin.x) - 1.0F},
        {0.0F, -2.0F * extentY, 0.0F, 1.0F - (2.0F * uvMin.y)},
        {0.0F, 0.0F, 0.0F, deviceDepth * clipW},
        {0.0F, 0.0F, 0.0F, clipW},
    }};
    return transform;
}

[[nodiscard]] constexpr Float2 RectMin(Float4 rect) noexcept
{
    return {rect.x, rect.y};
}

[[nodiscard]] constexpr Float2 RectMax(Float4 rect) noexcept
{
    return {rect.z, rect.w};
}

[[nodiscard]] constexpr Float4 MakeRect(float minX, float minY, float maxX, float maxY) noexcept
{
    return {minX, minY, maxX, maxY};
}

[[nodiscard]] constexpr SurfaceState MakeSurface(Float4 rect, Float4 color, std::uint32_t identity,
                                                 float currentLinearDepth, Float4 previousRect,
                                                 float expectedPreviousLinearDepth, float previousClipW = 1.0F) noexcept
{
    return {
        .currentRectUv = rect,
        .currentColor = color,
        .currentIdentity = identity,
        .currentLinearDepth = currentLinearDepth,
        .expectedPreviousLinearDepth = expectedPreviousLinearDepth,
        .currentClipFromLocal = MakeClipTransform(RectMin(rect), RectMax(rect), currentLinearDepth / 16.0F, 1.0F),
        .previousClipFromLocal = MakeClipTransform(RectMin(previousRect), RectMax(previousRect),
                                                   expectedPreviousLinearDepth / 16.0F, previousClipW),
    };
}

[[nodiscard]] bool IsFinite(float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool TryResolveLocal(Float4 rect, Float2 rasterUv, Float2 jitterUv, Float2 &localUv) noexcept
{
    float const minX = rect.x + jitterUv.x;
    float const minY = rect.y + jitterUv.y;
    float const maxX = rect.z + jitterUv.x;
    float const maxY = rect.w + jitterUv.y;
    float const extentX = maxX - minX;
    float const extentY = maxY - minY;
    if (extentX <= 0.0F || extentY <= 0.0F)
    {
        return false;
    }

    localUv.x = (rasterUv.x - minX) / extentX;
    localUv.y = (rasterUv.y - minY) / extentY;
    return localUv.x >= 0.0F && localUv.x <= 1.0F && localUv.y >= 0.0F && localUv.y <= 1.0F;
}

[[nodiscard]] ClipPosition Transform(ClipTransformRows const &transform, Float2 localUv) noexcept
{
    float const px = localUv.x;
    float const py = localUv.y;
    float const pz = 0.0F;
    float const pw = 1.0F;

    auto const dot = [px, py, pz, pw](Float4 row) noexcept
    { return (row.x * px) + (row.y * py) + (row.z * pz) + (row.w * pw); };

    return {
        .x = dot(transform.rows[0]),
        .y = dot(transform.rows[1]),
        .z = dot(transform.rows[2]),
        .w = dot(transform.rows[3]),
    };
}

[[nodiscard]] Float2 RasterUv(std::uint32_t x, std::uint32_t y, lgp::framework::Extent2D size) noexcept
{
    return {(static_cast<float>(x) + 0.5F) / static_cast<float>(size.width),
            (static_cast<float>(y) + 0.5F) / static_cast<float>(size.height)};
}

[[nodiscard]] Float4 BilinearSample(std::vector<Float4> const &data, lgp::framework::Extent2D size, Float2 uv) noexcept
{
    if (data.empty() || size.empty())
    {
        return {};
    }

    float const clampedX = std::clamp(uv.x, 0.0F, 1.0F);
    float const clampedY = std::clamp(uv.y, 0.0F, 1.0F);
    float const sampleX = (clampedX * static_cast<float>(size.width)) - 0.5F;
    float const sampleY = (clampedY * static_cast<float>(size.height)) - 0.5F;
    int const x0 = static_cast<int>(std::floor(sampleX));
    int const y0 = static_cast<int>(std::floor(sampleY));
    int const x1 = x0 + 1;
    int const y1 = y0 + 1;
    float const fracX = sampleX - static_cast<float>(x0);
    float const fracY = sampleY - static_cast<float>(y0);

    auto const sample = [&](int x, int y) noexcept -> Float4
    {
        std::uint32_t const sx = static_cast<std::uint32_t>(std::clamp(x, 0, static_cast<int>(size.width) - 1));
        std::uint32_t const sy = static_cast<std::uint32_t>(std::clamp(y, 0, static_cast<int>(size.height) - 1));
        return data[(static_cast<std::size_t>(sy) * size.width) + sx];
    };

    Float4 const c00 = sample(x0, y0);
    Float4 const c10 = sample(x1, y0);
    Float4 const c01 = sample(x0, y1);
    Float4 const c11 = sample(x1, y1);

    auto const lerp = [](float a, float b, float t) noexcept { return a + ((b - a) * t); };
    Float4 const row0{
        lerp(c00.x, c10.x, fracX),
        lerp(c00.y, c10.y, fracX),
        lerp(c00.z, c10.z, fracX),
        lerp(c00.w, c10.w, fracX),
    };
    Float4 const row1{
        lerp(c01.x, c11.x, fracX),
        lerp(c01.y, c11.y, fracX),
        lerp(c01.z, c11.z, fracX),
        lerp(c01.w, c11.w, fracX),
    };
    return {
        lerp(row0.x, row1.x, fracY),
        lerp(row0.y, row1.y, fracY),
        lerp(row0.z, row1.z, fracY),
        lerp(row0.w, row1.w, fracY),
    };
}

[[nodiscard]] std::uint32_t PointSample(std::vector<std::uint32_t> const &data, lgp::framework::Extent2D size,
                                        Float2 uv) noexcept
{
    if (data.empty() || size.empty())
    {
        return 0U;
    }

    float const clampedX = std::clamp(uv.x, 0.0F, std::nextafter(1.0F, 0.0F));
    float const clampedY = std::clamp(uv.y, 0.0F, std::nextafter(1.0F, 0.0F));
    std::uint32_t const x = static_cast<std::uint32_t>(std::floor(clampedX * static_cast<float>(size.width)));
    std::uint32_t const y = static_cast<std::uint32_t>(std::floor(clampedY * static_cast<float>(size.height)));
    return data[(static_cast<std::size_t>(y) * size.width) + x];
}

[[nodiscard]] float PointSample(std::vector<float> const &data, lgp::framework::Extent2D size, Float2 uv) noexcept
{
    if (data.empty() || size.empty())
    {
        return 0.0F;
    }

    float const clampedX = std::clamp(uv.x, 0.0F, std::nextafter(1.0F, 0.0F));
    float const clampedY = std::clamp(uv.y, 0.0F, std::nextafter(1.0F, 0.0F));
    std::uint32_t const x = static_cast<std::uint32_t>(std::floor(clampedX * static_cast<float>(size.width)));
    std::uint32_t const y = static_cast<std::uint32_t>(std::floor(clampedY * static_cast<float>(size.height)));
    return data[(static_cast<std::size_t>(y) * size.width) + x];
}

[[nodiscard]] float LocalPreviousDepthGradient(std::vector<float> const &data, lgp::framework::Extent2D size,
                                               Float2 uv) noexcept
{
    if (data.empty() || size.empty())
    {
        return 0.0F;
    }

    Float2 const texel{1.0F / static_cast<float>(size.width), 1.0F / static_cast<float>(size.height)};
    float const center = PointSample(data, size, uv);
    float const left = PointSample(data, size, {uv.x - texel.x, uv.y});
    float const right = PointSample(data, size, {uv.x + texel.x, uv.y});
    float const up = PointSample(data, size, {uv.x, uv.y - texel.y});
    float const down = PointSample(data, size, {uv.x, uv.y + texel.y});
    return (std::max)((std::max)(std::abs(center - left), std::abs(right - center)),
                      (std::max)(std::abs(center - up), std::abs(down - center)));
}

[[nodiscard]] SurfaceSample ResolveCurrentSurface(ScenarioState const &scenario, Float2 rasterUv) noexcept
{
    SurfaceSample sample{
        .identity = 0U,
        .color = MakeColor(0.0F, 0.0F, 0.0F),
        .currentLinearDepth = std::numeric_limits<float>::infinity(),
    };
    bool found = false;

    for (std::uint32_t index = 0U; index < scenario.surfaceCount; ++index)
    {
        SurfaceState const &surface = scenario.surfaces[index];
        Float2 localUv{};
        if (!TryResolveLocal(surface.currentRectUv, rasterUv, scenario.currentJitterUv, localUv))
        {
            continue;
        }
        if (!found || surface.currentLinearDepth < sample.currentLinearDepth)
        {
            sample.identity = surface.currentIdentity;
            sample.color = surface.currentColor;
            sample.currentLinearDepth = surface.currentLinearDepth;
            sample.expectedPreviousLinearDepth = surface.expectedPreviousLinearDepth;
            sample.currentClip = Transform(surface.currentClipFromLocal, localUv);
            sample.previousClip = Transform(surface.previousClipFromLocal, localUv);
            found = true;
        }
    }

    return sample;
}

[[nodiscard]] Float4 ScaleColor(Float4 color, float scale) noexcept
{
    if (!IsFinite(scale))
    {
        return {};
    }
    return {color.x * scale, color.y * scale, color.z * scale, color.w};
}

[[nodiscard]] Float4 ReasonTint(HistoryRejectReason reasons) noexcept
{
    if (HasReason(reasons, HistoryRejectReason::NoHistory) || HasReason(reasons, HistoryRejectReason::Reset))
    {
        return MakeColor(1.0F, 0.0F, 1.0F);
    }
    if (HasReason(reasons, HistoryRejectReason::NonFinite))
    {
        return MakeColor(1.0F, 1.0F, 1.0F);
    }
    if (HasReason(reasons, HistoryRejectReason::PreviousW))
    {
        return MakeColor(0.0F, 0.5F, 1.0F);
    }
    if (HasReason(reasons, HistoryRejectReason::UvOutOfBounds))
    {
        return MakeColor(1.0F, 1.0F, 0.0F);
    }
    if (HasReason(reasons, HistoryRejectReason::DepthMismatch))
    {
        return MakeColor(1.0F, 0.0F, 0.0F);
    }
    if (HasReason(reasons, HistoryRejectReason::IdentityMismatch))
    {
        return MakeColor(0.0F, 1.0F, 1.0F);
    }
    if (HasReason(reasons, HistoryRejectReason::Exposure))
    {
        return MakeColor(1.0F, 0.5F, 0.0F);
    }
    return MakeColor(0.125F, 0.125F, 0.125F);
}

[[nodiscard]] Float4 CompositeDiagnostic(Float4 currentColor, Float4 historyColor, HistoryRejectReason reasons) noexcept
{
    auto const saturate = [](float value) noexcept { return std::clamp(value, 0.0F, 1.0F); };

    if (reasons == HistoryRejectReason::None)
    {
        return {
            saturate((0.5F * currentColor.x) + (0.5F * historyColor.x)),
            saturate((0.5F * currentColor.y) + (0.5F * historyColor.y) + 0.125F),
            saturate((0.5F * currentColor.z) + (0.5F * historyColor.z)),
            1.0F,
        };
    }

    Float4 const tint = ReasonTint(reasons);
    return {
        saturate((0.25F * currentColor.x) + (0.75F * tint.x)),
        saturate((0.25F * currentColor.y) + (0.75F * tint.y)),
        saturate((0.25F * currentColor.z) + (0.75F * tint.z)),
        1.0F,
    };
}

[[nodiscard]] std::array<std::uint8_t, 4U> ToUnorm8(Float4 color) noexcept
{
    auto const convert = [](float value) noexcept
    { return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F)); };

    return {convert(color.x), convert(color.y), convert(color.z), convert(color.w)};
}

[[nodiscard]] UINT64 BytesPerPixel(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
        return 8U;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
        return 4U;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16U;
    default:
        return 0U;
    }
}

[[nodiscard]] lgp::framework::Error MakeUnsupportedReadbackFormatError(DXGI_FORMAT format)
{
    return lgp::framework::MakeError("ReadBackTexture",
                                     "Unsupported readback format " + std::to_string(static_cast<int>(format)) + ".");
}

} // namespace

lgp::framework::TextureBarrierState UndefinedState() noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_UNDEFINED};
}

lgp::framework::TextureBarrierState ComputeUnorderedAccessState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
            D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS};
}

lgp::framework::TextureBarrierState ComputeShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
            D3D12_BARRIER_LAYOUT_SHADER_RESOURCE};
}

lgp::framework::TextureBarrierState PixelShaderResourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
            D3D12_BARRIER_LAYOUT_SHADER_RESOURCE};
}

lgp::framework::TextureBarrierState RenderTargetState() noexcept
{
    return {D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
}

lgp::framework::TextureBarrierState CopySourceState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_SOURCE};
}

lgp::framework::TextureBarrierState CopyDestState() noexcept
{
    return {D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_DEST};
}

lgp::framework::TextureBarrierState FrameStartState(bool headless) noexcept
{
    return {D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
            headless ? D3D12_BARRIER_LAYOUT_COMMON : D3D12_BARRIER_LAYOUT_PRESENT};
}

lgp::framework::TextureBarrierState FrameEndState(bool headless) noexcept
{
    return FrameStartState(headless);
}

D3D12_RESOURCE_DESC1 MakeTextureDescription(lgp::framework::Extent2D size, DXGI_FORMAT format,
                                            D3D12_RESOURCE_FLAGS flags) noexcept
{
    D3D12_RESOURCE_DESC1 description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = size.width;
    description.Height = size.height;
    description.DepthOrArraySize = 1U;
    description.MipLevels = 1U;
    description.Format = format;
    description.SampleDesc.Count = 1U;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;
    return description;
}

D3D12_TEXTURE_BARRIER MakeTextureBarrier(ID3D12Resource &resource, lgp::framework::TextureBarrierState before,
                                         lgp::framework::TextureBarrierState after,
                                         D3D12_TEXTURE_BARRIER_FLAGS flags) noexcept
{
    D3D12_TEXTURE_BARRIER barrier{};
    barrier.SyncBefore = before.sync;
    barrier.SyncAfter = after.sync;
    barrier.AccessBefore = before.access;
    barrier.AccessAfter = after.access;
    barrier.LayoutBefore = before.layout;
    barrier.LayoutAfter = after.layout;
    barrier.pResource = &resource;
    barrier.Subresources.IndexOrFirstMipLevel = UINT32_MAX;
    barrier.Flags = flags;
    return barrier;
}

void SubmitTextureBarriers(ID3D12GraphicsCommandList7 &commandList, std::vector<D3D12_TEXTURE_BARRIER> &barriers)
{
    if (barriers.empty())
    {
        return;
    }

    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = static_cast<UINT>(barriers.size());
    group.pTextureBarriers = barriers.data();
    commandList.Barrier(1U, &group);
}

ScenarioState MakeScenarioState(Scenario scenario) noexcept
{
    ScenarioState state{};
    state.scenario = scenario;

    auto const add = [&state](SurfaceState const &surface) noexcept
    {
        state.surfaces[state.surfaceCount] = surface;
        ++state.surfaceCount;
    };

    switch (scenario)
    {
    case Scenario::Static:
    case Scenario::JitterA:
    case Scenario::JitterB:
    case Scenario::ExposurePrevious:
    case Scenario::ExposureValidCurrent:
    case Scenario::ExposureExcessiveCurrent:
    case Scenario::PreviousWCurrent:
        add(MakeSurface(MakeRect(0.0F, 0.0F, 1.0F, 1.0F), MakeColor(0.125F, 0.25F, 0.5F), 1U, 8.0F,
                        MakeRect(0.0F, 0.0F, 1.0F, 1.0F), 8.0F, scenario == Scenario::PreviousWCurrent ? -1.0F : 1.0F));
        add(MakeSurface(MakeRect(0.25F, 0.25F, 0.625F, 0.75F), MakeColor(0.875F, 0.25F, 0.125F), 7U, 4.0F,
                        MakeRect(0.25F, 0.25F, 0.625F, 0.75F), 4.0F,
                        scenario == Scenario::PreviousWCurrent ? -1.0F : 1.0F));
        break;

    case Scenario::MotionPrevious:
        add(MakeSurface(MakeRect(0.0F, 0.0F, 1.0F, 1.0F), MakeColor(0.125F, 0.25F, 0.5F), 1U, 8.0F,
                        MakeRect(0.0F, 0.0F, 1.0F, 1.0F), 8.0F));
        add(MakeSurface(MakeRect(0.5F, 0.25F, 0.875F, 0.75F), MakeColor(0.875F, 0.25F, 0.125F), 7U, 4.0F,
                        MakeRect(0.5F, 0.25F, 0.875F, 0.75F), 4.0F));
        break;

    case Scenario::MotionCurrent:
        add(MakeSurface(MakeRect(0.0F, 0.0F, 1.0F, 1.0F), MakeColor(0.125F, 0.25F, 0.5F), 1U, 8.0F,
                        MakeRect(0.125F, 0.0F, 1.125F, 1.0F), 8.0F));
        add(MakeSurface(MakeRect(0.25F, 0.25F, 0.625F, 0.75F), MakeColor(0.875F, 0.25F, 0.125F), 7U, 4.0F,
                        MakeRect(0.5F, 0.25F, 0.875F, 0.75F), 4.0F));
        break;

    case Scenario::IdentityDepthPrevious:
        add(MakeSurface(MakeRect(0.0F, 0.0F, 1.0F, 1.0F), MakeColor(0.125F, 0.25F, 0.5F), 1U, 8.0F,
                        MakeRect(0.0F, 0.0F, 1.0F, 1.0F), 8.0F));
        add(MakeSurface(MakeRect(0.125F, 0.25F, 0.375F, 0.75F), MakeColor(0.125F, 0.875F, 0.25F), 7U, 3.5F,
                        MakeRect(0.125F, 0.25F, 0.375F, 0.75F), 3.5F));
        add(MakeSurface(MakeRect(0.5F, 0.25F, 0.75F, 0.75F), MakeColor(0.75F, 0.75F, 0.125F), 9U, 4.0F,
                        MakeRect(0.5F, 0.25F, 0.75F, 0.75F), 4.0F));
        break;

    case Scenario::IdentityDepthCurrent:
        add(MakeSurface(MakeRect(0.0F, 0.0F, 1.0F, 1.0F), MakeColor(0.125F, 0.25F, 0.5F), 1U, 8.0F,
                        MakeRect(0.0F, 0.0F, 1.0F, 1.0F), 8.0F));
        add(MakeSurface(MakeRect(0.125F, 0.25F, 0.375F, 0.75F), MakeColor(0.125F, 0.875F, 0.25F), 8U, 3.5F,
                        MakeRect(0.125F, 0.25F, 0.375F, 0.75F), 3.5F));
        add(MakeSurface(MakeRect(0.5F, 0.25F, 0.75F, 0.75F), MakeColor(0.75F, 0.75F, 0.125F), 9U, 4.0F,
                        MakeRect(0.5F, 0.25F, 0.75F, 0.75F), 12.0F));
        break;
    }

    if (scenario == Scenario::JitterA)
    {
        state.currentJitterUv = {0.03125F, -0.03125F};
    }
    else if (scenario == Scenario::JitterB)
    {
        state.currentJitterUv = {-0.03125F, 0.03125F};
    }

    if (scenario == Scenario::ExposureValidCurrent)
    {
        state.currentPreExposure = 2.0F;
    }
    else if (scenario == Scenario::ExposureExcessiveCurrent)
    {
        state.currentPreExposure = 8.0F;
    }

    return state;
}

std::string_view ScenarioLabel(Scenario scenario) noexcept
{
    switch (scenario)
    {
    case Scenario::Static:
        return "Static";
    case Scenario::JitterA:
        return "Jitter A";
    case Scenario::JitterB:
        return "Jitter B";
    case Scenario::MotionPrevious:
        return "Motion Previous";
    case Scenario::MotionCurrent:
        return "Motion Current";
    case Scenario::IdentityDepthPrevious:
        return "Identity/Depth Previous";
    case Scenario::IdentityDepthCurrent:
        return "Identity/Depth Current";
    case Scenario::ExposurePrevious:
        return "Exposure Previous";
    case Scenario::ExposureValidCurrent:
        return "Exposure Valid";
    case Scenario::ExposureExcessiveCurrent:
        return "Exposure Excessive";
    case Scenario::PreviousWCurrent:
        return "Previous W Invalid";
    }
    return "Unknown";
}

HistoryBuffers MakeEmptyHistory(lgp::framework::Extent2D size)
{
    std::size_t const pixelCount = static_cast<std::size_t>(size.width) * size.height;
    HistoryBuffers history{};
    history.color.assign(pixelCount, {});
    history.linearDepth.assign(pixelCount, 0.0F);
    history.identity.assign(pixelCount, 0U);
    return history;
}

ReferenceOutputs SimulateFrame(ScenarioState const &scenario, HistoryBuffers const &previousHistory,
                               lgp::framework::Extent2D size, bool resetRequested)
{
    std::size_t const pixelCount = static_cast<std::size_t>(size.width) * size.height;
    ReferenceOutputs outputs{};
    outputs.currentColor.resize(pixelCount);
    outputs.currentLinearDepth.resize(pixelCount);
    outputs.currentIdentity.resize(pixelCount);
    outputs.motionClipDepth.resize(pixelCount);
    outputs.previousHistoryUv.resize(pixelCount);
    outputs.reprojectedHistoryColor.resize(pixelCount);
    outputs.rejectionReasons.resize(pixelCount);
    outputs.exposureScale.resize(pixelCount);
    outputs.composite.resize(pixelCount);
    outputs.capturedHistory = MakeEmptyHistory(size);
    outputs.capturedHistory.hasHistory = true;
    outputs.capturedHistory.jitterUv = scenario.currentJitterUv;
    outputs.capturedHistory.preExposure = scenario.currentPreExposure;

    HistoryValidationSettings const validationSettings{
        .renderWidth = size.width,
        .renderHeight = size.height,
    };

    for (std::uint32_t y = 0U; y < size.height; ++y)
    {
        for (std::uint32_t x = 0U; x < size.width; ++x)
        {
            std::size_t const index = (static_cast<std::size_t>(y) * size.width) + x;
            Float2 const rasterUv = RasterUv(x, y, size);
            SurfaceSample const currentSurface = ResolveCurrentSurface(scenario, rasterUv);

            outputs.currentColor[index] = currentSurface.color;
            outputs.currentLinearDepth[index] = currentSurface.currentLinearDepth;
            outputs.currentIdentity[index] = currentSurface.identity;
            outputs.capturedHistory.color[index] = currentSurface.color;
            outputs.capturedHistory.linearDepth[index] = currentSurface.currentLinearDepth;
            outputs.capturedHistory.identity[index] = currentSurface.identity;

            auto const motion = ComputeUnjitteredMotion(currentSurface.currentClip, currentSurface.previousClip);
            Float2 previousHistoryUv{};
            if (motion)
            {
                previousHistoryUv =
                    ReprojectToHistoryUv(rasterUv, *motion, scenario.currentJitterUv, previousHistory.jitterUv);
                outputs.motionClipDepth[index] = {motion->x, motion->y, currentSurface.previousClip.w,
                                                  currentSurface.expectedPreviousLinearDepth};
            }
            else
            {
                previousHistoryUv = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
                outputs.motionClipDepth[index] = {
                    std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                    currentSurface.previousClip.w, currentSurface.expectedPreviousLinearDepth};
            }
            outputs.previousHistoryUv[index] = previousHistoryUv;

            Float4 const sampledColor = BilinearSample(previousHistory.color, size, previousHistoryUv);
            float const sampledDepth = PointSample(previousHistory.linearDepth, size, previousHistoryUv);
            std::uint32_t const sampledIdentity = PointSample(previousHistory.identity, size, previousHistoryUv);
            float const localGradient =
                LocalPreviousDepthGradient(previousHistory.linearDepth, size, previousHistoryUv);

            HistoryValidationInput input{
                .hasHistory = previousHistory.hasHistory,
                .resetRequested = resetRequested,
                .previousHistoryUv = previousHistoryUv,
                .previousClipW = currentSurface.previousClip.w,
                .expectedPreviousViewDepth = currentSurface.expectedPreviousLinearDepth,
                .sampledPreviousViewDepth = sampledDepth,
                .localPreviousDepthGradient = localGradient,
                .currentIdentity = currentSurface.identity,
                .sampledPreviousIdentity = sampledIdentity,
                .currentPreExposure = scenario.currentPreExposure,
                .previousPreExposure = previousHistory.preExposure,
            };
            HistoryValidationResult validation = ValidateHistory(input, validationSettings);

            if (!motion)
            {
                if (motion.error() == ProjectionError::NonFinite)
                {
                    validation.reasons |= HistoryRejectReason::NonFinite;
                }
                else
                {
                    validation.reasons |= HistoryRejectReason::PreviousW;
                }
            }

            outputs.reprojectedHistoryColor[index] = ScaleColor(sampledColor, validation.historyToCurrentExposureScale);
            outputs.rejectionReasons[index] = static_cast<std::uint32_t>(validation.reasons);
            outputs.exposureScale[index] = validation.historyToCurrentExposureScale;
            outputs.composite[index] = ToUnorm8(
                CompositeDiagnostic(currentSurface.color, outputs.reprojectedHistoryColor[index], validation.reasons));
        }
    }

    return outputs;
}

std::expected<TextureReadback, lgp::framework::Error> ReadBackTexture(lgp::framework::DeviceResources &deviceResources,
                                                                      ID3D12Resource &resource,
                                                                      lgp::framework::TextureBarrierState currentState)
{
    auto const idle = deviceResources.WaitForGpuIdle();
    if (!idle)
    {
        return std::unexpected(idle.error());
    }

    D3D12_RESOURCE_DESC const sourceDescription = resource.GetDesc();
    UINT64 const bytesPerPixel = BytesPerPixel(sourceDescription.Format);
    if (bytesPerPixel == 0U)
    {
        return std::unexpected(MakeUnsupportedReadbackFormatError(sourceDescription.Format));
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rowCount = 0U;
    UINT64 rowSize = 0U;
    UINT64 totalBytes = 0U;
    deviceResources.device()->GetCopyableFootprints(&sourceDescription, 0U, 1U, 0U, &footprint, &rowCount, &rowSize,
                                                    &totalBytes);

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readbackDescription{};
    readbackDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDescription.Width = totalBytes;
    readbackDescription.Height = 1U;
    readbackDescription.DepthOrArraySize = 1U;
    readbackDescription.MipLevels = 1U;
    readbackDescription.SampleDesc.Count = 1U;
    readbackDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readbackBuffer{};
    HRESULT const readbackResult = deviceResources.device()->CreateCommittedResource(
        &readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(readbackBuffer.ReleaseAndGetAddressOf()));
    if (FAILED(readbackResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateCommittedResource", readbackResult,
                                             "Failed to create a Chapter 11 texture readback buffer."));
    }

    ComPtr<ID3D12CommandAllocator> allocator{};
    HRESULT const allocatorResult = deviceResources.device()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.ReleaseAndGetAddressOf()));
    if (FAILED(allocatorResult))
    {
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Device::CreateCommandAllocator", allocatorResult,
                                             "Failed to create a Chapter 11 readback command allocator."));
    }

    ComPtr<ID3D12GraphicsCommandList7> commandList{};
    HRESULT const listResult =
        deviceResources.device()->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                    IID_PPV_ARGS(commandList.ReleaseAndGetAddressOf()));
    if (FAILED(listResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateCommandList", listResult,
                                                                "Failed to create a Chapter 11 readback list."));
    }

    std::vector<D3D12_TEXTURE_BARRIER> barriers{
        MakeTextureBarrier(resource, currentState, CopySourceState()),
    };
    SubmitTextureBarriers(*commandList.Get(), barriers);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readbackBuffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = &resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0U;
    commandList->CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);

    barriers = {
        MakeTextureBarrier(resource, CopySourceState(), currentState),
    };
    SubmitTextureBarriers(*commandList.Get(), barriers);

    HRESULT const closeResult = commandList->Close();
    if (FAILED(closeResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12GraphicsCommandList::Close", closeResult,
                                                                "Failed to close a Chapter 11 readback list."));
    }

    ID3D12CommandList *const lists[]{commandList.Get()};
    deviceResources.graphics_queue()->ExecuteCommandLists(1U, lists);

    ComPtr<ID3D12Fence> fence{};
    HRESULT const fenceResult =
        deviceResources.device()->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.ReleaseAndGetAddressOf()));
    if (FAILED(fenceResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Device::CreateFence", fenceResult,
                                                                "Failed to create a Chapter 11 readback fence."));
    }

    HANDLE const eventHandle = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (eventHandle == nullptr)
    {
        return std::unexpected(
            lgp::framework::MakeLastError("CreateEventW", "Failed to create a Chapter 11 readback event."));
    }

    constexpr std::uint64_t kFenceValue = 1U;
    HRESULT const signalResult = deviceResources.graphics_queue()->Signal(fence.Get(), kFenceValue);
    if (FAILED(signalResult))
    {
        ::CloseHandle(eventHandle);
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12CommandQueue::Signal", signalResult,
                                                                "Failed to signal a Chapter 11 readback fence."));
    }

    HRESULT const setEventResult = fence->SetEventOnCompletion(kFenceValue, eventHandle);
    if (FAILED(setEventResult))
    {
        ::CloseHandle(eventHandle);
        return std::unexpected(
            lgp::framework::MakeHResultError("ID3D12Fence::SetEventOnCompletion", setEventResult,
                                             "Failed to register a Chapter 11 readback completion event."));
    }

    DWORD const waitResult = ::WaitForSingleObject(eventHandle, INFINITE);
    ::CloseHandle(eventHandle);
    if (waitResult != WAIT_OBJECT_0)
    {
        return std::unexpected(
            lgp::framework::MakeLastError("WaitForSingleObject", "Failed while waiting for Chapter 11 readback."));
    }

    TextureReadback readback{};
    readback.size = {
        static_cast<std::uint32_t>(sourceDescription.Width),
        sourceDescription.Height,
    };
    readback.format = sourceDescription.Format;
    readback.rowPitch = footprint.Footprint.RowPitch;
    readback.bytes.resize(static_cast<std::size_t>(totalBytes));

    void *mappedData = nullptr;
    D3D12_RANGE const readRange{0U, static_cast<SIZE_T>(totalBytes)};
    HRESULT const mapResult = readbackBuffer->Map(0U, &readRange, &mappedData);
    if (FAILED(mapResult))
    {
        return std::unexpected(lgp::framework::MakeHResultError("ID3D12Resource::Map", mapResult,
                                                                "Failed to map a Chapter 11 readback buffer."));
    }

    std::memcpy(readback.bytes.data(), mappedData, static_cast<std::size_t>(totalBytes));
    D3D12_RANGE const writtenRange{0U, 0U};
    readbackBuffer->Unmap(0U, &writtenRange);
    return readback;
}

} // namespace ch11::reprojection::gpu
