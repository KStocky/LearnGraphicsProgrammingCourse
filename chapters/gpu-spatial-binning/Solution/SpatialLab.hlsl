struct Particle
{
    float3 position;
    uint identity;
    uint alive;
};

struct Record
{
    uint key;
    uint identity;
    uint sourceIndex;
    float3 position;
};

struct CellRange
{
    uint start;
    uint end;
};

struct QueryRow
{
    uint identity;
    uint start;
    uint end;
    uint candidateCount;
    uint acceptedCount;
    uint overflowCount;
};

struct Statistics
{
    uint liveCount;
    uint outsideCount;
    uint emittedCount;
    uint recordOverflowCount;
    uint candidateCount;
    uint acceptedCount;
    uint queryOverflowCount;
    uint outputOverflowCount;
    uint outputCount;
};

cbuffer LabConstants : register(b0)
{
    uint inputCount;
    uint recordCapacity;
    uint3 dimensions;
    uint cellCount;
    uint queryCount;
    uint queryCapacity;
    uint outputCapacity;
    float3 origin;
    float cellSize;
    float radius;
};

StructuredBuffer<Particle> inputs : register(t0);
StructuredBuffer<uint> queryIds : register(t1);
RWStructuredBuffer<Record> keyed : register(u0);
RWStructuredBuffer<Record> sorted : register(u1);
RWStructuredBuffer<CellRange> ranges : register(u2);
RWStructuredBuffer<QueryRow> rows : register(u3);
RWStructuredBuffer<uint> neighbors : register(u4);
RWStructuredBuffer<Statistics> statistics : register(u5);

static const uint INVALID = 0xffffffffu;

Record EmptyRecord()
{
    Record record;
    record.key = INVALID;
    record.identity = INVALID;
    record.sourceIndex = INVALID;
    record.position = 0.0.xxx;
    return record;
}

uint GridKey(float3 position)
{
    float3 upper = origin + float3(dimensions) * cellSize;
    if (any(position < origin) || any(position >= upper))
        return INVALID;
    uint3 cell = min(uint3(floor((position - origin) / cellSize)), dimensions - 1u);
    return cell.x + dimensions.x * (cell.y + dimensions.y * cell.z);
}

// One lane per output rank: a stable source-order gather, with no append-counter races.
[numthreads(256, 1, 1)]
void KeyCS(uint lane : SV_GroupIndex)
{
    Record result = EmptyRecord();
    if (lane < recordCapacity)
    {
        uint rank = 0u;
        for (uint index = 0u; index < inputCount; ++index)
        {
            Particle particle = inputs[index];
            if (particle.alive != 0u)
            {
                uint key = GridKey(particle.position);
                if (key != INVALID)
                {
                    if (rank == lane)
                    {
                        result.key = key;
                        result.identity = particle.identity;
                        result.sourceIndex = index;
                        result.position = particle.position;
                    }
                    ++rank;
                }
            }
        }
    }
    keyed[lane] = result;
}

groupshared Record sharedRecords[256];

bool Greater(Record a, Record b)
{
    return a.key > b.key || (a.key == b.key && a.identity > b.identity);
}

