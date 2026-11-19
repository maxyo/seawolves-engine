// The land HUD: exploring and fighting on foot, and the land action sheet.
//
// Layout scenes `land` (explore) and `land-combat` (melee) of hud-layout.json. Input goes
// back through the engine's existing entry points:
//   move:1\t<inputOwner>\t<movementGeneration>\t<sequence>\t<active>\t<x>\t<forward>   (±1000)
//   combat:2\t<activityGeneration>\t<actionGeneration>\t<sequence>\t<action>\t<tap|down|up|cancel>
//     (the action's generation from the combat state; 0 for "all\tcancel")
//   landcommands:1\t<sceneEpoch>\t<revision>\t<activityGeneration>\t<verb>\t<id>
//   context:<action name>          shortcut:<mainmenu|character|characterscreen>
#include "hud.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

namespace storm::ui::hud
{

namespace
{

constexpr double kIdle = 3600.0;
constexpr double kChoiceSeconds = 1.5;    // a tapped row waits this long for the engine to select it
// The bottom hint (hud-layout.json "hint"): once what it names is gone it stays kHintHold more
// (a source that comes straight back, a reload locator re-entered at its edge, does not make it
// blink), then fades out for kHintLeave (pack time.toastLeave) and goes.
constexpr double kHintHold = 0.5, kHintLeave = 0.22;

using screens::StringKey;

// `label` is a key of the pack's string table ("" = the engine supplies the text).
struct Named
{
    const char *key, *icon, *label;
};

// Sword moves: the wheel sectors clockwise from the top left, then the centre. The strike
// button's swipe reports these keys (hud-layout.json: data swipe-ids, swipe-centre).
const Named kMoves[] = {
    {"feint", "feint", StringKey("hud.move.feint")},
    {"round", "attack-round", StringKey("hud.move.round")},
    {"break", "attack-pierce", StringKey("hud.move.break")},
    {"parry", "parry", StringKey("hud.move.parry")},
    {"force", "attack-heavy", StringKey("hud.move.force")},
    {"fast", "attack-fast", StringKey("hud.move.fast")},
};

// The script's quick action (BattleInterface "Log_SetActiveAction") in the context slot.
const Named kContext[] = {
    {"reload", "door", StringKey("hud.context.enter")},      {"pick", "hand", StringKey("hud.context.take")},          {"openbox", "chest", StringKey("hud.context.open")},
    {"entertosea", "boat", StringKey("hud.context.toSea")}, {"entertoship", "boat", StringKey("hud.context.toShip")}, {"action", "hand", StringKey("hud.context.action")},
};

// The text with its first letter capital (the game's LocLables write places in lower case:
// "таверна"); Latin and Cyrillic, UTF-8.
std::string Capitalized(std::string text)
{
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z')
        text[0] = static_cast<char>(text[0] - 'a' + 'A');
    else if (text.size() >= 2)
    {
        const auto first = static_cast<unsigned char>(text[0]), second = static_cast<unsigned char>(text[1]);
        const unsigned code = ((first & 0x1Fu) << 6) | (second & 0x3Fu);
        unsigned upper = code;
        if (code >= 0x430 && code <= 0x44F) // а…я
            upper = code - 0x20;
        else if (code >= 0x450 && code <= 0x45F) // ѐ…џ (ё)
            upper = code - 0x50;
        if ((first & 0xE0u) == 0xC0u && upper != code)
        {
            text[0] = static_cast<char>(0xC0u | (upper >> 6));
            text[1] = static_cast<char>(0x80u | (upper & 0x3Fu));
        }
    }
    return text;
}

// Rows of the action sheet by the script command behind them.
const Named kCommands[] = {
    {"BI_FastReload", "travel", ""},  {"BI_OutDoor", "door", ""},     {"BI_ItemsUse", "potion", ""},   {"BI_DialogStart", "talk", ""},
    {"BI_ItemsChange", "exchange", ""}, {"BI_TakeItem", "hand", ""},  {"BI_PlaceItem", "put", ""},     {"BI_UseBox", "chest", ""},
    {"BI_DeadBox", "search", ""},     {"BI_TalkSelf", "thought", ""}, {"BI_SelfRepair", "repair", ""}, {"BI_Alchemy", "retort", ""},
    {"BI_MapAtlas", "map", ""},       {"BI_ActivateRush", "energy", ""}, {"BI_Exit_Deck", "boat", ""},
};

// The land abilities by the land menu's command that launches each (landinterface.c); `icon` holds
// the command's event.
const Named kAbilityCommands[] = {{"Rush", "BI_ActivateRush", ""}};

// Root commands that are transitions: they have their own button (the boot), not rows of the
// actions sheet.
const char *const kTransitions[] = {"BI_FastReload", "BI_OutDoor"};
constexpr const char *kTravelCommand = "BI_FastReload";

bool IsTransition(const std::string &event)
{
    return std::any_of(std::begin(kTransitions), std::end(kTransitions), [&](const char *known) { return event == known; });
}

} // namespace

// Places of the travel stage by the game's own picture of the place: the cell of
// battle_interface\LandTarget_SLB the menu shows (fast_reload_table.c: pic = FRP_*; the ship's
// port icon is cell 10). The slot name "fr_<n>" is only the place's position in its town's table
// and says nothing about what the place is.
const char *LandPlaceIcon(const std::string &picture)
{
    static const struct
    {
        const char *picture, *icon;
    } kPlaces[] = {{"landtarget_slb:0", "store"},     {"landtarget_slb:1", "shipyard"}, {"landtarget_slb:2", "tavern"},
                   {"landtarget_slb:3", "usurer"},    {"landtarget_slb:4", "residence"}, {"landtarget_slb:5", "prison"},
                   {"landtarget_slb:6", "church"},    {"landtarget_slb:7", "fort"},      {"landtarget_slb:8", "portoffice"},
                   {"landtarget_slb:9", "lantern"},   {"landtarget_slb:10", "port"}};
    for (const auto &place : kPlaces)
        if (picture == place.picture)
            return place.icon;
    return nullptr;
}

const char *LandTalkIcon(const std::string &dialog)
{
    // The roles of the game's people (PROGRAM/dialogs: the dialog file each kind of character
    // talks through), with the place icons of the transitions where the role is a place's keeper.
    static const struct
    {
        const char *dialog, *icon;
    } kRoles[] = {{"common_store", "store"},         {"common_itemtrader", "store"},     {"common_ammo", "ammo"},
                  {"common_shipyard", "shipyard"},   {"common_tavern", "tavern"},        {"waitress_dialog", "tavern"},
                  {"usurer_dialog", "usurer"},       {"common_mayor", "residence"},      {"common_governor", "residence"},
                  {"common_church", "church"},       {"common_portman", "portoffice"},   {"common_brothel", "lantern"},
                  {"common_prison", "prison"},       {"common_lighthouse", "port"},      {"enc_officer_dialog", "officer"},
                  {"smuggler agent_dialog", "sack"}, {"common_soldier", "musket"},       {"common_fort", "musket"}};
    for (const auto &role : kRoles)
        if (dialog == role.dialog)
            return role.icon;
    return "talk";
}

namespace
{

class LandScene final : public Scene
{
  public:
    explicit LandScene(Hud &hud) : hud_(hud)
    {
    }

