// A field with no sources keeps its existing 12-game-second GPU budget. Fade its stored state to zero before
// sleeping, rather than leaving the last rendered height field frozen. A stationary hull is still a source.
#pragma once
#include <algorithm>
#include <utility>

namespace storm::fx
{
struct WaveIdle
{
    static constexpr float kFadeStart = 8.0f;
    static constexpr float kSleep = 12.0f;
    float seconds = 0.0f;
    float pending_retention = 1.0f;

    static float Envelope(float quiet_seconds)
    {
        const float t = std::clamp((quiet_seconds - kFadeStart) / (kSleep - kFadeStart), 0.0f, 1.0f);
        // Smoothstep complement, factored to remain positive near its zero endpoint.
        return (1.0f - t) * (1.0f - t) * (1.0f + 2.0f * t);
    }

    void Tick(float game_seconds, bool has_sources)
    {
        if (has_sources)
        {
            if (Sleeping())
                pending_retention = 1.0f; // the caller already reset the sleeping field
            seconds = 0.0f;
            // Already faded state is never amplified on reentry; any fade awaiting a simulation step stays due.
            return;
        }
        const float before = Envelope(seconds);
        seconds = std::min(seconds + std::max(game_seconds, 0.0f), kSleep);
        pending_retention *= before > 0.0f ? Envelope(seconds) / before : 0.0f;
    }

    bool Sleeping() const { return seconds >= kSleep; }
    // Frames shorter than a simulation step accumulate their attenuation instead of dropping it.
    float TakeRetention() { return std::exchange(pending_retention, 1.0f); }
};
} // namespace storm::fx
