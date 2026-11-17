#ifdef STORM_SCENE_SWEEP
#include "scene_sweep.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "compiler.h"
#include "core.h"
#include "core_impl.h"
#include "s_import_func.h"
#include "script_libriary.h"
#include "v_s_stack.h"
#include "vma.hpp"

// Protocol with tools/scene_sweep/run.py, all inside $STORM_SCENE_SWEEP_DIR:
//   scenes.txt  one scene per line, read by SceneSweepNext()
//   events.log  "<ms since start> <text>" lines appended by SceneSweepEvent()/SceneSweepCapture()
//   STORM_SCENE_SWEEP_SETTLE_MS  rendering time per scene before its capture (default 300)
//   ack         created by the runner once it has taken the screenshot and read the logs
//   STORM_SCENE_SWEEP_SAVE  the save the sweep boots into instead of a new game (run.py --save), read by SceneSweepSave()
// A loaded save replaces the whole script state: COMPILER::OnLoad loads the segment again (SceneSweepLoads() counts the
// loads, so the script knows it runs in the loaded game); the files and the scene queue stay where they were.
namespace
{

using Clock = std::chrono::steady_clock;

std::filesystem::path directory;
std::ifstream scenes;
std::FILE *events = nullptr;
const Clock::time_point started = Clock::now();
Clock::time_point windowStart = started;
uint32_t windowFrames = 0;
Clock::time_point lastFrame = started;
std::vector<float> windowFrameMs; // the window's frame times
uint32_t lastDrawCalls = 0;
std::function<std::string()> rendererWindow;
// Background products a variant may wait for (wait=<name>) before its capture.
struct Product
{
    const char *name;
    bool storm::scene_sweep::VariantState::*busy;
};
constexpr Product kProducts[] = {{"upscale", &storm::scene_sweep::VariantState::upscale},
                                 {"relief", &storm::scene_sweep::VariantState::relief},
                                 {"bakes", &storm::scene_sweep::VariantState::bakes}};
constexpr size_t kProductCount = std::size(kProducts);

struct Variant
{
    std::string options;                 // graphics options for the renderer
    std::array<bool, kProductCount> waits{}; // wait=<product>
    float hour = -1.0f;                      // hour=<h>: the script sets the game's time of day (-1: as it is)
};
std::vector<Variant> variants; // STORM_SCENE_SWEEP_VARIANTS split at ';'
int currentVariant = -1;
storm::scene_sweep::VariantHook variantHook = nullptr;
storm::scene_sweep::VariantStateHook variantStateHook = nullptr;

// Program preparation may go this long without a new program before the variant fails; the budget grows with the
// measured preparation (kStallPrograms times the mean time per program so far: a slow device or a cold program cache
// under VirtualGL links each program in about a second).
constexpr double kStallFloorMs = 30000.0;
constexpr double kStallPrograms = 10.0;
// A product counts as done after it stayed idle this long (the bakes of a place follow each other: visibility, then
// the bounce from it; the upscale plans twice a second).
constexpr double kIdleMs = 1000.0;
// While the script waits, the state is written to events.log this often (the runner's timeout counts from the last
// event, and the time is in the log).
constexpr double kReportMs = 10000.0;

// The applied variant as the script waits for it.
struct VariantRun
{
    Clock::time_point applied;
    Clock::time_point progress;  // the last program prepared (or applied)
    Clock::time_point reported;  // the last state event
    uint32_t programs = 0;
    bool active = false;
    double prepare_ms = 0.0;     // from applied to active
    bool settled = false;        // everything it waits for is done
    double settle_ms = 0.0;      // from active to settled
    std::array<Clock::time_point, kProductCount> idle_since{};
    std::string problem;         // why it failed (SceneSweepVariantProblem)
} variantRun;

double msSince(Clock::time_point from, Clock::time_point to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// The products still generating: "upscale+bakes", or "none".
std::string busyProducts(const storm::scene_sweep::VariantState &state)
{
    std::string busy;
    for (const Product &product : kProducts)
        if (state.*product.busy)
            busy += (busy.empty() ? "" : "+") + std::string(product.name);
    return busy.empty() ? "none" : busy;
}
storm::scene_sweep::IncompleteFrameHook incompleteFrameHook = nullptr;
uint32_t windowIncompleteMark = 0; // incompleteFrameHook() when the window started
int segmentLoads = 0;              // times scene_sweep.c was loaded: 1 at the boot, +1 after every loaded save

// The window's frame times: " p50=.. p95=.. p99=.. max=.." (milliseconds).
std::string windowFrameStats()
{
    std::vector<float> sorted = windowFrameMs;
    std::sort(sorted.begin(), sorted.end());
    const auto at = [&](double p) {
        return sorted.empty() ? 0.0f : sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * sorted.size()))];
    };
    char text[128];
    std::snprintf(text, sizeof(text), " p50=%.1f p95=%.1f p99=%.1f max=%.1f", at(0.50), at(0.95), at(0.99),
                  sorted.empty() ? 0.0f : sorted.back());
    return text;
}

