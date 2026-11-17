#include "data_graph.h"
#include "../../i_common/graph_time.h"
#include "../../i_common/types.h"
#include "vma.hpp"

#pragma warning(disable : 4800)

extern uint32_t GraphRead;

// Linear interpolation
float Lerp(float val1, float val2, float lerp_k)
{
    return (val1 + (val2 - val1) * lerp_k);
}

// Take a random number from a range
float RandomRange(float Min, float Max)
{
    float Temp;
    if (Min > Max)
    {
        Temp = Max;
        Max = Min;
        Min = Temp;
    }
    auto Rand = static_cast<float>(rand() % 1000) / 1000.0f;
    Rand *= (Max - Min);
    return Rand + Min;
}

// constructor / destructor
DataGraph::DataGraph()
{
    bRelative = false;
    bNegative = false;

    ResetCachedTime();
}

DataGraph::~DataGraph()
{
}

// Set the "default"
void DataGraph::SetDefaultValue(float MaxValue, float MinValue)
{
    //~!~
    MinGraph.clear();
    MaxGraph.clear();

    GraphVertex Min;
    Min.Val = MinValue;
    Min.Time = MIN_GRAPH_TIME;
    MinGraph.push_back(Min);
    MinGraph.push_back(Min);

    GraphVertex Max;
    Max.Val = MaxValue;
    MaxGraph.push_back(Max);
    Max.Time = MAX_GRAPH_TIME;
    MaxGraph.push_back(Max);

    ResetCachedTime();
}

void DataGraph::ResetCachedTime()
{
    MaxCachedTime = NOT_INITED_CACHE_VALUE;
    MinCachedTime = NOT_INITED_CACHE_VALUE;
}

void DataGraph::Load(MemFile *File)
{
    MinGraph.clear();
    MaxGraph.clear();

    uint32_t dwNegative = 0;
    File->ReadType(dwNegative);
    SetNegative(dwNegative);

    uint32_t dwRelative = 0;
    File->ReadType(dwRelative);
    SetRelative(dwRelative);

    uint32_t MaxGraphItemsCount = 0;
    File->ReadType(MaxGraphItemsCount);

    uint32_t i = 0;
    for (i = 0; i < MaxGraphItemsCount; i++)
    {
        auto fTime = 0.0f;
        File->ReadType(fTime);

        auto fValue = 0.0f;
        File->ReadType(fValue);

        if (!bNegative && fValue < 0.0f)
            fValue = 0.0f;

        GraphVertex MaxVertex;
        MaxVertex.Time = fTime;
        MaxVertex.Val = fValue;
        MaxGraph.push_back(MaxVertex);

    }

    uint32_t MinGraphItemsCount = 0;
    File->ReadType(MinGraphItemsCount);

    for (i = 0; i < MinGraphItemsCount; i++)
    {
        auto fTime = 0.0f;
        File->ReadType(fTime);

        auto fValue = 0.0f;
        File->ReadType(fValue);

        if (!bNegative && fValue < 0.0f)
            fValue = 0.0f;

        GraphVertex MinVertex;
        MinVertex.Time = fTime;
        MinVertex.Val = fValue;
        MinGraph.push_back(MinVertex);

    }

    static char AttribueName[128];
    uint32_t NameLength = 0;
    File->ReadType(NameLength);
    Assert(NameLength < 128);
    File->Read(AttribueName, NameLength);


    SetName(AttribueName);

    // HACK! For backward compatibility
    // convert after loading the graphs into the desired format
}

// Set / get can be negative values in the graph or not
void DataGraph::SetNegative(bool _bNegative)
{
    bNegative = _bNegative;
}

bool DataGraph::GetNegative() const
{
    return bNegative;
}

// Set / get relative schedule or not
void DataGraph::SetRelative(bool _bRelative)
{
    bRelative = _bRelative;
}

bool DataGraph::GetRelative() const
{
    return bRelative;
}

void DataGraph::SetName(const char *szName)
{
    Name = szName;
}

const char *DataGraph::GetName() const
{
    return Name.c_str();
}

