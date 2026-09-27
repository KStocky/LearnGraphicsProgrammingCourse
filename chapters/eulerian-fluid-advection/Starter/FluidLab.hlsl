cbuffer Parameters : register(b0)
{
    uint width;
    uint height;
    uint screenWidth;
    uint screenHeight;
};
StructuredBuffer<float> initialTracer : register(t0);

float4 FullscreenVS(uint vertex : SV_VertexID) : SV_Position
{
    float2 p = float2(vertex == 2u ? 3.0 : -1.0, vertex == 1u ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

float4 TracerPS(float4 position : SV_Position) : SV_Target0
{
    uint x = min(uint(position.x * float(width) / float(screenWidth)), width - 1u);
    uint y = min(uint(position.y * float(height) / float(screenHeight)), height - 1u);
    float density = saturate(initialTracer[(y + 1u) * (width + 2u) + x + 1u]);
    return float4(density * 0.25, density * 0.65, density, 1.0);
}
