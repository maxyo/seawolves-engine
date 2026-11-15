#include "storm/fx/bakes.hpp"
#include "storm/fx/cpu_admission.hpp"

#include "storm/fx/log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
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

// The device's cores by speed: /sys/devices/system/cpu/cpuN/cpu_capacity (arm64 Linux, Android); empty where the
// kernel gives none (every core counts the same).
std::vector<unsigned> ReadCapacities()
{
    std::vector<unsigned> capacity;
#if defined(__linux__)
    const unsigned cores = std::thread::hardware_concurrency();
    for (unsigned c = 0; c < cores; ++c)
    {
        std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpu_capacity");
        unsigned value = 0;
        if (!(file >> value))
            return {};
        capacity.push_back(value);
    }
#endif
    return capacity;
}

const BakeCores &CoresOf(BakeBudget budget)
{
    static const std::array<BakeCores, 2> cores = [] {
        const std::vector<unsigned> capacity = ReadCapacities();
        const unsigned count = std::thread::hardware_concurrency();
        std::array<BakeCores, 2> out{BakeCoresFor(BakeBudget::Loading, capacity, count),
                                     BakeCoresFor(BakeBudget::Play, capacity, count)};
        Log(LogLevel::Info,
            "ray bakes: %u cores (%s): behind a loading screen as many threads as the shared admission gives, in play "
            "%u%s", count, capacity.empty() ? "no cpu_capacity: all count the same" : "by cpu_capacity", out[1].threads,
            out[1].allowed.empty() ? "" : " on the cores below the fastest");
        return out;
    }();
    return cores[static_cast<size_t>(budget)];
}

