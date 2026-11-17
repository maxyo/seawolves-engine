#pragma once

#include "rain.h"
#include "sky.h"
#include "weather_base.h"
#include "typedef.h"
#include <stdio.h>

class WEATHER : public WEATHER_BASE
{
  private:
    VRenderer *pRS = nullptr;

    // Zero until the scripts set them (Execute reads them every frame; not every script set gives every one)
    float fFloats[MAX_FLOAT_PARAMS]{};
    int32_t iLongs[MAX_LONG_PARAMS]{};
    uint32_t dwColors[MAX_COLOR_PARAMS]{};
    glm::vec3 vVectors[MAX_VECTORS_PARAMS]{};
    char *pStrings[MAX_STRINGS_PARAMS]{};

    // timer data
    float fUpdateFrequence;

    // sun moving data
    bool bSunPresent;
    float fSunHeight;
    float fSunBegAngle;
    float fSunBegTime;
    float fSunEndAngle;
    float fSunEndTime;
    // moon moving data
    bool bMoonPresent;
    float fMoonHeight;
    float fMoonBegAngle;
    float fMoonBegTime;
    float fMoonEndAngle;
    float fMoonEndTime;

    void SetBeginData();
    void UpdateSunMoonPos();
    // The sun at `hour`: false (and straight down) outside the sun's day.
    bool SunAt(float hour, float &height_angle, float &azimuth_angle, glm::vec3 &pos) const;
    void PushEffectsWeather();

    void SetCommonStates();
    void CleanUP();

    // Weather_Base function
    int32_t GetInt(uint32_t dwCode) override;
    uint32_t GetColor(uint32_t, glm::vec3 *) override;
    uint32_t GetColor(uint32_t) override;
    float GetFloat(uint32_t) override;
    void GetVector(uint32_t, glm::vec3 *) override;

  public:
    WEATHER();
    ~WEATHER() override;

    void SetDevice();
    bool Init() override;
    void Execute(uint32_t Delta_Time);
    void Realize(uint32_t Delta_Time);
    uint32_t AttributeChanged(ATTRIBUTES *pAttribute) override;

    void ProcessStage(Stage stage, uint32_t delta) override
    {
        switch (stage)
        {
        case Stage::execute:
            Execute(delta);
            break;
        case Stage::loading_prepare:
            PushEffectsWeather(); // already configured weather; no clock or script update
            break;
        case Stage::realize:
            Realize(delta);
            break;
        }
    }
};
