#include "ai_ship.h"
#include "sea_base.h"

AIShipMoveController::AIShipMoveController(AIShip *pShip)
{
    fMoveTime = FRAND(2.0f);
    dwCurPnt = 0;
    vDeflectForce = glm::vec3(0.0f);
    vRetardForce = glm::vec3(0.0f);
    SetAIShip(pShip);
    Stop(true);
}

AIShipMoveController::~AIShipMoveController()
{
}

bool AIShipMoveController::Init()
{
    return true;
}

void AIShipMoveController::Execute(float fDeltaTime)
{

    fMoveTime -= fDeltaTime;

    if (isStopped() || GetAIShip()->isMainCharacter())
        return;
    auto *pShip = static_cast<SHIP_BASE *>(GetAIShip()->GetShipPointer());
    Assert(pShip);

    const auto vCurPos = GetAIShip()->GetPos();
    const auto vCurAng = GetAIShip()->GetAng();

    const auto vMovePoint = vDestPoint;

    vDeflectForce.z = 1.0f; // FRAND(2.0f) - 1.0f;

    const auto vCurDir = glm::vec3(sinf(vCurAng.y), 0.0f, cosf(vCurAng.y));
    const auto vDestDir = storm::math::normalizeOrZero(vMovePoint - vCurPos);

    auto fTime = 0.0f;
    const auto fDist = sqrtf(storm::math::lengthSquared(vCurPos - vMovePoint));
    const auto fBrakingDistance = pShip->GetBrakingDistance(&fTime);

    auto fSpeed = 1.0f;
    if (fBrakingDistance > fDist + 20.0f)
    {
        fSpeed = 1.0f - (fBrakingDistance - (fDist + 20.0f)) / 20.0f;
        if (fSpeed < 0.0f)
            fSpeed = 0.0f;
    }
    if (fabsf(fBrakingDistance - fDist) < 10.0f)
        fSpeed = 0.0f;
    GetAIShip()->GetSpeedController()->AddSpeed(fSpeed);

    const auto fRotationAngle = pShip->GetRotationAngle(&fTime);

    auto fTemp = storm::math::lengthSquared(vDeflectForce);
    if (storm::math::lengthSquared(vDeflectForce) > 0.00001f)
    {
        auto vRotDir = storm::math::normalizeOrZero(vDeflectForce);

        const auto fDot = glm::dot(vCurDir, vDestDir);
        const auto fRotAng = fabsf(acosf(Clamp(fDot)));

        const auto vProd = glm::cross(vCurDir, vDestDir);
        const auto fSignRot = ((vProd.y > 0.0f) ? 1.0f : -1.0f);
        // check fSignRot with can Rotate in these direction
        const auto fMul = (fDot > 0.0f) ? Bring2Range(1.0f, 0.1f, 0.0f, 1.0f, fDot) : 1.0f;
        auto fAngRot = fSignRot;
        if (fRotationAngle >= fRotAng)
            fAngRot = 0.0f;
        GetAIShip()->GetRotateController()->AddRotate(fMul * fAngRot);
        if (fMul > 0.3f)
            GetAIShip()->GetSpeedController()->MulSpeed(0.5f);
    }

    vRetardForce = glm::vec3(0.0f);
}

void AIShipMoveController::Realize(float fDeltaTime)
{
}

void AIShipMoveController::Move(glm::vec3 vMovePoint)
{
    auto fDist = sqrtf(storm::math::lengthSquared(vMovePoint - vDestPoint));
    if (fMoveTime > 0.0f)
        return;
    fMoveTime = 2.0f;

    if (AIHelper::pIsland)
    {
        glm::vec3 vRealMovePoint;
        auto vOurPos = GetAIShip()->GetPos();
        vOurPos.y = vMovePoint.y = 0.0f;
        const auto b = AIHelper::pIsland->GetMovePoint(vOurPos, vMovePoint, vRealMovePoint);
        if (b)
        {
            vMovePoint = vRealMovePoint;
        }
    }
    Stop(false);
    vDestPoint = vMovePoint;
}

void AIShipMoveController::AddRetardForce(glm::vec3 _vRetardForce)
{
    vRetardForce += _vRetardForce;
}

void AIShipMoveController::AddDeflectForce(glm::vec3 _vDeflectForce)
{
    vDeflectForce += _vDeflectForce;
}

void AIShipMoveController::Save(CSaveLoad *pSL) const
{
    pSL->SaveDword(bStopped);
    pSL->SaveVector(vDestPoint);
    pSL->SaveVector(vRetardForce);
    pSL->SaveVector(vDeflectForce);
    pSL->SaveFloat(fMoveTime);
    pSL->SaveDword(dwCurPnt);
}

void AIShipMoveController::Load(CSaveLoad *pSL)
{
    bStopped = pSL->LoadDword() != 0;
    vDestPoint = pSL->LoadVector();
    vRetardForce = pSL->LoadVector();
    vDeflectForce = pSL->LoadVector();
    fMoveTime = pSL->LoadFloat();
    dwCurPnt = pSL->LoadDword();
}
