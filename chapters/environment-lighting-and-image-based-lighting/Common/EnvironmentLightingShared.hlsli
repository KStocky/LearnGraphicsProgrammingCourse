static const float Pi = 3.14159265358979323846f;
static const float MinimumGgxAlpha = 1.0e-4f;

struct PixelStatistics
{
    float sourceR;
    float sourceG;
    float sourceB;
    float baselineDiffuseR;
    float baselineDiffuseG;
    float baselineDiffuseB;
    float baselineSpecularR;
    float baselineSpecularG;
    float baselineSpecularB;
    float irradianceR;
    float irradianceG;
    float irradianceB;
    float shadedDiffuseR;
    float shadedDiffuseG;
    float shadedDiffuseB;
    float prefilterBaseR;
    float prefilterBaseG;
    float prefilterBaseB;
    float prefilterMipR;
    float prefilterMipG;
    float prefilterMipB;
    float splitA;
    float splitB;
    float lutA;
    float lutB;
    float lutRoughness;
    float lutNDotView;
    float shadedSpecularR;
    float shadedSpecularG;
    float shadedSpecularB;
    float meanSelectedMip;
    float maximumSelectedMip;
    float equatorMip;
    float poleMip;
    float probeRadicalInverse;
    float probeHalfX;
    float probeHalfY;
    float probeHalfZ;
    float probeLightX;
    float probeLightY;
    float probeLightZ;
    float probeNDotH;
    float probeVDotH;
    float probeNDotL;
    float probeNdf;
    float probeHalfPdf;
    float probeLightPdf;
    uint acceptedPrefilterSamples;
    uint status;
};

cbuffer DispatchConstants : register(b0)
{
    uint Width;
    uint Height;
    uint EnvironmentWidth;
    uint EnvironmentHeight;
    uint EnvironmentMipCount;
    uint PrefilterSampleCount;
    uint SplitSumSampleCount;
    float Roughness;
    float NDotView;
};

RWStructuredBuffer<PixelStatistics> Statistics : register(u0);
Texture2D<float4> EnvironmentTexture : register(t0);
SamplerState EnvironmentSampler : register(s0);

float3 PixelDirection(uint x, uint y, uint width, uint height)
{
    float theta = Pi * ((float)y + 0.5f) / (float)height;
    float phi = 2.0f * Pi * ((float)x + 0.5f) / (float)width;
    float sine = sin(theta);
    return float3(sine * cos(phi), cos(theta), sine * sin(phi));
}

float2 DirectionToLatLongUv(float3 direction)
{
    float phi = atan2(direction.z, direction.x);
    if (phi < 0.0f)
    {
        phi += 2.0f * Pi;
    }
    return float2(phi / (2.0f * Pi), acos(clamp(direction.y, -1.0f, 1.0f)) / Pi);
}

float3 LatLongUvToDirection(float2 uv)
{
    float theta = saturate(uv.y) * Pi;
    float phi = frac(uv.x) * 2.0f * Pi;
    float sine = sin(theta);
    return float3(sine * cos(phi), cos(theta), sine * sin(phi));
}

float RadicalInverseBase2(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);
    bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0f0u) >> 4u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xccccccccu) >> 2u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xaaaaaaaau) >> 1u);
    return (float)bits * (1.0f / 4294967296.0f);
}

float2 Hammersley(uint index, uint count)
{
    return float2(((float)index + 0.5f) / (float)count, RadicalInverseBase2(index));
}

float GgxAlpha(float roughness)
{
    return max(roughness * roughness, MinimumGgxAlpha);
}

float GgxNdf(float nDotH, float roughness)
{
    float alpha = GgxAlpha(roughness);
    float alphaSquared = alpha * alpha;
    float denominator = nDotH * nDotH * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared / (Pi * denominator * denominator);
}

float SmithG1(float nDotDirection, float roughness)
{
    if (nDotDirection <= 0.0f)
    {
        return 0.0f;
    }
    float alpha = GgxAlpha(roughness);
    float alphaSquared = alpha * alpha;
    float root = sqrt(alphaSquared + (1.0f - alphaSquared) * nDotDirection * nDotDirection);
    return (2.0f * nDotDirection) / (nDotDirection + root);
}

void BuildCpuContractFrame(float3 normal, out float3 tangent, out float3 bitangent)
{
    if (abs(normal.y) > 0.999f)
    {
        tangent = normalize(cross(normal, float3(0.0f, 0.0f, 1.0f)));
    }
    else
    {
        tangent = normalize(cross(float3(0.0f, 1.0f, 0.0f), normal));
    }
    bitangent = cross(tangent, normal);
}

struct GgxSample
{
    float3 halfVector;
    float3 lightDirection;
    float nDotH;
    float vDotH;
    float nDotL;
    float ndf;
    float halfPdf;
    float lightPdf;
};