void startWindow()
{
    windowStart = Clock::now();
    windowFrames = 0;
    windowFrameMs.clear();
}

void writeEvent(const std::string &text)
{
    // The runner attributes log lines to scenes by reading the logs at each event.
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger> &logger) { logger->flush(); });
    std::fflush(stdout);
    std::fflush(stderr);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    std::fprintf(events, "%lld %s\n", static_cast<long long>(ms), text.c_str());
    std::fflush(events);
}

uint32_t next(VS_STACK *stack)
{
    auto *result = static_cast<VDATA *>(stack->Push());
    if (!result)
        return IFUNCRESULT_FAILED;
    std::string scene;
    std::getline(scenes, scene);
    result->Set(scene);
    return IFUNCRESULT_OK;
}

uint32_t event(VS_STACK *stack)
{
    auto *text = static_cast<VDATA *>(stack->Pop());
    if (!text)
        return IFUNCRESULT_FAILED;
    const std::string line = text->GetString();
    // A window not captured (Vulkan draws waited for pipelines): its frame times still count for the scene.
    if (line.rfind("incomplete ", 0) == 0)
        writeEvent("window frames=" + std::to_string(windowFrames) + windowFrameStats());
    writeEvent(line);
    // The script state at the points every run of a session passes: tools/vm_verify --state compares engines by them.
    if (line.rfind("loaded ", 0) == 0 || line.rfind("end ", 0) == 0)
        core_internal.Compiler->WriteStateDump(line.c_str());
    startWindow();
    windowIncompleteMark = incompleteFrameHook ? incompleteFrameHook() : 0;
    if (rendererWindow)
        rendererWindow();
    return IFUNCRESULT_OK;
}

// The capture's fields of the variant: its index, how long its preparation took and the background products still
// generating (the variant did not ask to wait for them).
std::string variantCaptureFields()
{
    char text[160];
    std::snprintf(text, sizeof(text), " variant=%d prepare_ms=%.0f programs=%u settle_ms=%.0f background=%s", currentVariant,
                  variantRun.prepare_ms, variantRun.programs, variantRun.settle_ms,
                  busyProducts(variantStateHook()).c_str());
    return text;
}

