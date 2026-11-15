#include "storm/fx/bakes.hpp"
#include "storm/fx/cpu_admission.hpp"

#include "storm/fx/log.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace storm::fx
{
CpuAdmission &SharedCpuAdmission() { static CpuAdmission admission; return admission; }

namespace
{
void LowerThreadPriority()
{
#if defined(__linux__)
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}

bool Cancelled(const detail::BakeState *bake)
{
    return bake && bake->cancelled.load(std::memory_order_relaxed);
}

// The bake the calling thread runs (the bake thread while it runs one; null elsewhere: never cancelled).
thread_local const detail::BakeState *t_bake = nullptr;
thread_local unsigned t_cpu_piece = 0;

std::atomic<const char *(*)()> g_gate{nullptr};
std::mutex g_pause;
std::condition_variable g_pause_wake; // a bake was cancelled: the pause ends for it
bool g_paused = false;
std::chrono::steady_clock::time_point g_paused_at;
std::atomic<int> g_pause_ms{5000};

void WakePausedBakes()
{
    {
        std::lock_guard lock(g_pause);
    }
    g_pause_wake.notify_all();
}

// Between two pieces of a bake: while the gate says wait every bake thread waits, the pause logged once; a cancelled
// bake goes on at once (to its end: ParallelRanges takes no further piece).
void BakeGate(const detail::BakeState *bake)
{
    const char *(*gate)() = g_gate.load();
    if (!gate || !gate())
        return;
    std::unique_lock lock(g_pause);
    for (;;)
    {
        if (Cancelled(bake))
            return;
        const char *why = gate();
        if (!why)
        {
            if (g_paused)
            {
                g_paused = false;
                Log(LogLevel::Info, "ray bakes resumed after %.1f s",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - g_paused_at).count());
            }
            return;
        }
        if (!g_paused)
        {
            g_paused = true;
            g_paused_at = std::chrono::steady_clock::now();
            Log(LogLevel::Info, "ray bakes paused: %s", why);
        }
        g_pause_wake.wait_for(lock, std::chrono::milliseconds(g_pause_ms.load()), [&] { return Cancelled(bake); });
    }
}

// Two lanes, a thread each (started on first use, nice 10): the loads (a bake's cache read, BakeStage::Load) and
// the bakes. A load never waits behind a running bake; a load that finds nothing goes on to the bake lane.
class BakeQueue
{
  public:
    static BakeQueue &instance()
    {
        SharedCpuAdmission(); // register lifetime before the queue and its worker
        static BakeQueue queue;
        return queue;
    }
    ~BakeQueue() { shutdown(false); }

    BakeTicket post(std::string what, BakeStep step, bool load)
    {
        auto state = std::make_shared<detail::BakeState>();
        {
            std::lock_guard lock(mutex_);
            Lane &lane = load ? loads_ : bakes_;
            lane.queue.push_back({std::move(what), std::make_shared<BakeStep>(std::move(step)), state});
            stopping_ = false;
            for (Lane *l : {&loads_, &bakes_})
                if (!l->thread.joinable())
                    l->thread = std::thread(&BakeQueue::run, this, std::ref(*l));
        }
        wake_.notify_all();
        return BakeTicket(std::move(state));
    }

    void shutdown(bool log)
    {
        size_t dropped = 0;
        int running = 0;
        {
            std::lock_guard lock(mutex_);
            if (!loads_.thread.joinable() && !bakes_.thread.joinable())
                return;
            stopping_ = true;
            for (Lane *lane : {&loads_, &bakes_})
            {
                for (Task &task : lane->queue)
                    dropped += task.state->cancelled.load() ? 0 : 1;
                lane->queue.clear();
                if (lane->running)
                    running += lane->running->cancelled.exchange(true) ? 0 : 1;
            }
        }
        wake_.notify_all();
        WakePausedBakes();
        for (Lane *lane : {&loads_, &bakes_})
            if (lane->thread.joinable())
            {
                lane->thread.join();
                lane->thread = {};
            }
        if (log && (dropped || running))
            Log(LogLevel::Info, "ray bakes: shut down, %zu queued not run, %d running cancelled", dropped, running);
    }

    size_t outstanding()
    {
        std::lock_guard lock(mutex_);
        size_t count = 0;
        for (const Lane *lane : {&loads_, &bakes_})
        {
            for (const Task &task : lane->queue)
                count += task.state->cancelled.load() ? 0 : 1;
            count += lane->running ? 1 : 0;
        }
        return count;
    }

  private:
    struct Task
    {
        std::string what;
        std::shared_ptr<BakeStep> step; // shared by its load and its bake
        std::shared_ptr<detail::BakeState> state;
    };
    struct Lane
    {
        std::deque<Task> queue;
        std::shared_ptr<detail::BakeState> running;
        std::thread thread;
    };

    void run(Lane &lane)
    {
        LowerThreadPriority();
        const bool load = &lane == &loads_;
        std::unique_lock lock(mutex_);
        for (;;)
        {
            wake_.wait(lock, [&] { return stopping_ || !lane.queue.empty(); });
            if (stopping_)
                return;
            Task task = std::move(lane.queue.front());
            lane.queue.pop_front();
            if (task.state->cancelled.load())
                continue; // its owner let go of it before it started: nothing reads it
            lane.running = task.state;
            lock.unlock();
            t_bake = task.state.get();
            bool done = true;
            try
            {
                done = (*task.step)(load ? BakeStage::Load : BakeStage::Bake);
            }
            catch (const BakeCancelled &)
            {
                Log(LogLevel::Info, "ray bake %s: cancelled while running (its owner let go of it, or the bakes shut down)",
                    task.what.c_str());
            }
            t_bake = nullptr;
            lock.lock();
            lane.running.reset();
            if (load && !done && !task.state->cancelled.load())
            {
                bakes_.queue.push_back(std::move(task)); // not in the cache: baked
                wake_.notify_all();
            }
            else if (!load && !done)
                Log(LogLevel::Error, "ray bake %s: its bake stage returned unfinished: a bug in the bake (nothing more runs)",
                    task.what.c_str());
            if (task.step)
            {
                lock.unlock();
                task = {}; // its captures (the geometry's copies) go here, not with the next task
                lock.lock();
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    Lane loads_, bakes_;
    bool stopping_ = false;
};
} // namespace

void BakeTicket::reset()
{
    if (!state_)
        return;
    if (!state_->cancelled.exchange(true))
        WakePausedBakes();
    state_.reset();
}

BakeTicket PostBake(std::string what, std::function<void()> work)
{
    return BakeQueue::instance().post(
        std::move(what), [work = std::move(work)](BakeStage) {
            work();
            return true;
        },
        false);
}

BakeTicket PostBake(std::string what, BakeStep step)
{
    return BakeQueue::instance().post(std::move(what), std::move(step), true);
}

size_t BakesOutstanding()
{
    return BakeQueue::instance().outstanding();
}

void ShutdownBakes()
{
    BakeQueue::instance().shutdown(true);
}

void SetBakeGate(const char *(*wait_reason)(), int pause_ms)
{
    g_gate = wait_reason;
    g_pause_ms = pause_ms;
}

void ParallelRanges(size_t count, unsigned threads, const std::function<void(size_t, size_t)> &work, size_t min_piece)
{
    ParallelRanges(count, threads, work, min_piece, nullptr);
}

void ParallelRanges(size_t count, unsigned threads, const std::function<void(size_t, size_t)> &work, size_t min_piece, ParallelRangeStats *stats)
{
    if (stats)
        stats->calls.fetch_add(1, std::memory_order_relaxed);
    const detail::BakeState *bake = t_bake;
    min_piece = std::max<size_t>(1, min_piece);
    threads = std::max(1u, std::min<unsigned>(threads, static_cast<unsigned>(std::max<size_t>(1, count / min_piece))));
    // A nested range belongs to the current piece permit: never acquire again
    // or spawn helpers while its parent waits for this call.
    if (t_cpu_piece) threads = 1;
    // Pieces of at least min_piece items, about 16 per thread, taken in turn: the gate (SetBakeGate) sits between them.
    const size_t piece = std::max<size_t>(min_piece, (count + threads * 16 - 1) / (threads * 16));
    std::atomic<size_t> next{0};
    auto admitted_work = [&](size_t first, size_t last) {
        const auto wait_start = std::chrono::steady_clock::now();
        auto permit = t_cpu_piece ? std::shared_ptr<void>{} :
            SharedCpuAdmission().acquire(1, [&] { return Cancelled(bake); });
        if (!t_cpu_piece && !permit) throw BakeCancelled{};
        struct Piece { Piece() { ++t_cpu_piece; } ~Piece() { --t_cpu_piece; } } piece_scope;
        const auto work_start = std::chrono::steady_clock::now();
        if (stats) stats->gate_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(work_start - wait_start).count()), std::memory_order_relaxed);
        struct Timing {
            ParallelRangeStats *stats;
            std::chrono::steady_clock::time_point start;
            ~Timing() { if (stats) stats->work_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed); }
        } timing{stats, work_start};
        if (stats) stats->pieces.fetch_add(1, std::memory_order_relaxed);
        work(first, last);
    };
    auto run = [&] {
        for (;;)
        {
            if (Cancelled(bake))
                return;
            const size_t first = next.fetch_add(piece);
            if (first >= count)
                return;
            if (!stats)
            {
                BakeGate(bake);
                if (Cancelled(bake))
                    return;
                admitted_work(first, std::min(count, first + piece));
            }
            else
            {
                // RAII also attributes a piece interrupted by an exception; ownership/cancellation is unchanged.
                struct Elapsed
                {
                    std::atomic<uint64_t> &sum;
                    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
                    ~Elapsed()
                    {
                        sum.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
                    }
                };
                {
                    Elapsed timing{stats->gate_ns};
                    BakeGate(bake);
                }
                if (Cancelled(bake))
                    return;
                admitted_work(first, std::min(count, first + piece));
            }
        }
    };
    // The first exception of any thread (`work` failing, or a nested ParallelRanges of a cancelled bake) stops the
    // others' next pieces and is thrown here once all are joined: an exception must not end a pool thread (that
    // would end the program).
    std::mutex failure_mutex;
    std::exception_ptr failure;
    const auto guarded = [&] {
        const auto previous_bake = t_bake;
        t_bake = bake;
        struct RestoreBake { const detail::BakeState *old; ~RestoreBake() { t_bake = old; } } restore{previous_bake};
        try
        {
            run();
        }
        catch (...)
        {
            std::lock_guard lock(failure_mutex);
            if (!failure)
                failure = std::current_exception();
            next = count;
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t)
        pool.emplace_back([&] {
            LowerThreadPriority();
            guarded();
        });
    guarded();
    for (std::thread &th : pool)
        th.join();
    if (failure)
        std::rethrow_exception(failure);
    if (Cancelled(bake))
        throw BakeCancelled{};
}

unsigned BakeThreads()
{
    // Helpers exist up to the physical ceiling; per-piece admission decides
    // how many may actually work. First opaque Present can wake more helpers
    // even when this range was constructed before the loading frame.
    return SharedCpuAdmission().maximum_threads();
}

} // namespace storm::fx
