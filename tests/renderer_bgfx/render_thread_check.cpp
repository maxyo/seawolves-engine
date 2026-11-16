// bgfx's render thread through the shim (src/shim/device.hpp, Context; patches/bgfx-render-thread-hooks.patch) on
// OpenGL ES in an SDL window (an X display: xvfb-run in CI), with the render thread and with render_thread=0:
//   1. the device reports the mode asked for and the device's strings (OpenGL ES: read on the render thread), and
//      the backend self-test passes;
//   2. frames present with the game thread taking turns with the render thread, and overlapping it;
//   3. a surface release from another thread (Android's surfaceDestroyed) returns only when no frame is in flight,
//      presentation stops, and resumes after the surface is restored: many times, at any point of a frame; a restore
//      asked for before the surface was destroyed again is dropped, the next one restores; a surface release after
//      a reset waits until a frame applied it (never a later read-back's flush frame, to a destroyed window);
//   4. the renderer shuts down and starts again;
//   5. Vulkan runs only: the safety nets of a Vulkan start (backend_select.hpp). A self-test that fails starts the
//      device on OpenGL ES and records the driver, so the next start goes to OpenGL ES at once; a crash marker left
//      by a start that never confirmed does the same; a cleared cache lets Vulkan start again, auto included on a
//      hardware GPU.
#include "backend_select.hpp"
#include "device.hpp"
#include "storm/render/frame_owner.hpp"

#include "check_backend.hpp"

#include <SDL2/SDL.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>

using namespace storm;
using shim::Context;

