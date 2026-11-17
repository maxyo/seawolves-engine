// The Android surface lifecycle through the shim's Context (src/shim/context.cpp) in every order, on OpenGL ES (and
// Vulkan: run.py) in an SDL window (an X display: xvfb-run in CI), with bgfx's render thread and with render_thread=0.
//
// Android's UI thread and SDL report the surface in fixed steps (corsairs-android GameActivity puts its callbacks
// ahead of SDL's):
//   surfaceDestroyed: D1 Context::request_surface_release (blocks until no frame uses the surface), then D2 SDL drops
//                     its native window;
//   surfaceCreated:   C  SDL holds the new native window;
//   surfaceChanged:   G  Context::request_surface_restore, then S SDL takes the surface's size (rotation, split
//                     screen, fold: the size may differ).
// The game thread, between and during them: P a frame presented as Device::Present does (resize to SDL's size, then
// present), O a frame begun (BeginScene) and left open, B WillEnterBackground (surface_lost), F DidEnterForeground
// (surface_restored), Z WindowResized (resize to SDL's size), X a read-back (flush frames), E new reset flags
// (refresh_reset_flags), W a draw (poll_surface).
//
// Every sequence is a UI script (destroy/create/change, in the order Android sends them) with one game step at every
// position, or a pair of them (pause and resume, a frame begun and a pause, new reset flags and a read-back) at every
// two positions. A UI step that
// waits for the game thread (a release waiting for a reset to be applied) lets the game thread go on with its script,
// as the real game loop does. Each sequence then ends the way Android does (the surface back, the app in the
// foreground) and checks:
//   - no presented frame and no reset of the surface from the moment a surfaceDestroyed returned until SDL holds a new
//     window (Android destroys that window: a frame or an EGL surface made on it crashes or fails);
//   - a surfaceDestroyed returns with no reset left to apply and, when the game thread presented to the surface and
//     waited for it, no frame in flight;
//   - no ERROR line from the shim (a 3 s wait that ran out, a reset left for a destroyed window);
//   - the surface is restored within two frames, at the window's size, and a presented frame reaches the render
//     thread (no black screen).
// The checks wait on progress; a watchdog reports a sequence that makes none for 30 s (a deadlock) and ends the run.
#include "check_backend.hpp"
#include "device.hpp"
#include "storm/render/frame_owner.hpp"
#include "storm/render/log.hpp"

#include <SDL2/SDL.h>
#include <bgfx/bgfx.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace storm;
using shim::Context;

namespace
{
constexpr int kWidth = 96, kHeight = 64;

struct Step
{
    char op;          // see the top of the file
    bool other_size;  // S: the other of the two sizes (landscape / portrait)
    bool ui() const { return op == '1' || op == '2' || op == 'C' || op == 'G' || op == 'S'; }
};

std::string describe(const std::vector<Step> &steps)
{
    std::string text;
    for (const Step &step : steps)
    {
        if (!text.empty())
            text += ' ';
        if (step.op == '1' || step.op == '2')
            text += std::string("D") + step.op;
        else
            text += step.op;
        if (step.op == 'S' && step.other_size)
            text += '\'';
    }
    return text;
}

Context::HostWindow host_window;
std::atomic<bool> gone{false}; // a surfaceDestroyed returned and SDL holds no new window yet
std::atomic<uint64_t> progress{0};

// The shim's log: ERROR lines and resets of a surface Android has taken away fail the sequence.
std::mutex log_mutex;
std::deque<std::string> recent;
std::vector<std::string> problems;
void log_sink(render::LogLevel level, const char *line)
{
    std::lock_guard lock(log_mutex);
    recent.emplace_back(line);
    if (recent.size() > 40)
        recent.pop_front();
    if (level == render::LogLevel::Error)
        problems.push_back(std::string("shim error: ") + line);
    else if (gone && (std::strncmp(line, "surface restored ", 17) == 0 || std::strncmp(line, "surface resized ", 16) == 0))
        problems.push_back(std::string("the surface was reset after Android took it away: ") + line);
}
void problem(const std::string &text)
{
    std::lock_guard lock(log_mutex);
    problems.push_back(text);
}

// Android's UI thread: runs the UI steps in order.
class UiThread
{
  public:
    explicit UiThread(Context &context) : context_(context), thread_([this] { run(); }) {}
    ~UiThread()
    {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    // `game_waits`: the game thread does nothing until this step returns (or goes on, game_goes_on) and presents to
    // the surface (a release must then wait for every frame in flight; with no surface in use only read-backs' flush
    // frames can be, and they may still run).
    void post(Step step, bool game_waits)
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(step);
        game_waits_ = game_waits;
        idle_ = false;
        wake_.notify_all();
    }
    bool idle() const { return idle_; }
    bool releasing() const { return releasing_; }
    void game_goes_on() { game_waits_ = false; }

