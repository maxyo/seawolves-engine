#pragma once
// Four steps of the game's time acceleration for a touch HUD: x1, x2, x4, x8.
// The scripts keep the logic: TimeScaleCounter (scale = 1 + counter * 0.25) is set one step of
// their own ladder below the target and one TimeScaleFaster / TimeScaleSlower activation runs
// their handler, which applies their guards (underwater, quest locks), clamps (x6 in a fight,
// x3 on land, x2.5 on the world map), logs the new scale once and keeps the TimeSpeed perk in
// step.
//
// The engine's time scale is the truth, not the counter: the HUD's button shows the scale and
// asks for the step after it, and the scripts do not keep the counter in step with the scale.
// Stock Start_InterfaceDoExit sets x2 (GetSeaTimeScale) on leaving any interface whenever the
// TimeSpeed perk is active and leaves the counter at x4 or x8 (the app's script patch
// interface-exit-time-reset resets scale, counter and perk to x1 instead, for the game version
// it is made for); a dialogue sets the scale without the counter. Deciding by the counter, x4 → ship screen → x2 then asked for x4 found
// the counter at x4 already and did nothing, at every tap.
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace storm::android
{
// The scripts' counters of x1, x2, x4, x8 and the counter one of their own steps below each.
inline constexpr std::array<int32_t, 4> TimeScaleCounters{0, 4, 12, 28};
inline constexpr std::array<int32_t, 4> TimeScaleFrom{1, 3, 10, 24};

// "TimeScaleX1" … "TimeScaleX8" → the step, or -1.
inline int timeScaleStep(std::string_view control)
{
    constexpr std::array<std::string_view, 4> names{"TimeScaleX1", "TimeScaleX2", "TimeScaleX4", "TimeScaleX8"};
    for (size_t i = 0; i < names.size(); ++i)
        if (control == names[i]) return static_cast<int>(i);
    return -1;
}

// One HUD request for a step. `Game` is the scripts' side:
//   bool counter(int32_t &value)      TimeScaleCounter (false: no such variable)
template <class Game> void requestTimeScaleStep(Game &game, int step)
{
    int32_t current = 0;
    if (step < 0 || step >= static_cast<int>(TimeScaleCounters.size()) || !game.counter(current)) return;
    const float before = game.scale();
    const float target = 1.f + static_cast<float>(TimeScaleCounters[step]) * 0.25f;
    if (current == TimeScaleCounters[step] && std::fabs(before - target) < 0.01f) return;
    game.setCounter(TimeScaleFrom[step]);
    game.activate(step == 0 ? "TimeScaleSlower" : "TimeScaleFaster");
    if (std::fabs(game.scale() - before) >= 0.01f) return;
    // Nothing changed: the scripts refused (underwater, a quest lock) or their limit (x2.5 on the
    // a step up asked again from there goes round to x1 instead of standing still on a button
    // that no longer changes anything.
    game.setCounter(current);
    if (step > 0) requestTimeScaleStep(game, 0);
}
} // namespace storm::android

#ifdef __ANDROID__
#include "core.h"
#include "platform/android_ui.hpp"
#include "v_data.h"

namespace storm::android
{
// Runs at a completed frame boundary, like every other control activation.
inline void applyTimeScaleStep(int step)
{
    struct Scripts
    {
        VDATA *variable = static_cast<VDATA *>(core.GetScriptVariable("TimeScaleCounter"));
        bool counter(int32_t &value) const { return variable && variable->Get(value); }
        void setCounter(int32_t value) const { variable->Set(value); }
        float scale() const { return core.GetTimeScale(); }
        void activate(const char *control) const
        {
            // No toast for the new scale: the HUD's time button shows it.
            ++gameMessagesMuted;
            core.Event("Control Activation", "s", control);
            --gameMessagesMuted;
        }
    } scripts;
    requestTimeScaleStep(scripts, step);
}
} // namespace storm::android
#endif
