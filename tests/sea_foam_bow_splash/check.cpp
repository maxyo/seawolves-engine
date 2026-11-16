// Host check of the storm's bow splash gate (src/libs/sea_foam/src/bow_splash.hpp): every ship splashes from its own bow's rise.
#include "../../src/libs/sea_foam/src/bow_splash.hpp"

#include <cmath>
#include <cstdio>

int main()
{
    int failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failed; } } while (false)
    using storm::sea_foam::BowSplashGate;

    // Two ships in a storm, updated in turn every frame as SEAFOAM does: one bow rises out of a swell while the
    // other falls into the trough. One gate shared between them (the old static height) compared each bow with the
    // other ship's, so the higher bow splashed all the time and the lower one never did.
    BowSplashGate rising, falling;
    int rising_splashes = 0, falling_splashes = 0;
    const float dt = 1.0f / 60.0f;
    for (int frame = 0; frame < 120; ++frame)
    {
        const float t = frame * dt;
        // Rising at 1 m/s around 21 m (a tall hull's local waterline) and falling at 1 m/s around 0 m.
        rising_splashes += rising.Update(21.0f + 1.0f * t, dt);
        falling_splashes += falling.Update(-1.0f * t, dt);
    }
    CHECK(rising_splashes == 119); // every frame after the first
    CHECK(falling_splashes == 0);

    // A pitching bow: it splashes only while it climbs faster than the threshold, about half of a swell's period.
    BowSplashGate pitching;
    int splashes = 0, frames = 0;
    for (int frame = 0; frame < 600; ++frame, ++frames)
        splashes += pitching.Update(1.5f * std::sin(frame * dt * 0.8f), dt); // 1.2 m/s at most
    CHECK(splashes > frames / 4 && splashes < frames / 2);

    // A paused frame (no game time) keeps the state; a slow drift never splashes.
    BowSplashGate gate;
    gate.Update(0.0f, dt);
    CHECK(gate.Update(1.0f, dt)); // 60 m/s up
    CHECK(gate.Update(5.0f, 0.0f) && gate.last == 1.0f);
    CHECK(!gate.Update(1.0f + 0.4f * dt, dt));
    gate.Reset();
    CHECK(!gate.Update(100.0f, dt)); // after a reset the first frame only remembers

    if (failed)
        return 1;
    std::printf("bow splash: rising bow %d/119 frames, falling bow %d, pitching %d/%d\n", rising_splashes,
                falling_splashes, splashes, frames);
    return 0;
}
