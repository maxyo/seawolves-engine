//    Spirenkov Maxim, 2003
//
//    LocEagle

#pragma once

#include "model.h"

class LocEagle : public Entity
{
  public:
    LocEagle();
    ~LocEagle() override;

    bool Init() override;
    void Execute(uint32_t delta_time);

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
        case Stage::execute:
            Execute(delta);
            break;
        }
    }

    entid_t mdl;
    glm::vec3 cnt;
    float kTime, time;
    float y, dltY;
    float kRad, dltRad;
    float ay;
    float kAy, timeAy;
};
