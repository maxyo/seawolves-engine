#pragma once
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace storm::android
{
// Game-thread owned. Java only receives immutable JSON and supplies a bounded
// mailbox through pollMobileAction; no script/entity pointers cross threads.
// Script control groups can lag entity Execute by a frame. For the two ordinary
// land groups, the verified Player owns stance; actual group bindings still own
// capabilities. Modal, scene and player guards are resolved before this policy.
struct GameplayContext
{
    std::string_view group;
    // blocked: a command menu (ComState) or a video takes the controls; video says which.
    bool blocked = false, video = false, sea = false, land = false, naval = false, worldmap = false;
    bool location = false; // a land location is loaded (land: its interface is started too)
    std::optional<bool> nativeFight;
};
inline std::string_view gameplayMode(const GameplayContext &context)
{
    if (context.blocked) return "unknown";
    if (context.worldmap)
        return context.group == "WorldMapControls" ? "worldmap" : "unknown";
    if (context.sea)
        return context.group == "Sailing1Pers" || context.group == "Sailing3Pers" ?
            (context.naval ? "naval" : "sailing") : "unknown";
    if (!context.land || !context.nativeFight.has_value()) return "unknown";
    if (context.group == "PrimaryLand" || context.group == "FightModeControls")
        return *context.nativeFight ? "melee" : "explore";
    return "unknown";
}
// Why gameplayMode is "unknown" (GameplaySnapshot::blocked): the touch UI decides from it
// whether the screen shows anything the player can use (storm::ui Host::WantsTouch).
inline std::string_view gameplayBlocked(const GameplayContext &context)
{
    if (context.blocked) return context.video ? "video" : "commands";
    if (context.worldmap) return context.group == "WorldMapControls" ? "" : "group";
    if (context.sea) return context.group == "Sailing1Pers" || context.group == "Sailing3Pers" ? "" : "group";
    if (!context.land) return context.location ? "no-land-interface" : "no-scene";
    if (!context.nativeFight.has_value()) return "no-player";
    if (context.group == "PrimaryLand" || context.group == "FightModeControls") return "";
    return "group";
}
struct GameplaySnapshot
{
    std::string mode = "unknown", language, group, scene;
    std::vector<std::string> capabilities;
    // Stable only for one native land Player in one loaded scene. Android may
    // retain locomotion across stance revisions only when this nonempty token matches.
    std::string inputOwner;
    bool analogMovement = false;
    uint64_t movementGeneration = 0;
    // Mode "unknown" only: why. starting (the engine or its scripts not up yet), paused (the
    // app not in front), loading (the loading screen), transition (a fader is up: a scene change,
    // or the fade in after a load), interface (one open or launched),
    // dialogue, cutscene (the hero is an actor), video, commands (a command menu), no-scene
    // (no land, sea or world map yet), no-land-interface (a location whose land interface is not
    // started), no-player, group (a control group no mode knows),
    // error (the scripts could not be read).
    std::string blocked;
    bool operator==(const GameplaySnapshot &) const = default;
};
inline std::string gameplayJsonString(std::string_view value)
{
    std::string out = "\"";
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c >= 32) out += c;
    }
    return out + '"';
}
// An active ability as a touch HUD shows it, at sea (android_ship_abilities.hpp) and on land
// (battle_interface land_status.hpp): all of it read from the scripts' own state.
//   id     the perk (ChrPerksList) and BattleInterface / land interface command name
//   label  the game's own name of it (the command's note: AbilityDescribe.txt)
//   state  ready (the scripts enable its command now), active (its effect runs), recovery (it
//          was used and its delay runs), blocked (owned, not usable now)
//   fill   active: what is left of the effect; recovery: how far it has recovered (0…1)
//   left   active / recovery: whole seconds the scripts still count (perks.c procChrPerkDelay)
struct BattleAbility
{
    enum State { Ready, Active, Recovery, Blocked };
    std::string id, label;
    State state = Blocked;
    double fill = 0.0;
    int left = 0;
    bool operator==(const BattleAbility &) const = default;
};
// The state of a perk ability from the scripts' counters (perks.c ActivateCharacterPerk sets
// perks.list.<X>.active = ChrPerksList TimeDuration and .delay = TimeDelay, procChrPerkDelay counts
// both down once a second and deletes them at 0): absent counters are std::nullopt. `enabled` is
// the scripts' own verdict on the command (its .enable), asked only when neither counter runs.
inline void battleAbilityTiming(BattleAbility &ability, std::optional<double> active, std::optional<double> delay,
                                std::optional<double> duration, std::optional<double> cooldown, bool enabled)
{
    const auto seconds = [](double value) { return static_cast<int>(std::ceil(std::clamp(value, 0.0, 86400.0))); };
    ability.fill = 0.0;
    ability.left = 0;
    if (active && *active > 0 && duration && *duration > 0)
    {
        ability.state = BattleAbility::Active;
        ability.fill = std::clamp(*active / *duration, 0.0, 1.0); // what is left of the effect
        ability.left = seconds(*active);
    }
    else if (delay && *delay > 0 && cooldown && *cooldown > 0)
    {
        ability.state = BattleAbility::Recovery;
        ability.fill = std::clamp(1.0 - *delay / *cooldown, 0.0, 1.0); // how far it has recovered
        ability.left = seconds(*delay);
    }
    else
        ability.state = enabled ? BattleAbility::Ready : BattleAbility::Blocked;
    ability.fill = std::round(ability.fill * 100.0) / 100.0;
}
inline std::string battleAbilitiesJson(const std::vector<BattleAbility> &abilities)
{
    static constexpr const char *states[] = {"ready", "active", "recovery", "blocked"};
    std::string out = "[";
    for (size_t i = 0; i < abilities.size(); ++i)
    {
        const auto &ability = abilities[i];
        out += std::string(i ? "," : "") + "{\"id\":" + gameplayJsonString(ability.id) + ",\"label\":" + gameplayJsonString(ability.label) +
               ",\"state\":\"" + states[ability.state] + "\",\"fill\":" + std::to_string(ability.fill) + ",\"left\":" + std::to_string(ability.left) + "}";
    }
    return out + "]";
}
class GameplayState
{
    struct Action { uint64_t revision; std::string control; uint32_t tick; };
    // Survives SDL_main restart in the same process/Activity.
    inline static std::atomic<uint64_t> nextRevision_{0};
    GameplaySnapshot snapshot_;
    uint64_t revision_ = 0;
    std::deque<Action> pending_;
public:
    bool update(GameplaySnapshot snapshot)
    {
        if (revision_ && snapshot_ == snapshot) return false;
        snapshot_ = std::move(snapshot);
        revision_ = nextRevision_.fetch_add(1, std::memory_order_relaxed) + 1;
        pending_.clear();
        return true;
    }
    uint64_t revision() const { return revision_; }
    const GameplaySnapshot &snapshot() const { return snapshot_; }
    bool allows(std::string_view control) const
    {
        return std::find(snapshot_.capabilities.begin(), snapshot_.capabilities.end(), control) != snapshot_.capabilities.end();
    }
    std::string json() const
    {
        std::string result = "{\"schemaVersion\":1,\"revision\":" + std::to_string(revision_) +
            ",\"mode\":" + gameplayJsonString(snapshot_.mode) + ",\"language\":" + gameplayJsonString(snapshot_.language) +
            ",\"inputOwner\":" + gameplayJsonString(snapshot_.inputOwner) +
            ",\"analogMovement\":" + (snapshot_.analogMovement ? "true" : "false") +
            ",\"movementGeneration\":" + std::to_string(snapshot_.movementGeneration) +
            ",\"blocked\":" + gameplayJsonString(snapshot_.blocked) +
            ",\"capabilities\":[";
        for (size_t i = 0; i < snapshot_.capabilities.size(); ++i)
        {
            if (i) result += ',';
            result += gameplayJsonString(snapshot_.capabilities[i]);
        }
        return result + "]}";
    }
    // Everything json() reads (HudChannel, android_ui.hpp): every change of the snapshot is a new revision.
    auto publicationKey() const { return std::tie(revision_); }
    bool offer(std::string_view request, uint32_t tick)
    {
        if (pending_.size() >= 8 || request.size() > 96) return false;
        const auto tab = request.find('\t');
        if (tab == std::string_view::npos) return false;
        uint64_t revision = 0;
        const auto parsed = std::from_chars(request.data(), request.data() + tab, revision);
        if (parsed.ec != std::errc() || parsed.ptr != request.data() + tab || revision != revision_) return false;
        const auto control = request.substr(tab + 1);
        // Only controls consumed by the script's Control Activation handler.
        // Locomotion and melee controls are polled/held and must use normal input.
        if (control != "Ship_Fire" && control != "Sea_CameraSwitch" &&
            control != "hk_charge1" && control != "hk_charge2" &&
            control != "hk_charge3" && control != "hk_charge4" &&
            control != "TimeScaleX1" && control != "TimeScaleX2" && control != "TimeScaleX4" && control != "TimeScaleX8" &&
            control != "BI_ImmediateReload" && control != "BI_LightRepair" && control != "BI_InstantRepair" &&
            control != "BI_Turn180" && // the ship abilities (android_ship_abilities.hpp ShipAbilityNames)
            control != "TelescopeIn" && control != "TelescopeOut") return false; // the spyglass: raise, lower (main.cpp)
        if (!allows(control)) return false;
        pending_.push_back({revision, std::string(control), tick});
        return true;
    }
    std::optional<std::string> take(uint32_t tick)
    {
        while (!pending_.empty())
        {
            auto action = std::move(pending_.front());
            pending_.pop_front();
            if (action.revision == revision_ && tick - action.tick <= 1500 && allows(action.control))
                return action.control;
        }
        return std::nullopt;
    }
};
} // namespace storm::android
