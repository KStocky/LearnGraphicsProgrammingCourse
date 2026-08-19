#include "../Common/VisibilityBufferShared.hlsli"

struct RasterDrawConstants
{
    uint4 identifiers;
    float4 baseTintAndRoughness;
    float4 materialParameters;
};

struct VertexData
{
    float3 position;
    float3 normal;
    float4 tangent;
    float2 textureCoordinates;
};

ConstantBuffer<RasterDrawConstants> gDraw : register(b1);
Texture2D<uint2> gVisibility : register(t0);
Texture2D<float> gDepth : register(t1);
StructuredBuffer<VertexData> gVertices : register(t2);
StructuredBuffer<uint> gIndices : register(t3);
StructuredBuffer<DrawData> gDraws : register(t4);
Texture2D<float4> gMaterialTexture : register(t5);
RWTexture2D<float4> gOutput : register(u0);
RWStructuredBuffer<PixelDiagnostics> gDiagnostics : register(u1);
SamplerState gMaterialSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
};

struct RasterVertexOutput
{
    float4 position : SV_Position;
};

RasterVertexOutput RasterVS(VertexInput input)
{
    RasterVertexOutput output;
    output.position = mul(float4(input.position, 1.0), gFrame.projection);
    return output;
}

uint2 VisibilityPS(uint primitiveIdentifier : SV_PrimitiveID,
                   noperspective float3 screenBarycentrics : SV_Barycentrics) : SV_Target0
{
    static const uint MaximumBarycentric = 16383u;
    float3 canonical = max(screenBarycentrics, 0.0);
    canonical /= canonical.x + canonical.y + canonical.z;
    float scaledFirst = canonical.x * float(MaximumBarycentric);
    float scaledSecond = canonical.y * float(MaximumBarycentric);
    uint first = uint(floor(scaledFirst + 0.5));
    uint second = uint(floor(scaledSecond + 0.5));
    if (first + second > MaximumBarycentric)
    {
        float firstError = float(first) - scaledFirst;
        float secondError = float(second) - scaledSecond;
        if (first > 0u && (second == 0u || firstError >= secondError))
        {
            --first;
        }
        else
        {
            --second;
        }
    }

    uint word0 = gDraw.identifiers.x | ((primitiveIdentifier & 0xffffu) << 16u);
    uint word1 = (primitiveIdentifier >> 16u) | (first << 4u) | (second << 18u);
    return uint2(word0, word1);
}

bool CheckedAdd(uint left, uint right, out uint result)
{
    result = left + right;
    return result >= left;
}

bool DecodeVisibility(uint2 record, out uint drawIdentifier, out uint primitiveIdentifier,
                      out float3 screenBarycentrics)
{
    static const uint MaximumBarycentric = 16383u;
    drawIdentifier = record.x & 0xffffu;
    uint primitiveLow = record.x >> 16u;
    uint primitiveHigh = record.y & 0xfu;
    primitiveIdentifier = primitiveLow | (primitiveHigh << 16u);
    uint first = (record.y >> 4u) & MaximumBarycentric;
    uint second = (record.y >> 18u) & MaximumBarycentric;
    if (drawIdentifier == 0u || first + second > MaximumBarycentric)
    {
        screenBarycentrics = 0.0;
        return false;
    }
    screenBarycentrics =
        float3(float(first), float(second), float(MaximumBarycentric - first - second)) /
        float(MaximumBarycentric);
    return true;
}

float Cross2(float2 first, float2 second)
{
    return first.x * second.y - first.y * second.x;
}

float2 ProjectToPixel(float3 viewPosition)
{
    float4 clip = mul(float4(viewPosition, 1.0), gFrame.projection);
    float2 ndc = clip.xy / clip.w;
    return float2((ndc.x * 0.5 + 0.5) * float(gFrame.dimensions.x),
                  (0.5 - ndc.y * 0.5) * float(gFrame.dimensions.y));
}

