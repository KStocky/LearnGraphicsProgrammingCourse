#include "TextureSamplingMath.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ch03::texture::solution
{
namespace
{

[[nodiscard]] bool IsFinite(float value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool IsFinite(XMFLOAT2 value) noexcept
{
    return IsFinite(value.x) && IsFinite(value.y);
}

[[nodiscard]] std::optional<TextureSamplingError> ValidateImage(Image2D<XMFLOAT4> const &image) noexcept
{
    if (!image.Extent().IsValid())
    {
        return TextureSamplingError::EmptyImage;
    }
    if (!image.IsValid())
    {
        return TextureSamplingError::TexelDataSizeMismatch;
    }
    return std::nullopt;
}

[[nodiscard]] std::int64_t PositiveModulo(std::int64_t value, std::int64_t modulus) noexcept
{
    std::int64_t result = value % modulus;
    if (result < 0)
    {
        result += modulus;
    }
    return result;
}

[[nodiscard]] std::uint32_t AddressTexelIndex(std::int64_t index, std::uint32_t extent, AddressMode mode) noexcept
{
    std::int64_t const size = static_cast<std::int64_t>(extent);
    switch (mode)
    {
    case AddressMode::Clamp:
        return static_cast<std::uint32_t>(std::clamp(index, std::int64_t{0}, size - 1));
    case AddressMode::Wrap:
        return static_cast<std::uint32_t>(PositiveModulo(index, size));
    case AddressMode::Mirror:
    {
        std::int64_t const period = size * 2;
        std::int64_t const repeated = PositiveModulo(index, period);
        std::int64_t const mirrored = repeated < size ? repeated : (period - 1) - repeated;
        return static_cast<std::uint32_t>(mirrored);
    }
    }
    return 0U;
}

[[nodiscard]] float Lerp(float first, float second, float amount) noexcept
{
    return (first * (1.0F - amount)) + (second * amount);
}

[[nodiscard]] XMFLOAT4 Lerp(XMFLOAT4 first, XMFLOAT4 second, float amount) noexcept
{
    return {
        Lerp(first.x, second.x, amount),
        Lerp(first.y, second.y, amount),
        Lerp(first.z, second.z, amount),
        Lerp(first.w, second.w, amount),
    };
}

[[nodiscard]] std::expected<XMFLOAT4, TextureSamplingError> ReadTexel(Image2D<XMFLOAT4> const &image, std::int64_t x,
                                                                      std::int64_t y, AddressMode uMode,
                                                                      AddressMode vMode) noexcept
{
    std::uint32_t const addressedX = AddressTexelIndex(x, image.Width(), uMode);
    std::uint32_t const addressedY = AddressTexelIndex(y, image.Height(), vMode);
    auto const texel = image.TexelAt(addressedX, addressedY);
    if (!texel.has_value())
    {
        return std::unexpected(texel.error());
    }
    return **texel;
}

[[nodiscard]] bool IsValidFootprint(TextureFootprint footprint) noexcept
{
    return IsFinite(footprint.xAxisLengthTexels) && IsFinite(footprint.yAxisLengthTexels) &&
           footprint.xAxisLengthTexels >= 0.0F && footprint.yAxisLengthTexels >= 0.0F;
}

} // namespace

std::expected<float, TextureSamplingError> AddressNormalized(float coordinate, AddressMode mode) noexcept
{
    if (!IsFinite(coordinate))
    {
        return std::unexpected(TextureSamplingError::InvalidCoordinate);
    }

    switch (mode)
    {
    case AddressMode::Clamp:
        return std::clamp(coordinate, kSamplingConventions.normalizedMinimum.x,
                          kSamplingConventions.normalizedMaximum.x);
    case AddressMode::Wrap:
    {
        float wrapped = coordinate - std::floor(coordinate);
        if (wrapped >= 1.0F)
        {
            wrapped = 0.0F;
        }
        return wrapped;
    }
    case AddressMode::Mirror:
    {
        float mirrored = coordinate - (std::floor(coordinate * 0.5F) * 2.0F);
        if (mirrored < 0.0F)
        {
            mirrored += 2.0F;
        }
        return mirrored <= 1.0F ? mirrored : 2.0F - mirrored;
    }
    }
    return std::unexpected(TextureSamplingError::InvalidCoordinate);
}

std::expected<XMFLOAT2, TextureSamplingError> TexelCenterToUv(ImageExtent extent, std::uint32_t x,
                                                              std::uint32_t y) noexcept
{
    if (!extent.IsValid())
    {
        return std::unexpected(TextureSamplingError::InvalidExtent);
    }
    if (x >= extent.width || y >= extent.height)
    {
        return std::unexpected(TextureSamplingError::InvalidCoordinate);
    }

    return XMFLOAT2{
        (static_cast<float>(x) + kSamplingConventions.texelCenterOffset.x) / static_cast<float>(extent.width),
        (static_cast<float>(y) + kSamplingConventions.texelCenterOffset.y) / static_cast<float>(extent.height),
    };
}

std::expected<XMFLOAT2, TextureSamplingError> UvToTexelCenterCoordinate(ImageExtent extent, XMFLOAT2 uv,
                                                                        AddressMode uMode, AddressMode vMode) noexcept
{
    if (!extent.IsValid())
    {
        return std::unexpected(TextureSamplingError::InvalidExtent);
    }
    if (!IsFinite(uv))
    {
        return std::unexpected(TextureSamplingError::InvalidCoordinate);
    }

    auto const addressedU = AddressNormalized(uv.x, uMode);
    auto const addressedV = AddressNormalized(uv.y, vMode);
    if (!addressedU.has_value())
    {
        return std::unexpected(addressedU.error());
    }
    if (!addressedV.has_value())
    {
        return std::unexpected(addressedV.error());
    }

    return XMFLOAT2{
        (*addressedU * static_cast<float>(extent.width)) - kSamplingConventions.texelCenterOffset.x,
        (*addressedV * static_cast<float>(extent.height)) - kSamplingConventions.texelCenterOffset.y,
    };
}

std::expected<XMFLOAT4, TextureSamplingError> SamplePoint(Image2D<XMFLOAT4> const &image, XMFLOAT2 uv,
                                                          AddressMode uMode, AddressMode vMode) noexcept
{
    if (std::optional<TextureSamplingError> const imageError = ValidateImage(image); imageError.has_value())
    {
        return std::unexpected(*imageError);
    }

    auto const center = UvToTexelCenterCoordinate(image.Extent(), uv, uMode, vMode);
    if (!center.has_value())
    {
        return std::unexpected(center.error());
    }

    std::int64_t const x = static_cast<std::int64_t>(std::floor(center->x + 0.5F));
    std::int64_t const y = static_cast<std::int64_t>(std::floor(center->y + 0.5F));
    return ReadTexel(image, x, y, uMode, vMode);
}

std::expected<XMFLOAT4, TextureSamplingError> SampleBilinear(Image2D<XMFLOAT4> const &image, XMFLOAT2 uv,
                                                             AddressMode uMode, AddressMode vMode) noexcept
{
    if (std::optional<TextureSamplingError> const imageError = ValidateImage(image); imageError.has_value())
    {
        return std::unexpected(*imageError);
    }

    auto const center = UvToTexelCenterCoordinate(image.Extent(), uv, uMode, vMode);
    if (!center.has_value())
    {
        return std::unexpected(center.error());
    }

    float const xFloor = std::floor(center->x);
    float const yFloor = std::floor(center->y);
    std::int64_t const x0 = static_cast<std::int64_t>(xFloor);
    std::int64_t const y0 = static_cast<std::int64_t>(yFloor);
    float const xBlend = center->x - xFloor;
    float const yBlend = center->y - yFloor;

    auto const c00 = ReadTexel(image, x0, y0, uMode, vMode);
    auto const c10 = ReadTexel(image, x0 + 1, y0, uMode, vMode);
    auto const c01 = ReadTexel(image, x0, y0 + 1, uMode, vMode);
    auto const c11 = ReadTexel(image, x0 + 1, y0 + 1, uMode, vMode);
    if (!c00.has_value())
    {
        return std::unexpected(c00.error());
    }
    if (!c10.has_value())
    {
        return std::unexpected(c10.error());
    }
    if (!c01.has_value())
    {
        return std::unexpected(c01.error());
    }
    if (!c11.has_value())
    {
        return std::unexpected(c11.error());
    }

    XMFLOAT4 const top = Lerp(*c00, *c10, xBlend);
    XMFLOAT4 const bottom = Lerp(*c01, *c11, xBlend);
    return Lerp(top, bottom, yBlend);
}

std::expected<std::uint32_t, TextureSamplingError> MaximumMipLevel(ImageExtent extent) noexcept
{
    if (!extent.IsValid())
    {
        return std::unexpected(TextureSamplingError::InvalidExtent);
    }

    std::uint32_t largest = (std::max)(extent.width, extent.height);
    std::uint32_t level = 0U;
    while (largest > 1U)
    {
        largest >>= 1U;
        ++level;
    }
    return level;
}

std::expected<ImageExtent, TextureSamplingError> MipExtent(ImageExtent baseExtent, std::uint32_t mipLevel) noexcept
{
    auto const maximumLevel = MaximumMipLevel(baseExtent);
    if (!maximumLevel.has_value())
    {
        return std::unexpected(maximumLevel.error());
    }
    if (mipLevel > *maximumLevel)
    {
        return std::unexpected(TextureSamplingError::InvalidMipLevelRange);
    }

    return ImageExtent{
        (std::max)(1U, baseExtent.width >> mipLevel),
        (std::max)(1U, baseExtent.height >> mipLevel),
    };
}

std::expected<TextureFootprint, TextureSamplingError> ComputeFootprintLengths(ImageExtent extent, XMFLOAT2 uvDdx,
                                                                              XMFLOAT2 uvDdy) noexcept
{
    if (!extent.IsValid())
    {
        return std::unexpected(TextureSamplingError::InvalidExtent);
    }
    if (!IsFinite(uvDdx) || !IsFinite(uvDdy))
    {
        return std::unexpected(TextureSamplingError::InvalidFootprint);
    }

    float const width = static_cast<float>(extent.width);
    float const height = static_cast<float>(extent.height);
    float const dxU = uvDdx.x * width;
    float const dxV = uvDdx.y * height;
    float const dyU = uvDdy.x * width;
    float const dyV = uvDdy.y * height;
    return TextureFootprint{
        std::sqrt((dxU * dxU) + (dxV * dxV)),
        std::sqrt((dyU * dyU) + (dyV * dyV)),
    };
}

std::expected<float, TextureSamplingError> ComputeIsotropicLod(TextureFootprint footprint,
                                                               std::uint32_t maximumMipLevel) noexcept
{
    if (!IsValidFootprint(footprint))
    {
        return std::unexpected(TextureSamplingError::InvalidFootprint);
    }

    float const maximumFootprint = footprint.MajorLength();
    if (maximumFootprint <= kSamplingConventions.magnificationFootprintThreshold)
    {
        return kSamplingConventions.minimumLod;
    }

    float const unclampedLod = std::log2(maximumFootprint);
    return std::clamp(unclampedLod, kSamplingConventions.minimumLod, static_cast<float>(maximumMipLevel));
}

std::expected<TrilinearSelection, TextureSamplingError> SelectTrilinearLevels(float lod,
                                                                              std::uint32_t maximumMipLevel) noexcept
{
    if (!IsFinite(lod))
    {
        return std::unexpected(TextureSamplingError::InvalidMipLevelRange);
    }

    float const clampedLod = std::clamp(lod, kSamplingConventions.minimumLod, static_cast<float>(maximumMipLevel));
    float const lowerFloor = std::floor(clampedLod);
    std::uint32_t const lowerLevel = static_cast<std::uint32_t>(lowerFloor);
    std::uint32_t const upperLevel = (std::min)(lowerLevel + 1U, maximumMipLevel);
    float const blend = upperLevel == lowerLevel ? 0.0F : clampedLod - lowerFloor;
    return TrilinearSelection{lowerLevel, upperLevel, blend};
}

std::expected<AnisotropyResult, TextureSamplingError> ComputeAnisotropy(TextureFootprint footprint,
                                                                        float maximumAnisotropy, float epsilon) noexcept
{
    if (!IsValidFootprint(footprint))
    {
        return std::unexpected(TextureSamplingError::InvalidFootprint);
    }
    if (!IsFinite(maximumAnisotropy) || !IsFinite(epsilon) || maximumAnisotropy < 1.0F || epsilon <= 0.0F)
    {
        return std::unexpected(TextureSamplingError::InvalidAnisotropy);
    }

    float const major = (std::max)(footprint.MajorLength(), epsilon);
    float const minor = (std::max)(footprint.MinorLength(), epsilon);
    float const ratio = (std::max)(1.0F, major / minor);
    return AnisotropyResult{major, minor, ratio, std::clamp(ratio, 1.0F, maximumAnisotropy)};
}

} // namespace ch03::texture::solution
