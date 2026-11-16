#include "thermal_api.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

using namespace storm::effects;

static void *Acquire() { return nullptr; }
static int Status(void *) { return 0; }
static float Headroom(void *, int) { return 0.f; }

static void check(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

// A lookup that knows exactly the NDK's real names, as libandroid.so exports them (the names a wrong one cannot match).
static void *Real(const char *name)
{
    if (!std::strcmp(name, "AThermal_acquireManager"))
        return reinterpret_cast<void *>(&Acquire);
    if (!std::strcmp(name, "AThermal_getCurrentThermalStatus"))
        return reinterpret_cast<void *>(&Status);
    if (!std::strcmp(name, "AThermal_getThermalHeadroom"))
        return reinterpret_cast<void *>(&Headroom);
    return nullptr;
}

int main()
{
    check(!resolve_thermal_api(29, Real).available && resolve_thermal_api(29, Real).error.empty(), "API 29: none, no error");
    for (int level : {30, 31, 36})
    {
        const auto r = resolve_thermal_api(level, Real);
        check(r.available && r.error.empty(), "real symbols resolve on API >= 30");
        check((r.api.headroom != nullptr) == (level >= 31), "headroom resolves from API 31");
    }
    // The bug: a wrong status name left the API unresolved on Android 16 and only an info line said so.
    const auto wrong = resolve_thermal_api(36, [](const char *name) {
        return std::strcmp(name, "AThermal_getCurrentThermalStatus") ? Real(name) : nullptr;
    });
    check(!wrong.available, "a lookup without AThermal_getCurrentThermalStatus fails");
    check(wrong.error.find("AThermal_getCurrentThermalStatus") != std::string::npos, "the error names the missing symbol");
    std::set<std::string> asked;
    resolve_thermal_api(36, [&](const char *name) { asked.insert(name); return Real(name); });
    check(asked == std::set<std::string>{"AThermal_acquireManager", "AThermal_getCurrentThermalStatus",
                                         "AThermal_getThermalHeadroom"}, "the exact NDK symbol names are asked");
    const auto no_headroom = resolve_thermal_api(31, [](const char *n) {
        return std::strcmp(n, "AThermal_getThermalHeadroom") ? Real(n) : nullptr;
    });
    check(no_headroom.available && !no_headroom.error.empty(), "a missing headroom is an error but the status works");

    const float nan = std::nanf("");
    check(!thermal_hot(false, 0, 0.5f), "cool");
    check(thermal_hot(false, 2, 0.f), "moderate is elevated");
    check(thermal_hot(false, 0, 0.95f), "headroom flags elevated load early");
    check(thermal_hot(true, 0, 0.8f), "hysteresis retains elevated load");
    check(!thermal_hot(true, 0, 0.7f), "clears below the lower threshold");
    check(!thermal_hot(false, 1, nan), "no forecast: the status decides");
    // What heat asks of the background work while the game is played.
    check(thermal_load(false, 1) == ThermalLoad::Normal, "light status, no forecast: normal");
    check(thermal_load(true, 1) == ThermalLoad::Elevated, "a hot forecast under MODERATE: elevated");
    check(thermal_load(true, 2) == ThermalLoad::Elevated, "moderate: elevated");
    check(thermal_load(true, 3) == ThermalLoad::Severe && thermal_load(true, 5) == ThermalLoad::Severe, "severe and above");
    check(background_threads(ThermalLoad::Normal, 2) == 2, "cool: the normal share");
    check(background_threads(ThermalLoad::Elevated, 2) == 1, "hot: one thread");
    check(background_threads(ThermalLoad::Severe, 2) == 0, "severe: none");
    std::puts("thermal_api: ok");
}
