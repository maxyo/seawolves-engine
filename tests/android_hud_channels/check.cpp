// The HUD state channels build their JSON only when their typed state changed (HudChannel,
// platform/android_hud_channel.hpp) and publish exactly what comparing the JSON text every loop
// published. Each channel's state is driven through a deterministic random walk of its real
// mutations; after every step the old publisher (build the JSON, compare it with the last text)
// and the new one (compare the publication key, build only on a change) run side by side and must
// publish the same byte-identical sequence. Loops with an unchanged state must not build.
#include <platform/android_combat_input.hpp>
#include <platform/android_gameplay.hpp>
#include <platform/android_hud_channel.hpp>
#include <platform/android_land_commands.hpp>
#include <platform/android_land_vitals.hpp>
#include <platform/android_sea_orders.hpp>
#include <platform/android_pistol_state.hpp>
#include <platform/android_sea_quick_action.hpp>
#include <platform/android_ship_telemetry.hpp>
#include <platform/android_spyglass.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace storm::android;

namespace
{
// The old publisher and the new one, side by side, over one channel.
template <class KeyRefs> struct Twin
{
    const char *name;
    std::string lastText;
    std::vector<std::string> oldSent, newSent;
    HudChannel<KeyRefs> channel;
    size_t steps = 0, builds = 0, unchangedSteps = 0;
    // `json`: what the old publisher compared; `key`: the new publisher's typed state.
    template <class Json> void step(const Json &json, const KeyRefs &key)
    {
        ++steps;
        const bool unchanged = channel.same(key);
        const auto text = json();
        if (text != lastText) { lastText = text; oldSent.push_back(text); }
        if (const auto *sent = channel.publish(key, [&] { ++builds; return json(); })) newSent.push_back(*sent);
        if (unchanged) ++unchangedSteps;
    }
    // A new UI host: both forget what they sent.
    void reset() { lastText.clear(); channel.reset(); }
    void verify() const
    {
        assert(oldSent == newSent);
        assert(builds + unchangedSteps == steps); // an unchanged state never builds
        std::printf("  %-14s %5zu loops, %4zu publications identical, %5zu JSON builds (old: %zu)\n", name, steps,
                    newSent.size(), builds, steps);
    }
};

SeaOrdersFrame seaFrame(std::mt19937 &random)
{
    SeaOrdersFrame f;
    f.available = random() % 8 != 0;
    f.active = random() % 3 == 0;
    f.language = "russian";
    f.recipients.push_back({"r11_0", "Flagship", "11", 0, true});
    if (random() % 2) f.recipients.push_back({"r12_3", random() % 4 ? "Companion" : "Comp\"anion", "12", 3, false});
    f.recipientId = "r11_0";
    if (f.active)
    {
        f.mode = 32;
        f.items.push_back({"i0", "command", "Drift", "native-drift", true, 1, "BI_Drift"});
        // Cooldown animation, sometimes below the printed precision (no JSON change).
        f.items.push_back({"i1", "command", "Ability", "native-ability", random() % 2 == 0,
                           (random() % 4) * .25 + (random() % 2) * 1e-9, "BI_Ability"});
        f.selectedItemId = random() % 2 ? "i0" : "";
    }
    return f;
}

void seaOrdersChannel()
{
    std::mt19937 random(7);
    SeaOrdersState s;
    Twin<decltype(s.publicationKey())> twin{"seaOrders"};
    const auto json = [&] { return s.json(); };
    s.resetActivity();
    twin.step(json, s.publicationKey());
    auto frame = seaFrame(random);
    for (int i = 0; i < 4000; ++i)
    {
        switch (random() % 16)
        {
        case 0: frame = seaFrame(random); s.update("battle1:ship11", frame); break;
        case 1: s.update(random() % 2 ? "battle1:ship11" : "battle2:ship11", frame); break;
        case 2: s.invalidate(); break;
        case 3: s.resetActivity(); break;
        case 4: twin.reset(); s.resetActivity(); break; // publishSeaOrders on a new UI host
        case 5: s.update({}, {}); break;
        default: s.update("battle1:ship11", frame); break; // an unchanged refresh: most loops
        }
        twin.step(json, s.publicationKey());
    }
    // A change the wire format does not carry: built, compared, not published.
    frame.active = true;
    frame.items = {{"i0", "command", "Drift", "native-drift", true, .5, "BI_Drift"}};
    s.update("battle1:ship11", frame);
    twin.step(json, s.publicationKey());
    const auto sent = twin.newSent.size(), builds = twin.builds;
    frame.items[0].cooldown = .5 + 1e-9;
    s.update("battle1:ship11", frame);
    twin.step(json, s.publicationKey());
    assert(twin.builds == builds + 1 && twin.newSent.size() == sent);
    twin.verify();
}

LandCommandsFrame landFrame(std::mt19937 &random)
{
    LandCommandsFrame f;
    f.available = random() % 8 != 0;
    f.active = random() % 3 == 0;
    f.language = "russian";
    f.ownerIdentity = "list1";
    f.offered = {"BI_Dialog", "BI_Officers"};
    if (f.active)
    {
        f.items.push_back({"i0", "command", "Talk", "BI_Dialog", "id0", true, 1, "icons:3"});
        f.items.push_back({"i1", "command", "Off\\icers", "BI_Officers", "id1", random() % 2 == 0,
                           (random() % 4) * .25 + (random() % 2) * 1e-9, ""});
        f.selectedItemId = random() % 2 ? "i0" : "i1";
    }
    return f;
}

std::string landRequest(const LandCommandsState &s, const char *verb, const char *id)
{
    return "1\t" + std::to_string(s.sceneEpoch()) + "\t" + std::to_string(s.revision()) + "\t" +
           std::to_string(s.activityGeneration()) + "\t" + verb + "\t" + id;
}

void landCommandsChannel()
{
    std::mt19937 random(11);
    LandCommandsState s;
    Twin<decltype(s.publicationKey())> twin{"landCommands"};
    const auto json = [&] { return s.json(); };
    s.resetActivity();
    twin.step(json, s.publicationKey());
    auto frame = landFrame(random);
    uint32_t tick = 0;
    uint64_t receipts = 0, lastSerial = 0;
    for (int i = 0; i < 4000; ++i)
    {
        tick += 16;
        switch (random() % 18)
        {
        case 0: frame = landFrame(random); s.update("owner:list1", frame); break;
        case 1: s.update(random() % 2 ? "owner:list1" : "owner:list2", frame); break;
        case 2: s.invalidate(); break;
        case 3: s.resetActivity(); break;
        case 4: twin.reset(); s.resetActivity(); break; // publishLandCommands on a new UI host
        case 5: s.releaseUnavailableSource(random() % 2 ? LandCommandSource::Missing : LandCommandSource::Forbidden); break;
        // Taps: accepted, refused with a receipt, or stale.
        case 6: s.offer(landRequest(s, "activateItem", random() % 2 ? "i0" : "i1"), tick); break;
        case 7: s.offer(landRequest(s, "open", ""), tick); break;
        case 8: s.offer("1\t1\t1\t" + std::to_string(s.activityGeneration()) + "\tactivateItem\ti0", tick); break;
        case 9:
            if (const auto request = s.take(tick + (random() % 2) * 2000))
                s.result(*request, random() % 2 == 0, "native-dispatched");
            break;
        default: s.update("owner:list1", frame); break; // an unchanged refresh: most loops
        }
        twin.step(json, s.publicationKey());
        if (const auto serial = std::get<4>(s.publicationKey()); serial != lastSerial) { lastSerial = serial; ++receipts; }
    }
    assert(receipts > 10); // the walk does reach the receipts
    twin.verify();
}

SeaQuickActionFrame quickFrame(std::mt19937 &random)
{
    SeaQuickActionFrame f;
    f.host = 5;
    f.sea = true;
    f.available = random() % 4 != 0;
    f.complete = random() % 6 != 0;
    static const char *actions[] = {"Moor", "Board", "Reload", "Bogus"};
    f.action = actions[random() % 4];
    f.label = f.action + " here";
    f.owner = "owner";
    f.target = random() % 2 ? "t1" : "t2";
    f.map = random() % 2 == 0;
    f.mapLabel = f.map ? "Map" : "";
    return f;
}

void seaQuickActionChannel()
{
    std::mt19937 random(13);
    SeaQuickActionState s;
    Twin<decltype(s.publicationKey())> twin{"seaQuickAction"};
    const auto json = [&] { return s.json(); };
    twin.step(json, s.publicationKey());
    auto frame = quickFrame(random);
    uint32_t tick = 0;
    for (int i = 0; i < 4000; ++i)
    {
        tick += 16;
        const auto request = [&](const std::string &verb, const std::string &value) {
            return "1\t" + std::to_string(s.activity()) + "\t" + std::to_string(s.revision()) + "\t" + verb + "\t" + value;
        };
        switch (random() % 14)
        {
        case 0: frame = quickFrame(random); s.update(frame); break;
        case 1: s.update({}); break;
        case 2: s.resetActivity(); break;
        case 3: twin.reset(); s.resetActivity(); break; // publishSeaQuickAction on a new UI host
        case 4: s.offer(request("ack", random() % 2 ? "1" : "0"), tick); break;
        case 5: s.offer(request("activate", frame.action), tick); break;
        case 6: s.consume(tick + (random() % 2) * 2000); break;
        default: s.update(frame); break; // an unchanged frame: most loops
        }
        twin.step(json, s.publicationKey());
    }
    twin.verify();
}

// The "combat" channel as publishCombatState (android_combat_state.hpp) keys and builds it.
std::string combatInteractKind = "dialog", combatInteractName, combatInteractDialog;
auto combatKey(const CombatInput &input, const PistolState &pistol)
{
    return std::tuple_cat(input.publicationKey(), std::tie(std::as_const(combatInteractKind),
        std::as_const(combatInteractName), std::as_const(combatInteractDialog), pistol));
}
std::string combatJson(const CombatInput &input, const PistolState &pistol)
{
    auto json = input.json();
    json.pop_back();
    json += ",\"interactKind\":" + gameplayJsonString(combatInteractKind);
    json += ",\"interactName\":" + gameplayJsonString(combatInteractName);
    json += ",\"interactDialog\":" + gameplayJsonString(combatInteractDialog);
    json += ",\"pistol\":{\"state\":" + gameplayJsonString(pistol.state) +
            ",\"progress\":" + std::to_string(pistol.progress) + "}}";
    return json;
}

std::vector<CombatBinding> combatBindings(std::mt19937 &random, bool melee)
{
    std::vector<CombatBinding> bindings;
    if (random() % 4) bindings.push_back({"weapon", "ChrFightMode", false});
    if (random() % 2) bindings.push_back({"interact", "ChrAction", false, static_cast<int64_t>(random() % 3)});
    bindings.push_back({"freelook", "ChrCamSpecMode", true});
    if (melee)
    {
        bindings.push_back({"block", random() % 2 ? "ChrBlock" : "ChrBlock2", true});
        bindings.push_back({"dash", "ChrRecoil", false});
    }
    return bindings;
}

void combatChannel()
{
    std::mt19937 random(17);
    CombatInput input;
    input.guard = [] { return true; };
    PistolState pistol;
    Twin<decltype(combatKey(input, pistol))> twin{"combat"};
    const auto json = [&] { return combatJson(input, pistol); };
    twin.step(json, combatKey(input, pistol));
    uint64_t revision = 1;
    std::string mode = "explore", player = "7";
    auto bindings = combatBindings(random, false);
    uint32_t tick = 0;
    for (int i = 0; i < 4000; ++i)
    {
        tick += 16;
        switch (random() % 20)
        {
        case 0: ++revision; break;
        case 1: mode = random() % 2 ? "melee" : "explore"; bindings = combatBindings(random, mode == "melee"); break;
        case 2: bindings = combatBindings(random, mode == "melee"); break;
        case 3: player = random() % 2 ? "7" : ""; break;
        case 4: twin.reset(); input.resetActivity(); break; // publishCombatState on a new UI host
        case 5: cancelAndroidCombatInput(); break;
        case 6:
            input.offer("2\t" + std::to_string(input.generation()) + "\t" + std::to_string(input.actionGeneration("weapon")) +
                        "\t" + std::to_string(i + 1) + "\tweapon\ttap", tick);
            break;
        case 7: input.advance(tick); break;
        case 8: combatInteractName = random() % 2 ? "Nathaniel \"Hawk\"" : ""; break;
        case 9: combatInteractKind = random() % 2 ? "dialog" : "boarding"; break;
        case 10: combatInteractDialog = random() % 2 ? "common_store" : ""; break;
        case 11: pistol = pistolState(random() % 2, true, (random() % 5) * .25, .5); break;
        default: break; // an unchanged loop: most of them
        }
        // updateCombatState runs every loop with what it read (the bindings anew each time).
        input.update(revision, "scene1", player, mode, bindings);
        twin.step(json, combatKey(input, pistol));
    }
    twin.verify();
}

GameplaySnapshot gameplaySnapshot(std::mt19937 &random)
{
    GameplaySnapshot snapshot;
    static const char *modes[] = {"explore", "melee", "sailing", "unknown"};
    snapshot.mode = modes[random() % 4];
    snapshot.language = "russian";
    snapshot.group = "PrimaryLand";
    snapshot.scene = random() % 2 ? "loc1" : "loc2";
    snapshot.capabilities = {"ChrAction", "BI_InstantRepair"};
    if (random() % 2) snapshot.capabilities.push_back("Ship_Fire");
    snapshot.inputOwner = random() % 2 ? "player1" : "";
    snapshot.analogMovement = !snapshot.inputOwner.empty();
    snapshot.movementGeneration = random() % 3;
    snapshot.blocked = snapshot.mode == "unknown" ? "interface" : "";
    return snapshot;
}

void gameplayChannel()
{
    std::mt19937 random(19);
    GameplayState state;
    Twin<decltype(state.publicationKey())> twin{"gameplay"};
    const auto json = [&] { return state.json(); };
    auto snapshot = gameplaySnapshot(random);
    uint32_t tick = 0;
    for (int i = 0; i < 4000; ++i)
    {
        tick += 16;
        switch (random() % 10)
        {
        case 0: snapshot = gameplaySnapshot(random); break;
        case 1: state.offer(std::to_string(state.revision()) + "\tShip_Fire", tick); break;
        case 2: state.take(tick); break;
        default: break;
        }
        state.update(snapshot); // readGameplaySnapshot every loop
        twin.step(json, state.publicationKey());
    }
    twin.verify();
}

ShipTelemetryFrame shipFrame()
{
    ShipTelemetryFrame frame;
    frame.valid = true;
    frame.ship = {0, "11", "Flagship", "Frigate", shipMetric(50, 100), shipMetric(120, 200), shipMetric(80, 100), 3, {}};
    frame.fleet = {frame.ship, {3, "12", "Companion", "Brig", shipMetric(10, 50), shipMetric(30, 60), shipMetric(5, 10), 5, {}}};
    frame.ammo = "balls";
    frame.ammoType = 0;
    frame.ammoCount = 40;
    frame.sailState = .5;
    frame.captionsVisible = true;
    frame.shipSpeedKnots = 7.5;
    frame.dateTimeText = "1 May 1666";
    return frame;
}

// The "ship" channel keeps its own publication (ShipTelemetryState::publication, its 100 ms
// throttle and forced changes): an unchanged state returns nothing without building the JSON;
// what it does publish is the state's JSON, at the moments the throttle allowed before.
void shipChannel()
{
    ShipTelemetryState state;
    auto frame = shipFrame();
    state.update(1, frame);
    auto first = state.publication(0);
    assert(first && *first == state.json());
    state.published(*first, 0);
    frame.ship.hull = shipMetric(49, 100); // an ordinary HP change: throttled to 100 ms
    state.update(1, frame);
    assert(!state.publication(99));
    const auto changed = state.publication(100);
    assert(changed && *changed == state.json() && *changed != *first);
    state.published(*changed, 100);
    for (uint32_t tick = 101; tick < 5101; tick += 16)
    {
        state.update(1, frame); // the loop reads the same frame
        assert(!state.publication(tick));
    }
    assert(!state.publication(5101));
    frame.fleet.pop_back(); // another fleet: a forced change goes at once
    state.update(1, frame);
    const auto forced = state.publication(5102);
    assert(forced && *forced == state.json());
    state.published(*forced, 5102);
    assert(!state.publication(5103));
    state.resetActivity(); // a new UI host: republished at once
    const auto again = state.publication(5104);
    assert(again && *again == state.json());
    state.published(*again, 5104);
    assert(!state.publication(9000));
    std::puts("  ship           unchanged loops publish nothing; changes go as the throttle allowed before");
}

void spyglassChannel()
{
    std::mt19937 random(23);
    SpyglassState state;
    Twin<decltype(state.publicationKey())> twin{"spyglass"};
    const auto json = [&] { return state.json(); };
    SpyglassTarget target;
    for (int i = 0; i < 4000; ++i)
    {
        switch (random() % 12)
        {
        case 0: state.turn(random() % 3 != 0); break;
        case 1: target.name = random() % 2 ? "Black \"Pearl\"" : "Interceptor"; target.hull = random() % 100; break;
        case 2: target.distance = (random() % 1000) / 7.0; break;
        case 3: state.clear(); break;
        case 4: state.acknowledge(std::to_string(state.generation()) + (random() % 2 ? "\t1" : "\t0")); break;
        default: break;
        }
        if (state.on() && random() % 2) state.target(target); // the telescope's per-frame read
        twin.step(json, state.publicationKey());
    }
    twin.verify();
}

// publishReloadPlace (android_ui.hpp) keyed on the place and the lock.
std::string reloadPlaceJson(const std::string &place, bool locked)
{
    return place.empty() ? std::string("null") : "{\"place\":" + gameplayJsonString(place) + ",\"locked\":" + (locked ? "true" : "false") + "}";
}
void reloadPlaceChannel()
{
    std::mt19937 random(29);
    Twin<std::tuple<const std::string &, const bool &>> twin{"reloadPlace"};
    std::string place;
    bool locked = false;
    for (int i = 0; i < 4000; ++i)
    {
        if (random() % 8 == 0) place = random() % 3 ? "Tavern" : "";
        if (random() % 8 == 0) locked = !locked;
        twin.step([&] { return reloadPlaceJson(place, locked); }, std::tie(place, std::as_const(locked)));
    }
    twin.verify();
}

// The old vitals text, field by field as battle_man_sign.cpp and landStatusJson concatenated it.
std::string oldVitalsJson(const LandVitals &v)
{
    const auto picture = [](const LandPortrait &p) { return p.face.empty() && !p.texture.empty() ? "{\"texture\":\"" + p.texture + "\"}" : std::string("null"); };
    std::string effects;
    for (const auto &name : v.effects) effects += std::string(effects.empty() ? "" : ",") + "\"" + name + "\"";
    std::string json = ",\"id\":" + std::to_string(v.hero.id) + ",\"name\":" + gameplayJsonString(v.hero.name) +
                       ",\"face\":" + gameplayJsonString(v.hero.face) + ",\"picture\":" + picture(v.hero) +
                       ",\"effects\":[" + effects + "],\"potions\":" + std::to_string(v.potions) + ",\"officers\":[";
    bool first = true;
    for (const auto &o : v.officers)
    {
        json += std::string(first ? "" : ",") + "{\"id\":" + std::to_string(o.who.id) + ",\"name\":" + gameplayJsonString(o.who.name) +
                ",\"face\":" + gameplayJsonString(o.who.face) + ",\"picture\":" + picture(o.who) +
                ",\"health\":" + std::to_string(o.health) + ",\"energy\":" + std::to_string(o.energy) + "}";
        first = false;
    }
    json += "]";
    if (v.opponent) json += ",\"opponent\":{\"name\":" + gameplayJsonString(v.opponent->name) + ",\"health\":" + std::to_string(v.opponent->health) + "}";
    if (v.boarding)
        json += ",\"boarding\":{\"ours\":" + std::to_string(v.boarding->ours) + ",\"theirs\":" + std::to_string(v.boarding->theirs) +
                ",\"oursStart\":" + std::to_string(v.boarding->oursStart) + ",\"theirsStart\":" + std::to_string(v.boarding->theirsStart) + "}";
    if (v.dash) json += ",\"dash\":" + std::to_string(*v.dash);
    return "{\"health\":" + std::to_string(v.health) + ",\"energy\":" + std::to_string(v.energy) + ",\"loaded\":" +
           std::to_string(v.loaded) + ",\"capacity\":" + std::to_string(v.capacity) + ",\"reload\":" + std::to_string(v.reload) + json + "}";
}

LandVitals landVitals(std::mt19937 &random)
{
    LandVitals v;
    v.health = (random() % 5) * .25f;
    v.energy = (random() % 3) * .5f;
    v.capacity = 3;
    v.loaded = random() % 4;
    v.reload = (random() % 2) * .5f;
    v.hero = {0, "Charles de Maure", random() % 2 ? "201" : "", "battle_interface\\two", "0,0,0.5,0.5"};
    if (random() % 2) v.effects = {"poison", "healing"};
    v.potions = random() % 3;
    if (random() % 2) v.officers.push_back({{12, "Tichingitu", "", "battle_interface\\two", ""}, .5f, 1.f});
    if (random() % 3 == 0) v.opponent = LandOpponent{"Captain \"Bones\"", (random() % 4) * .25f};
    else if (random() % 2) v.boarding = LandBoarding{int(random() % 90), int(random() % 130), 90, 130};
    if (random() % 2) v.dash = (random() % 3) * .5f;
    return v;
}

// The boarding script's bodies and reserve as men (land_status.hpp readLandBoarding).
void boardingMen()
{
    // 12 bodies of 7 men each at the start; 5 in reserve and 4 alive on the deck: 63 of 84 men.
    const auto men = landBoardingMen(5, 4, 7.f, 12, 3, 2, 10.f, 12);
    assert(men && men->ours == 63 && men->oursStart == 84 && men->theirs == 50 && men->theirsStart == 120);
    assert(landBoardingMen(0, 0, 7.f, 12, 0, 0, 10.f, 12)->ours == 0);
    assert(landBoardingMen(1, 1, 2.5f, 3, 1, 1, 1.f, 3)->ours == 5);
    // The script's musketeers overdraw the enemy reserve (-2 seen in a real boarding): an empty reserve.
    assert(landBoardingMen(0, 1, 10.f, 1, -2, 16, 12.928572f, 14)->theirs == 207);
    assert(!landBoardingMen(0, -1, 7.f, 12, 0, 0, 10.f, 12) && !landBoardingMen(1, 0, 0.f, 12, 0, 0, 10.f, 12) &&
           !landBoardingMen(1, 0, 7.f, 0, 0, 0, 10.f, 12) && !landBoardingMen(1, 0, 7.f, 12, 0, 0, std::nanf(""), 12));
    std::printf("  %-14s men from bodies x crew_per_chr, refused without a start or a crew per body\n", "boarding");
}

// The game's walk-only rule as an effect (land_status.hpp readLandStatus): its run is its walk.
void walkOnly()
{
    assert(!landWalkOnlyEffect("run", "walk", false) && !landWalkOnlyEffect("run", "walk", true));
    assert(std::string_view(landWalkOnlyEffect("walk", "walk", false)) == "overload");
    assert(std::string_view(landWalkOnlyEffect("walk", "walk", true)) == "cantrun");
    // No actions read (a chest, the hero not set up yet): no claim.
    assert(!landWalkOnlyEffect("", "", false));
    std::printf("  %-14s overload / cantrun only while the run animation is the walk\n", "walk-only");
}

// publishVitals (android_ui.hpp): the old text compare with its 100 ms throttle beside the typed one.
void vitalsChannel()
{
    std::mt19937 random(31);
    const auto picture = [](const LandPortrait &p) { return p.face.empty() && !p.texture.empty() ? "{\"texture\":\"" + p.texture + "\"}" : std::string("null"); };
    std::string lastText;
    uint32_t lastTextTick = 0, lastTick = 0;
    HudChannel<std::tuple<const std::optional<LandVitals> &>> channel;
    std::vector<std::string> oldSent, newSent;
    size_t builds = 0, unchanged = 0, loops = 0;
    std::optional<LandVitals> vitals;
    uint32_t tick = 0;
    for (int i = 0; i < 6000; ++i)
    {
        tick += 1 + random() % 40;
        switch (random() % 10)
        {
        case 0: vitals = landVitals(random); break;
        case 1: vitals.reset(); break;
        default: break;
        }
        ++loops;
        // old
        const std::string text = vitals ? oldVitalsJson(*vitals) : std::string();
        if (!(lastText == text || (!text.empty() && tick - lastTextTick < 100)))
        {
            lastText = text; lastTextTick = tick;
            oldSent.push_back(text.empty() ? "null" : text);
        }
        // new, as publishVitals
        const auto key = std::tie(vitals);
        if (channel.same(key)) { ++unchanged; continue; }
        if (vitals && tick - lastTick < 100) continue;
        if (const auto *json = channel.publish(key, [&] { ++builds; return vitals ? landVitalsJson(*vitals, picture) : std::string(); }))
        {
            lastTick = tick;
            newSent.push_back(json->empty() ? "null" : *json);
        }
    }
    assert(oldSent == newSent);
    assert(builds + unchanged <= loops);
    std::printf("  %-14s %5zu loops, %4zu publications identical, %5zu JSON builds (old: %zu)\n", "vitals", loops,
                newSent.size(), builds, loops);
}

template <class Run> double nanosPerLoop(int loops, Run &&run)
{
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < loops; ++i) run();
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / loops;
}
volatile size_t sink;

