#include "platform/android_land_commands.hpp"
#include <cassert>
#include <functional>
#include <iostream>
using namespace storm::android;
struct Core { template<class... T> void Trace(const char *, T...) {} } core;
struct BATTLE_LAND_INTERFACE {
    bool accept = true; int calls = 0;
    bool ExecuteAndroidLand(const LandCommandsRequest &) { ++calls; return accept; }
} battle;
static bool ownerExists = true;
static std::function<void()> refresh;
namespace storm::android {
BATTLE_LAND_INTERFACE *landOwner() { return ownerExists ? &battle : nullptr; }
void refreshLandCommands(bool) { if (refresh) refresh(); }
// PRODUCTION_BRIDGE
}
static LandCommandsFrame frame() {
    LandCommandsFrame f; f.available = f.active = true; f.ownerIdentity="list"; f.selectedItemId="i0";
    f.items={{"i0","travel","Port","port","reloadtoport:SantaCatalina_town:reload1",true,1}};
    return f;
}
static LandCommandsRequest request() {
    return {landCommands.sceneEpoch(),landCommands.revision(),landCommands.activityGeneration(),"activateItem","i0",10};
}
static std::string payload(const LandCommandsRequest &r) {
    return "1\t"+std::to_string(r.scene)+"\t"+std::to_string(r.revision)+"\t"+std::to_string(r.activity)+"\t"+r.verb+"\t"+r.id;
}
int main() {
    landCommands.resetActivity(); landCommands.update("tavern",frame());
    auto r=request();
    refresh=[] { auto changed=frame(); changed.items[0].identity="a-different-target"; landCommands.update("tavern",changed); };
    assert(!executeLandCommand(r) && battle.calls==0);
    assert(landCommands.json().find("candidates-changed-on-refresh")!=std::string::npos);
    assert(landCommands.json().find("\"accepted\":false")!=std::string::npos);
    // Same item id is insufficient: changed candidate identity never executes.
    landCommands.update("tavern",frame()); r=request();
    refresh=[] { landCommands.update("another-native-target-context",frame()); };
    assert(!executeLandCommand(r) && battle.calls==0);
    assert(landCommands.json().find("target-context-changed-on-refresh")!=std::string::npos);
    landCommands.update("tavern",frame()); r=request(); refresh=[]{};
    ownerExists=false;
    assert(!executeLandCommand(r) && battle.calls==0);
    assert(landCommands.json().find("no-land-owner")!=std::string::npos);
    ownerExists=true;
    assert(executeLandCommand(r) && battle.calls==1);
    assert(landCommands.json().find("native-dispatched")!=std::string::npos);
    // Accepted native dispatch does not assert any script transition happened.
    assert(landCommands.frame().active);
    const auto acceptedJson=landCommands.json();
    assert(!landCommands.offer(payload(r),10)); // duplicate of the accepted, now-stale lease
    assert(landCommands.json()==acceptedJson); // may not downgrade native acceptance
    auto unrelated=request(); unrelated.id="i7";
    assert(!landCommands.offer(payload(unrelated),10));
    assert(landCommands.json().find("native-dispatched")!=std::string::npos); // kept in history
    r=request(); battle.accept=false;
    assert(!executeLandCommand(r) && battle.calls==2);
    assert(landCommands.json().find("native-refused")!=std::string::npos);
    r=request(); assert(landCommands.offer(payload(r),10));
    const auto pendingJson=landCommands.json();
    assert(!landCommands.offer(payload(r),10));
    assert(landCommands.json()==pendingJson); // duplicate pending offer is not a refusal of the original
    landCommands.invalidate(); assert(!landCommands.take(11));
    assert(landCommands.json().find("invalidated-before-dispatch")!=std::string::npos);
    r=request(); assert(landCommands.offer(payload(r),10)); assert(!landCommands.take(1511));
    assert(landCommands.json().find("expired-before-dispatch")!=std::string::npos);
    r=request(); ++r.revision; assert(!landCommands.offer(payload(r),10));
    assert(landCommands.json().find("stale-at-enqueue")!=std::string::npos);
    landCommands.resetActivity();
    assert(landCommands.json().find("\"serial\":0")!=std::string::npos);
    assert(landCommands.json().find("\"activations\":[]")!=std::string::npos);
    std::cout<<"PASS production activation bridge: refreshed target refusal receipt, accepted/refused native dispatch, invalidation, expiry, stale enqueue, Activity reset\n";
}
