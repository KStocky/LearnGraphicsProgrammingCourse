// Chapter 34 Starter GPU particle lab.
//
// The Starter is an honest deterministic baseline, not a pretend GPU pipeline. The CPU reference advances the
// particle system, compacts the live set, and the host uploads that compacted state for a direct instanced draw.
// There is no emission, simulation, compaction, or indirect-argument compute here: the render pass simply draws
// the particles the reference produced, so the picture on screen is real GPU output while every decision it
// depends on is made by the auditable CPU contract. The Solution replaces the upload with the equivalent on-GPU
// passes, and shares this exact draw so the two images agree.

struct GpuParticle
{
    uint identity;
    uint alive;
    float px;
    float py;
    float pz;
    float vx;
    float vy;
    float vz;
    float age;
    float lifetime;
};

StructuredBuffer<GpuParticle> gDrawStateA : register(t0);
StructuredBuffer<GpuParticle> gDrawStateB : register(t1);
StructuredBuffer<uint> gDrawSlots : register(t2);

cbuffer DrawConstants : register(b1)
{
    uint gDrawStateSlot;
    float gDrawViewScale;
    float gDrawQuadHalfExtent;
    uint gDrawPad;
};

uint Finalize32(uint value)
{
    value ^= value >> 16u;
    value *= 0x7FEB352Du;
    value ^= value >> 15u;
    value *= 0x846CA68Bu;
    value ^= value >> 16u;
    return value;
}

float2 QuadCorner(uint vertexId)
{
    switch (vertexId)
    {
    case 0u:
        return float2(-1.0, 1.0);
    case 1u:
        return float2(1.0, 1.0);
    case 2u:
        return float2(-1.0, -1.0);
    case 3u:
        return float2(1.0, 1.0);
    case 4u:
        return float2(1.0, -1.0);
    default:
        return float2(-1.0, -1.0);
    }
}

struct VsOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VsOutput ParticleVS(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    uint slot = gDrawSlots[instanceId];
    GpuParticle p;
    if (gDrawStateSlot == 0u)
    {
        p = gDrawStateA[slot];
    }
    else
    {
        p = gDrawStateB[slot];
    }

    float2 center = float2(p.px, p.py) * gDrawViewScale;
    float2 corner = QuadCorner(vertexId) * gDrawQuadHalfExtent;

    uint tint = Finalize32(p.identity);
    float r = 0.35 + (0.6 * (float((tint >> 0u) & 0xFFu) / 255.0));
    float g = 0.35 + (0.6 * (float((tint >> 8u) & 0xFFu) / 255.0));
    float b = 0.35 + (0.6 * (float((tint >> 16u) & 0xFFu) / 255.0));

    VsOutput output;
    output.position = float4(center + corner, 0.0, 1.0);
    output.color = float4(r, g, b, 1.0);
    return output;
}

float4 ParticlePS(VsOutput input) : SV_Target0
{
    return input.color;
}
