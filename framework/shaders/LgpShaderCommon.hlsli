#ifndef LGP_FRAMEWORK_SHADER_COMMON_HLSLI
#define LGP_FRAMEWORK_SHADER_COMMON_HLSLI

float4 LgpTransformPosition(float3 position, row_major float4x4 transform)
{
    return mul(float4(position, 1.0f), transform);
}

float3 LgpTransformDirection(float3 direction, row_major float4x4 transform)
{
    return mul(float4(direction, 0.0f), transform).xyz;
}

float3 LgpPerspectiveDivide(float4 clipPosition)
{
    return clipPosition.xyz / clipPosition.w;
}

#endif // LGP_FRAMEWORK_SHADER_COMMON_HLSLI
