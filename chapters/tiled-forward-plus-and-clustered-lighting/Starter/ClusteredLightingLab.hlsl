struct FrameConstants
{
    row_major float4x4 projection;
    float4 projectionData;
    float4 slicing;
    uint4 dimensions;
    uint4 counts;
    uint4 options;
};

struct ObjectConstants
{
    float3 translation;
    float roughness;
    float3 scale;
    float metalness;
    float3 baseColor;
    float padding;
};

struct PointLightData
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
};

ConstantBuffer<FrameConstants> gFrame : register(b0);
ConstantBuffer<ObjectConstants> gObject : register(b1);
StructuredBuffer<PointLightData> gLights : register(t0);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 viewPosition : POSITION0;
    float3 viewNormal : NORMAL0;
};

static const float Pi = 3.14159265358979323846;

float SrgbEncode(float linearValue)
{
    float value = saturate(linearValue);
    return value <= 0.0031308 ? value * 12.92 : (1.055 * pow(value, 1.0 / 2.4)) - 0.055;
}

float3 DisplayEncode(float3 linearColor)
{
    return float3(SrgbEncode(linearColor.r), SrgbEncode(linearColor.g), SrgbEncode(linearColor.b));
}

float3 ShadePointLight(PointLightData light, float3 position, float3 normal, float3 baseColor, float roughness,
                       float metalness)
{
    float3 toLight = light.position - position;
    float distanceSquared = dot(toLight, toLight);
    float radiusSquared = light.radius * light.radius;
    if (distanceSquared >= radiusSquared || distanceSquared <= 1.0e-8)
    {
        return 0.0;
    }

    float inverseDistance = rsqrt(distanceSquared);
    float3 lightDirection = toLight * inverseDistance;
    float nDotL = saturate(dot(normal, lightDirection));
    if (nDotL <= 0.0)
    {
        return 0.0;
    }

    float3 viewDirection = normalize(-position);
    float3 halfVector = normalize(viewDirection + lightDirection);
    float specularPower = lerp(8.0, 96.0, 1.0 - roughness);
    float3 f0 = lerp(0.04.xxx, baseColor, metalness);
    float3 diffuse = baseColor * (1.0 - metalness) / Pi;
    float3 specular = f0 * pow(saturate(dot(normal, halfVector)), specularPower);
    float radial = saturate(1.0 - (distanceSquared / radiusSquared));
    float attenuation = radial * radial;
    return light.color * light.intensity * attenuation * nDotL * (diffuse + specular);
}

VertexOutput ForwardVS(VertexInput input)
{
    VertexOutput output;
    output.viewPosition = (input.position * gObject.scale) + gObject.translation;
    output.viewNormal = normalize(input.normal);
    output.position = mul(float4(output.viewPosition, 1.0), gFrame.projection);
    return output;
}

float4 ForwardPS(VertexOutput input) : SV_Target0
{
    float3 normal = normalize(input.viewNormal);
    float3 linearColor = gObject.baseColor * 0.018;
    [loop]
    for (uint lightIndex = 0u; lightIndex < gFrame.counts.x; ++lightIndex)
    {
        linearColor += ShadePointLight(gLights[lightIndex], input.viewPosition, normal, gObject.baseColor,
                                       gObject.roughness, gObject.metalness);
    }
    return float4(DisplayEncode(linearColor), 1.0);
}
