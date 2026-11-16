#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <vector>

static std::vector<std::string> logs;

static int64_t now_ms = 100000;
static int status = 0, status_calls = 0, forecast_calls = 0;
static int64_t last_forecast = -100000;
static float forecast = .95f;
static auto TestClockNow()
{
    return std::chrono::steady_clock::time_point(std::chrono::milliseconds(now_ms));
}

// The runner copies the production TU and substitutes only its clock expression.
#define dlopen TestDlopen
#define dlsym TestDlsym
#include "thermal_under_test.cpp"
#undef dlopen
#undef dlsym
#include "effects/scene_readiness.hpp"

namespace storm::shim
{
void logf(const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    logs.emplace_back(text);
}
} // namespace storm::shim

static void *Acquire() { return reinterpret_cast<void *>(1); }
static int Status(void *) { ++status_calls; return status; }
static float Headroom(void *, int)
{
    ++forecast_calls;
    const bool too_soon = now_ms - last_forecast < 10000;
    last_forecast = now_ms;
    return too_soon ? std::nanf("") : forecast;
}
extern "C" void *TestDlopen(const char *, int) noexcept { return reinterpret_cast<void *>(1); }
extern "C" void *TestDlsym(void *, const char *name) noexcept
{
    if (!std::strcmp(name, "AThermal_acquireManager")) return reinterpret_cast<void *>(&Acquire);
    if (!std::strcmp(name, "AThermal_getCurrentThermalStatus")) return reinterpret_cast<void *>(&Status);
    if (!std::strcmp(name, "AThermal_getThermalHeadroom")) return reinterpret_cast<void *>(&Headroom);
    return nullptr;
}
static void check(bool ok, const char *why)
{
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", why); std::exit(1); }
}
int main()
{
    using namespace storm::effects;
    check(device_hot(), "valid hot forecast remains visible as telemetry");
    const char *why = background_wait_reason();
    check(why == nullptr, "hot forecast must not pause background work");
    check(!SceneCooling(true, false, why ? why : ""), "hot telemetry alone does not report a cooling preparation phase");
    hold_background_work("renderer measurement");
    check(std::strcmp(background_wait_reason(), "renderer measurement") == 0, "explicit renderer hold still blocks while hot");
    hold_background_work(nullptr);
    check(background_wait_reason() == nullptr, "releasing explicit hold resumes work despite hot forecast");
    for (int second = 1; second < 10; ++second)
    {
        now_ms += 1000;
        check(device_hot(), "polling must not erase a valid hot forecast with rate-limit NaN");
        check(background_wait_reason() == nullptr, "retained hot forecast remains telemetry only");
        check(forecast_calls == 1, "headroom queried no more than once per ten seconds");
        check(status_calls == second + 1, "status still polled every second");
    }
    forecast = std::nanf(""); now_ms += 1000;
    check(device_hot() && forecast_calls == 2, "one failed refresh retains the recent valid forecast");
    now_ms += 9000;
    check(device_hot(), "cached forecast remains valid before its bounded expiry");
    now_ms += 1000;
    check(!device_hot(), "repeated unknown forecast cannot retain a hot classification indefinitely");
    status = 2; now_ms += 1000;
    check(device_hot(), "moderate status takes effect between forecast polls");
    check(background_wait_reason() == nullptr, "moderate status must not pause background work");
    status = 0; now_ms += 1000;
    check(!device_hot(), "expired unknown forecast falls back to current status");
    forecast = .95f; now_ms += 8000;
    check(device_hot(), "fresh forecast recovers after unavailable samples");
    forecast = .7f; now_ms += 10000;
    check(!device_hot(), "fresh cool forecast clears hot telemetry without changing thresholds");
    for (status = 2; status <= 6; ++status)
    {
        now_ms += 1000;
        check(device_hot(), "elevated platform status remains visible as telemetry");
        check(background_wait_reason() == nullptr, "elevated status must not pause background work");
    }
    status = 0; forecast = 1.2f; now_ms += 10000;
    check(device_hot(), "high forecast remains visible even with a cool status");
    check(background_wait_reason() == nullptr, "high forecast must not pause background work");
    hold_background_work("lifecycle hold");
    check(std::strcmp(background_wait_reason(), "lifecycle hold") == 0, "explicit lifecycle hold still blocks");
    hold_background_work(nullptr);
    check(background_wait_reason() == nullptr, "lifecycle release does not wait for cooling");
    // Heat holds the work only at SEVERE and only while the game is played; a place's loading goes on.
    forecast = .7f; now_ms += 10000;
    check(!device_hot(), "cool again");
    background_play(true);
    status = 2; now_ms += 1000;
    check(thermal_load() == ThermalLoad::Elevated && background_wait_reason() == nullptr, "moderate in play: one thread, no pause");
    status = 3; now_ms += 1000;
    check(thermal_load() == ThermalLoad::Severe, "severe is read");
    check(background_wait_reason() == kThermalWaitReason, "severe while the game is played holds the background work");
    check(SceneCooling(true, false, background_wait_reason()), "a reveal does not wait for work that waits to cool");
    background_play(false);
    check(background_wait_reason() == nullptr, "severe while a place loads does not hold its work");
    background_play(true);
    status = 1; now_ms += 1000;
    check(background_wait_reason() == nullptr, "cooled below severe: the work goes on");
    background_play(false);
    bool said = false;
    for (const auto &line : logs)
        said = said || (line.find("thermal status: severe") != std::string::npos && line.find("background work waits") != std::string::npos);
    check(said, "the severe status is logged with what it asks of the background work");
    for (const auto &line : logs)
    {
        check(line.find("background work paused") == std::string::npos, "telemetry does not falsely log a pause");
        check(line.find("waiting to cool") == std::string::npos, "telemetry does not falsely log cooling");
    }
    std::puts("thermal production polling/gate: hot status/forecast never pause, severe pauses in play only, explicit holds preserved");
}
