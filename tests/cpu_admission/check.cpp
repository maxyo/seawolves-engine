#include "storm/fx/cpu_admission.hpp"
#include "storm/fx/bakes.hpp"
#include <cassert>
#include <cstdlib>
#include <atomic>
#include <future>
#include <chrono>
#include <cstdio>
#include <thread>
using namespace std::chrono_literals;
using storm::fx::CpuAdmission;
// Every positive wait is on progress, never on a clock: a waiter that cannot progress hangs here and run.py's
// deadlock guard reports the last "step:" line. Only "must still be waiting" controls use a short wait_for; load
// can only make those pass more often.
void step(const char *what){std::printf("step: %s\n",what);std::fflush(stdout);}
template<class F> void until(F fn) {
 while(!fn())std::this_thread::sleep_for(100us);
}
int main(int argc,char**) {
 if(argc>1) {
  // Queue is constructed before its worker first needs admission. std::exit
  // deliberately retains the local inference holder: queue teardown must cancel
  // its waiting worker before destroying the governor, not wait for holder release.
  step("exit while a real bake waits for admission");std::atomic<bool> start=false;
  auto ticket=storm::fx::PostBake("exit admission",[&]{until([&]{return start.load();});storm::fx::ParallelRanges(2,2,[](size_t,size_t){},1);});
  auto &g=storm::fx::SharedCpuAdmission();auto all=g.acquire(g.threads(),{});start=true;
  until([&]{return g.queued()>0;});std::exit(0);
 }

 step("opaque phase gating");CpuAdmission first(2);first.begin(11,true);
 auto waiting=std::async(std::launch::async,[&]{assert(first.wait_for_opaque({}));return first.threads();});
 assert(waiting.wait_for(30ms)==std::future_status::timeout);
 first.opaque(10,true);assert(waiting.wait_for(30ms)==std::future_status::timeout);
 first.opaque(11,true);assert(waiting.get()==2); // first selected job opens its pool only now
 first.suspend();assert(first.threads()==1);
 std::atomic<bool> stop=false;
 auto cancelled_phase=std::async(std::launch::async,[&]{return first.wait_for_opaque([&]{return stop.load();});});
 stop=true;step("cancelled opaque wait returns");assert(!cancelled_phase.get());
 // The cancellation poll runs inside the wait, after it has taken its phase revision: the new phase must find it.
 std::atomic<bool> parked=false;
 auto reset_phase=std::async(std::launch::async,[&]{return first.wait_for_opaque([&]{parked=true;return false;});});
 until([&]{return parked.load();});
 assert(reset_phase.wait_for(30ms)==std::future_status::timeout);first.begin(12,true);
 step("new phase releases the old opaque wait");assert(!reset_phase.get());
 first.opaque(12,true);first.end(11);first.suspend(11);assert(first.threads()==2);
 first.end(12);assert(first.wait_for_opaque({})&&first.threads()==1);
 CpuAdmission p(2);assert(p.threads()==1);p.begin(7,true);p.opaque(7,true);assert(p.threads()==2);
 assert(!p.acquire(3,{})); // infeasible request must not poison FIFO
 auto held=p.acquire(2,{});std::atomic<bool> cancel=false;
 auto cancelled=std::async(std::launch::async,[&]{return p.acquire(2,[&]{return cancel.load();});});
 step("FIFO: cancellable, bake and inference waiters queue");until([&]{return p.queued()==1;});
 std::atomic<int> order=0;std::promise<void> release;auto release_future=release.get_future();
 auto bake=std::async(std::launch::async,[&]{auto lease=p.acquire(1,{});assert(++order==1);release_future.wait();});
 until([&]{return p.queued()==2;});
 auto infer=std::async(std::launch::async,[&]{auto lease=p.acquire(2,{});assert(++order==2);});
 until([&]{return p.queued()==3;});
 cancel=true;step("cancelled waiter leaves FIFO");assert(!cancelled.get());
 step("FIFO order: bake, then inference");held.reset();until([&]{return order==1;});release.set_value();
 bake.get();infer.get();
 p.suspend();assert(p.threads()==1);
 p.begin(7,true);p.opaque(7,true);assert(p.threads()==2);p.suspend();assert(p.threads()==1);
 // An immutable old two-thread job may finish after the loading permission ends.
 auto old=p.acquire(2,{});assert(old);old.reset();
 // Production bake consumers share the out-of-line singleton; nested work must
 // run under its parent piece permit, including the helpers' thread-local context.
 auto &global=storm::fx::SharedCpuAdmission();
 step("nested bake work under inference permits");auto all=global.acquire(global.threads(),{});std::atomic<int> pieces=0;
 auto ranges=std::async(std::launch::async,[&]{storm::fx::ParallelRanges(8,2,[&](size_t a,size_t b){
  storm::fx::ParallelRanges(b-a,2,[&](size_t x,size_t y){pieces+=y-x;},1);
 },1);});
 assert(ranges.wait_for(80ms)==std::future_status::timeout && "actual bake work must wait for inference permits");
 all.reset();ranges.get();assert(pieces==8);
 // A cancelled real bake queued behind inference must leave FIFO promptly,
 // without requiring the inference holder to release first.
 step("cancelled real bake leaves FIFO");all=global.acquire(global.threads(),{});std::atomic<bool> ran=false;
 auto ticket=storm::fx::PostBake("cancel admission",[&]{storm::fx::ParallelRanges(8,2,[&](size_t,size_t){ran=true;},1);});
 until([&]{return global.queued()>0;});ticket.reset();until([&]{return global.queued()==0;});
 assert(!ran);all.reset();storm::fx::ShutdownBakes();
 // Real production helpers exist before the opaque grant, but only one piece
 // may execute under gameplay capacity on the injected two-core device.
 assert(storm::fx::BakeThreads()==2);
 step("opaque grant boosts real bake helpers");global.begin(33,true);std::atomic<int> active=0,peak=0;std::atomic<bool> finish=false;
 auto boosted=std::async(std::launch::async,[&]{storm::fx::ParallelRanges(2,storm::fx::BakeThreads(),[&](size_t,size_t){
  const int now=++active;int previous=peak.load();while(previous<now&&!peak.compare_exchange_weak(previous,now)){}
  until([&]{return finish.load();});--active;
 },1);});
 until([&]{return active==1&&global.queued()>0;});assert(peak==1);
 global.opaque(33,true);until([&]{return active==2;});finish=true;
 boosted.get();assert(peak==2);global.end(33);
 // The gameplay limit: a quarter of the cores, at most two; the renderer lowers it when the device is hot.
 // None admits nothing while the game is played, whatever the job's pool; a rise or a place's loading admits again.
 assert(CpuAdmission(8).gameplay_threads()==2&&CpuAdmission(16).gameplay_threads()==2&&CpuAdmission(4).gameplay_threads()==1);
 CpuAdmission heat(8);assert(heat.threads()==2&&heat.set_gameplay_limit(1)&&!heat.set_gameplay_limit(1)&&heat.threads()==1);
 assert(heat.set_gameplay_limit(0)&&heat.gameplay_threads()==0&&heat.threads()==1&&heat.stats().phase_cap==0);
 auto gameplay_none=std::async(std::launch::async,[&]{return heat.acquire(1,{})!=nullptr;});
 assert(gameplay_none.wait_for(60ms)==std::future_status::timeout && "no gameplay threads: nothing is admitted");
 heat.set_gameplay_limit(1);assert(gameplay_none.get());
 heat.set_gameplay_limit(0);
 auto loading=std::async(std::launch::async,[&]{return heat.acquire(8,{})!=nullptr;});
 assert(loading.wait_for(60ms)==std::future_status::timeout);
 heat.begin(5,true);heat.opaque(5,true);assert(heat.threads()==8);
 assert(loading.get() && "a place's loading takes every core whatever the heat");
 heat.end(5);

}
