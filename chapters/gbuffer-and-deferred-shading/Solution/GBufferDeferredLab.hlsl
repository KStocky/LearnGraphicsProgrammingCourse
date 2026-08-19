struct FrameConstants
{
    row_major float4x4 viewMatrix;
    row_major float4x4 viewProjection;
    float4 projectionData;
    float4 lightDirectionIntensity;
    uint4 options;
};

struct ObjectConstants
{
    float3 translation;
    float roughness;
    float3 baseColor;
    float metalness;
    uint4 metadata;
};

ConstantBuffer<FrameConstants> frameData : register(b0);
ConstantBuffer<ObjectConstants> objectData : register(b1);
Texture2D<float4> baseColorMetalnessTexture : register(t0);
Texture2D<float2> octahedralNormalTexture : register(t1);
Texture2D<float> roughnessTexture : register(t2);
Texture2D<float> deviceDepthTexture : register(t3);
Texture2D<float2> motionTexture : register(t4);
Texture2D<uint> identityTexture : register(t5);

struct MeshVertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
};

struct GeometryVertex
{
    float4 position : SV_Position;
    float3 viewPosition : POSITION0;
    float3 viewNormal : NORMAL0;
};

struct GBufferOutputs
{
    float4 baseColorMetalness : SV_Target0;
    float4 octahedralNormal : SV_Target1;
    float4 roughness : SV_Target2;
    float4 motion : SV_Target3;
    uint4 identity : SV_Target4;
};

struct FullscreenVertex
{
    float4 position : SV_Position;
};

static const float Pi = 3.14159265358979323846;
static const float MinimumRoughness = 0.045;

float SignNotZero(float value)
{
    return value >= 0.0 ? 1.0 : -1.0;
}

float2 EncodeOctahedral(float3 unitNormal)
{
    float inverseL1 = rcp(abs(unitNormal.x) + abs(unitNormal.y) + abs(unitNormal.z));
    float2 projected = unitNormal.xy * inverseL1;
    if (unitNormal.z < 0.0)
    {
        projected = float2((1.0 - abs(projected.y)) * SignNotZero(projected.x),
                           (1.0 - abs(projected.x)) * SignNotZero(projected.y));
    }
    return (projected * 0.5) + 0.5;
}

float3 DecodeOctahedral(float2 encoded)
{
    float3 normal = float3((encoded * 2.0) - 1.0, 0.0);
    normal.z = 1.0 - abs(normal.x) - abs(normal.y);
    float fold = saturate(-normal.z);
    normal.x += normal.x >= 0.0 ? -fold : fold;
    normal.y += normal.y >= 0.0 ? -fold : fold;
    return normalize(normal);
}

float SrgbEncode(float linearValue)
{
    float value = saturate(linearValue);
    return value <= 0.0031308 ? value * 12.92 : (1.055 * pow(value, 1.0 / 2.4)) - 0.055;
}

float3 DisplayEncode(float3 linearColor)
{
    return float3(SrgbEncode(linearColor.r), SrgbEncode(linearColor.g), SrgbEncode(linearColor.b));
}

float3 SchlickFresnel(float3 f0, float cosine)
{
    float factor = pow(1.0 - saturate(cosine), 5.0);
    return f0 + ((1.0 - f0) * factor);
}

float GgxDistribution(float nDotH, float roughness)
{
    float effectiveRoughness = max(roughness, MinimumRoughness);
    float alpha = effectiveRoughness * effectiveRoughness;
    float alphaSquared = alpha * alpha;
    float term = (nDotH * nDotH * (alphaSquared - 1.0)) + 1.0;
    return alphaSquared / max(Pi * term * term, 1.0e-7);
}

float SmithG1(float nDotDirection, float roughness)
{
    float cosine = saturate(nDotDirection);
    if (cosine <= 0.0)
    {
        return 0.0;
    }
    float effectiveRoughness = max(roughness, MinimumRoughness);
    float alpha = effectiveRoughness * effectiveRoughness;
    float alphaSquared = alpha * alpha;
    float root = sqrt(alphaSquared + ((1.0 - alphaSquared) * cosine * cosine));
    return (2.0 * cosine) / max(cosine + root, 1.0e-7);
}

float BackgroundDepth()
{
    return frameData.options.y == 0U ? 1.0 : 0.0;
}

float ViewDepthFromDeviceDepth(float deviceDepth)
{
    float denominator = deviceDepth - frameData.projectionData.z;
    if (abs(denominator) < 1.0e-8)
    {
        return 0.0;
    }
    return frameData.projectionData.w / denominator;
}

float3 ReconstructViewPosition(int2 pixel, float deviceDepth)
{
    float2 renderSize = float2(frameData.options.z, frameData.options.w);
    float2 uv = (float2(pixel) + 0.5) / renderSize;
    float2 ndc = float2((uv.x * 2.0) - 1.0, 1.0 - (uv.y * 2.0));
    float viewDepth = ViewDepthFromDeviceDepth(deviceDepth);
    return float3(ndc.x * viewDepth * frameData.projectionData.x, ndc.y * viewDepth * frameData.projectionData.y,
                  viewDepth);
}

