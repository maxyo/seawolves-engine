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

// Tight spaces. The follow camera keeps out of walls by pulling in towards the hero
// (CameraFollow::FindRadius: the room, in metres, behind him in a direction). While the room at
// the camera's heading and the player's pitch is at least `want` (kTightClose, or less of a short
// zoomed radius) the camera only comes closer. With less, it looks for a view that frames the hero
// from at least `want`, in this order: swung sideways at the player's pitch, lower (a low ceiling
// leaves more room nearer the floor), raised halfway, raised to kTightPitch, and only then from in
// front of him. It never rises above kTightPitch (about 49 degrees, an over-the-shoulder look,
// never top-down) unless the player's own pitch is higher. With no view that good, the one with the
// most room (a later one in that order only for kTightGain metres more). Without `swing` (just
// after a drag) only the camera's own heading is tried.
constexpr float kTightClose = 1.2f, kTightPitch = 0.85f, kTightLower = 0.35f, kTightGain = 0.2f;
// The camera swings to the chosen side at kSwingRate rad/s (at once on arriving in a location); a
// chosen view is looked for again after kTightRecheck seconds or when it closes; a drag stops the
// swing and holds it off for kDragHold seconds. A swing pauses AutoFollow as a drag does.
constexpr float kSwingRate = 1.6f, kTightRecheck = 0.25f, kDragHold = 1.0f;

struct TightView
{
    float yaw = 0.0f;   // to turn the camera by (radians)
    float pitch = 0.0f; // the camera's pitch
    float room = 0.0f;  // the room there (metres)
};

// `probe(yawOffset, pitch)`: the room with the camera turned by yawOffset at that pitch.
template <class Probe> TightView tightView(float pitch, float want, bool swing, Probe &&probe)
{
    constexpr float kHalf = 3.14159265f;
    const float cap = std::max(pitch, kTightPitch);
    const float low = std::max(0.0f, pitch - kTightLower);
    const bool lower = low < pitch - 0.05f; // a player's pitch near level has no lower one to try
    struct Stage
    {
        float pitch;
        float yaws[3];
        int count;
        bool low;
    };
    const Stage stages[] = {
        {pitch, {0.0f, 0.0f, 0.0f}, 1, false}, {pitch, {0.5f, 1.0f, 1.5f}, 3, false},
        {low, {0.0f, 0.5f, 1.0f}, 3, true},    {pitch + (cap - pitch) * 0.5f, {0.0f, 0.5f, 1.0f}, 3, false},
        {cap, {0.0f, 0.5f, 1.0f}, 3, false},   {pitch, {2.1f, kHalf, 0.0f}, 2, false},
        {low, {2.1f, kHalf, 0.0f}, 2, true},
    };
    TightView best{0.0f, pitch, -1.0f};
    for (const Stage &stage : stages)
    {
        if (stage.low && !lower)
            continue;
        for (int i = 0; i < stage.count; ++i)
        {
            const float yaw = stage.yaws[i];
            if (yaw != 0.0f && !swing)
                continue;
            TightView pick{yaw, stage.pitch, probe(yaw, stage.pitch)};
            if (yaw != 0.0f && yaw != kHalf) // the mirrored side too: the one with more room
                if (const float other = probe(-yaw, stage.pitch); other > pick.room)
                    pick = {-yaw, stage.pitch, other};
            if (pick.room >= want)
                return pick;
            if (pick.room > best.room + (best.room < 0.0f ? 0.0f : kTightGain))
                best = pick;
        }
    }
    return best;
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
