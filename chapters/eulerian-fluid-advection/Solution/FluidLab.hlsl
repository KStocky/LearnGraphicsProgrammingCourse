cbuffer Parameters : register(b0)
{
    uint width;
    uint height;
    float cellSize;
    float timeStep;
    uint boundaryMode;
    uint correctedMode;
    uint screenWidth;
    uint screenHeight;
    float accelerationX;
    float accelerationY;
};

StructuredBuffer<float> sourceScalar : register(t0);
StructuredBuffer<float> sourceU : register(t1);
StructuredBuffer<float> sourceV : register(t2);
StructuredBuffer<float> prediction : register(t3);
StructuredBuffer<float> reverseField : register(t4);
StructuredBuffer<float2> outputField : register(t5);
StructuredBuffer<float> transportedU : register(t6);
StructuredBuffer<float> transportedV : register(t7);
RWStructuredBuffer<float> intermediate : register(u0);
RWStructuredBuffer<float2> resultField : register(u1);
RWStructuredBuffer<float> velocityU : register(u2);
RWStructuredBuffer<float> velocityV : register(u3);

int Wrap(int index, int count)
{
    return (index % count + count) % count;
}

// Map a ghost access to a physical interior sample. For periodic face fields,
// the last face aliases face zero; solid walls erase only normal face velocity.
float Fetch(uint kind, int x, int y)
{
    int nx = int(width) + (kind == 1u ? 1 : 0);
    int ny = int(height) + (kind == 2u ? 1 : 0);
    if (boundaryMode == 2u)
    {
        x = Wrap(x, int(width));
        y = Wrap(y, int(height));
    }
    else
    {
        x = clamp(x, 0, nx - 1);
        y = clamp(y, 0, ny - 1);
        if (boundaryMode == 0u &&
            ((kind == 1u && (x == 0 || x == nx - 1)) ||
             (kind == 2u && (y == 0 || y == ny - 1))))
            return 0.0;
    }
    uint address = uint(y + 1) * uint(nx + 2) + uint(x + 1);
    if (kind == 0u) return sourceScalar[address];
    if (kind == 1u) return sourceU[address] + accelerationX * timeStep;
    if (kind == 2u) return sourceV[address] + accelerationY * timeStep;
    return prediction[address];
}

float SampleField(uint kind, float2 world)
{
    float2 offset = float2(kind == 1u ? 0.0 : 0.5, kind == 2u ? 0.0 : 0.5);
    float2 p = world / cellSize - offset;
    int2 lower = int2(floor(p));
    float2 fraction = p - floor(p);
    float a = lerp(Fetch(kind, lower.x, lower.y), Fetch(kind, lower.x + 1, lower.y), fraction.x);
    float b = lerp(Fetch(kind, lower.x, lower.y + 1), Fetch(kind, lower.x + 1, lower.y + 1), fraction.x);
    return lerp(a, b, fraction.y);
}

float2 Departure(int2 cell, float direction)
{
    float2 centre = (float2(cell) + 0.5) * cellSize;
    float2 velocity = float2(SampleField(1u, centre), SampleField(2u, centre));
    return centre - direction * timeStep * velocity;
}

[numthreads(8, 8, 1)]
void ForwardCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= width || cell.y >= height) return;
    intermediate[(cell.y + 1u) * (width + 2u) + cell.x + 1u] =
        SampleField(0u, Departure(int2(cell), 1.0));
}

[numthreads(8, 8, 1)]
void ReverseCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= width || cell.y >= height) return;
    intermediate[(cell.y + 1u) * (width + 2u) + cell.x + 1u] =
        SampleField(3u, Departure(int2(cell), -1.0));
}

[numthreads(8, 8, 1)]
void CorrectCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x >= width || cell.y >= height) return;
    uint index = (cell.y + 1u) * (width + 2u) + cell.x + 1u;
    float value = prediction[index];
    float clamped = 0.0;
    if (correctedMode != 0u)
    {
        float2 p = Departure(int2(cell), 1.0) / cellSize - 0.5;
        int2 lower = int2(floor(p));
        float a = Fetch(0u, lower.x, lower.y);
        float b = Fetch(0u, lower.x + 1, lower.y);
        float c = Fetch(0u, lower.x, lower.y + 1);
        float d = Fetch(0u, lower.x + 1, lower.y + 1);
        float candidate = value + 0.5 * (sourceScalar[index] - reverseField[index]);
        value = clamp(candidate, min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
        clamped = candidate != value ? 1.0 : 0.0;
    }
    resultField[index] = float2(value, clamped);
}