float3 IdentityColor(uint identity)
{
    if (identity == 0U) return 0.0;
    if (identity == 1U) return float3(0.28, 0.30, 0.34);
    if (identity == 2U) return float3(0.92, 0.18, 0.06);
    if (identity == 3U) return float3(0.92, 0.88, 0.84);
    if (identity == 9U) return float3(0.84, 0.48, 0.14);
    return float3(0.18, 0.64, 0.90);
}

GeometryVertex GeometryVS(MeshVertex input)
{
    GeometryVertex output;
    float4 worldPosition = float4(input.position + objectData.translation, 1.0);
    float4 viewPosition = mul(worldPosition, frameData.viewMatrix);
    output.position = mul(worldPosition, frameData.viewProjection);
    output.viewPosition = viewPosition.xyz;
    output.viewNormal = mul(float4(input.normal, 0.0), frameData.viewMatrix).xyz;
    return output;
}

GBufferOutputs GeometryPS(GeometryVertex input, bool frontFace : SV_IsFrontFace)
{
    GBufferOutputs output;
    float3 viewNormal = normalize(input.viewNormal) * (frontFace ? 1.0 : -1.0);
    output.baseColorMetalness = float4(objectData.baseColor, objectData.metalness);
    output.octahedralNormal = float4(EncodeOctahedral(viewNormal), 0.0, 0.0);
    output.roughness = float4(objectData.roughness, 0.0, 0.0, 0.0);
    output.motion = float4(0.0, 0.0, 0.0, 0.0);
    output.identity = uint4(objectData.metadata.x, 0U, 0U, 0U);
    return output;
}

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    FullscreenVertex output;
    output.position = float4((uv * float2(2.0, -2.0)) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 DeferredPS(FullscreenVertex input) : SV_Target
{
    int2 pixel = int2(input.position.xy);
    float deviceDepth = deviceDepthTexture.Load(int3(pixel, 0));
    uint debugView = frameData.options.x;
    if (deviceDepth == BackgroundDepth())
    {
        float3 background = debugView == 5U ? deviceDepth.xxx : 0.0;
        return float4(debugView == 5U ? saturate(background) : DisplayEncode(background), 1.0);
    }

    float4 baseColorMetalness = baseColorMetalnessTexture.Load(int3(pixel, 0));
    float3 baseColor = baseColorMetalness.rgb;
    float metalness = baseColorMetalness.a;
    float3 viewNormal = DecodeOctahedral(octahedralNormalTexture.Load(int3(pixel, 0)));
    float roughness = roughnessTexture.Load(int3(pixel, 0));
    float2 motion = motionTexture.Load(int3(pixel, 0));
    uint identity = identityTexture.Load(int3(pixel, 0));
    float3 viewPosition = ReconstructViewPosition(pixel, deviceDepth);

    float3 linearColor = 0.0;
    if (debugView == 1U)
    {
        linearColor = baseColor;
        return float4(DisplayEncode(linearColor), 1.0);
    }
    if (debugView == 2U)
    {
        return float4(metalness.xxx, 1.0);
    }
    if (debugView == 3U)
    {
        return float4(saturate((viewNormal * 0.5) + 0.5), 1.0);
    }
    if (debugView == 4U)
    {
        return float4(roughness.xxx, 1.0);
    }
    if (debugView == 5U)
    {
        return float4(deviceDepth.xxx, 1.0);
    }
    if (debugView == 6U)
    {
        return float4((motion * 0.5) + 0.5, 0.0, 1.0);
    }
    if (debugView == 7U)
    {
        return float4(DisplayEncode(IdentityColor(identity)), 1.0);
    }

    float3 viewDirection = normalize(-viewPosition);
    float3 lightDirection = normalize(frameData.lightDirectionIntensity.xyz);
    float nDotL = saturate(dot(viewNormal, lightDirection));
    float nDotV = saturate(dot(viewNormal, viewDirection));
    float3 halfVector = normalize(viewDirection + lightDirection);
    float nDotH = saturate(dot(viewNormal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metalness);
    float3 fresnel = SchlickFresnel(f0, vDotH);
    float distribution = GgxDistribution(nDotH, roughness);
    float geometry = SmithG1(nDotV, roughness) * SmithG1(nDotL, roughness);
    float denominator = max(4.0 * nDotV * nDotL, 1.0e-6);
    float3 specularBrdf = nDotL > 0.0 && nDotV > 0.0 ? fresnel * distribution * geometry / denominator : 0.0;
    float3 diffuseWeight =
        (1.0 - SchlickFresnel(f0, nDotV)) * (1.0 - SchlickFresnel(f0, nDotL)) * (1.0 - metalness);
    float3 diffuseBrdf = diffuseWeight * baseColor / Pi;
    linearColor = frameData.lightDirectionIntensity.w * nDotL * (diffuseBrdf + specularBrdf);
    return float4(saturate(DisplayEncode(linearColor)), 1.0);
}
