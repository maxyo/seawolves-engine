#include "particles.h"

#include "../particles.h"
#include "core.h"
#include "entity.h"
#include "math_inlines.h"
#include "p_system.h"
#include "string_compare.hpp"
#include "shared/messages.h"
#include "storm/render/effects.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace
{
// The particle systems of the game's shots and explosions, which have no light of their own: the renderer adds a
// short flash light at the emitter (render::EffectsFlash). Cannon shots get theirs from AICannon (ShipLights), not
// from their smoke ("cancloud_*", "Bombard"). The names are the systems the scripts and the engine create
// (PROGRAM/sea_ai, quests, BLADE::GunFire); a system not listed here makes no light.
std::optional<storm::render::EffectsFlashKind> FlashOf(std::string_view name)
{
    using Kind = storm::render::EffectsFlashKind;
    struct Entry
    {
        std::string_view name;
        Kind kind;
    };
    static constexpr std::array<Entry, 5> kFlashes{{
        {"gunfire", Kind::Gunshot},
        {"blast", Kind::BallBlast},
        {"blast_inv", Kind::Explosion},
        {"town_explodion", Kind::Explosion},
        {"ShipExplode", Kind::ShipExplosion},
    }};
    for (const Entry &e : kFlashes)
        if (storm::iEquals(name, e.name))
            return e.kind;
    return std::nullopt;
}

void ReportFlash(std::string_view name, const glm::vec3 &pos)
{
    if (const auto kind = FlashOf(name))
        storm::render::EffectsFlash(*kind, {pos.x, pos.y, pos.z});
}
} // namespace

PARTICLES::PARTICLES()
{
    bSystemDelete = false;
    pService = nullptr;
    pManager = nullptr;
    CreationCapture = false;
}

PARTICLES::~PARTICLES()
{
    bSystemDelete = true;
    DeleteAll();
}

bool PARTICLES::Init()
{
    core.AddToLayer(REALIZE, GetId(), 0xfffff);
    core.AddToLayer(EXECUTE, GetId(), 0);

    pService = static_cast<IParticleService *>(core.GetService("ParticleService"));
    Assert(pService);
    pManager = pService->DefManager();
    Assert(pManager);
    return true;
}

uint64_t PARTICLES::ProcessMessage(MESSAGE &message)
{
    const auto code = message.Long();

    glm::vec3 pos, angles;
    int32_t lifetime;

    switch (code)
    {
        // Pause all particles
        // new not paused ones are emitted ...
    case PS_PAUSEALL: {
        PauseAllActive(message.Long() != 0);
        break;
    }

        // Delete a specific system
    case PS_DELETE: {
        DeleteSystem(message.Long());
        break;
    }

    case PS_CREATIONCAPTURE_BEG: {
        CreationCapture = true;
        break;
    }
    case PS_CREATIONCAPTURE_END: {
        CreationCapture = false;
        break;
    }
    case PS_CLEAR_CAPTURED: {
        DeleteCaptured();
        break;
    }

        // delete everything
    case PS_CLEARALL: {
        DeleteAll();
        break;
    }
        // create system (string name, float x, y, z position, float rx, ry, rz rotation, float life_time lifetime)
    case PS_CREATE_RIC: {
        const std::string &ps_name = message.String();
        pos.x = message.Float();
        pos.y = message.Float();
        pos.z = message.Float();

        angles.x = message.Float();
        angles.y = message.Float();
        angles.z = message.Float();
        lifetime = message.Long();

        auto *pSystem = CreateSystem(ps_name.c_str(), lifetime);
        if (!pSystem)
            return 0;

        pSystem->SetEmitter(pos, angles);
        pSystem->SetDelay(0);
        pSystem->SetLifeTime(lifetime);
        ReportFlash(ps_name, pos);
        return (uintptr_t)pSystem;

        break;
    }
    case PS_CREATEX_RIC: {
        throw std::runtime_error("Unsupported particle manager command !!!");
    }

        // create a system
    case PS_CREATE: {
        const std::string &ps_name = message.String();
        pos.x = message.Float();
        pos.y = message.Float();
        pos.z = message.Float();

        angles.x = message.Float();
        angles.y = message.Float();
        angles.z = message.Float();
        lifetime = message.Long();

        auto *pSystem = CreateSystem(ps_name.c_str(), lifetime);
        if (!pSystem)
            return 0;

        pSystem->SetEmitter(pos, angles);
        pSystem->SetDelay(0);
        pSystem->SetLifeTime(lifetime);
        ReportFlash(ps_name, pos);
        return (uintptr_t)pSystem;

        break;
    }
        // create a system
    case PS_CREATEX: {
        const std::string &ps_name = message.String();
        pos.x = message.Float();
        pos.y = message.Float();
        pos.z = message.Float();

        glm::vec3 normal;
        normal.x = message.Float();
        normal.y = message.Float();
        normal.z = message.Float();
        const auto fLen = static_cast<double>(storm::math::normalizeInPlace(normal));

        if (fLen)
        {
            angles.y = storm::math::angleY(normal);
            auto fDiv = -(normal.y / fLen);
            fDiv = Min(Max(fDiv, -1.0), 1.0);
            angles.x = static_cast<float>(asin(fDiv));
        }
        else
        {
            angles.x = 0.0f;
            angles.y = 0.0f;
        }

        angles.z = 0.0f;

        lifetime = message.Long();

        auto *pSystem = CreateSystem(ps_name.c_str(), lifetime);
        if (!pSystem)
            return 0;

        pSystem->SetEmitter(pos, angles);
        pSystem->SetDelay(0);
        pSystem->SetLifeTime(lifetime);
        ReportFlash(ps_name, pos);
        return (uintptr_t)pSystem;

        break;
    }
    case PS_ADDTRACKPOINT: {
        throw std::runtime_error("Unsupported particle manager command !!!");
    }
    case PS_USESURFACE: {
        throw std::runtime_error("Unsupported particle manager command !!!");
    }
    case PS_VALIDATE_PARTICLE: {
        auto *const SystemID = reinterpret_cast<PARTICLE_SYSTEM *>(message.Pointer());
        for (uint32_t n = 0; n < CreatedSystems.size(); n++)
            if (CreatedSystems[n].pSystem == SystemID)
                return 1;
        return 0;
        break;
    }
    }
    return 0;
}

