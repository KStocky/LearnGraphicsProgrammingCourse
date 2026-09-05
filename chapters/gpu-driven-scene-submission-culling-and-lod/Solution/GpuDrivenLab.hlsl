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

struct DrawTemplate
{
    uint indexCount;
    uint startIndex;
    int baseVertex;
    uint materialIndex;
};

StructuredBuffer<InstanceData> gInstances : register(t0);
StructuredBuffer<DrawTemplate> gDrawTemplates : register(t1);
RWByteAddressBuffer gCommands : register(u0);
RWStructuredBuffer<uint> gCount : register(u1);

cbuffer DrawConstants : register(b0)
{
    uint gStableId;
    uint gLod;
    uint gInstanceDataIndex;
};

cbuffer DispatchConstants : register(b0)
{
    uint gInstanceCount;
    uint gCapacity;
    uint gViewportHeight;
    float gProjectionScale;
    float gNearPlane;
    uint3 gReserved;
};

static const uint kThreadCount = 64;
static const uint kCommandStride = 32;
static const uint kGuardValue = 0xcdcdcdcdu;

bool SphereVisible(float4 sphere)
{
    const float4 planes[6] = {
        float4(1.0, 0.0, 0.75, 0.0),
        float4(-1.0, 0.0, 0.75, 0.0),
        float4(0.0, 1.0, 0.75, 0.0),
        float4(0.0, -1.0, 0.75, 0.0),
        float4(0.0, 0.0, 1.0, -0.5),
        float4(0.0, 0.0, -1.0, 30.0),
    };
    [unroll]
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex)
    {
        float scaledRadius = sphere.w * length(planes[planeIndex].xyz);
        if (dot(planes[planeIndex].xyz, sphere.xyz) + planes[planeIndex].w < -scaledRadius)
        {
            return false;
        }
    }
    return true;
}

uint SelectLod(float4 sphere, uint previousLod)
{
    float nearestDepth = max(sphere.z - sphere.w, gNearPlane);
    float radiusPixels = sphere.w * gProjectionScale * (0.5 * float(gViewportHeight)) / nearestDepth;
    uint selected = min(previousLod, 2u);
    const float thresholds[2] = {80.0, 32.0};
    while (selected > 0u && radiusPixels > thresholds[selected - 1u] + 4.0)
    {
        --selected;
    }
    while (selected < 2u && radiusPixels < thresholds[selected] - 4.0)
    {
        ++selected;
    }
    return selected;
}

[numthreads(kThreadCount, 1, 1)]
void ResetCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint commandDwordCount = (gCapacity + 1u) * (kCommandStride / 4u);
    for (uint dwordIndex = dispatchThreadId.x; dwordIndex < commandDwordCount; dwordIndex += kThreadCount)
    {
        gCommands.Store(dwordIndex * 4u, kGuardValue);
    }
    if (dispatchThreadId.x == 0u)
    {
        gCount[0] = 0u;
    }
}

[numthreads(kThreadCount, 1, 1)]
void CullAndBuildCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint physicalIndex = dispatchThreadId.x;
    if (physicalIndex >= gInstanceCount)
    {
        return;
    }
    InstanceData instance = gInstances[physicalIndex];
    if (!SphereVisible(instance.bounds))
    {
        return;
    }

    uint outputIndex;
    InterlockedAdd(gCount[0], 1u, outputIndex);
    if (outputIndex >= gCapacity)
    {
        return;
    }

    uint lod = min(SelectLod(instance.bounds, instance.previousLod), instance.lodCount - 1u);
    DrawTemplate draw = gDrawTemplates[instance.firstDrawTemplate + lod];
    uint offset = outputIndex * kCommandStride;
    gCommands.Store(offset + 0u, instance.stableId);
    gCommands.Store(offset + 4u, lod);
    gCommands.Store(offset + 8u, instance.instanceDataIndex);
    gCommands.Store(offset + 12u, draw.indexCount);
    gCommands.Store(offset + 16u, 1u);
    gCommands.Store(offset + 20u, draw.startIndex);
    gCommands.Store(offset + 24u, asuint(draw.baseVertex));
    gCommands.Store(offset + 28u, instance.instanceDataIndex);
}

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
