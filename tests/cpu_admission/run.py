#!/usr/bin/env python3
"""Actual CPU admission governor, bake helpers and Run/Session admission prefixes. The checks wait on progress
only; DEADLOCK_GUARD is not a time budget but the point at which a waiter that cannot progress is reported (with
the last step it printed)."""
from pathlib import Path
import subprocess,tempfile,sys
DEADLOCK_GUARD=300 # seconds; an idle run of each binary takes well under 1 s
def guarded(command):
 try:r=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=DEADLOCK_GUARD)
 except subprocess.TimeoutExpired as hung:
  steps=(hung.stdout or b'').decode(errors='replace').strip().splitlines()
  sys.exit(f'FAIL: {Path(command[0]).name}: no progress for {DEADLOCK_GUARD} s (an admission deadlock) at {steps[-1] if steps else "start"}')
 assert r.returncode==0,(command,r.returncode,r.stdout)
root=Path(__file__).resolve().parents[2]
fx=root/'src/libs/renderer_bgfx/fx'
with tempfile.TemporaryDirectory(prefix='cpu-admission-') as tmp:
 p=Path(tmp)
 production_bake=fx/'src/bakes.cpp'
 bake=p/'two-core-bakes.cpp';bake.write_text(production_bake.read_text().replace('static CpuAdmission admission;','static CpuAdmission admission(2);'))
 stub=p/'log.cpp';stub.write_text('#include "storm/fx/log.hpp"\nnamespace storm::fx {void Log(LogLevel,const char*,...) {}}\n')
 exe=p/'check'
 subprocess.run(['c++','-std=c++20','-O0','-pthread','-I'+str(fx/'include'),str(Path(__file__).with_name('check.cpp')),str(bake),str(stub),'-o',str(exe)],check=True)
 guarded([str(exe)])
 guarded([str(exe),'--exit-waiting-bake'])
 print('actual bake/nested + concurrent FIFO/cancel/phase/invalid-request + two-core boost/exit PASS')

 # One real static provider archive is linked into an engine-like shared object;
 # two independently loaded consumer DSOs must resolve the same out-of-line state.
 import ctypes
 obj=p/'bakes.o';logobj=p/'log.o';archive=p/'budget.a';provider=p/'libbudget.so'
 flags=['c++','-std=c++20','-O0','-pthread','-fPIC','-I'+str(fx/'include')]
 for source,target in [(production_bake,obj),(stub,logobj)]:
  subprocess.run(flags+['-c',str(source),'-o',str(target)],check=True)
 subprocess.run(['ar','rcs',str(archive),str(obj),str(logobj)],check=True)
 subprocess.run(['c++','-shared','-pthread','-Wl,--whole-archive',str(archive),'-Wl,--no-whole-archive','-o',str(provider)],check=True)
 consumer=p/'consumer.cpp';consumer.write_text('#include "storm/fx/cpu_admission.hpp"\nextern "C" void* identity(){return &storm::fx::SharedCpuAdmission();}\n')
 libs=[]
 for n in ['one','two']:
  lib=p/(n+'.so');subprocess.run(flags+['-shared',str(consumer),'-L'+str(p),'-lbudget','-Wl,-rpath,'+str(p),'-o',str(lib)],check=True)
  libs.append(ctypes.CDLL(str(lib)))
 for lib in libs: lib.identity.restype=ctypes.c_void_p
 assert libs[0].identity()==libs[1].identity(), 'DSO consumers must share admission identity'
 print('actual static provider + two DSO consumer singleton identity PASS')
 # Execute the exact production CPU Run admission prefix and Session budget
 # method with inert ncnn/model teardown doubles (not an inference speed test).
 backend=(root/'src/libs/relief/backend.cpp').read_text()
 start=backend.index('        auto cpu_lease =')
 prefix=backend[start:backend.index('        ncnn::Mat in(width, height, 3);',start)]
 prepare=(root/'src/libs/relief/prepare.cpp').read_text()
 start=prepare.index('void Session::SetCpuBudget(')
 method=prepare[start:prepare.index('\nvoid Session::SetGame(',start)]
 probe_start=prepare.index('    auto probe_permit =')
 probe_prefix=prepare[probe_start:prepare.index('    const Device device',probe_start)]
 fixture=r'''
#include <cassert>
#include <memory>
#include <functional>
#include <string>
#include <chrono>
#include <future>
#include <atomic>
#include "storm/fx/cpu_admission.hpp"
namespace fx=storm::fx;
using Clock=std::chrono::steady_clock;
void Log(const char*,...){}
int active=0;
struct CpuRun {
 std::function<std::shared_ptr<void>()> cpu_admission_;
 bool Run(std::string &error) {
 PREFIX
 assert(!cpu_admission_ || active==1);return true;
 }
};
struct Session {
 struct Runtime {int threads=0;void SetThreads(int n){threads=n;}};
 struct Impl {int threads=1;std::unique_ptr<int> synchronous_finalizer=std::make_unique<int>();std::mutex networks_mutex;std::unique_ptr<Runtime> runtime=std::make_unique<Runtime>();};
 std::unique_ptr<Impl> impl_=std::make_unique<Impl>();
 void SetCpuBudget(unsigned);
};
METHOD
std::shared_ptr<void> initial_open(unsigned threads,std::function<bool()> cancelled){std::string error;
PROBE
return probe_permit;
}
int main(){
 std::string error;CpuRun r;assert(r.Run(error));
 r.cpu_admission_=[] {++active;return std::shared_ptr<void>(new int,[&](void*p){delete static_cast<int*>(p);--active;});};
 assert(r.Run(error));assert(active==0);
 r.cpu_admission_=[] {return std::shared_ptr<void>{};};
 assert(!r.Run(error)&&error=="CPU admission cancelled");
 auto &g=fx::SharedCpuAdmission();auto all=g.acquire(g.threads(),{});std::atomic<bool> stop=false;
 auto initial=std::async(std::launch::async,[&]{return initial_open(g.threads(),[&]{return stop.load();});});
 while(!g.queued())std::this_thread::yield();stop=true;
 assert(!initial.get());all.reset();
 Session s;auto *finalizer=s.impl_->synchronous_finalizer.get();
 s.SetCpuBudget(1);s.SetCpuBudget(0);
 assert(s.impl_->runtime->threads==0&&s.impl_->synchronous_finalizer.get()==finalizer&&s.impl_->threads==1);
 s.SetCpuBudget(2);
 // The loaded networks take the new count (no reload).
 assert(s.impl_->runtime->threads==2&&!s.impl_->synchronous_finalizer&&s.impl_->threads==2);
 // Reopened synchronous encoder resources survive a no-op budget and are released
 // by the actual production method on the next pool-size change.
 s.impl_->synchronous_finalizer=std::make_unique<int>();finalizer=s.impl_->synchronous_finalizer.get();
 s.SetCpuBudget(2);
 assert(s.impl_->runtime->threads==2&&s.impl_->synchronous_finalizer.get()==finalizer);
 s.SetCpuBudget(1);
 assert(s.impl_->runtime->threads==1&&s.impl_->threads==1&&!s.impl_->synchronous_finalizer);
 s.impl_->runtime.reset();s.SetCpuBudget(4); // no CPU networks loaded: only the count
 assert(s.impl_->threads==4);
}
'''.replace('PREFIX',prefix).replace('METHOD',method).replace('PROBE',probe_prefix)
 source=p/'hook.cpp';source.write_text(fixture);binary=p/'hook'
 subprocess.run(['c++','-std=c++20','-O0','-pthread','-I'+str(fx/'include'),str(source),str(production_bake),str(stub),'-o',str(binary)],check=True)
 guarded([str(binary)])
 import hashlib
 print('actual CPU Run hook + Session finalizer-reset/budget boundary/no-op controls PASS; backend sha256 '+hashlib.sha256(backend.encode()).hexdigest())
subprocess.run([sys.executable,str(Path(__file__).with_name('run_admission.py'))],check=True)
