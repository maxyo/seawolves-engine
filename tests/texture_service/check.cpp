// The production texture Service scheduler (texture/core/service.cpp) under ASan/UBSan with its real worker count and
// queues; only the external codec/cache boundary is replaced by gated work. Every wait is on progress, never on a
// clock: a step that cannot finish (a scheduler deadlock) hangs here and run.py's deadlock guard reports the last
// step printed. The checks therefore pass or fail the same way on an idle machine and under a loaded land queue.
#include "texture/core/service.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

using namespace storm::texture;

namespace
{
std::mutex mutex;
std::condition_variable changed;
unsigned gated = 0, permits = 0;
bool release_all = false;
std::vector<int> events;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::fflush(stderr);
        std::_Exit(1);
    }
}

// What the fixture waits for next: the last line printed names the step when the deadlock guard fires.
void step(const char *what)
{
    std::printf("step: %s\n", what);
    std::fflush(stdout);
}

template <class Predicate> void wait(Predicate predicate)
{
    std::unique_lock lock(mutex);
    changed.wait(lock, predicate);
}

void event(int value)
{
    {
        std::lock_guard lock(mutex);
        events.push_back(value);
    }
    changed.notify_all();
}

void reset()
{
    std::lock_guard lock(mutex);
    gated = permits = 0;
    release_all = false;
    events.clear();
}

std::shared_ptr<Job> transcode(Service &service, int id)
{
    return service.submit({DxtFormat::Bc1, 1, 1, 1, 1}, static_cast<uint64_t>(id), GpuFormat::Bc1,
                          {static_cast<uint8_t>(id)});
}

void occupy(Service &service)
{
    for (unsigned i = 0; i < Service::worker_count(); ++i)
        require(bool(transcode(service, static_cast<int>(i))), "initial jobs admitted");
    step("all real Service workers occupied");
    wait([] { return gated == Service::worker_count(); });
}

void finish(Service &service)
{
    {
        std::lock_guard lock(mutex);
        release_all = true;
    }
    changed.notify_all();
    service.shutdown();
}

// A cache task that holds the IO worker until released.
struct Blocker
{
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false, leave = false;
    void hold()
    {
        std::unique_lock lock(mutex);
        started = true;
        changed.notify_all();
        changed.wait(lock, [&] { return leave; });
    }
    void await_start()
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return started; });
    }
    void release()
    {
        {
            std::lock_guard lock(mutex);
            leave = true;
        }
        changed.notify_all();
    }
};
} // namespace

// Only external work is replaced: the production Service queue, worker count, fairness, generation promotion,
// deduplication, publication and shutdown all execute unchanged.
namespace storm::texture
{
Layout output_layout(const Layout &layout)
{
    return layout;
}
std::optional<Blob> read_cache_file(const std::filesystem::path &, uint64_t, const Layout &)
{
    return {};
}
uint64_t scan_cache(const std::filesystem::path &)
{
    return 0;
}
uint64_t evict_cache(const std::filesystem::path &, uint64_t)
{
    return 0;
}
uint64_t write_cache_file(const std::filesystem::path &, uint64_t, const Layout &, GpuFormat, const Blob &)
{
    return 0;
}
Blob transcode_to_ktx(const Layout &, const uint8_t *input, GpuFormat)
{
    if (*input < Service::worker_count())
    {
        std::unique_lock lock(mutex);
        ++gated;
        changed.notify_all();
        changed.wait(lock, [] { return permits || release_all; });
        if (!release_all)
            --permits;
    }
    else
        event(*input);
    return {};
}
} // namespace storm::texture

// The current location's claim on a ticket, read from the service's private state (-fno-access-control).
static bool scope_current(const Service &service, const std::shared_ptr<CacheTask> &ticket)
{
    const uint64_t generation = service.loading_generation_;
    return generation != 0 && ticket && ticket->generation.load() == generation;
}

