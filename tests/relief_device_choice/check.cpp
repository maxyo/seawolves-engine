// The device of a relief texture (device_choice.hpp) and the place's loading window (loading_retry.hpp).
#include "device_choice.hpp"
#include "loading_retry.hpp"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
using namespace storm::relief;
void require(bool v,const char *s) { if(!v){std::fprintf(stderr,"FAIL: %s\n",s);std::exit(1);} std::printf("ok: %s\n",s); }
int main() {
    int windows=0;
    const auto window=[&](bool grant){return [&windows,grant]{++windows;return grant;};};
    const auto choose=[&](bool possible,bool game,bool measured,bool shown,bool free,bool grant){
        return ChooseTextureDevice(possible,game,measured,shown,free,window(grant));};
    // The game is played: the CPU, whatever else holds; the loading window is not even asked.
    DeviceChoice c=choose(true,true,true,true,true,true);
    require(!c.gpu && c.why==DeviceReason::SceneShown && windows==0,"the scene shown: CPU, even with a free GPU and a window");
    c=choose(true,true,false,true,false,false);
    require(!c.gpu && c.why==DeviceReason::SceneShown,"the scene shown: CPU");
    // A loading or an interface screen without a scene, the game in the background.
    c=choose(true,true,false,false,true,false);
    require(c.gpu && c.why==DeviceReason::GpuFree && windows==0,"the GPU free: GPU (whatever was measured)");
    // The scene drawn under the loading cover: the window decides.
    c=choose(true,true,true,false,false,true);
    require(c.gpu && c.why==DeviceReason::LoadingWindow && windows==1,"under the loading cover: GPU in the window");
    c=choose(true,true,true,false,false,false);
    require(!c.gpu && c.why==DeviceReason::GameBusy && windows==2,"under the loading cover, no window: CPU");
    // No usable GPU: never.
    c=choose(false,true,true,false,true,true);
    require(!c.gpu && c.why==DeviceReason::NoGpu,"no usable GPU: CPU");
    c=choose(false,false,true,false,false,false);
    require(!c.gpu && c.why==DeviceReason::NoGpu,"no usable GPU, host tool: CPU");
    // The host tool: the measured device.
    require(choose(true,false,true,true,false,false).gpu && choose(true,false,true,false,false,false).why==DeviceReason::Measured,
            "host tool: measured GPU");
    require(!choose(true,false,false,false,true,true).gpu,"host tool: measured CPU");
    for (DeviceReason why : {DeviceReason::None,DeviceReason::NoGpu,DeviceReason::Measured,DeviceReason::SceneShown,
                             DeviceReason::GameBusy,DeviceReason::GpuFree,DeviceReason::LoadingWindow})
        require(DeviceReasonText(why)[0]!='?',"every reason has its log text");
    // The loading window: one texture, scoped, at most one a minute.
    LoadingRetry scoped(500,42), renewed(550,42), next(600,43);
    require(scoped.generation==renewed.generation && next.generation!=scoped.generation,
        "deadline renewal preserves loading generation; replacement scene changes it");
    require(scoped.deadline_ms==500 && renewed.deadline_ms==550,"generation identity does not extend an expired lease");
    LoadingRetry lease(500);
    require(lease.generation==0,"legacy retry permission grants no tile-recovery epoch");
    require(lease.take(100,true,0),"the window grants one texture");
    require(!lease.take(101,true,0),"only one texture consumes the window");
    LoadingRetry expired(100);require(!expired.take(100,true,0),"deadline refuses a late texture");
    LoadingRetry revoked(500);revoked.active=false;require(!revoked.take(100,true,0),"revoked window grants nothing");
    LoadingRetry cooldown(60500);require(!cooldown.take(60000,true,100),"one window texture a minute");
    require(cooldown.take(60100,true,100),"the next minute's window grants again");
    LoadingRetry missing(500);require(!missing.take(100,false,0),"no usable GPU: the window grants nothing");
    std::puts("relief device choice: scene shown CPU, GPU free/loading window GPU, host measured: PASS");
}
