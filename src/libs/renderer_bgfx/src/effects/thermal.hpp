// When the background work must yield: the relief generation and the ray bakes (the place's probes, the town light:
// fx::SetBakeGate). Two things hold it: an explicit renderer hold (the automatic graphics preset measures the GPU:
// renderer auto_preset.hpp), and the device's heat while the game is played (thermal_api.hpp ThermalLoad): at
// thermal status SEVERE it waits, at MODERATE or a forecast near SEVERE it takes one thread (effects.cpp sets
// fx::CpuAdmission's gameplay limit every frame). A place's loading is never paused or slowed by heat.
#pragma once

#include "effects/thermal_api.hpp"

namespace storm::effects
{
// Why the background work waits for the device to cool (background_wait_reason; scene_readiness.hpp knows it by
// its address: a reveal does not wait for work that waits to cool).
inline constexpr const char *kThermalWaitReason =
    "thermal protection (status SEVERE while the game is played; waiting to cool, a place's loading goes on)";

// The device's heat (thermal_api.hpp; AThermal, Android 11+ / API 30, headroom API 31, through dlsym: the app's
// minimum API is 24; status at most once a second, forecast once per ten seconds with at most twenty seconds of
// valid-sample retention). Older Android and the host have no thermal status: Normal (said once in the log); a
// symbol missing on API 30+ is logged as an error. Every change is logged with what it asks of the background work.
ThermalLoad thermal_load();
// Elevated or Severe.
bool device_hot();
// "status/headroom" of the last reading for the profile lines ("severe/1.03"); "n/a" without a thermal status.
const char *thermal_text();

// Main thread: the renderer holds the background work (`why` says why; null releases it). Logged on change.
void hold_background_work(const char *why);
// Main thread, every frame: whether the game is played (no place is loading). Heat holds the work only then.
void background_play(bool playing);
// Why the background work must wait now (the explicit hold, or heat while the game is played); null: it may run.
// Any thread.
const char *background_wait_reason();

} // namespace storm::effects
