cbuffer Constants : register(b0)
{
    uint frameIndex, screenWidth, screenHeight, historyValid;
    float distance, extinction, albedo, anisotropy;
    float light, signature, historyShiftX, historyCompatible;
};
StructuredBuffer<float4> oldHistory : register(t0);
StructuredBuffer<float4> preview : register(t1);
RWStructuredBuffer<float4> froxels : register(u0);
RWStructuredBuffer<float4> integrated : register(u1);
RWStructuredBuffer<float4> newHistory : register(u2);
static const uint width = 32, height = 32, slices = 16;

uint Hash(uint n)
{
    n ^= n >> 16; n *= 0x7feb352d; n ^= n >> 15;
    n *= 0x846ca68b; n ^= n >> 16;
    return n;
}
float Density(float2 uv, float z)
{
    return 0.6 + ((all(uv > 0.2) && all(uv < 0.8) && z > 2.0 && z < 6.0) ? 0.8 : 0.0);
}
float Shadow(float2 uv, float z)
{
    // Fixed synthetic circular blocker at z=3, light centered at (0.5,0.5,0).
    if (z <= 3.0) return 1.0;
    float2 hit = 0.5 + (uv - 0.5) * (3.0 / z);
    return dot(hit - 0.5, hit - 0.5) < 0.11 * 0.11 ? 0.0 : 1.0;
}
[numthreads(8, 8, 1)]
void InjectCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height || id.z >= slices) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    float jitter = float(Hash(id.x + id.y * width + frameIndex * 0x9e3779b9) & 0xffff) / 65536.0;
    float z = (float(id.z) + jitter) * distance / slices;
    float density = Density(uv, z);
    float visibility = Shadow(uv, z);
    // Henyey-Greenstein at a fixed synthetic view/light cosine of 0.8.
    // Multiplication by 4*pi makes the isotropic phase equal to one.
    float denom = 1.0 + anisotropy * anisotropy - 1.6 * anisotropy;
    float phase = (1.0 - anisotropy * anisotropy) / (denom * sqrt(denom));
    froxels[(id.z * height + id.y) * width + id.x] =
        float4(density, light * phase * visibility, visibility, z);
}
[numthreads(8, 8, 1)]
void IntegrateCS(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    uint index = id.y * width + id.x;
    float transmittance = 1.0, radiance = 0.0;
    float stepLength = distance / slices;
    [loop] for (uint z = 0; z < slices; ++z)
    {
        float4 cell = froxels[(z * height + id.y) * width + id.x];
        float opacity = 1.0 - exp(-extinction * cell.x * stepLength);
        radiance += transmittance * opacity * albedo * cell.y;
        transmittance *= 1.0 - opacity;
    }
    float accepted = 0.0, rejected = 0.0;
    if (historyValid != 0)
    {
        // No camera/geometry reprojection exists in this fixed-ray lab:
        // even an in-bounds shifted sample belongs to a different froxel.
        if (historyShiftX == 0.0)
        {
            float4 previous = oldHistory[index];
            if (historyCompatible != 0.0 && all(isfinite(previous)) && previous.x >= 0.0 &&
                previous.y >= 0.0 && previous.y <= 1.0 &&
                abs(previous.z - distance) <= 0.05 && previous.w == signature)
            {
                radiance = lerp(radiance, previous.x, 0.75);
                transmittance = lerp(transmittance, previous.y, 0.75);
                accepted = 1.0;
            }
            else rejected = 1.0;
        }
        else rejected = 1.0;
    }
    integrated[index] = float4(radiance, transmittance, rejected, accepted);
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
    // Two froxels at top-left report red for rejected history, green for accepted.
    if (pixel.x < 2 && pixel.y < 2)
        return float4(fog.z, fog.w, 0, 1);
    float mapped = fog.x / (1.0 + fog.x);
    // Synthetic blue surface attenuated by Beer-Lambert; warm scattering adds shafts.
    return float4(saturate(0.06 * fog.y + 0.7 * mapped),
                  saturate(0.15 * fog.y + 0.50 * mapped),
                  saturate(0.35 * fog.y + 0.18 * mapped), 1);
}
