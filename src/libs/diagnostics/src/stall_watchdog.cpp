#include "stall_watchdog.hpp"

#include "loading_transaction.hpp"
#include "platform/frame_work.hpp"
#include "platform/stall_watch.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#if defined(__linux__)
#include <csignal>
#include <cxxabi.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>
#define STORM_STALL_BACKTRACE 1
#endif

namespace storm::diagnostics
{
namespace sw = storm::platform::stall_watch;
namespace fw = storm::platform::frame_work;

namespace
{
double Seconds(int64_t ns)
{
    return static_cast<double>(ns) * 1e-9;
}
int64_t Nanoseconds(double seconds)
{
    return static_cast<int64_t>(seconds * 1e9);
}

std::string Format(const char *format, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char *format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

#if STORM_STALL_BACKTRACE
// One capture at a time: the watchdog thread asks (state 1), the target thread's handler answers (state 2).
constexpr int kMaxFrames = 64;
struct Capture
{
    std::atomic<int> target{0};
    std::atomic<int> state{0};
    uintptr_t stack_low = 0, stack_high = 0;
    uintptr_t frames[kMaxFrames];
    int count = 0;
    bool frame_pointers = false; // the frames come from the frame-pointer walk
};
Capture capture;

int Signal()
{
    return SIGRTMIN + 5;
}

struct UnwindState
{
    uintptr_t *frames;
    int count;
};

_Unwind_Reason_Code Collect(_Unwind_Context *context, void *argument)
{
    auto *state = static_cast<UnwindState *>(argument);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc == 0 || state->count >= kMaxFrames)
        return _URC_END_OF_STACK;
    state->frames[state->count++] = pc;
    return _URC_NO_REASON;
}

// The target thread, in the signal handler: async-signal-safe work only (no allocation, no locks of our own).
void Handler(int, siginfo_t *, void *raw)
{
    if (capture.state.load(std::memory_order_acquire) != 1 ||
        static_cast<int>(syscall(SYS_gettid)) != capture.target.load(std::memory_order_relaxed))
        return;
    const int saved_errno = errno;
    uintptr_t pc = 0, fp = 0, lr = 0;
    const auto *context = static_cast<const ucontext_t *>(raw);
#if defined(__aarch64__)
    pc = context->uc_mcontext.pc;
    fp = context->uc_mcontext.regs[29];
    lr = context->uc_mcontext.regs[30];
#elif defined(__x86_64__)
    pc = static_cast<uintptr_t>(context->uc_mcontext.gregs[REG_RIP]);
    fp = static_cast<uintptr_t>(context->uc_mcontext.gregs[REG_RBP]);
#else
    (void)context;
#endif
    // The unwinder walks from here, through the signal frame, into the interrupted code: the frames from the
    // interrupted pc on are the thread's.
    UnwindState state{capture.frames, 0};
    _Unwind_Backtrace(&Collect, &state);
    int first = -1;
    for (int i = 0; i < state.count && pc; ++i)
        if (capture.frames[i] == pc)
        {
            first = i;
            break;
        }
    if (first >= 0)
    {
        for (int i = first; i < state.count; ++i)
            capture.frames[i - first] = capture.frames[i];
        capture.count = state.count - first;
        capture.frame_pointers = false;
    }
    else
    {
        // The unwinder did not get past the signal frame: the frame-pointer chain from the interrupted registers,
        // read only inside the thread's stack.
        int count = 0;
        if (pc)
            capture.frames[count++] = pc;
        if (lr)
            capture.frames[count++] = lr;
        while (count < kMaxFrames && fp >= capture.stack_low && fp + 2 * sizeof(uintptr_t) <= capture.stack_high &&
               fp % sizeof(uintptr_t) == 0)
        {
            const auto *record = reinterpret_cast<const uintptr_t *>(fp);
            if (record[1] == 0)
                break;
            if (count == 0 || record[1] != capture.frames[count - 1])
                capture.frames[count++] = record[1];
            if (record[0] <= fp)
                break;
            fp = record[0];
        }
        capture.count = count;
        capture.frame_pointers = true;
    }
    errno = saved_errno;
    capture.state.store(2, std::memory_order_release);
}

std::string Symbol(uintptr_t pc, bool return_address)
{
    Dl_info info{};
    // A return address points after its call: the call's own function is looked up.
    if (!dladdr(reinterpret_cast<void *>(return_address ? pc - 1 : pc), &info) || !info.dli_fname)
        return Format("pc %016" PRIxPTR "  <unknown>", pc);
    const uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    std::string line = Format("pc %016" PRIxPTR "  %s", pc - base, info.dli_fname);
    if (info.dli_sname)
    {
        int status = 0;
        char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        line += Format(" (%s+%" PRIuPTR ")", status == 0 && demangled ? demangled : info.dli_sname,
                       pc - reinterpret_cast<uintptr_t>(info.dli_saddr));
        std::free(demangled);
    }
    return line;
}

// "S (sleeping), wchan futex_wait_queue" from /proc: what the kernel says the thread does.
std::string KernelState(int tid)
{
    std::ifstream stat("/proc/self/task/" + std::to_string(tid) + "/stat");
    std::string text;
    std::getline(stat, text);
    const auto close = text.rfind(')');
    if (close == std::string::npos || close + 2 >= text.size())
        return "kernel state unreadable";
    const char state = text[close + 2];
    const char *meaning = state == 'R' ? "running" : state == 'S' ? "sleeping" : state == 'D' ? "uninterruptible wait"
                        : state == 'T' || state == 't' ? "stopped" : "other";
    std::string result = Format("kernel state %c (%s)", state, meaning);
    std::ifstream wchan("/proc/self/task/" + std::to_string(tid) + "/wchan");
    std::string channel;
    if (std::getline(wchan, channel) && !channel.empty() && channel != "0")
        result += ", wchan " + channel;
    return result;
}
#endif
} // namespace

StallWatchdog::StallWatchdog(Config config, Sink sink) : config_(config), sink_(std::move(sink))
{
}

StallWatchdog::~StallWatchdog()
{
    Stop();
}

void StallWatchdog::Start()
{
    if (thread_.joinable())
        return;
#if STORM_STALL_BACKTRACE
    struct sigaction previous{};
    if (sigaction(Signal(), nullptr, &previous) != 0)
        sink_(Level::Error, Format("stall watchdog: cannot read the action of signal %d (errno %d): stalls are reported "
                                   "without backtraces", Signal(), errno));
    else if ((previous.sa_flags & SA_SIGINFO) ? previous.sa_sigaction != nullptr
                                              : previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN)
        sink_(Level::Error, Format("stall watchdog: signal %d already has a handler: stalls are reported without "
                                   "backtraces", Signal()));
    else
    {
        struct sigaction action{};
        action.sa_sigaction = &Handler;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        signal_ready_ = sigaction(Signal(), &action, nullptr) == 0;
        if (!signal_ready_)
            sink_(Level::Error, Format("stall watchdog: cannot set the handler of signal %d (errno %d): stalls are "
                                       "reported without backtraces", Signal(), errno));
    }
#endif
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
}

void StallWatchdog::Stop()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void StallWatchdog::Run()
{
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "stall-watchdog");
#endif
    std::unique_lock lock(mutex_);
    while (!wake_.wait_for(lock, std::chrono::duration<double>(config_.poll_seconds), [this] { return stop_; }))
    {
        lock.unlock();
        Check();
        lock.lock();
    }
}

