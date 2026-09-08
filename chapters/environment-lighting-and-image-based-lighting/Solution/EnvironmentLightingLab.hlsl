// Chapter 27 Solution: isotropic GGX environment lighting.
//
// Diffuse stores irradiance E = integral L(wi) max(N.wi,0) dwi and applies
// albedo/pi only in shading. Specular prefiltering uses isotropic GGX NDF
// sampling (not VNDF) and the distant-environment V=N approximation. Its N.L
// weighted estimator builds a roughness-dependent environment value; it is not
// described as the full microfacet BRDF integral. The split-sum term separately
// uses exact uncorrelated Smith G1(V)G1(L), Schlick Fresnel, and returns A/B for
// F0*A+B. View 8 evaluates the complete (N.V, roughness) 2D LUT into the
// structured output surface. Mip selection uses the exact solid angle of the
// sampled lat-long row; the pole view intentionally exposes that this footprint
// remains distorted.
#include "../Common/EnvironmentLightingShared.hlsli"

float3 IntegrateIrradiance(float3 normal)
{
    float3 irradiance = 0.0f;
    for (uint row = 0u; row < EnvironmentHeight; ++row)
    {
        float solidAngle = LatLongTexelSolidAngle(row);
        for (uint column = 0u; column < EnvironmentWidth; ++column)
        {
            float3 direction = LatLongUvToDirection(float2(((float)column + 0.5f) / (float)EnvironmentWidth,
                                                           ((float)row + 0.5f) / (float)EnvironmentHeight));
            float cosine = max(0.0f, dot(normal, direction));
            irradiance += EnvironmentTexture.Load(int3(column, row, 0)).rgb * (cosine * solidAngle);
        }
    }
    return irradiance;
}

void IntegratePrefilter(float3 normal, out float3 baseResult, out float3 mipResult, out float meanMip,
                        out float maximumMip, out uint accepted)
{
    float3 baseAccumulation = 0.0f;
    float3 mipAccumulation = 0.0f;
    float weight = 0.0f;
    float mipSum = 0.0f;
    maximumMip = 0.0f;
    accepted = 0u;
    for (uint sampleIndex = 0u; sampleIndex < PrefilterSampleCount; ++sampleIndex)
    {
        GgxSample sample =
            SampleGgxReflection(normal, normal, Roughness, Hammersley(sampleIndex, PrefilterSampleCount));
        if (sample.nDotL <= 0.0f || sample.lightPdf <= 0.0f)
        {
            continue;
        }
        float mip = SelectEnvironmentMip(sample.lightDirection, sample.lightPdf, PrefilterSampleCount, Roughness);
        baseAccumulation += SampleEnvironment(sample.lightDirection, 0.0f) * sample.nDotL;
        mipAccumulation += SampleEnvironment(sample.lightDirection, mip) * sample.nDotL;
        weight += sample.nDotL;
        mipSum += mip;
        maximumMip = max(maximumMip, mip);
        ++accepted;
    }
    baseResult = baseAccumulation / weight;
    mipResult = mipAccumulation / weight;
    meanMip = mipSum / (float)accepted;
}

float2 IntegrateSplitSum(float nDotView, float roughness)
{
    float3 normal = float3(0.0f, 1.0f, 0.0f);
    float3 viewDirection = float3(sqrt(max(0.0f, 1.0f - nDotView * nDotView)), nDotView, 0.0f);
    float gView = SmithG1(nDotView, roughness);
    float2 result = 0.0f;
    for (uint sampleIndex = 0u; sampleIndex < SplitSumSampleCount; ++sampleIndex)
    {
        GgxSample sample =
            SampleGgxReflection(normal, viewDirection, roughness, Hammersley(sampleIndex, SplitSumSampleCount));
        if (sample.nDotL <= 0.0f || sample.nDotH <= 0.0f || sample.vDotH <= 0.0f)
        {
            continue;
        }
        float visibility = (gView * SmithG1(sample.nDotL, roughness) * sample.vDotH) / (sample.nDotH * nDotView);
        float fresnelComplement = pow(1.0f - sample.vDotH, 5.0f);
        result.x += (1.0f - fresnelComplement) * visibility;
        result.y += fresnelComplement * visibility;
    }
    return result / (float)SplitSumSampleCount;
}

