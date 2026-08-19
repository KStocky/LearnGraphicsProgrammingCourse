#pragma once

#include "../Common/ChapterTypes.hpp"

namespace ch03::texture::solution
{

[[nodiscard]] std::expected<float, TextureSamplingError> AddressNormalized(float coordinate, AddressMode mode) noexcept;
[[nodiscard]] std::expected<XMFLOAT2, TextureSamplingError> TexelCenterToUv(ImageExtent extent, std::uint32_t x,
                                                                            std::uint32_t y) noexcept;
[[nodiscard]] std::expected<XMFLOAT2, TextureSamplingError> UvToTexelCenterCoordinate(ImageExtent extent, XMFLOAT2 uv,
                                                                                      AddressMode uMode,
                                                                                      AddressMode vMode) noexcept;
[[nodiscard]] std::expected<XMFLOAT4, TextureSamplingError> SamplePoint(
    Image2D<XMFLOAT4> const &image, XMFLOAT2 uv, AddressMode uMode = AddressMode::Clamp,
    AddressMode vMode = AddressMode::Clamp) noexcept;
[[nodiscard]] std::expected<XMFLOAT4, TextureSamplingError> SampleBilinear(
    Image2D<XMFLOAT4> const &image, XMFLOAT2 uv, AddressMode uMode = AddressMode::Clamp,
    AddressMode vMode = AddressMode::Clamp) noexcept;
[[nodiscard]] std::expected<std::uint32_t, TextureSamplingError> MaximumMipLevel(ImageExtent extent) noexcept;
[[nodiscard]] std::expected<ImageExtent, TextureSamplingError> MipExtent(ImageExtent baseExtent,
                                                                         std::uint32_t mipLevel) noexcept;
[[nodiscard]] std::expected<TextureFootprint, TextureSamplingError> ComputeFootprintLengths(ImageExtent extent,
                                                                                            XMFLOAT2 uvDdx,
                                                                                            XMFLOAT2 uvDdy) noexcept;
[[nodiscard]] std::expected<float, TextureSamplingError> ComputeIsotropicLod(TextureFootprint footprint,
                                                                             std::uint32_t maximumMipLevel) noexcept;
[[nodiscard]] std::expected<TrilinearSelection, TextureSamplingError> SelectTrilinearLevels(
    float lod, std::uint32_t maximumMipLevel) noexcept;
[[nodiscard]] std::expected<AnisotropyResult, TextureSamplingError> ComputeAnisotropy(
    TextureFootprint footprint, float maximumAnisotropy,
    float epsilon = kSamplingConventions.anisotropyEpsilon) noexcept;

} // namespace ch03::texture::solution
