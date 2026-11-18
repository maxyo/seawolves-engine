// The backend choice (src/shim/backend_select.hpp) without a GPU: what DecideBackend answers for each request and
// device, the denylist, the crash guard of a Vulkan start (VulkanStartGuard on storm/fx/gpu_crash_guard.hpp), and a
// bgfx FATAL reaching the engine's log at error level before the process aborts (ProgramCache::fatal).
#include "backend_select.hpp"
#include "program_cache.hpp"
#include "storm/render/log.hpp"

#include <csignal>
#include <sys/wait.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace storm;
using shim::DecideBackend;
using shim::VulkanDevice;
using shim::VulkanStartGuard;

namespace
{
bool expect(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    return ok;
}

// The child's log sink: every line with its level, to the pipe the parent reads.
int log_pipe = -1;
void pipe_sink(render::LogLevel level, const char *line)
{
    const std::string text = std::string(level == render::LogLevel::Error ? "E " : level == render::LogLevel::Warning ? "W " : "I ") + line + "\n";
    [[maybe_unused]] const ssize_t written = write(log_pipe, text.data(), text.size());
}

// Runs `call` in a child with pipe_sink as the renderer's log sink: what it logged and how it ended.
std::string in_child(void (*call)(), int *status)
{
    int fds[2];
    if (pipe(fds) != 0)
        return "pipe failed";
    const pid_t child = fork();
    if (child == 0)
    {
        close(fds[0]);
        log_pipe = fds[1];
        render::SetLogSink(&pipe_sink);
        call();
        _exit(0);
    }
    close(fds[1]);
    std::string out;
    char buffer[512];
    for (ssize_t got; (got = read(fds[0], buffer, sizeof(buffer))) > 0;)
        out.append(buffer, static_cast<size_t>(got));
    close(fds[0]);
    waitpid(child, status, 0);
    return out;
}

VulkanDevice device(uint32_t major, uint32_t minor)
{
    VulkanDevice out;
    out.name = "Test GPU";
    out.api_version = major << 22 | minor << 12;
    out.driver_version = 7;
    out.vendor_id = 0x5143;
    out.device_id = 0x43050a01;
    out.swapchain = true;
    out.compressed_textures = true;
    return out;
}
} // namespace