GgxSample SampleGgxReflection(float3 normal, float3 viewDirection, float roughness, float2 unitSample)
{
    float alpha = GgxAlpha(roughness);
    float alphaSquared = alpha * alpha;
    float cosineTheta = sqrt((1.0f - unitSample.y) / (1.0f + (alphaSquared - 1.0f) * unitSample.y));
    float sineTheta = sqrt(max(0.0f, 1.0f - cosineTheta * cosineTheta));
    float phi = 2.0f * Pi * unitSample.x;
    float3 tangent;
    float3 bitangent;
    BuildCpuContractFrame(normal, tangent, bitangent);
    float3 localHalf = float3(sineTheta * cos(phi), cosineTheta, sineTheta * sin(phi));
    float3 halfVector = normalize(tangent * localHalf.x + normal * localHalf.y + bitangent * localHalf.z);
    float vDotH = dot(viewDirection, halfVector);
    float3 lightDirection = normalize(2.0f * vDotH * halfVector - viewDirection);
    float nDotH = max(0.0f, dot(normal, halfVector));
    float nDotL = dot(normal, lightDirection);
    float ndf = GgxNdf(nDotH, roughness);
    float halfPdf = ndf * nDotH;
    float lightPdf = vDotH == 0.0f ? 0.0f : halfPdf / (4.0f * abs(vDotH));
    GgxSample result;
    result.halfVector = halfVector;
    result.lightDirection = lightDirection;
    result.nDotH = nDotH;
    result.vDotH = vDotH;
    result.nDotL = nDotL;
    result.ndf = ndf;
    result.halfPdf = halfPdf;
    result.lightPdf = lightPdf;
    return result;
}

float LatLongTexelSolidAngle(uint row)
{
    float theta0 = Pi * (float)row / (float)EnvironmentHeight;
    float theta1 = Pi * (float)(row + 1u) / (float)EnvironmentHeight;
    return (2.0f * Pi / (float)EnvironmentWidth) * (cos(theta0) - cos(theta1));
}

float SelectEnvironmentMip(float3 lightDirection, float lightPdf, uint sampleCount, float roughness)
{
    if (roughness == 0.0f)
    {
        return 0.0f;
    }
    float2 uv = DirectionToLatLongUv(lightDirection);
    uint row = min((uint)(uv.y * (float)EnvironmentHeight), EnvironmentHeight - 1u);
    float texelSolidAngle = LatLongTexelSolidAngle(row);
    float mip = 0.5f * (-log2((float)sampleCount) - log2(lightPdf) - log2(texelSolidAngle));
    return clamp(mip, 0.0f, (float)(EnvironmentMipCount - 1u));
}

float3 SampleEnvironment(float3 direction, float mip)
{
    return EnvironmentTexture.SampleLevel(EnvironmentSampler, DirectionToLatLongUv(direction), mip).rgb;
}

bool Finite3(float3 value)
{
    return all(isfinite(value));
}

float3 ToneMap(float3 color)
{
    color = max(color, 0.0f);
    return pow(color / (1.0f + color), 1.0f / 2.2f);
}

struct FullscreenVertex
{
    float4 position : SV_Position;
};

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenVertex output;
    output.position = float4(vertexId == 2u ? 3.0f : -1.0f, vertexId == 1u ? 3.0f : -1.0f, 0.0f, 1.0f);
    return output;
}

cbuffer DisplayConstants : register(b0)
{
    uint DisplayWidth;
    uint DisplayHeight;
    uint DebugView;
    uint ExpectedStatus;
};

StructuredBuffer<PixelStatistics> DisplayStatistics : register(t0);

float3 StatisticsColor(PixelStatistics statistics, uint2 pixel)
{
    float3 source = float3(statistics.sourceR, statistics.sourceG, statistics.sourceB);
    float3 baselineDiffuse =
        float3(statistics.baselineDiffuseR, statistics.baselineDiffuseG, statistics.baselineDiffuseB);
    float3 baselineSpecular =
        float3(statistics.baselineSpecularR, statistics.baselineSpecularG, statistics.baselineSpecularB);
    float3 solutionDiffuse = float3(statistics.shadedDiffuseR, statistics.shadedDiffuseG, statistics.shadedDiffuseB);
    float3 solutionSpecular =
        float3(statistics.shadedSpecularR, statistics.shadedSpecularG, statistics.shadedSpecularB);
    if (DebugView == 0u)
    {
        return ToneMap(baselineDiffuse + baselineSpecular);
    }
    if (DebugView == 1u)
    {
        return ToneMap(source);
    }
    if (DebugView == 2u)
    {
        return ToneMap(pixel.x < DisplayWidth / 2u ? baselineDiffuse : baselineSpecular);
    }
    if (DebugView == 3u)
    {
        return ToneMap(solutionDiffuse + solutionSpecular);
    }
    if (DebugView == 4u)
    {
        return ToneMap(float3(statistics.irradianceR, statistics.irradianceG, statistics.irradianceB) / Pi);
    }
    if (DebugView == 5u)
    {
        return ToneMap(float3(statistics.prefilterMipR, statistics.prefilterMipG, statistics.prefilterMipB));
    }
    if (DebugView == 6u)
    {
        // The generated 64x32 environment has seven levels (0..6). Keeping
        // display-only constants independent avoids making DisplayPS retain
        // the compute cbuffer at b0.
        float normalizedMip = statistics.meanSelectedMip / 6.0f;
        return float3(normalizedMip, 1.0f - normalizedMip, statistics.maximumSelectedMip > 4.0f ? 1.0f : 0.0f);
    }
    if (DebugView == 7u)
    {
        return float3(statistics.lutA, statistics.lutB, saturate(statistics.lutA + statistics.lutB));
    }
    return ToneMap(pixel.x < DisplayWidth / 2u ? baselineDiffuse + baselineSpecular
                                               : solutionDiffuse + solutionSpecular);
}

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
    uint2 pixel = min(uint2(position.xy), uint2(DisplayWidth - 1u, DisplayHeight - 1u));
    PixelStatistics statistics = DisplayStatistics[pixel.y * DisplayWidth + pixel.x];
    if (statistics.status != ExpectedStatus)
    {
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    }
    return float4(StatisticsColor(statistics, pixel), 1.0f);
}