    void Build(const States &states, View &view, Input &input) override
    {
        const Gameplay &gameplay = states.gameplay;
        const Combat &combat = states.combat;
        const bool melee = gameplay.mode == "melee";
        const bool ready = Ready(states);
        view.layout = melee ? "land-combat" : "land";
        // The stick's events are the scene's whether or not this view shows the stick: a finger that was on it when
        // the command sheet opened lifts later, and that release (the stick back at rest) must stop the hero.
        if (gameplay.analog_movement)
            input.On("move", "change", [this](const Event &event) { Stick(event.value); });

        if (states.vitals.known)
        {
            // The effects row under the bars (V2-Land-Explore: small state icons in one row).
            static const struct
            {
                const char *effect, *icon, *tone, *label;
            } kEffects[] = {{"poison", "skull", "danger", StringKey("hud.effect.poison")},
                            {"drunk", "rum", "effect", StringKey("hud.effect.drunk")},
                            {"healing", "potion", "active", StringKey("hud.effect.healing")},
                            // The game makes him walk at a full stick: the load, or a quest.
                            {"overload", "weight", "danger", StringKey("hud.effect.overload")},
                            {"cantrun", "lock", "effect", StringKey("hud.effect.cantrun")}};
            Json effects = Json::array();
            for (const auto &known : kEffects)
                if (std::find(states.vitals.effects.begin(), states.vitals.effects.end(), known.effect) != states.vitals.effects.end())
                    effects.push_back(Json{{"icon", known.icon}, {"fill", 100}, {"fillTone", known.tone}, {"label", hud_.Text(known.label)}});
            if (!effects.empty())
                view.elements["effects"] = Json{{"states", effects}};
            BuildParty(states, melee, view, input);
        }

        const LandCommands &commands = states.land_commands;
        if (commands.known && commands.active)
        {
            abilities_.Close(); // the land menu (also the one Rush opens) takes the abilities row's place
            if (commands.complete)
            {
                BuildSheet(commands, view, input);
                return;
            }
            // The engine marked its menu as something the sheet cannot show. The engine never
            // draws its own menu under the touch HUD, so an invisible menu would hold the
            // player: close it and report it.
            hud_.Error("land commands: the engine's menu is not drawable here (stage " + commands.stage + "); closed");
            hud_.Send("landcommands", Owner(commands) + "\tclose\t");
            return;
        }
        choice_.clear();

        const std::string place = Place(states.world_info);
        if (!place.empty())
            view.elements["location"] = Json{{"text", place}};
        view.log = true;
        const Json lines = hud_.LogLines();
        if (!lines.empty())
            view.elements["log"] = Json{{"lines", lines}};

        view.elements["hero"] = Json{{"disabled", melee}};
        input.On("hero", "click", [this](const Event &) { hud_.Send("shortcut", "character"); });
        view.elements["pause"] = Json::object();
        input.On("pause", "click", [this](const Event &) { hud_.Send("shortcut", "mainmenu"); });

        if (gameplay.analog_movement)
            view.elements["move"] = Json::object();
        else
            hud_.Error("land HUD: the engine offers no analog movement (inputOwner or movementGeneration missing); no move stick");

        // Bottom right, where the potion button was: one button with the potion and Rush beside it
        // (hud.h AbilityControls): the potion while the gameplay offers it, Rush in a fight while the
        // scripts offer it or it runs (land_status.hpp).
        const bool potion = combat.Has("potion");
        const std::vector<Ability> rush = melee && states.vitals.known ? states.vitals.abilities : std::vector<Ability>();
        if (!potion && rush.empty())
            abilities_.Close();
        // The menu split: the actions at the top, as the sea orders; the
        // transitions under the thumb where the actions were, the boot (no transitions in a fight).
        if (commands.known && commands.available)
        {
            const auto &offered = commands.offered;
            if (std::any_of(offered.begin(), offered.end(), [](const std::string &event) { return !IsTransition(event); }))
            {
                view.elements["actions"] = Json{{"label", hud_.Text("hud.sheet.actions")}};
                input.On("actions", "click", [this](const Event &) { Commands("open", {}); });
            }
            // the open row takes the travel button's place (hud-layout.json "replaces")
            if (!melee && !abilities_.Open() && std::find(offered.begin(), offered.end(), kTravelCommand) != offered.end())
            {
                view.elements["travel"] = Json{{"label", hud_.Text("hud.travel")}};
                input.On("travel", "click", [this](const Event &) { Commands("open", kTravelCommand); });
            }
        }
        if (combat.Has("weapon"))
        {
            view.elements["weapon"] = Json{{"disabled", !ready}, {"label", melee ? hud_.Text("hud.weapon.sheathe") : hud_.Text("hud.weapon.draw")}};
            input.On("weapon", "click", [this](const Event &) { Strike("weapon", "tap"); });
        }

        if (potion || !rush.empty())
        {
            std::vector<Fan::Option> lead;
            if (potion)
            {
                // The potion with the count carried, greyed with nothing to drink; a potion still
                // healing fills it (V2-Land-Combat). A tap drinks, as the game's potion key.
                const bool none = states.vitals.known && states.vitals.potions == 0;
                Fan::Option drink{Json{{"icon", "potion"}, {"label", hud_.Text("hud.potion")}, {"disabled", !ready || none}}, {}};
                if (states.vitals.known)
                {
                    drink.props["badge"] = std::to_string(states.vitals.potions);
                    if (std::find(states.vitals.effects.begin(), states.vitals.effects.end(), "healing") != states.vitals.effects.end())
                        drink.props["fill"] = 100, drink.props["fillTone"] = "active";
                }
                if (ready && !none)
                    drink.pick = [this] { Strike("potion", "tap"); };
                lead.push_back(std::move(drink));
            }
            // Rush launches through the land menu's own command, as its row.
            abilities_.Build(rush, view, input, [this](const Ability &ability) {
                for (const Named &command : kAbilityCommands)
                    if (ability.id == command.key)
                        Commands("open", command.icon);
            }, std::move(lead));
        }
        if (melee)
        {
            BuildCombat(states, ready, view, input);
            DropHint(); // the combat layout has no hint (V2-Land-Combat): it does not linger into a fight
        }
        else if (const Context context = BuildContext(states, ready, view, input); context.button && !context.hint)
            DropHint(); // a button the hint has no words for: a hint of what the button did before must not stay
        else
            BuildHint(context.button ? context.hint : ready ? LockedDoorHint(states) : std::nullopt, view);
        if (abilities_.Open())
            view.elements.erase("hint"); // the open row takes its place (hud-layout.json "replaces"); it comes back folded
    }

