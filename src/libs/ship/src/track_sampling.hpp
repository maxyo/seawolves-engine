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

inline float TrackWidth(float initial, float spread_speed, float age, float lifetime)
{
    if (!(lifetime > 0)) return 0;
    const float time = std::clamp(age, 0.0f, lifetime);
    // Integral of spread_speed * (1 - time/lifetime), not a frame-size-dependent Euler update.
    return std::max(0.0f, initial + spread_speed * time * (1 - time / (2 * lifetime)));
}
} // namespace storm::ship
