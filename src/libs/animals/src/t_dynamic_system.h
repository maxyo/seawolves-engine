#pragma once

#include "t_dynamic_object.h"

#define MAX_DYNAMIC_OBJECTS 150

class TDynamicSystem
{
  public:
    TDynamicSystem();
    virtual ~TDynamicSystem();

    bool AddAttractor(TDynamicObject *_obj);
    bool AddDeflector(TDynamicObject *_obj);

  protected:
    TDynamicObject *attractors[MAX_DYNAMIC_OBJECTS];
    TDynamicObject *deflectors[MAX_DYNAMIC_OBJECTS];
};
