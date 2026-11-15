#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace storm::sea
{
// Camera coverage only. The solver, wave amplitudes and sampling remain unchanged.
class WaveFocus
{
    std::uint64_t tracked_ = 0;
    bool tracking_ = false, corrected_ = false, initialized_ = false;
    std::array<float, 3> previous_{};
public:
    template<class Sources>
    std::array<float, 3> Update(const std::array<float, 3>& camera,
                              const std::array<float, 3>& look,
                              const std::array<float, 3>& ordinary,
                              const Sources& sources, float dt)
    {
        using Source = typename Sources::value_type;
        const Source* selected = nullptr;
        float best = 0;
        if (look[1] < -0.01f)
        {
            for (const auto& source : sources)
            {
                if (source.kind != Source::Kind::Hull) continue;
                float along = 0, length2 = 0;
                for (int i = 0; i < 3; ++i)
                {
                    const float d = source.position[i] - camera[i];
                    along += d * look[i]; length2 += d * d;
                }
                if (along <= 0) continue;
                const float radius = std::max(source.half_length, source.half_width);
                const bool retained = tracking_ && source.identity == tracked_;
                // Wider release cone prevents jitter when the ray grazes the hull.
                const float limit = radius * (retained ? 1.5f : 1.0f);
                const float miss2 = std::max(0.0f, length2 - along * along);
                if (radius <= 0 || miss2 > limit * limit) continue;
                if (retained) { selected = &source; break; }
                if (!selected || miss2 < best ||
                    (miss2 == best && source.identity < selected->identity))
                { selected = &source; best = miss2; }
            }
        }
        auto target = ordinary;
        tracking_ = selected != nullptr;
        if (selected)
        {
            tracked_ = selected->identity;
            const float dx = ordinary[0] - selected->position[0];
            const float dz = ordinary[2] - selected->position[2];
            const float distance = std::hypot(dx, dz);
            // Keep the viewed hull within the inner half of the 128 m field.
            // Nearby views retain their original centre exactly.
            if (distance > 32.0f)
            {
                target[0] = selected->position[0] + dx * (32.0f / distance);
                target[2] = selected->position[2] + dz * (32.0f / distance);
            }
        }
        if (initialized_ && (corrected_ || target[0] != ordinary[0] || target[2] != ordinary[2]))
        {
            const float dx = target[0] - previous_[0], dz = target[2] - previous_[2];
            const float distance = std::hypot(dx, dz);
            // At most 16 m per submission, below the solver's 64 m reset boundary.
            // Ordinary stop/turn and ownership changes advect existing world-space tails.
            const float step = 64.0f * std::clamp(dt, 0.0f, 0.25f);
            if (distance > step && distance > 0)
            { target[0] = previous_[0] + dx * step / distance;
              target[2] = previous_[2] + dz * step / distance; }
        }
        corrected_ = target[0] != ordinary[0] || target[2] != ordinary[2];
        previous_ = target;
        initialized_ = true;
        return target;
    }
};
}