bool ResolveTriangle(uint drawIdentifier, uint primitiveIdentifier, out DrawData draw,
                     out VertexData vertices[3])
{
    uint drawBufferCount;
    uint drawStride;
    uint vertexBufferCount;
    uint vertexStride;
    uint indexBufferCount;
    uint indexStride;
    gDraws.GetDimensions(drawBufferCount, drawStride);
    gVertices.GetDimensions(vertexBufferCount, vertexStride);
    gIndices.GetDimensions(indexBufferCount, indexStride);
    if (drawIdentifier == 0u || drawIdentifier > gFrame.counts.x || drawIdentifier > drawBufferCount ||
        drawIdentifier > 4u)
    {
        return false;
    }

    draw = gDraws[drawIdentifier - 1u];
    if (draw.vertexCount == 0u || draw.indexCount == 0u || draw.indexCount % 3u != 0u ||
        draw.vertexCount > 64u || draw.indexCount > 192u)
    {
        return false;
    }
    uint vertexEnd;
    uint indexEnd;
    if (!CheckedAdd(draw.vertexOffset, draw.vertexCount, vertexEnd) ||
        !CheckedAdd(draw.indexOffset, draw.indexCount, indexEnd) || vertexEnd > gFrame.counts.y ||
        vertexEnd > vertexBufferCount || indexEnd > gFrame.counts.z || indexEnd > indexBufferCount ||
        primitiveIdentifier >= draw.indexCount / 3u || primitiveIdentifier >= 64u)
    {
        return false;
    }

    uint primitiveOffset = primitiveIdentifier * 3u;
    uint triangleOffset;
    if (!CheckedAdd(draw.indexOffset, primitiveOffset, triangleOffset))
    {
        return false;
    }
    [unroll]
    for (uint corner = 0u; corner < 3u; ++corner)
    {
        uint indexOffset;
        if (!CheckedAdd(triangleOffset, corner, indexOffset))
        {
            return false;
        }
        uint localVertex = gIndices[indexOffset];
        uint absoluteVertex;
        if (localVertex >= draw.vertexCount || !CheckedAdd(draw.vertexOffset, localVertex, absoluteVertex) ||
            absoluteVertex >= vertexEnd)
        {
            return false;
        }
        vertices[corner] = gVertices[absoluteVertex];
        if (vertices[corner].position.z <= 0.0)
        {
            return false;
        }
    }
    return true;
}

bool PerspectiveWeights(float3 screenBarycentrics, VertexData vertices[3], out float3 weights)
{
    float3 reciprocalW =
        float3(1.0 / vertices[0].position.z, 1.0 / vertices[1].position.z, 1.0 / vertices[2].position.z);
    float3 weighted = screenBarycentrics * reciprocalW;
    float denominator = weighted.x + weighted.y + weighted.z;
    if (denominator <= 0.0)
    {
        weights = 0.0;
        return false;
    }
    weights = weighted / denominator;
    return true;
}

