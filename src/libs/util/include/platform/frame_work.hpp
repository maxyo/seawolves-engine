// Where the game thread's frame goes: always on, a clock read per phase change and two per entity stage.
//
// The frame interval the renderer reports (bgfx cpuTimeFrame, the effects profile's cpu_frame_ms) contains the wait
// for the render thread and for the display's slot, so it says nothing about the game thread's work. Here every
// frame is split into exclusive phases (Scope: a phase entered inside another takes its time out of the outer one,
// so script events raised from an entity's Execute count as script), and the Execute and Realize stages also per
// entity class (EntityScope, inclusive). EndFrame closes the frame into the windows of its readers:
//
//   * Summary: the "frame work:" line every 30 s next to "frame pacing:", in every session.
//
// A window keeps the means per phase, the per-frame work for percentiles, the slowest frame with its own split and
// its slowest entity (what a stutter was), and the classes that took the most. Work is everything but the two waits.
// The scopes are per thread; only the thread that calls EndFrame (the game thread) is reported. The game thread also
// publishes its running phase, load and entity for the stall watchdog (stall_watch.hpp).
#pragma once

#include "platform/stall_watch.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

namespace storm::platform::frame_work
{

enum class Phase : uint8_t
{
    Other,       // the main loop outside the phases below: platform events, input
    Hud,         // Android: the UI host's input and the HUD adapters
    Script,      // the script VM: its frame, its events (also those entities raise)
    Load,        // save/load state
    Services,    // services' RunStart
    Execute,     // entities' Execute: game logic, physics
    Realize,     // entities' Realize: draw submission
    Controls,
    Present,     // services' RunEnd: the renderer's end of frame (post chain, composite)
    Ui,          // the UI clients' render
    WaitRender,  // bgfx::frame(): the render thread still draws the previous frame (GPU or driver bound), hand-over
    WaitPresent, // the frame gate: the display's slot of the paced frame
    Count,
};

inline constexpr size_t kPhases = static_cast<size_t>(Phase::Count);
inline constexpr const char *kPhaseNames[kPhases] = {"other",   "hud",      "script",  "load", "services",    "execute",
                                                     "realize", "controls", "present", "ui",   "wait_render", "wait_present"};

inline bool IsWork(Phase phase)
{
    return phase != Phase::WaitRender && phase != Phase::WaitPresent;
}

// Synchronous work that makes a frame long wherever it is called from (a script's CreateEntity, an entity's first
// Realize): counted beside the phases, exclusive among themselves (a model's textures count as texture loads), so a
// slow frame says whether it was the game's script or a load inside it.
enum class Sync : uint8_t
{
    None,
    TextureLoad,   // a texture file read and prepared on the game thread (Renderer::TextureLoad)
    ModelLoad,     // a model file (GEOMETRY::CreateGeometry), without its textures
    AnimationLoad, // an animation file (AnimationServiceImp::LoadAnimation)
    ReadBack,      // a render target read back: the queued frame is executed and waited for
    Count,
};
inline constexpr size_t kSyncs = static_cast<size_t>(Sync::Count);
inline constexpr const char *kSyncNames[kSyncs] = {"none", "texture_load", "model_load", "animation_load", "read_back"};

enum class Reader : uint8_t
{
    Profile,
    Summary,
    Count,
};

using Clock = std::chrono::steady_clock;
using ClassTimes = std::unordered_map<const std::type_info *, int64_t>;

struct Window
{
    uint32_t frames = 0;
    std::array<int64_t, kPhases> sum{}; // ns
    std::vector<float> work_ms;         // per frame, for percentiles
    int64_t worst_work = 0;             // ns: the frame with the most work
    std::array<int64_t, kPhases> worst{};
    const std::type_info *worst_entity = nullptr; // that frame's slowest entity class, its stage and time
    bool worst_entity_realize = false;
    int64_t worst_entity_ns = 0;
    ClassTimes execute, realize; // ns per entity class over the window
    std::array<int64_t, kSyncs> sync{}, worst_sync{};          // ns: over the window, of the slowest frame
    std::array<uint32_t, kSyncs> sync_count{}, worst_sync_count{};
};

struct State
{
    Clock::time_point mark = Clock::now();
    Phase current = Phase::Other;
    std::array<int64_t, kPhases> frame{};
    int64_t last_work = 0;       // ns: the work of the last ended frame
    Sync sync = Sync::None;      // the synchronous work running now
    Clock::time_point sync_mark{};
    std::array<int64_t, kSyncs> sync_ns{}; // this frame
    std::array<uint32_t, kSyncs> sync_count{};
    ClassTimes execute, realize; // this frame
    std::array<Window, static_cast<size_t>(Reader::Count)> windows;
};

inline State &state()
{
    thread_local State value;
    return value;
}

// Charges the time since the last change to the phase that ran, and makes `phase` the running one.
inline Phase Enter(Phase phase)
{
    State &s = state();
    const Clock::time_point now = Clock::now();
    s.frame[static_cast<size_t>(s.current)] += std::chrono::duration_cast<std::chrono::nanoseconds>(now - s.mark).count();
    s.mark = now;
    if (stall_watch::Publishing())
        stall_watch::PublishPhase(static_cast<uint8_t>(phase), std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    return std::exchange(s.current, phase);
}

class Scope
{
  public:
    explicit Scope(Phase phase) : previous_(Enter(phase))
    {
    }
    ~Scope()
    {
        Enter(previous_);
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

  private:
    Phase previous_;
};

inline Sync EnterSync(Sync sync)
{
    State &s = state();
    const Clock::time_point now = Clock::now();
    if (s.sync != Sync::None)
        s.sync_ns[static_cast<size_t>(s.sync)] += std::chrono::duration_cast<std::chrono::nanoseconds>(now - s.sync_mark).count();
    s.sync_mark = now;
    if (stall_watch::Publishing())
        stall_watch::PublishSync(static_cast<uint8_t>(sync), std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    return std::exchange(s.sync, sync);
}

class SyncScope
{
  public:
    explicit SyncScope(Sync sync) : previous_(EnterSync(sync))
    {
        ++state().sync_count[static_cast<size_t>(sync)];
    }
    ~SyncScope()
    {
        EnterSync(previous_);
    }
    SyncScope(const SyncScope &) = delete;
    SyncScope &operator=(const SyncScope &) = delete;

  private:
    Sync previous_;
};

// One entity's stage, by its class (inclusive: the script events it raises are in it too).
class EntityScope
{
  public:
    EntityScope(const std::type_info &type, bool realize) : type_(&type), realize_(realize), start_(Clock::now())
    {
        if ((published_ = stall_watch::Publishing()))
            previous_ = stall_watch::PublishEntity(type_, realize_, std::chrono::duration_cast<std::chrono::nanoseconds>(start_.time_since_epoch()).count());
    }
    ~EntityScope()
    {
        if (published_)
            stall_watch::RestoreEntity(previous_);
        State &s = state();
        (realize_ ? s.realize : s.execute)[type_] +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count();
    }
    EntityScope(const EntityScope &) = delete;
    EntityScope &operator=(const EntityScope &) = delete;

  private:
    const std::type_info *type_;
    bool realize_;
    Clock::time_point start_;
    bool published_ = false;
    stall_watch::EntityState previous_{};
};

// The presented frame ends here (the game thread, after its wait for the display's slot).
inline void EndFrame()
{
    stall_watch::Progress();
    State &s = state();
    Enter(s.current);
    int64_t work = 0;
    for (size_t i = 0; i < kPhases; ++i)
        if (IsWork(static_cast<Phase>(i)))
            work += s.frame[i];
    s.last_work = work;
    const std::type_info *slowest = nullptr;
    bool slowest_realize = false;
    int64_t slowest_ns = 0;
    for (const bool realize : {false, true})
        for (const auto &[type, ns] : realize ? s.realize : s.execute)
            if (ns > slowest_ns)
            {
                slowest = type;
                slowest_realize = realize;
                slowest_ns = ns;
            }
    for (Window &window : s.windows)
    {
        // A reader that never takes its window (the profile line is off) costs nothing more than this bound.
        if (window.frames >= 4096)
            window = {};
        ++window.frames;
        for (size_t i = 0; i < kPhases; ++i)
            window.sum[i] += s.frame[i];
        window.work_ms.push_back(static_cast<float>(work * 1e-6));
        for (size_t i = 0; i < kSyncs; ++i)
        {
            window.sync[i] += s.sync_ns[i];
            window.sync_count[i] += s.sync_count[i];
        }
        if (work > window.worst_work)
        {
            window.worst_work = work;
            window.worst = s.frame;
            window.worst_sync = s.sync_ns;
            window.worst_sync_count = s.sync_count;
            window.worst_entity = slowest;
            window.worst_entity_realize = slowest_realize;
            window.worst_entity_ns = slowest_ns;
        }
        for (const auto &[type, ns] : s.execute)
            window.execute[type] += ns;
        for (const auto &[type, ns] : s.realize)
            window.realize[type] += ns;
    }
    s.frame = {};
    s.sync_ns = {};
    s.sync_count = {};
    // The classes stay as keys (the same ones run every frame); only their times start again.
    for (auto &[type, ns] : s.execute)
        ns = 0;
    for (auto &[type, ns] : s.realize)
        ns = 0;
}

// The game thread's work of the last ended frame, ms.
inline double LastWorkMs()
{
    return static_cast<double>(state().last_work) * 1e-6;
}

// The reader's window since its last take.
inline Window Take(Reader reader)
{
    return std::exchange(state().windows[static_cast<size_t>(reader)], {});
}

inline std::string ClassName(const std::type_info *type)
{
    if (!type)
        return "none";
    std::string name = type->name();
#if defined(__GNUG__)
    int status = 0;
    if (char *demangled = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status))
    {
        if (status == 0)
            name = demangled;
        std::free(demangled);
    }
#endif
    name.erase(std::remove(name.begin(), name.end(), ' '), name.end()); // one token of a key=value line
    return name;
}

// "Class:ms,Class:ms": the `count` classes with the most time, in ms per frame of the window.
inline std::string TopClasses(const ClassTimes &times, uint32_t frames, size_t count)
{
    std::vector<std::pair<int64_t, const std::type_info *>> order;
    for (const auto &[type, ns] : times)
        if (ns > 0)
            order.emplace_back(ns, type);
    count = std::min(count, order.size());
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(count), order.end(),
                      [](const auto &a, const auto &b) { return a.first > b.first; });
    std::string text;
    for (size_t i = 0; i < count; ++i)
    {
        char item[32];
        snprintf(item, sizeof(item), ":%.2f", static_cast<double>(order[i].first) * 1e-6 / std::max(frames, 1u));
        text += (i ? "," : "") + ClassName(order[i].second) + item;
    }
    return text.empty() ? "none" : text;
}

// The window as key=value pairs, ms per frame (empty without frames):
// game_work_ms game_work_p95_ms <phase>_ms... worst_work_ms worst=<phase:ms,...> worst_entity=<stage:Class:ms>
inline std::string Describe(const Window &window, size_t top = 4)
{
    if (window.frames == 0)
        return {};
    const double frames = window.frames;
    std::vector<float> sorted = window.work_ms;
    std::sort(sorted.begin(), sorted.end());
    double mean = 0.0;
    for (const float ms : sorted)
        mean += ms;
    mean /= frames;
    const float p95 = sorted[(sorted.size() * 95 + 99) / 100 - 1];
    char number[64];
    snprintf(number, sizeof(number), "game_work_ms=%.2f game_work_p95_ms=%.2f", mean, static_cast<double>(p95));
    std::string text = number;
    for (size_t i = 0; i < kPhases; ++i)
    {
        snprintf(number, sizeof(number), " %s_ms=%.2f", kPhaseNames[i], static_cast<double>(window.sum[i]) * 1e-6 / frames);
        text += number;
    }
    snprintf(number, sizeof(number), " worst_work_ms=%.2f worst=", static_cast<double>(window.worst_work) * 1e-6);
    text += number;
    // The slowest frame's own split: its phases of at least half a millisecond, the largest first.
    std::vector<std::pair<int64_t, size_t>> phases;
    for (size_t i = 0; i < kPhases; ++i)
        if (IsWork(static_cast<Phase>(i)) && window.worst[i] >= 500000)
            phases.emplace_back(window.worst[i], i);
    std::sort(phases.begin(), phases.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (size_t i = 0; i < phases.size(); ++i)
    {
        snprintf(number, sizeof(number), "%s%s:%.1f", i ? "," : "", kPhaseNames[phases[i].second],
                 static_cast<double>(phases[i].first) * 1e-6);
        text += number;
    }
    if (phases.empty())
        text += "none";
    snprintf(number, sizeof(number), ":%.1f", static_cast<double>(window.worst_entity_ns) * 1e-6);
    text += std::string(" worst_entity=") + (window.worst_entity ? (window.worst_entity_realize ? "realize:" : "execute:") : "") +
            ClassName(window.worst_entity) + (window.worst_entity ? number : "");
    // The synchronous loads: per frame of the window, and those of the slowest frame with their counts.
    std::string sync, worst_sync;
    for (size_t i = 1; i < kSyncs; ++i)
    {
        if (window.sync_count[i])
        {
            snprintf(number, sizeof(number), "%s%s:%.2f", sync.empty() ? "" : ",", kSyncNames[i],
                     static_cast<double>(window.sync[i]) * 1e-6 / frames);
            sync += number;
        }
        if (window.worst_sync_count[i])
        {
            snprintf(number, sizeof(number), "%s%s:%.1fx%u", worst_sync.empty() ? "" : ",", kSyncNames[i],
                     static_cast<double>(window.worst_sync[i]) * 1e-6, window.worst_sync_count[i]);
            worst_sync += number;
        }
    }
    text += " sync=" + (sync.empty() ? std::string("none") : sync);
    text += " worst_sync=" + (worst_sync.empty() ? std::string("none") : worst_sync);
    text += " execute_top=" + TopClasses(window.execute, window.frames, top);
    text += " realize_top=" + TopClasses(window.realize, window.frames, top);
    return text;
}

} // namespace storm::platform::frame_work