    double Tick(const States &states, double now) override
    {
        double next = kIdle;
        if (moving_ || x_ != 0 || y_ != 0)
            SyncMove(states.gameplay);
        abilities_.Expire(now, next);
        if (!choice_.empty() && now >= choice_until_)
        {
            choice_.clear();
            hud_.Rebuild();
        }
        else if (!choice_.empty())
            next = std::min(next, choice_until_ - now);
        if (hint_ && hint_lost_ >= 0.0)
        {
            // the lost hint's two steps: it starts to fade, then it goes
            const double fade = hint_lost_ + kHintHold, gone = fade + kHintLeave;
            if ((now >= fade && !hint_leaving_) || now >= gone)
                hud_.Rebuild();
            else
                next = std::min(next, (now < fade ? fade : gone) - now);
        }
        return next;
    }

    void Drawn(const States &states) override
    {
        // A tapped row goes on once the sheet showing the menu's new revision is on the screen
        // (select, then activate against the revision that reports the selection). The engine
        // never draws its own menu under the touch HUD (android_hud_presenter.hpp).
        const LandCommands &commands = states.land_commands;
        if (commands.known && commands.active && commands.complete && hud_.Interactive() && hud_.Shown().modal)
            Follow(commands);
    }

    void Released(const States &states) override
    {
        x_ = y_ = 0;
        SyncMove(states.gameplay);
        wheel_open_ = false;
        move_.clear();
        if (states.combat.known && states.combat.mode != "unknown")
            Fight(states.combat, "all", "cancel");
        hud_.Rebuild();
    }

