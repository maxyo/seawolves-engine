#include "sink_effect.h"
#include "entity.h"
#include "core.h"
#include "shared/messages.h"
#include "ship_base.h"

CREATE_CLASS(SINKEFFECT)

SINKEFFECT::SINKEFFECT() : renderer(nullptr), sea(nullptr)
{
}

SINKEFFECT::~SINKEFFECT()
{

    // UNGUARD
}

bool SINKEFFECT::Init()
{

    sea = static_cast<SEA_BASE *>(core.GetEntityPointer(core.GetEntityId("sea")));

    renderer = static_cast<VRenderer *>(core.GetService("renderer"));

    InitializeSinks();

    return true;
    // UNGUARD
}

uint64_t SINKEFFECT::ProcessMessage(MESSAGE &message)
{

    const auto code = message.Long();
    const uint32_t outValue = 0;

    switch (code)
    {
    case MSG_SHIP_DELETE: {
        auto *const attrs = message.AttributePointer();
        if (attrs)
        {
            auto &&entities = core.GetEntityIds("ship");
            for (auto ent : entities)
            {

                auto *shipBase = static_cast<SHIP_BASE *>(core.GetEntityPointer(ent));
                if (shipBase->GetACharacter() == attrs)
                {
                    TryToAddSink(shipBase->GetPos(), shipBase->GetBoxsize().z / 2.0f);
                    return outValue;
                }
            } // if (FindClass)
        }     // if (attrs)
    }         // case
    break;
    }

    return outValue;
    // UNGUARD
}

void SINKEFFECT::Realize(uint32_t _dTime)
{

    for (auto i = 0; i < sink_effect::MAX_SINKS; ++i)
        sinks[i].Realize(_dTime);

    // UNGUARD
}

void SINKEFFECT::Execute(uint32_t _dTime)
{
    for (auto i = 0; i < sink_effect::MAX_SINKS; ++i)
        sinks[i].Process(_dTime);

    // UNGUARD
}

void SINKEFFECT::InitializeSinks()
{
    auto psIni = fio->OpenIniFile("resource\\ini\\particles.ini");

    for (auto i = 0; i < sink_effect::MAX_SINKS; ++i)
    {
        sinks[i].Release();
        sinks[i].Initialize(psIni.get(), sea, renderer);
    }
}

TSink *SINKEFFECT::TryToAddSink(const glm::vec3 &_pos, float _r)
{
    for (auto i = 0; i < sink_effect::MAX_SINKS; ++i)
    {
        if (!sinks[i].Enabled())
        {
            sinks[i].Start(_pos, _r);
            return &sinks[i];
        }
    }

    return nullptr;
}
