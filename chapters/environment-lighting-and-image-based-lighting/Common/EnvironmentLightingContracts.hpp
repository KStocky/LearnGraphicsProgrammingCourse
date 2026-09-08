#pragma once

#include <cstdint>
#include <expected>
#include <span>

namespace ch27::environment_lighting
{

inline constexpr double kPi = 3.141592653589793238462643383279502884;
inline constexpr std::uint32_t kMaximumEnvironmentDimension = 16'384U;
inline constexpr std::uint32_t kMaximumReferenceSampleCount = 1U << 20U;
inline constexpr double kMinimumGgxAlpha = 1.0e-4;

enum class ContractError : std::uint8_t
{
    NonFinite,
    NegativeRadiance,
    InvalidDimensions,
    SizeMismatch,
    CountOverflow,
    InvalidDirection,
    InvalidUnitSample,
    InvalidSampleIndex,
    InvalidRoughness,
    InvalidDensity,
    InvalidSampleCount,
    InvalidViewCosine,
    InvalidMipCount,
    NoAcceptedSamples,
};

struct Float2 final
{
    double x{};
    double y{};

    [[nodiscard]] bool operator==(Float2 const &) const noexcept = default;
};

struct Float3 final
{
    double x{};
    double y{};
    double z{};

    [[nodiscard]] bool operator==(Float3 const &) const noexcept = default;
};

struct Rgb final
{
    double r{};
    double g{};
    double b{};

    [[nodiscard]] bool operator==(Rgb const &) const noexcept = default;
};

// Non-owning scene-linear RGB image. Pixels are row-major from theta=0 (+Y) to theta=pi (-Y).
struct EnvironmentImageView final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::span<Rgb const> pixels{};
};

struct GgxDirectionSample final
{
    Float2 unitSample{};
    Float3 halfVector{};
    Float3 lightDirection{};
    double alpha{};
    double nDotH{};
    double vDotH{};
    double nDotL{};
    double ndf{};
    double halfVectorPdf{};
    double lightDirectionPdf{};

    [[nodiscard]] bool operator==(GgxDirectionSample const &) const noexcept = default;
};

struct SpecularPrefilterDiagnostics final
{
    Rgb result{};
    std::uint32_t requestedSampleCount{};
    std::uint32_t acceptedSampleCount{};
    std::uint32_t rejectedBackfacingSampleCount{};
    double accumulatedWeight{};
    double minimumLightPdf{};
    double maximumLightPdf{};

    [[nodiscard]] bool operator==(SpecularPrefilterDiagnostics const &) const noexcept = default;
};

struct SplitSumBrdfDiagnostics final
{
    double scaleA{};
    double biasB{};
    std::uint32_t requestedSampleCount{};
    std::uint32_t acceptedSampleCount{};
    std::uint32_t rejectedBackfacingSampleCount{};

    [[nodiscard]] bool operator==(SplitSumBrdfDiagnostics const &) const noexcept = default;
};

[[nodiscard]] std::expected<void, ContractError> ValidateEnvironment(EnvironmentImageView image) noexcept;

// Right-handed, Y-up. theta is measured from +Y; phi turns around +Y from +X toward +Z.
// u wraps horizontally and v clamps vertically.
[[nodiscard]] std::expected<Float2, ContractError> DirectionToLatLongUv(Float3 direction) noexcept;
[[nodiscard]] std::expected<Float3, ContractError> LatLongUvToDirection(Float2 uv) noexcept;
[[nodiscard]] std::expected<double, ContractError> LatLongTexelSolidAngle(std::uint32_t width, std::uint32_t height,
                                                                          std::uint32_t row) noexcept;
[[nodiscard]] std::expected<Rgb, ContractError> SampleNearest(EnvironmentImageView image, Float2 uv) noexcept;
[[nodiscard]] std::expected<Rgb, ContractError> SampleBilinear(EnvironmentImageView image, Float2 uv) noexcept;

// Returns E=int L(wi)*max(N.wi,0) dwi. A diffuse BRDF applies its albedo/pi factor separately.
[[nodiscard]] std::expected<Rgb, ContractError> IntegrateDiffuseIrradiance(EnvironmentImageView image,
                                                                           Float3 surfaceNormal) noexcept;

// The teaching prefilter uses isotropic GGX NDF (not VNDF) sampling and the distant-environment approximation V=N.
// Perceptual roughness maps to alpha=max(roughness^2,kMinimumGgxAlpha), so values below 0.01 share the same alpha.
[[nodiscard]] std::expected<double, ContractError> PerceptualRoughnessToAlpha(double roughness) noexcept;
[[nodiscard]] std::expected<double, ContractError> EvaluateGgxNdf(double nDotH, double roughness) noexcept;
[[nodiscard]] std::expected<double, ContractError> EvaluateSmithG1(double nDotDirection, double roughness) noexcept;
[[nodiscard]] std::expected<Float2, ContractError> HammersleySample(std::uint32_t sampleIndex,
                                                                    std::uint32_t sampleCount) noexcept;
[[nodiscard]] std::expected<GgxDirectionSample, ContractError> SampleGgxReflection(Float3 normal, Float3 viewDirection,
                                                                                   double roughness,
                                                                                   Float2 unitSample) noexcept;
[[nodiscard]] std::expected<double, ContractError> EvaluateGgxReflectionPdf(Float3 normal, Float3 viewDirection,
                                                                            Float3 lightDirection,
                                                                            double roughness) noexcept;
[[nodiscard]] std::expected<SpecularPrefilterDiagnostics, ContractError> PrefilterSpecular(
    EnvironmentImageView image, Float3 reflectionDirection, double roughness, std::uint32_t sampleCount) noexcept;

// Split-sum assumes an isotropic single-scatter GGX microfacet BRDF, uncorrelated exact Smith masking-shadowing,
// Schlick Fresnel, and a white environment. The returned A/B satisfy F0*A+B; high roughness can lose energy because
// multiple scattering is not compensated. The same roughness-to-alpha mapping and numerical floor are used.
[[nodiscard]] std::expected<SplitSumBrdfDiagnostics, ContractError> IntegrateSplitSumBrdf(
    double nDotView, double roughness, std::uint32_t sampleCount) noexcept;

// Selects a lat-long mip from omega_sample=1/(N*p_L) relative to the exact base-level texel solid angle at the
// sampled row. This locks in the lat-long pole distortion: equal solid angle does not imply an isotropic footprint
// there. Roughness zero is the bounded delta-distribution fallback and returns mip zero.
[[nodiscard]] std::expected<double, ContractError> SelectEnvironmentMip(std::uint32_t width, std::uint32_t height,
                                                                        Float3 lightDirection, double lightPdf,
                                                                        std::uint32_t sampleCount, double roughness,
                                                                        std::uint32_t mipCount) noexcept;

} // namespace ch27::environment_lighting
