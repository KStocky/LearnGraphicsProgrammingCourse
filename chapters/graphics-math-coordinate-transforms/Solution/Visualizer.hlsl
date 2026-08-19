#include <LgpShaderCommon.hlsli>

struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

cbuffer LinePassConstants : register(b0)
{
    row_major float4x4 gModel;
    row_major float4x4 gViewProjection;
    float4 gColorTint;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float4 world = LgpTransformPosition(input.position, gModel);
    output.position = mul(world, gViewProjection);
    output.color = input.color * gColorTint;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    return input.color;
}
