struct Record
{
    uint key;
    uint identity;
    uint sourceIndex;
    float3 position;
};

cbuffer DrawConstants : register(b1)
{
    float viewScale;
    float pointHalfExtent;
};
StructuredBuffer<Record> drawRecords : register(t0);

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

VertexOutput PointVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
        float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0)
    };
    Record record = drawRecords[instance];
    VertexOutput output;
    if (record.key == 0xffffffffu)
    {
        output.position = float4(2.0, 2.0, 0.0, 1.0);
        output.color = 0.0.xxx;
    }
    else
    {
        output.position = float4(record.position.xy * viewScale + corners[vertex] * pointHalfExtent, 0.0, 1.0);
        output.color = float3(0.3, 0.5, 0.35) + float3(float(record.key % 5u) * 0.12, 0.0, 0.2);
    }
    return output;
}

float4 PointPS(VertexOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
