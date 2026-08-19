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

struct CellLightRange
{
    uint offset;
    uint count;
    uint attemptedCount;
    uint overflowCount;
};

ConstantBuffer<FrameConstants> gFrame : register(b0);
ConstantBuffer<ObjectConstants> gObject : register(b1);
StructuredBuffer<PointLightData> gLights : register(t0);
StructuredBuffer<CellLightRange> gCells : register(t1);
StructuredBuffer<uint> gLightIndices : register(t2);
Texture2D<float> gDepth : register(t5);
RWStructuredBuffer<uint> gCountsUav : register(u0);
RWStructuredBuffer<CellLightRange> gCellsUav : register(u1);
RWStructuredBuffer<uint> gLightIndicesUav : register(u2);
RWStructuredBuffer<uint> gStatisticsUav : register(u3);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct DepthVertexOutput
{
    float4 position : SV_Position;
};

struct ForwardVertexOutput
{
    float4 position : SV_Position;
    float3 viewPosition : POSITION0;
    float3 viewNormal : NORMAL0;
};

static const float Pi = 3.14159265358979323846;
static const uint ThreadCount = 64u;
static const uint BufferCanary = 0xcdcdcdcdu;
static const uint MaximumLightIndexCapacity = 262144u;

float SrgbEncode(float linearValue)
{
    float value = saturate(linearValue);
    return value <= 0.0031308 ? value * 12.92 : (1.055 * pow(value, 1.0 / 2.4)) - 0.055;
}

float3 DisplayEncode(float3 linearColor)
{
    return float3(SrgbEncode(linearColor.r), SrgbEncode(linearColor.g), SrgbEncode(linearColor.b));
}

float ViewDepthFromDeviceDepth(float deviceDepth)
{
    float denominator = deviceDepth - gFrame.projectionData.z;
    return abs(denominator) < 1.0e-8 ? 0.0 : gFrame.projectionData.w / denominator;
}

float BackgroundDepth()
{
    return gFrame.options.y == 0u ? 1.0 : 0.0;
}

void ReduceTileDepth(uint tileIndex, out float minimumDepth, out float maximumDepth, out uint foregroundCount)
{
    uint tileX = tileIndex % gFrame.dimensions.z;
    uint tileY = tileIndex / gFrame.dimensions.z;
    uint2 minimumPixel = uint2(tileX * 16u, tileY * 16u);
    uint2 maximumPixel = min(minimumPixel + uint2(16u, 16u), gFrame.dimensions.xy);
    minimumDepth = 3.402823466e+38;
    maximumDepth = 0.0;
    foregroundCount = 0u;
    for (uint y = minimumPixel.y; y < maximumPixel.y; ++y)
    {
        for (uint x = minimumPixel.x; x < maximumPixel.x; ++x)
        {
            float deviceDepth = gDepth.Load(int3(uint2(x, y), 0));
            if (deviceDepth == BackgroundDepth())
            {
                continue;
            }
            float viewDepth = ViewDepthFromDeviceDepth(deviceDepth);
            minimumDepth = min(minimumDepth, viewDepth);
            maximumDepth = max(maximumDepth, viewDepth);
            ++foregroundCount;
        }
    }
}

void SliceBounds(uint sliceIndex, out float minimumDepth, out float maximumDepth)
{
    float sliceCount = gFrame.slicing.z;
    float ratio = gFrame.slicing.y / gFrame.slicing.x;
    minimumDepth =
        sliceIndex == 0u ? gFrame.slicing.x : gFrame.slicing.x * pow(ratio, float(sliceIndex) / sliceCount);
    maximumDepth = sliceIndex + 1u == uint(sliceCount)
                       ? gFrame.slicing.y
                       : gFrame.slicing.x * pow(ratio, float(sliceIndex + 1u) / sliceCount);
}