void StallWatchdog::Check()
{
    const int64_t now = sw::Now();
    const sw::Game &game = sw::game();
    const sw::Render &render = sw::render();
    const bool background = sw::background.load(std::memory_order_relaxed);
    const int64_t progress = game.progress.load(std::memory_order_relaxed);

    // The game thread: no frame finished since `reference` (a return to the foreground starts the count again).
    const bool game_watched = game.tid.load(std::memory_order_acquire) != 0;
    const int64_t reference = std::max(progress, sw::resumed.load(std::memory_order_relaxed));
    const bool game_stalled = game_watched && !background && now - reference >= Nanoseconds(config_.stall_seconds);

    // The render thread: busy with a handed frame it has not finished.
    const uint32_t handed = render.handed.load(std::memory_order_relaxed);
    const uint32_t done = render.done.load(std::memory_order_relaxed);
    const bool render_busy = render.tid.load(std::memory_order_relaxed) != 0 &&
                             render.any_handed.load(std::memory_order_acquire) && static_cast<int32_t>(handed - done) > 0;
    const uint32_t working = done + 1;
    const int64_t render_since = std::max(render.done_at.load(std::memory_order_relaxed),
                                          render.handed_at[working & 3].load(std::memory_order_relaxed));
    const bool render_stalled = render_busy && now - render_since >= Nanoseconds(config_.stall_seconds);

    // Recoveries and the end of a stall in the background.
    if (game_.active && progress > game_.since)
    {
        sink_(level_, Format("stall watchdog: the game thread moves again after %.1f s",
                             Seconds(progress - game_.since)));
        game_.active = false;
    }
    else if (game_.active && background)
    {
        sink_(level_, Format("stall watchdog: the app went to the background %.1f s into the game thread's stall",
                             Seconds(now - game_.since)));
        game_.active = false;
    }
    if (render_.active && static_cast<int32_t>(done - render_.frame) >= 0)
    {
        sink_(level_, Format("stall watchdog: the render thread finished frame %u after %.1f s", render_.frame,
                             Seconds(render.done_at.load(std::memory_order_relaxed) - render_.since)));
        render_.active = false;
    }

    if (!game_stalled && !render_stalled)
    {
        if (!game_.active && !render_.active)
            reports_ = 0;
        return;
    }
    const bool started = (game_stalled && !game_.active) || (render_stalled && !render_.active);
    if (game_stalled && !game_.active)
        game_ = {true, reference, 0};
    if (render_stalled && !render_.active)
        render_ = {true, render_since, working};
    if (!started && now - last_report_ < Nanoseconds(config_.repeat_seconds))
        return;
    // A long frame inside a load (the loading transaction, the loading screen, a save's state applied) is a warning.
    if (reports_ == 0)
        level_ = storm::loading::active || storm::loading::screen_up ||
                         sw::game().phase.load(std::memory_order_relaxed) == static_cast<uint8_t>(fw::Phase::Load)
                     ? Level::Warning
                     : Level::Error;
    Report(now, game_stalled, render_stalled, render_busy, render_since);
    last_report_ = now;
    ++reports_;
}

