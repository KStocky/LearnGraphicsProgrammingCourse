struct FullscreenVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D<float4> inputT0 : register(t0);
Texture2D<float4> inputT1 : register(t1);
RWTexture2D<float4> outputU0 : register(u0);

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    FullscreenVertex output;
    output.position = float4((uv * float2(2.0, -2.0)) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

uint2 BandIndex(uint2 pixel, uint2 dimensions)
{
    return min(uint2(7u, 7u), (pixel * 8u) / dimensions);
}

[numthreads(8, 8, 1)]
void ComputeGenerateCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    outputU0.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }
    uint2 band = BandIndex(dispatchThreadId.xy, uint2(width, height));
    float mask = (band.x == band.y || (band.x + band.y) == 7u) ? 1.0 : 0.0;
    outputU0[dispatchThreadId.xy] = float4(mask, 0.0, 0.0, 1.0);
}

[numthreads(8, 8, 1)]
void ComputeCollapseCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    outputU0.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }
    float mask = inputT0.Load(int3(dispatchThreadId.xy, 0)).r;
    outputU0[dispatchThreadId.xy] = float4(0.0, mask * 0.75, mask * 1.5, 1.0);
}

float3 AnalyticHdrColor(uint2 band)
{
    if ((band.x >= 3u && band.x <= 4u) && (band.y >= 3u && band.y <= 4u))
    {
        return float3(4.0, 4.0, 4.0);
    }
    if (band.x < 4u && band.y < 4u)
    {
        return float3(4.0, 0.25 + (0.125 * band.y), 0.125 + (0.125 * (band.x & 1u)));
    }
    if (band.x >= 4u && band.y < 4u)
    {
        return float3(0.25 + (0.25 * (band.x - 4u)), 3.5, 0.5 + (0.125 * band.y));
    }
    if (band.x < 4u && band.y >= 4u)
    {
        return float3(0.25, 0.5 + (0.125 * band.x), 3.75);
    }
    return float3(
        0.75 + (0.25 * (band.x - 4u)),
        0.5 + (0.25 * (band.y - 4u)),
        1.0 + (0.125 * ((band.x - 4u) + (band.y - 4u))));
}

float4 GraphicsGeometryPS(FullscreenVertex input) : SV_Target
{
    uint2 band = min(uint2(7u, 7u), uint2(input.uv * 8.0));
    return float4(AnalyticHdrColor(band), 1.0);
}

float4 GraphicsResolvePS(FullscreenVertex input) : SV_Target
{
    return inputT0.Load(int3(uint2(input.position.xy), 0));
}

float SrgbOetf(float linearValue)
{
    return linearValue <= 0.0031308
        ? linearValue * 12.92
        : (1.055 * pow(linearValue, 1.0 / 2.4)) - 0.055;
}

float3 ToneMap(float3 hdrColor)
{
    float3 exposed = hdrColor * 0.5;
    return exposed / (1.0 + exposed);
}

float4 CompositePS(FullscreenVertex input) : SV_Target
{
    int3 coordinate = int3(uint2(input.position.xy), 0);
    float3 hdr = inputT1.Load(coordinate).rgb + inputT0.Load(coordinate).rgb;
    float3 mapped = saturate(ToneMap(hdr));
    return float4(SrgbOetf(mapped.r), SrgbOetf(mapped.g), SrgbOetf(mapped.b), 1.0);
}