float DataGraph::GetMinAtTime(float Time, float LifeTime)
{
    if (bRelative)
        Time = Time / LifeTime * 100.0f;

    const uint32_t Count = MinGraph.size();
    uint32_t Index;
    if (MinCachedTime < Time)
        Index = MinCachedIndex;
    else
        Index = 0;

    for (; Index < (Count - 1); Index++)
    {
        const auto ToTime = MinGraph[Index + 1].Time;

        // If the time is in the correct range
        if (Time <= ToTime)
        {
            const auto FromTime = MinGraph[Index].Time;

            const auto SegmentDeltaTime = ToTime - FromTime;
            const auto ValueDeltaTime = Time - FromTime;
            float blend_k;
            if (SegmentDeltaTime > 0.001f)
                blend_k = ValueDeltaTime / SegmentDeltaTime;
            else
                blend_k = 0.0f;

            const auto ValueFirst = MinGraph[Index].Val;
            const auto ValueSecond = MinGraph[Index + 1].Val;

            MinCachedTime = Time;
            MinCachedIndex = Index;
            return Lerp(ValueFirst, ValueSecond, blend_k);
        }
    }

    return 0.0f;
}

float DataGraph::GetMaxAtTime(float Time, float LifeTime)
{
    if (bRelative)
        Time = Time / LifeTime * 100.0f;

    const uint32_t Count = MaxGraph.size();

    uint32_t Index;

    if (MaxCachedTime < Time)
        Index = MaxCachedIndex;
    else
        Index = 0;

    for (; Index < (Count - 1); Index++)
    {
        const auto ToTime = MaxGraph[Index + 1].Time;

        // If the time is in the correct range
        if (Time <= ToTime)
        {
            const auto FromTime = MaxGraph[Index].Time;

            const auto SegmentDeltaTime = ToTime - FromTime;
            const auto ValueDeltaTime = Time - FromTime;
            float blend_k;
            if (SegmentDeltaTime > 0.001f)
                blend_k = ValueDeltaTime / SegmentDeltaTime;
            else
                blend_k = 0.0f;

            const auto ValueFirst = MaxGraph[Index].Val;
            const auto ValueSecond = MaxGraph[Index + 1].Val;

            MaxCachedTime = Time;
            MaxCachedIndex = Index;
            return Lerp(ValueFirst, ValueSecond, blend_k);
        }
    }

    return 0.0f;
}

float DataGraph::GetValue(float Time, float LifeTime, float K_rand)
{
    GraphRead++;
    const auto pMax = GetMaxAtTime(Time, LifeTime);
    const auto pMin = GetMinAtTime(Time, LifeTime);
    return Lerp(pMin, pMax, K_rand);
}

float DataGraph::GetRandomValue(float Time, float LifeTime)
{
    GraphRead++;
    const auto pMax = GetMaxAtTime(Time, LifeTime);
    const auto pMin = GetMinAtTime(Time, LifeTime);
    return RandomRange(pMin, pMax);
}

void DataGraph::Write(MemFile *File)
{
    uint32_t dwNegative = GetNegative();
    File->WriteType(dwNegative);

    uint32_t dwRelative = GetRelative();
    File->WriteType(dwRelative);

    uint32_t MaxGraphItemsCount = MaxGraph.size();
    File->WriteType(MaxGraphItemsCount);

    uint32_t i = 0;
    for (i = 0; i < MaxGraphItemsCount; i++)
    {
        auto fTime = MaxGraph[i].Time;
        File->WriteType(fTime);

        auto fValue = MaxGraph[i].Val;
        File->WriteType(fValue);
    }

    uint32_t MinGraphItemsCount = MinGraph.size();
    File->WriteType(MinGraphItemsCount);

    for (i = 0; i < MinGraphItemsCount; i++)
    {
        auto fTime = MinGraph[i].Time;
        File->WriteType(fTime);

        auto fValue = MinGraph[i].Val;
        File->WriteType(fValue);
    }

    // save name
    const uint32_t NameLength = Name.size();
    auto NameLengthPlusZero = NameLength + 1;
    File->WriteType(NameLengthPlusZero);
    Assert(NameLength < 128);
    File->Write(Name.c_str(), NameLength);
    File->WriteZeroByte();
}