  private:
    void run()
    {
        std::unique_lock lock(mutex_);
        for (;;)
        {
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_)
                return;
            const Step step = queue_.front();
            queue_.pop_front();
            lock.unlock();
            perform(step);
            ++progress;
            lock.lock();
            if (queue_.empty())
                idle_ = true;
        }
    }
    void perform(Step step)
    {
        switch (step.op)
        {
        case '1':
            releasing_ = true;
            context_.request_surface_release();
            releasing_ = false;
            if (context_.surface_reset_pending())
                problem("surfaceDestroyed returned with a reset of the surface not applied yet");
            if (game_waits_ && context_.frames_in_flight())
                problem("surfaceDestroyed returned with a frame in flight");
            gone = true;
            break;
        case '2': host_window.present = false; break;
        case 'C':
            host_window.present = true;
            gone = false;
            break;
        case 'G': context_.request_surface_restore(); break;
        case 'S':
            if (step.other_size)
            {
                const int width = host_window.width;
                host_window.width = host_window.height.load();
                host_window.height = width;
            }
            break;
        }
    }

    Context &context_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Step> queue_;
    std::atomic<bool> idle_{true}, releasing_{false}, game_waits_{false};
    bool stop_ = false;
    std::thread thread_;
};

class Run
{
  public:
    Run(SDL_Window *window, bool render_thread) : window_(window), render_thread_(render_thread), ui_(context_) {}

    bool start()
    {
        render::SetRenderThread(render_thread_);
        host_window.present = true;
        host_window.width = kWidth;
        host_window.height = kHeight;
        background_ = false;
        gone = false;
        context_.set_host_window(&host_window);
        return context_.init(window_, kWidth, kHeight, false);
    }
    void stop() { context_.shutdown(); }

    // One sequence; false (and its story printed) when it failed.
    bool sequence(const std::vector<Step> &steps)
    {
        {
            std::lock_guard lock(log_mutex);
            problems.clear();
            recent.clear();
        }
        current_ = describe(steps);
        for (const Step &step : steps)
        {
            if (step.ui())
                ui_step(step);
            else
                game_step(step.op);
            ++progress;
        }
        settle();
        std::lock_guard lock(log_mutex);
        if (problems.empty())
            return true;
        std::printf("FAIL: %s: %s\n", render_thread_ ? "render thread" : "render_thread=0", current_.c_str());
        for (const std::string &text : problems)
            std::printf("  %s\n", text.c_str());
        std::printf("  the shim's last lines:\n");
        for (const std::string &line : recent)
            std::printf("    %s\n", line.c_str());
        return false;
    }
    const std::string &current() const { return current_; }