namespace
{
constexpr uint32_t kWidth = 96, kHeight = 64;

bool expect(bool condition, const char *mode, const char *what)
{
    std::printf("%s: %s: %s\n", condition ? "ok" : "FAIL", mode, what);
    return condition;
}

void present(Context &context, int frames)
{
    for (int i = 0; i < frames; ++i)
        context.present(BGFX_INVALID_HANDLE, 0, 0);
}

bool run(SDL_Window *window, bool render_thread)
{
    const char *mode = render_thread ? "render thread" : "render_thread=0";
    Context &context = Context::instance();
    bool ok = true;
    for (int start = 0; start < 2; ++start)
    {
        render::SetRenderThread(render_thread);
        render::SetFrameOverlap(-1);
        if (!context.init(window, kWidth, kHeight, false))
            return expect(false, mode, "the device starts");
        ok &= expect(context.render_thread() == render_thread, mode, "bgfx runs in the mode asked for");
        const shim::DeviceInfo &device = context.device_info();
        ok &= expect(!device.renderer.empty() && device.renderer != "?" &&
                         (device.vulkan ? device.version.find("Vulkan") != std::string::npos
                                        : device.version.find("OpenGL ES") != std::string::npos && !device.extensions.empty()),
                     mode, "the device's strings are known after init");
        // The self-test that decides whether a device keeps Vulkan (Context::start) holds on every backend.
        std::string failure;
        ok &= expect(context.backend_self_test(&failure), mode, "the backend self-test passes");
        if (!failure.empty())
            std::printf("  %s\n", failure.c_str());
        present(context, 20);
        render::SetFrameOverlap(0);
        present(context, 20);
        render::SetFrameOverlap(1);
        present(context, 20);
        render::SetFrameOverlap(-1);
        ok &= expect(context.ready(), mode, "frames present taking turns and overlapped");

        // Android's UI thread asks for the surface at any point of the game thread's frames.
        std::atomic<bool> stop{false}, flying{false};
        std::atomic<int> releases{0};
        // The game thread's restores, as it sees them: Context::ready() is the game thread's (Android's UI thread
        // never reads it), so the fake UI thread waits on this.
        std::atomic<int> restores{0};
        std::thread ui([&] {
            for (int i = 0; i < 40 && !stop; ++i)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(300 + 173 * i));
                // A release that waited out its 3 s limit (Context::wait_render_idle) returns with the frame still in
                // flight: that outcome, not the wall time a loaded host makes the wait take, is what fails.
                context.request_surface_release();
                if (context.frames_in_flight())
                    flying = true;
                ++releases;
                const int restored = restores;
                context.request_surface_restore();
                // Until the game thread has restored the surface (a release it had not seen yet costs it no restore).
                for (int wait = 0; wait < 2000 && restores == restored && !stop; ++wait)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            stop = true;
        });
        int presented = 0;
        uint32_t generation = context.Surface().generation; // a loss and a restore both move it, even in one frame
        while (!stop) // until the UI thread's 40 releases; run.py's deadlock guard reports a hang
        {
            context.present(BGFX_INVALID_HANDLE, 0, 0); // polls the surface: honours the release, restores
            ++presented;
            if (context.ready() && context.Surface().generation != generation)
                ++restores;
            generation = context.Surface().generation;
        }
        stop = true;
        ui.join();
        ok &= expect(releases == 40, mode, "40 surface releases and restores while frames are presented");
        ok &= expect(!flying, mode, "no frame is in flight when a surface release returns");
        context.surface_restored();
        present(context, 5);
        ok &= expect(context.ready() && presented > 40, mode, "presentation resumes after the last restore");

        // surfaceChanged, then surfaceDestroyed again before the game thread took the restore (fold, split screen,
        // lock/unlock): the window SDL still hands out is the one being destroyed, so that restore is dropped; the
        // next surfaceChanged restores.
        context.request_surface_release();
        present(context, 2);
        context.request_surface_restore();
        context.request_surface_release();
        present(context, 3);
        ok &= expect(!context.ready(), mode, "a restore asked for before the surface was destroyed again is dropped");
        context.request_surface_restore();
        present(context, 3);
        ok &= expect(context.ready(), mode, "the next surfaceChanged restores");

        // A reset (here a resize; a restore or new reset flags alike) is applied by the next frame the render thread
        // runs, a read-back's flush frame too. A surfaceDestroyed right after it must not return before a frame
        // applied it to the window: Android destroys the window then, and a later read-back would apply the reset to
        // the destroyed one.
        {
            present(context, 2);
            context.resize(kWidth + 16, kHeight + 8);
            std::atomic<bool> returned{false}, pending_at_return{true};
            std::thread destroyed([&] {
                context.request_surface_release();
                pending_at_return = context.surface_reset_pending();
                returned = true;
            });
            // Android's UI thread first: the game thread's next frame comes only after a while.
            for (int wait = 0; wait < 200 && !returned; ++wait)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            while (!returned) // frames until the release returns: on its progress, not a frame budget
                present(context, 1);
            destroyed.join();
            ok &= expect(!pending_at_return, mode,
                         "a surfaceDestroyed after a reset returns only once a frame applied the reset to the window");
            present(context, 2);
            ok &= expect(!context.ready() && !context.surface_reset_pending(), mode,
                         "the surface is given up with no reset left for a read-back's flush frame");
            context.request_surface_restore();
            present(context, 3);
            ok &= expect(context.ready(), mode, "... and restored");
        }
        context.shutdown();
        ok &= expect(!context.initialised() && !context.frames_in_flight(), mode, "the renderer shuts down");
    }
    return ok;
}
// Step 5: Vulkan's crash guard and self-test fallback, with the guard's files in a fresh directory.
bool safety_nets(SDL_Window *window)
{
    const char *mode = "vulkan safety nets";
    Context &context = Context::instance();
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / ("storm-render-thread-check-" + std::to_string(getpid()));
    fs::remove_all(directory);
    const auto start = [&](render::Backend backend) {
        render::SetRenderBackend(backend, directory);
        return context.init(window, kWidth, kHeight, false);
    };
    const auto reason = [] { return render::RenderBackendChoice().reason; };
    bool ok = true;

    render::FailRenderBackendSelfTest(true);
    ok &= expect(start(render::Backend::Vulkan) && !context.device_info().vulkan && reason().find("self-test") != std::string::npos &&
                     fs::exists(directory / "vulkan-crashed.txt") && !fs::exists(directory / "vulkan-running.txt"),
                 mode, "a failed self-test starts the device on OpenGL ES and records the driver");
    context.shutdown();
    render::FailRenderBackendSelfTest(false);
    ok &= expect(start(render::Backend::Vulkan) && !context.device_info().vulkan && reason().find("failed (") != std::string::npos,
                 mode, "... and the next start goes to OpenGL ES at once, saying why");
    context.shutdown();

    fs::remove_all(directory); // «Очистить кэш»
    ok &= expect(start(render::Backend::Vulkan) && context.device_info().vulkan && fs::exists(directory / "vulkan-running.txt"),
                 mode, "a cleared cache starts Vulkan again; its marker stays until frames were presented");
    // The process "dies" here: the marker as it was before the orderly shutdown below removes it.
    const fs::path kept = directory / "kept-marker";
    fs::copy_file(directory / "vulkan-running.txt", kept);
    context.shutdown();
    ok &= expect(!fs::exists(directory / "vulkan-running.txt"), mode, "an orderly shutdown is no crash");
    fs::rename(kept, directory / "vulkan-running.txt");
    ok &= expect(start(render::Backend::Vulkan) && !context.device_info().vulkan &&
                     reason().find("crashed the process") != std::string::npos,
                 mode, "a marker left by a start that never confirmed runs OpenGL ES, saying why");
    context.shutdown();

    fs::remove_all(directory);
    ok &= expect(start(render::Backend::Vulkan) && context.device_info().vulkan, mode, "Vulkan again after the cache is cleared");
    present(context, 70);
    ok &= expect(!fs::exists(directory / "vulkan-running.txt"), mode, "presented frames confirm the start: no marker");
    context.shutdown();
    if (!shim::ProbeVulkan().software)
    {
        const bool started = start(render::Backend::Auto);
        ok &= expect(started && context.device_info().vulkan && reason().find("asks for") == std::string::npos, mode,
                     ("auto takes Vulkan on this hardware GPU: " + reason()).c_str());
        context.shutdown();
    }
    fs::remove_all(directory);
    return ok;
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
    SDL_Window *window = SDL_CreateWindow("render_thread_check", 0, 0, kWidth, kHeight, 0);
    if (!window)
    {
        std::fprintf(stderr, "FAIL: no window: %s\n", SDL_GetError());
        return 1;
    }
    bool ok = run(window, true);
    ok &= run(window, false);
    ok &= run(window, true); // and back: bgfx forgets the single-threaded choice at shutdown
    if (host_check::VulkanAsked())
        ok &= safety_nets(window);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf("%s: render thread\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
