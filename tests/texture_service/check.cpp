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
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
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
void release_one() {
    { std::lock_guard lock(mutex); ++permits; }
    changed.notify_all();
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
int main() {
    auto &service = Service::instance();
    reset(); service.configure({}, nullptr); occupy(service);
    for (int i = 20; i < 24; ++i) require(bool(transcode(service, i)), "ordinary backlog admitted");
    service.begin_loading_cache_scope();
    for (int i = 100; i < 104; ++i) {
        auto ticket = std::make_shared<CacheTask>();
        service.post_cache(ticket, "fixture cached map", [i] { event(i); });
        service.promote_cache(ticket); // promotion AFTER enqueue, as prepare_scene does
    }
    release_one();
    wait([] { return events.size() >= 8; }, "foreground and ordinary work finish with another worker blocked");
    {
        std::lock_guard lock(mutex);
        require(events[0] == 100 && events[1] == 101,
                "promoted loading cache hits start before the next queued transcode");
        require(events[2] == 20 && events[3] == 102 && events[4] == 103 && events[5] == 21,
                "two foreground cache reads then ordinary transcode; neither lane starves");
    }
    finish(service);

    // Both reveal/cancel and replacing the scene revoke existing tickets. A new scope
    // must not accidentally re-admit an old generation, even while old work is queued.
    for (bool replacement : {false, true}) {
        reset(); service.configure({}, nullptr); occupy(service);
        service.begin_loading_cache_scope();
        auto ticket = std::make_shared<CacheTask>();
        service.post_cache(ticket, "stale scene", [] { event(100); });
        service.promote_cache(ticket);
        service.end_loading_cache_scope();
        if (replacement) service.begin_loading_cache_scope();
        require(bool(transcode(service, 20)), "ordinary after revoked scope admitted");
        release_one();
        wait([] { return events.size() >= 2; }, "revoked scope drains ordinary work");
        { std::lock_guard lock(mutex); require(events[0] == 20 && events[1] == 100,
            "cancel/reveal/replacement must remove stale generation priority"); }
        finish(service);
    }

    reset(); service.configure({}, nullptr); occupy(service);
    auto ticket = std::make_shared<CacheTask>();
    service.post_cache(ticket, "shutdown queued cache", [] { event(100); });
    std::thread stop([&] { service.shutdown(); });
    // Synchronize with actual shutdown's queue invalidation, not a timing sleep.
    for (;;) {
        bool stopped;
        { std::lock_guard lock(service.mutex_); stopped = service.stopping_; }
        if (stopped) break;
        std::this_thread::yield();
    }
    { std::lock_guard lock(mutex); release_all = true; }
    changed.notify_all(); stop.join();
    require(events.empty(), "shutdown drops unrun cache task and joins bounded worker pool");
    std::puts("PASS: real Service promoted cache hits, 2:1 fairness, scope revocation/replacement, shutdown");
}
