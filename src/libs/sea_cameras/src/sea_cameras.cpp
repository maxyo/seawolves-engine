#include "sea_cameras.h"
#include "deck_camera.h"
#include "free_camera.h"
#include "ship_camera.h"
#include "shared/sea_ai/script_defines.h"

CREATE_CLASS(SEA_CAMERAS)

CREATE_CLASS(FREE_CAMERA)

CREATE_CLASS(SHIP_CAMERA)

CREATE_CLASS(DECK_CAMERA)

SEA_CAMERAS::SEA_CAMERAS()
{
    bActive = true;
}

SEA_CAMERAS::~SEA_CAMERAS()
{
}

uint64_t SEA_CAMERAS::ProcessMessage(MESSAGE &message)
{
    uint32_t i;
    switch (message.Long())
    {
    case AI_CAMERAS_ADD_CAMERA: {
        const auto eidCamera = message.EntityID();
        auto *pCamera = static_cast<COMMON_CAMERA *>(core.GetEntityPointer(eidCamera));
        const auto it = std::find(CamerasArray.begin(), CamerasArray.end(), pCamera);
        if (it == CamerasArray.end())
            CamerasArray.push_back(pCamera);
        pCamera->SetOn(false);
        pCamera->SetActive(bActive);
    }
    break;
    case AI_CAMERAS_SET_CAMERA: {
        const auto eidCamera = message.EntityID();
        auto *const pACharacter = message.AttributePointer();
        auto *pCamera = static_cast<COMMON_CAMERA *>(core.GetEntityPointer(eidCamera));
        const auto it = std::find(CamerasArray.begin(), CamerasArray.end(), pCamera);
        if (it == CamerasArray.end())
            CamerasArray.push_back(pCamera);
        for (i = 0; i < CamerasArray.size(); i++)
            CamerasArray[i]->SetOn(false);
        pCamera->SetOn(true);
        pCamera->SetActive(bActive);
        pCamera->SetCharacter(pACharacter);
    }
    break;
    case AI_MESSAGE_SEASAVE: {
        auto *pSL = (CSaveLoad *)message.Pointer();
        for (i = 0; i < CamerasArray.size(); i++)
            CamerasArray[i]->Save(pSL);
    }
    break;
    case AI_MESSAGE_SEALOAD: {
        auto *pSL = (CSaveLoad *)message.Pointer();
        for (i = 0; i < CamerasArray.size(); i++)
            CamerasArray[i]->Load(pSL);
    }
    break;
    }
    return 0;
}
