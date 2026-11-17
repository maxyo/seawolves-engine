#include "platform/android_worldmap.hpp"
#include <cassert>
#include <iostream>
using namespace storm::android;
struct WMShipCommandList {
    struct Item {std::string sCommandName,sNote;};
    std::vector<Item> m_aUsedCommand;
    std::vector<MapCommand> AndroidMapCommands() const;
    std::string selected;
    bool SelectAndroidMapCommand(const std::string &id);
    bool SelectAndroidItem(const std::string &id){selected=id;return true;}
};
#include "commands.inc"
#include "select.inc"
constexpr int BI_COMMODE_COMMAND_SELECT=1,BI_MSG_COMMAND_ACTIVATE=2;
struct WM_INTERFACE {
    bool m_bVisible=true,active=false,stopOnPurpose=false;int m_nCommandMode=0,executed=0;
    void MarkStopOnPurpose(){assert(!executed);stopOnPurpose=true;} // before the command leaves the map
    WMShipCommandList *m_pCommandList=nullptr;
    std::vector<WMShipCommandList::Item> fresh;
    bool IsCommandMenuActive(){return active;}
    void UpdateCommandList(){m_pCommandList->m_aUsedCommand=fresh;}
    void ExecuteCommand(int command){executed=command;}
    bool ActivateAndroidMapCommand(const std::string &id);
};
#include "activate.inc"
WorldMapFrame complete()
{
    WorldMapFrame f;f.available=true;f.owner=19;f.gameplayRevision=4;f.identity="profile:captain:19";f.context="island:Martinique";
    f.instrumentsVisible=f.shipsVisible=f.commandsReady=true;
    auto &i=f.instruments;i.owner=19;i.complete=true;i.visible=true;i.date="18 October 1664";i.hour=12.25;
    i.windForce=.5;i.windAngle=1;i.heading=2;i.morale=50;i.foodDays=17;i.rumDays=20;i.flag=2;i.coordinates="15 N";
    f.ships.push_back({"0:captain:22","Adelina",100,98,40,6});
    f.commands.push_back({"EnterToIsland","Approach island"});return f;
}