  private:
    // The game thread's next safe point while Android's UI thread waits for it.
    void wait_ui()
    {
        while (!ui_.idle())
        {
            if (ui_.releasing() && context_.surface_reset_pending())
                context_.poll_surface();
            else
                std::this_thread::yield();
            check_gone();
        }
    }
    void ui_step(Step step)
    {
        wait_ui();
        ui_.post(step, context_.ready());
        // A release that waits for a reset to be applied waits for the game thread: it goes on with its script.
        while (!ui_.idle())
        {
            if (ui_.releasing() && context_.surface_reset_pending())
            {
                ui_.game_goes_on();
                return;
            }
            std::this_thread::yield();
        }
        check_gone();
    }
    void device_present()
    {
        context_.resize(host_window.width, host_window.height);
        context_.present(BGFX_INVALID_HANDLE, 0, 0);
    }
    void game_step(char op)
    {
        switch (op)
        {
        case 'P': device_present(); break;
        case 'O': context_.begin_frame_if_needed(); break;
        case 'B':
            context_.surface_lost();
            background_ = true;
            break;
        case 'F':
            context_.surface_restored();
            background_ = false;
            break;
        case 'Z': context_.resize(host_window.width, host_window.height); break;
        case 'X':
            context_.run_frame(BGFX_FRAME_FLUSH);
            context_.run_frame(BGFX_FRAME_FLUSH);
            break;
        case 'E': context_.refresh_reset_flags(); break;
        case 'W': context_.poll_surface(); break;
        }
        check_gone();
    }
    // While the window is gone: no presented frame, no reset waiting to be applied to it.
    void check_gone()
    {
        if (!gone)
        {
            gone_seen_ = false;
            return;
        }
        if (!gone_seen_)
        {
            gone_seen_ = true;
            gone_frame_ = context_.last_frame();
            return;
        }
        if (context_.last_frame() != gone_frame_)
            problem("a frame was presented after Android took the surface away");
        if (context_.surface_reset_pending())
            problem("a reset of the surface is pending after Android took it away");
        gone_frame_ = context_.last_frame();
    }
    // Android gives the surface back and resumes the app: the game must present again.
    void settle()
    {
        wait_ui();
        if (!host_window.present)
        {
            ui_step({'C', false});
            ui_step({'G', false});
            ui_step({'S', false});
            wait_ui();
        }
        if (background_)
            game_step('F');
        int frames = 0;
        for (; frames < 2 && !context_.ready(); ++frames)
            device_present();
        if (!context_.ready())
        {
            problem("black screen: no surface after the surface came back and two frames");
            recover();
            return;
        }
        const uint32_t before = context_.last_frame();
        device_present();
        if (context_.last_frame() == before)
            problem("black screen: the surface is ready but a frame was not presented");
        while (context_.frames_in_flight()) // the render thread finishes it: progress, the watchdog reports a hang
            std::this_thread::yield();
        const render::SurfaceMetrics surface = context_.Surface();
        if (static_cast<int>(surface.width_px) != host_window.width || static_cast<int>(surface.height_px) != host_window.height)
            problem("the surface is " + std::to_string(surface.width_px) + "x" + std::to_string(surface.height_px) +
                    ", the window " + std::to_string(host_window.width) + "x" + std::to_string(host_window.height));
    }
    // A failed sequence leaves the next one a fresh device.
    void recover()
    {
        stop();
        if (!start())
            problem("the device does not start again");
    }

    Context &context_ = Context::instance();
    SDL_Window *window_;
    bool render_thread_;
    UiThread ui_;
    bool background_ = false;
    bool gone_seen_ = false;
    uint32_t gone_frame_ = 0;
    std::string current_;
};

std::vector<Step> parse(const char *ui)
{
    std::vector<Step> steps;
    for (const char *at = ui; *at; ++at)
    {
        if (*at == ' ')
            continue;
        if (*at == '\'')
            steps.back().other_size = true;
        else
            steps.push_back({*at, false});
    }
    return steps;
}

