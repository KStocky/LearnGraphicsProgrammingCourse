struct ThreadOutput
{
    float4 value;
    uint branchClass;
    uint status;
    uint threadIndex;
    uint valueChecksum;
};

cbuffer DispatchConstants : register(b0)
{
    uint ElementCount;
};

RWStructuredBuffer<ThreadOutput> Outputs : register(u0);

float4 BaseValue(uint threadIndex)
{
    return float4(threadIndex & 255u,
                  (threadIndex * 3u) & 255u,
                  (threadIndex * 5u + 7u) & 255u,
                  (threadIndex * 11u + 13u) & 255u) /
           256.0;
}

uint HashValue(float4 value, uint threadIndex)
{
    uint hash = 2166136261u;
    hash = (hash ^ asuint(value.x)) * 16777619u;
    hash = (hash ^ asuint(value.y)) * 16777619u;
    hash = (hash ^ asuint(value.z)) * 16777619u;
    hash = (hash ^ asuint(value.w)) * 16777619u;
    return (hash ^ threadIndex) * 16777619u;
}

void StoreOutput(uint threadIndex, bool takeThenPath, float4 value)
{
    float4 reference = BaseValue(threadIndex);
    uint checksum = HashValue(value, threadIndex);

    ThreadOutput output;
    output.value = value;
    output.branchClass = takeThenPath ? 1u : 0u;
    output.status = 1u;
    output.status |= all(value == reference) ? 2u : 0u;
    output.status |= checksum == HashValue(reference, threadIndex) ? 4u : 0u;
    output.threadIndex = threadIndex;
    output.valueChecksum = checksum;
    Outputs[threadIndex] = output;
}

[numthreads(64, 1, 1)]
void CoherentLowPressureCS(uint3 dispatchThreadId : SV_DispatchThreadID,
                           uint3 groupId : SV_GroupID)
{
    uint threadIndex = dispatchThreadId.x;
    if (threadIndex >= ElementCount)
    {
        return;
    }

    float4 base = BaseValue(threadIndex);
    bool takeThenPath = (groupId.x & 1u) == 0u;
    precise float4 value;
    [branch]
    if (takeThenPath)
    {
        precise float4 shifted = base + 0.125;
        value = shifted - 0.125;
    }
    else
    {
        precise float4 shifted = base - 0.125;
        value = shifted + 0.125;
    }
    StoreOutput(threadIndex, takeThenPath, value);
}

struct FullscreenVertex
{
    float4 position : SV_Position;
};

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenVertex output;
    output.position = float4(vertexId == 2u ? 3.0 : -1.0,
                             vertexId == 1u ? 3.0 : -1.0,
                             0.0,
                             1.0);
    return output;
}

cbuffer DisplayConstants : register(b0)
{
    uint DisplayWidth;
    uint DisplayHeight;
    uint DisplayElementCount;
    uint DiagnosticView;
    uint Variant;
    uint ExpectedStatus;
    uint ResidentWaves;
    uint MaximumResidentWaves;
    uint WaveOpsSupported;
    uint WaveLaneCountMinimum;
    uint WaveLaneCountMaximum;
    uint IsWarp;
};

StructuredBuffer<ThreadOutput> DisplayOutputs : register(t0);

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
    uint2 pixel = min(uint2(position.xy), uint2(DisplayWidth - 1u, DisplayHeight - 1u));
    uint outputIndex = min((pixel.x * DisplayElementCount) / DisplayWidth, DisplayElementCount - 1u);
    ThreadOutput output = DisplayOutputs[outputIndex];
    if (output.status != ExpectedStatus)
    {
        return float4(1.0, 0.0, 1.0, 1.0);
    }

    if (DiagnosticView == 0u)
    {
        return float4(output.value.xyz, 1.0);
    }
    if (DiagnosticView == 1u)
    {
        float groupStripe = ((output.threadIndex / 64u) & 1u) == 0u ? 0.12 : 0.0;
        float3 branchColor = output.branchClass != 0u ? float3(0.1, 0.75, 0.25) : float3(0.95, 0.35, 0.08);
        return float4(saturate(branchColor + groupStripe), 1.0);
    }
    if (DiagnosticView == 2u)
    {
        float occupancy = float(ResidentWaves) / max(float(MaximumResidentWaves), 1.0);
        float normalizedX = (float(pixel.x) + 0.5) / float(DisplayWidth);
        float3 variantTint = float3(0.15 + 0.12 * float(Variant), 0.55, 0.9 - 0.12 * float(Variant));
        return float4(normalizedX <= occupancy ? variantTint : variantTint * 0.12, 1.0);
    }

    float normalizedX = (float(pixel.x) + 0.5) / float(DisplayWidth);
    float3 evidenceColor =
        normalizedX < (1.0 / 3.0)
            ? float3(0.42, 0.2, 0.72)
            : (normalizedX < (2.0 / 3.0) ? float3(0.12, 0.68, 0.32) : float3(0.18, 0.08, 0.08));
    float waveRange = float(WaveLaneCountMaximum - WaveLaneCountMinimum) / 64.0;
    if (pixel.y < 8u)
    {
        evidenceColor = WaveOpsSupported != 0u ? float3(0.1, 0.35 + waveRange, IsWarp != 0u ? 0.8 : 0.45)
                                              : float3(0.45, 0.2, 0.1);
    }
    return float4(saturate(evidenceColor), 1.0);
}