void StallWatchdog::Report(int64_t now, bool game_stalled, bool render_stalled, bool render_busy, int64_t render_since)
{
    const sw::Game &game = sw::game();
    const sw::Render &render = sw::render();
    const bool loading = storm::loading::active, screen = storm::loading::screen_up;

    std::string text = Format("stall watchdog (report %d): ", reports_ + 1);
    const int game_tid = game.tid.load(std::memory_order_relaxed);
    const int render_tid = render.tid.load(std::memory_order_relaxed);
    if (game_stalled)
        text += Format("the game thread (tid %d) has not come back to its main loop nor presented a frame for %.1f s", game_tid, Seconds(now - game_.since));
    if (render_stalled)
        text += Format("%sthe render thread (tid %d) has not finished frame %u for %.1f s", game_stalled ? "; " : "",
                       render_tid, render_.frame, Seconds(now - render_since));
    if (loading || screen)
        text += Format(" (inside a load: %s%s%s)", loading ? "the loading transaction" : "", loading && screen ? ", " : "",
                       screen ? "the loading screen is up" : "");
    else if (level_ == Level::Warning)
        text += " (inside a load: a save's state is applied)";

    // What the game thread does.
    const uint8_t phase = game.phase.load(std::memory_order_relaxed);
    text += Format(". Game thread: phase %s for %.1f s", phase < fw::kPhases ? fw::kPhaseNames[phase] : "?",
                   Seconds(now - game.phase_since.load(std::memory_order_relaxed)));
    const uint8_t sync = game.sync.load(std::memory_order_relaxed);
    if (sync != 0 && sync < fw::kSyncs)
        text += Format(", %s for %.1f s", fw::kSyncNames[sync], Seconds(now - game.sync_since.load(std::memory_order_relaxed)));
    if (const std::type_info *entity = game.entity.load(std::memory_order_relaxed))
        text += Format(", entity %s:%s for %.1f s", game.entity_realize.load(std::memory_order_relaxed) ? "realize" : "execute",
                       fw::ClassName(entity).c_str(), Seconds(now - game.entity_since.load(std::memory_order_relaxed)));
    const std::string event = sw::EventName();
    if (!event.empty())
        text += Format(", script event %s for %.1f s", event.c_str(), Seconds(now - game.event_since.load(std::memory_order_relaxed)));

    // What the render thread does.
    if (render_tid == 0)
        text += "; render thread: not watched (none, or no frame finished yet)";
    else if (!render_busy)
        text += Format("; render thread: idle, waiting for the game thread's next frame (frame %u finished %.1f s ago)",
                       render.done.load(std::memory_order_relaxed),
                       Seconds(now - render.done_at.load(std::memory_order_relaxed)));
    else
    {
        const uint32_t working = render.done.load(std::memory_order_relaxed) + 1;
        text += Format("; render thread: frame %u (handed %u) for %.1f s", working, render.handed.load(std::memory_order_relaxed),
                       Seconds(now - render_since));
        const int64_t present = render.present_since.load(std::memory_order_relaxed);
        const char *call = render.present_call.load(std::memory_order_relaxed);
        if (present)
            text += Format(", in the present (%s) for %.1f s", call ? call : "?", Seconds(now - present));
        else
            text += ", not in the present: submitting, acquiring the swap chain image or waiting a fence (see its backtrace)";
    }
    sink_(level_, text);

    if (game_stalled && game_tid)
        for (const std::string &line : Backtrace("game", game_tid))
            sink_(level_, line);
    if (render_busy && render_tid)
        for (const std::string &line : Backtrace("render", render_tid))
            sink_(level_, line);
}

