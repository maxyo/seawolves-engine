#pragma once
// The stall watchdog: a diagnostic, never a recovery. A thread of its own notices that the game thread has neither
// come back to its main loop nor presented a frame for Config::stall_seconds (in the foreground), or that bgfx's render thread has not finished a frame it was
// handed for as long, and reports what each was doing (platform/stall_watch.hpp: the game thread's frame_work phase,
// synchronous load, entity stage and script event; the render thread's frame and present) with native backtraces of
// the stalled threads: the watchdog signals the thread, whose handler unwinds its own stack (_Unwind_Backtrace, or the
// frame-pointer chain when the unwinder does not get past the signal frame). Frames are logged like a tombstone's
// ("#00 pc <offset in the module>  <module> (<symbol>+<offset>)") so that ndk-stack or llvm-symbolizer resolve them
// offline with the build's unstripped libraries.
//
// A report is repeated every Config::repeat_seconds while the stall lasts; when the threads move again one line gives
// the total. A long frame inside a load (the loading transaction or the loading screen) is reported as such at warning
// level; any other stall at error level. Both levels are flushed at once (logging.cpp flush_on): the process is often
// killed next.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace storm::diagnostics
{

class StallWatchdog
{
  public:
    enum class Level
    {
        Warning,
        Error,
    };
    using Sink = std::function<void(Level, const std::string &)>;

    struct Config
    {
        double stall_seconds = 5.0;
        double repeat_seconds = 10.0;
        double poll_seconds = 0.5;
        double backtrace_wait_seconds = 1.0; // how long a stalled thread may take to answer the signal
    };

    StallWatchdog(Config config, Sink sink);
    ~StallWatchdog();
    StallWatchdog(const StallWatchdog &) = delete;
    StallWatchdog &operator=(const StallWatchdog &) = delete;

    void Start();
    void Stop();

  private:
    struct Episode
    {
        bool active = false;
        int64_t since = 0; // ns: the stall counts from here
        uint32_t frame = 0; // render thread: the frame it was busy with
    };

    void Run();
    void Check();
    void Report(int64_t now, bool game_stalled, bool render_stalled, bool render_busy, int64_t render_since);
    std::vector<std::string> Backtrace(const char *name, int tid);

    const Config config_;
    const Sink sink_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    bool signal_ready_ = false;

    Episode game_, render_;
    int64_t last_report_ = 0;
    int reports_ = 0;
    Level level_ = Level::Error;
};

} // namespace storm::diagnostics
