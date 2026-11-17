#include "platform/android_land_commands.hpp"
#include <cassert>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#define __ANDROID__ 1
namespace storm::android { inline void traceLandAdapter(unsigned,const std::string&) {} }
struct DATA {};
struct FileService {
    bool exists=true; int probes=0;
    bool _FileOrDirectoryExists(const char *path) {
        ++probes; assert(std::string(path)=="PROGRAM\\interface/android_ui_bootstrap.c");return exists;
    }
} files;
auto *fio=&files;
class COMPILER {
public:
    bool androidUiAdapterAttempted_=false,bFirstRun=true;
    const char *ProgramDirectory="PROGRAM\\";void *pRun_fi=nullptr;
    bool loaded=false,handler=false,compileSucceeds=true,executeSucceeds=true,mainDone=false,onLoadDone=false,baseRestored=false;
    int compiles=0;std::vector<std::string> order;
    struct Functions {uint32_t FindFunc(const char *name){return std::string_view(name)=="Main"?1:2;}} FuncTab;
    bool BC_Execute(uint32_t function,DATA *&out){
        out=nullptr;if(!executeSucceeds)return false;
        if(function==1){mainDone=true;order.push_back("Main returned");}
        else{assert(baseRestored);onLoadDone=true;order.push_back("OnLoad returned");}
        return true;
    }
    bool BC_SegmentIsLoaded(const char *name){assert(std::string_view(name)=="interface/android_ui_bootstrap.c");return loaded;}
    bool BC_LoadSegment(const char *name){
        assert(mainDone||onLoadDone);assert(std::string_view(name)=="interface/android_ui_bootstrap.c");
        ++compiles;order.push_back("load adapter");loaded=handler=compileSucceeds;return compileSucceeds;
    }
    void ResetAndroidUiAdapter();void LoadAndroidUiAdapter();bool Run();bool OnLoad();
    // Interpreter/file boundaries only: LoadState's production Release destroys
    // EventTab/segments, then restores the saved base before calling OnLoad.
    void releasedTables(){ResetAndroidUiAdapter();loaded=handler=mainDone=onLoadDone=baseRestored=false;}
    void restoreBase(){assert(!loaded&&!handler);baseRestored=true;}
    void clearEvents(){/* static handlers survive ClearEvents, unlike Release */}
};
// PRODUCTION_METHODS
using namespace storm::android;
static std::string action(const char *verb,const char *id=""){
    return "1\t"+std::to_string(landCommands.sceneEpoch())+"\t"+std::to_string(landCommands.revision())+"\t"+
        std::to_string(landCommands.activityGeneration())+"\t"+verb+"\t"+id;
}
static LandCommandsFrame frame(){LandCommandsFrame f;f.available=f.active=true;f.ownerIdentity="native-list";f.selectedItemId="i0";
    f.items={{"i0","command","Talk","BI_DialogStart","target",true,1},{"i1","cancel","Cancel","","cancel",true,1}};return f;}
int main(){
    COMPILER c;c.Run();assert(c.handler&&c.compiles==1&&c.order[0]=="Main returned"&&c.order[1]=="load adapter");
    c.clearEvents();for(int i=0;i<1000;++i)c.LoadAndroidUiAdapter();assert(c.handler&&c.compiles==1); // new game within same program
    landCommands.resetActivity();landCommands.update("same-land-save",frame());
    auto oldAction=action("activateItem","i0");
    assert(landCommands.offer(oldAction,12));
    c.releasedTables();assert(!c.handler&&!landCommands.frame().available&&!landCommands.take(13));
    assert(!landCommands.offer(oldAction,13));
    c.restoreBase();c.OnLoad();assert(c.handler&&c.compiles==2&&c.order[c.order.size()-2]=="OnLoad returned");
    landCommands.update("same-land-save",frame());assert(!landCommands.offer(oldAction,14));
    for(int i=0;i<3;++i){c.releasedTables();c.restoreBase();c.OnLoad();assert(c.handler);}assert(c.compiles==5); // same/another/repeated save
    c.releasedTables();c.Run();assert(c.handler&&c.compiles==6); // whole-program new game/reset

    // A missing or uncompiled bootstrap stops the start (fatal dialog): it throws once, never retries silently.
    const auto throws=[](auto &&call){try{call();}catch(const std::runtime_error &error){
        return std::string(error.what()).find("android_ui_bootstrap.c")!=std::string::npos;}return false;};
    COMPILER absent;files.exists=false;int before=files.probes;assert(throws([&]{absent.Run();}));
    assert(!absent.handler&&absent.compiles==0&&files.probes==before+1);
    files.exists=true;for(int i=0;i<1000;++i)absent.LoadAndroidUiAdapter();assert(absent.compiles==0&&files.probes==before+1);
    COMPILER absentOnLoad;files.exists=false;absentOnLoad.restoreBase();assert(throws([&]{absentOnLoad.OnLoad();}));files.exists=true;
    COMPILER malformed;malformed.compileSucceeds=false;assert(throws([&]{malformed.Run();}));
    for(int i=0;i<1000;++i)malformed.LoadAndroidUiAdapter();assert(malformed.compiles==1&&!malformed.handler);
    COMPILER already;already.loaded=already.handler=true;before=files.probes;already.Run();assert(already.compiles==0&&files.probes==before);
    COMPILER failed;failed.executeSucceeds=false;failed.Run();assert(failed.compiles==0&&!failed.androidUiAdapterAttempted_);
    std::cout<<"PASS production compiler hooks: startup/new game, static ClearEvents, base-only save restore/repeat, fresh owner tokens, missing/uncompiled bootstrap throws once (fatal dialog), no duplicate load\n";
}
