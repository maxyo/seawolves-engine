#pragma once

#include "renderer.h"
#include "geometry.h"
#include "vma.hpp"

class Lights;

class LocModelRealizer : public Entity
{
    entid_t eid_model;
    Lights *lights;
    VRenderer *rs;
    VGEOMETRY *gs;
    bool bShow;

  public:
    LocModelRealizer();
    ~LocModelRealizer() override;

    bool Init() override;
    void Execute(uint32_t delta_time);
    void Realize(uint32_t delta_time) const;

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
        case Stage::execute:
            Execute(delta);
            break;
        case Stage::loading_realize:
            Realize(0);
            break;
        case Stage::realize:
            Realize(delta);
            break;
        }
    }

    uint64_t ProcessMessage(MESSAGE &message) override;
};