// Reports the frames rendered since the previous event and keeps the last frame on
// screen until the runner acknowledges that it has captured it.
uint32_t capture(VS_STACK *)
{
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - windowStart).count();
    writeEvent("capture frames=" + std::to_string(windowFrames) + " frame_ms=" +
               std::to_string(windowFrames ? ms / windowFrames : 0.0) + windowFrameStats() +
               " draws=" + std::to_string(lastDrawCalls) +
               (currentVariant >= 0 ? variantCaptureFields() : std::string()) +
               (rendererWindow ? " renderer: " + rendererWindow() : std::string()));
    const auto ack = directory / "ack";
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    std::error_code error;
    while (!std::filesystem::remove(ack, error))
    {
        if (Clock::now() > deadline)
        {
            spdlog::critical("Scene sweep: the runner did not acknowledge a capture within 60 s");
            spdlog::apply_all([](const std::shared_ptr<spdlog::logger> &logger) { logger->flush(); });
            std::_Exit(EXIT_FAILURE);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    startWindow();
    return IFUNCRESULT_OK;
}

// Milliseconds a scene renders after its fader is gone before the capture (run.py --settle).
uint32_t variantCount(VS_STACK *stack)
{
    auto *result = static_cast<VDATA *>(stack->Push());
    if (!result)
        return IFUNCRESULT_FAILED;
    result->Set(static_cast<int32_t>(variants.size()));
    return IFUNCRESULT_OK;
}

// The time of day the current variant sets (hour=<h>), -1 when it keeps the scene's.
uint32_t variantHour(VS_STACK *stack)
{
    auto *result = static_cast<VDATA *>(stack->Push());
    if (!result)
        return IFUNCRESULT_FAILED;
    result->Set(currentVariant >= 0 ? variants[currentVariant].hour : -1.0f);
    return IFUNCRESULT_OK;
}

uint32_t variant(VS_STACK *stack)
{
    auto *index = static_cast<VDATA *>(stack->Pop());
    if (!index)
        return IFUNCRESULT_FAILED;
    int32_t value = 0;
    index->Get(value);
    if (value < 0 || static_cast<size_t>(value) >= variants.size() || !variantHook)
    {
        spdlog::critical("Scene sweep: variant {} of {} cannot be applied (renderer hook set: {})", value,
                         variants.size(), variantHook != nullptr);
        spdlog::apply_all([](const std::shared_ptr<spdlog::logger> &logger) { logger->flush(); });
        std::_Exit(EXIT_FAILURE);
    }
    currentVariant = value;
    const Clock::time_point now = Clock::now();
    variantRun = {};
    variantRun.applied = variantRun.progress = variantRun.reported = now;
    variantHook(variants[value].options.c_str());
    return IFUNCRESULT_OK;
}

// 1 when no frame since the window started (the last event) was presented with scene draws left out.
uint32_t windowComplete(VS_STACK *stack)
{
    auto *result = static_cast<VDATA *>(stack->Push());
    if (!result)
        return IFUNCRESULT_FAILED;
    result->Set(static_cast<int32_t>(!incompleteFrameHook || incompleteFrameHook() == windowIncompleteMark ? 1 : 0));
    return IFUNCRESULT_OK;
}

int waitTimeoutMs();

// 1: the applied variant draws and everything it waits for is done; 0: not yet; -1: failed (SceneSweepVariantProblem).
// Readiness means the variant's own settings and programs are active. Program preparation fails only when it stops
// progressing (kStallFloorMs, or kStallPrograms times the measured mean per program); the time is logged either way.
int variantReadyNow()
{
    VariantRun &run = variantRun;
    if (!run.problem.empty())
        return -1;
    if (run.settled)
        return 1;
    const storm::scene_sweep::VariantState state = variantStateHook();
    const Clock::time_point now = Clock::now();
    const bool report = msSince(run.reported, now) >= kReportMs;
    if (report)
        run.reported = now;
    if (!run.active)
    {
        if (!state.active)
        {
            if (state.programs != run.programs)
            {
                run.programs = state.programs;
                run.progress = now;
            }
            const double elapsed = msSince(run.applied, now);
            const double budget = std::max(kStallFloorMs, run.programs ? kStallPrograms * elapsed / run.programs : 0.0);
            const double idle = msSince(run.progress, now);
            char text[256];
            if (idle > budget)
            {
                std::snprintf(text, sizeof(text),
                              "graphics variant %d: program preparation stalled: %u programs in %.0f ms, none for the "
                              "last %.0f ms (budget %.0f ms)",
                              currentVariant, run.programs, elapsed, idle, budget);
                run.problem = text;
                spdlog::error("Scene sweep: {}", run.problem);
                return -1;
            }
            if (report)
            {
                std::snprintf(text, sizeof(text), "variant %d preparing ms=%.0f programs=%u", currentVariant, elapsed,
                              run.programs);
                writeEvent(text);
            }
            return 0;
        }
        run.active = true;
        run.prepare_ms = msSince(run.applied, now);
        run.programs = std::max(run.programs, state.programs);
        run.reported = now;
        char text[160];
        std::snprintf(text, sizeof(text), "variant %d active ms=%.0f programs=%u background=%s", currentVariant,
                      run.prepare_ms, run.programs, busyProducts(state).c_str());
        writeEvent(text);
        spdlog::info("Scene sweep: graphics variant {} active after {:.0f} ms ({} programs prepared)", currentVariant,
                     run.prepare_ms, run.programs);
    }
    const Variant &variant = variants[currentVariant];
    std::string waiting;
    for (size_t i = 0; i < kProductCount; ++i)
    {
        if (!variant.waits[i])
            continue;
        if (state.*kProducts[i].busy || run.idle_since[i] == Clock::time_point{})
            run.idle_since[i] = state.*kProducts[i].busy ? Clock::time_point{} : now;
        if (run.idle_since[i] == Clock::time_point{} || msSince(run.idle_since[i], now) < kIdleMs)
            waiting += (waiting.empty() ? "" : "+") + std::string(kProducts[i].name);
    }
    const double settling = msSince(run.applied, now) - run.prepare_ms;
    char text[256];
    if (waiting.empty())
    {
        run.settled = true;
        run.settle_ms = settling;
        if (std::any_of(variant.waits.begin(), variant.waits.end(), [](bool wait) { return wait; }))
        {
            std::snprintf(text, sizeof(text), "variant %d settled ms=%.0f", currentVariant, run.settle_ms);
            writeEvent(text);
        }
        return 1;
    }
    if (settling > waitTimeoutMs())
    {
        std::snprintf(text, sizeof(text), "graphics variant %d: %s still generating %.0f ms after the settings became active "
                      "(the variant waits for it)", currentVariant, waiting.c_str(), settling);
        run.problem = text;
        spdlog::error("Scene sweep: {}", run.problem);
        return -1;
    }
    if (report)
    {
        std::snprintf(text, sizeof(text), "variant %d waiting %s ms=%.0f", currentVariant, waiting.c_str(), settling);
        writeEvent(text);
    }
    return 0;
}

uint32_t variantReady(VS_STACK *stack)
{
    auto *result = static_cast<VDATA *>(stack->Push());
    if (!result)
        return IFUNCRESULT_FAILED;
    result->Set(static_cast<int32_t>(variantReadyNow()));
    return IFUNCRESULT_OK;
}

uint32_t variantProblem(VS_STACK *stack)
{
    auto *result = static_cast<VDATA *>(stack->Push());
    if (!result)
        return IFUNCRESULT_FAILED;
    result->Set(variantRun.problem);
    return IFUNCRESULT_OK;
}

// A millisecond setting of the runner (run.py sets every one of them).
int runnerMs(const char *name)
{
    const char *text = std::getenv(name);
    if (!text || !*text)
    {
        spdlog::critical("Scene sweep: {} is not set (run.py sets it)", name);
        return 0;
    }
    return std::atoi(text);
}

// How long a variant may wait for the background products it names after its settings became active.
int waitTimeoutMs()
{
    static const int value = runnerMs("STORM_SCENE_SWEEP_WAIT_TIMEOUT_MS");
    return value;
}

uint32_t pushInt(VS_STACK *stack, int value)
{
    auto *result = static_cast<VDATA *>(stack->Push());
    if (!result)
        return IFUNCRESULT_FAILED;
    result->Set(static_cast<int32_t>(value));
    return IFUNCRESULT_OK;
}

uint32_t settleMs(VS_STACK *stack)
{
    static const int value = runnerMs("STORM_SCENE_SWEEP_SETTLE_MS");
    return pushInt(stack, value);
}

// How long a scene may load before the script fails it with its own reason: below the runner's kill timeout.
uint32_t loadTimeoutMs(VS_STACK *stack)
{
    static const int value = runnerMs("STORM_SCENE_SWEEP_LOAD_TIMEOUT_MS");
    return pushInt(stack, value);
}

void registerFunction(const char *name, const char *returns, uint32_t arguments, SIMPORTFUNC address)
{
    IFUNCINFO info;
    info.pFuncName = name;
    info.pReturnValueName = returns;
    info.nArguments = arguments;
    info.pFuncAddress = address;
    core.SetScriptFunction(&info);
}

// One entry of STORM_SCENE_SWEEP_VARIANTS: its wait=<product> and hour=<h> items are the sweep's, the rest goes to the
// renderer.
Variant parseVariant(const std::string &text)
{
    Variant variant;
    for (size_t start = 0, end; start < text.size(); start = end + 1)
    {
        end = text.find(',', start);
        if (end == std::string::npos)
            end = text.size();
        const std::string item = text.substr(start, end - start);
        if (item.rfind("hour=", 0) == 0)
        {
            char *rest = nullptr;
            variant.hour = std::strtof(item.c_str() + 5, &rest);
            if (rest == item.c_str() + 5 || *rest || variant.hour < 0.0f || variant.hour >= 24.0f)
            {
                spdlog::critical("Scene sweep: variant '{}': hour= wants a time of day 0..24 (e.g. hour=7.5)", text);
                spdlog::apply_all([](const std::shared_ptr<spdlog::logger> &logger) { logger->flush(); });
                std::_Exit(EXIT_FAILURE);
            }
            continue;
        }
        if (item.rfind("wait=", 0) != 0)
        {
            variant.options += (variant.options.empty() ? "" : ",") + item;
            continue;
        }
        const std::string name = item.substr(5);
        const auto product = std::find_if(std::begin(kProducts), std::end(kProducts),
                                          [&](const Product &p) { return name == p.name; });
        if (product == std::end(kProducts))
        {
            spdlog::critical("Scene sweep: variant '{}': unknown wait={} (upscale, relief or bakes)", text, name);
            spdlog::apply_all([](const std::shared_ptr<spdlog::logger> &logger) { logger->flush(); });
            std::_Exit(EXIT_FAILURE);
        }
        variant.waits[static_cast<size_t>(product - std::begin(kProducts))] = true;
    }
    return variant;
}


uint32_t save(VS_STACK *stack)
{
    auto *result = static_cast<VDATA *>(stack->Push());
    if (!result)
        return IFUNCRESULT_FAILED;
    const char *name = std::getenv("STORM_SCENE_SWEEP_SAVE");
    result->Set(std::string(name ? name : ""));
    return IFUNCRESULT_OK;
}

uint32_t loads(VS_STACK *stack)
{
    return pushInt(stack, segmentLoads);
}

// Loaded by `#libriary "SceneSweepLibrary"` in tools/scene_sweep/scene_sweep.c, again after every loaded save (the
// script's functions are registered anew; the files, the scene queue and the variants are opened and read once).
class SceneSweepLibrary : public SCRIPT_LIBRIARY
{
  public:
    bool Init() override
    {
        registerFunction("SceneSweepSave", "string", 0, save);
        registerFunction("SceneSweepLoads", "int", 0, loads);
        registerFunctions();
        if (events)
            return true;
        scenes.open(directory / "scenes.txt");
        events = std::fopen((directory / "events.log").c_str(), "a");
        if (!scenes || !events)
        {
            spdlog::critical("Scene sweep: cannot open scenes.txt or events.log in '{}'", directory.string());
            spdlog::apply_all([](const std::shared_ptr<spdlog::logger> &logger) { logger->flush(); });
            std::_Exit(EXIT_FAILURE);
        }
        if (const char *list = std::getenv("STORM_SCENE_SWEEP_VARIANTS"); list && *list)
        {
            const std::string text = list;
            for (size_t start = 0, end; start <= text.size(); start = end + 1)
            {
                end = text.find(';', start);
                if (end == std::string::npos)
                    end = text.size();
                if (end > start)
                    variants.push_back(parseVariant(text.substr(start, end - start)));
            }
        }
        return true;
    }

  private:
    static void registerFunctions()
    {
        registerFunction("SceneSweepNext", "string", 0, next);
        registerFunction("SceneSweepEvent", "void", 1, event);
        registerFunction("SceneSweepCapture", "void", 0, capture);
        registerFunction("SceneSweepSettleMs", "int", 0, settleMs);
        registerFunction("SceneSweepLoadTimeoutMs", "int", 0, loadTimeoutMs);
        registerFunction("SceneSweepVariants", "int", 0, variantCount);
        registerFunction("SceneSweepVariant", "void", 1, variant);
        registerFunction("SceneSweepVariantHour", "float", 0, variantHour);
        registerFunction("SceneSweepVariantReady", "int", 0, variantReady);
        registerFunction("SceneSweepVariantProblem", "string", 0, variantProblem);
        registerFunction("SceneSweepWindowComplete", "int", 0, windowComplete);
    }
};

CREATE_SCRIPTLIBRIARY(SceneSweepLibrary)

} // namespace