    void Hidden(const States &) override
    {
        abilities_.Close();
    }

    void Suspended(const States &states) override
    {
        const LandCommands &commands = states.land_commands;
        if (commands.known && commands.active)
            hud_.Send("landcommands", Owner(commands) + "\tclose\t");
        choice_.clear();
        abilities_.Close();
    }

    bool Holds(const States &states) const override
    {
        return states.land_commands.known && states.land_commands.active; // the open menu blocks gameplay
    }

    bool Back(const States &states) override
    {
        if (abilities_.Back())
            return true;
        const LandCommands &commands = states.land_commands;
        if (!commands.known || !commands.active || !commands.complete)
            return false;
        // The travel stage opens from its own button: nothing to go back to.
        Commands(commands.stage == "commands" || commands.stage == "travel" || !commands.can_back ? "close" : "back", {});
        return true;
    }

  private:
    static bool Ready(const States &states)
    {
        return states.combat.known && states.combat.gameplay_revision == states.gameplay.revision && states.combat.mode == states.gameplay.mode;
    }

    // The location line names the place only: town · place. The engine's text lists island,
    // town and place and then the date and the time, one per line, without saying which is
    // which; the calendar lines are the ones with digits, and the last two names are the
    // most local ones.
    static std::string Place(const std::string &lines)
    {
        std::vector<std::string> names;
        for (size_t at = 0; at <= lines.size();)
        {
            size_t end = lines.find('\n', at);
            if (end == std::string::npos)
                end = lines.size();
            const std::string line = lines.substr(at, end - at);
            if (!line.empty() && line.find_first_of("0123456789") == std::string::npos)
                names.push_back(line);
            at = end + 1;
        }
        if (names.empty())
            return {};
        return names.size() == 1 ? names.back() : names[names.size() - 2] + " · " + names.back();
    }

    static std::string Owner(const LandCommands &commands)
    {
        return "1\t" + std::to_string(commands.scene) + "\t" + std::to_string(commands.revision) + "\t" + std::to_string(commands.activity);
    }

    // --- the party ---

    // The portrait of a party member: the game's 128 px face by FaceId. Without a FaceId the
    // land interface draws its generic figure (landinterface.c: two.tga), which the engine sends
    // as the icon's picture when the game has that file: shown the same; without it the game
    // has no picture either and the neutral placeholder stays. Both are said once per person.
    Picture Face(const std::string &face, const Picture &icon, const std::string &name, int character)
    {
        if (!face.empty())
            return {"INTERFACES/PORTRAITS/128/face_" + face, {}};
        hud_.Log("land HUD: " + name + " (character " + std::to_string(character) + ") has no FaceId: " +
                 (icon.texture.empty() ? "the game's two.tga fallback is missing, the placeholder stays" : "the game's own figure " + icon.texture));
        return icon;
    }

    // The party row (as the original land interface's icons): the hero first, then the officers
    // who walk with him, each the round face portrait ringed by health and energy. A tap opens
    // the character screen on that person; the game refuses that screen in a fight, so a tap
    // there does nothing.
    void BuildParty(const States &states, bool melee, View &view, Input &input)
    {
        const Vitals &vitals = states.vitals;
        const bool tap = !melee && vitals.hero >= 0;
        Json members = Json::array();
        members.push_back(MemberCell(vitals.name, Face(vitals.face, vitals.picture, vitals.name, vitals.hero), {{"health", vitals.health}, {"energy", vitals.energy}}, tap));
        for (const Vitals::Officer &officer : vitals.officers)
            members.push_back(MemberCell(officer.name, Face(officer.face, officer.picture, officer.name, officer.id), {{"health", officer.health}, {"energy", officer.energy}}, tap));
        view.elements["party"] = Json{{"officers", members}};
        if (!tap)
            return;
        input.On("party", "member", [this](const Event &event) {
            const Vitals &now = hud_.Current().vitals;
            const size_t index = static_cast<size_t>(std::max(0, std::atoi(event.value.c_str())));
            if (!now.known || index > now.officers.size())
                return;
            hud_.RequestSubject({"character", index == 0 ? now.hero : now.officers[index - 1].id});
            hud_.Send("shortcut", "characterscreen");
        });
    }

    // --- context slot (explore) and the bottom hint ---

    struct Hint
    {
        std::string icon, text;
    };

    // The context button and the hint that says what it does, from one place: the hint never
    // names another target than the button acts on (a guard beside the door the button opens).
    struct Context
    {
        bool button = false;
        std::optional<Hint> hint;
    };

