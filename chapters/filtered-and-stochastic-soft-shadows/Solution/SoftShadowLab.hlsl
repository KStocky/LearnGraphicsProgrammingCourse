cbuffer Parameters : register(b0)
{
    uint method;
    float receiverDepth, blockerDepth, lightRadius, bias;
    uint screenWidth, screenHeight;
    float2 receiverUvOffset;
};
StructuredBuffer<float> shadowDepth : register(t0);
StructuredBuffer<float> preview : register(t1);
RWStructuredBuffer<float> generatedDepth : register(u0);
RWStructuredBuffer<float> visibility : register(u1);

static const uint resolution = 64;

// Synthetic light-space depths: an occluder disk in front of an otherwise empty map.
// This is not a rasterized scene depth pass.
[numthreads(8, 8, 1)]
void GenerateDepthCS(uint2 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= resolution || pixel.y >= resolution)
        return;
    float2 uv = (float2(pixel) + 0.5) / float(resolution);
    generatedDepth[pixel.y * resolution + pixel.x] =
        length(uv - 0.5) < 0.22 ? blockerDepth : 1.0;
}

bool InFrustum(float2 uv)
{
    return all(uv >= 0.0) && all(uv <= 1.0);
}
float DepthAt(float2 uv)
{
    // Comparison sampler border convention: taps outside the map are lit.
    if (!InFrustum(uv))
        return 1.0;
    uint2 texel = min(uint2(uv * float(resolution)), resolution - 1);
    return shadowDepth[texel.y * resolution + texel.x];
}
float CompareDepth(float2 uv)
{
    return receiverDepth - bias <= DepthAt(uv) ? 1.0 : 0.0;
}
float Pcf(float2 uv, float radius)
{
    // The receiver outside the light frustum is lit even if a nearby tap
    // happens to land inside the map.
    if (!InFrustum(uv))
        return 1.0;
    float sum = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x)
            sum += CompareDepth(uv + float2(x, y) * radius / (2.0 * resolution));
    return sum / 25.0;
}
float Pcss(float2 uv)
{
    if (!InFrustum(uv))
        return 1.0;
    if (lightRadius == 0.0)
        return CompareDepth(uv);
    float searchRadius = min(lightRadius * max(receiverDepth - 0.1, 0.0) /
                                 max(receiverDepth, 0.001), 0.18);
    float blockers = 0.0, total = 0.0;
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x)
        {
            float depth = DepthAt(uv + float2(x, y) * searchRadius * 0.5);
            if (depth < receiverDepth - bias)
            {
                total += depth;
                blockers += 1.0;
            }
        }
    if (blockers == 0.0)
        return 1.0;
    float averageDepth = total / blockers;
    // Both depths are normalized *linear* light-space distances. A blocker
    // exactly at zero has an unbounded ideal depth ratio; clamp the finite
    // filter radius to keep this bounded GPU demonstration well-defined.
    float penumbra = lightRadius * max(receiverDepth - averageDepth, 0.0) /
                     max(averageDepth, 0.001);
    return Pcf(uv, min(penumbra * resolution, 12.0));
}
uint Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    return value ^ (value >> 16);
}
float AnalyticDiskVisibility(float2 uv, float2 lightPosition)
{
    // The central-light uv identifies a point on the blocker plane; moving
    // the emitter shifts the segment's blocker-plane intersection.
    float2 hit = blockerDepth == 0.0 ? lightPosition :
                 uv + (lightPosition - 0.5) * (1.0 - blockerDepth / receiverDepth);
    return length(hit - 0.5) < 0.22 ? 0.0 : 1.0;
}
float AreaVisibility(float2 uv, uint2 pixel)
{
    if (!InFrustum(uv))
        return 1.0;
    if (receiverDepth - bias <= blockerDepth)
        return 1.0;
    if (lightRadius == 0.0)
        return AnalyticDiskVisibility(uv, 0.5);
    // Stratify projected emitter-disk area radially; the 32 samples are
    // randomized within strata but are not independent identically distributed.
    uint seed = Hash(pixel.x + pixel.y * resolution);
    float sum = 0.0;
    [unroll] for (uint i = 0; i < 32; ++i)
    {
        float radial = float(Hash(seed ^ (i * 0x9e3779b9))) * (1.0 / 4294967296.0);
        float azimuth = float(Hash(seed ^ (i * 0x85ebca6b) ^ 0xb5297a4d)) * (6.2831853 / 4294967296.0);
        float radius = lightRadius * sqrt((float(i) + radial) / 32.0);
        float2 lightPosition = 0.5 + radius * float2(cos(azimuth), sin(azimuth));
        sum += AnalyticDiskVisibility(uv, lightPosition);
    }
    return sum / 32.0;
}

[numthreads(8, 8, 1)]
void ShadeCS(uint2 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= resolution || pixel.y >= resolution)
        return;
    float2 uv = (float2(pixel) + 0.5) / float(resolution) + receiverUvOffset;
    float result;
    if (method == 3)
        result = AreaVisibility(uv, pixel);
    else if (method == 1)
        result = Pcf(uv, 2.0);
    else if (method == 2)
        result = Pcss(uv);
    else
        result = CompareDepth(uv);
    visibility[pixel.y * resolution + pixel.x] = result;
}

float4 FullscreenVS(uint id : SV_VertexID) : SV_POSITION
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 VisibilityPS(float4 position : SV_POSITION) : SV_Target
{
    uint2 texel = min(uint2(position.xy * float2(resolution, resolution) /
                             max(float2(screenWidth, screenHeight), 1.0)), resolution - 1);
    float light = saturate(preview[texel.y * resolution + texel.x]);
    return float4(0.08 + 0.85 * light, 0.12 + 0.75 * light, 0.18 + 0.60 * light, 1);
}
