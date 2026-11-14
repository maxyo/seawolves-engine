// Simulation steps for fx::SeaWaves (sea_waves.hpp), without the GPU: how many steps, and how long, a frame of game
// time owes. Game time (accelerated time included) runs the water: a frame's time is split into at most kMaxSteps
// steps of kStep, longer ones up to kMaxStep when the frame holds more (a slow frame, x2 / x4 time), so the waves
// travel and fade by game time, not by frames.
#pragma once

#include <algorithm>
#include <cmath>

namespace storm::fx
{

struct WaveClock
{
    static constexpr float kStep = 1.0f / 60.0f; // seconds of a step while the frames are short
    static constexpr float kMaxStep = 0.1f;      // the longest step the wave equation stays stable with (Courant 0.5)
    static constexpr int kMaxSteps = 3;          // per frame
    float pending = 0.0f;                        // game time not yet stepped

    // Steps due after `dt` seconds (0..kMaxSteps), each `step` seconds long. Game time beyond kMaxSteps * kMaxStep
    // in one frame (below about 13.3 frames a second at x4) is dropped and `dropped` says so: the water slows down there.
    int Due(float dt, float &step, bool &dropped)
    {
        pending += std::max(dt, 0.0f);
        dropped = pending > kMaxSteps * kMaxStep;
        pending = std::min(pending, kMaxSteps * kMaxStep);
        const int steps = std::min(static_cast<int>(pending / kStep), kMaxSteps);
        if (steps == 0)
            return 0;
        step = std::min(pending / static_cast<float>(steps), kMaxStep);
        pending = std::max(pending - static_cast<float>(steps) * step, 0.0f);
        return steps;
    }

    // Body momentum forcing keeps its established reference-step calibration over unequal steps.
    static float ForcingStep(float step, float previous_step)
    {
        return step * (step + previous_step) * (0.5f / kStep);
    }
};

} // namespace storm::fx
