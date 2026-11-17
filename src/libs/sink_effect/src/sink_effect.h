#pragma once

#include "storm_glm.h"
#include "sink_splash_defines.h"
#include "t_sink.h"
#include "renderer.h"
#include "geometry.h"
#include "sea_base.h"

class SINKEFFECT : public Entity
{
  public:
    SINKEFFECT();
    ~SINKEFFECT() override;

    bool Init() override;
    uint64_t ProcessMessage(MESSAGE &message) override;
    virtual void Realize(uint32_t dTime);
    virtual void Execute(uint32_t dTime);

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

  private:
    void InitializeSinks();
    TSink *TryToAddSink(const glm::vec3 &_pos, float _r);

    TSink sinks[sink_effect::MAX_SINKS];
    VRenderer *renderer;
    SEA_BASE *sea;
};