    Context BuildContext(const States &states, bool ready, View &view, Input &input)
    {
        if (!ready)
            return {};
        if (states.combat.Has("interact"))
        {
            // The engine's action key: somebody to talk to stands in front of the hero, or the game
            // waits for it to go on after a won boarding fight (it outranks the script's icon).
            const std::string &kind = states.combat.interact_kind;
            const bool talk = kind == "dialog", boarding = kind == "boarding";
            const std::string label = hud_.Text(talk ? "hud.context.talk" : boarding ? "hud.context.boarding" : "hud.context.action");
            view.elements["context"] = Json{{"icon", talk ? "talk" : boarding ? "grapple" : "hand"}, {"label", label}};
            input.On("context", "click", [this](const Event &) { Strike("interact", "tap"); });
            if (!talk)
                return {true, std::nullopt};
            const std::string &name = states.combat.interact_name;
            return {true, Hint{LandTalkIcon(states.combat.interact_dialog), name.empty() ? label : label + ": " + name}};
        }
        const std::string action = Lower(states.context_action);
        // "Talk" belongs to the engine's target query above, never to the cached script icon.
        if (action.empty() || action == "talk" || action == "nothing")
            return {};
        const Named *known = nullptr;
        for (const Named &entry : kContext)
            if (action == entry.key)
                known = &entry;
        if (!known)
        {
            // Documented fallback: an action name this table lacks still gets its button,
            // with the generic look, because the engine accepts any name it published.
            hud_.Log("land HUD: context action '" + states.context_action + "' has no icon of its own");
            known = &kContext[std::size(kContext) - 1];
        }
        const std::string label = hud_.Text(known->label);
        view.elements["context"] = Json{{"icon", known->icon}, {"label", label}};
        input.On("context", "click", [this, name = states.context_action](const Event &) { hud_.Send("context", name); });
        // A door: where it leads (an open lock); a door, a thing or a chest the game names no
        // place or object for: the button's own words and icon.
        if (action == "reload" && !states.door.place.empty())
            return {true, Hint{"unlock", Capitalized(states.door.place)}};
        if (action == "reload" || action == "pick" || action == "openbox")
            return {true, Hint{known->icon, label}};
        return {true, std::nullopt};
    }

    // No button: a door the game keeps closed now still says where it leads, with a closed lock.
    static std::optional<Hint> LockedDoorHint(const States &states)
    {
        if (states.door.locked && !states.door.place.empty())
            return Hint{"lock", Capitalized(states.door.place)};
        return std::nullopt;
    }

    void DropHint()
    {
        hint_.reset();
        hint_lost_ = -1.0;
        hint_leaving_ = false;
    }

    // Shown at once and exactly while it applies; a lost one holds, fades and goes (kHintHold).
    void BuildHint(const std::optional<Hint> &wanted, View &view)
    {
        const double now = hud_.Now();
        if (wanted)
        {
            hint_ = wanted;
            hint_lost_ = -1.0;
        }
        else if (hint_ && hint_lost_ < 0.0)
            hint_lost_ = now;
        if (hint_ && hint_lost_ >= 0.0 && now >= hint_lost_ + kHintHold + kHintLeave)
            hint_.reset();
        hint_leaving_ = hint_ && hint_lost_ >= 0.0 && now >= hint_lost_ + kHintHold;
        if (hint_)
            view.elements["hint"] = Json{{"icon", hint_->icon}, {"text", hint_->text}, {"leaving", hint_leaving_}};
    }

    // --- combat set ---

