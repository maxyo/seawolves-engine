#!/usr/bin/env python3
"""Compile actual stock transaction/core/render boundaries; no GPU or game corpus.
The baseline uses the old actual native completion function and must fail readiness
before release. Encoder/resource readiness/platform pump are explicit test doubles.
"""
from pathlib import Path
import os, subprocess, tempfile, sys
root=Path(__file__).resolve().parents[2]
def extract(text, signature):
    start=text.index(signature); brace=text.index('{',start); end=brace+1; depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
core=(root/'src/libs/core/src/core_impl.cpp').read_text()
renderer=(root/'src/libs/renderer/src/renderer.cpp').read_text()
native=(root/'src/libs/location/src/location_script_lib.cpp').read_text()
method=extract(core,'bool CoreImpl::PrepareLoadingScene()')
abort=extract(core,'bool CoreImpl::AbortLoadingTransaction(')
source=r'''
#include <cassert>
#include <chrono>
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include "loading_transaction.hpp"
#include "platform/host_hooks.hpp"
int frames=0,draws=0,presents=0,releases=0,pumps=0,pose=0,prime=0,danger=0;
int pendingUntil=4,suspendFor=0,cancelAt=0;bool error=false,ack=false,prepared=false;
int scheduledStarts=0,scheduledPresents=0,scheduledEnds=0,scheduledSuspends=0;
int waitPolls=0,waits=0,cpuEnds=0,cpuSuspends=0;bool invalidateAfterPresent=false;
namespace storm::render {
struct Status {bool active=true;uint64_t generation=7;std::string state,phase,detail;uint32_t textures=0,relief=0,shadows=0,failed=0;}; Status status;
Status GetSceneLoadStatus(){return status;}
void SceneLoadingTransactionFrame(){}
bool HoldSceneReveal(){assert(prepared);return error||frames<pendingUntil;}
void CompleteSceneReveal(){assert(!error&&frames>=pendingUntil);status.active=false;}
void EndSceneCpuBudget(uint64_t g){assert(g==7);++cpuEnds;}
void SuspendSceneCpuBudget(uint64_t g){assert(g==7);++cpuSuspends;}
}
storm::platform::LoadingPlatformState hostPump(uint64_t g){
 using storm::platform::LoadingPlatformState;
 assert(g==7);++pumps;
 if(cancelAt&&pumps==cancelAt){ack=true;return LoadingPlatformState::Cancel;}
 if(pumps<=suspendFor)return LoadingPlatformState::Suspended;
 return LoadingPlatformState::Continue;
}
unsigned schedulePump(uint64_t g,storm::platform::LoadingSchedulePhase phase){
 using Phase=storm::platform::LoadingSchedulePhase;
 assert(g==7&&storm::platform::LoadingPlatformTransactionActive());
 if(phase==Phase::Poll&&waitPolls){--waitPolls;return 5;}
 if(phase==Phase::FrameStarted){assert(!prepared);++scheduledStarts;}
 if(phase==Phase::FramePresented){
  assert(!prepared&&presents==frames);++scheduledPresents;
  if(invalidateAfterPresent)storm::render::status.generation=8;
 }
 if(phase==Phase::Suspended)++scheduledSuspends;
 if(phase==Phase::Ended)++scheduledEnds;
 return 0;
}
void SDL_Delay(unsigned ms){assert(ms<=2);++waits;}
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
char last_trace[512];std::string all_traces;
struct Vm {int aborts=0;void AbortExecution(){++aborts;}} vm;
struct CoreImpl {
 void Trace(const char *format, ...) {va_list a;va_start(a,format);std::vsnprintf(last_trace,sizeof(last_trace),format,a);va_end(a);all_traces+=last_trace;all_traces+='\n';}
 TIMER Timer;
 Vm *Compiler=&vm;
 bool Exit_flag=false;
 static inline std::chrono::seconds kLoadingStall{300}; // the fixture can shorten it
 bool PrepareLoadingScene();
 bool AbortLoadingTransaction(storm::loading::Outcome outcome,uint64_t generation);
 void *GetService(const char*s){return std::string(s)=="renderer"?static_cast<SERVICE*>(&renderer):static_cast<SERVICE*>(&animation);}
 std::vector<int> GetEntityIds(layer_type_t){return {0,1};}
 Entity *GetEntityPointerSafe(int id){return id==2?&weather:id?&dialog:&resource;}
 void ForEachEntity(const std::function<void(Entity*)>&f){f(&resource);f(&dialog);f(&weather);}
} core;
struct VS_STACK{};constexpr unsigned IFUNCRESULT_OK=0,IFUNCRESULT_FAILED=1;
METHOD
ABORT
RENDER_END
END_PROGRESS
NATIVE
void reset(){
 frames=draws=presents=releases=pumps=pose=prime=danger=0;
 pendingUntil=4;suspendFor=cancelAt=0;error=ack=prepared=false;
 scheduledStarts=scheduledPresents=scheduledEnds=scheduledSuspends=0;
 waitPolls=waits=cpuEnds=cpuSuspends=0;invalidateAfterPresent=false;
 storm::platform::loadingPlatformPump=hostPump;
 storm::platform::loadingSchedulePump=schedulePump;
 storm::render::status={true,7};renderer.progressTexture=1;
 vm.aborts=0;core.Exit_flag=false;last_trace[0]=0;all_traces.clear();
}
// An abort: no exception, the scripts unwound once, the engine exit requested, the outcome in the log.
bool aborted(const char *outcome,bool error){
 const std::string trace=last_trace;
 return vm.aborts==1&&core.Exit_flag&&trace.find(std::string("outcome=")+outcome)!=std::string::npos&&
        (trace.rfind("ERROR: ",0)==0)==error;
}
int main(){
 reset();slNativeReloadProgressEnd(nullptr);
 assert(frames>=4&&frames==draws&&frames==presents&&pose==frames&&prime==2*frames);
 assert(releases==1&&!danger&&!storm::loading::active&&core.Timer.Carry==0);
 assert(scheduledStarts==frames&&scheduledPresents==frames&&scheduledEnds==1&&cpuEnds==1);
 assert(!storm::platform::LoadingPlatformTransactionActive());
 reset();suspendFor=3;slNativeReloadProgressEnd(nullptr);
 assert(pumps==frames+3&&releases==1&&!danger);
 assert(scheduledSuspends==3&&cpuSuspends==3&&scheduledEnds==1);
 reset();waitPolls=3;slNativeReloadProgressEnd(nullptr);
 assert(pumps==frames+3&&waits==3&&scheduledStarts==frames&&scheduledPresents==frames);
 assert(vm.aborts==0&&!core.Exit_flag);
 reset();waitPolls=8;cancelAt=3;
 assert(slNativeReloadProgressEnd(nullptr)==IFUNCRESULT_FAILED);
 assert(aborted("cancelled",false)&&ack&&frames==0&&waits==2&&scheduledEnds==1&&cpuEnds==1);
 reset();error=true;cancelAt=3;
 assert(slNativeReloadProgressEnd(nullptr)==IFUNCRESULT_FAILED&&aborted("cancelled",false)&&ack);
 assert(releases==0&&frames==2&&draws==2&&presents==2&&!danger);
 assert(!storm::loading::active&&core.Timer.Carry==0);
 assert(scheduledEnds==1&&cpuEnds==1&&!storm::platform::LoadingPlatformTransactionActive());
 reset();invalidateAfterPresent=true;
 assert(slNativeReloadProgressEnd(nullptr)==IFUNCRESULT_FAILED&&aborted("invalidated",true));
 assert(frames==1&&releases==0&&scheduledEnds==1&&cpuEnds==1);
 // Missing loading artwork: loudly logged, the location still loads.
 reset();renderer.progressTexture=-1;
 assert(slNativeReloadProgressEnd(nullptr)==IFUNCRESULT_OK&&vm.aborts==0&&!core.Exit_flag);
 assert(frames>=4&&presents==frames&&scheduledEnds==1&&cpuEnds==1&&all_traces.find("ERROR: the stock loading artwork is missing")!=std::string::npos);
 assert(!storm::platform::LoadingPlatformTransactionActive());
 reset();storm::loading::active=true;
 assert(!core.PrepareLoadingScene()&&aborted("reentrant",true)&&frames==0&&scheduledEnds==0);storm::loading::active=false;
 // A preparation that stops progressing is reported once as an ERROR and keeps waiting: never an exit by itself.
 reset();pendingUntil=1000;CoreImpl::kLoadingStall=std::chrono::seconds(0);cancelAt=8;
 assert(slNativeReloadProgressEnd(nullptr)==IFUNCRESULT_FAILED&&aborted("cancelled",false)&&frames>=4);
 {size_t at=all_traces.find("made no progress");assert(at!=std::string::npos&&all_traces.find("made no progress",at+1)==std::string::npos);}
 CoreImpl::kLoadingStall=std::chrono::seconds(300);
}
'''
source=source.replace('METHOD',method).replace('ABORT',abort).replace('RENDER_END',extract(renderer,'void Renderer::RunEnd()')).replace('END_PROGRESS',extract(renderer,'void Renderer::EndProgressView()')).replace('NATIVE',extract(native,'uint32_t slNativeReloadProgressEnd('))
with tempfile.TemporaryDirectory(prefix='stock-transaction-') as tmp:
    p=Path(tmp);(p/'check.cpp').write_text(source)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(root/'src/libs/util/include'),str(p/'check.cpp'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True)
print('stock transaction actual core/native/renderer control PASS')
