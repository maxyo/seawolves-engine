// Sample each hull at its substep time instead of stamping the final frame pose several times.
#pragma once
#include "storm/fx/wave_source.hpp"
#include <algorithm>
#include <cmath>
#include <span>

namespace storm::fx
{
class WaveSourceHistory
{
  public:
    static constexpr size_t kCapacity = 16;
    void Reset() { count_ = 0; }

    WaveSource Sample(std::span<const WaveSource> current, size_t index, float frame_seconds, float fraction) const
    {
        WaveSource result = current[index];
        if (result.kind != WaveSource::Kind::Hull || frame_seconds <= 0)
            return result;
        const WaveSource *previous = nullptr;
        if (result.identity != 0)
        {
            // Duplicated ids are invalid input: never pick one arbitrarily or blend different objects.
            if (std::count_if(current.begin(), current.end(), [&](const WaveSource &s) {
                    return s.identity == result.identity; }) != 1)
                return result;
            for (size_t i = 0; i < count_; ++i)
                if (sources_[i].identity == result.identity)
                {
                    if (previous) return result;
                    previous = &sources_[i];
                }
        }
        // Anonymous standalone callers can describe one continuous hull. Multiple anonymous sources cannot
        // be associated safely when importance sorting changes; they retain frame-held poses.
        else if (current.size() == 1 && count_ == 1 && sources_[0].identity == 0 &&
                 sources_[0].half_length == result.half_length && sources_[0].half_width == result.half_width)
            previous = &sources_[0];
        if (!previous || previous->kind != result.kind)
            return result;
        const double dx = result.position[0] - previous->position[0];
        const double dz = result.position[1] - previous->position[1];
        // Same teleport boundary as the hull producer; reversing heading discontinuously is also a reset.
        const float dot = result.forward[0]*previous->forward[0] + result.forward[1]*previous->forward[1];
        if (std::hypot(dx, dz) > 50.0 * frame_seconds + 5.0 || dot <= 0.0f)
            return result;
        const float t = std::clamp(fraction, 0.0f, 1.0f);
        for (size_t i = 0; i < 2; ++i)
        {
            result.position[i] = previous->position[i] + (result.position[i] - previous->position[i]) * t;
            result.forward[i] = std::lerp(previous->forward[i], result.forward[i], t);
        }
        const float length = std::hypot(result.forward[0], result.forward[1]);
        if (length > 0) for (auto &x : result.forward) x /= length;
        result.speed = std::lerp(previous->speed, result.speed, t);
        result.half_length = std::lerp(previous->half_length, result.half_length, t);
        result.half_width = std::lerp(previous->half_width, result.half_width, t);
        result.stern = std::lerp(previous->stern, result.stern, t);
        result.stem = std::lerp(previous->stem, result.stem, t);
        for (size_t i = 0; i < WaveSource::kHullStations; ++i)
        {
            result.station_side[i] = std::lerp(previous->station_side[i], result.station_side[i], t);
            result.station_half[i] = std::lerp(previous->station_half[i], result.station_half[i], t);
        }
        return result;
    }

    void Remember(std::span<const WaveSource> sources)
    {
        count_ = std::min(sources.size(), sources_.size());
        std::copy_n(sources.begin(), count_, sources_.begin());
    }

  private:
    std::array<WaveSource, kCapacity> sources_{};
    size_t count_ = 0;
};
} // namespace storm::fx
