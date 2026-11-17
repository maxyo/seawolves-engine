//    Spirenkov Maxim, 2003
//
//    Lizards

#include "lizards.h"
#include "entity.h"
#include "core.h"

Lizards::Lizards()
{
    num = sizeof(lizard) / sizeof(Lizard);
}

Lizards::~Lizards()
{
}

bool Lizards::Init()
{
    const auto loc = core.GetEntityId("location");
    auto *location = (Location *)core.GetEntityPointer(loc);
    if (!location)
        return false;
    // init lizards
    for (int32_t i = 0; i < num; i++)
        lizard[i].Init(location);
    core.SetLayerType(REALIZE, layer_type_t::realize);
    core.AddToLayer(REALIZE, GetId(), 100000);
    return true;
}

void Lizards::Execute(uint32_t delta_time)
{
}

// Drawing
void Lizards::Realize(uint32_t delta_time)
{
    const auto dltTime = delta_time * 0.001f;
    for (int32_t i = 0; i < num; i++)
        lizard[i].Update(dltTime);
}
