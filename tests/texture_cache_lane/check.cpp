#include "texture/core/service.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

using namespace storm::texture;
namespace {
std::mutex mutex;
std::condition_variable changed;
unsigned gated = 0, permits = 0;
bool release_all = false;
std::vector<int> events;
void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::fflush(stderr); std::_Exit(1); }
}
template<class Predicate> void wait(Predicate predicate, const char *message) {
    std::unique_lock lock(mutex);
    require(changed.wait_for(lock, std::chrono::seconds(3), predicate), message);
}
void event(int value) {
    { std::lock_guard lock(mutex); events.push_back(value); }
    changed.notify_all();
}
void reset() {
    std::lock_guard lock(mutex);
    gated = permits = 0; release_all = false; events.clear();
}
std::shared_ptr<Job> transcode(Service &service, int id) {
    return service.submit({DxtFormat::Bc1, 1, 1, 1, 1}, static_cast<uint64_t>(id), GpuFormat::Bc1,
                          {static_cast<uint8_t>(id)});
}
void occupy(Service &service) {
    for (unsigned i = 0; i < Service::worker_count(); ++i)
        require(bool(transcode(service, static_cast<int>(i))), "initial jobs admitted");
    wait([] { return gated == Service::worker_count(); }, "all real Service workers occupied");
}
void finish(Service &service) {
    { std::lock_guard lock(mutex); release_all = true; }
    changed.notify_all();
    service.shutdown();
}
}
// Only external work is replaced: the production Service queue, worker count, fairness,
// generation promotion, deduplication, publication and shutdown all execute unchanged.
namespace storm::texture {
Layout output_layout(const Layout &layout) { return layout; }
std::optional<Blob> read_cache_file(const std::filesystem::path &, uint64_t, const Layout &) { return {}; }
uint64_t scan_cache(const std::filesystem::path &) { return 0; }
uint64_t evict_cache(const std::filesystem::path &, uint64_t) { return 0; }
uint64_t write_cache_file(const std::filesystem::path &, uint64_t, const Layout &, GpuFormat, const Blob &) { return 0; }
Blob transcode_to_ktx(const Layout &, const uint8_t *input, GpuFormat) {
    if (*input < Service::worker_count()) {
        std::unique_lock lock(mutex);
        ++gated; changed.notify_all();
        changed.wait(lock, [] { return permits || release_all; });
        if (!release_all) --permits;
    } else event(*input);
    return {};
}
}
// The current location's claim on a ticket, read from the service's private state (-fno-access-control).
static bool scope_current(const Service &service,const std::shared_ptr<CacheTask> &ticket){
 const uint64_t generation=service.loading_generation_;return generation!=0&&ticket&&ticket->generation.load()==generation;}

int main() {
 auto &service=Service::instance();reset();service.configure({},nullptr);occupy(service);
 auto io=std::make_shared<CacheTask>();service.begin_loading_cache_scope();service.promote_cache(io);
 service.post_cache(io,[]{event(100);});
 {std::unique_lock lock(mutex);require(changed.wait_for(lock,std::chrono::milliseconds(500),[]{return events.size()==1;}),"cache IO must finish before held transcode release");}
 finish(service);
 // IO fairness/cap/cancel independent from occupied CPU transcodes.
 reset();service.configure({},nullptr);
 std::mutex gate_mutex;std::condition_variable gate_cv;bool started=false,leave=false;
 auto blocker=std::make_shared<CacheTask>();
 require(service.post_cache(blocker,[&]{std::unique_lock l(gate_mutex);started=true;gate_cv.notify_all();gate_cv.wait(l,[&]{return leave;});}),"IO blocker admitted");
 {std::unique_lock l(gate_mutex);require(gate_cv.wait_for(l,std::chrono::seconds(1),[&]{return started;}),"IO running");}
 service.begin_loading_cache_scope();
 for(int i=0;i<255;++i){auto t=std::make_shared<CacheTask>();if(i%3!=2)service.promote_cache(t);if(i==3)t->cancelled=true;
  if(i==3){require(!service.post_cache(t,[]{event(999);}),"cancelled ticket rejected");t->cancelled=false;}
  require(service.post_cache(t,[i]{event(i);}),"capacity admits queued job");if(i==3)t->cancelled=true;}
 require(!service.post_cache(std::make_shared<CacheTask>(),[]{}),"queued plus inflight bounded256");
 {std::lock_guard l(gate_mutex);leave=true;}gate_cv.notify_all();
 wait([]{return events.size()==254;},"all IO classes make progress");
 {std::lock_guard l(mutex);require(events[0]==0&&events[1]==1&&events[2]==2,"two foreground then ordinary IO fairness");}
 auto a=std::make_shared<CacheTask>(),b=std::make_shared<CacheTask>();
 require(service.reserve_cache_bytes(a,256ull<<20)==Service::CacheBytes::Reserved,"exact ready budget reserved");
 require(service.reserve_cache_bytes(b,1)==Service::CacheBytes::Busy,"ready budget provides backpressure");
 a->bytes.reset();require(service.reserve_cache_bytes(b,1)==Service::CacheBytes::Reserved,"ready release admits next");b.reset();
 require(service.reserve_cache_bytes(a,(256ull<<20)+1)==Service::CacheBytes::TooLarge,"oversized read explicit error");
 service.end_loading_cache_scope();require(!scope_current(service,io),"old scene promotion revoked");
 service.begin_loading_cache_scope();require(!scope_current(service,io),"replacement cannot revive old ticket");
 service.promote_cache(a);require(scope_current(service,a),"new scene claim accepted");
 service.shutdown();require(!service.post_cache(a,[]{}),"shutdown stops intake");
 // Shutdown drops queued jobs and joins one in-flight IO task outside its mutex.
 reset();service.configure({},nullptr);started=false;leave=false;
 service.post_cache(std::make_shared<CacheTask>(),[&]{std::unique_lock l(gate_mutex);started=true;gate_cv.notify_all();gate_cv.wait(l,[&]{return leave;});l.unlock();service.post([]{event(998);});});
 {std::unique_lock l(gate_mutex);require(gate_cv.wait_for(l,std::chrono::seconds(1),[&]{return started;}),"shutdown IO running");}
 service.post_cache(std::make_shared<CacheTask>(),[]{event(999);});
 std::thread stop([&]{service.shutdown();});
 for(;;){{std::lock_guard l(service.mutex_);if(service.stopping_)break;}std::this_thread::yield();}
 {std::lock_guard l(gate_mutex);leave=true;}gate_cv.notify_all();stop.join();
 require(events.empty(),"queued callbacks dropped on shutdown");
 require(service.workers_.empty(),"late IO callback must not restart CPU workers during shutdown");
 std::puts("PASS actual cache IO isolation, fairness, bounds, generation and shutdown");
}
