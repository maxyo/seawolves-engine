#pragma once

#include "common_camera.h"
#include "island_base.h"
#include "renderer.h"

class FREE_CAMERA : public COMMON_CAMERA
{
  private:
    VRenderer *pRS;
    glm::vec3 vPos{}, vAng{};
    float fFov;
    int32_t iLockX, iLockY;

    ISLAND_BASE *pIslandBase;
    BOOL bCameraOnEarth;
    float fCameraOnEarthHeight;

  public:
    FREE_CAMERA();
    ~FREE_CAMERA() override;

    void SetDevice();
    bool Init() override;
    void Move(uint32_t DeltaTime);
    void Execute(uint32_t Delta_Time);
    void PrimeLoadingCamera();

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
        case Stage::loading_prepare:
            PrimeLoadingCamera();
            break;
        case Stage::execute:
            Execute(delta);
            break;
        }
    }

    void Save(CSaveLoad *pSL) override;
    void Load(CSaveLoad *pSL) override;
};
