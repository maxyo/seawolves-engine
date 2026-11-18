#pragma once
#include <algorithm>
#include <cmath>

namespace storm::android
{
// The mobile land camera (location CameraFollow::MoveCamera, LocationCamera): pure parts.

// The touch camera drag (land: LocationCamera, Player; sea: SHIP_CAMERA, DECK_CAMERA) turns the view
// directly by the finger's travel, 1:1 in the frame the travel arrives (no inertia, no easing), at any
// frame rate, and by the same angle per share of the screen width on every phone: what each camera's
// native per-pixel turn gave on the owner's phone (owner, build 1666: the drag before the per-width
// change, land-camera 402fd5e2a, felt right; 180 and 360 degrees per width were far too slow).
// Its counts are the finger's touch pixels times the camera sensitivity: the game's mouse sensitivity
// («Чувствительность» in the settings, option_screen VMOUSE/HMOUSE_SENSITIVITY_SLIDE, persisted in the
// options file), applied live by PCS_CONTROLS, 0.5..2 (controls.c SetRealMouseSensitivity), 1 in the middle.
constexpr float kOwnerWidth = 3200.0f;   // px: the owner's phone (3200 x 1440), where the native turns were felt
constexpr float kNativeFrame = 1.0f / 60; // s: a native turn per frame scaled by the frame time, as felt at 60 fps

// The native turns per touch pixel (radians), at the sensitivity's default:
// on land the yaw of Player::Rotate (0.01 per count; mobile land camera before 402fd5e2a);
constexpr float kNativeLandYaw = 0.01f;
// on land the pitch of LocationCamera (ChrCamTurnV: dAx 0.05 per count into a velocity 10 dAx dt that decays
// by 8 dt per frame: 0.05 * 10 / 8 dt in all).
constexpr float kNativeLandPitch = 0.05f * 10.0f / 8.0f * kNativeFrame;
// At sea the cameras' own script sensitivities (AICameras.c SensivityAzimuthAngle and so on) times their
// factors (SHIP_CAMERA: 2 and 3 per count scaled by the frame time; DECK_CAMERA: 3 per count).
constexpr float kNativeShipYaw = 2.0f * kNativeFrame, kNativeShipPitch = 3.0f * kNativeFrame, kNativeDeckTurn = 3.0f;

// The turn (radians) of a drag of `counts` on a surface `width` pixels wide, for a camera whose native turn
// was `native` radians per count: as far as that turn went for the same share of the owner's screen.
inline float dragTurn(float counts, float width, float native)
{
    if (!(width > 0.0f) || !std::isfinite(counts))
        return 0.0f;
    return counts * native * kOwnerWidth / width;
}
// The land yaw: about 1833 degrees across the whole width at the default sensitivity (916..3667 with the slider).
inline float dragYaw(float counts, float width)
{
    return dragTurn(counts, width, kNativeLandYaw);
}
// The land pitch: about 191 degrees across a whole width.
inline float dragPitch(float counts, float width)
{
    return dragTurn(counts, width, kNativeLandPitch);
}

// Critically damped motion of `value` towards `target`, settling in about `time` seconds: no overshoot,
// no oscillation, a continuous velocity. Each step is the exact solution for a target held over the
// step, so the path is the same at any frame rate.
struct Damped
{
    float value = 0.0f, velocity = 0.0f;

    void step(float target, float dt, float time)
    {
        if (!(dt > 0.0f) || !(time > 0.0f))
            return;
        const float w = 4.0f / time; // within 10 % of the way after `time`
        const float e = value - target, a = velocity + w * e, k = std::exp(-w * dt);
        value = target + (e + a * dt) * k;
        velocity = (velocity - w * a * dt) * k;
    }
    void reset(float to = 0.0f)
    {
        value = to;
        velocity = 0.0f;
    }
};

// The camera follows the hero while he walks (as mobile third-person games do), gently and rarely:
// once he has walked kDelay seconds, the camera's turn rate eases (Damped, kRateTime) towards
// kRate * shape(sin(offset)), `offset` being his heading minus the camera's. The stick steers
// relative to the camera, so a sideways stick curves him round while the camera comes in behind;
// within kDead of his heading, or straight towards the camera (offset pi), it does not turn.
// The player always wins: a drag (or a swing out of a tight spot) stops the turn at once and holds
// following off until the hero has stood for kStand seconds without a drag and walks again.
// "Walking" is his displacement speed, smoothed over kSpeedTime, with hysteresis (kWalk/kStop):
// Character::IsMove stays set while he walks into a wall, and a single short frame is no stop.
class AutoFollow
{
    float speed_ = 0.0f;    // smoothed displacement speed (m/s)
    float walked_ = 0.0f;   // seconds he has walked since following may start
    float stood_ = 0.0f;    // seconds he has stood (without a drag) before walking
    bool walking_ = false;
    bool held_ = false;     // a drag or swing holds following off until he stands and walks again
    bool dragged_ = false;  // a drag (not a swing) since he last stood and walked again
    Damped rate_;           // the camera's turn rate (rad/s)

