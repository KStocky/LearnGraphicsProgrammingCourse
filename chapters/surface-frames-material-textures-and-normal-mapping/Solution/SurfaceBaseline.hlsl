struct LightingConstants
{
    row_major float4x4 viewProjection;
    float3 cameraPosition;
    uint visualization;
    float3 lightPosition;
    float intensity;
    float3 directionToLight;
    uint lightType;
    float3 lightColor;
    uint materialFlags;
    float3 overrideBaseColor;
    float overrideRoughness;
    float overrideMetalness;
    float normalStrength;
    uint invertNormalGreen;
    uint overrideNormalSample;
    uint padding;
    float3 overrideNormal;
    uint padding2;
};

struct ObjectConstants
{
    float3 translation;
    float uvScale;
    float3 baseColor;
    float roughness;
    float metalness;
    float3 dielectricF0;
    uint objectId;
};

struct OutputConstants
{
    float exposure;
    uint applyHdrDisplayTransform;
    float2 padding;
};

ConstantBuffer<LightingConstants> lighting : register(b0);
ConstantBuffer<ObjectConstants> objectData : register(b1);
ConstantBuffer<OutputConstants> outputData : register(b2);
Texture2D<float4> baseColorTexture : register(t0);
Texture2D<float4> packedMaterialTexture : register(t1);
Texture2D<float4> normalTexture : register(t2);
Texture2D<float4> baseColorLinearTexture : register(t3);
Texture2D<float4> hdrInput : register(t4);
SamplerState materialSampler : register(s0);
SamplerState linearClampSampler : register(s1);

struct MeshVertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
};

