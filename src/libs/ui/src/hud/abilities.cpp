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

std::string AbilityControls::Name(const Ability &ability)
{
    // The game's own name (the command's note); the pack's only when the game gave none.
    return ability.label.empty() ? hud_.Text("hud.ability.name." + ability.id) : ability.label;
}

Json AbilityControls::Row(const Ability &ability)
{
    const std::string seconds = std::to_string(std::max(1, ability.left)) + "\u00a0" + hud_.Text("hud.ability.seconds");
    Json row = {{"id", "ability-" + ability.id}, {"icon", IconOf(ability.id)}, {"title", Name(ability)}, {"state", ability.state}};
    // Ready: a tap launches it. Running and recovering: the seconds the scripts still count, the
    // row only shows (recovering also greys the icon by how far it is from back). Blocked:
    // disabled, a tap toasts when the game allows it (the press behaviour's reason).
    if (ability.state == "ready")
        row.update(Json{{"meta", hud_.Text("hud.ability.ready")}, {"metaTone", "ok"}, {"cooldown", 0}});
    else if (ability.state == "active")
        row.update(Json{{"meta", hud_.Text("hud.ability.active") + " " + seconds}, {"metaTone", "gold"}, {"cooldown", 0}});
    else if (ability.state == "recovery")
        row.update(Json{{"meta", hud_.Text("hud.ability.recovery") + " " + seconds},
                        {"metaTone", "dim"},
                        {"cooldown", std::lround((1.0 - ability.fill) * 100.0)}});
    else
        row.update(Json{{"meta", hud_.Text("hud.ability.blocked")},
                        {"metaTone", "dim"},
                        {"cooldown", 0},
                        {"reason", Name(ability) + ": " + hud_.Text("hud.ability.blocked." + ability.id)}});
    return row;
}

void AbilityControls::Build(const std::vector<Ability> &abilities, View &view, Input &input,
                            const std::function<void(const Ability &)> &launch)
{
    if (abilities.empty())
    {
        open_ = false; // nothing to offer: no button at all
        return;
    }
    // The button: small and quiet; a dot when one of them can be used now, the fill while one runs.
    bool ready = false;
    double active = 0.0;
    for (const Ability &ability : abilities)
    {
        ready = ready || ability.state == "ready";
        if (ability.state == "active")
            active = std::max(active, ability.fill);
    }
    Json button = {{"icon", "rank"}, {"label", hud_.Text("hud.ability.group")}, {"variant", open_ ? "active" : "normal"}, {"cue", ready}};
    if (active > 0.0)
        button.update(Json{{"fill", std::lround(active * 100.0)}, {"fillTone", "active"}, {"reloading", true}});
    view.elements["abilities"] = std::move(button);
    input.On("abilities", "click", [this](const Event &) {
        open_ = !open_;
        hud_.Rebuild();
    });
    if (!open_)
        return;

    // The list: a modal sheet beside the button, every ability with its icon, name and state.
    Json rows = Json::array();
    for (const Ability &ability : abilities)
    {
        rows.push_back(Row(ability));
        const std::string element = "ability-" + ability.id;
        if (ability.state == "ready")
            input.On(element, "click", [this, launch, ability](const Event &) {
                open_ = false;
                launch(ability);
                hud_.Rebuild();
            });
        else if (ability.state == "blocked")
            input.Ignore(element, ""); // disabled: the press behaviour toasts its reason
    }
    view.sheet = "s-hud-abilities";
    view.modal = true;
    // beside the button, wherever «Под левую руку» puts it
    view.sheet_props = Json{{"title", hud_.Text("hud.ability.group")}, {"place", place_}, {"mirrored", hud_.Mirrored()}, {"rows", std::move(rows)}};
    input.On("sheet", "close", [this](const Event &) {
        open_ = false;
        hud_.Rebuild();
    });
}

bool AbilityControls::Back()
{
    if (!open_)
        return false;
    open_ = false;
    hud_.Rebuild();
    return true;
}

} // namespace storm::ui::hud
