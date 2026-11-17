#pragma once
#include <algorithm>
#include <cmath>

namespace storm::android
{
// The mobile land camera (location CameraFollow::MoveCamera, LocationCamera): pure parts.

// A horizontal camera drag of `counts` (ChrTurnH: touch pixels after the game's mouse
// sensitivity, 1.0 at its default) on a surface `width` pixels wide: a drag across the whole
// width turns the view by half a turn on any screen size and density, scaled by the setting.
inline float dragYaw(float counts, float width)
{
    if (!(width > 0.0f) || !std::isfinite(counts))
        return 0.0f;
    return counts * 3.14159265f / width;
}

// The camera follows the hero while he moves (as mobile third-person games do): after he has
// moved for kDelay seconds without a drag, the camera is pulled round towards his heading,
// easing in over kEaseIn seconds, at kRate * sin(offset) radians per second, `offset` being
// his heading minus the camera's. The stick steers relative to the camera, so a sideways
// stick curves him round while the camera swings in behind; straight towards the camera
// (offset pi) does not swing it. A drag sets the view and pauses following until the hero
// stands and moves again.
class AutoFollow
{
    float moving_ = 0.0f; // seconds the hero has moved since following may start
    bool paused_ = false;

  public:
    // kMoving: the speed (m/s) from which the hero counts as moving.
    static constexpr float kDelay = 0.6f, kEaseIn = 0.8f, kRate = 0.75f, kMoving = 0.3f;

    // The yaw (radians) the camera turns by this frame.
    float step(float dt, bool dragged, bool heroMoving, float offset)
    {
        if (dragged)
            paused_ = true;
        if (dragged || !heroMoving)
        {
            moving_ = 0.0f;
            if (!heroMoving)
                paused_ = false;
            return 0.0f;
        }
        if (paused_ || !(dt > 0.0f))
            return 0.0f;
        moving_ += dt;
        const float in = std::clamp((moving_ - kDelay) / kEaseIn, 0.0f, 1.0f);
        if (in <= 0.0f)
            return 0.0f;
        const float ease = in * in * (3.0f - 2.0f * in);
        const float turn = ease * kRate * std::sin(offset) * dt;
        // Never past the heading in one frame (a long frame near it).
        const float wrapped = std::remainder(offset, 6.28318531f);
        return std::fabs(turn) > std::fabs(wrapped) ? wrapped : turn;
    }
    bool paused() const
    {
        return paused_;
    }
};

// Tight spaces: the follow camera keeps out of walls by pulling in towards the hero
// (CameraFollow::FindRadius). When the walls leave it less than kTightRoom of its radius at the
// player's pitch, it may rise and look down over the hero instead, up to kTightPitch. Of the
// player's pitch, halfway and kTightPitch (`radius`: the room at each) the lowest with kTightGood
// metres is taken; with none that good, the one with the most room, a higher one only when it
// gains kTightGain metres.
constexpr float kTightRoom = 0.6f, kTightPitch = 1.35f, kTightGood = 1.6f, kTightGain = 0.3f;
inline float tightPitchAt(float pitch, int step)
{
    return pitch >= kTightPitch ? pitch : pitch + (kTightPitch - pitch) * 0.5f * static_cast<float>(step);
}
inline float tightPitch(float pitch, const float (&radius)[3])
{
    for (int step = 0; step < 3; ++step)
        if (radius[step] >= kTightGood)
            return tightPitchAt(pitch, step);
    int best = 0;
    for (int step = 1; step < 3; ++step)
        if (radius[step] >= radius[best] + kTightGain)
            best = step;
    return tightPitchAt(pitch, best);
}

// Cornered (less than kCorneredRoom metres at every pitch, e.g. backed into a corner under a low
// ceiling): the camera turns towards the side with the most room of kCorneredTurns (FindRadius at
// each, `side`), at most kCorneredRate radians per second, when that side has kTightGain more.
constexpr float kCorneredRoom = 1.0f, kCorneredRate = 0.9f;
constexpr float kCorneredTurns[4] = {0.6f, -0.6f, 1.2f, -1.2f};
inline float corneredTurn(float best, const float (&side)[4], float dt)
{
    int pick = 0;
    for (int i = 1; i < 4; ++i)
        if (side[i] > side[pick])
            pick = i;
    if (side[pick] < best + kTightGain)
        return 0.0f;
    return std::clamp(kCorneredTurns[pick], -kCorneredRate * dt, kCorneredRate * dt);
}

// The dialogue close-up: the dialogue sheet covers the lower part of the screen (mobile-ui
// screens/dialogue: 52 % of the height and its margin, more for a long speech), so the speaker's
// face is put kDialogueFaceY of the height from the top, in the middle of what stays visible.
// `elevation`: the face seen from the camera (radians above the horizon); `projectionH`: the
// projection's _22 (1 / tan(half the vertical field of view)). Returns the elevation to look at.
constexpr float kDialogueFaceY = 0.23f;
inline float dialogueLookElevation(float elevation, float projectionH)
{
    if (!(projectionH > 0.0f))
        return elevation;
    return elevation - std::atan((1.0f - 2.0f * kDialogueFaceY) / projectionH);
}
} // namespace storm::android