// One loop of an unchanged channel: the old publisher built and compared the JSON, the new one
// compares the key.
void bench()
{
    constexpr int loops = 200000;
    std::mt19937 random(3);
    {
        SeaOrdersState s;
        s.resetActivity();
        auto f = seaFrame(random);
        f.active = false;
        f.items.clear();
        s.update("battle1:ship11", f);
        std::string last = s.json();
        HudChannel<decltype(s.publicationKey())> channel;
        channel.publish(s.publicationKey(), [&] { return s.json(); });
        const double old = nanosPerLoop(loops, [&] { const auto j = s.json(); sink = sink + (j == last); });
        const double now = nanosPerLoop(loops, [&] { sink = sink + (channel.publish(s.publicationKey(), [&] { return s.json(); }) != nullptr); });
        std::printf("  bench %-14s unchanged loop: old %7.1f ns, new %6.1f ns\n", "seaOrders", old, now);
    }
    {
        LandCommandsState s;
        s.resetActivity();
        auto f = landFrame(random);
        f.active = false;
        f.items.clear();
        s.update("owner:list1", f);
        std::string last = s.json();
        HudChannel<decltype(s.publicationKey())> channel;
        channel.publish(s.publicationKey(), [&] { return s.json(); });
        const double old = nanosPerLoop(loops, [&] { const auto j = s.json(); sink = sink + (j == last); });
        const double now = nanosPerLoop(loops, [&] { sink = sink + (channel.publish(s.publicationKey(), [&] { return s.json(); }) != nullptr); });
        std::printf("  bench %-14s unchanged loop: old %7.1f ns, new %6.1f ns\n", "landCommands", old, now);
    }
    {
        SeaQuickActionState s;
        s.update(quickFrame(random));
        std::string last = s.json();
        HudChannel<decltype(s.publicationKey())> channel;
        channel.publish(s.publicationKey(), [&] { return s.json(); });
        const double old = nanosPerLoop(loops, [&] { const auto j = s.json(); sink = sink + (j == last); });
        const double now = nanosPerLoop(loops, [&] { sink = sink + (channel.publish(s.publicationKey(), [&] { return s.json(); }) != nullptr); });
        std::printf("  bench %-14s unchanged loop: old %7.1f ns, new %6.1f ns\n", "seaQuickAction", old, now);
    }
    {
        CombatInput input;
        input.update(1, "scene1", "7", "melee", combatBindings(random, true));
        combatInteractName = "Nathaniel Hawk";
        const PistolState pistol = pistolState(true, true, 1., 0.);
        std::string last = combatJson(input, pistol);
        HudChannel<decltype(combatKey(input, pistol))> channel;
        channel.publish(combatKey(input, pistol), [&] { return combatJson(input, pistol); });
        const double old = nanosPerLoop(loops, [&] { const auto j = combatJson(input, pistol); sink = sink + (j == last); });
        const double now = nanosPerLoop(loops, [&] { sink = sink + (channel.publish(combatKey(input, pistol), [&] { return combatJson(input, pistol); }) != nullptr); });
        std::printf("  bench %-14s unchanged loop: old %7.1f ns, new %6.1f ns\n", "combat", old, now);
    }
    {
        GameplayState state;
        state.update(gameplaySnapshot(random));
        std::string last = state.json();
        HudChannel<decltype(state.publicationKey())> channel;
        channel.publish(state.publicationKey(), [&] { return state.json(); });
        const double old = nanosPerLoop(loops, [&] { const auto j = state.json(); sink = sink + (j == last); });
        const double now = nanosPerLoop(loops, [&] { sink = sink + (channel.publish(state.publicationKey(), [&] { return state.json(); }) != nullptr); });
        std::printf("  bench %-14s unchanged loop: old %7.1f ns, new %6.1f ns\n", "gameplay", old, now);
    }
    {
        ShipTelemetryState state;
        state.update(1, shipFrame());
        const auto first = state.publication(0);
        state.published(*first, 0);
        const double old = nanosPerLoop(loops, [&] { const auto j = state.json(); sink = sink + (j == *first); });
        const double now = nanosPerLoop(loops, [&] { sink = sink + state.publication(1000).has_value(); });
        std::printf("  bench %-14s unchanged loop: old %7.1f ns, new %6.1f ns\n", "ship", old, now);
    }
    {
        const auto picture = [](const LandPortrait &) { return std::string("null"); };
        std::optional<LandVitals> vitals = landVitals(random);
        vitals->officers.push_back({{13, "Mary Casper", "", "two", ""}, .75f, .5f});
        const std::string last = landVitalsJson(*vitals, picture);
        HudChannel<std::tuple<const std::optional<LandVitals> &>> channel;
        channel.publish(std::tie(vitals), [&] { return landVitalsJson(*vitals, picture); });
        const double old = nanosPerLoop(loops, [&] { const auto j = landVitalsJson(*vitals, picture); sink = sink + (j == last); });
        const double now = nanosPerLoop(loops, [&] { sink = sink + channel.same(std::tie(vitals)); });
        std::printf("  bench %-14s unchanged loop: old %7.1f ns, new %6.1f ns (JSON part only; reading the attributes is unchanged)\n", "vitals", old, now);
    }
}
} // namespace