void MakeTileAabb(uint tileIndex, float minimumDepth, float maximumDepth, out float3 minimumBounds,
                  out float3 maximumBounds)
{
    uint tileX = tileIndex % gFrame.dimensions.z;
    uint tileY = tileIndex / gFrame.dimensions.z;
    uint minimumPixelX = tileX * 16u;
    uint minimumPixelY = tileY * 16u;
    uint maximumPixelX = min(minimumPixelX + 16u, gFrame.dimensions.x);
    uint maximumPixelY = min(minimumPixelY + 16u, gFrame.dimensions.y);

    float minimumNdcX = (2.0 * float(minimumPixelX) / float(gFrame.dimensions.x)) - 1.0;
    float maximumNdcX = (2.0 * float(maximumPixelX) / float(gFrame.dimensions.x)) - 1.0;
    float maximumNdcY = 1.0 - (2.0 * float(minimumPixelY) / float(gFrame.dimensions.y));
    float minimumNdcY = 1.0 - (2.0 * float(maximumPixelY) / float(gFrame.dimensions.y));
    float minimumSlopeX = minimumNdcX * gFrame.projectionData.x;
    float maximumSlopeX = maximumNdcX * gFrame.projectionData.x;
    float minimumSlopeY = minimumNdcY * gFrame.projectionData.y;
    float maximumSlopeY = maximumNdcY * gFrame.projectionData.y;

    float x0 = minimumSlopeX * minimumDepth;
    float x1 = minimumSlopeX * maximumDepth;
    float x2 = maximumSlopeX * minimumDepth;
    float x3 = maximumSlopeX * maximumDepth;
    float y0 = minimumSlopeY * minimumDepth;
    float y1 = minimumSlopeY * maximumDepth;
    float y2 = maximumSlopeY * minimumDepth;
    float y3 = maximumSlopeY * maximumDepth;
    minimumBounds = float3(min(min(x0, x1), min(x2, x3)), min(min(y0, y1), min(y2, y3)), minimumDepth);
    maximumBounds = float3(max(max(x0, x1), max(x2, x3)), max(max(y0, y1), max(y2, y3)), maximumDepth);
}

float DistanceToInterval(float value, float minimumValue, float maximumValue)
{
    return value < minimumValue ? minimumValue - value
                                : (value > maximumValue ? value - maximumValue : 0.0);
}

bool SphereOverlapsAabb(PointLightData light, float3 minimumBounds, float3 maximumBounds)
{
    float3 distance = float3(DistanceToInterval(light.position.x, minimumBounds.x, maximumBounds.x),
                             DistanceToInterval(light.position.y, minimumBounds.y, maximumBounds.y),
                             DistanceToInterval(light.position.z, minimumBounds.z, maximumBounds.z));
    return dot(distance, distance) <= light.radius * light.radius;
}

bool LightOverlapsCell(uint cellIndex, PointLightData light, float tileMinimumDepth, float tileMaximumDepth,
                       uint foregroundCount)
{
    if (foregroundCount == 0u)
    {
        return false;
    }

    uint tileIndex = cellIndex % gFrame.counts.y;
    float minimumDepth = tileMinimumDepth;
    float maximumDepth = tileMaximumDepth;
    if (gFrame.options.x == 2u)
    {
        uint sliceIndex = cellIndex / gFrame.counts.y;
        SliceBounds(sliceIndex, minimumDepth, maximumDepth);
        if (tileMaximumDepth < minimumDepth || tileMinimumDepth > maximumDepth)
        {
            return false;
        }
    }

    float3 minimumBounds;
    float3 maximumBounds;
    MakeTileAabb(tileIndex, minimumDepth, maximumDepth, minimumBounds, maximumBounds);
    return SphereOverlapsAabb(light, minimumBounds, maximumBounds);
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

DepthVertexOutput DepthVS(VertexInput input)
{
    DepthVertexOutput output;
    float3 viewPosition = (input.position * gObject.scale) + gObject.translation;
    output.position = mul(float4(viewPosition, 1.0), gFrame.projection);
    return output;
}

ForwardVertexOutput ForwardVS(VertexInput input)
{
    ForwardVertexOutput output;
    output.viewPosition = (input.position * gObject.scale) + gObject.translation;
    output.viewNormal = normalize(input.normal);
    output.position = mul(float4(output.viewPosition, 1.0), gFrame.projection);
    return output;
}

uint DepthToSlice(float viewDepth)
{
    float normalized = log(viewDepth / gFrame.slicing.x) / log(gFrame.slicing.y / gFrame.slicing.x);
    return min(uint(max(normalized, 0.0) * gFrame.slicing.z), uint(gFrame.slicing.z) - 1u);
}

float4 ForwardPS(ForwardVertexOutput input) : SV_Target0
{
    uint2 pixel = uint2(input.position.xy);
    uint2 tile = min(pixel / uint2(16u, 16u), gFrame.dimensions.zw - 1u);
    uint tileIndex = tile.y * gFrame.dimensions.z + tile.x;
    uint cellIndex = tileIndex;
    if (gFrame.options.x == 2u)
    {
        cellIndex += DepthToSlice(input.viewPosition.z) * gFrame.counts.y;
    }

    CellLightRange range = gCells[cellIndex];
    if (gFrame.options.z == 1u)
    {
        float occupancy = gFrame.counts.x == 0u ? 0.0 : float(range.attemptedCount) / float(gFrame.counts.x);
        return float4(saturate(float3(occupancy, occupancy * occupancy, 1.0 - occupancy)), 1.0);
    }
    if (gFrame.options.z == 2u)
    {
        float overflow = range.attemptedCount == 0u ? 0.0 : float(range.overflowCount) / float(range.attemptedCount);
        return float4(saturate(float3(overflow, 0.12 * (1.0 - overflow), 0.0)), 1.0);
    }

    float3 normal = normalize(input.viewNormal);
    float3 linearColor = gObject.baseColor * 0.018;
    if (gFrame.options.x == 0u)
    {
        [loop]
        for (uint lightIndex = 0u; lightIndex < gFrame.counts.x; ++lightIndex)
        {
            linearColor += ShadePointLight(gLights[lightIndex], input.viewPosition, normal, gObject.baseColor,
                                           gObject.roughness, gObject.metalness);
        }
    }
    else
    {
        [loop]
        for (uint listOffset = 0u; listOffset < range.count; ++listOffset)
        {
            uint lightIndex = gLightIndices[range.offset + listOffset];
            linearColor += ShadePointLight(gLights[lightIndex], input.viewPosition, normal, gObject.baseColor,
                                           gObject.roughness, gObject.metalness);
        }
    }
    return float4(DisplayEncode(linearColor), 1.0);
}

[numthreads(ThreadCount, 1, 1)]
void ResetListsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint elementIndex = dispatchThreadId.x;
    if (elementIndex < gFrame.options.w)
    {
        gCountsUav[elementIndex] = BufferCanary;
        CellLightRange canaryRange;
        canaryRange.offset = BufferCanary;
        canaryRange.count = BufferCanary;
        canaryRange.attemptedCount = BufferCanary;
        canaryRange.overflowCount = BufferCanary;
        gCellsUav[elementIndex] = canaryRange;
    }
    if (elementIndex < MaximumLightIndexCapacity)
    {
        gLightIndicesUav[elementIndex] = BufferCanary;
    }
    if (elementIndex < 4u)
    {
        gStatisticsUav[elementIndex] = BufferCanary;
    }
}

