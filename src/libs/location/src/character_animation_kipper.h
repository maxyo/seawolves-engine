//    Spirenkov Maxim aka Sp-Max Shaman, 2001
//
//    CharacterAnimationKipper

#pragma once

#include "animation.h"
#include "entity.h"
#include "vma.hpp"

class VRenderer;

class CharacterAnimationKipper : public Entity
{
  public:
    CharacterAnimationKipper();
    virtual ~CharacterAnimationKipper();

    bool Init() override;
    void ProcessStage(Stage, uint32_t) override
    {
    }

  private:
    VRenderer *rs;

    AnimationService *asr;
    Animation *aniMan;
    Animation *aniWoman;

    int32_t lockTextures[16];
    int32_t numLTextures;
};
