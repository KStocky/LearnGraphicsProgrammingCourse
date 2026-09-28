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

float CompareDepth(float2 uv)
{
    // Outside the light's frustum is lit, not a clamped copy of the border.
    if (any(uv < 0.0) || any(uv > 1.0))
        return 1.0;
    uint2 texel = min(uint2(uv * float(resolution)), resolution - 1);
    return receiverDepth - bias <= shadowDepth[texel.y * resolution + texel.x] ? 1.0 : 0.0;
}

[numthreads(8, 8, 1)]
void ShadeCS(uint2 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= resolution || pixel.y >= resolution)
        return;
    float2 uv = (float2(pixel) + 0.5) / float(resolution) + receiverUvOffset;
    visibility[pixel.y * resolution + pixel.x] = CompareDepth(uv);
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
