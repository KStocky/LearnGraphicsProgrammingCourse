struct InstanceData
{
    float4 bounds;
    float4 display;
    uint stableId;
    uint instanceDataIndex;
    uint firstDrawTemplate;
    uint lodCount;
    uint previousLod;
    uint3 padding;
};

StructuredBuffer<InstanceData> gInstances : register(t0);

cbuffer DrawConstants : register(b0)
{
    uint gStableId;
    uint gLod;
    uint gInstanceDataIndex;
};

float2 VertexPosition(uint vertexId)
{
    const float2 positions[4] = {
        float2(-1.0, 1.0),
        float2(1.0, 1.0),
        float2(-1.0, -1.0),
        float2(1.0, -1.0),
    };
    return positions[min(vertexId, 3u)];
}

struct VsOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VsOutput IndirectVS(uint vertexId : SV_VertexID)
{
    InstanceData instance = gInstances[gInstanceDataIndex];
    float2 extent = float2(instance.display.z, instance.display.z);
    VsOutput output;
    output.position = float4(instance.display.xy + VertexPosition(vertexId) * extent, 0.0, 1.0);
    output.color = float4(0.15 + 0.25 * float(gLod), 0.75 - 0.2 * float(gLod),
                          0.25 + float(gStableId % 5u) * 0.12, 1.0);
    return output;
}

float4 ColorPS(VsOutput input) : SV_Target0
{
    return input.color;
}
