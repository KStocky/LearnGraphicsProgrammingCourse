#include "TextureSamplingMath.hpp"

namespace ch03::texture::starter
{

std::expected<float, TextureSamplingError> AddressNormalized(float, AddressMode) noexcept
{
    return std::unexpected(TextureSamplingError::NotImplemented);
}

std::expected<XMFLOAT2, TextureSamplingError> TexelCenterToUv(ImageExtent, std::uint32_t, std::uint32_t) noexcept
{
    return std::unexpected(TextureSamplingError::NotImplemented);
}

std::expected<XMFLOAT2, TextureSamplingError> UvToTexelCenterCoordinate(ImageExtent, XMFLOAT2, AddressMode,
                                                                        AddressMode) noexcept
{
    return std::unexpected(TextureSamplingError::NotImplemented);
}

std::expected<XMFLOAT4, TextureSamplingError> SamplePoint(Image2D<XMFLOAT4> const &, XMFLOAT2, AddressMode,
                                                          AddressMode) noexcept
{
    return std::unexpected(TextureSamplingError::NotImplemented);
}

std::expected<XMFLOAT4, TextureSamplingError> SampleBilinear(Image2D<XMFLOAT4> const &, XMFLOAT2, AddressMode,
                                                             AddressMode) noexcept
{
    return std::unexpected(TextureSamplingError::NotImplemented);
}

std::expected<std::uint32_t, TextureSamplingError> MaximumMipLevel(ImageExtent) noexcept
{
    return std::unexpected(TextureSamplingError::NotImplemented);
}

std::expected<ImageExtent, TextureSamplingError> MipExtent(ImageExtent, std::uint32_t) noexcept
{
    return std::unexpected(TextureSamplingError::NotImplemented);
}

std::expected<TextureFootprint, TextureSamplingError> ComputeFootprintLengths(ImageExtent, XMFLOAT2, XMFLOAT2) noexcept
{
    return std::unexpected(TextureSamplingError::NotImplemented);
}

std::expected<float, TextureSamplingError> ComputeIsotropicLod(TextureFootprint, std::uint32_t) noexcept
{
    return std::unexpected(TextureSamplingError::NotImplemented);
}

std::expected<TrilinearSelection, TextureSamplingError> SelectTrilinearLevels(float, std::uint32_t) noexcept
{
    return std::unexpected(TextureSamplingError::NotImplemented);
}

std::expected<AnisotropyResult, TextureSamplingError> ComputeAnisotropy(TextureFootprint, float, float) noexcept
{
    return std::unexpected(TextureSamplingError::NotImplemented);
}

} // namespace ch03::texture::starter
