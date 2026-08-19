struct FrameConstants
{
    row_major float4x4 projection;
    float4 projectionData;
    uint4 dimensions;
    uint4 counts;
    uint4 clusters;
    float4 slicing;
};

struct DrawData
{
    uint vertexOffset;
    uint vertexCount;
    uint indexOffset;
    uint indexCount;
    float4 baseTintAndRoughness;
    float4 materialParameters;
};

struct PointLightData
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
};

struct CellLightRange
{
    uint offset;
    uint count;
    uint attemptedCount;
    uint overflowCount;
};

struct PixelDiagnostics
{
    float3 viewPosition;
    uint status;
    float3 normal;
    uint drawIdentifier;
    float4 tangent;
    float2 textureCoordinates;
    float2 textureDdx;
    float2 textureDdy;
    uint primitiveIdentifier;
    uint clusterIndex;
};

ConstantBuffer<FrameConstants> gFrame : register(b0);
StructuredBuffer<PointLightData> gLights : register(t20);
StructuredBuffer<CellLightRange> gCells : register(t21);
StructuredBuffer<uint> gLightIndices : register(t22);

static const float Pi = 3.14159265358979323846;
static const uint TileWidth = 16u;
static const uint TileHeight = 16u;

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

bool ComputeClusterIndex(uint2 pixel, float viewDepth, out uint clusterIndex)
{
    if (viewDepth < gFrame.slicing.x || viewDepth > gFrame.slicing.y || gFrame.clusters.z == 0u)
    {
        clusterIndex = 0u;
        return false;
    }

    uint2 tile = min(pixel / uint2(TileWidth, TileHeight), gFrame.dimensions.zw - 1u);
    uint tileIndex = tile.y * gFrame.dimensions.z + tile.x;
    float normalized =
        log(viewDepth / gFrame.slicing.x) / log(gFrame.slicing.y / gFrame.slicing.x);
    uint sliceIndex = min(uint(max(normalized, 0.0) * float(gFrame.clusters.z)), gFrame.clusters.z - 1u);
    uint tileCount = gFrame.dimensions.z * gFrame.dimensions.w;
    clusterIndex = sliceIndex * tileCount + tileIndex;
    return clusterIndex < gFrame.clusters.x;
}

bool ShadeCluster(uint clusterIndex, float3 position, float3 normal, float3 baseColor, float roughness,
                  float metalness, out float3 linearColor)
{
    if (clusterIndex >= gFrame.clusters.x)
    {
        linearColor = 0.0;
        return false;
    }

    CellLightRange range = gCells[clusterIndex];
    if (range.offset > gFrame.clusters.y || range.count > gFrame.clusters.y - range.offset)
    {
        linearColor = 0.0;
        return false;
    }

    linearColor = baseColor * 0.018;
    [loop]
    for (uint listOffset = 0u; listOffset < range.count; ++listOffset)
    {
        uint lightIndex = gLightIndices[range.offset + listOffset];
        if (lightIndex >= gFrame.counts.w)
        {
            linearColor = 0.0;
            return false;
        }
        linearColor += ShadePointLight(gLights[lightIndex], position, normal, baseColor, roughness, metalness);
    }
    return true;
}

float4 DebugOutput(float3 linearColor, float2 uv, float2 textureDdx, float2 textureDdy, uint drawIdentifier,
                   uint primitiveIdentifier)
{
    if (gFrame.clusters.w == 1u)
    {
        return float4(frac(uv), 0.0, 1.0);
    }
    if (gFrame.clusters.w == 2u)
    {
        float magnitude = max(length(textureDdx), length(textureDdy));
        return float4(saturate(magnitude * 12.0).xxx, 1.0);
    }
    if (gFrame.clusters.w == 3u)
    {
        float3 identifiers =
            frac(float3(float(drawIdentifier) * 0.271, float(primitiveIdentifier + 1u) * 0.193,
                        float(drawIdentifier + primitiveIdentifier + 1u) * 0.117));
        return float4(identifiers, 1.0);
    }
    return float4(DisplayEncode(linearColor), 1.0);
}
