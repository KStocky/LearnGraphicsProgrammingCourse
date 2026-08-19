struct VertexInput
{
    float4 position : POSITION;
    float3 attribute : ATTRIBUTE;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 perspectiveAttribute : COLOR0;
    noperspective float3 affineAttribute : COLOR1;
};

struct DisplayConstants
{
    uint interpolationMode;
    uint visualizationMode;
};

ConstantBuffer<DisplayConstants> display : register(b0);

VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    output.position = input.position;
    output.perspectiveAttribute = input.attribute;
    output.affineAttribute = input.attribute;
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target
{
    if (display.visualizationMode == 1)
    {
        return float4(input.position.zzz, 1.0);
    }

    float3 attribute =
        display.interpolationMode == 0 ? input.perspectiveAttribute : input.affineAttribute;
    return float4(attribute, 1.0);
}
