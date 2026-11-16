#include "effects/thermal.hpp"

#include "effects/thermal_api.hpp"
#include "shim/shim.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef __ANDROID__
#include <android/api-level.h>
#include <dlfcn.h>
#endif

namespace storm::effects
{

namespace
{
#ifdef __ANDROID__
const char *status_name(int status)
{
    static const char *const names[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
    return status >= 0 && status < 7 ? names[status] : "unknown";
}

struct Thermal
{
    ThermalApi api;
    void *manager = nullptr;
    Thermal()
    {
        void *library = dlopen("libandroid.so", RTLD_NOW);
        const int level = android_get_device_api_level();
        const auto resolution = resolve_thermal_api(level, [library](const char *name) {
            return library ? dlsym(library, name) : nullptr;
        });
        if (!resolution.error.empty() || (level >= kThermalStatusApi && !library))
        {
            shim::logf("ERROR: effects: thermal status unavailable: %s", resolution.error.empty() ? "libandroid.so did not load"
                                                                                                  : resolution.error.c_str());
            return;
        }
        if (!resolution.available)
        {
            shim::logf("effects: no thermal telemetry on Android API %d (AThermal needs API 30)", level);
            return;
        }
        api = resolution.api;
        manager = api.acquire();
        if (!manager)
            shim::logf("ERROR: effects: AThermal_acquireManager returned null on Android API %d: no thermal telemetry", level);
        else
            shim::logf("effects: thermal status is read (API %d%s)", level, api.headroom ? ", with headroom" : "");
    }
};
#endif
} // namespace

namespace
{
std::atomic<const char *> g_hold{nullptr};
std::atomic<bool> g_playing{false};
#ifdef __ANDROID__
std::mutex g_text_mutex;
char g_text[32] = "n/a"; // under g_text_mutex
#endif
} // namespace

ThermalLoad thermal_load()
{
#ifdef __ANDROID__
    static Thermal thermal; // thread-safe initialisation
    static std::mutex sample_mutex;
    static int64_t asked = 0, forecast_asked = 0, forecast_valid = 0;
    static bool has_forecast_query = false, has_valid_forecast = false;
    static float cached_headroom = std::nanf("");
    static std::atomic<bool> hot{false};
    static std::atomic<int> last_status{-1};
    static std::atomic<ThermalLoad> load{ThermalLoad::Normal};
    const int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    // Do not overlap platform calls from the renderer and background workers.
    // A busy caller uses the last decision; it never stalls drawing on a sensor.
    std::unique_lock lock(sample_mutex, std::try_to_lock);
    if (lock && thermal.manager && now - asked >= 1000)
    {
        asked = now;
        const int status = thermal.api.status(thermal.manager);
        if (thermal.api.headroom && (!has_forecast_query || now - forecast_asked >= kHeadroomPollMilliseconds))
        {
            has_forecast_query = true;
            forecast_asked = now;
            const float sample = thermal.api.headroom(thermal.manager, kHeadroomForecastSeconds);
            if (std::isfinite(sample) && sample >= 0.0f)
            {
                cached_headroom = sample;
                forecast_valid = now;
                has_valid_forecast = true;
            }
        }
        const float headroom = has_valid_forecast && now - forecast_valid < kHeadroomValidMilliseconds
                                   ? cached_headroom : std::nanf("");
        const bool was_hot = hot.load();
        const bool now_hot = thermal_hot(was_hot, status, headroom);
        const ThermalLoad now_load = effects::thermal_load(now_hot, status);
        // Forecast-driven changes are logged even while the reported status stays LIGHT.
        if (status != last_status.exchange(status) || now_hot != was_hot)
            shim::logf("effects: thermal status: %s, headroom %.2f, load %s: %s", status_name(status),
                       static_cast<double>(headroom), thermal_load_name(now_load), background_policy(now_load));
        hot = now_hot;
        load = now_load;
        std::lock_guard text(g_text_mutex);
        snprintf(g_text, sizeof(g_text), "%s/%.2f", status_name(status), static_cast<double>(headroom));
    }
    return load.load();
#else
    return ThermalLoad::Normal;
#endif
}

bool device_hot()
{
    return thermal_load() != ThermalLoad::Normal;
}

const char *thermal_text()
{
#ifdef __ANDROID__
    static thread_local char text[32];
    std::lock_guard lock(g_text_mutex);
    std::memcpy(text, g_text, sizeof(text));
    return text;
#else
    return "n/a";
#endif
}

void hold_background_work(const char *why)
{
    const char *was = g_hold.exchange(why);
    if (why && !was)
        shim::logf("effects: background work (relief generation, ray bakes) held: %s", why);
    else if (!why && was)
        shim::logf("effects: background work (relief generation, ray bakes) released (was held: %s)", was);
}

void background_play(bool playing)
{
    g_playing = playing;
}

const char *background_wait_reason()
{
    if (const char *held = g_hold.load())
        return held;
    // The sensor is read whether or not the game is played: its changes are logged.
    return thermal_load() == ThermalLoad::Severe && g_playing.load() ? kThermalWaitReason : nullptr;
}

} // namespace storm::effects
