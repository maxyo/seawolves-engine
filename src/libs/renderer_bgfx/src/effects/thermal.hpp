// When the background work must yield: the relief generation and the ray bakes (town light, visibility, bounce:
// fx::SetBakeGate). Only an explicit renderer hold stops it (the automatic graphics preset
// measures the GPU: renderer auto_preset.hpp). Thermal sensors provide telemetry, not a pause.
#pragma once

namespace storm::effects
{
// Legacy worker-pause marker understood by scene readiness; the current gate never emits it.
inline constexpr const char *kThermalWaitReason =
    "thermal protection (MODERATE status or forecast headroom; waiting to cool)";

// True from ATHERMAL_STATUS_MODERATE up, the first level where the platform asks to cut load, or when the thermal
// headroom forecast says it is about to (thermal_api.hpp; AThermal, Android 11+ / API 30, headroom API 31, through
// dlsym: the app's minimum API is 24; status at most once a second, forecast once per ten seconds with at most
// twenty seconds of valid-sample retention). Older Android and the host have no thermal
// status: never hot (said once in the log); a symbol missing on API 30+ is logged as an error.
// This classification is telemetry only and does not pause texture preparation or ray bakes.
bool device_hot();

// Main thread: the renderer holds the background work (`why` says why; null releases it). Logged on change.
void hold_background_work(const char *why);
// Why the background work must wait now (an explicit hold only); null: it may run. Any thread.
const char *background_wait_reason();

} // namespace storm::effects
