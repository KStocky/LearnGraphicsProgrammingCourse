struct LightingConstants
{
    row_major float4x4 viewProjection;
    row_major float4x4 lightViewProjection;
    float3 cameraPosition;
    float exposure;
    float3 directionToLight;
    float intensity;
    float receiverDepthBias;
    float receiverNormalOffsetWorld;
    float configuredConstantBiasEstimateDepth;
    float configuredSlopeBias;
    float configuredBiasClamp;
    uint visualization;
};

struct ObjectConstants
{
    row_major float4x4 world;
    uint objectId;
};

ConstantBuffer<LightingConstants> lighting : register(b0);
ConstantBuffer<ObjectConstants> objectData : register(b1);
Texture2D<float4> hdrInput : register(t0);
Texture2D<float> shadowMap : register(t1);
SamplerState linearClampSampler : register(s0);
SamplerComparisonState shadowComparisonSampler : register(s1);
SamplerState shadowPointSampler : register(s2);

struct MeshVertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct LightVertex
{
    float4 position : SV_Position;
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

LightVertex ShadowVS(MeshVertex input)
{
    LightVertex output;
    float4 worldPosition = mul(float4(input.position, 1.0), objectData.world);
    output.position = mul(worldPosition, lighting.lightViewProjection);
    return output;
}

LitVertex LightingVS(MeshVertex input)
{
    LitVertex output;
    float4 worldPosition = mul(float4(input.position, 1.0), objectData.world);
    output.position = mul(worldPosition, lighting.viewProjection);
    output.worldPosition = worldPosition.xyz;
    float3 row0 = objectData.world[0].xyz;
    float3 row1 = objectData.world[1].xyz;
    float3 row2 = objectData.world[2].xyz;
    float3x3 inverseTranspose = float3x3(cross(row1, row2), cross(row2, row0), cross(row0, row1));
    output.worldNormal = mul(input.normal, inverseTranspose);
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

float RasterSlope(float3 normal, float3 lightDirection)
{
    float cosine = max(abs(dot(normal, lightDirection)), 1.0e-4);
    return min(sqrt(max(1.0 - (cosine * cosine), 0.0)) / cosine, 1.0e4);
}

float ConfiguredBiasEstimate(float normalSlopeEstimate)
{
    float estimate =
        lighting.configuredConstantBiasEstimateDepth + (lighting.configuredSlopeBias * normalSlopeEstimate);
    if (lighting.configuredBiasClamp > 0.0)
    {
        estimate = min(estimate, lighting.configuredBiasClamp);
    }
    else if (lighting.configuredBiasClamp < 0.0)
    {
        estimate = max(estimate, lighting.configuredBiasClamp);
    }
    return estimate;
}

float3 ObjectIdColor(uint objectId)
{
    static const float3 colors[] = {
        float3(0.72, 0.72, 0.72), float3(0.9, 0.2, 0.1), float3(1.0, 0.65, 0.1),
        float3(0.1, 0.35, 1.0), float3(0.1, 0.85, 0.25), float3(0.75, 0.15, 0.9),
        float3(0.85, 0.75, 0.1), float3(0.1, 0.8, 0.85)};
    return objectId >= 1U && objectId <= 8U ? colors[objectId - 1U] : float3(1.0, 0.0, 1.0);
}

bool IsInShadowFrustum(float3 shadowNdc)
{
    return shadowNdc.x >= -1.0 && shadowNdc.x <= 1.0 &&
           shadowNdc.y >= -1.0 && shadowNdc.y <= 1.0 &&
           shadowNdc.z >= 0.0 && shadowNdc.z <= 1.0;
}

void ObjectMaterial(out float3 baseColor, out float roughness, out float metalness)
{
    static const float3 colors[] = {
        float3(0.0, 0.0, 0.0), float3(0.32, 0.34, 0.38), float3(0.72, 0.12, 0.055),
        float3(0.95, 0.64, 0.18), float3(0.08, 0.32, 0.82), float3(0.16, 0.68, 0.24),
        float3(0.62, 0.18, 0.72), float3(0.72, 0.65, 0.18), float3(0.08, 0.62, 0.68)};
    static const float roughnesses[] = {1.0, 0.72, 0.38, 0.22, 0.68, 0.5, 0.58, 0.84, 0.42};
    uint id = min(objectData.objectId, 8U);
    baseColor = colors[id];
    roughness = roughnesses[id];
    metalness = id == 3U ? 1.0 : 0.0;
}

float4 LightingPS(LitVertex input) : SV_Target
{
    uint view = lighting.visualization & 0x7fffffffU;
    bool shadowsEnabled = (lighting.visualization & 0x80000000U) == 0U;
    float3 normal = normalize(input.worldNormal);
    float3 baseColor;
    float roughness;
    float metalness;
    ObjectMaterial(baseColor, roughness, metalness);
    float3 lightDirection = normalize(lighting.directionToLight);
    float3 offsetWorldPosition = input.worldPosition + (normal * lighting.receiverNormalOffsetWorld);
    float4 shadowClip = mul(float4(offsetWorldPosition, 1.0), lighting.lightViewProjection);
    bool finiteProjection = all(isfinite(shadowClip)) && abs(shadowClip.w) > 1.0e-7;
    float3 shadowNdc = finiteProjection ? shadowClip.xyz / shadowClip.w : 0.0;
    float2 shadowUv = float2((shadowNdc.x * 0.5) + 0.5, 0.5 - (shadowNdc.y * 0.5));
    float receiverDepth = shadowNdc.z;
    bool inFrustum = finiteProjection && IsInShadowFrustum(shadowNdc);
    float comparisonDepth = receiverDepth - lighting.receiverDepthBias;
    float storedDepth = inFrustum ? shadowMap.SampleLevel(shadowPointSampler, shadowUv, 0.0) : 1.0;
    float visibility = (!shadowsEnabled || !inFrustum)
        ? 1.0
        : shadowMap.SampleCmpLevelZero(shadowComparisonSampler, shadowUv, comparisonDepth);
    float normalSlopeEstimate = RasterSlope(normal, lightDirection);
    float configuredBiasEstimate = ConfiguredBiasEstimate(normalSlopeEstimate);
    float diagnosticVisibility =
        (!shadowsEnabled || !inFrustum || comparisonDepth <= storedDepth) ? 1.0 : 0.0;
    bool finiteValues = finiteProjection && all(isfinite(normal)) && all(isfinite(shadowUv)) &&
                        isfinite(receiverDepth) && isfinite(storedDepth) && isfinite(normalSlopeEstimate) &&
                        isfinite(configuredBiasEstimate);

    if (view == 1U)
    {
        return float4(visibility.xxx, 1.0);
    }
    if (view == 2U)
    {
        return float4(storedDepth.xxx, 1.0);
    }
    if (view == 3U)
    {
        return float4(saturate(receiverDepth).xxx, 1.0);
    }
    if (view == 4U)
    {
        float encodedDifference = saturate(0.5 + ((storedDepth - comparisonDepth) * 80.0));
        return float4(encodedDifference, 1.0 - abs((encodedDifference * 2.0) - 1.0), 1.0 - encodedDifference, 1.0);
    }
    if (view == 5U)
    {
        float encodedBias = saturate(0.5 + (configuredBiasEstimate * 40.0));
        return float4(encodedBias, 1.0 - abs((encodedBias * 2.0) - 1.0), 1.0 - encodedBias, 1.0);
    }
    if (view == 6U)
    {
        return float4(saturate(shadowUv), 0.0, 1.0);
    }
    if (view == 7U)
    {
        return float4(inFrustum ? float3(0.1, 0.9, 0.2) : float3(0.9, 0.1, 0.1), 1.0);
    }
    if (view == 8U)
    {
        return float4(saturate(normalSlopeEstimate / 8.0).xxx, 1.0);
    }
    if (view == 9U)
    {
        return float4(finiteValues ? float3(0.1, 0.9, 0.2) : float3(1.0, 0.0, 1.0), 1.0);
    }
    if (view == 10U)
    {
        return float4(ObjectIdColor(objectData.objectId), 1.0);
    }
    if (view == 11U)
    {
        bool agrees = visibility == diagnosticVisibility;
        return float4(agrees ? float3(0.1, 0.9, 0.2) : float3(0.9, 0.1, 0.1), 1.0);
    }
    if (view == 12U)
    {
        if (!inFrustum)
        {
            return float4(0.0, 0.0, 0.0, 1.0);
        }
        if (comparisonDepth == storedDepth)
        {
            return float4(0.1, 0.9, 0.2, 1.0);
        }
        return float4(comparisonDepth < storedDepth ? float3(0.1, 0.25, 0.95) : float3(0.95, 0.15, 0.1), 1.0);
    }
    if (view == 13U)
    {
        float2 worldTexelSize =
            float2(lighting.configuredConstantBiasEstimateDepth, lighting.configuredSlopeBias);
        return float4(saturate(worldTexelSize * 16.0), 0.0, 1.0);
    }

    float3 viewDirection = normalize(lighting.cameraPosition - input.worldPosition);
    float nDotL = saturate(dot(normal, lightDirection));
    float nDotV = saturate(dot(normal, viewDirection));
    float3 halfVector = normalize(viewDirection + lightDirection);
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metalness);
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
    float3 directLight = lighting.intensity * nDotL * (diffuseBrdf + specularBrdf);
    return float4(visibility * directLight, 1.0);
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
    uint view = lighting.visualization & 0x7fffffffU;
    if (view == 14U)
    {
        static const float3 probePositions[] = {
            float3(-1.0, 0.0, 0.5), float3(1.0, 0.0, 0.5), float3(0.0, -1.0, 0.5),
            float3(0.0, 1.0, 0.5), float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 1.0),
            float3(-1.0001, 0.0, 0.5), float3(1.0001, 0.0, 0.5), float3(0.0, -1.0001, 0.5),
            float3(0.0, 1.0001, 0.5), float3(0.0, 0.0, -0.0001), float3(0.0, 0.0, 1.0001)};
        uint probeIndex = min((uint)(input.uv.x * 12.0), 11U);
        bool inFrustum = IsInShadowFrustum(probePositions[probeIndex]);
        float3 probeColor = inFrustum ? float3(0.1, 0.9, 0.2) : float3(0.9, 0.1, 0.1);
        return float4(SrgbEncode(probeColor.r), SrgbEncode(probeColor.g), SrgbEncode(probeColor.b), 1.0);
    }
    float3 hdr = hdrInput.SampleLevel(linearClampSampler, input.uv, 0.0).rgb;
    float3 displayLinear = view == 0U
        ? AcesFit(max(hdr * exp2(lighting.exposure), 0.0))
        : saturate(hdr);
    return float4(saturate(float3(SrgbEncode(displayLinear.r), SrgbEncode(displayLinear.g),
                                  SrgbEncode(displayLinear.b))), 1.0);
}
