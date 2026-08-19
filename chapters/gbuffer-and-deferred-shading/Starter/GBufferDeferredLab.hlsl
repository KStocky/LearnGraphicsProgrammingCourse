struct FrameConstants
{
    row_major float4x4 viewMatrix;
    row_major float4x4 viewProjection;
    float4 projectionData;
    float4 lightDirectionIntensity;
    uint4 options;
};

struct ObjectConstants
{
    float3 translation;
    float roughness;
    float3 baseColor;
    float metalness;
    uint4 metadata;
};

ConstantBuffer<FrameConstants> frameData : register(b0);
ConstantBuffer<ObjectConstants> objectData : register(b1);

struct MeshVertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
};

struct ForwardVertex
{
    float4 position : SV_Position;
    float3 viewPosition : POSITION0;
    float3 viewNormal : NORMAL0;
};

static const float Pi = 3.14159265358979323846;
static const float MinimumRoughness = 0.045;

float SignNotZero(float value)
{
    return value >= 0.0 ? 1.0 : -1.0;
}

float2 EncodeOctahedral(float3 unitNormal)
{
    float inverseL1 = rcp(abs(unitNormal.x) + abs(unitNormal.y) + abs(unitNormal.z));
    float2 projected = unitNormal.xy * inverseL1;
    if (unitNormal.z < 0.0)
    {
        projected = float2((1.0 - abs(projected.y)) * SignNotZero(projected.x),
                           (1.0 - abs(projected.x)) * SignNotZero(projected.y));
    }
    return (projected * 0.5) + 0.5;
}

float3 DecodeOctahedral(float2 encoded)
{
    float3 normal = float3((encoded * 2.0) - 1.0, 0.0);
    normal.z = 1.0 - abs(normal.x) - abs(normal.y);
    float fold = saturate(-normal.z);
    normal.x += normal.x >= 0.0 ? -fold : fold;
    normal.y += normal.y >= 0.0 ? -fold : fold;
    return normalize(normal);
}

float QuantizeUnorm(float value, float maximumCode)
{
    return round(saturate(value) * maximumCode) / maximumCode;
}

float SrgbEncode(float linearValue)
{
    float value = saturate(linearValue);
    return value <= 0.0031308 ? value * 12.92 : (1.055 * pow(value, 1.0 / 2.4)) - 0.055;
}

float SrgbDecode(float encoded)
{
    float value = saturate(encoded);
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

float3 QuantizedBaseColor(float3 linearBaseColor)
{
    float3 encoded = float3(SrgbEncode(linearBaseColor.r), SrgbEncode(linearBaseColor.g), SrgbEncode(linearBaseColor.b));
    encoded = float3(QuantizeUnorm(encoded.r, 255.0), QuantizeUnorm(encoded.g, 255.0), QuantizeUnorm(encoded.b, 255.0));
    return float3(SrgbDecode(encoded.r), SrgbDecode(encoded.g), SrgbDecode(encoded.b));
}

float3 QuantizedNormal(float3 unitNormal)
{
    float2 encoded = EncodeOctahedral(unitNormal);
    encoded = float2(QuantizeUnorm(encoded.x, 65535.0), QuantizeUnorm(encoded.y, 65535.0));
    return DecodeOctahedral(encoded);
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

float3 IdentityColor(uint identity)
{
    if (identity == 0U) return 0.0;
    if (identity == 1U) return float3(0.28, 0.30, 0.34);
    if (identity == 2U) return float3(0.92, 0.18, 0.06);
    if (identity == 3U) return float3(0.92, 0.88, 0.84);
    if (identity == 9U) return float3(0.84, 0.48, 0.14);
    return float3(0.18, 0.64, 0.90);
}

float3 DisplayEncode(float3 linearColor)
{
    return float3(SrgbEncode(linearColor.r), SrgbEncode(linearColor.g), SrgbEncode(linearColor.b));
}

ForwardVertex ForwardVS(MeshVertex input)
{
    ForwardVertex output;
    float4 worldPosition = float4(input.position + objectData.translation, 1.0);
    float4 viewPosition = mul(worldPosition, frameData.viewMatrix);
    output.position = mul(worldPosition, frameData.viewProjection);
    output.viewPosition = viewPosition.xyz;
    output.viewNormal = mul(float4(input.normal, 0.0), frameData.viewMatrix).xyz;
    return output;
}

float4 ForwardPS(ForwardVertex input, bool frontFace : SV_IsFrontFace) : SV_Target
{
    float3 viewNormal = normalize(input.viewNormal) * (frontFace ? 1.0 : -1.0);
    float3 quantizedNormal = QuantizedNormal(viewNormal);
    float3 baseColor = QuantizedBaseColor(objectData.baseColor);
    float roughness = QuantizeUnorm(objectData.roughness, 255.0);
    float metalness = QuantizeUnorm(objectData.metalness, 255.0);

    float3 viewDirection = normalize(-input.viewPosition);
    float3 lightDirection = normalize(frameData.lightDirectionIntensity.xyz);
    float nDotL = saturate(dot(quantizedNormal, lightDirection));
    float nDotV = saturate(dot(quantizedNormal, viewDirection));
    float3 halfVector = normalize(viewDirection + lightDirection);
    float nDotH = saturate(dot(quantizedNormal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metalness);
    float3 fresnel = SchlickFresnel(f0, vDotH);
    float distribution = GgxDistribution(nDotH, roughness);
    float geometry = SmithG1(nDotV, roughness) * SmithG1(nDotL, roughness);
    float denominator = max(4.0 * nDotV * nDotL, 1.0e-6);
    float3 specularBrdf = nDotL > 0.0 && nDotV > 0.0 ? fresnel * distribution * geometry / denominator : 0.0;
    float3 diffuseWeight =
        (1.0 - SchlickFresnel(f0, nDotV)) * (1.0 - SchlickFresnel(f0, nDotL)) * (1.0 - metalness);
    float3 diffuseBrdf = diffuseWeight * baseColor / Pi;
    float3 linearColor = frameData.lightDirectionIntensity.w * nDotL * (diffuseBrdf + specularBrdf);

    uint debugView = frameData.options.x;
    if (debugView == 1U)
    {
        linearColor = baseColor;
    }
    else if (debugView == 2U)
    {
        linearColor = metalness.xxx;
    }
    else if (debugView == 3U)
    {
        linearColor = quantizedNormal * 0.5 + 0.5;
        return float4(saturate(linearColor), 1.0);
    }
    else if (debugView == 4U)
    {
        linearColor = roughness.xxx;
    }
    else if (debugView == 5U)
    {
        linearColor = saturate(input.position.z).xxx;
    }
    else if (debugView == 6U)
    {
        linearColor = 0.5.xxx;
        return float4(linearColor, 1.0);
    }
    else if (debugView == 7U)
    {
        linearColor = IdentityColor(objectData.metadata.x);
    }

    return float4(saturate(DisplayEncode(linearColor)), 1.0);
}
