// What the game thread and bgfx's render thread are doing right now, for the stall watchdog (diagnostics
// stall_watchdog.hpp): a diagnostic, never a recovery. Plain relaxed atomics written by the two threads and read by
// the watchdog thread; nothing here waits or allocates.
//
//   * The game thread registers at the start of the main loop (RegisterGameThread: its tid and stack for the
//     backtrace) and from then on publishes its frame_work phase, synchronous load, entity stage (frame_work.hpp
//     Enter/EnterSync/EntityScope) and the outermost script event (core compiler ProcessEvent). Progress() is each
//     main loop iteration (also one that runs no frame: an inactive window, a paced load) and each presented frame
//     (frame_work EndFrame: also the loading screen's presents inside one long frame).
//   * The render thread (shim context.cpp) registers at its first finished frame; the game thread marks each frame
//     it hands over (Handed), the render thread each frame it finished (Done) and its present (PresentScope:
//     Swappy / vkQueuePresentKHR / eglSwapBuffers). Busy = a handed frame not done yet.
//   * The app's lifecycle (Background): the game thread is paused by SDL in the background, which is no stall.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <typeinfo>

#if defined(__linux__)
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace storm::platform::stall_watch
{

inline int64_t Now()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The thread's kernel id (0 where the watchdog cannot signal threads).
inline int ThreadId()
{
#if defined(__linux__)
    return static_cast<int>(syscall(SYS_gettid));
#else
    return 0;
#endif
}

struct Thread
{
    std::atomic<int> tid{0};
    std::atomic<uintptr_t> stack_low{0}, stack_high{0}; // its stack, for the frame-pointer walk's bounds

    void Register()
    {
#if defined(__linux__)
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0)
        {
            void *base = nullptr;
            size_t size = 0;
            if (pthread_attr_getstack(&attr, &base, &size) == 0)
            {
                stack_low.store(reinterpret_cast<uintptr_t>(base), std::memory_order_relaxed);
                stack_high.store(reinterpret_cast<uintptr_t>(base) + size, std::memory_order_relaxed);
            }
            pthread_attr_destroy(&attr);
        }
#endif
        tid.store(ThreadId(), std::memory_order_release);
    }
};

inline constexpr size_t kEventChars = 64;

struct Game : Thread
{
    std::atomic<int64_t> progress{0}; // ns (Now) of the last progress: a main loop iteration, a presented frame
    std::atomic<uint8_t> phase{0}; // frame_work::Phase
    std::atomic<int64_t> phase_since{0};
    std::atomic<uint8_t> sync{0}; // frame_work::Sync, 0: none
    std::atomic<int64_t> sync_since{0};
    std::atomic<const std::type_info *> entity{nullptr}; // the entity class whose Execute/Realize runs
    std::atomic<bool> entity_realize{false};
    std::atomic<int64_t> entity_since{0};
    std::array<std::atomic<char>, kEventChars> event{}; // the outermost script event running, "" outside
    std::atomic<int64_t> event_since{0};
};

struct Render : Thread
{
    std::atomic<uint32_t> handed{0}; // the last frame bgfx::frame() handed over (game thread)
    std::array<std::atomic<int64_t>, 4> handed_at{}; // ns, by frame number & 3
    std::atomic<uint32_t> done{0}; // the last frame the render thread finished (rendered and presented)
    std::atomic<int64_t> done_at{0};
    std::atomic<bool> any_handed{false};
    std::atomic<int64_t> present_since{0}; // in the present since (0: not presenting)
    std::atomic<const char *> present_call{nullptr}; // a string literal: what presents
};

inline Game &game()
{
    static Game value;
    return value;
}
inline Render &render()
{
    static Render value;
    return value;
}
inline std::atomic<bool> background{false};
inline std::atomic<int64_t> resumed{0}; // ns of the last return to the foreground

// The thread publishing its phases: the registered game thread only.
inline thread_local bool tPublishing = false;

