// The active abilities control shared by the sea and land scenes (hud.h AbilityControls).
#include "hud.h"

#include <cmath>

namespace storm::ui::hud
{

namespace
{

struct Look
{
    const char *id, *icon;
};
// The icon of each ability the channels admit (states.cpp ReadAbilities).
constexpr Look kLooks[] = {
    {"ImmediateReload", "reload"}, {"LightRepair", "repair"}, {"InstantRepair", "perk-instantrepair"},
    {"Turn180", "maneuver"},       {"Rush", "flame"},
};

const char *IconOf(const std::string &id)
{
    for (const Look &look : kLooks)
        if (id == look.id)
            return look.icon;
    return "actions"; // unreachable: the consumers admit the ids of the table only
}

} // namespace

void AbilityControls::Build(const std::vector<Ability> &abilities, View &view, Input &input,
                            const std::function<void(const Ability &)> &launch, std::vector<Fan::Option> lead)
{
    if (abilities.empty() && lead.empty())
    {
        fan_.Close(); // nothing to offer: no button at all
        return;
    }
    // The button: a green dot when an ability can be used now, the gold fill while one runs.
    bool ready = false;
    double active = 0.0;
    std::vector<Fan::Option> options = std::move(lead);
    for (const Ability &ability : abilities)
    {
        ready = ready || ability.state == "ready";
        if (ability.state == "active")
            active = std::max(active, ability.fill);
        // The game's own name (the command's note); the pack's only when the game gave none.
        Json props = {{"icon", IconOf(ability.id)},
                      {"label", ability.label.empty() ? hud_.Text("hud.ability.name." + ability.id) : ability.label}};
        // Running and recovering: the fill and the seconds the scripts still count (recovering, as
        // blocked, in the disabled look: it cannot be used now).
        if (ability.state == "active" || ability.state == "recovery")
            props.update(Json{{"fill", std::lround(ability.fill * 100.0)},
                              {"fillTone", ability.state == "active" ? "reload" : "cooldown"},
                              {"reloading", true},
                              {"badge", std::to_string(std::max(1, ability.left))}});
        if (ability.state == "recovery" || ability.state == "blocked")
            props["disabled"] = true;
        Fan::Option option{std::move(props), {}};
        if (ability.state == "ready")
            option.pick = [launch, ability] { launch(ability); };
        options.push_back(std::move(option));
    }
    Json button = {{"icon", "rank"}, {"label", hud_.Text("hud.ability.group")}, {"cue", ready}};
    if (active > 0.0)
        button.update(Json{{"fill", std::lround(active * 100.0)}, {"fillTone", "reload"}, {"reloading", true}});
    fan_.Build(std::move(button), options, view, input);
}

} // namespace storm::ui::hud
