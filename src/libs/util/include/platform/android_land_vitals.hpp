// The land hero's bars and status, channel "vitals" of the portable UI (android_ui.hpp
// publishVitals), as the land interface read them (battle_interface land_status.hpp, which
// documents the fields): the typed state the channel compares every frame, and its JSON, built
// only when that state changed.
#pragma once
#include "android_gameplay.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
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
// The boarding deck fight (not the cabin, where the captain is the opponent): our men and theirs,
// now and at the fight's start (real men, as the game counts them after the fight).
struct LandBoarding
{
    int ours = 0, theirs = 0, oursStart = 0, theirsStart = 0;
    bool operator==(const LandBoarding &) const = default;
};
// The boarding script's counters (TEHO Loc_ai/LAi_boarding.c) as men: a fighter on the deck is a body
// standing for crew_per_chr real men, and each side counts its reserve (boarding_*_crew, not yet on
// the deck) and its living fighters on the deck; the start is boarding_*_crew_start bodies. A reserve
// below 0 is an empty one (the script takes its musketeers from the enemy reserve without a check, so
// it runs to -2 and less). Nothing when there is no start or a body stands for no men (the script
// divides the crew by the bodies).
inline std::optional<LandBoarding> landBoardingMen(int oursReserve, int oursAlive, float oursPerBody, int oursStart,
                                                   int theirsReserve, int theirsAlive, float theirsPerBody, int theirsStart)
{
    if (std::min(oursAlive, theirsAlive) < 0 || oursStart <= 0 || theirsStart <= 0 ||
        !std::isfinite(oursPerBody) || !std::isfinite(theirsPerBody) || oursPerBody <= 0.f || theirsPerBody <= 0.f)
        return std::nullopt;
    const auto men = [](int bodies, float perBody) { return static_cast<int>(std::lround(bodies * perBody)); };
    return LandBoarding{men(std::max(0, oursReserve) + oursAlive, oursPerBody), men(std::max(0, theirsReserve) + theirsAlive, theirsPerBody),
                        men(oursStart, oursPerBody), men(theirsStart, theirsPerBody)};
}
// The game's walk-only rule (TEHO characters.c CheckAndSetOverloadMode): an overloaded hero, one a quest
// forbids to run (GenQuest.CantRun) or one in the "protocusto" model gets the walk animation as his run
// (actions.run = actions.walk), so a full stick only walks. The HUD names why: "overload" when the load is
// the reason, "cantrun" when a quest or the model is. Nothing while his run is a run.
inline const char *landWalkOnlyEffect(std::string_view run, std::string_view walk, bool scripted)
{
    if (run.empty() || run != walk) return nullptr;
    return scripted ? "cantrun" : "overload";
}
struct LandVitals
{
    float health = 0.f, energy = 0.f, reload = 0.f;
    int loaded = 0, capacity = 0;
    LandPortrait hero;
    std::vector<std::string> effects;
    int potions = 0;
    std::vector<LandOfficer> officers;
    std::optional<LandOpponent> opponent;
    std::optional<LandBoarding> boarding;
    std::optional<float> dash;
    std::vector<BattleAbility> abilities; // the land abilities (land_status.hpp readLandAbilities)
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
    if (v.boarding)
        json += ",\"boarding\":{\"ours\":" + std::to_string(v.boarding->ours) + ",\"theirs\":" + std::to_string(v.boarding->theirs) +
                ",\"oursStart\":" + std::to_string(v.boarding->oursStart) +
                ",\"theirsStart\":" + std::to_string(v.boarding->theirsStart) + "}";
    if (v.dash) json += ",\"dash\":" + std::to_string(*v.dash);
    if (!v.abilities.empty()) json += ",\"abilities\":" + battleAbilitiesJson(v.abilities);
    return json + "}";
}
} // namespace storm::android
