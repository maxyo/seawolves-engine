// Distance-based foam emission and age-based width, independent of rendering/frame rate.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace storm::ship
{
// Keep the stock near/far lift ramp; the observer and emitter are world positions.
template<class Position>
inline float TrackElevation(const Position &camera, const Position &ship, float near_height, float far_height)
{
    const float dx = camera.x - ship.x, dy = camera.y - ship.y, dz = camera.z - ship.z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float blend = std::clamp(distance / 10000.0f, 0.0f, 1.0f);
    return near_height + blend * (far_height - near_height);
}

struct TrackSpacing
{
    double remainder = 0;
    void Reset() { remainder = 0; }

    template<class Emit>
    void Advance(float distance, float spacing, float previous_yaw, float yaw, Emit emit)
    {
        if (!(spacing > 0) || !std::isfinite(spacing) || !std::isfinite(distance) || distance < 0)
        {
            Reset();
            return;
        }
        if (distance == 0) return;
        const float turn = std::remainder(yaw - previous_yaw, 6.283185307179586f);
        double next = spacing - remainder;
        std::size_t count = 0;
        while (next <= distance + 1e-6 * spacing)
        {
            const float fraction = std::clamp(static_cast<float>(next / distance), 0.0f, 1.0f);
            emit(static_cast<float>(std::min(next, double(distance))), fraction, previous_yaw + turn * fraction);
            next += spacing;
            ++count;
        }
        remainder = std::max(0.0, remainder + distance - count * double(spacing));
    }
};

// How much of a trail strip's foam stays at `across` (0..1 over its width) on the strip at `along` (its texture v,
// metres of track / 20) when it is `age` (0..1 of its life) old: the foam breaks into patches that differ edge to edge
// and strip to strip, more the older it gets, instead of every strip repeating the texture's lines. Smooth in both
// directions; `seed` gives each ship and track its own pattern. 1 = all of it.
inline float TrackFoamKeep(float across, float along, float age, float seed)
{
    const float n = 0.5f + 0.5f * std::sin(11.0f * across + 9.3f * along + seed) *
                               std::sin(4.7f * across - 3.9f * along + 1.7f * seed);
    const float breakup = std::clamp(0.45f + 0.55f * age, 0.0f, 1.0f);
    const float t = std::clamp((n - 0.3f) / 0.45f, 0.0f, 1.0f);
    return 1.0f - breakup * t * t * (3.0f - 2.0f * t);
}

// Where a trail strip's cross-section point at `across` (0..1) lies, as a fraction of the strip's width from its
// centre: the strip's edges wander by up to `wobble` of the width along the trail, so its foam lines are not ruled.
inline float TrackFoamOffset(float across, float along, float seed, float wobble)
{
    return (across - 0.5f) * (1.0f + wobble * std::sin(5.3f * along + seed + 2.0f * across));
}

inline float TrackWidth(float initial, float spread_speed, float age, float lifetime)
{
    if (!(lifetime > 0)) return 0;
    const float time = std::clamp(age, 0.0f, lifetime);
    // Integral of spread_speed * (1 - time/lifetime), not a frame-size-dependent Euler update.
    return std::max(0.0f, initial + spread_speed * time * (1 - time / (2 * lifetime)));
}
} // namespace storm::ship