PARTICLE_SYSTEM *PARTICLES::CreateSystem(const char *pFileName, uint32_t LifeTime)
{
    //psnip_trap(); //~!~
    auto path = std::filesystem::path() / "resource" / "particles" / pFileName;
    std::string pathStr = path.extension().string();
    if (!storm::iEquals(pathStr, ".xps"))
        path += ".xps";
    pathStr = path.string();
    // MessageBoxA(NULL, (LPCSTR)path.c_str(), "", MB_OK); //~!~

    IParticleSystem *pSys = pManager->CreateParticleSystemEx(pathStr.c_str(), __FILE__, __LINE__);
    if (!pSys)
    {
        return nullptr;
    }

    pSys->AutoDelete(false);

    auto *pNewPS = new PARTICLE_SYSTEM(pSys);
    pNewPS->SetManager(this);


    SystemInfo Info;
    Info.pSystem = pNewPS;
    Info.LifeTime = LifeTime;
    Info.FileName = pathStr;
    CreatedSystems.push_back(Info);

    if (CreationCapture)
    {
        CaptureBuffer.push_back((uintptr_t)pNewPS);
    }

    return pNewPS;
}

void PARTICLES::DeleteSystem(uintptr_t SystemID)
{
    bSystemDelete = true;
    for (uint32_t n = 0; n < CreatedSystems.size(); n++)
    {
        if (CreatedSystems[n].pSystem == (PARTICLE_SYSTEM *)SystemID)
        {
            delete CreatedSystems[n].pSystem;
            CreatedSystems[n] = CreatedSystems.back();
            CreatedSystems.pop_back();
            bSystemDelete = false;
            return;
        }
    }


    bSystemDelete = false;
}

void PARTICLES::DeleteAll()
{
    bSystemDelete = true;
    for (uint32_t n = 0; n < CreatedSystems.size(); n++)
    {
        delete CreatedSystems[n].pSystem;
    }

    CreatedSystems.clear();
    bSystemDelete = false;
}

void PARTICLES::DeleteResource(PARTICLE_SYSTEM *pResource)
{
    if (bSystemDelete)
        return;

    for (uint32_t n = 0; n < CreatedSystems.size(); n++)
    {
        if (CreatedSystems[n].pSystem == pResource)
        {
            CreatedSystems[n] = CreatedSystems.back();
            CreatedSystems.pop_back();
            return;
        }
    }
}

void PARTICLES::LoadingRealize()
{
    if (pManager) pManager->DrawCurrent();
}

void PARTICLES::Realize(uint32_t Delta_Time)
{
    bSystemDelete = true;
    const float fDeltaTime = static_cast<float>(Delta_Time) * 0.001f;
    pManager->Execute(fDeltaTime);

    // If it's time, pause emission
    // when all particles die, the system will retire by itself ...
    for (uint32_t n = 0; n < CreatedSystems.size(); n++)
    {
        CreatedSystems[n].PassedTime += Delta_Time;

        if (CreatedSystems[n].LifeTime == 0)
            continue;

        if (CreatedSystems[n].PassedTime > CreatedSystems[n].LifeTime)
        {
            CreatedSystems[n].pSystem->Stop();
        }
    }

    // Removing dead systems ...
    for (uint32_t n = 0; n < CreatedSystems.size(); n++)
    {
        if (!CreatedSystems[n].pSystem->GetSystem()->IsAlive())
        {
            delete CreatedSystems[n].pSystem;
            CreatedSystems[n] = CreatedSystems.back();
            CreatedSystems.pop_back();
            n--;
        }
    }

    bSystemDelete = false;
}

void PARTICLES::Execute(uint32_t Delta_Time)
{
}

void PARTICLES::PauseAllActive(bool bPaused)
{
    for (uint32_t n = 0; n < CreatedSystems.size(); n++)
    {
        CreatedSystems[n].pSystem->Pause(bPaused);
    }
}

void PARTICLES::DeleteCaptured()
{
    for (uint32_t n = 0; n < CaptureBuffer.size(); n++)
    {
        DeleteSystem(CaptureBuffer[n]);
    }

    CaptureBuffer.clear();
}