[numthreads(ThreadCount, 1, 1)]
void CountLightsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint cellIndex = dispatchThreadId.x;
    if (cellIndex >= gFrame.counts.z)
    {
        return;
    }

    uint tileIndex = cellIndex % gFrame.counts.y;
    float tileMinimumDepth;
    float tileMaximumDepth;
    uint foregroundCount;
    ReduceTileDepth(tileIndex, tileMinimumDepth, tileMaximumDepth, foregroundCount);

    uint attemptedCount = 0u;
    [loop]
    for (uint lightIndex = 0u; lightIndex < gFrame.counts.x; ++lightIndex)
    {
        attemptedCount +=
            LightOverlapsCell(cellIndex, gLights[lightIndex], tileMinimumDepth, tileMaximumDepth, foregroundCount)
                ? 1u
                : 0u;
    }
    gCountsUav[cellIndex] = attemptedCount;
}

[numthreads(1, 1, 1)]
void PrefixCellsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x != 0u)
    {
        return;
    }

    uint cursor = 0u;
    uint totalAttempted = 0u;
    uint totalOverflow = 0u;
    [loop]
    for (uint cellIndex = 0u; cellIndex < gFrame.counts.z; ++cellIndex)
    {
        uint attemptedCount = gCountsUav[cellIndex];
        uint remaining = cursor < gFrame.counts.w ? gFrame.counts.w - cursor : 0u;
        uint emittedCount = min(attemptedCount, remaining);
        uint overflowCount = attemptedCount - emittedCount;
        CellLightRange range;
        range.offset = cursor;
        range.count = emittedCount;
        range.attemptedCount = attemptedCount;
        range.overflowCount = overflowCount;
        gCellsUav[cellIndex] = range;
        cursor += emittedCount;
        totalAttempted += attemptedCount;
        totalOverflow += overflowCount;
    }
    gStatisticsUav[0] = totalAttempted;
    gStatisticsUav[1] = cursor;
    gStatisticsUav[2] = totalOverflow;
    gStatisticsUav[3] = gFrame.counts.z;
}

[numthreads(ThreadCount, 1, 1)]
void FillLightsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint cellIndex = dispatchThreadId.x;
    if (cellIndex >= gFrame.counts.z)
    {
        return;
    }

    uint tileIndex = cellIndex % gFrame.counts.y;
    float tileMinimumDepth;
    float tileMaximumDepth;
    uint foregroundCount;
    ReduceTileDepth(tileIndex, tileMinimumDepth, tileMaximumDepth, foregroundCount);
    CellLightRange range = gCellsUav[cellIndex];
    uint localOffset = 0u;
    [loop]
    for (uint lightIndex = 0u; lightIndex < gFrame.counts.x; ++lightIndex)
    {
        if (!LightOverlapsCell(cellIndex, gLights[lightIndex], tileMinimumDepth, tileMaximumDepth, foregroundCount))
        {
            continue;
        }
        if (localOffset < range.count)
        {
            uint destination = range.offset + localOffset;
            if (destination < gFrame.counts.w)
            {
                gLightIndicesUav[destination] = lightIndex;
            }
        }
        ++localOffset;
    }
}