struct LitVertex
{
    float4 position : SV_Position;
    float3 worldPosition : POSITION;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 worldTangent : TANGENT;
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
    float4 worldPosition = float4(input.position + objectData.translation, 1.0);
    output.position = mul(worldPosition, lighting.viewProjection);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = input.normal;
    output.uv = input.uv * objectData.uvScale;
    output.worldTangent = input.tangent;
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

float4 LightingPS(LitVertex input, bool frontFace : SV_IsFrontFace) : SV_Target
{
    float3 interpolatedNormal = normalize(input.worldNormal) * (frontFace ? 1.0 : -1.0);
    float3 tangent = normalize(input.worldTangent.xyz - interpolatedNormal *
                               dot(input.worldTangent.xyz, interpolatedNormal));
    float handedness = input.worldTangent.w < 0.0 ? -1.0 : 1.0;
    float3 bitangent = normalize(cross(interpolatedNormal, tangent)) * handedness;
    float3 geometricNormal = normalize(cross(ddx(input.worldPosition), ddy(input.worldPosition)));
    geometricNormal *= frontFace ? 1.0 : -1.0;

    float4 encodedBaseColor = (lighting.materialFlags & 8U) != 0U
        ? baseColorLinearTexture.Sample(materialSampler, input.uv)
        : baseColorTexture.Sample(materialSampler, input.uv);
    float2 packedMaterial = packedMaterialTexture.Sample(materialSampler, input.uv).rg;
    float3 baseColor = (lighting.materialFlags & 1U) != 0U ? encodedBaseColor.rgb : objectData.baseColor;
    float roughness = (lighting.materialFlags & 2U) != 0U ? packedMaterial.r : objectData.roughness;
    float metalness = (lighting.materialFlags & 2U) != 0U ? packedMaterial.g : objectData.metalness;
    if ((lighting.materialFlags & 16U) != 0U)
    {
        baseColor = lighting.overrideBaseColor;
        roughness = lighting.overrideRoughness;
        metalness = lighting.overrideMetalness;
    }

    float3 encodedNormal = lighting.overrideNormalSample != 0U
        ? lighting.overrideNormal
        : normalTexture.Sample(materialSampler, input.uv).xyz;
    float3 tangentNormal = encodedNormal * 2.0 - 1.0;
    tangentNormal.y *= lighting.invertNormalGreen != 0U ? -1.0 : 1.0;
    tangentNormal = normalize(lerp(float3(0.0, 0.0, 1.0), tangentNormal, lighting.normalStrength));
    float3 mappedNormal = (lighting.materialFlags & 4U) != 0U
        ? normalize((tangent * tangentNormal.x) + (bitangent * tangentNormal.y) +
                    (interpolatedNormal * tangentNormal.z))
        : interpolatedNormal;

    float3 viewDirection = normalize(lighting.cameraPosition - input.worldPosition);
    float3 lightDirection = normalize(lighting.directionToLight);
    float normalIlluminance = lighting.intensity;
    if (lighting.lightType != 0U)
    {
        float3 offset = lighting.lightPosition - input.worldPosition;
        float distanceSquared = max(dot(offset, offset), 1.0e-4);
        lightDirection = offset * rsqrt(distanceSquared);
        normalIlluminance = lighting.intensity / distanceSquared;
    }

    float nDotL = saturate(dot(mappedNormal, lightDirection));
    float nDotV = saturate(dot(mappedNormal, viewDirection));
    float3 halfVector = normalize(viewDirection + lightDirection);
    float nDotH = saturate(dot(mappedNormal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));
    float3 f0 = lerp(objectData.dielectricF0, baseColor, metalness);
    float3 fresnel = SchlickFresnel(f0, vDotH);
    float distribution = GgxDistribution(nDotH, roughness);
    float geometry = SmithG1(nDotV, roughness) * SmithG1(nDotL, roughness);
    float denominator = max(4.0 * nDotV * nDotL, 1.0e-6);
    float3 specularBrdf = nDotL > 0.0 && nDotV > 0.0
        ? fresnel * distribution * geometry / denominator
        : 0.0;
    float3 diffuseWeight =
        (1.0 - SchlickFresnel(f0, nDotV)) * (1.0 - SchlickFresnel(f0, nDotL)) * (1.0 - metalness);
    float3 diffuseBrdf = diffuseWeight * baseColor / Pi;
    float3 irradiance = lighting.lightColor * normalIlluminance * nDotL;
    float3 diffuse = irradiance * diffuseBrdf;
    float3 specular = irradiance * specularBrdf;
    float3 color = diffuse + specular;

    if (lighting.visualization == 1U) color = float3(frac(input.uv), 0.0);
    else if (lighting.visualization == 2U) color = geometricNormal * 0.5 + 0.5;
    else if (lighting.visualization == 3U) color = interpolatedNormal * 0.5 + 0.5;
    else if (lighting.visualization == 4U) color = mappedNormal * 0.5 + 0.5;
    else if (lighting.visualization == 5U) color = tangent * 0.5 + 0.5;
    else if (lighting.visualization == 6U) color = bitangent * 0.5 + 0.5;
    else if (lighting.visualization == 7U) color = handedness < 0.0 ? float3(1.0, 0.12, 0.05) : float3(0.05, 0.35, 1.0);
    else if (lighting.visualization == 8U) color = baseColor;
    else if (lighting.visualization == 9U) color = roughness.xxx;
    else if (lighting.visualization == 10U) color = metalness.xxx;
    else if (lighting.visualization == 11U) color = diffuse;
    else if (lighting.visualization == 12U) color = specular;
    else if (lighting.visualization == 13U)
    {
        if (objectData.objectId == 1U) color = float3(0.85, 0.12, 0.08);
        else if (objectData.objectId == 2U) color = float3(0.08, 0.72, 0.16);
        else if (objectData.objectId == 3U) color = float3(0.08, 0.22, 0.9);
        else if (objectData.objectId == 4U) color = float3(0.82, 0.68, 0.08);
        else color = float3(1.0, 0.0, 0.8);
    }
    else if (lighting.visualization == 14U)
    {
        bool finite = all(isfinite(baseColor)) && isfinite(roughness) && isfinite(metalness) &&
                      all(isfinite(tangentNormal)) && all(isfinite(mappedNormal)) && all(isfinite(diffuse)) &&
                      all(isfinite(specular)) && all(isfinite(color));
        color = finite ? float3(0.05, 0.8, 0.12) : float3(1.0, 0.0, 0.8);
    }
    return float4(color, 1.0);
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
    float3 mapped = outputData.applyHdrDisplayTransform != 0U
        ? AcesFit(max(hdr * exp2(outputData.exposure), 0.0))
        : saturate(hdr);
    return float4(saturate(float3(SrgbEncode(mapped.r), SrgbEncode(mapped.g), SrgbEncode(mapped.b))), 1.0);
}