// An ability's state from the scripts' perk counters (android_gameplay.hpp battleAbilityTiming) and
// its JSON in the ship and vitals channels.
void battleAbilities()
{
    BattleAbility a{"Rush", "Tincture \"O\""};
    battleAbilityTiming(a, 9.0, 72.0, 12.0, 72.0, false); // just used: the effect runs (the delay runs too)
    assert(a.state == BattleAbility::Active && a.fill == 0.75 && a.left == 9);
    battleAbilityTiming(a, std::nullopt, 53.5, 12.0, 72.0, false); // the effect over, the delay on
    assert(a.state == BattleAbility::Recovery && a.fill == 0.26 && a.left == 54);
    battleAbilityTiming(a, std::nullopt, std::nullopt, 12.0, 72.0, true);
    assert(a.state == BattleAbility::Ready && a.fill == 0 && a.left == 0);
    battleAbilityTiming(a, std::nullopt, std::nullopt, 12.0, 72.0, false);
    assert(a.state == BattleAbility::Blocked);
    battleAbilityTiming(a, 5.0, std::nullopt, std::nullopt, 72.0, true); // a counter without its timing: the scripts' verdict
    assert(a.state == BattleAbility::Ready);
    a.state = BattleAbility::Recovery, a.fill = 0.26, a.left = 54;
    assert(battleAbilitiesJson({a}) == R"([{"id":"Rush","label":"Tincture \"O\"","state":"recovery","fill":0.260000,"left":54}])");
    LandVitals v;
    assert(landVitalsJson(v, [](const LandPortrait &) { return std::string("null"); }).find("abilities") == std::string::npos);
    v.abilities = {a};
    assert(landVitalsJson(v, [](const LandPortrait &) { return std::string("null"); }).find(",\"abilities\":[{\"id\":\"Rush\"") != std::string::npos);
    std::printf("  %-14s active / recovery / ready / blocked from the perk counters, JSON in ship and vitals\n", "abilities");
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) { bench(); return 0; }
    seaOrdersChannel();
    landCommandsChannel();
    seaQuickActionChannel();
    combatChannel();
    gameplayChannel();
    shipChannel();
    spyglassChannel();
    reloadPlaceChannel();
    vitalsChannel();
    boardingMen();
    walkOnly();
    battleAbilities();
    std::puts("android_hud_channels: ok");
}
