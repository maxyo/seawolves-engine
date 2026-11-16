// The storm's bow splash (SEAFOAM's "seafoam_front" particles and the "ship_bow" sound): it bursts while the bow
// climbs out of the sea faster than kRiseRate. Each ship keeps its own gate: the bow's height of the previous frame
// is that ship's own.
#pragma once

namespace storm::sea_foam
{
struct BowSplashGate
{
    static constexpr float kRiseRate = 0.5f; // metres per second the bow's waterline climbs (the game's 5e-4 m/ms)

    // `height`: the bow's waterline in the ship's frame (metres); `seconds`: the frame's time. A frame without time
    // keeps the gate as it was (no rate to measure); the first frame only remembers the height.
    bool Update(float height, float seconds)
    {
        if (!(seconds > 0.0f))
            return splashing;
        splashing = known && (height - last) / seconds >= kRiseRate;
        last = height;
        known = true;
        return splashing;
    }
    void Reset()
    {
        known = splashing = false;
    }

    float last = 0.0f;
    bool known = false;
    bool splashing = false;
};
} // namespace storm::sea_foam