int main()
{
    assert(storm::android::mapStormState("0","0") == 0);
    assert(storm::android::mapStormState("1","0") == 1);
    assert(storm::android::mapStormState("1","1") == 2);
    assert(!storm::android::mapStormState(nullptr,"0"));
    assert(!storm::android::mapStormState("1",nullptr));
    assert(!storm::android::mapStormState("bad","0"));
    assert(!storm::android::mapStormState("0","1"));

    // The moon of the game's sky (WhrSun.c: Moon00…Moon07 by day / 4).
    assert(storm::android::mapMoonPhase(1)==0&&storm::android::mapMoonPhase(3)==0&&storm::android::mapMoonPhase(4)==1);
    assert(storm::android::mapMoonPhase(16)==4&&storm::android::mapMoonPhase(19)==4&&storm::android::mapMoonPhase(31)==7);
    assert(!storm::android::mapMoonPhase(0)&&!storm::android::mapMoonPhase(32));
    // The game's calendar has 365-day years: 1 March is 59 days after 1 January in every year.
    assert(*storm::android::mapCalendarMinutes(1652,3,1,0)-*storm::android::mapCalendarMinutes(1652,1,1,0)==59*1440);
    assert(*storm::android::mapCalendarMinutes(1653,1,1,1.5)-*storm::android::mapCalendarMinutes(1652,12,31,23.5)==120);
    assert(!storm::android::mapCalendarMinutes(1652,13,1,0)&&!storm::android::mapCalendarMinutes(1652,1,1,25));
    {
        using storm::android::MapRainPlan;using storm::android::mapRainState;
        MapRainPlan none{false,false,{},{},{}};
        assert(mapRainState(none,12)==0);
        MapRainPlan planned{false,true,13.0,90.0,{}};
        assert(mapRainState(planned,11.9)==0&&mapRainState(planned,12.0)==1&&mapRainState(planned,12.99)==1&&mapRainState(planned,13.0)==2);
        MapRainPlan raining{true,true,13.0,90.0,30.0};
        assert(mapRainState(raining,13.5)==2);
        raining.elapsed=91.0;assert(mapRainState(raining,14.6)==0); // the rain is over when the sea updates the weather
        raining.elapsed.reset();assert(mapRainState(raining,14.6)==2); // no recorded start: the scripts count 0 minutes
        assert(!mapRainState(MapRainPlan{std::nullopt,false,{},{},{}},12)); // a malformed flag is not fair weather
        assert(!mapRainState(MapRainPlan{false,true,{},{},{}},12));          // planned without a start hour
        assert(!mapRainState(MapRainPlan{true,true,13.0,{},{}},12));         // raining without a duration
        assert(!mapRainState(none,-1));
    }

    WorldMapHud weatherHud;auto weatherFrame=complete();weatherHud.update(weatherFrame);
    const auto weatherRevision=weatherHud.revision();
    assert(weatherHud.json().find("\"stormState\":null")!=std::string::npos);
    for(int storm=0;storm<=2;++storm){
        weatherFrame.instruments.stormState=storm;weatherHud.update(weatherFrame);
        assert(weatherHud.revision()==weatherRevision); // Weather alone must not revoke held input/commands.
        assert(weatherHud.json().find("\"stormState\":"+std::to_string(storm))!=std::string::npos);
    }
    assert(weatherHud.json().find("\"moonPhase\":null,\"rainState\":null")!=std::string::npos);
    weatherFrame.instruments.moonPhase=6;weatherFrame.instruments.rainState=1;weatherHud.update(weatherFrame);
    assert(weatherHud.revision()==weatherRevision);
    assert(weatherHud.json().find("\"moonPhase\":6,\"rainState\":1")!=std::string::npos);
    WorldMapHud betweenFrames;betweenFrames.update(complete());betweenFrames.resetActivity();
    // The drawn UI compass's full box is accepted only for this owner/activity.
    const auto packet=[](WorldMapHud &h,const std::string &verb,const std::string &value){return std::to_string(h.revision())+"\t1\t"+verb+"\t"+value;};
    const auto labelArea=packet(betweenFrames,"labelarea","960:540:702:15:240:219");
    assert(betweenFrames.offer(labelArea,118));
    assert(betweenFrames.labelCompass() && (*betweenFrames.labelCompass())[4]==240);
    assert(!betweenFrames.offer(packet(betweenFrames,"labelarea","960:540:900:15:240:219"),119));
    assert(!betweenFrames.offer(packet(betweenFrames,"labelarea","960:540:702:15:240:219:1"),119));
    assert(!betweenFrames.offer(packet(betweenFrames,"labelarea","960:0:702:15:240:219"),119));
    assert(betweenFrames.labelCompass()); // malformed geometry cannot overwrite the last drawn box
    assert(betweenFrames.offer(packet(betweenFrames,"labelarea","none"),120));
    assert(!betweenFrames.labelCompass());
    assert(betweenFrames.offer(labelArea,121));
    betweenFrames.resetActivity();
    assert(!betweenFrames.labelCompass() && !betweenFrames.offer(labelArea,122));
    const auto freshArea=std::to_string(betweenFrames.revision())+"\t2\tlabelarea\t960:540:702:15:240:219";
    assert(betweenFrames.offer(freshArea,123) && betweenFrames.labelCompass());
    auto changedFrame=complete();changedFrame.owner=20;betweenFrames.update(changedFrame);
    assert(!betweenFrames.labelCompass() && !betweenFrames.offer(freshArea,125));
    WMShipCommandList commands;commands.m_aUsedCommand={{"Cancel","Cancel"},{"EnterToIsland","Approach island"}};
    assert(commands.AndroidMapCommands().size()==1&&commands.AndroidMapCommands()[0].id=="EnterToIsland");
    commands.m_aUsedCommand[0].sCommandName="cancel";assert(commands.AndroidMapCommands().size()==1);
    commands.m_aUsedCommand[1].sNote="";assert(commands.AndroidMapCommands().empty());
    WM_INTERFACE interface;interface.m_pCommandList=&commands;
    interface.fresh={{"Cancel","Cancel"},{"EnterToIsland","Approach island"}};
    assert(!interface.ActivateAndroidMapCommand("EnterToSea")&&!interface.executed);
    assert(interface.ActivateAndroidMapCommand("EnterToIsland")&&interface.executed==BI_MSG_COMMAND_ACTIVATE&&commands.selected=="i1");
    // Leaving for the open sea is a stop on purpose (WMapCancel's script half, first): an
    // encounter met there can be sailed past. A failed selection marks nothing.
    assert(interface.stopOnPurpose);
    {
        WM_INTERFACE engage;engage.m_pCommandList=&commands;engage.fresh={{"EnterToAttack","Attack"},{"EnterToSea","To sea"}};
        assert(engage.ActivateAndroidMapCommand("EnterToAttack")&&engage.executed&&!engage.stopOnPurpose);
        WM_INTERFACE missing;missing.m_pCommandList=&commands;missing.fresh={{"EnterToAttack","Attack"}};
        assert(!missing.ActivateAndroidMapCommand("EnterToSea")&&!missing.stopOnPurpose);
        WM_INTERFACE sea;sea.m_pCommandList=&commands;sea.fresh={{"EnterToSea","To sea"}};
        assert(sea.ActivateAndroidMapCommand("EnterToSea")&&sea.stopOnPurpose&&sea.executed);
        assert(leavesForOpenSea("EnterToSea")&&leavesForOpenSea("EnterToIsland")&&!leavesForOpenSea("EnterToShip")&&
               !leavesForOpenSea("EnterToStorm")&&!leavesForOpenSea("EnterToEnemy"));
    }
    interface.active=true;assert(!interface.ActivateAndroidMapCommand("EnterToIsland"));interface.active=false;
    interface.m_bVisible=false;assert(!interface.ActivateAndroidMapCommand("EnterToIsland"));
    WorldMapHud h;auto f=complete();h.update(f);h.resetActivity();
    auto request=[&](std::string verb,std::string value){return std::to_string(h.revision())+"\t1\t"+verb+"\t"+value;};
    assert(f.availableMask()==7);
    assert(!h.offer(request("ack","7"),10)&&!h.offer(request("prepare","17:7:1"),10)); // no modal protocol: only labelarea and activate
    const auto old=request("activate","EnterToIsland");
    assert(h.offer(old,12));f.context="island:Guadeloupe";h.update(f);
    assert(!h.take(14));assert(!h.offer(old,14));
    assert(!h.offer(request("activate","EnterToEnemy"),14));
    assert(h.offer(request("activate","EnterToIsland"),14));assert(!h.offer(request("activate","EnterToIsland"),14));
    assert(h.take(15)=="EnterToIsland");assert(!h.take(15));
    auto rev=h.revision();f.instruments.hour=14;h.update(f);assert(h.revision()==rev); // Numeric telemetry keeps held controls stable.
    f.instruments.foodDays.reset();h.update(f);assert(h.frame().availableMask()==6);
    f.commandsReady=false;h.update(f);assert(!h.offer(request("activate","EnterToIsland"),1040));
    f=complete();h.update(f);assert(h.offer(request("activate","EnterToIsland"),1051));assert(!h.take(2052)); // a lease lasts 1 s
    const auto staleActivity=request("activate","EnterToIsland");h.resetActivity();assert(!h.offer(staleActivity,2053));
    h.update({});assert(!h.offer(staleActivity,2055));assert(!h.take(2055));
    h.update(complete());assert(h.offer(std::to_string(h.revision())+"\t2\tactivate\tEnterToIsland",UINT32_MAX-10));
    assert(h.take(5)=="EnterToIsland"); // tick rollover
    auto before=h.revision();auto changed=complete();changed.instruments.flag.reset();h.update(changed);assert(h.revision()!=before);
    changed.instruments.rumDays.reset();changed.instruments.coordinates.clear();h.update(changed);assert(h.frame().availableMask()==7);
    changed.shipsComplete=false;h.update(changed);assert(h.frame().availableMask()==5);
    h.update(complete());
    auto json=h.json();assert(json.find("\"foodDays\":17")!=std::string::npos);assert(json.find("Adelina")!=std::string::npos);
    {   // the sign's ship picture and the crew capacity go with the ship
        auto pictured=complete();pictured.ships[0].crewMax=80;
        pictured.ships[0].picture=shipPicture("battle_interface\\ship_icons2.tga.tx",{0.25f,0.0625f,0.3125f,0.125f});
        WorldMapHud p;p.update(pictured);const auto out=p.json();
        assert(out.find("\"crew\":40,\"shipClass\":6")!=std::string::npos&&out.find("\"crewMax\":80")!=std::string::npos);
        assert(out.find("\"picture\":{\"texture\":\"battle_interface/ship_icons2\",\"uv\":[0.25,0.0625,0.3125,0.125]}")!=std::string::npos);
        assert(json.find("\"picture\"")==std::string::npos&&json.find("crewMax")==std::string::npos);
    }
    {
        // One coverage rule: while the UI presents the world map HUD the native instruments, fleet
        // and command icon are off; without it they draw.
        WorldMapHud p;p.update(complete());p.resetActivity();
        assert(!p.covered(7,19));
        hudPresenters.worldMap=true;
        assert(p.covered(7,19)&&!p.covered(7,20));
        auto next=complete();next.instruments.foodDays.reset();p.update(next);assert(p.covered(6,19)&&!p.covered(1,19));
        p.update({});assert(!p.covered(2,19));
        hudPresenters.worldMap=false;
    }
    std::cout<<"PASS map coverage by the presenting UI, command identity, one-shot dispatch, quest lock, release, activity and tick rollover\n";
}