std::vector<std::vector<Step>> sequences()
{
    // In Android's order: surfaceDestroyed = 1 2, surfaceCreated + surfaceChanged = C G S, surfaceChanged = G S.
    const char *scripts[] = {
        "1 2 C G S",               // background and back, or the surface re-made at the same size
        "1 2 C G S'",              // ... at the other size (rotated while away, split screen)
        "G S'",                    // rotation: surfaceChanged without a new surface
        "G S' 1 2 C G S",          // rotation, then the surface re-made
        "1 2 C G S' 1 2 C G S'",   // the surface re-made twice in a row (fold, lock/unlock)
        "1 2",                     // the surface goes away (the end brings it back)
    };
    const char game[] = {'P', 'O', 'B', 'F', 'Z', 'X', 'E', 'W'};
    const std::pair<char, char> pairs[] = {{'B', 'F'}, {'O', 'B'}, {'E', 'X'}};
    std::vector<std::vector<Step>> all;
    for (const char *script : scripts)
    {
        const std::vector<Step> ui = parse(script);
        for (char op : game)
            for (size_t at = 0; at <= ui.size(); ++at)
            {
                std::vector<Step> steps = ui;
                steps.insert(steps.begin() + static_cast<long>(at), Step{op, false});
                all.push_back(steps);
            }
        for (const auto &[first, second] : pairs)
            for (size_t a = 0; a <= ui.size(); ++a)
                for (size_t b = a; b <= ui.size(); ++b)
                {
                    std::vector<Step> steps = ui;
                    steps.insert(steps.begin() + static_cast<long>(b), Step{second, false});
                    steps.insert(steps.begin() + static_cast<long>(a), Step{first, false});
                    all.push_back(steps);
                }
    }
    return all;
}
} // namespace

int main()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::fprintf(stderr, "FAIL: SDL video: %s (an X display is needed)\n", SDL_GetError());
        return 1;
    }
    host_check::UseBackendFromEnvironment();
    SDL_Window *window = SDL_CreateWindow("surface_lifecycle_check", 0, 0, kWidth, kHeight, 0);
    if (!window)
    {
        std::fprintf(stderr, "FAIL: no window: %s\n", SDL_GetError());
        return 1;
    }
    render::SetLogSink(&log_sink);
    std::mutex current_mutex;
    std::string current = "start";
    std::atomic<bool> finished{false};
    std::thread watchdog([&] {
        uint64_t seen = progress;
        int still = 0;
        while (!finished)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (progress != seen)
            {
                seen = progress;
                still = 0;
                continue;
            }
            if (++still < 30)
                continue;
            std::lock_guard lock(current_mutex);
            std::printf("FAIL: no progress for 30 s (a deadlock) in: %s\n", current.c_str());
            std::lock_guard log(log_mutex);
            for (const std::string &line : recent)
                std::printf("    %s\n", line.c_str());
            std::fflush(stdout);
            _exit(1);
        }
    });
    const std::vector<std::vector<Step>> all = sequences();
    const auto started = std::chrono::steady_clock::now();
    int failed = 0;
    for (const bool render_thread : {true, false})
    {
        const char *mode = render_thread ? "render thread" : "render_thread=0";
        Run run(window, render_thread);
        if (!run.start())
        {
            std::printf("FAIL: %s: the device starts\n", mode);
            failed = -1;
            break;
        }
        // run.py's Vulkan pass looks for this line (the shim's own goes to the log sink).
        std::printf("render backend: %s\n", render::RenderBackendChoice().line.c_str());
        int mode_failed = 0;
        for (const std::vector<Step> &steps : all)
        {
            {
                std::lock_guard lock(current_mutex);
                current = std::string(mode) + ": " + describe(steps);
            }
            if (!run.sequence(steps))
                ++mode_failed;
        }
        run.stop();
        std::printf("%s: %s: %zu sequences, %d failed\n", mode_failed ? "FAIL" : "ok", mode, all.size(), mode_failed);
        failed += mode_failed;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    finished = true;
    watchdog.join();
    render::SetLogSink(nullptr);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf("%s: surface lifecycle, %zu sequences in each mode, %.1f s\n", failed ? "FAIL" : "PASS", all.size(), seconds);
    return failed ? 1 : 0;
}
