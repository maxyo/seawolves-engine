// The AThermal NDK API's availability and thermal-load classification, free of Android so a host check can run them: the
// symbol lookup is injected (thermal.cpp passes dlsym on libandroid.so). A symbol missing on an API that has it is an
// error to log loudly, never a quiet "no thermal status" (a wrong symbol name once did that on every phone).
#pragma once

#include <cmath>
#include <string>

namespace storm::effects
{

struct ThermalApi
{
    void *(*acquire)() = nullptr;                 // AThermal_acquireManager, API 30
    int (*status)(void *) = nullptr;              // AThermal_getCurrentThermalStatus, API 30
    float (*headroom)(void *, int) = nullptr;     // AThermal_getThermalHeadroom, API 31
};

struct ThermalResolution
{
    ThermalApi api;
    bool available = false; // the status can be read
    std::string error;      // not empty: the API level has the functions but a symbol did not resolve
};

constexpr int kThermalStatusApi = 30;
constexpr int kThermalHeadroomApi = 31;

// lookup(name) returns the symbol's address or null.
template <class Lookup> ThermalResolution resolve_thermal_api(int api_level, Lookup lookup)
{
    ThermalResolution result;
    if (api_level < kThermalStatusApi)
        return result;
    result.api.acquire = reinterpret_cast<void *(*)()>(lookup("AThermal_acquireManager"));
    result.api.status = reinterpret_cast<int (*)(void *)>(lookup("AThermal_getCurrentThermalStatus"));
    if (!result.api.acquire)
        result.error += " AThermal_acquireManager";
    if (!result.api.status)
        result.error += " AThermal_getCurrentThermalStatus";
    if (api_level >= kThermalHeadroomApi)
    {
        result.api.headroom = reinterpret_cast<float (*)(void *, int)>(lookup("AThermal_getThermalHeadroom"));
        if (!result.api.headroom)
            result.error += " AThermal_getThermalHeadroom";
    }
    if (!result.error.empty())
        result.error = "Android API " + std::to_string(api_level) + " has AThermal but these symbols did not resolve:" +
                       result.error;
    result.available = result.api.acquire && result.api.status;
    return result;
}

// ATHERMAL_STATUS_MODERATE: the first level where the platform asks to cut load.
constexpr int kThermalModerate = 2;
// ATHERMAL_STATUS_SEVERE: the platform throttles hard (Xiaomi 15 Ultra: the GPU about 1.7 times slower)
// and vendors stop the app soon after (MIUI's AutoThermalKillAll came 12 minutes into it).
constexpr int kThermalSevere = 3;
// Headroom (1.0 = severe throttling) forecast kHeadroomForecastSeconds ahead: flag elevated load from kHeadroomPause,
// clear below kHeadroomResume (and a status under MODERATE), so the classification does not flap.
constexpr int kHeadroomForecastSeconds = 10;
constexpr float kHeadroomPause = 0.9f;
constexpr float kHeadroomResume = 0.75f;
// Older Android implementations rate-limit headroom more strictly than status.
// Keep one failed refresh's forecast, but never turn an unavailable sensor into
// an indefinitely hot classification. The current thermal status is still sampled every second.
constexpr int kHeadroomPollMilliseconds = 10000;
constexpr int kHeadroomValidMilliseconds = 20000;

// headroom is NaN when the platform cannot forecast (then the status alone decides).
inline bool thermal_hot(bool was_hot, int status, float headroom)
{
    if (status >= kThermalModerate)
        return true;
    if (std::isnan(headroom))
        return false;
    return was_hot ? headroom >= kHeadroomResume : headroom >= kHeadroomPause;
}

// What the device's heat asks of the background work (relief and x2 textures, ray bakes) while the game is played.
// A place's loading is never slowed or paused by it: nothing is played then, and a load that waited for a phone to
// cool never ended.
enum class ThermalLoad : unsigned char
{
    Normal,   // the background work's normal share of the cores (fx::CpuAdmission::normal_gameplay_threads)
    Elevated, // MODERATE, or the forecast says SEVERE is near: one thread
    Severe,   // SEVERE and above: it waits until the device cools or a place loads
};

inline ThermalLoad thermal_load(bool hot, int status)
{
    return status >= kThermalSevere ? ThermalLoad::Severe : hot ? ThermalLoad::Elevated : ThermalLoad::Normal;
}

// The CPU threads of the background work while the game is played; `normal` is its share on a cool device.
inline unsigned background_threads(ThermalLoad load, unsigned normal)
{
    return load == ThermalLoad::Severe ? 0u : load == ThermalLoad::Elevated ? 1u : normal;
}

inline const char *thermal_load_name(ThermalLoad load)
{
    return load == ThermalLoad::Severe ? "severe" : load == ThermalLoad::Elevated ? "elevated" : "normal";
}

inline const char *background_policy(ThermalLoad load)
{
    return load == ThermalLoad::Severe     ? "while the game is played the background work waits (a place's loading goes on)"
           : load == ThermalLoad::Elevated ? "while the game is played the background work takes one thread"
                                           : "the background work takes its normal share of the cores";
}

} // namespace storm::effects
