// Shared vocabulary for the Chapter 22 meshlet lab. The classic indexed pipeline and
// the amplification/mesh pipeline both emit SceneVertexOutput and resolve colour
// through MeshletColor, so every path rasterizes the same per-meshlet flat shading.

#ifndef LGP_CH22_MESHLET_LAB_HLSLI
#define LGP_CH22_MESHLET_LAB_HLSLI

struct SceneVertex
{
    float3 position;
    uint meshletId;
};

struct SceneVertexOutput
{
    float4 position : SV_Position;
    nointerpolation uint meshletId : MESHLETID;
};

// Deterministic integer hash so each meshlet identity maps to a stable, distinct
// colour without depending on transcendental functions.
float3 MeshletColor(uint meshletId)
{
    uint hash = meshletId * 2654435761u;
    float3 channels = float3(float((hash >> 16u) & 0xffu), float((hash >> 8u) & 0xffu), float(hash & 0xffu));
    return 0.2 + 0.75 * (channels / 255.0);
}

// The flattened, meshlet-ordered vertex stream consumed by the classic pipeline.
StructuredBuffer<SceneVertex> gVertices : register(t0);

SceneVertexOutput ClassicVS(uint vertexId : SV_VertexID)
{
    SceneVertex vertex = gVertices[vertexId];
    SceneVertexOutput output;
    output.position = float4(vertex.position, 1.0);
    output.meshletId = vertex.meshletId;
    return output;
}

float4 ScenePS(SceneVertexOutput input) : SV_Target0
{
    return float4(MeshletColor(input.meshletId), 1.0);
}

#endif // LGP_CH22_MESHLET_LAB_HLSLI
