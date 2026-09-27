cbuffer Parameters : register(b0)
{
    uint nx, ny;
    float h, dt;
    uint boundaryMode, method, stopMode, limit;
    float tolerance;
    uint iteration, colour, screenWidth, screenHeight;
};

StructuredBuffer<float> inputX : register(t0);
StructuredBuffer<float> inputY : register(t1);
StructuredBuffer<float> preview : register(t2);
RWStructuredBuffer<float> pressure : register(u0);
RWStructuredBuffer<float> scratch : register(u1);
RWStructuredBuffer<float> source : register(u2);
RWStructuredBuffer<float> residual : register(u3);
RWStructuredBuffer<float> direction : register(u4);
RWStructuredBuffer<float> applied : register(u5);
RWStructuredBuffer<float> outputX : register(u6);
RWStructuredBuffer<float> outputY : register(u7);
RWStructuredBuffer<float> history : register(u8);

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
int Wrap(int value, int size)
{
    return (value % size + size) % size;
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

float PressureAt(int x, int y, int cx, int cy, uint field)
{
    if (x < 0 || y < 0 || x >= int(nx) || y >= int(ny))
    {
        if (boundaryMode == 0)
        {
            x = cx;
            y = cy;
        }
        else if (boundaryMode == 1)
            return 0.0;
        else
        {
            x = Wrap(x, int(nx));
            y = Wrap(y, int(ny));
        }
    }
    uint i = Cell(x, y);
    if (field == 0)
        return pressure[i];
    return direction[i];
}
float Operator(int x, int y, uint field)
{
    float centre = PressureAt(x, y, x, y, field);
    float sum = PressureAt(x - 1, y, x, y, field) + PressureAt(x + 1, y, x, y, field) +
                PressureAt(x, y - 1, x, y, field) + PressureAt(x, y + 1, x, y, field);
    return (4.0 * centre - sum) / (h * h);
}
float Diagonal(int x, int y)
{
    uint walls = uint(x == 0) + uint(y == 0) + uint(x + 1 == int(nx)) + uint(y + 1 == int(ny));
    return (4.0 - (boundaryMode == 0 ? float(walls) : 0.0)) / (h * h);
}
bool Active()
{
    float initial = history[0];
    float state = history[limit + 2];
    return initial > 0.0 && state >= 0.0 && (state == 0.0 || (stopMode == 0 && method != 2));
}

[numthreads(8, 8, 1)]
void SourceCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= nx || cell.y >= ny)
        return;
    int x = int(cell.x), y = int(cell.y);
    uint i = Cell(x, y);
    source[i] = -(FaceX(x + 1, y) - FaceX(x, y) + FaceY(x, y + 1) - FaceY(x, y)) / (h * dt);
    pressure[i] = 0.0;
    scratch[i] = 0.0;
}

[numthreads(1, 1, 1)]
void InitializeCS()
{
    float sum = 0.0, squares = 0.0;
    for (int y = 0; y < int(ny); ++y)
        for (int x = 0; x < int(nx); ++x)
        {
            float b = source[Cell(x, y)];
            sum += b;
            squares += b * b;
        }
    float initial = sqrt(squares / float(nx * ny));
    float mean = sum / float(nx * ny);
    history[0] = initial;
    history[limit + 1] = 0.0;
    history[limit + 2] = initial == 0.0 ? 1.0 : 0.0;
    if (!isfinite(initial) || (boundaryMode != 1 && abs(mean) > 1.0e-6 * initial))
        history[limit + 2] = -1.0;
    float norm = 0.0;
    for (int y = 0; y < int(ny); ++y)
        for (int x = 0; x < int(nx); ++x)
        {
            uint i = Cell(x, y);
            float b = source[i] - (boundaryMode == 1 ? 0.0 : mean);
            source[i] = b;
            residual[i] = b;
            direction[i] = b;
            norm += b * b;
        }
    history[limit + 3] = norm;
}

[numthreads(8, 8, 1)]
void JacobiCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= nx || cell.y >= ny || !Active())
        return;
    int x = int(cell.x), y = int(cell.y);
    uint i = Cell(x, y);
    scratch[i] = pressure[i] + (source[i] - Operator(x, y, 0)) / Diagonal(x, y);
}
[numthreads(8, 8, 1)]
void CopyPressureCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= nx || cell.y >= ny || !Active())
        return;
    pressure[Cell(int(cell.x), int(cell.y))] = scratch[Cell(int(cell.x), int(cell.y))];
}
[numthreads(8, 8, 1)]
void RedBlackCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= nx || cell.y >= ny || !Active() || ((cell.x + cell.y) & 1) != colour)
        return;
    int x = int(cell.x), y = int(cell.y);
    uint i = Cell(x, y);
    pressure[i] += (source[i] - Operator(x, y, 0)) / Diagonal(x, y);
}