void storm::scene_sweep::framePresented(uint32_t drawCalls)
{
    ++windowFrames;
    lastDrawCalls = drawCalls;
    const Clock::time_point now = Clock::now();
    windowFrameMs.push_back(std::chrono::duration<float, std::milli>(now - lastFrame).count());
    lastFrame = now;
}

void storm::scene_sweep::setVariantHook(VariantHook apply, VariantStateHook state)
{
    variantHook = apply;
    variantStateHook = state;
}

void storm::scene_sweep::setIncompleteFrameHook(IncompleteFrameHook hook)
{
    incompleteFrameHook = hook;
}

void storm::scene_sweep::setRendererWindow(std::function<std::string()> window)
{
    rendererWindow = std::move(window);
}

// With STORM_VM_STATE_DIR set: the script state, as SaveState serializes it (without the script's OnSave, which
// changes it), into <dir>/<number>-<label>.state.
void COMPILER::WriteStateDump(const char *label)
{
    const char *path = std::getenv("STORM_VM_STATE_DIR");
    if (!path || !*path)
        return;
    static int number = 0;
    std::string name = label;
    std::replace_if(name.begin(), name.end(), [](char c) { return c == ' ' || c == '/' || c == ':'; }, '_');
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "%03d-", number++);

    delete[] pBuffer;
    pBuffer = nullptr;
    dwCurPointer = 0;
    dwMaxSize = 0;
    SerializeState();
    std::ofstream file(std::filesystem::path(path) / (prefix + name + ".state"), std::ios::binary);
    file.write(pBuffer, dwCurPointer);
    if (!file.flush())
    {
        spdlog::critical("VM state: cannot write {}/{}{}.state", path, prefix, name);
        spdlog::apply_all([](const std::shared_ptr<spdlog::logger> &logger) { logger->flush(); });
        std::_Exit(EXIT_FAILURE);
    }
    delete[] pBuffer;
    pBuffer = nullptr;
    dwCurPointer = 0;
    dwMaxSize = 0;
}

void COMPILER::LoadSceneSweep()
{
    const char *path = std::getenv("STORM_SCENE_SWEEP_DIR");
    if (!path || !*path)
        return;
    directory = path;
    constexpr const char *segment = "scene_sweep.c";
    if (BC_SegmentIsLoaded(segment))
        return; // a save made by a sweep holds the segment already
    ++segmentLoads;
    if (!BC_LoadSegment(segment))
    {
        spdlog::critical("Scene sweep: script segment '{}' did not load, see error.log", segment);
        spdlog::apply_all([](const std::shared_ptr<spdlog::logger> &logger) { logger->flush(); });
        std::_Exit(EXIT_FAILURE);
    }
}
#endif
