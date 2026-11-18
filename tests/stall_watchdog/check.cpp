// tests/stall_watchdog/run.py
#include "stall_watchdog.hpp"

#include "loading_transaction.hpp"
#include "platform/frame_work.hpp"
#include "platform/stall_watch.hpp"

#include <cassert>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using storm::diagnostics::StallWatchdog;
namespace sw = storm::platform::stall_watch;
namespace fw = storm::platform::frame_work;

namespace
{
std::mutex linesMutex;
std::vector<std::pair<StallWatchdog::Level, std::string>> lines;
volatile int sink_guard = 0;

std::vector<std::pair<StallWatchdog::Level, std::string>> Take()
{
    std::lock_guard lock(linesMutex);
    return std::exchange(lines, {});
}

size_t Count(const std::vector<std::pair<StallWatchdog::Level, std::string>> &taken, const std::string &text)
{
    size_t n = 0;
    for (const auto &[level, line] : taken)
        n += line.find(text) != std::string::npos;
    return n;
}

void Dump(const std::vector<std::pair<StallWatchdog::Level, std::string>> &taken)
{
    for (const auto &[level, line] : taken)
        std::printf("  [%s] %s\n", level == StallWatchdog::Level::Error ? "error" : "warning", line.c_str());
}

#define CHECK(condition, taken)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                         \
            Dump(taken);                                                                                               \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

void Frames(int count)
{
    for (int i = 0; i < count; ++i)
    {
        fw::EndFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
} // namespace

struct FakeEntity
{
    virtual ~FakeEntity() = default;
};

// The functions the backtraces must name (exported by -rdynamic, not inlined, no tail call).
extern "C" __attribute__((noinline)) void StallBlockingGameFunction(double seconds)
{
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    sink_guard = sink_guard + 1;
}
extern "C" __attribute__((noinline)) void StallBlockingPresentFunction(double seconds)
{
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    sink_guard = sink_guard + 1;
}

int main()
{
    StallWatchdog::Config config;
    config.stall_seconds = 0.3;
    config.repeat_seconds = 0.4;
    config.poll_seconds = 0.02;
    StallWatchdog watchdog(config, [](StallWatchdog::Level level, const std::string &line) {
        std::lock_guard lock(linesMutex);
        lines.emplace_back(level, line);
    });
    watchdog.Start();

    auto body = []() -> int {
        sw::RegisterGameThread();
        Frames(5);
        auto taken = Take();
        CHECK(taken.empty(), taken);

        // 1. The game thread blocks inside a script event raised from an entity's Execute.
        {
            const fw::Scope execute(fw::Phase::Execute);
            const fw::EntityScope entity(typeid(FakeEntity), false);
            const fw::Scope script(fw::Phase::Script);
            sw::PublishEvent("TestStallEvent");
            StallBlockingGameFunction(1.0);
            sw::PublishEvent(nullptr);
        }
        Frames(3);
        taken = Take();
        CHECK(Count(taken, "the game thread (tid ") >= 2, taken); // the first report and a repeat
        CHECK(Count(taken, "(report 2)") == 1, taken);
        CHECK(Count(taken, "phase script for") >= 2, taken);
        CHECK(Count(taken, "entity execute:FakeEntity") >= 2, taken);
        CHECK(Count(taken, "script event TestStallEvent") >= 2, taken);
        CHECK(Count(taken, "render thread: not watched") >= 2, taken);
        CHECK(Count(taken, "game thread (tid ") >= 2 && Count(taken, "backtrace") >= 2, taken);
        CHECK(Count(taken, "StallBlockingGameFunction") >= 2, taken);
        CHECK(Count(taken, "the game thread moves again after 1.") == 1, taken);
        for (const auto &[level, line] : taken)
            CHECK(level == StallWatchdog::Level::Error, taken);
        std::printf("game thread stall report:\n");
        Dump(taken);

        // 2. The render thread blocks in its present while the game thread keeps finishing frames.
        std::thread render([] {
            sw::Done(0); // its first finished frame registers it
            while (static_cast<int32_t>(sw::render().handed.load() - 1) < 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            {
                const sw::PresentScope present("vkQueuePresentKHR");
                StallBlockingPresentFunction(0.8);
            }
            sw::Done(1);
        });
        while (sw::render().tid.load() == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        sw::Handed(1);
        Frames(110);
        render.join();
        taken = Take();
        CHECK(Count(taken, "the render thread (tid ") >= 1, taken);
        CHECK(Count(taken, "has not finished frame 1 for") >= 1, taken);
        CHECK(Count(taken, "in the present (vkQueuePresentKHR)") >= 1, taken);
        CHECK(Count(taken, "the game thread (tid ") == 0, taken);
        CHECK(Count(taken, "StallBlockingPresentFunction") >= 1, taken);
        CHECK(Count(taken, "the render thread finished frame 1 after 0.") == 1, taken);
        std::printf("render thread stall report:\n");
        Dump(taken);

        // 3. A long frame inside a load is a warning, and says so.
        storm::loading::screen_up = true;
        StallBlockingGameFunction(0.5);
        Frames(3);
        storm::loading::screen_up = false;
        taken = Take();
        CHECK(Count(taken, "inside a load: the loading screen is up") == 1, taken);
        for (const auto &[level, line] : taken)
            CHECK(level == StallWatchdog::Level::Warning, taken);

        // 4. The background ends the stall's reports (SDL pauses the game thread there); the foreground counts again.
        StallBlockingGameFunction(0.4);
        sw::Background(true);
        StallBlockingGameFunction(0.6);
        sw::Background(false);
        StallBlockingGameFunction(0.15);
        Frames(3);
        taken = Take();
        CHECK(Count(taken, "the game thread (tid ") == 1, taken);
        CHECK(Count(taken, "went to the background") == 1, taken);
        CHECK(Count(taken, "moves again") == 0, taken);
        return 0;
    };
    int result = 0;
    std::thread game([&] { result = body(); });
    game.join();
    watchdog.Stop();
    if (result)
        return result;
    std::printf("PASS stall watchdog: game/render stalls reported with phase, entity, event, present and backtraces; "
                "repeats, recovery, load warning, background\n");
    return 0;
}