int main()
{
    auto &service = Service::instance();

    // Cache IO runs on its own worker: it completes while every CPU worker is held (they are released only by
    // finish()); an IO lane that depended on them would never complete.
    reset();
    service.configure({}, nullptr);
    occupy(service);
    auto io = std::make_shared<CacheTask>();
    service.begin_loading_cache_scope();
    service.promote_cache(io);
    service.post_cache(io, [] { event(100); });
    step("cache IO finishes while every transcode worker is held");
    wait([] { return events.size() == 1; });
    finish(service);

    // IO fairness, capacity and cancellation, independent of occupied CPU transcodes.
    reset();
    service.configure({}, nullptr);
    Blocker blocker;
    require(service.post_cache(std::make_shared<CacheTask>(), [&] { blocker.hold(); }), "IO blocker admitted");
    step("IO blocker running");
    blocker.await_start();
    service.begin_loading_cache_scope();
    for (int i = 0; i < 255; ++i)
    {
        auto ticket = std::make_shared<CacheTask>();
        if (i % 3 != 2)
            service.promote_cache(ticket);
        if (i == 3)
        {
            ticket->cancelled = true;
            require(!service.post_cache(ticket, [] { event(999); }), "cancelled ticket rejected");
            ticket->cancelled = false;
        }
        require(service.post_cache(ticket, [i] { event(i); }), "capacity admits queued job");
        if (i == 3)
            ticket->cancelled = true;
    }
    require(!service.post_cache(std::make_shared<CacheTask>(), [] {}), "queued plus inflight bounded256");
    blocker.release();
    step("all IO classes make progress");
    wait([] { return events.size() == 254; });
    {
        std::lock_guard lock(mutex);
        require(events[0] == 0 && events[1] == 1 && events[2] == 2, "two foreground then ordinary IO fairness");
    }
    auto a = std::make_shared<CacheTask>(), b = std::make_shared<CacheTask>();
    require(service.reserve_cache_bytes(a, 256ull << 20) == Service::CacheBytes::Reserved, "exact ready budget reserved");
    require(service.reserve_cache_bytes(b, 1) == Service::CacheBytes::Busy, "ready budget provides backpressure");
    a->bytes.reset();
    require(service.reserve_cache_bytes(b, 1) == Service::CacheBytes::Reserved, "ready release admits next");
    b.reset();
    require(service.reserve_cache_bytes(a, (256ull << 20) + 1) == Service::CacheBytes::TooLarge,
            "oversized read explicit error");
    service.end_loading_cache_scope();
    require(!scope_current(service, io), "old scene promotion revoked");
    service.begin_loading_cache_scope();
    require(!scope_current(service, io), "replacement cannot revive old ticket");
    service.promote_cache(a);
    require(scope_current(service, a), "new scene claim accepted");
    service.shutdown();
    require(!service.post_cache(a, [] {}), "shutdown stops intake");

    // Shutdown drops queued jobs and joins one in-flight IO task outside its mutex.
    reset();
    service.configure({}, nullptr);
    Blocker running;
    service.post_cache(std::make_shared<CacheTask>(), [&] {
        running.hold();
        service.post([] { event(998); });
    });
    step("shutdown IO running");
    running.await_start();
    service.post_cache(std::make_shared<CacheTask>(), [] { event(999); });
    std::thread stop([&] { service.shutdown(); });
    step("shutdown begins");
    for (;;)
    {
        {
            std::lock_guard lock(service.mutex_);
            if (service.stopping_)
                break;
        }
        std::this_thread::yield();
    }
    running.release();
    stop.join();
    require(events.empty(), "queued callbacks dropped on shutdown");
    require(service.workers_.empty(), "late IO callback must not restart CPU workers during shutdown");

    // The game thread promotes a ticket right after posting it (Upscales::prepare_scene) while the IO worker picks its
    // next task: the pick must classify every task exactly once (owner build 1650 crashed in cache_worker, about 1 in
    // 4 loads, when a lone task read ordinary to one scan and foreground to the next). Another thread flips the
    // ticket's generation between current and old without pause, so nearly every pick of the lone task races a
    // change: the two-scan selection fails within a few hundred picks (it failed 20 of 20 runs), the single pass never.
    reset();
    service.configure({}, nullptr);
    service.begin_loading_cache_scope();
    {
        const uint64_t current = service.loading_generation_.load();
        auto ticket = std::make_shared<CacheTask>();
        std::atomic<bool> flipping{true};
        std::thread flip([&] {
            while (flipping.load(std::memory_order_relaxed))
            {
                ticket->generation.store(current);
                ticket->generation.store(0);
            }
        });
        constexpr long kPicks = 5000;
        std::atomic<long> ran{0};
        step("lone tasks picked while their generation flips");
        for (long i = 0; i < kPicks; ++i)
        {
            while (!service.post_cache(ticket, [&] { ran.fetch_add(1); }))
                std::this_thread::yield();
            while (ran.load() <= i)
                std::this_thread::yield();
        }
        flipping = false;
        flip.join();
    }
    service.shutdown();
    std::puts("PASS actual cache IO isolation, fairness, bounds, generation, promotion race and shutdown");
}