  public:
    static constexpr float kDelay = 0.8f, kRate = 0.5f, kDead = 0.35f, kRateTime = 0.8f;
    static constexpr float kWalk = 0.5f, kStop = 0.2f, kSpeedTime = 0.15f, kStand = 0.4f;

    // `speed`: the hero's displacement speed this frame (m/s). The yaw (radians) the camera turns by.
    float step(float dt, bool hold, float speed, float offset)
    {
        if (!(dt > 0.0f))
            return 0.0f;
        speed_ += (speed - speed_) * (1.0f - std::exp(-dt / kSpeedTime));
        walking_ = speed_ > (walking_ ? kStop : kWalk);
        if (hold)
        {
            held_ = dragged_ = true;
            stood_ = walked_ = 0.0f;
            rate_.reset();
            return 0.0f;
        }
        if (!walking_)
        {
            walked_ = 0.0f;
            stood_ += dt;
        }
        else
        {
            if (stood_ >= kStand)
                held_ = dragged_ = false; // he stood, and walks again
            stood_ = 0.0f;
            if (!held_)
                walked_ += dt;
        }
        float target = 0.0f;
        if (walking_ && !held_ && walked_ >= kDelay)
        {
            const float s = std::sin(offset), m = std::max(0.0f, std::fabs(s) - kDead) / (1.0f - kDead);
            target = std::copysign(kRate * m, s);
        }
        const float before = rate_.value;
        rate_.step(target, dt, kRateTime);
        const float turn = 0.5f * (before + rate_.value) * dt; // the rate's mean over the step
        // Never past his heading (a long frame near it).
        const float wrapped = std::remainder(offset, 6.28318531f);
        if (turn * wrapped > 0.0f && std::fabs(turn) > std::fabs(wrapped))
        {
            rate_.reset();
            return wrapped;
        }
        return turn;
    }
    // A swing out of a tight spot holds following off as a drag does.
    void hold()
    {
        held_ = true;
        stood_ = walked_ = 0.0f;
        rate_.reset();
    }
    bool held() const
    {
        return held_;
    }
    // The player has dragged since the hero last stood and walked again (a swing alone does not count).
    bool dragged() const
    {
        return dragged_;
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
// most room (a later one in that order only for kTightGain metres more). Without `swing` (the player
// has dragged since the hero last stood) only the camera's own heading is tried. A swing under way is
// judged at the heading it goes to: a new choice starts from there and never takes it back.
constexpr float kTightClose = 1.2f, kTightPitch = 0.85f, kTightLower = 0.35f, kTightGain = 0.2f;
// The camera swings to the chosen side critically damped over kSwingTime seconds, never faster than
// kSwingMax rad/s (at once on arriving in a location); it rises or lowers over kRaiseTime seconds. A
// chosen view is kept while it has at least kTightKeep of the room wanted; the player's own pitch
// comes back once it has kTightGain metres more than wanted. A drag stops a swing at once; a swing
// holds AutoFollow off as a drag does. After a drag nothing swings until the hero has stood and walks on;
// after a swing the camera swings again only when the view it went to has lost its room (the hero walked
// on into a wall: holding it would leave the camera in his hiding distance, the view on the floor).
constexpr float kSwingTime = 2.0f, kSwingMax = 1.0f, kRaiseTime = 0.6f, kTightKeep = 0.85f;

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
