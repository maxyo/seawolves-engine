//    Spirenkov Maxim
//
//    Lizards

#pragma once

#include "renderer.h"
#include "vma.hpp"

#include "lizard.h"

class Lizards : public Entity
{
  public:
    Lizards();
    ~Lizards() override;

    bool Init() override;
    // Update
    void Execute(uint32_t delta_time);
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

    Lizard lizard[8];
    int32_t num;
};
