struct EntryRecord
{
    uint dispatchGrid;
    uint stableId;
    uint bucketSeed;
    int sourceValue;
};

struct ClassifiedRecord
{
    uint stableId;
    uint bucketIndex;
    int sourceValue;
    uint selected;
};

struct TransformedRecord
{
    uint stableId;
    uint bucketIndex;
    int sourceValue;
    int transformedValue;
    uint seed;
    uint recordChecksum;
    uint selected;
    uint reserved;
};

struct CanonicalRecord
{
    uint stableId;
    uint bucketIndex;
    int sourceValue;
    int transformedValue;
    uint seed;
    uint recordChecksum;
    uint valid;
    uint reserved;
};

struct BucketAggregate
{
    uint recordCount;
    int transformedSum;
    uint checksum;
    uint reserved;
};

cbuffer DispatchConstants : register(b0)
{
    uint InputCount;
    uint OutputCapacity;
    uint BucketCount;
    uint FixtureSeed;
    uint MaximumInputRecords;
    uint ThreadGroupSize;
    uint ExecutionPath;
    uint ReservedDispatch;
};

StructuredBuffer<EntryRecord> Inputs : register(t0);
StructuredBuffer<ClassifiedRecord> ClassifiedInputs : register(t1);
StructuredBuffer<TransformedRecord> TransformedInputs : register(t2);
StructuredBuffer<CanonicalRecord> DisplayRecords : register(t3);
RWStructuredBuffer<ClassifiedRecord> ClassifiedOutputs : register(u0);
RWStructuredBuffer<TransformedRecord> TransformedOutputs : register(u1);
RWStructuredBuffer<CanonicalRecord> CanonicalOutputs : register(u2);
RWStructuredBuffer<uint> Counters : register(u3);
RWStructuredBuffer<BucketAggregate> BucketAggregates : register(u4);
RWByteAddressBuffer TransformArguments : register(u5);
RWStructuredBuffer<uint> TransformCount : register(u6);
RWByteAddressBuffer FinalizeArguments : register(u7);
RWStructuredBuffer<uint> FinalizeCount : register(u8);

static const uint kClassifyStageBit = 1u;
static const uint kTransformStageBit = 2u;
static const uint kFinalizeStageBit = 4u;

uint HashWord(uint hash, uint value)
{
    hash = (hash ^ (value & 0xffu)) * 16777619u;
    hash = (hash ^ ((value >> 8u) & 0xffu)) * 16777619u;
    hash = (hash ^ ((value >> 16u) & 0xffu)) * 16777619u;
    return (hash ^ ((value >> 24u) & 0xffu)) * 16777619u;
}

uint RecordChecksum(uint stableId, uint bucketIndex, int sourceValue, int transformedValue, uint seed)
{
    uint hash = 2166136261u;
    hash = HashWord(hash, stableId);
    hash = HashWord(hash, bucketIndex);
    hash = HashWord(hash, asuint(sourceValue));
    hash = HashWord(hash, asuint(transformedValue));
    return HashWord(hash, seed);
}

bool IsActive(EntryRecord input)
{
    uint mixed = input.stableId * 1664525u + input.bucketSeed * 1013904223u + FixtureSeed;
    return (mixed & 3u) != 0u;
}

ClassifiedRecord Classify(EntryRecord input)
{
    ClassifiedRecord output;
    output.stableId = input.stableId;
    output.bucketIndex = (input.bucketSeed + input.stableId + FixtureSeed) % BucketCount;
    output.sourceValue = input.sourceValue;
    output.selected = IsActive(input) && input.stableId < OutputCapacity ? 1u : 0u;
    return output;
}

TransformedRecord Transform(ClassifiedRecord input)
{
    TransformedRecord output;
    output.stableId = input.stableId;
    output.bucketIndex = input.bucketIndex;
    output.sourceValue = input.sourceValue;
    output.transformedValue =
        input.sourceValue * 3 + int(input.bucketIndex * 17u) - int((FixtureSeed >> 3u) & 31u);
    output.seed = FixtureSeed;
    output.recordChecksum = RecordChecksum(output.stableId, output.bucketIndex, output.sourceValue,
                                           output.transformedValue, output.seed);
    output.selected = input.selected;
    output.reserved = 0u;
    return output;
}

void RecordClassifyEvidence(EntryRecord input, ClassifiedRecord classified)
{
    if (IsActive(input))
    {
        InterlockedAdd(Counters[1], 1u);
        if (classified.selected == 0u)
        {
            InterlockedAdd(Counters[3], 1u);
        }
    }
}

void RecordTransformEvidence(TransformedRecord transformed)
{
    InterlockedAdd(BucketAggregates[transformed.bucketIndex].recordCount, 1u);
    InterlockedAdd(BucketAggregates[transformed.bucketIndex].transformedSum, transformed.transformedValue);
    InterlockedXor(BucketAggregates[transformed.bucketIndex].checksum, transformed.recordChecksum);
    InterlockedAdd(Counters[7], 1u);
}