std::vector<std::string> StallWatchdog::Backtrace(const char *name, int tid)
{
#if STORM_STALL_BACKTRACE
    const std::string kernel = KernelState(tid);
    if (!signal_ready_)
        return {Format("stall watchdog: %s thread (tid %d): %s; no backtrace (no signal handler)", name, tid, kernel.c_str())};
    const sw::Thread &thread = std::string(name) == "game" ? static_cast<const sw::Thread &>(sw::game())
                                                           : static_cast<const sw::Thread &>(sw::render());
    capture.stack_low = thread.stack_low.load(std::memory_order_relaxed);
    capture.stack_high = thread.stack_high.load(std::memory_order_relaxed);
    capture.count = 0;
    capture.target.store(tid, std::memory_order_relaxed);
    capture.state.store(1, std::memory_order_release);
    if (syscall(SYS_tgkill, getpid(), tid, Signal()) != 0)
    {
        capture.state.store(0, std::memory_order_relaxed);
        return {Format("stall watchdog: %s thread (tid %d): %s; no backtrace (signal failed, errno %d)", name, tid,
                       kernel.c_str(), errno)};
    }
    const int64_t deadline = sw::Now() + Nanoseconds(config_.backtrace_wait_seconds);
    while (capture.state.load(std::memory_order_acquire) != 2 && sw::Now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (capture.state.load(std::memory_order_acquire) != 2)
    {
        capture.state.store(0, std::memory_order_relaxed);
        return {Format("stall watchdog: %s thread (tid %d): %s; no backtrace: the thread did not answer the signal within "
                       "%.1f s (blocked in the kernel, e.g. a driver call)",
                       name, tid, kernel.c_str(), config_.backtrace_wait_seconds)};
    }
    // One line per frame, as a tombstone's, so that ndk-stack reads the log as it is.
    std::vector<std::string> lines{Format("stall watchdog: %s thread (tid %d) backtrace, %d frames (%s), %s:", name, tid,
                                          capture.count, capture.frame_pointers ? "frame pointers" : "unwinder",
                                          kernel.c_str())};
    for (int i = 0; i < capture.count; ++i)
        lines.push_back(Format("stall watchdog: %s #%02d ", name, i) + Symbol(capture.frames[i], i > 0));
    capture.state.store(0, std::memory_order_relaxed);
    return lines;
#else
    return {Format("stall watchdog: %s thread (tid %d): no backtrace on this platform", name, tid)};
#endif
}

} // namespace storm::diagnostics