inline void RegisterGameThread()
{
    Game &g = game();
    const int64_t now = Now();
    g.progress.store(now, std::memory_order_relaxed);
    g.phase_since.store(now, std::memory_order_relaxed);
    g.Register();
    tPublishing = true;
}

inline bool Publishing()
{
    return tPublishing;
}

// The game thread is back in its main loop or presented a frame (a loading screen's presents inside one long frame).
inline void Progress()
{
    if (!tPublishing)
        return;
    Game &g = game();
    g.progress.store(Now(), std::memory_order_relaxed);
}

inline void PublishPhase(uint8_t phase, int64_t now)
{
    Game &g = game();
    g.phase.store(phase, std::memory_order_relaxed);
    g.phase_since.store(now, std::memory_order_relaxed);
}

inline void PublishSync(uint8_t sync, int64_t now)
{
    Game &g = game();
    g.sync.store(sync, std::memory_order_relaxed);
    g.sync_since.store(now, std::memory_order_relaxed);
}

// Returns the published entity to restore when its stage ends (entity stages nest: an Execute creating an entity).
struct EntityState
{
    const std::type_info *type;
    bool realize;
    int64_t since;
};
inline EntityState PublishEntity(const std::type_info *type, bool realize, int64_t now)
{
    Game &g = game();
    EntityState previous{g.entity.load(std::memory_order_relaxed), g.entity_realize.load(std::memory_order_relaxed),
                         g.entity_since.load(std::memory_order_relaxed)};
    g.entity.store(type, std::memory_order_relaxed);
    g.entity_realize.store(realize, std::memory_order_relaxed);
    g.entity_since.store(now, std::memory_order_relaxed);
    return previous;
}
inline void RestoreEntity(const EntityState &state)
{
    Game &g = game();
    g.entity.store(state.type, std::memory_order_relaxed);
    g.entity_realize.store(state.realize, std::memory_order_relaxed);
    g.entity_since.store(state.since, std::memory_order_relaxed);
}

// The outermost script event (nullptr: it ended). Game thread.
inline void PublishEvent(const char *name)
{
    if (!tPublishing)
        return;
    Game &g = game();
    size_t i = 0;
    for (; name && name[i] && i + 1 < kEventChars; ++i)
        g.event[i].store(name[i], std::memory_order_relaxed);
    g.event[i].store('\0', std::memory_order_relaxed);
    g.event_since.store(Now(), std::memory_order_relaxed);
}
inline std::string EventName()
{
    std::string name;
    for (const auto &c : game().event)
    {
        const char value = c.load(std::memory_order_relaxed);
        if (!value)
            break;
        name += value;
    }
    return name;
}

// Render thread hooks.
inline void Handed(uint32_t frame)
{
    Render &r = render();
    r.handed_at[frame & 3].store(Now(), std::memory_order_relaxed);
    r.handed.store(frame, std::memory_order_relaxed);
    r.any_handed.store(true, std::memory_order_release);
}
inline void Done(uint32_t frame)
{
    Render &r = render();
    // A re-created device runs a new render thread.
    thread_local const int self = ThreadId();
    if (r.tid.load(std::memory_order_relaxed) != self)
        r.Register();
    r.done_at.store(Now(), std::memory_order_relaxed);
    r.done.store(frame, std::memory_order_relaxed);
}

class PresentScope
{
  public:
    explicit PresentScope(const char *call)
    {
        render().present_call.store(call, std::memory_order_relaxed);
        render().present_since.store(Now(), std::memory_order_relaxed);
    }
    ~PresentScope()
    {
        render().present_since.store(0, std::memory_order_relaxed);
    }
    PresentScope(const PresentScope &) = delete;
    PresentScope &operator=(const PresentScope &) = delete;
};

// The app's lifecycle (any thread).
inline void Background(bool in_background)
{
    if (!in_background)
        resumed.store(Now(), std::memory_order_relaxed);
    background.store(in_background, std::memory_order_relaxed);
}

} // namespace storm::platform::stall_watch
