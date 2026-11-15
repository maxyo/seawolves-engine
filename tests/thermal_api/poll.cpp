#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

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
    using storm::effects::device_hot;
    check(device_hot(), "valid hot forecast pauses work");
    for (int second = 1; second < 10; ++second)
    {
        now_ms += 1000;
        check(device_hot(), "polling must not erase a valid hot forecast with rate-limit NaN");
        check(forecast_calls == 1, "headroom queried no more than once per ten seconds");
        check(status_calls == second + 1, "status still polled every second");
    }
    forecast = std::nanf(""); now_ms += 1000;
    check(device_hot() && forecast_calls == 2, "one failed refresh retains the recent valid forecast");
    now_ms += 9000;
    check(device_hot(), "cached forecast remains valid before its bounded expiry");
    now_ms += 1000;
    check(!device_hot(), "repeated unknown forecast cannot hold work indefinitely");
    status = 2; now_ms += 1000;
    check(device_hot(), "moderate status takes effect between forecast polls");
    status = 0; now_ms += 1000;
    check(!device_hot(), "expired unknown forecast falls back to current status");
    forecast = .95f; now_ms += 8000;
    check(device_hot(), "fresh forecast recovers after unavailable samples");
    forecast = .7f; now_ms += 10000;
    check(!device_hot(), "fresh cool forecast resumes without changing thresholds");
    std::puts("thermal production polling: ok");
}
