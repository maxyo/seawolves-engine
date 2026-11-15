#!/usr/bin/env python3
"""Exercise the actual host controller/registration with renderer receipts doubled.

No window, GPU, model or game corpus. A submitted opaque frame and GPU timing are
explicit inputs; this does not claim physical GPU completion or visual acceptance.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
main = (root / 'src/apps/engine/src/main.cpp').read_text()
start = main.index('auto loadingSchedule =')
brace = main.index('{', start)
end, depth = brace + 1, 1
while depth:
    depth += (main[end] == '{') - (main[end] == '}')
    end += 1
controller = main[start:end] + ';'
source = r'''
#include <cassert>
#include <limits>
#include "platform/host_hooks.hpp"
#include "storm/render/loading_budget.hpp"
namespace storm::render {
struct Status {bool active=true;uint64_t generation=7;} status;
struct Budget {uint64_t generation,measured_at_ms;double gpu_ms;bool eligible;};
LoadingOpaque opaque;
uint64_t now=1000,measured=1000,windowGeneration=0,windowUntil=0;
double gpu=8;bool required=true;
Status GetSceneLoadStatus(){return status;}
Budget GetSceneLoadingBudget(){return {status.generation,measured,gpu,
    status.active&&required&&opaque.presented(status.generation)};}
void SetSceneLoadingWindow(uint64_t generation,uint64_t deadline){
    windowGeneration=generation;windowUntil=deadline;
}
void present(){
    opaque.arm(status.generation,true,true,255);
    opaque.submitted(status.generation);
    opaque.frame(status.generation,true,true);
}
}
int main(){
 using namespace storm::render;
 using namespace storm::platform;
 using Phase=LoadingSchedulePhase;
 LoadingBudget loadingBudget;
 auto loadingClock=[] {return now;};
 uint64_t loadingScheduleGeneration=0;
 CONTROLLER
 LoadingScheduleRegistration registration(loadingSchedule);
 // The host hook has exactly one owner, and is unavailable outside a native
 // transaction. The outer loop invokes the same controller directly.
 assert(PumpLoadingSchedule(7,Phase::Poll)==0&&loadingScheduleGeneration==0);
 {
  LoadingPlatformTransaction transaction;
  auto pump=[&](Phase p){return PumpLoadingSchedule(status.generation,p);};
  assert(pump(Phase::FramePresented)==0&&!windowUntil); // no frame start
  pump(Phase::FrameStarted);now=1004;
  pump(Phase::FramePresented);assert(!windowUntil); // request is not opaque
  present();pump(Phase::FramePresented);
  assert(windowGeneration==7&&windowUntil==1016&&pump(Phase::Poll)==2);
  now=1015;assert(pump(Phase::Poll)==1);
  now=1016;assert(pump(Phase::Poll)==0&&!windowUntil); // backend must progress
  now=1020;pump(Phase::FrameStarted);now=1024;pump(Phase::FramePresented);
  assert(windowGeneration==7&&windowUntil==1036);
  pump(Phase::Suspended);assert(!windowUntil&&loadingScheduleGeneration==0);
  now=1025;assert(pump(Phase::Poll)==0&&!windowUntil);
  pump(Phase::FrameStarted);now=1028;pump(Phase::FramePresented);
  assert(windowGeneration==7&&windowUntil==1041);
  // Replacement revokes the old opportunity. A late old-generation Ended may
  // not revoke the replacement transaction's independently granted window.
  status.generation=8;opaque.invalidate();measured=now;
  pump(Phase::FrameStarted);now=1030;pump(Phase::FramePresented);
  assert(!windowUntil);
  present();pump(Phase::FramePresented);assert(windowGeneration==8);
  const auto replacementUntil=windowUntil;
  PumpLoadingSchedule(7,Phase::Ended);assert(windowUntil==replacementUntil);
  PumpLoadingSchedule(7,Phase::Poll);assert(windowUntil==replacementUntil);
  // Unsupported/stale timing and absent required work never create an
  // indefinite hold. They return control to a real resource/render frame.
  gpu=std::numeric_limits<double>::quiet_NaN();assert(pump(Phase::Poll)==0&&!windowUntil);
  gpu=8;measured=1;assert(pump(Phase::Poll)==0&&!windowUntil);
  measured=now;required=false;assert(pump(Phase::Poll)==0&&!windowUntil);
  required=true;assert(pump(Phase::Poll)>0);
  status.active=false;pump(Phase::Poll);assert(!windowUntil&&loadingScheduleGeneration==0);
  int innerCalls=0;
  {
   LoadingScheduleRegistration inner([&](uint64_t,Phase){++innerCalls;return 1;});
   assert(PumpLoadingSchedule(8,Phase::Poll)==1&&innerCalls==1);
  }
  assert(PumpLoadingSchedule(8,Phase::Poll)==0&&innerCalls==1);
 }
 assert(!LoadingPlatformTransactionActive());
 assert(PumpLoadingSchedule(8,Phase::Poll)==0);
}
'''.replace('CONTROLLER', controller)
with tempfile.TemporaryDirectory(prefix='stock-scheduler-') as tmp:
    path = Path(tmp)
    (path / 'check.cpp').write_text(source)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(root / 'src/libs/util/include'),
                    '-I' + str(root / 'src/libs/renderer_bgfx/include'),
                    str(path / 'check.cpp'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True)
print('stock scheduler actual host/controller control PASS')
