// The land hero's bars and status, channel "vitals" of the portable UI (android_ui.hpp
// publishVitals), as the land interface read them (battle_interface land_status.hpp, which
// documents the fields): the typed state the channel compares every frame, and its JSON, built
// only when that state changed.
#pragma once
#include "android_gameplay.hpp"
#include <optional>
#include <string>
#include <vector>

namespace storm::android
{
// A character of the land HUD: its index, name, FaceId and, for the picture drawn without a
// FaceId, the land interface icon's texture and uv text as the scripts set them.
struct LandPortrait
{
    int id = -1;
    std::string name, face, texture, uv;
    bool operator==(const LandPortrait &) const = default;
};
struct LandOfficer
{
    LandPortrait who;
    float health = 0.f, energy = 0.f;
    bool operator==(const LandOfficer &) const = default;
};
struct LandOpponent
{
    std::string name;
    float health = 0.f;
    bool operator==(const LandOpponent &) const = default;
};
struct LandVitals
{
    float health = 0.f, energy = 0.f, reload = 0.f;
    int loaded = 0, capacity = 0;
    LandPortrait hero;
    std::vector<std::string> effects;
    int potions = 0;
    std::vector<LandOfficer> officers;
    std::optional<LandOpponent> opponent;
    std::optional<float> dash;
    bool operator==(const LandVitals &) const = default;
};
// `picture(portrait)`: the JSON of the picture drawn for a portrait ("null" for none).
template <class Picture> std::string landVitalsJson(const LandVitals &v, Picture &&picture)
{
    std::string json = "{\"health\":" + std::to_string(v.health) + ",\"energy\":" + std::to_string(v.energy) +
                       ",\"loaded\":" + std::to_string(v.loaded) + ",\"capacity\":" + std::to_string(v.capacity) +
                       ",\"reload\":" + std::to_string(v.reload);
    std::string effects;
    for (const auto &effect : v.effects) effects += std::string(effects.empty() ? "" : ",") + "\"" + effect + "\"";
    json += ",\"id\":" + std::to_string(v.hero.id) + ",\"name\":" + gameplayJsonString(v.hero.name) +
            ",\"face\":" + gameplayJsonString(v.hero.face) + ",\"picture\":" + picture(v.hero) + ",\"effects\":[" + effects +
            "],\"potions\":" + std::to_string(v.potions) + ",\"officers\":[";
    bool first = true;
    for (const auto &officer : v.officers)
    {
        json += std::string(first ? "" : ",") + "{\"id\":" + std::to_string(officer.who.id) +
                ",\"name\":" + gameplayJsonString(officer.who.name) + ",\"face\":" + gameplayJsonString(officer.who.face) +
                ",\"picture\":" + picture(officer.who) + ",\"health\":" + std::to_string(officer.health) +
                ",\"energy\":" + std::to_string(officer.energy) + "}";
        first = false;
    }
    json += "]";
    if (v.opponent)
        json += ",\"opponent\":{\"name\":" + gameplayJsonString(v.opponent->name) +
                ",\"health\":" + std::to_string(v.opponent->health) + "}";
    if (v.dash) json += ",\"dash\":" + std::to_string(*v.dash);
    return json + "}";
}
} // namespace storm::android