    void BuildCombat(const States &states, bool ready, View &view, Input &input)
    {
        const Combat &combat = states.combat;
        Json moves = Json::array();
        bool any = false;
        std::string chosen = "centre", caption;
        for (size_t i = 0; i < std::size(kMoves); ++i)
        {
            const Named &move = kMoves[i];
            const bool offered = combat.Has(move.key);
            any = any || offered;
            // The last move sits in the centre of the wheel, the others in its sectors.
            const std::string sector = i + 1 == std::size(kMoves) ? "centre" : std::to_string(i);
            moves.push_back(Json{{"id", move.key}, {"icon", move.icon}, {"sector", sector}, {"disabled", !offered}});
            if (move_ == move.key)
            {
                chosen = sector;
                caption = hud_.Text(move.label);
            }
        }
        if (any)
        {
            view.elements["strike"] = Json{{"disabled", !ready}};
            input.On("strike", "open", [this](const Event &event) { Wheel(true, event.value); });
            input.On("strike", "preview", [this](const Event &event) { Wheel(true, event.value); });
            input.On("strike", "cancel", [this](const Event &) { Wheel(false, {}); });
            input.On("strike", "commit", [this](const Event &event) {
                Wheel(false, {});
                Strike(event.value, "tap");
            });
            input.Ignore("strike", "click");
            if (wheel_open_ && ready)
            {
                view.elements["moves"] = Json{{"moves", moves}, {"selected", chosen}, {"caption", caption}};
                view.dimmed = {"strike", "block", "shot", "dash", "abilities", "weapon"};
            }
        }
        if (combat.Has("block"))
        {
            view.elements["block"] = Json{{"disabled", !ready}};
            input.On("block", "down", [this](const Event &) { Strike("block", "down"); });
            input.On("block", "up", [this](const Event &) { Strike("block", "up"); });
            input.Ignore("block", "click");
        }
        // The shot is always in the combat cluster once the engine binds it:
        // without a firearm equipped it is inactive, and a tap on it toasts the reason (the
        // press behaviour of a disabled button).
        if (combat.Has("pistol") && combat.pistol == "no_weapon")
            view.elements["shot"] = Json{{"disabled", true}, {"reason", hud_.Text("hud.combat.noFirearm")}};
        else if (combat.Has("pistol"))
        {
            const bool loading = combat.pistol == "unloaded";
            Json shot = {{"disabled", !ready || loading}, {"reloading", loading}};
            if (loading)
            {
                shot["fill"] = combat.pistol_progress * 5;
                shot["fillTone"] = "reload";
            }
            if (states.vitals.known && states.vitals.capacity > 0)
                shot["badge"] = std::to_string(states.vitals.loaded);
            view.elements["shot"] = std::move(shot);
            input.On("shot", "click", [this](const Event &) { Strike("pistol", "tap"); });
        }
        if (combat.Has("dash") && states.vitals.known && states.vitals.dash >= 0.0)
        {
            const bool cooling = states.vitals.dash > 0.0;
            Json dash = {{"disabled", !ready || cooling}};
            if (cooling)
                dash["fill"] = std::lround(states.vitals.dash * 100.0), dash["fillTone"] = "cooldown";
            view.elements["dash"] = std::move(dash);
            input.On("dash", "click", [this](const Event &) { Strike("dash", "tap"); });
        }
        // The enemy captain of a boarding cabin fight (V2-Land-Boarding-Cabin): name and health.
        if (states.vitals.known && !states.vitals.opponent.empty())
        {
            view.elements["opponent"] = Json{{"title", hud_.Text("hud.captain") + " " + states.vitals.opponent}};
            view.elements["opponent-hp"] = Json{{"value", std::lround(states.vitals.opponent_health * 100.0)}};
            // The enemy captain takes the place of the location line (hud-layout.json: the
            // opponent replaces the location; they never overlap).
            view.elements.erase("location");
        }
        // The boarding deck fight: our men and theirs as two bars facing
        // each other, each full at the fight's start, with the men left; the deck (the location line)
        // as its title. It takes the location line's place like the cabin's captain, who replaces it.
        else if (states.vitals.known && states.vitals.boarding)
        {
            const Vitals::Boarding &b = *states.vitals.boarding;
            const auto share = [](int men, int start) { return std::clamp<long>(std::lround(100.0 * men / start), 0, 100); };
            Json boarding = {{"ours", share(b.ours, b.ours_start)}, {"theirs", share(b.theirs, b.theirs_start)},
                             {"oursCount", b.ours}, {"theirsCount", b.theirs}};
            if (const auto location = view.elements.find("location"); location != view.elements.end())
                boarding["title"] = (*location)["text"];
            view.elements["boarding"] = std::move(boarding);
            view.elements.erase("location");
        }
        // Elements of a dimmed cluster that are not shown now cannot be dimmed.
        view.dimmed.erase(std::remove_if(view.dimmed.begin(), view.dimmed.end(), [&view](const std::string &id) { return !view.elements.contains(id); }),
                          view.dimmed.end());
    }

    void Wheel(bool open, const std::string &move)
    {
        if (wheel_open_ == open && move_ == move)
            return;
        wheel_open_ = open;
        move_ = move;
        hud_.Rebuild();
    }

    // Sends a combat action when the engine still offers it to the player the HUD shows.
    void Strike(const std::string &action, const std::string &phase)
    {
        const States &states = hud_.Current();
        const Combat &combat = states.combat;
        if (!hud_.Interactive() || !Ready(states) || !combat.Has(action))
            return;
        if (action == "pistol" && combat.pistol != "unknown" && combat.pistol != "loaded")
            return;
        Fight(combat, action, phase);
    }

    void Fight(const Combat &combat, const std::string &action, const std::string &phase)
    {
        if (combat.activity != combat_activity_)
        {
            combat_activity_ = combat.activity;
            combat_sequence_ = 0;
        }
        const auto offered = combat.actions.find(action);
        const uint64_t generation = offered == combat.actions.end() ? 0 : offered->second;
        hud_.Send("combat", "2\t" + std::to_string(combat.activity) + "\t" + std::to_string(generation) + "\t" +
                                std::to_string(++combat_sequence_) + "\t" + action + "\t" + phase);
    }

    // --- movement ---

    void Stick(const std::string &value)
    {
        const size_t comma = value.find(',');
        if (comma == std::string::npos)
        {
            hud_.Error("land HUD: the move stick reported '" + value + "', expected 'x,y'");
            return;
        }
        // The stick reports ±100 with y growing downwards; the engine takes ±1000, forward up.
        x_ = static_cast<int>(std::clamp(std::strtod(value.c_str(), nullptr), -100.0, 100.0) * 10.0);
        y_ = static_cast<int>(std::clamp(std::strtod(value.c_str() + comma + 1, nullptr), -100.0, 100.0) * -10.0);
        SyncMove(hud_.Current().gameplay);
    }

