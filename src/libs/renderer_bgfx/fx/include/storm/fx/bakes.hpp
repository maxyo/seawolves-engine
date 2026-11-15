// The ray bakes' threads (the place's probes, the town light, the far LOD): a bake thread of their own, apart from the
// texture transcode workers, so a long or paused bake never holds up a transcode or a relief look-up; and a load
// thread for the bakes' cache reads, so a result in the cache never waits behind a running bake.
//
// A bake is posted with PostBake and runs on its bake thread (the place's bakes on one, the background bakes on
// another, which yields to the first), one after the other; its ParallelRanges spread it over
// BakeThreads() threads. A bake posted with a load stage runs that first on the load thread: found in the cache it is
// done there, otherwise its bake stage is queued on the bake thread. It is cancelled when its owner drops it (the BakeTicket, or the Bake<Job> that holds it, is
// reset or destroyed: the player left the place, the option was switched off) and by ShutdownBakes: a bake not
// started yet is not run, a running one stops at its next piece (ParallelRanges throws BakeCancelled, which the bake
// thread catches) and a bake waiting in the pause is woken at once.
//
// How many threads a bake takes: every piece takes a permit of the shared CPU admission (fx/cpu_admission.hpp, with
// the inference tiles), and the budget of the moment caps the bake's own threads: behind a loading picture (the
// admission's opaque phase) none but the admission's; in play at most two, kept off the device's fastest cores, so a
// bake neither takes the frame's cores nor heats the device as six threads do. The gate (SetBakeGate) holds in both.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace storm::fx
{

// Runs `work(first, last)` over [0, count) on at most `threads` threads and never more than the budget of the moment
// (asked again before every piece: a thread beyond a budget that shrank waits, and goes on when it grows), in pieces
// of at least `min_piece` items (about 16 per thread); returns when all are done. The threads it starts run at
// nice 10 (Linux, Android): the game's main and render threads go first; in play they are kept on the cores below the
// device's fastest. Between pieces every thread waits while the
// gate says so (SetBakeGate: the device is hot, the renderer holds the work), the pause and the resume logged. On the bake thread a cancelled bake takes no
// further piece and throws BakeCancelled once the pieces already running are done.
// Optional cumulative attribution. These are sums of elapsed times on participating threads, NOT CPU time
// and NOT wall-time components that may be subtracted from one another (threads overlap/deschedule).
struct ParallelRangeStats
{
    std::atomic<uint64_t> gate_ns{0}, work_ns{0}, pieces{0}, calls{0};
};
void ParallelRanges(size_t count, unsigned threads, const std::function<void(size_t, size_t)> &work,
                    size_t min_piece = 256);
void ParallelRanges(size_t count, unsigned threads, const std::function<void(size_t, size_t)> &work,
                    size_t min_piece, ParallelRangeStats *stats);
// Why ParallelRanges must wait now (null: it may run; asked between pieces, so cheap); a null gate never pauses.
// While it says wait it is asked again every `pause_ms`; a cancelled bake leaves the pause at once.
void SetBakeGate(const char *(*wait_reason)(), int pause_ms = 5000);
// What the bakes may take now.
enum class BakeBudget
{
    Loading, // behind a loading picture: as many threads as the shared admission admits
    Play,    // while one plays: at most kPlayBakeThreads, on the cores below the fastest ones
};
inline constexpr unsigned kPlayBakeThreads = 2;
// Any thread: Loading while the shared admission is in its opaque phase (a loading picture covers the scene).
BakeBudget CurrentBakeBudget();
// The most threads a bake's range may start (the physical ceiling: the admission and the budget decide how many of
// them work at a time).
unsigned BakeThreads();
// The threads of a bake under `budget` on a device whose cores have these capacities (Linux cpu_capacity, one per
// core; empty or all equal: no core is faster than another), and the cores it may run on (empty: any).
struct BakeCores
{
    unsigned threads = 1;
    std::vector<unsigned> allowed;
};
BakeCores BakeCoresFor(BakeBudget budget, std::span<const unsigned> capacity, unsigned cores);

// Thrown by ParallelRanges in a bake that was cancelled; caught by the bake thread (logged there).
struct BakeCancelled
{
};

namespace detail
{
struct BakeState
{
    std::atomic<bool> cancelled{false};
    bool background = false; // BakePriority::Background
};
} // namespace detail

// The owner's hold on a posted bake: dropping it (reset, assignment, destruction) cancels the bake.
class BakeTicket
{
  public:
    BakeTicket() = default;
    explicit BakeTicket(std::shared_ptr<detail::BakeState> state) : state_(std::move(state)) {}
    BakeTicket(BakeTicket &&other) noexcept = default;
    BakeTicket &operator=(BakeTicket &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            state_ = std::move(other.state_);
        }
        return *this;
    }
    BakeTicket(const BakeTicket &) = delete;
    BakeTicket &operator=(const BakeTicket &) = delete;
    ~BakeTicket() { reset(); }
    void reset();

  private:
    std::shared_ptr<detail::BakeState> state_;
};

enum class BakeStage
{
    Load, // on the load thread: the cache read; true when it filled the job, false to be baked
    Bake, // on the bake thread: the bake (returns true)
};
using BakeStep = std::function<bool(BakeStage)>;

// Whose bake it is.
enum class BakePriority
{
    Place,      // of the place one stands in (its probes, its lightmaps): what the picture waits for
    Background, // nothing waits for it (a far LOD): it runs on a lane of its own and yields between its pieces while a
                // bake of the place is queued or runs
};
// Main thread. Queues `work` on its bake thread (started on first use); `what` names it in the log.
BakeTicket PostBake(std::string what, std::function<void()> work, BakePriority priority = BakePriority::Place);
// Main thread. `step(Load)` on the load thread, then, when it returns false, `step(Bake)` on its bake thread.
BakeTicket PostBake(std::string what, BakeStep step, BakePriority priority = BakePriority::Place);
// Any thread: the bakes queued (not cancelled) or running on any lane, cache loads included.
size_t BakesOutstanding();
// Cancels every bake (queued or running, the thermal pause woken) and joins the bake thread; the next PostBake
// starts it again. Also run when the program ends.
void ShutdownBakes();

// A bake's result slot as its owner holds it: the job the bake writes into and the ticket that cancels the bake when
// the owner lets go of the job (reset, a new start, destruction).
template <class Job> class Bake
{
  public:
    explicit operator bool() const { return job_ != nullptr; }
    Job *operator->() const { return job_.get(); }
    Job &operator*() const { return *job_; }
    // `work` captures `job` and fills it on the bake thread.
    void start(std::string what, std::shared_ptr<Job> job, std::function<void()> work)
    {
        reset();
        job_ = std::move(job);
        ticket_ = PostBake(std::move(what), std::move(work));
    }
    // `step` captures `job`: its load stage fills it from the cache, or its bake stage bakes it (PostBake).
    void start(std::string what, std::shared_ptr<Job> job, BakeStep step)
    {
        reset();
        job_ = std::move(job);
        ticket_ = PostBake(std::move(what), std::move(step));
    }
    void reset()
    {
        ticket_.reset();
        job_.reset();
    }

  private:
    std::shared_ptr<Job> job_;
    BakeTicket ticket_;
};

} // namespace storm::fx
