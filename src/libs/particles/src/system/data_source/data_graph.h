#pragma once

#include "../../i_common/mem_file.h"

#include "../../i_common/graph_vertex.h"
#include <string>
#include <vector>

class DataGraph
{
    std::string Name;

    // when was the last time the value was read
    float MaxCachedTime;
    float MinCachedTime;
    // What was the index of this time
    uint32_t MaxCachedIndex;
    uint32_t MinCachedIndex;

    std::vector<GraphVertex> MinGraph;
    std::vector<GraphVertex> MaxGraph;

    void ResetCachedTime();

    float GetMinAtTime(float Time, float LifeTime);
    float GetMaxAtTime(float Time, float LifeTime);

    bool bRelative;
    bool bNegative;

  public:
    // constructor / destructor
    DataGraph();
    virtual ~DataGraph();

    // Set / get can be negative values in the graph or not
    void SetNegative(bool _bNegative);
    bool GetNegative() const;

    // Set / get relative schedule or not
    void SetRelative(bool _bRelative);
    bool GetRelative() const;

    // Get value (Current time, Random factor [0..1])
    float GetValue(float Time, float LifeTime, float K_rand);
    float GetRandomValue(float Time, float LifeTime);

    // Set the "default"
    void SetDefaultValue(float MaxValue, float MinValue);

    void Load(MemFile *File);
    void Write(MemFile *File);

    void SetName(const char *szName);
    const char *GetName() const;

};
