#include "t_dynamic_system.h"
#include <algorithm>
#include <memory.h>

TDynamicSystem::TDynamicSystem()
{
    std::fill(attractors, attractors + MAX_DYNAMIC_OBJECTS, nullptr);
    std::fill(deflectors, deflectors + MAX_DYNAMIC_OBJECTS, nullptr);
}

TDynamicSystem::~TDynamicSystem()
{
}

bool TDynamicSystem::AddAttractor(TDynamicObject *_obj)
{
    for (auto i = 0; i < MAX_DYNAMIC_OBJECTS; i++)
    {
        if (!attractors[i])
        {
            attractors[i] = _obj;
            return true;
        }
    }

    return false;
}

bool TDynamicSystem::AddDeflector(TDynamicObject *_obj)
{
    for (auto i = 0; i < MAX_DYNAMIC_OBJECTS; i++)
    {
        if (!deflectors[i])
        {
            deflectors[i] = _obj;
            return true;
        }
    }

    return false;
}

