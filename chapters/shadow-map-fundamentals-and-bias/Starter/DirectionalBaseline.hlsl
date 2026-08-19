struct LightingConstants
{
    row_major float4x4 viewProjection;
    float3 cameraPosition;
    float exposure;
    float3 directionToLight;
    float intensity;
};

struct ObjectConstants
{
    row_major float4x4 world;
    float4 normalColumn0;
    float4 normalColumn1;
    float4 normalColumn2;
    float3 baseColor;
    float roughness;
    float metalness;
    float3 dielectricF0;
};

ConstantBuffer<LightingConstants> lighting : register(b0);
ConstantBuffer<ObjectConstants> objectData : register(b1);
Texture2D<float4> hdrInput : register(t0);
SamplerState linearClampSampler : register(s0);

struct MeshVertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct LitVertex
{
    float4 position : SV_Position;
    float3 worldPosition : POSITION;
    float3 worldNormal : NORMAL;
};

struct FullscreenVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

static const float Pi = 3.14159265358979323846;
static const float MinimumRoughness = 0.045;

LitVertex LightingVS(MeshVertex input)
{
    LitVertex output;
    float4 worldPosition = mul(float4(input.position, 1.0), objectData.world);
    output.position = mul(worldPosition, lighting.viewProjection);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = float3(dot(input.normal, objectData.normalColumn0.xyz),
                                dot(input.normal, objectData.normalColumn1.xyz),
                                dot(input.normal, objectData.normalColumn2.xyz));
    return output;
}

float3 SchlickFresnel(float3 f0, float cosine)
{
    float factor = pow(1.0 - saturate(cosine), 5.0);
    return f0 + ((1.0 - f0) * factor);
}

float GgxDistribution(float nDotH, float roughness)
{
    float effectiveRoughness = max(roughness, MinimumRoughness);
    float alpha = effectiveRoughness * effectiveRoughness;
    float alphaSquared = alpha * alpha;
    float term = (nDotH * nDotH * (alphaSquared - 1.0)) + 1.0;
    return alphaSquared / max(Pi * term * term, 1.0e-7);
}

float SmithG1(float nDotDirection, float roughness)
{
    float cosine = saturate(nDotDirection);
    if (cosine <= 0.0)
    {
        return 0.0;
    }
    float effectiveRoughness = max(roughness, MinimumRoughness);
    float alpha = effectiveRoughness * effectiveRoughness;
    float alphaSquared = alpha * alpha;
    float root = sqrt(alphaSquared + ((1.0 - alphaSquared) * cosine * cosine));
    return (2.0 * cosine) / max(cosine + root, 1.0e-7);
}

float4 LightingPS(LitVertex input) : SV_Target
{
    float3 normal = normalize(input.worldNormal);
    float3 viewDirection = normalize(lighting.cameraPosition - input.worldPosition);
    float3 lightDirection = normalize(lighting.directionToLight);
    float nDotL = saturate(dot(normal, lightDirection));
    float nDotV = saturate(dot(normal, viewDirection));
    float3 halfVector = normalize(viewDirection + lightDirection);
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));
    float3 f0 = lerp(objectData.dielectricF0, objectData.baseColor, objectData.metalness);
    float3 fresnel = SchlickFresnel(f0, vDotH);
    float distribution = GgxDistribution(nDotH, objectData.roughness);
    float geometry = SmithG1(nDotV, objectData.roughness) * SmithG1(nDotL, objectData.roughness);
    float denominator = max(4.0 * nDotV * nDotL, 1.0e-6);
    float3 specularBrdf = nDotL > 0.0 && nDotV > 0.0
        ? fresnel * distribution * geometry / denominator
        : 0.0;
    float3 diffuseWeight =
        (1.0 - SchlickFresnel(f0, nDotV)) * (1.0 - SchlickFresnel(f0, nDotL)) * (1.0 - objectData.metalness);
    float3 diffuseBrdf = diffuseWeight * objectData.baseColor / Pi;
    float3 irradiance = lighting.intensity * nDotL;
    return float4(irradiance * (diffuseBrdf + specularBrdf), 1.0);
}

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    FullscreenVertex output;
    output.position = float4((uv * float2(2.0, -2.0)) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

float3 AcesFit(float3 color)
{
    float3 numerator = color * ((2.51 * color) + 0.03);
    float3 denominator = (color * ((2.43 * color) + 0.59)) + 0.14;
    return saturate(numerator / denominator);
}

float SrgbEncode(float value)
{
    return value <= 0.0031308 ? value * 12.92 : (1.055 * pow(value, 1.0 / 2.4)) - 0.055;
}

float4 DisplayPS(FullscreenVertex input) : SV_Target
{
    float3 hdr = hdrInput.SampleLevel(linearClampSampler, input.uv, 0.0).rgb;
    float3 mapped = AcesFit(max(hdr * exp2(lighting.exposure), 0.0));
    return float4(saturate(float3(SrgbEncode(mapped.r), SrgbEncode(mapped.g), SrgbEncode(mapped.b))), 1.0);
}
