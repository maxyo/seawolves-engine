//    Spirenkov Maxim aka Sp-Max Shaman, 2001
//
//    WideScreen

#pragma once

#include "renderer.h"
#include "vma.hpp"

class WideScreen : public Entity
{
  public:
    WideScreen();
    ~WideScreen() override;

    bool Init() override;
    uint64_t ProcessMessage(MESSAGE &message) override;
    void Realize(uint32_t delta_time);

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
        case Stage::realize:
            Realize(delta);
            break;
        }
    }

  private:
    VRenderer *rs; //
    float w, h;     // Screen dimensions
    float state;    // Screen state
    float dlt;      // Screen change direction
};
