#pragma once

#include "common_camera.h"

#include <vector>

class SEA_CAMERAS : public Entity
{
    std::vector<COMMON_CAMERA *> CamerasArray;
    bool bActive;

  public:
    SEA_CAMERAS();
    ~SEA_CAMERAS() override;

    uint64_t ProcessMessage(MESSAGE &message) override;

    bool Init() override
    {
        return true;
    }

    void ProcessStage(Stage, uint32_t) override
    {
    }
};