void StoreCanonical(TransformedRecord transformed)
{
    CanonicalRecord output;
    output.stableId = transformed.stableId;
    output.bucketIndex = transformed.bucketIndex;
    output.sourceValue = transformed.sourceValue;
    output.transformedValue = transformed.transformedValue;
    output.seed = transformed.seed;
    output.recordChecksum = transformed.recordChecksum;
    output.valid = 1u;
    output.reserved = 0u;
    CanonicalOutputs[transformed.stableId] = output;
    InterlockedAdd(Counters[2], 1u);
    InterlockedXor(Counters[4], transformed.recordChecksum);
}

#ifndef LGP_ENABLE_WORK_GRAPH_NODES
[numthreads(32, 1, 1)]
void ResetCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index < MaximumInputRecords)
    {
        ClassifiedRecord classified = (ClassifiedRecord)0;
        TransformedRecord transformed = (TransformedRecord)0;
        CanonicalRecord canonical = (CanonicalRecord)0;
        ClassifiedOutputs[index] = classified;
        TransformedOutputs[index] = transformed;
        CanonicalOutputs[index] = canonical;
    }
    if (index < BucketCount)
    {
        BucketAggregate aggregate = (BucketAggregate)0;
        BucketAggregates[index] = aggregate;
    }
    if (index < 8u)
    {
        Counters[index] = 0u;
    }
    if (index == 0u)
    {
        Counters[0] = InputCount;
        Counters[5] = InputCount == 0u ? 1u : 0u;
        TransformArguments.Store3(0u, uint3(0u, 1u, 1u));
        FinalizeArguments.Store3(0u, uint3(0u, 1u, 1u));
        TransformCount[0] = 0u;
        FinalizeCount[0] = 0u;
    }
}

[numthreads(32, 1, 1)]
void ClassifyCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= InputCount)
    {
        return;
    }

    EntryRecord input = Inputs[index];
    ClassifiedRecord classified = Classify(input);
    ClassifiedOutputs[input.stableId] = classified;
    RecordClassifyEvidence(input, classified);
    InterlockedOr(Counters[6], kClassifyStageBit);

    if (index == 0u)
    {
        uint groupCount = (InputCount + ThreadGroupSize - 1u) / ThreadGroupSize;
        TransformArguments.Store3(0u, uint3(groupCount, 1u, 1u));
        TransformCount[0] = groupCount == 0u ? 0u : 1u;
    }
}

[numthreads(32, 1, 1)]
void TransformCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= InputCount)
    {
        return;
    }

    ClassifiedRecord classified = ClassifiedInputs[index];
    if (classified.selected != 0u)
    {
        TransformedRecord transformed = Transform(classified);
        TransformedOutputs[classified.stableId] = transformed;
        RecordTransformEvidence(transformed);
    }
    InterlockedOr(Counters[6], kTransformStageBit);

    if (index == 0u)
    {
        uint groupCount = (InputCount + ThreadGroupSize - 1u) / ThreadGroupSize;
        FinalizeArguments.Store3(0u, uint3(groupCount, 1u, 1u));
        FinalizeCount[0] = groupCount == 0u ? 0u : 1u;
    }
}

[numthreads(32, 1, 1)]
void FinalizeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= InputCount)
    {
        return;
    }

    TransformedRecord transformed = TransformedInputs[index];
    if (transformed.selected != 0u)
    {
        StoreCanonical(transformed);
    }
    InterlockedOr(Counters[6], kFinalizeStageBit);
}

struct FullscreenVertex
{
    float4 position : SV_Position;
};

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenVertex output;
    output.position = float4(vertexId == 2u ? 3.0 : -1.0, vertexId == 1u ? 3.0 : -1.0, 0.0, 1.0);
    return output;
}

cbuffer DisplayConstants : register(b1)
{
    uint DisplayWidth;
    uint DisplayHeight;
    uint DisplayInputCount;
    uint DisplayView;
    uint DisplayPath;
    uint DisplayOutcome;
    uint DisplayActiveCount;
    uint DisplayFinalCount;
    uint DisplayOverflowCount;
    uint DisplayChecksum;
    uint DisplayTier;
    uint DisplaySupportStatus;
    uint DisplayInitializeCount;
    uint DisplayReuseCount;
    uint DisplayBackingMinimum;
    uint DisplayBackingMaximum;
};