// All 256 lanes participate, including (INVALID,INVALID) padding.
[numthreads(256, 1, 1)]
void SortCS(uint lane : SV_GroupIndex)
{
    sharedRecords[lane] = keyed[lane];
    GroupMemoryBarrierWithGroupSync();
    for (uint width = 2u; width <= 256u; width <<= 1u)
    {
        for (uint stride = width >> 1u; stride != 0u; stride >>= 1u)
        {
            uint peer = lane ^ stride;
            if (peer > lane)
            {
                Record a = sharedRecords[lane];
                Record b = sharedRecords[peer];
                bool ascending = (lane & width) == 0u;
                if (Greater(a, b) == ascending)
                {
                    sharedRecords[lane] = b;
                    sharedRecords[peer] = a;
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    sorted[lane] = sharedRecords[lane];
}

[numthreads(256, 1, 1)]
void RangesCS(uint cell : SV_DispatchThreadID)
{
    if (cell >= cellCount)
        return;
    uint count = 0u;
    while (count < recordCapacity && sorted[count].key != INVALID)
        ++count;
    CellRange range;
    range.start = count;
    range.end = count;
    for (uint index = 0u; index < count; ++index)
    {
        if (sorted[index].key == cell)
        {
            if (range.start == count)
                range.start = index;
            range.end = index + 1u;
        }
    }
    ranges[cell] = range;
}

groupshared uint acceptedByQuery[256];
groupshared uint candidatesByQuery[256];

void Visit(Record query, bool writeOutputs, uint first, uint permitted,
           out uint candidates, out uint accepted)
{
    candidates = 0u;
    accepted = 0u;
    uint3 cell = uint3(query.key % dimensions.x,
                       (query.key / dimensions.x) % dimensions.y,
                       query.key / (dimensions.x * dimensions.y));
    for (int z = -1; z <= 1; ++z)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                int3 next = int3(cell) + int3(x, y, z);
                if (any(next < 0) || any(next >= int3(dimensions)))
                    continue;
                uint key = uint(next.x) + dimensions.x * (uint(next.y) + dimensions.y * uint(next.z));
                CellRange range = ranges[key];
                for (uint index = range.start; index < range.end; ++index)
                {
                    Record other = sorted[index];
                    ++candidates;
                    float3 difference = other.position - query.position;
                    if (other.identity != query.identity && dot(difference, difference) <= radius * radius)
                    {
                        if (writeOutputs && accepted < permitted)
                            neighbors[first + accepted] = other.identity;
                        ++accepted;
                    }
                }
            }
        }
    }
}

[numthreads(256, 1, 1)]
void QueryCS(uint lane : SV_GroupIndex)
{
    uint candidates = 0u;
    uint accepted = 0u;
    Record query = EmptyRecord();
    if (lane < queryCount)
    {
        uint identity = queryIds[lane];
        for (uint index = 0u; index < recordCapacity; ++index)
        {
            Record current = sorted[index];
            if (current.key != INVALID && current.identity == identity)
                query = current;
        }
        if (query.key != INVALID)
            Visit(query, false, 0u, 0u, candidates, accepted);
    }
    acceptedByQuery[lane] = accepted;
    candidatesByQuery[lane] = candidates;
    GroupMemoryBarrierWithGroupSync();

    if (lane < queryCount && lane < queryCapacity)
    {
        uint prefix = 0u;
        for (uint index = 0u; index < lane; ++index)
            prefix += acceptedByQuery[index];
        QueryRow row;
        row.identity = queryIds[lane];
        row.start = min(prefix, outputCapacity);
        row.end = min(prefix + accepted, outputCapacity);
        row.candidateCount = candidates;
        row.acceptedCount = accepted;
        row.overflowCount = accepted - (row.end - row.start);
        rows[lane] = row;
        if (query.key != INVALID && row.end > row.start)
        {
            uint ignoredCandidates;
            uint ignoredAccepted;
            Visit(query, true, row.start, row.end - row.start, ignoredCandidates, ignoredAccepted);
        }
    }
    if (lane == 0u)
    {
        Statistics result = (Statistics)0;
        for (uint index = 0u; index < inputCount; ++index)
        {
            if (inputs[index].alive != 0u)
            {
                ++result.liveCount;
                if (GridKey(inputs[index].position) == INVALID)
                    ++result.outsideCount;
            }
        }
        for (uint index = 0u; index < recordCapacity; ++index)
        {
            if (sorted[index].key != INVALID)
                ++result.emittedCount;
        }
        result.recordOverflowCount = result.liveCount - result.outsideCount - result.emittedCount;
        result.queryOverflowCount = queryCount - min(queryCount, queryCapacity);
        for (uint index = 0u; index < queryCount; ++index)
        {
            result.candidateCount += candidatesByQuery[index];
            result.acceptedCount += acceptedByQuery[index];
        }
        uint storedAccepted = 0u;
        for (uint index = 0u; index < min(queryCount, queryCapacity); ++index)
            storedAccepted += acceptedByQuery[index];
        result.outputCount = min(storedAccepted, outputCapacity);
        result.outputOverflowCount = storedAccepted - result.outputCount;
        statistics[0] = result;
        neighbors[outputCapacity] = INVALID;
    }
}

cbuffer DrawConstants : register(b1)
{
    float viewScale;
    float pointHalfExtent;
};
StructuredBuffer<Record> drawRecords : register(t0);

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

VertexOutput PointVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
        float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0)
    };
    Record record = drawRecords[instance];
    VertexOutput output;
    if (record.key == INVALID)
    {
        output.position = float4(2.0, 2.0, 0.0, 1.0);
        output.color = 0.0.xxx;
    }
    else
    {
        output.position = float4(record.position.xy * viewScale + corners[vertex] * pointHalfExtent, 0.0, 1.0);
        output.color = float3(0.3, 0.5, 0.35) + float3(float(record.key % 5u) * 0.12, 0.0, 0.2);
    }
    return output;
}

float4 PointPS(VertexOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
