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

Json AbilityControls::Button(const Ability &ability)
{
    // The game's own name (the command's note); the pack's only when the game gave none.
    const std::string name = ability.label.empty() ? hud_.Text("hud.ability.name." + ability.id) : ability.label;
    Json button = {{"icon", IconOf(ability.id)}, {"label", name}};
    const std::string seconds = std::to_string(std::max(1, ability.left));
    // Running and recovering keep the plate: the fill and the seconds must stay readable (the
    // disabled look greys and halves the whole button); a tap says it in the event log instead
    // (Tell). Blocked has nothing to read: the disabled look, its reason toasted on a tap.
    if (ability.state == "active")
        button.update(Json{{"fill", std::lround(ability.fill * 100.0)}, {"fillTone", "active"}, {"reloading", true}, {"badge", seconds}});
    else if (ability.state == "recovery")
        button.update(Json{{"fill", std::lround(ability.fill * 100.0)}, {"fillTone", "cooldown"}, {"reloading", true}, {"badge", seconds}});
    else if (ability.state == "blocked")
        button.update(Json{{"disabled", true}, {"reason", name + ": " + hud_.Text("hud.ability.blocked." + ability.id)}});
    return button;
}

void AbilityControls::Build(const std::vector<Ability> &abilities, View &view, Input &input,
                            const std::function<void(const Ability &)> &launch)
{
    const auto bind = [&](const std::string &element, const Ability &ability) {
        view.elements[element] = Button(ability);
        if (ability.state == "ready")
            input.On(element, "click", [this, launch, ability](const Event &) {
                open_ = false;
                launch(ability);
                hud_.Rebuild();
            });
        else if (ability.state == "blocked")
            input.Ignore(element, "click"); // disabled: the press behaviour toasts its reason
        else
            input.On(element, "click", [this, ability](const Event &) { Tell(ability); });
    };
    if (abilities.empty())
    {
        open_ = false;
        return;
    }
    if (abilities.size() == 1)
    {
        open_ = false;
        bind("abilities", abilities.front());
        return;
    }
    // More than one: a button that fans them out. It tells what waits inside: how many are
    // ready (the badge) and the effect that runs (the fill).
    size_t ready = 0;
    double active = 0.0;
    for (const Ability &ability : abilities)
    {
        ready += ability.state == "ready";
        if (ability.state == "active")
            active = std::max(active, ability.fill);
    }
    Json group = {{"icon", "actions"}, {"label", hud_.Text("hud.ability.group")}, {"variant", open_ ? "active" : "normal"}};
    if (ready > 0)
        group["badge"] = std::to_string(ready);
    if (active > 0.0)
        group.update(Json{{"fill", std::lround(active * 100.0)}, {"fillTone", "active"}, {"reloading", true}});
    view.elements["abilities"] = std::move(group);
    input.On("abilities", "click", [this](const Event &) {
        open_ = !open_;
        until_ = hud_.Now() + kFanSeconds;
        hud_.Rebuild();
    });
    if (!open_)
        return;
    for (size_t i = 0; i < abilities.size() && i < kFanSlots; ++i)
        bind("ability-" + std::to_string(i + 1), abilities[i]);
}

// A tap on a running or recovering ability: how long it still runs or when it is back, in the
// scene's event log (the line the game's own messages use).
void AbilityControls::Tell(const Ability &ability)
{
    const std::string name = ability.label.empty() ? hud_.Text("hud.ability.name." + ability.id) : ability.label;
    const std::string seconds = std::to_string(std::max(1, ability.left));
    const char *key = ability.state == "active" ? "hud.ability.active" : "hud.ability.recovery";
    // the number and its unit never part at a line break (U+00A0)
    if (!hud_.Notice(name + ": " + hud_.Text(key) + " " + seconds + "\u00a0" + hud_.Text("hud.ability.seconds"), "info"))
        hud_.Log("abilities: no event log in this scene to say when " + ability.id + " is back");
}

void AbilityControls::Expire(double now, double &next)
{
    if (!open_)
        return;
    if (now >= until_)
    {
        open_ = false;
        hud_.Rebuild();
    }
    else
        next = std::min(next, until_ - now);
}

} // namespace storm::ui::hud