float4 DisplayPS(float4 position : SV_Position) : SV_Target0
{
    uint2 pixel = min(uint2(position.xy), uint2(DisplayWidth - 1u, DisplayHeight - 1u));
    if (DisplayOutcome != 0u)
    {
        float stripe = ((pixel.x / 12u) & 1u) == 0u ? 0.2 : 0.65;
        return float4(0.75 + stripe * 0.2, 0.05, 0.08, 1.0);
    }

    if (DisplayView == 0u)
    {
        if (DisplayInputCount == 0u)
        {
            return float4(0.02, 0.06, 0.1, 1.0);
        }
        uint recordIndex = min((pixel.x * DisplayInputCount) / DisplayWidth, DisplayInputCount - 1u);
        CanonicalRecord record = DisplayRecords[recordIndex];
        if (record.valid == 0u)
        {
            return float4(0.025, 0.025, 0.035, 1.0);
        }
        float3 bucketColors[4] = {
            float3(0.18, 0.65, 0.95),
            float3(0.95, 0.45, 0.12),
            float3(0.32, 0.82, 0.32),
            float3(0.75, 0.32, 0.9),
        };
        float intensity = 0.45 + 0.5 * float(record.recordChecksum & 255u) / 255.0;
        return float4(bucketColors[record.bucketIndex] * intensity, 1.0);
    }
    if (DisplayView == 1u)
    {
        float3 pathColors[3] = {
            float3(0.15, 0.55, 0.95),
            float3(0.95, 0.58, 0.12),
            float3(0.65, 0.25, 0.9),
        };
        float segment = (float(pixel.x) + 0.5) / float(DisplayWidth);
        return float4(segment < (1.0 / 3.0) ? pathColors[DisplayPath] : pathColors[DisplayPath] * 0.35, 1.0);
    }
    if (DisplayView == 2u)
    {
        float finalRatio = float(DisplayFinalCount) / max(float(DisplayInputCount), 1.0);
        float activeRatio = float(DisplayActiveCount) / max(float(DisplayInputCount), 1.0);
        float x = (float(pixel.x) + 0.5) / float(DisplayWidth);
        float3 color = x < finalRatio ? float3(0.12, 0.82, 0.34)
                                     : (x < activeRatio ? float3(0.95, 0.4, 0.08) : float3(0.08, 0.08, 0.12));
        return float4(color, 1.0);
    }

    float x = (float(pixel.x) + 0.5) / float(DisplayWidth);
    float initialized = DisplayInitializeCount != 0u ? 1.0 : 0.0;
    float reused = DisplayReuseCount != 0u ? 1.0 : 0.0;
    float3 capability = DisplayTier >= 1u ? float3(0.15, 0.72, 0.35) : float3(0.72, 0.16, 0.1);
    if (x > 0.66)
    {
        capability = float3(0.2 + 0.5 * initialized, 0.15 + 0.65 * reused, 0.75);
    }
    return float4(capability, 1.0);
}
#endif

#ifdef LGP_ENABLE_WORK_GRAPH_NODES
struct WorkGraphEntry
{
    uint dispatchGrid : SV_DispatchGrid;
    uint stableId;
    uint bucketSeed;
    int sourceValue;
};

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
void ClassifyNode(DispatchNodeInputRecord<WorkGraphEntry> inputRecord,
                  [MaxRecords(1)] NodeOutput<ClassifiedRecord> TransformNode)
{
    EntryRecord input;
    input.dispatchGrid = inputRecord.Get().dispatchGrid;
    input.stableId = inputRecord.Get().stableId;
    input.bucketSeed = inputRecord.Get().bucketSeed;
    input.sourceValue = inputRecord.Get().sourceValue;

    ClassifiedRecord classified = Classify(input);
    ClassifiedOutputs[input.stableId] = classified;
    RecordClassifyEvidence(input, classified);
    InterlockedOr(Counters[6], kClassifyStageBit);

    uint outputCount = classified.selected;
    ThreadNodeOutputRecords<ClassifiedRecord> output = TransformNode.GetThreadNodeOutputRecords(outputCount);
    if (outputCount != 0u)
    {
        output.Get() = classified;
    }
    output.OutputComplete();
}

[Shader("node")]
[NodeLaunch("coalescing")]
[NumThreads(32, 1, 1)]
void TransformNode([MaxRecords(32)] GroupNodeInputRecords<ClassifiedRecord> inputRecords,
                   [MaxRecords(32)] NodeOutput<TransformedRecord> FinalizeNode,
                   uint threadIndex : SV_GroupIndex)
{
    uint recordCount = inputRecords.Count();
    GroupNodeOutputRecords<TransformedRecord> outputs = FinalizeNode.GetGroupNodeOutputRecords(recordCount);
    if (threadIndex < recordCount)
    {
        TransformedRecord transformed = Transform(inputRecords[threadIndex]);
        TransformedOutputs[transformed.stableId] = transformed;
        RecordTransformEvidence(transformed);
        outputs[threadIndex] = transformed;
    }
    InterlockedOr(Counters[6], kTransformStageBit);
    outputs.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void FinalizeNode(ThreadNodeInputRecord<TransformedRecord> inputRecord)
{
    StoreCanonical(inputRecord.Get());
    InterlockedOr(Counters[6], kFinalizeStageBit);
}
#endif
