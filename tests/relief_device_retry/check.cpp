#include "device_retry.hpp"
#include "loading_retry.hpp"
#include <cstdio>
#include <cstdlib>
using namespace storm::relief;
void require(bool v,const char *s) { if(!v){std::fprintf(stderr,"FAIL: %s\n",s);std::exit(1);} }
int main() {
    LoadingRetry scoped(500,42), renewed(550,42), next(600,43);
    require(scoped.generation==renewed.generation && next.generation!=scoped.generation,
        "deadline renewal preserves loading generation; replacement scene changes it");
    require(scoped.deadline_ms==500 && renewed.deadline_ms==550,
        "generation identity does not extend an expired lease");
    LoadingRetry lease(500);
    require(lease.generation==0,"legacy retry permission grants no tile-recovery epoch");
    require(!lease.take(100,true,false,0,0),"loading window cannot probe unmeasured x2");
    require(lease.take(100,true,false,1,0),"measured loading job can retry without gpu_free");
    require(!lease.take(101,true,false,1,0),"only one job consumes window");
    LoadingRetry expired(100);require(!expired.take(100,true,false,1,0),"deadline refuses late decode");
    LoadingRetry revoked(500);revoked.active=false;require(!revoked.take(100,true,false,1,0),"cancel/generation revoke before decode");
    LoadingRetry cooldown(60500);require(!cooldown.take(60000,true,false,1,100),"failed loading retry retains 60s cooldown");
    require(cooldown.take(60100,true,false,1,100),"cooldown opens once due");
    LoadingRetry current(500);require(!current.take(100,true,true,1,0),"already GPU needs no retry token");
    require(!current.take(100,false,false,1,0),"missing GPU cannot use token");
    // Background admission and either texture lane must agree on the same bounded retry.
    require(WaitForGpuRetry(true,false,false,59.9,7,0),"busy game defers background before deadline");
    require(!WaitForGpuRetry(true,false,false,60.,0,0),"background admission opens at60s without loaded requests");
    require(UseGpuForTexture(true,false,false,60.,0,0),"a due background texture runs onGPU");
    require(UseGpuForTexture(true,false,false,1.,8,0),"eight CPU x2-only jobs trigger GPUretry");
    require(!WaitForGpuRetry(true,false,false,1.,8,0),"eight CPUjobs cannot be hidden by admission");
    require(UseGpuForTexture(true,false,true,0.,0,0),"idle renderer retries immediately");
    require(!UseGpuForTexture(false,false,true,600.,80,0),"crash or missing device never retried");
    require(!WaitForGpuRetry(false,false,false,0.,0,0),"CPU-only device does not starve background");
    // Retries that stayed slow while the game drew back off: 60 s, 120 s, ... 16 min (8, 16, ... 128 textures).
    require(!UseGpuForTexture(true,false,false,60.,0,1),"one slow in-scene retry doubles the wait");
    require(UseGpuForTexture(true,false,false,120.,0,1),"the doubled wait opens at 120 s");
    require(!UseGpuForTexture(true,false,false,1.,15,1),"and at 16 textures, not 8");
    require(!UseGpuForTexture(true,false,false,959.,0,9),"the wait stops growing at 16x: not before 960 s");
    require(UseGpuForTexture(true,false,false,960.,0,9),"the wait stops growing at 16x: 960 s");
    require(UseGpuForTexture(true,false,true,0.,0,4),"a free GPU is retried at once whatever the backoff");
    require(WaitForGpuRetry(true,false,false,60.,0,2),"background waits for the backed-off retry too");
    require(UseGpuForTexture(true,true,false,0.,0,0),"current GPU work stays GPU");
    require(!WaitForGpuRetry(true,true,false,0.,0,0),"current GPU does not block background");
    require(!UseGpuForTexture(true,false,false,-1.,0,0),"clock before detour does not bypass busy gate");
    std::puts("relief device retry: background deadline, x2-only quota, idle and CPU-only controls PASS");
}
