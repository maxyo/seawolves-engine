#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,sys
root=Path(__file__).resolve().parents[2]
fx=root/'src/libs/renderer_bgfx/fx'
with tempfile.TemporaryDirectory(prefix='cpu-admission-') as tmp:
 p=Path(tmp)
 production_bake=fx/'src/bakes.cpp'
 bake=p/'two-core-bakes.cpp';bake.write_text(production_bake.read_text().replace('static CpuAdmission admission;','static CpuAdmission admission(2);'))
 if '--baseline' in sys.argv:
  old=subprocess.check_output(['git','show','35190edf:src/libs/renderer_bgfx/fx/src/bakes.cpp'],cwd=root,text=True)
  bake=p/'old.cpp';bake.write_text('#include "storm/fx/cpu_admission.hpp"\nnamespace storm::fx {CpuAdmission &SharedCpuAdmission(){static CpuAdmission p(2);return p;}}\n'+old)
 stub=p/'log.cpp';stub.write_text('#include "storm/fx/log.hpp"\nnamespace storm::fx {void Log(LogLevel,const char*,...) {}}\n')
 exe=p/'check'
 subprocess.run(['c++','-std=c++20','-O0','-pthread','-I'+str(fx/'include'),str(Path(__file__).with_name('check.cpp')),str(bake),str(stub),'-o',str(exe)],check=True)
 r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=8)
 if '--baseline' in sys.argv:
  assert r.returncode!=0 and 'actual bake work must wait for inference permits' in r.stderr,r.stderr
  print('actual old bake admission negative PASS')
 else:
  assert r.returncode==0,r.stderr
  subprocess.run([str(exe),'--exit-waiting-bake'],check=True,timeout=3)
  print('actual bake/nested + concurrent FIFO/cancel/phase/invalid-request + two-core boost/exit PASS')

 # One real static provider archive is linked into an engine-like shared object;
 # two independently loaded consumer DSOs must resolve the same out-of-line state.
 if '--baseline' not in sys.argv:
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
 # method with inert ORT/model teardown doubles (not an inference speed test).
 if '--baseline' not in sys.argv:
  backend=(root/'src/libs/relief/backend.cpp').read_text()
  start=backend.index('        const auto admission_started =')
  prefix=backend[start:backend.index('        const int64_t shape[4]',start)]
  prepare=(root/'src/libs/relief/prepare.cpp').read_text()
  start=prepare.index('void Session::SetCpuBudget(')
  method=prepare[start:prepare.index('\nvoid Session::ReleaseIdleModels()',start)]
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
double Ms(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
struct Timing {double admission_wall_ms=0;};
int active=0;
struct CpuRun {
 Timing cpu_timing_;
 std::function<std::shared_ptr<void>()> cpu_admission_;
 bool Run(std::string &error) {
 PREFIX
 assert(!cpu_admission_ || active==1);return true;
 }
};
struct Session {
 struct Impl {int threads=1,releases=0;std::unique_ptr<int> encoder=std::make_unique<int>();double cpu_ms_per_texel=1,upscale_cpu_ms_per_texel=2;void ReleaseCpu(){++releases;}};
 std::unique_ptr<Impl> impl_=std::make_unique<Impl>();
 void SetCpuBudget(unsigned);
};
METHOD
std::shared_ptr<void> initial_open(unsigned threads,std::function<bool()> cancelled){bool gpu_models=true;std::string error;
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
 assert(initial.wait_for(std::chrono::milliseconds(500))==std::future_status::ready&&!initial.get());all.reset();
 Session s;s.SetCpuBudget(1);assert(s.impl_->releases==0);
 s.SetCpuBudget(2);assert(s.impl_->releases==1&&!s.impl_->encoder&&s.impl_->threads==2&&s.impl_->cpu_ms_per_texel==0);
 s.SetCpuBudget(2);assert(s.impl_->releases==1);
 s.SetCpuBudget(1);assert(s.impl_->releases==2&&s.impl_->threads==1);
}
'''.replace('PREFIX',prefix).replace('METHOD',method).replace('PROBE',probe_prefix)
  source=p/'hook.cpp';source.write_text(fixture);binary=p/'hook'
  subprocess.run(['c++','-std=c++20','-O0','-pthread','-I'+str(fx/'include'),str(source),str(production_bake),str(stub),'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True,timeout=3)
  import hashlib
  print('actual CPU Run hook + Session boundary/no-op controls PASS; backend sha256 '+hashlib.sha256(backend.encode()).hexdigest())
