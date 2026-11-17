//    Spirenkov Maxim, 2003
//
//    LocRats

#pragma once

#include "renderer.h"
#include "vma.hpp"

#include "loc_rat.h"

class LocRats : public Entity
{
  public:
    LocRats();
    ~LocRats() override;

  public:
    bool Init() override;
    void Execute(uint32_t delta_time);
    // Drawing
    void Realize(uint32_t delta_time);

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
        case Stage::execute:
            Execute(delta);
            break;
        case Stage::realize:
            Realize(delta);
            break;
        }
    }

    uint64_t ProcessMessage(MESSAGE &message) override;

  private:
    LocRat rat[32];
    int32_t num;
};