int main()
{
    bool ok = true;
    const VulkanDevice good = device(1, 3);
    VulkanDevice none;
    none.problem = "no Vulkan loader on this system (libvulkan)";
    VulkanDevice old = device(1, 0);
    VulkanDevice bare = device(1, 1);
    bare.swapchain = false;

    VulkanDevice software = device(1, 3);
    software.software = true;
    VulkanDevice uncompressed = device(1, 3);
    uncompressed.compressed_textures = false;
    const std::string crashed = "crashed the process";

    ok &= expect(!DecideBackend(render::Backend::OpenGLES, good, {}).vulkan, "gles asked: OpenGL ES on a Vulkan device");
    ok &= expect(DecideBackend(render::Backend::Vulkan, good, {}).vulkan, "vulkan asked: Vulkan on a 1.3 device");
    ok &= expect(DecideBackend(render::Backend::Vulkan, device(1, 1), {}).vulkan, "vulkan asked: Vulkan on a 1.1 device");
    ok &= expect(!DecideBackend(render::Backend::Vulkan, old, {}).vulkan, "vulkan asked: not on a 1.0 device");
    ok &= expect(!DecideBackend(render::Backend::Vulkan, none, {}).vulkan, "vulkan asked: not without a loader");
    ok &= expect(!DecideBackend(render::Backend::Vulkan, bare, {}).vulkan, "vulkan asked: not without a swap chain");
    ok &= expect(!DecideBackend(render::Backend::Vulkan, good, crashed).vulkan,
                 "vulkan asked: not after a Vulkan start on this driver crashed or failed");
    ok &= expect(DecideBackend(render::Backend::Vulkan, software, {}).vulkan &&
                     DecideBackend(render::Backend::Vulkan, uncompressed, {}).vulkan,
                 "vulkan asked: also on a CPU implementation and without compressed textures (host checks, lavapipe)");
    for (const VulkanDevice *each : {&none, &old, &bare})
        ok &= expect(!DecideBackend(render::Backend::Vulkan, *each, {}).reason.empty(), "every refusal has a reason");

    // auto: Vulkan wherever the device has what the game needs.
    ok &= expect(DecideBackend(render::Backend::Auto, good, {}).vulkan, "auto: Vulkan on a capable 1.3 device");
    ok &= expect(DecideBackend(render::Backend::Auto, device(1, 1), {}).vulkan, "auto: Vulkan on a capable 1.1 device");
    ok &= expect(!DecideBackend(render::Backend::Auto, old, {}).vulkan && !DecideBackend(render::Backend::Auto, none, {}).vulkan &&
                     !DecideBackend(render::Backend::Auto, bare, {}).vulkan,
                 "auto: never where a Vulkan asked for is refused");
    const shim::BackendDecision after_crash = DecideBackend(render::Backend::Auto, good, crashed);
    ok &= expect(!after_crash.vulkan && after_crash.reason.find(crashed) != std::string::npos,
                 "auto: OpenGL ES after a crash on this driver, and says so");
    ok &= expect(!DecideBackend(render::Backend::Auto, software, {}).vulkan, "auto: not on a CPU implementation");
    ok &= expect(!DecideBackend(render::Backend::Auto, uncompressed, {}).vulkan,
                 "auto: not without ETC2 or BC textures (8x the texture memory)");
    ok &= expect(shim::VulkanDenied(good).empty(), "the denylist holds no driver without evidence");

    // The denylist: every Imagination driver (PowerVR Rogue 1.13@5776728 on the Galaxy A12, D-Series 25.1@6794074 on
    // the Pixel 10 Pro, Test Lab 2026-11-18) runs OpenGL ES under auto, whatever its capabilities say; an explicit
    // request still takes Vulkan.
    VulkanDevice rogue = device(1, 1);
    rogue.name = "PowerVR Rogue GE8320";
    rogue.vendor_id = 0x1010;
    rogue.device_id = 0x22104018;
    rogue.driver_version = 0x582558; // 5776728
    const shim::BackendDecision rogue_auto = DecideBackend(render::Backend::Auto, rogue, {});
    ok &= expect(!rogue_auto.vulkan && rogue_auto.reason.find("denylist") != std::string::npos &&
                     rogue_auto.reason.find("5776728") != std::string::npos,
                 "auto: OpenGL ES on the A12's PowerVR Rogue driver, with the evidence in the reason");
    VulkanDevice dseries = device(1, 4);
    dseries.name = "PowerVR D-Series DXT-48-1536 MC1";
    dseries.vendor_id = 0x1010;
    dseries.device_id = 0x46050000;
    dseries.driver_version = 0x67ab5a; // 6794074
    ok &= expect(!DecideBackend(render::Backend::Auto, dseries, {}).vulkan,
                 "auto: OpenGL ES on the Pixel 10 Pro's PowerVR D-Series driver");
    ok &= expect(DecideBackend(render::Backend::Vulkan, rogue, {}).vulkan && DecideBackend(render::Backend::Vulkan, dseries, {}).vulkan,
                 "vulkan asked: Vulkan on a denylisted driver too (the player's or developer's explicit choice)");
    VulkanDevice other_vendor = good;
    other_vendor.driver_version = 0x582558;
    ok &= expect(DecideBackend(render::Backend::Auto, other_vendor, {}).vulkan,
                 "auto: the same driver number of another vendor is not denied");

    // The cache root holds the app's previous-exit.txt; the guard's files are in its RenderBackend directory.
    const std::filesystem::path cache_root =
        std::filesystem::temp_directory_path() / ("storm-backend-select-check-" + std::to_string(getpid()));
    const std::filesystem::path directory = cache_root / "RenderBackend";
    const auto previous_exit = [&](const char *line) {
        std::filesystem::create_directories(cache_root);
        std::ofstream(cache_root / "previous-exit.txt", std::ios::trunc) << line << "\n";
    };
    std::filesystem::remove_all(cache_root);
    // The system killed the process for memory, or stopped it (the lock screen's cleaner), while the start ran: logged,
    // not a crash. A crash, an ANR, a signal or an unknown reason still are.
    for (const char *reason : {"low_memory", "other", "user_requested", "user_stopped", "exit_self", "package_updated"})
    {
        previous_exit((std::string(reason) + " 1763300000000 LockScreenClean").c_str());
        VulkanStartGuard(directory, good).begin();
        ok &= expect(VulkanStartGuard(directory, good).crash().empty(),
                     std::string("a marker left by a process the system ended (") + reason + ") is no crash");
        ok &= expect(VulkanStartGuard(directory, good).crash().empty(), "... and nothing is recorded");
    }
    for (const char *reason : {"crash", "crash_native", "anr", "signaled", "unknown"})
    {
        previous_exit((std::string(reason) + " 1763300000000 description").c_str());
        VulkanStartGuard(directory, good).begin();
        ok &= expect(VulkanStartGuard(directory, good).crash() == crashed,
                     std::string("a marker left by a process that ended with ") + reason + " is a crash");
        std::filesystem::remove(directory / "vulkan-crashed.txt");
    }
    std::filesystem::remove_all(cache_root);
    {
        VulkanStartGuard first(directory, good);
        ok &= expect(first.crash().empty(), "no marker: no crash");
        first.begin(); // the process "dies" here: nothing confirms the start
    }
    {
        const VulkanStartGuard next(directory, good);
        ok &= expect(next.crash() == crashed, "a start that never confirmed is a crash at the next start");
        ok &= expect(!DecideBackend(render::Backend::Auto, good, next.crash()).vulkan, "... and auto runs OpenGL ES");
    }
    ok &= expect(VulkanStartGuard(directory, good).crash() == crashed, "... at every start after it, for that driver");
    VulkanDevice updated = good;
    updated.driver_version = 8;
    ok &= expect(VulkanStartGuard(directory, updated).crash().empty(), "another driver version is tried again");
    ok &= expect(VulkanStartGuard(directory, good).crash().empty(), "... and its record is gone");
    {
        const VulkanStartGuard confirmed(directory, good);
        confirmed.begin();
        confirmed.succeed();
    }
    ok &= expect(VulkanStartGuard(directory, good).crash().empty(), "a confirmed start leaves no crash");
    VulkanStartGuard(directory, good).fail("the backend self-test read 0,0,0");
    const std::string failed = VulkanStartGuard(directory, good).crash();
    ok &= expect(failed.find("self-test") != std::string::npos && !DecideBackend(render::Backend::Auto, good, failed).vulkan,
                 "a failed self-test is recorded with its reason and keeps auto on OpenGL ES");
    std::filesystem::remove_all(directory);
    {
        // A renderer FATAL after a confirmed start (Context::vulkan_failed with the start's guard): recorded too.
        const VulkanStartGuard started(directory, good);
        started.begin();
        started.succeed();
        started.fail("bgfx FATAL: Failed to create graphics PSO for vs_ff_lit + fs_ff_lit_h");
    }
    const std::string fatal = VulkanStartGuard(directory, good).crash();
    ok &= expect(fatal.find("bgfx FATAL") != std::string::npos && !DecideBackend(render::Backend::Auto, good, fatal).vulkan,
                 "a renderer FATAL after a confirmed start is recorded and keeps auto on OpenGL ES");
    std::filesystem::remove_all(directory);
    ok &= expect(VulkanStartGuard(directory, good).crash().empty(), "a cleared cache forgets every record");
    ok &= expect(VulkanStartGuard({}, good).crash().empty(), "no cache directory: no guard");
    std::filesystem::remove_all(directory);

    // A bgfx FATAL (vkCreateGraphicsPipelines VK_ERROR_OUT_OF_HOST_MEMORY on the Galaxy A12) is an error line, then
    // the abort bgfx's contract asks for; a debug check is a warning and the process goes on.
    int status = 0;
    const std::string fatal_log = in_child(
        [] {
            shim::ProgramCache::instance().fatal("renderer_vk.cpp", 5828, bgfx::Fatal::InvalidShader,
                                                 "Failed to create graphics PSO for vs_ff_lit + fs_ff_lit_h! "
                                                 "vkCreateGraphicsPipelines failed -1: VK_ERROR_OUT_OF_HOST_MEMORY.");
        },
        &status);
    ok &= expect(fatal_log.rfind("E ERROR: bgfx FATAL (renderer_vk.cpp:5828): Failed to create graphics PSO", 0) == 0,
                 "a bgfx FATAL is logged at error level (error.log), with bgfx's message: " + fatal_log.substr(0, 80));
    ok &= expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "... and the process aborts after it");
    const std::string check_log = in_child(
        [] { shim::ProgramCache::instance().fatal("bgfx.cpp", 1, bgfx::Fatal::DebugCheck, "a debug check"); }, &status);
    ok &= expect(check_log.rfind("W WARNING: bgfx debug check", 0) == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                 "a bgfx debug check is a warning and the process goes on");
    std::printf("%s: backend_select_check\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