    // The engine holds a stick from its "1" packet until its "0" packet (events, no clock). So
    // every run of "1" packets ends with exactly one "0" in the session it was said in, whatever
    // the HUD does meanwhile: the thumb lifted while the HUD is not interactive, the HUD stopping
    // being interactive under a held thumb (a fader, a dialogue), a new movement session and the
    // scene's release all say it. (A session the engine has already ended drops it as stale.)
    // A thumb still down is said again once the HUD is interactive in a live session.
    void SyncMove(const Gameplay &gameplay)
    {
        const bool held = x_ != 0 || y_ != 0;
        const bool live = gameplay.analog_movement && hud_.Interactive();
        const bool same = gameplay.input_owner == move_owner_ && gameplay.movement_generation == move_generation_;
        if (moving_ && (!held || !live || !same))
        {
            moving_ = false;
            SendMove(false);
        }
        if (held && live && (!moving_ || x_ != said_x_ || y_ != said_y_))
        {
            moving_ = true;
            move_owner_ = gameplay.input_owner;
            move_generation_ = gameplay.movement_generation;
            said_x_ = x_;
            said_y_ = y_;
            SendMove(true);
        }
    }

    void SendMove(bool active)
    {
        hud_.Send("move",
                  "1\t" + move_owner_ + "\t" + std::to_string(move_generation_) + "\t" + std::to_string(++move_sequence_) + "\t" +
                      (active ? "1" : "0") + "\t" + std::to_string(active ? x_ : 0) + "\t" + std::to_string(active ? y_ : 0),
                  "move");
    }

    // --- action sheet ---

    void Commands(const std::string &verb, const std::string &id)
    {
        const LandCommands &commands = hud_.Current().land_commands;
        if (!hud_.Interactive() || !commands.known)
            return;
        if (verb == "open" ? !commands.available || commands.active : !commands.active)
            return;
        if (verb == "open" && !id.empty() && std::find(commands.offered.begin(), commands.offered.end(), id) == commands.offered.end())
            return;
        if ((verb == "back" && !commands.can_back) || (verb == "close" && !commands.can_close))
            return;
        hud_.Send("landcommands", Owner(commands) + "\t" + verb + "\t" + id);
    }

    // A tap acts at once. The engine wants the row selected first and activated second,
    // each against the menu revision it answers to: the second step follows the revision
    // that reports the row as selected.
    void Choose(const std::string &id)
    {
        const LandCommands &commands = hud_.Current().land_commands;
        const LandCommands::Item *item = commands.Find(id);
        if (!choice_.empty() && commands.stage != choice_stage_)
            choice_.clear(); // a tap of the last stage is over
        // A tap while another waits replaces it unless that one is being activated already: a
        // tap is never swallowed by an earlier one that the engine has not answered yet.
        if (!item || !item->enabled || (!choice_.empty() && (choice_activated_ || choice_ == id)))
            return;
        choice_ = id;
        choice_stage_ = commands.stage;
        choice_until_ = hud_.Now() + kChoiceSeconds;
        choice_revision_ = 0;
        choice_activated_ = false;
        hud_.Rebuild();
        Follow(commands);
    }

    void Follow(const LandCommands &commands)
    {
        if (!choice_.empty() && commands.stage != choice_stage_)
        {
            // The menu went on to another stage (its items reuse the ids): the tap belonged to
            // the last one. Nothing of the new stage is pressed, and its first tap acts.
            choice_.clear();
            hud_.Rebuild();
            return;
        }
        if (choice_.empty() || choice_revision_ == commands.revision)
            return;
        const LandCommands::Item *item = commands.Find(choice_);
        if (!item || !item->enabled || choice_activated_)
        {
            // Activated (the menu moved on) or gone: the tap is over.
            choice_.clear();
            hud_.Rebuild();
            return;
        }
        choice_revision_ = commands.revision;
        choice_activated_ = commands.selected == choice_;
        if (choice_activated_ && item->kind == "travel")
            hud_.ExpectLandTransition("the travel place '" + item->label + "'", commands, choice_);
        Commands(choice_activated_ ? "activateItem" : "selectItem", choice_);
    }

    const char *RowIcon(const LandCommands::Item &item)
    {
        if (item.kind == "potion")
            return "potion";
        if (item.kind == "ability")
            return "energy";
        if (item.kind == "travel")
        {
            if (const char *icon = LandPlaceIcon(item.picture))
                return icon;
            // Documented fallback: a place whose picture the table lacks (a mod's) keeps its tile
            // with the travel icon; said once per picture.
            if (unknown_places_.insert(item.picture).second)
                hud_.Log("land HUD: travel place '" + item.label + "' (" + item.source + ") has the picture '" + item.picture +
                         "' the place table does not know: the travel icon");
            return "travel";
        }
        for (const Named &command : kCommands)
            if (item.source == command.key)
                return command.icon;
        return "actions";
    }