bool ComputeAnalyticGradients(uint2 pixel, VertexData vertices[3], out float2 uv, out float2 textureDdx,
                              out float2 textureDdy)
{
    float2 projected[3] = {
        ProjectToPixel(vertices[0].position),
        ProjectToPixel(vertices[1].position),
        ProjectToPixel(vertices[2].position),
    };
    float2 edgeOne = projected[1] - projected[0];
    float2 edgeTwo = projected[2] - projected[0];
    float denominator = Cross2(edgeOne, edgeTwo);
    if (abs(denominator) <= 1.0e-8)
    {
        uv = 0.0;
        textureDdx = 0.0;
        textureDdy = 0.0;
        return false;
    }

    float2 samplePosition = float2(pixel) + 0.5;
    float2 fromFirst = samplePosition - projected[0];
    float second = Cross2(fromFirst, edgeTwo) / denominator;
    float third = Cross2(edgeOne, fromFirst) / denominator;
    float3 screen = float3(1.0 - second - third, second, third);
    float3 barycentricDdx =
        float3((edgeOne.y - edgeTwo.y) / denominator, edgeTwo.y / denominator, -edgeOne.y / denominator);
    float3 barycentricDdy =
        float3((edgeTwo.x - edgeOne.x) / denominator, -edgeTwo.x / denominator, edgeOne.x / denominator);
    float3 reciprocalW =
        float3(1.0 / vertices[0].position.z, 1.0 / vertices[1].position.z, 1.0 / vertices[2].position.z);
    float3 weighted = screen * reciprocalW;
    float3 weightedDdx = barycentricDdx * reciprocalW;
    float3 weightedDdy = barycentricDdy * reciprocalW;
    float perspectiveDenominator = weighted.x + weighted.y + weighted.z;
    float denominatorDdx = weightedDdx.x + weightedDdx.y + weightedDdx.z;
    float denominatorDdy = weightedDdy.x + weightedDdy.y + weightedDdy.z;
    if (perspectiveDenominator <= 0.0)
    {
        uv = 0.0;
        textureDdx = 0.0;
        textureDdy = 0.0;
        return false;
    }

    float2 numerator = weighted.x * vertices[0].textureCoordinates +
                       weighted.y * vertices[1].textureCoordinates +
                       weighted.z * vertices[2].textureCoordinates;
    float2 numeratorDdx = weightedDdx.x * vertices[0].textureCoordinates +
                          weightedDdx.y * vertices[1].textureCoordinates +
                          weightedDdx.z * vertices[2].textureCoordinates;
    float2 numeratorDdy = weightedDdy.x * vertices[0].textureCoordinates +
                          weightedDdy.y * vertices[1].textureCoordinates +
                          weightedDdy.z * vertices[2].textureCoordinates;
    float denominatorSquared = perspectiveDenominator * perspectiveDenominator;
    uv = numerator / perspectiveDenominator;
    textureDdx =
        (numeratorDdx * perspectiveDenominator - numerator * denominatorDdx) / denominatorSquared;
    textureDdy =
        (numeratorDdy * perspectiveDenominator - numerator * denominatorDdy) / denominatorSquared;
    return true;
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
    uint2 record = gVisibility.Load(int3(pixel, 0));
    if (all(record == 0u))
    {
        StoreBackground(pixel, linearIndex);
        return;
    }

    uint drawIdentifier;
    uint primitiveIdentifier;
    float3 screenBarycentrics;
    if (!DecodeVisibility(record, drawIdentifier, primitiveIdentifier, screenBarycentrics))
    {
        StoreInvalid(pixel, linearIndex, drawIdentifier, primitiveIdentifier);
        return;
    }

    DrawData draw;
    VertexData vertices[3];
    if (!ResolveTriangle(drawIdentifier, primitiveIdentifier, draw, vertices))
    {
        StoreInvalid(pixel, linearIndex, drawIdentifier, primitiveIdentifier);
        return;
    }
    float backgroundDepth = gFrame.slicing.w == 0.0 ? 1.0 : 0.0;
    if (gDepth.Load(int3(pixel, 0)) == backgroundDepth)
    {
        StoreInvalid(pixel, linearIndex, drawIdentifier, primitiveIdentifier);
        return;
    }

    float3 weights;
    if (!PerspectiveWeights(screenBarycentrics, vertices, weights))
    {
        StoreInvalid(pixel, linearIndex, drawIdentifier, primitiveIdentifier);
        return;
    }
    float3 viewPosition =
        weights.x * vertices[0].position + weights.y * vertices[1].position + weights.z * vertices[2].position;
    float3 normal =
        normalize(weights.x * vertices[0].normal + weights.y * vertices[1].normal + weights.z * vertices[2].normal);
    float3 interpolatedTangent = weights.x * vertices[0].tangent.xyz + weights.y * vertices[1].tangent.xyz +
                                 weights.z * vertices[2].tangent.xyz;
    float3 tangentDirection = normalize(interpolatedTangent - normal * dot(normal, interpolatedTangent));
    float handedness = vertices[0].tangent.w;
    float3 bitangent = cross(normal, tangentDirection) * handedness;
    if (dot(bitangent, bitangent) <= 0.0)
    {
        StoreInvalid(pixel, linearIndex, drawIdentifier, primitiveIdentifier);
        return;
    }

    float2 uv;
    float2 textureDdx;
    float2 textureDdy;
    if (!ComputeAnalyticGradients(pixel, vertices, uv, textureDdx, textureDdy))
    {
        StoreInvalid(pixel, linearIndex, drawIdentifier, primitiveIdentifier);
        return;
    }
    float3 baseColor =
        gMaterialTexture.SampleGrad(gMaterialSampler, uv, textureDdx, textureDdy).rgb *
        draw.baseTintAndRoughness.rgb;
    uint clusterIndex;
    if (!ComputeClusterIndex(pixel, viewPosition.z, clusterIndex))
    {
        StoreInvalid(pixel, linearIndex, drawIdentifier, primitiveIdentifier);
        return;
    }
    float3 linearColor;
    if (!ShadeCluster(clusterIndex, viewPosition, normal, baseColor, draw.baseTintAndRoughness.w,
                      draw.materialParameters.x, linearColor))
    {
        StoreInvalid(pixel, linearIndex, drawIdentifier, primitiveIdentifier);
        return;
    }

    gOutput[pixel] =
        DebugOutput(linearColor, uv, textureDdx, textureDdy, drawIdentifier, primitiveIdentifier);
    PixelDiagnostics diagnostics = (PixelDiagnostics)0;
    diagnostics.viewPosition = viewPosition;
    diagnostics.status = 1u;
    diagnostics.normal = normal;
    diagnostics.drawIdentifier = drawIdentifier;
    diagnostics.tangent = float4(tangentDirection, handedness);
    diagnostics.textureCoordinates = uv;
    diagnostics.textureDdx = textureDdx;
    diagnostics.textureDdy = textureDdy;
    diagnostics.primitiveIdentifier = primitiveIdentifier;
    diagnostics.clusterIndex = clusterIndex;
    gDiagnostics[linearIndex] = diagnostics;
}