[numthreads(8, 8, 1)]
void CgOperatorCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= nx || cell.y >= ny || !Active())
        return;
    applied[Cell(int(cell.x), int(cell.y))] = Operator(int(cell.x), int(cell.y), 1);
}
// A separate single-work-item dispatch reduces the grid between parallel vector passes.
[numthreads(1, 1, 1)]
void CgAlphaCS()
{
    if (!Active())
        return;
    float denominator = 0.0;
    for (int y = 0; y < int(ny); ++y)
        for (int x = 0; x < int(nx); ++x)
        {
            uint i = Cell(x, y);
            denominator += direction[i] * applied[i];
        }
    float previous = history[limit + 3];
    if (!isfinite(denominator) || !isfinite(previous) || denominator <= 0.0 || previous <= 0.0)
    {
        history[limit + 2] = -1.0;
        return;
    }
    history[limit + 4] = previous / denominator;
}
[numthreads(8, 8, 1)]
void CgUpdateCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= nx || cell.y >= ny || !Active())
        return;
    uint i = Cell(int(cell.x), int(cell.y));
    float alpha = history[limit + 4];
    pressure[i] += alpha * direction[i];
    residual[i] -= alpha * applied[i];
}
[numthreads(1, 1, 1)]
void CgBetaCS()
{
    if (!Active())
        return;
    float next = 0.0;
    for (int y = 0; y < int(ny); ++y)
        for (int x = 0; x < int(nx); ++x)
        {
            float r = residual[Cell(x, y)];
            next += r * r;
        }
    if (!isfinite(next))
    {
        history[limit + 2] = -1.0;
        return;
    }
    history[limit + 5] = next > 0.0 ? next / history[limit + 3] : 0.0;
    history[limit + 3] = next;
}
[numthreads(8, 8, 1)]
void CgDirectionCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= nx || cell.y >= ny || !Active())
        return;
    uint i = Cell(int(cell.x), int(cell.y));
    direction[i] = residual[i] + history[limit + 5] * direction[i];
}

[numthreads(1, 1, 1)]
void GaugeMeanCS()
{
    if (!Active() || boundaryMode == 1)
        return;
    float mean = 0.0;
    for (int y = 0; y < int(ny); ++y)
        for (int x = 0; x < int(nx); ++x)
            mean += pressure[Cell(x, y)];
    history[limit + 6] = mean / float(nx * ny);
}
[numthreads(8, 8, 1)]
void GaugeApplyCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= nx || cell.y >= ny || !Active() || boundaryMode == 1)
        return;
    uint i = Cell(int(cell.x), int(cell.y));
    pressure[i] -= history[limit + 6];
}
[numthreads(8, 8, 1)]
void TrueResidualCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= nx || cell.y >= ny || !Active())
        return;
    int x = int(cell.x), y = int(cell.y);
    scratch[Cell(x, y)] = source[Cell(x, y)] - Operator(x, y, 0);
}
[numthreads(1, 1, 1)]
void ReduceResidualCS()
{
    if (!Active())
        return;
    float sum = 0.0;
    for (int y = 0; y < int(ny); ++y)
        for (int x = 0; x < int(nx); ++x)
        {
            float r = scratch[Cell(x, y)];
            sum += r * r;
        }
    float rms = sqrt(sum / float(nx * ny));
    history[iteration + 1] = rms;
    history[limit + 1] = float(iteration + 1);
    history[limit + 2] = !isfinite(rms) ? -1.0 : (rms <= tolerance * history[0] ? 1.0 : 0.0);
}

[numthreads(8, 8, 1)]
void PressureGhostCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= nx + 2 || cell.y >= ny + 2)
        return;
    int x = int(cell.x) - 1, y = int(cell.y) - 1;
    if (x >= 0 && y >= 0 && x < int(nx) && y < int(ny))
        return;
    pressure[Cell(x, y)] = PressureAt(x, y, clamp(x, 0, int(nx) - 1), clamp(y, 0, int(ny) - 1), 0);
}
[numthreads(8, 8, 1)]
void GradientXCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= nx + 3 || cell.y >= ny + 2)
        return;
    int x = int(cell.x) - 1, y = int(cell.y) - 1;
    int sx = boundaryMode == 2 ? Wrap(x, int(nx)) : clamp(x, 0, int(nx));
    int sy = boundaryMode == 2 ? Wrap(y, int(ny)) : clamp(y, 0, int(ny) - 1);
    float value = FaceX(sx, sy);
    if ((sx > 0 && sx < int(nx)) || boundaryMode != 0)
        value -= dt * (PressureAt(sx, sy, sx, sy, 0) - PressureAt(sx - 1, sy, sx - 1, sy, 0)) / h;
    outputX[XFace(x, y)] = value;
}
[numthreads(8, 8, 1)]
void GradientYCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= nx + 2 || cell.y >= ny + 3)
        return;
    int x = int(cell.x) - 1, y = int(cell.y) - 1;
    int sx = boundaryMode == 2 ? Wrap(x, int(nx)) : clamp(x, 0, int(nx) - 1);
    int sy = boundaryMode == 2 ? Wrap(y, int(ny)) : clamp(y, 0, int(ny));
    float value = FaceY(sx, sy);
    if ((sy > 0 && sy < int(ny)) || boundaryMode != 0)
        value -= dt * (PressureAt(sx, sy, sx, sy, 0) - PressureAt(sx, sy - 1, sx, sy - 1, 0)) / h;
    outputY[YFace(x, y)] = value;
}

float4 FullscreenVS(uint id : SV_VertexID)
    : SV_POSITION
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 HeatPS(float4 position : SV_POSITION) : SV_Target
{
    int x = clamp(int(position.x * float(nx) / float(screenWidth)), 0, int(nx) - 1);
    int y = clamp(int(position.y * float(ny) / float(screenHeight)), 0, int(ny) - 1);
    float p = preview[Cell(x, y)];
    float magnitude = saturate(abs(p) * 2.0);
    return p >= 0.0 ? float4(0.08 + magnitude, 0.08 + 0.3 * magnitude, 0.12, 1)
                    : float4(0.08, 0.12 + 0.4 * magnitude, 0.1 + magnitude, 1);
}