[numthreads(8, 8, 1)] void SampleCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Width || dispatchThreadId.y >= Height)
    {
        return;
    }
    uint index = dispatchThreadId.y * Width + dispatchThreadId.x;
    float3 normal = PixelDirection(dispatchThreadId.x, dispatchThreadId.y, Width, Height);
    float3 source = SampleEnvironment(normal, 0.0f);
    static const float3 Albedo = float3(0.62f, 0.28f, 0.12f);
    static const float3 F0 = float3(0.04f, 0.04f, 0.04f);
    float3 baselineDiffuse = source * Albedo;
    float3 baselineSpecular = source * F0;
    float3 irradiance = IntegrateIrradiance(normal);
    float3 shadedDiffuse = irradiance * Albedo / Pi;

    float3 prefilterBase;
    float3 prefilterMip;
    float meanMip;
    float maximumMip;
    uint accepted;
    IntegratePrefilter(normal, prefilterBase, prefilterMip, meanMip, maximumMip, accepted);
    float2 split = IntegrateSplitSum(NDotView, Roughness);
    float lutNDotView = ((float)dispatchThreadId.x + 0.5f) / (float)Width;
    float lutRoughness = ((float)dispatchThreadId.y + 0.5f) / (float)Height;
    float2 lut = IntegrateSplitSum(lutNDotView, lutRoughness);
    float3 shadedSpecular = prefilterMip * (F0 * split.x + split.y);

    float2 probeUnit = Hammersley(5u, 16u);
    GgxSample probe =
        SampleGgxReflection(float3(0.0f, 1.0f, 0.0f), normalize(float3(0.6f, 0.8f, 0.0f)), Roughness, probeUnit);
    float equatorMip = SelectEnvironmentMip(float3(1.0f, 0.0f, 0.0f), 0.01f, PrefilterSampleCount, Roughness);
    float poleMip = SelectEnvironmentMip(float3(0.0f, 1.0f, 0.0f), 0.01f, PrefilterSampleCount, Roughness);

    bool valid = accepted > 0u && Finite3(source) && Finite3(irradiance) && Finite3(shadedDiffuse) &&
                 Finite3(prefilterBase) && Finite3(prefilterMip) && all(isfinite(split)) && all(isfinite(lut)) &&
                 Finite3(shadedSpecular) && isfinite(meanMip) && isfinite(maximumMip) && Finite3(probe.halfVector) &&
                 Finite3(probe.lightDirection) && isfinite(probe.lightPdf) && poleMip >= equatorMip;

    PixelStatistics output = (PixelStatistics)0;
    output.sourceR = source.r;
    output.sourceG = source.g;
    output.sourceB = source.b;
    output.baselineDiffuseR = baselineDiffuse.r;
    output.baselineDiffuseG = baselineDiffuse.g;
    output.baselineDiffuseB = baselineDiffuse.b;
    output.baselineSpecularR = baselineSpecular.r;
    output.baselineSpecularG = baselineSpecular.g;
    output.baselineSpecularB = baselineSpecular.b;
    output.irradianceR = irradiance.r;
    output.irradianceG = irradiance.g;
    output.irradianceB = irradiance.b;
    output.shadedDiffuseR = shadedDiffuse.r;
    output.shadedDiffuseG = shadedDiffuse.g;
    output.shadedDiffuseB = shadedDiffuse.b;
    output.prefilterBaseR = prefilterBase.r;
    output.prefilterBaseG = prefilterBase.g;
    output.prefilterBaseB = prefilterBase.b;
    output.prefilterMipR = prefilterMip.r;
    output.prefilterMipG = prefilterMip.g;
    output.prefilterMipB = prefilterMip.b;
    output.splitA = split.x;
    output.splitB = split.y;
    output.lutA = lut.x;
    output.lutB = lut.y;
    output.lutRoughness = lutRoughness;
    output.lutNDotView = lutNDotView;
    output.shadedSpecularR = shadedSpecular.r;
    output.shadedSpecularG = shadedSpecular.g;
    output.shadedSpecularB = shadedSpecular.b;
    output.meanSelectedMip = meanMip;
    output.maximumSelectedMip = maximumMip;
    output.equatorMip = equatorMip;
    output.poleMip = poleMip;
    output.probeRadicalInverse = probeUnit.y;
    output.probeHalfX = probe.halfVector.x;
    output.probeHalfY = probe.halfVector.y;
    output.probeHalfZ = probe.halfVector.z;
    output.probeLightX = probe.lightDirection.x;
    output.probeLightY = probe.lightDirection.y;
    output.probeLightZ = probe.lightDirection.z;
    output.probeNDotH = probe.nDotH;
    output.probeVDotH = probe.vDotH;
    output.probeNDotL = probe.nDotL;
    output.probeNdf = probe.ndf;
    output.probeHalfPdf = probe.halfPdf;
    output.probeLightPdf = probe.lightPdf;
    output.acceptedPrefilterSamples = accepted;
    output.status = valid ? (1u | 2u | 4u | 8u | 16u | 32u | 64u) : 0u;
    Statistics[index] = output;
}
