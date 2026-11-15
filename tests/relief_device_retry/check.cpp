#include "device_retry.hpp"
#include "loading_retry.hpp"
#include <cstdio>
#include <cstdlib>
using namespace storm::relief;
void require(bool v,const char *s) { if(!v){std::fprintf(stderr,"FAIL: %s\n",s);std::exit(1);} }
int main() {
    LoadingRetry lease(500);
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
    require(WaitForGpuRetry(true,false,false,59.9,7),"busy game defers background before deadline");
    require(!WaitForGpuRetry(true,false,false,60.,0),"background admission opens at60s without loaded requests");
    require(UseGpuForTexture(true,false,false,60.,0),"a due background texture runs onGPU");
    require(UseGpuForTexture(true,false,false,1.,8),"eight CPU x2-only jobs trigger GPUretry");
    require(!WaitForGpuRetry(true,false,false,1.,8),"eight CPUjobs cannot be hidden by admission");
    require(UseGpuForTexture(true,false,true,0.,0),"idle renderer retries immediately");
    require(!UseGpuForTexture(false,false,true,600.,80),"crash or missing device never retried");
    require(!WaitForGpuRetry(false,false,false,0.,0),"CPU-only device does not starve background");
    require(UseGpuForTexture(true,true,false,0.,0),"current GPU work stays GPU");
    require(!WaitForGpuRetry(true,true,false,0.,0),"current GPU does not block background");
    require(!UseGpuForTexture(true,false,false,-1.,0),"clock before detour does not bypass busy gate");
    std::puts("relief device retry: background deadline, x2-only quota, idle and CPU-only controls PASS");
}
