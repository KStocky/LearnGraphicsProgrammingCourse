// Chapter 27 Starter: honest bounded environment-lighting baseline.
//
// The environment is deterministic analytic data stored in a generated
// lat-long texture. This baseline deliberately does not claim to integrate
// irradiance or prefilter a BRDF: diffuse uses one normal-direction lookup and
// specular uses one sharp reflection lookup. The Solution adds the complete
// paired lab while preserving these values for direct comparison.
#include "../Common/EnvironmentLightingShared.hlsli"

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

    float2 probeUnit = Hammersley(5u, 16u);
    GgxSample probe =
        SampleGgxReflection(float3(0.0f, 1.0f, 0.0f), normalize(float3(0.6f, 0.8f, 0.0f)), Roughness, probeUnit);
    bool valid = Finite3(source) && Finite3(baselineDiffuse) && Finite3(baselineSpecular) &&
                 Finite3(probe.halfVector) && Finite3(probe.lightDirection) && isfinite(probe.lightPdf);

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
    output.status = valid ? (1u | 2u | 64u) : 0u;
    Statistics[index] = output;
}
