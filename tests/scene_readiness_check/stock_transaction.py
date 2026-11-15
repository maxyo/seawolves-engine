#!/usr/bin/env python3
"""Compile actual stock transaction/core/render boundaries; no GPU or game corpus.
The baseline uses the old actual native completion function and must fail readiness
before release. Encoder/resource readiness/platform pump are explicit test doubles.
"""
from pathlib import Path
import subprocess, tempfile, sys
root=Path(__file__).resolve().parents[2]
def extract(text, signature):
    start=text.index(signature); brace=text.index('{',start); end=brace+1; depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
core=(root/'src/libs/core/src/core_impl.cpp').read_text()
renderer=(root/'src/libs/renderer/src/renderer.cpp').read_text()
native=(root/'src/libs/location/src/location_script_lib.cpp').read_text()
if len(sys.argv)>1 and sys.argv[1]=='--baseline':
    native=subprocess.check_output(['git','show','e052f656:src/libs/location/src/location_script_lib.cpp'],cwd=root,text=True)
method=extract(core,'void CoreImpl::PrepareLoadingScene()')
assert 'Stage::realize' not in method and 'Stage::execute' not in method
assert 'ProcessFrame(' not in method and 'ProcessExecute(' not in method and 'ProcessRealize(' not in method
assert 'SetLayerFrozen' not in method and 'SetLayerHeld' not in method
camera=extract((root/'src/libs/location/src/location_camera.cpp').read_text(),'void LocationCamera::PrimeLoadingCamera()')
assert 'Controls' not in camera and 'Execute(0' not in camera and 'SetCamera(' in camera
animation=extract((root/'src/libs/animation/src/animation_service_imp.cpp').read_text(),'void AnimationServiceImp::RunStart()')
warm=animation[animation.index('if (storm::loading::active)'):animation.index('if (core.Controls')]
assert 'BuildAnimationMatrices()' in warm and '->Execute(' not in warm
source=r'''
#include <cassert>
#include <chrono>
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include "loading_transaction.hpp"
int frames=0,draws=0,presents=0,releases=0,pumps=0,pose=0,prime=0,danger=0;
int pendingUntil=4,suspendFor=0,cancelAt=0;bool error=false,ack=false,prepared=false;
namespace storm::render {
struct Status {bool active=true;uint64_t generation=7;}; Status status;
Status GetSceneLoadStatus(){return status;}
bool HoldSceneReveal(){assert(prepared);return error||frames<pendingUntil;}
void CompleteSceneReveal(){assert(!error&&frames>=pendingUntil);status.active=false;}
}
namespace storm::platform {
enum class LoadingPlatformState {Continue,Suspended,Cancel,Quit,Unavailable};
struct LoadingPlatformTransaction{};
LoadingPlatformState PumpLoadingPlatform(uint64_t g){
 assert(g==7);++pumps;
 if(cancelAt&&pumps==cancelAt){ack=true;return LoadingPlatformState::Cancel;}
 if(pumps<=suspendFor)return LoadingPlatformState::Suspended;
 return LoadingPlatformState::Continue;
}
}
void SDL_Delay(int){}
struct TIMER {std::chrono::high_resolution_clock::time_point Current,Previous;double Carry=99;};
struct SERVICE {virtual void RunStart(){};virtual void RunEnd(){};virtual ~SERVICE()=default;};
struct Renderer: SERVICE {
 int progressTexture=1,progressSafeCounter=0;bool bInsideScene=false;
 void RunStart()override{++frames;prepared=true;}
 void RunEnd()override;
 void DrawProgressView(bool p){assert(!p&&prepared);++draws;}
 void DrawFrameStats(){assert(draws==frames);++presents;prepared=false;}
 void EndProgressView();
 void ReleaseProgressView(){assert(!storm::render::status.active);++releases;progressTexture=-1;}
} renderer;
using VRenderer=Renderer;
struct Animation:SERVICE {void RunStart()override{assert(storm::loading::active);++pose;}} animation;
enum class layer_type_t {realize};
struct Entity {
 enum class Stage {loading_prepare,loading_realize,realize,execute};
 bool resource; int id; int GetId()const{return id;}
 void ProcessStage(Stage s,unsigned dt){
  assert(dt==0);
  if(s==Stage::realize||s==Stage::execute)++danger;
  if(resource&&s==Stage::loading_prepare)++prime;
 }
} resource{true,0},dialog{false,1},weather{true,2};
using entid_t=int;using entptr_t=Entity*;
struct CoreImpl {
 TIMER Timer;
 void PrepareLoadingScene();
 void *GetService(const char*s){return std::string(s)=="renderer"?static_cast<SERVICE*>(&renderer):static_cast<SERVICE*>(&animation);}
 std::vector<int> GetEntityIds(layer_type_t){return {0,1};}
 Entity *GetEntityPointerSafe(int id){return id==2?&weather:id?&dialog:&resource;}
 void ForEachEntity(const std::function<void(Entity*)>&f){f(&resource);f(&dialog);f(&weather);}
} core;
struct VS_STACK{};constexpr unsigned IFUNCRESULT_OK=0;
METHOD
RENDER_END
END_PROGRESS
NATIVE
void reset(){
 frames=draws=presents=releases=pumps=pose=prime=danger=0;
 pendingUntil=4;suspendFor=cancelAt=0;error=ack=prepared=false;
 storm::render::status={true,7};renderer.progressTexture=1;
}
int main(){
 reset();slNativeReloadProgressEnd(nullptr);
 assert(frames>=4&&frames==draws&&frames==presents&&pose==frames&&prime==2*frames);
 assert(releases==1&&!danger&&!storm::loading::active&&core.Timer.Carry==0);
 reset();suspendFor=3;slNativeReloadProgressEnd(nullptr);
 assert(pumps==frames+3&&releases==1&&!danger);
 reset();error=true;cancelAt=3;bool continued=false;
 try {slNativeReloadProgressEnd(nullptr);continued=true;}
 catch(const storm::loading::Aborted&a){assert(a.outcome==storm::loading::Outcome::Cancel&&ack);}
 assert(!continued&&releases==0&&frames==2&&draws==2&&presents==2&&!danger);
 assert(!storm::loading::active&&core.Timer.Carry==0);
 reset();storm::loading::active=true;bool reentrant=false;
 try{core.PrepareLoadingScene();}catch(const storm::loading::Aborted&a){reentrant=a.outcome==storm::loading::Outcome::Reentrant;}
 assert(reentrant&&frames==0);storm::loading::active=false;
}
'''
source=source.replace('METHOD',method).replace('RENDER_END',extract(renderer,'void Renderer::RunEnd()')).replace('END_PROGRESS',extract(renderer,'void Renderer::EndProgressView()')).replace('NATIVE',extract(native,'uint32_t slNativeReloadProgressEnd('))
with tempfile.TemporaryDirectory(prefix='stock-transaction-') as tmp:
    p=Path(tmp);(p/'check.cpp').write_text(source)
    subprocess.run(['c++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(root/'src/libs/util/include'),str(p/'check.cpp'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True)
print('stock transaction actual core/native/renderer control PASS')
