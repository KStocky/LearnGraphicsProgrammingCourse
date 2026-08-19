#include "../Common/VisibilityBufferShared.hlsli"

struct RasterDrawConstants
{
    uint4 identifiers;
    float4 baseTintAndRoughness;
    float4 materialParameters;
};

ConstantBuffer<RasterDrawConstants> gDraw : register(b1);
Texture2D<float4> gPositionRoughness : register(t0);
Texture2D<float4> gNormalMetalness : register(t1);
Texture2D<float4> gTangent : register(t2);
Texture2D<float4> gBaseColor : register(t3);
Texture2D<float2> gTextureCoordinates : register(t4);
Texture2D<float4> gTextureGradients : register(t5);
Texture2D<uint2> gIdentity : register(t6);
Texture2D<float> gDepth : register(t7);
Texture2D<float4> gMaterialTexture : register(t8);
RWTexture2D<float4> gOutput : register(u0);
RWStructuredBuffer<PixelDiagnostics> gDiagnostics : register(u1);
SamplerState gMaterialSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 textureCoordinates : TEXCOORD0;
};

struct RasterVertexOutput
{
    float4 position : SV_Position;
    float3 viewPosition : POSITION0;
    float3 normal : NORMAL0;
    float4 tangent : TANGENT0;
    float2 textureCoordinates : TEXCOORD0;
};

struct GBufferOutput
{
    float4 positionRoughness : SV_Target0;
    float4 normalMetalness : SV_Target1;
    float4 tangent : SV_Target2;
    float4 baseColor : SV_Target3;
    float2 textureCoordinates : SV_Target4;
    float4 textureGradients : SV_Target5;
    uint2 identity : SV_Target6;
};

RasterVertexOutput RasterVS(VertexInput input)
{
    RasterVertexOutput output;
    output.position = mul(float4(input.position, 1.0), gFrame.projection);
    output.viewPosition = input.position;
    output.normal = input.normal;
    output.tangent = input.tangent;
    output.textureCoordinates = input.textureCoordinates;
    return output;
}

GBufferOutput RasterPS(RasterVertexOutput input, uint primitiveIdentifier : SV_PrimitiveID)
{
    GBufferOutput output;
    float2 textureDdx = ddx(input.textureCoordinates);
    float2 textureDdy = ddy(input.textureCoordinates);
    float3 material = gMaterialTexture.Sample(gMaterialSampler, input.textureCoordinates).rgb;
    float3 normal = normalize(input.normal);
    float3 tangent = normalize(input.tangent.xyz - normal * dot(normal, input.tangent.xyz));

    output.positionRoughness = float4(input.viewPosition, gDraw.baseTintAndRoughness.w);
    output.normalMetalness = float4(normal, gDraw.materialParameters.x);
    output.tangent = float4(tangent, input.tangent.w);
    output.baseColor = float4(material * gDraw.baseTintAndRoughness.rgb, 1.0);
    output.textureCoordinates = input.textureCoordinates;
    output.textureGradients = float4(textureDdx, textureDdy);
    output.identity = uint2(gDraw.identifiers.x, primitiveIdentifier);
    return output;
}

void StoreBackground(uint2 pixel, uint linearIndex)
{
    gOutput[pixel] = float4(0.0, 0.0, 0.0, 1.0);
    PixelDiagnostics diagnostics = (PixelDiagnostics)0;
    gDiagnostics[linearIndex] = diagnostics;
}

void StoreInvalid(uint2 pixel, uint linearIndex, uint drawIdentifier, uint primitiveIdentifier)
{
    gOutput[pixel] = float4(1.0, 0.0, 1.0, 1.0);
    PixelDiagnostics diagnostics = (PixelDiagnostics)0;
    diagnostics.status = 2u;
    diagnostics.drawIdentifier = drawIdentifier;
    diagnostics.primitiveIdentifier = primitiveIdentifier;
    gDiagnostics[linearIndex] = diagnostics;
}

[numthreads(8, 8, 1)]
void ShadeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= gFrame.dimensions.xy))
    {
        return;
    }
    uint linearIndex = pixel.y * gFrame.dimensions.x + pixel.x;
    float depth = gDepth.Load(int3(pixel, 0));
    float backgroundDepth = gFrame.slicing.w == 0.0 ? 1.0 : 0.0;
    if (depth == backgroundDepth)
    {
        StoreBackground(pixel, linearIndex);
        return;
    }

    float4 positionRoughness = gPositionRoughness.Load(int3(pixel, 0));
    float4 normalMetalness = gNormalMetalness.Load(int3(pixel, 0));
    float4 tangent = gTangent.Load(int3(pixel, 0));
    float3 baseColor = gBaseColor.Load(int3(pixel, 0)).rgb;
    float2 uv = gTextureCoordinates.Load(int3(pixel, 0));
    float4 gradients = gTextureGradients.Load(int3(pixel, 0));
    uint2 identity = gIdentity.Load(int3(pixel, 0));
    if (identity.x == 0u || identity.x > gFrame.counts.x || identity.y >= 64u)
    {
        StoreInvalid(pixel, linearIndex, identity.x, identity.y);
        return;
    }

    uint clusterIndex;
    if (!ComputeClusterIndex(pixel, positionRoughness.z, clusterIndex))
    {
        StoreInvalid(pixel, linearIndex, identity.x, identity.y);
        return;
    }
    float3 linearColor;
    if (!ShadeCluster(clusterIndex, positionRoughness.xyz, normalize(normalMetalness.xyz), baseColor,
                      positionRoughness.w, normalMetalness.w, linearColor))
    {
        StoreInvalid(pixel, linearIndex, identity.x, identity.y);
        return;
    }

    gOutput[pixel] =
        DebugOutput(linearColor, uv, gradients.xy, gradients.zw, identity.x, identity.y);
    PixelDiagnostics diagnostics = (PixelDiagnostics)0;
    diagnostics.viewPosition = positionRoughness.xyz;
    diagnostics.status = 1u;
    diagnostics.normal = normalize(normalMetalness.xyz);
    diagnostics.drawIdentifier = identity.x;
    diagnostics.tangent = tangent;
    diagnostics.textureCoordinates = uv;
    diagnostics.textureDdx = gradients.xy;
    diagnostics.textureDdy = gradients.zw;
    diagnostics.primitiveIdentifier = identity.y;
    diagnostics.clusterIndex = clusterIndex;
    gDiagnostics[linearIndex] = diagnostics;
}
