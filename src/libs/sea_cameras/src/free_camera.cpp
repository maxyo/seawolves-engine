#include "free_camera.h"
#include "save_load.h"
#include "collide.h"

#define SENSITIVITY 0.0015f
#define FOV 1.285f

entid_t sphere;
COLLIDE *pCollide;

FREE_CAMERA::FREE_CAMERA()
{
    SetOn(false);
    SetActive(false);

    pIslandBase = nullptr;
    pRS = nullptr;
    vPos.z = 250.0f;
    vPos.y = 3.0f;
    fFov = FOV;

    iLockX = 0;
    iLockY = 0;

    fCameraOnEarthHeight = 3.0f;
    bCameraOnEarth = false;
}

FREE_CAMERA::~FREE_CAMERA()
{
}

bool FREE_CAMERA::Init()
{
    SetDevice();
    // UNGUARD
    return true;
}

void FREE_CAMERA::SetDevice()
{
    pRS = static_cast<VRenderer *>(core.GetService("renderer"));
    Assert(pRS);
    pCollide = static_cast<COLLIDE *>(core.GetService("COLL"));
    Assert(pCollide);

}

void FREE_CAMERA::Execute(uint32_t Delta_Time)
{
    if (!isOn())
        return;

    SetPerspective(AttributesPointer->GetAttributeAsFloat("Perspective"));
    
    float persp;
    pRS->GetCamera(vPos, vAng, persp);

    if (!pIslandBase)
        pIslandBase = static_cast<ISLAND_BASE *>(core.GetEntityPointer(core.GetEntityId("island")));

    Move(core.GetDeltaTime());
}

void FREE_CAMERA::PrimeLoadingCamera()
{
    if (!isOn() || !isActive()) return;
    SetPerspective(AttributesPointer->GetAttributeAsFloat("Perspective"));
    pRS->SetCamera(vPos, vAng, GetPerspective());
}

void FREE_CAMERA::Move(uint32_t DeltaTime)
{
    if (!isActive())
        return;

    // POINT pnt;
    CONTROL_STATE cs;

    {
        core.Controls->GetControlState("FreeCamera_Turn_H", cs);
        vAng.y += SENSITIVITY * static_cast<float>(cs.fValue);
        core.Controls->GetControlState("FreeCamera_Turn_V", cs);
        vAng.x += SENSITIVITY * static_cast<float>(cs.fValue);
    }
    if (bCameraOnEarth && pIslandBase)
    {
        pIslandBase->GetDepth(vPos.x, vPos.z, &vPos.y);
        vPos.y += fCameraOnEarthHeight;
    }
    const auto c0 = cosf(vAng.y);
    const auto s0 = sinf(vAng.y);
    const auto c1 = cosf(vAng.x);
    const auto s1 = sinf(vAng.x);
    auto c2 = cosf(vAng.z);
    float s2 = sinf(vAng.z);
    float speed = 5.0f * 0.001f * static_cast<float>(DeltaTime);

    if (core.Controls->GetAsyncKeyState(VK_SHIFT))
        speed *= 4.0f;
    if (core.Controls->GetAsyncKeyState(VK_CONTROL))
        speed *= 8.0f;

    core.Controls->GetControlState("FreeCamera_Forward", cs);
    if (cs.state == CST_ACTIVE)
        vPos += speed * glm::vec3(s0 * c1, -s1, c0 * c1);
    core.Controls->GetControlState("FreeCamera_Backward", cs);
    if (cs.state == CST_ACTIVE)
        vPos -= speed * glm::vec3(s0 * c1, -s1, c0 * c1);



    pRS->SetCamera(vPos, vAng, GetPerspective());

}

void FREE_CAMERA::Save(CSaveLoad *pSL)
{
    pSL->SaveVector(vPos);
    pSL->SaveVector(vAng);
    pSL->SaveFloat(fFov);
    pSL->SaveLong(iLockX);
    pSL->SaveLong(iLockY);

    pSL->SaveDword(bCameraOnEarth);
    pSL->SaveFloat(fCameraOnEarthHeight);
}

void FREE_CAMERA::Load(CSaveLoad *pSL)
{
    vPos = pSL->LoadVector();
    vAng = pSL->LoadVector();
    fFov = pSL->LoadFloat();
    iLockX = pSL->LoadLong();
    iLockY = pSL->LoadLong();

    bCameraOnEarth = pSL->LoadDword() != 0;
    fCameraOnEarthHeight = pSL->LoadFloat();
}