[numthreads(8, 8, 1)]
void GhostCS(uint2 thread : SV_DispatchThreadID)
{
    if (thread.x >= width + 2u || thread.y >= height + 2u) return;
    int x = int(thread.x) - 1;
    int y = int(thread.y) - 1;
    if (boundaryMode == 2u)
    {
        x = Wrap(x, int(width));
        y = Wrap(y, int(height));
    }
    else
    {
        x = clamp(x, 0, int(width) - 1);
        y = clamp(y, 0, int(height) - 1);
    }
    float2 value = outputField[uint(y + 1) * (width + 2u) + uint(x + 1)];
    resultField[thread.y * (width + 2u) + thread.x] = value;
}

[numthreads(8, 8, 1)]
void TransportUCS(uint2 face : SV_DispatchThreadID)
{
    if (face.x > width || face.y >= height) return;
    float2 centre = float2(float(face.x), float(face.y) + 0.5) * cellSize;
    float2 velocity = float2(SampleField(1u, centre), SampleField(2u, centre));
    velocityU[(face.y + 1u) * (width + 3u) + face.x + 1u] =
        SampleField(1u, centre - timeStep * velocity);
}

[numthreads(8, 8, 1)]
void TransportVCS(uint2 face : SV_DispatchThreadID)
{
    if (face.x >= width || face.y > height) return;
    float2 centre = float2(float(face.x) + 0.5, float(face.y)) * cellSize;
    float2 velocity = float2(SampleField(1u, centre), SampleField(2u, centre));
    velocityV[(face.y + 1u) * (width + 2u) + face.x + 1u] =
        SampleField(2u, centre - timeStep * velocity);
}

int2 FaceSource(int2 face, uint kind)
{
    int nx = int(width) + (kind == 1u ? 1 : 0);
    int ny = int(height) + (kind == 2u ? 1 : 0);
    if (boundaryMode == 2u)
        return int2(Wrap(face.x, int(width)), Wrap(face.y, int(height)));
    return clamp(face, int2(0, 0), int2(nx - 1, ny - 1));
}

[numthreads(8, 8, 1)]
void VelocityGhostCS(uint2 cell : SV_DispatchThreadID)
{
    if (cell.x < width + 3u && cell.y < height + 2u)
    {
        int2 face = int2(cell) - 1;
        int2 source = FaceSource(face, 1u);
        float value = transportedU[uint(source.y + 1) * (width + 3u) + uint(source.x + 1)];
        if (boundaryMode == 0u && (source.x == 0 || source.x == int(width)))
            value = 0.0;
        velocityU[cell.y * (width + 3u) + cell.x] = value;
    }
    if (cell.x < width + 2u && cell.y < height + 3u)
    {
        int2 face = int2(cell) - 1;
        int2 source = FaceSource(face, 2u);
        float value = transportedV[uint(source.y + 1) * (width + 2u) + uint(source.x + 1)];
        if (boundaryMode == 0u && (source.y == 0 || source.y == int(height)))
            value = 0.0;
        velocityV[cell.y * (width + 2u) + cell.x] = value;
    }
}

float4 FullscreenVS(uint vertex : SV_VertexID) : SV_Position
{
    float2 p = float2(vertex == 2u ? 3.0 : -1.0, vertex == 1u ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

float4 TracerPS(float4 position : SV_Position) : SV_Target0
{
    uint x = min(uint(position.x * float(width) / float(screenWidth)), width - 1u);
    uint y = min(uint(position.y * float(height) / float(screenHeight)), height - 1u);
    float density = saturate(outputField[(y + 1u) * (width + 2u) + x + 1u].x);
    return float4(density * 0.25, density * 0.65, density, 1.0);
}
