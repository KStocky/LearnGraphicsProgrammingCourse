cbuffer Constants : register(b0)
{
    uint frameIndex, screenWidth, screenHeight, historyValid;
    float distance, extinction, albedo, anisotropy;
    float light, signature, historyShiftX, unused;
};
StructuredBuffer<float4> oldHistory : register(t0);
StructuredBuffer<float4> preview : register(t1);
RWStructuredBuffer<float4> froxels : register(u0);
RWStructuredBuffer<float4> integrated : register(u1);
RWStructuredBuffer<float4> newHistory : register(u2);
static const uint width = 32, height = 32, slices = 16;
[numthreads(8, 8, 1)]
void InjectCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height || id.z >= slices) return;
    // Homogeneous, unshadowed baseline. No stochastic sampling or history.
    froxels[(id.z * height + id.y) * width + id.x] =
        float4(1.0, light, 1.0, (float(id.z) + 0.5) * distance / slices);
}
[numthreads(8, 8, 1)]
void IntegrateCS(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    uint index = id.y * width + id.x;
    float stepLength = distance / slices;
    float opacity = 1.0 - exp(-extinction * stepLength);
    float transmittance = 1.0, radiance = 0.0;
    [loop] for (uint z = 0; z < slices; ++z)
    {
        float4 cell = froxels[(z * height + id.y) * width + id.x];
        radiance += transmittance * opacity * albedo * cell.y;
        transmittance *= 1.0 - opacity;
    }
    integrated[index] = float4(radiance, transmittance, 0.0, 0.0);
    newHistory[index] = float4(radiance, transmittance, distance, signature);
}
float4 FullscreenVS(uint id : SV_VertexID) : SV_POSITION
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PreviewPS(float4 position : SV_POSITION) : SV_Target
{
    uint2 pixel = min(uint2(position.xy * float2(width, height) /
                            max(float2(screenWidth, screenHeight), 1.0)), uint2(width - 1, height - 1));
    float4 fog = preview[pixel.y * width + pixel.x];
    float mapped = fog.x / (1.0 + fog.x);
    return float4(saturate(0.06 * fog.y + 0.7 * mapped),
                  saturate(0.15 * fog.y + 0.50 * mapped),
                  saturate(0.35 * fog.y + 0.18 * mapped), 1);
}
