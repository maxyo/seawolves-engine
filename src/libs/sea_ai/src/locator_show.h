#pragma once

#include "ai_helper.h"

class SeaLocatorShow : public Entity
{
    ATTRIBUTES *pALocators;
    bool bShow;
    float fScale;

    float fWidth, fHeight;
    glm::mat4 view, mtx;

    struct SphVertex
    {
        glm::vec3 v;
        uint32_t c;
    };

    uint32_t sphereNumTrgs;
    SphVertex *sphereVertex;

    void CreateSphere();

    bool isLocator(ATTRIBUTES *pA);
    glm::vec3 GetLocatorPos(ATTRIBUTES *pA);
    float GetLocatorAng(ATTRIBUTES *pA);
    float GetLocatorRadius(ATTRIBUTES *pA);
    const char *GetLocatorName(ATTRIBUTES *pA);
    const char *GetLocatorGroupName(ATTRIBUTES *pA);

    void PrintLocator(ATTRIBUTES *pA);
    void ProcessLocators(ATTRIBUTES *pA);

  public:
    SeaLocatorShow();
    ~SeaLocatorShow() override;

    bool Init() override;
    void Realize(uint32_t Delta_Time);
    void Execute(uint32_t Delta_Time) const;

    uint64_t ProcessMessage(MESSAGE &message) override;

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

    uint32_t AttributeChanged(ATTRIBUTES *pAttribute) override;
};