    static size_t TravelRank(const LandCommands::Item &item)
    {
        static const char *const kOrder[] = {"store",  "shipyard",   "tavern", "residence", "usurer", "church",
                                             "lantern", "portoffice", "prison", "fort",      "port"};
        const char *icon = item.kind == "travel" ? LandPlaceIcon(item.picture) : nullptr;
        for (size_t rank = 0; icon && rank < std::size(kOrder); ++rank)
            if (std::strcmp(icon, kOrder[rank]) == 0)
                return rank;
        return std::size(kOrder);
    }

    void BuildSheet(const LandCommands &commands, View &view, Input &input)
    {
        const bool root = commands.stage == "commands";
        const bool travel = commands.stage == "travel";
        Json rows = Json::array();
        std::string group;
        int headings = 0;
        std::vector<const LandCommands::Item *> items;
        for (const LandCommands::Item &item : commands.items)
            items.push_back(&item);
        // The place the hero stands in: the game leaves it out of the list, the sheet shows its tile in its
        // slot, marked and not tappable.
        LandCommands::Item here;
        if (travel && !commands.here_picture.empty())
        {
            here.id = "here";
            here.kind = "travel";
            here.label = commands.here_label;
            here.picture = commands.here_picture;
            items.push_back(&here);
        }
        // Each town's table lists its places in its own order and the game puts the port first
        // whenever the ship is in reach: the travel tiles go in one order of place kinds instead,
        // so a kind keeps its slot from town to town (the port, which comes and goes, last).
        if (travel)
            std::stable_sort(items.begin(), items.end(), [](const LandCommands::Item *a, const LandCommands::Item *b) {
                return TravelRank(*a) < TravelRank(*b);
            });
        for (const LandCommands::Item *entry : items)
        {
            const LandCommands::Item &item = *entry;
            if (entry == &here)
            {
                rows.push_back(Json{{"id", "item-here"}, {"title", item.label}, {"icon", RowIcon(item)}, {"disabled", true}, {"pressed", false},
                                    {"chevron", false}, {"cooldown", 0}, {"here", true}});
                // A tap on it shakes the tile (the disabled button's answer, behaviors.cpp) and asks the game nothing.
                input.On("item-here", "refused", [](const Event &) {});
                continue;
            }
            if (item.kind == "cancel" || (root && item.kind == "command" && IsTransition(item.source)))
                continue; // transitions have their own button
            const std::string element = "item-" + item.id;
            Json row = {{"id", element},
                        {"title", item.label},
                        {"icon", RowIcon(item)},
                        {"disabled", !item.enabled},
                        {"pressed", item.id == choice_ && commands.stage == choice_stage_},
                        {"chevron", item.kind == "command" && item.source == "BI_ItemsUse"},
                        {"cooldown", item.enabled ? 0 : std::lround((1.0 - item.cooldown) * 100.0)}};
            // Potions and abilities share one stage: each kind under its own heading.
            if (!root && !travel && item.kind != group)
            {
                group = item.kind;
                ++headings;
                row["heading"] = item.kind == "potion" ? hud_.Text("hud.sheet.potions") : hud_.Text("hud.sheet.abilities");
            }
            rows.push_back(std::move(row));
            input.On(element, "click", [this, id = item.id](const Event &) { Choose(id); });
        }
        view.sheet = "s-hud-sheet";
        view.modal = true;
        view.sheet_props = Json{{"title", commands.title.empty() ? hud_.Text("hud.sheet.actions") : commands.title},
                                {"back", !root && !travel && commands.can_back},
                                {"layout", travel ? "tiles" : root ? "rows" : "dense"},
                                {"headings", headings},
                                {"rows", rows}};
        input.On("sheet-back", "click", [this](const Event &) { Commands("back", {}); });
        input.On("sheet-close", "click", [this](const Event &) { Commands("close", {}); });
        input.On("sheet", "close", [this](const Event &) { Commands("close", {}); });
    }

    Hud &hud_;
    AbilityControls abilities_{hud_};
    // movement
    bool moving_ = false;
    int x_ = 0, y_ = 0;           // the thumb now (0, 0: up)
    int said_x_ = 0, said_y_ = 0; // what the engine was last told while moving_
    // The stick is said on change only (event model): the engine holds it until its release.
    std::string move_owner_;
    uint64_t move_generation_ = 0;
    uint64_t move_sequence_ = 0;
    // combat
    uint64_t combat_activity_ = 0, combat_sequence_ = 0;
    bool wheel_open_ = false;
    std::string move_;
    // action sheet
    std::optional<Hint> hint_; // the bottom hint shown
    double hint_lost_ = -1.0;  // when what it names went away (-1 = it still applies)
    bool hint_leaving_ = false;
    std::string choice_; // tapped row on its way through select → activate
    double choice_until_ = 0.0;
    uint64_t choice_revision_ = 0;
    bool choice_activated_ = false;
    std::string choice_stage_; // the menu stage the tapped row belongs to
    std::set<std::string> unknown_places_; // travel pictures already reported
};

} // namespace

std::unique_ptr<Scene> CreateLandScene(Hud &hud)
{
    return std::make_unique<LandScene>(hud);
}

} // namespace storm::ui::hud
