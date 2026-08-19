#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace ch03::texture
{

using DirectX::XMFLOAT2;
using DirectX::XMFLOAT4;

enum class AddressMode : std::uint8_t
{
    Clamp = 0,
    Wrap,
    Mirror,
};

enum class TextureSamplingError : std::uint8_t
{
    EmptyImage = 0,
    TexelDataSizeMismatch,
    InvalidExtent,
    InvalidCoordinate,
    InvalidMipLevelRange,
    InvalidFootprint,
    InvalidAnisotropy,
    NotImplemented,
};

struct ImageExtent final
{
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return width > 0U && height > 0U;
    }
};

[[nodiscard]] constexpr std::optional<std::size_t> TexelCount(ImageExtent extent) noexcept
{
    if (!extent.IsValid())
    {
        return std::nullopt;
    }

    constexpr std::size_t kMaximumSize = (std::numeric_limits<std::size_t>::max)();
    std::size_t const width = static_cast<std::size_t>(extent.width);
    std::size_t const height = static_cast<std::size_t>(extent.height);
    if (height != 0U && width > (kMaximumSize / height))
    {
        return std::nullopt;
    }
    return width * height;
}

template <typename Texel> class Image2D final
{
  public:
    Image2D() = default;

    Image2D(ImageExtent extent, std::vector<Texel> texels) : extent_(extent), texels_(std::move(texels)) {}

    [[nodiscard]] ImageExtent Extent() const noexcept
    {
        return extent_;
    }

    [[nodiscard]] std::uint32_t Width() const noexcept
    {
        return extent_.width;
    }

    [[nodiscard]] std::uint32_t Height() const noexcept
    {
        return extent_.height;
    }

    [[nodiscard]] std::vector<Texel> const &Texels() const noexcept
    {
        return texels_;
    }

    [[nodiscard]] bool IsValid() const noexcept
    {
        std::optional<std::size_t> const count = TexelCount(extent_);
        return count.has_value() && texels_.size() == *count;
    }

    [[nodiscard]] std::expected<Texel const *, TextureSamplingError> TexelAt(std::uint32_t x,
                                                                             std::uint32_t y) const noexcept
    {
        if (!extent_.IsValid())
        {
            return std::unexpected(TextureSamplingError::EmptyImage);
        }
        std::optional<std::size_t> const count = TexelCount(extent_);
        if (!count.has_value() || texels_.size() != *count)
        {
            return std::unexpected(TextureSamplingError::TexelDataSizeMismatch);
        }
        if (x >= extent_.width || y >= extent_.height)
        {
            return std::unexpected(TextureSamplingError::InvalidCoordinate);
        }

        std::size_t const index =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(extent_.width)) + static_cast<std::size_t>(x);
        return &texels_[index];
    }

  private:
    ImageExtent extent_{};
    std::vector<Texel> texels_{};
};

struct SamplingConventions final
{
    XMFLOAT2 normalizedMinimum{0.0F, 0.0F};
    XMFLOAT2 normalizedMaximum{1.0F, 1.0F};
    XMFLOAT2 texelCenterOffset{0.5F, 0.5F};
    std::uint32_t baseMipLevel{0U};
    float minimumLod{0.0F};
    float magnificationFootprintThreshold{1.0F};
    float anisotropyEpsilon{1.0e-6F};
};

inline constexpr SamplingConventions kSamplingConventions{};

struct TextureFootprint final
{
    float xAxisLengthTexels{};
    float yAxisLengthTexels{};

    [[nodiscard]] constexpr float MajorLength() const noexcept
    {
        return xAxisLengthTexels > yAxisLengthTexels ? xAxisLengthTexels : yAxisLengthTexels;
    }

    [[nodiscard]] constexpr float MinorLength() const noexcept
    {
        return xAxisLengthTexels < yAxisLengthTexels ? xAxisLengthTexels : yAxisLengthTexels;
    }
};

struct TrilinearSelection final
{
    std::uint32_t lowerLevel{};
    std::uint32_t upperLevel{};
    float upperBlend{};
};

struct AnisotropyResult final
{
    float majorLengthTexels{};
    float minorLengthTexels{};
    float unclampedRatio{};
    float clampedRatio{};
};

} // namespace ch03::texture
