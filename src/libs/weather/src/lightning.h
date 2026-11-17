#pragma once

#include "weather_base.h"
#include "typedef.h"
#include <vector>

#define MAX_LIGHTNING_TEXTURES 2

class LIGHTNING : public Entity
{
    struct flash_t
    {
        std::string sTechnique;
        float fSize;
        float fTime;
    };

    struct lightning_t
    {
        // script parameters
        uint32_t dwSubTexture;
        uint32_t dwFlickerTime;
        float fTime;
        float fSize, fScaleX, fScaleY;
        glm::vec3 vPos;

        // inner parameters
        float fAlpha;
        float fPower;

        std::string sTechnique;

        flash_t Flash;
    };

    std::vector<lightning_t> aLightnings;

    uint32_t dwSubTexX, dwSubTexY;
    int32_t iLightningTexture, iFlashTexture;

    float fKDist;
    int32_t iFlickerTime;

    WEATHER_BASE *pWeather;
    VRenderer *pRS;
    COLLIDE *pCollide;

    void Release() const;
    void CalcFlashPower(lightning_t *pL) const;

  public:
    LIGHTNING();
    ~LIGHTNING() override;

    void SetDevice();
    bool Init() override;
    void Realize(uint32_t Delta_Time);
    void Execute(uint32_t Delta_Time);
    uint64_t ProcessMessage(MESSAGE &message) override;
    uint32_t AttributeChanged(ATTRIBUTES *pAttributeChanged) override;

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
};
