cbuffer Parameters : register(b0)
{
    uint nx, ny;
    float h, dt;
    uint boundaryMode, screenWidth, screenHeight;
};
StructuredBuffer<float> inputX : register(t0);
StructuredBuffer<float> inputY : register(t1);
StructuredBuffer<float> preview : register(t2);
RWStructuredBuffer<float> divergence : register(u0);
RWStructuredBuffer<float> normalizedX : register(u1);
RWStructuredBuffer<float> normalizedY : register(u2);

uint Cell(int x, int y)
{
    return uint(y + 1) * (nx + 2) + uint(x + 1);
}
uint XFace(int x, int y)
{
    return uint(y + 1) * (nx + 3) + uint(x + 1);
}
uint YFace(int x, int y)
{
    return uint(y + 1) * (nx + 2) + uint(x + 1);
}
int Wrap(int v, int count)
{
    return (v % count + count) % count;
}
float FaceX(int x, int y)
{
    if (boundaryMode == 0 && (x == 0 || x == int(nx)))
        return 0.0;
    if (boundaryMode == 2 && x == int(nx))
        x = 0;
    return inputX[XFace(x, y)];
}
float FaceY(int x, int y)
{
    if (boundaryMode == 0 && (y == 0 || y == int(ny)))
        return 0.0;
    if (boundaryMode == 2 && y == int(ny))
        y = 0;
    return inputY[YFace(x, y)];
}

[numthreads(8, 8, 1)]
void DivergenceCS(uint2 cell : SV_DispatchThreadID)
{
    int x = int(cell.x) - 1, y = int(cell.y) - 1;
    if (cell.x < nx && cell.y < ny)
    {
        int cx = int(cell.x), cy = int(cell.y);
        divergence[Cell(cx, cy)] = (FaceX(cx + 1, cy) - FaceX(cx, cy) + FaceY(cx, cy + 1) - FaceY(cx, cy)) / h;
    }
    if (cell.x < nx + 3 && cell.y < ny + 2)
    {
        int sx = boundaryMode == 2 ? Wrap(x, int(nx)) : clamp(x, 0, int(nx));
        int sy = boundaryMode == 2 ? Wrap(y, int(ny)) : clamp(y, 0, int(ny) - 1);
        normalizedX[XFace(x, y)] = FaceX(sx, sy);
    }
    if (cell.x < nx + 2 && cell.y < ny + 3)
    {
        int sx = boundaryMode == 2 ? Wrap(x, int(nx)) : clamp(x, 0, int(nx) - 1);
        int sy = boundaryMode == 2 ? Wrap(y, int(ny)) : clamp(y, 0, int(ny));
        normalizedY[YFace(x, y)] = FaceY(sx, sy);
    }
} float4 FullscreenVS(uint id : SV_VertexID)
    : SV_POSITION
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 HeatPS(float4 position : SV_POSITION) : SV_Target
{
    int x = clamp(int(position.x * float(nx) / float(screenWidth)), 0, int(nx) - 1);
    int y = clamp(int(position.y * float(ny) / float(screenHeight)), 0, int(ny) - 1);
    float d = preview[Cell(x, y)];
    float magnitude = saturate(abs(d) * 3.0);
    return d >= 0.0 ? float4(0.1 + magnitude, 0.1 + 0.3 * magnitude, 0.1, 1)
                    : float4(0.1, 0.1 + 0.3 * magnitude, 0.1 + magnitude, 1);
}