// The calling thread onto the cores of the budget of the moment (when it changed since the thread last asked).
void FollowBudget()
{
#if defined(__linux__)
    thread_local int applied = -1;
    const int budget = static_cast<int>(CurrentBakeBudget());
    if (budget == applied)
        return;
    applied = budget;
    const BakeCores &cores = CoresOf(static_cast<BakeBudget>(budget));
    cpu_set_t set;
    CPU_ZERO(&set);
    if (cores.allowed.empty())
        for (unsigned c = 0; c < std::thread::hardware_concurrency() && c < CPU_SETSIZE; ++c)
            CPU_SET(c, &set);
    else
        for (const unsigned c : cores.allowed)
            if (c < CPU_SETSIZE)
                CPU_SET(c, &set);
    // A refusal (a core taken offline, a container's mask) leaves the thread where the system put it: it still
    // counts against the budget's thread count.
    static std::atomic<bool> refused{false};
    if (sched_setaffinity(0, sizeof(set), &set) != 0 && !refused.exchange(true))
        Log(LogLevel::Error, "ray bakes: the system refused the bake threads' cores (sched_setaffinity): they run where "
                             "it puts them, %u at most in play", CoresOf(BakeBudget::Play).threads);
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

// Bakes of the place one stands in (BakePriority::Place) queued or running: the background bakes wait for them.
std::atomic<int> g_place_bakes{0};

// Between two pieces of a bake: while the gate says wait every bake thread waits, the pause logged once; a cancelled
// bake goes on at once (to its end: ParallelRanges takes no further piece). A background bake also waits while a
// bake of the place is queued or runs: the place's threads are not shared with it.
void BakeGate(const detail::BakeState *bake)
{
    while (bake && bake->background && g_place_bakes.load(std::memory_order_relaxed) > 0 && !Cancelled(bake))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
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

// Three lanes, a thread each (started on first use, nice 10): the loads (a bake's cache read, BakeStage::Load), the
// bakes of the place and the background bakes. A load never waits behind a running bake; a load that finds nothing
// goes on to its bake lane. A background bake never holds up the place's: it has its own lane and yields between its
// pieces while a bake of the place is queued or runs (BakeGate), so only one of the two takes the budget's threads.
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

    BakeTicket post(std::string what, BakeStep step, bool load, BakePriority priority)
    {
        auto state = std::make_shared<detail::BakeState>();
        state->background = priority == BakePriority::Background;
        {
            std::lock_guard lock(mutex_);
            Task task{std::move(what), std::make_shared<BakeStep>(std::move(step)), state};
            if (load)
                loads_.queue.push_back(std::move(task));
            else
                queue_bake(std::move(task));
            stopping_ = false;
            for (Lane *l : {&loads_, &bakes_, &background_})
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
            if (!loads_.thread.joinable() && !bakes_.thread.joinable() && !background_.thread.joinable())
                return;
            stopping_ = true;
            g_place_bakes -= static_cast<int>(bakes_.queue.size());
            for (Lane *lane : {&loads_, &bakes_, &background_})
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
        for (Lane *lane : {&loads_, &bakes_, &background_})
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
        for (const Lane *lane : {&loads_, &bakes_, &background_})
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

    // With the lock held: a bake stage onto its lane.
    void queue_bake(Task task)
    {
        if (task.state->background)
            background_.queue.push_back(std::move(task));
        else
        {
            ++g_place_bakes;
            bakes_.queue.push_back(std::move(task));
        }
    }

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
            const bool place = &lane == &bakes_;
            if (task.state->cancelled.load())
            {
                g_place_bakes -= place ? 1 : 0;
                continue; // its owner let go of it before it started: nothing reads it
            }
            lane.running = task.state;
            lock.unlock();
            t_bake = task.state.get();
            FollowBudget();
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
            g_place_bakes -= place ? 1 : 0;
            if (load && !done && !task.state->cancelled.load())
            {
                queue_bake(std::move(task)); // not in the cache: baked
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
    Lane loads_, bakes_, background_;
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

BakeTicket PostBake(std::string what, std::function<void()> work, BakePriority priority)
{
    return BakeQueue::instance().post(
        std::move(what), [work = std::move(work)](BakeStage) {
            work();
            return true;
        },
        false, priority);
}

BakeTicket PostBake(std::string what, BakeStep step, BakePriority priority)
{
    return BakeQueue::instance().post(std::move(what), std::move(step), true, priority);
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
    // Pieces of at least min_piece items, about 16 per thread, taken in turn: the gate (SetBakeGate) and the budget
    // sit between them.
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
    auto run = [&](unsigned thread) {
        for (;;)
        {
            // In play the threads beyond the budget's wait: a loading screen that comes wakes them, one that goes
            // parks them at their next piece.
            while (!Cancelled(bake) && next.load(std::memory_order_relaxed) < count &&
                   CurrentBakeBudget() == BakeBudget::Play && thread >= CoresOf(BakeBudget::Play).threads)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (Cancelled(bake))
                return;
            FollowBudget();
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
    const auto guarded = [&](unsigned thread) {
        const auto previous_bake = t_bake;
        t_bake = bake;
        struct RestoreBake { const detail::BakeState *old; ~RestoreBake() { t_bake = old; } } restore{previous_bake};
        try
        {
            run(thread);
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
        pool.emplace_back([&, t] {
            LowerThreadPriority();
            guarded(t);
        });
    guarded(0);
    for (std::thread &th : pool)
        th.join();
    if (failure)
        std::rethrow_exception(failure);
    if (Cancelled(bake))
        throw BakeCancelled{};
}

BakeCores BakeCoresFor(BakeBudget budget, std::span<const unsigned> capacity, unsigned cores)
{
    BakeCores out;
    cores = std::max(1u, cores);
    if (budget == BakeBudget::Loading)
    {
        out.threads = cores; // the shared admission's permits decide how many of them work
        return out;
    }
    // In play: the cores below the fastest class (a phone's prime cores carry the game's main and render threads).
    if (capacity.size() == cores)
    {
        const unsigned fastest = *std::max_element(capacity.begin(), capacity.end());
        for (unsigned c = 0; c < cores; ++c)
            if (capacity[c] < fastest)
                out.allowed.push_back(c);
    }
    const unsigned available = out.allowed.empty() ? (cores > 2 ? cores - 2 : 1) : static_cast<unsigned>(out.allowed.size());
    out.threads = std::max(1u, std::min(kPlayBakeThreads, available));
    return out;
}

BakeBudget CurrentBakeBudget()
{
    // The shared admission knows when a loading picture covers the scene (its opaque phase gives every core).
    const CpuAdmission &admission = SharedCpuAdmission();
    return admission.threads() >= admission.maximum_threads() ? BakeBudget::Loading : BakeBudget::Play;
}

unsigned BakeThreads()
{
    // Helpers exist up to the physical ceiling; per-piece admission and, in play, the budget decide how many may
    // actually work. The first opaque Present can wake more helpers even when a range was made before it.
    return SharedCpuAdmission().maximum_threads();
}

} // namespace storm::fx
