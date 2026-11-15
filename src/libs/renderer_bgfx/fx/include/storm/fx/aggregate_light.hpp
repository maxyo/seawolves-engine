// Immutable, absolute-world location light transport. No GPU handles or mutable lookup ledger.
#pragma once
#include "storm/fx/light_falloff.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace storm::fx
{
inline constexpr uint32_t kAggregateSources = 256, kAggregateGroups = 256;
inline constexpr uint32_t kAggregateTextureSide = 32, kAggregateBundlesPerFrame = 64;
struct AggregateMember
{
    uint64_t id = 0;
    std::array<float, 3> position{}, color{}, steady{}; // current and steady gamma RGB
    float authored_range = 0.0f;
    std::array<float, 3> attenuation{};
};
struct AggregateGroup
{
    uint64_t id = 0;
    std::array<float, 3> center{};
    uint32_t first = 0, count = 0;
    float authored_range = 0.0f, support = 0.0f;
    std::array<float, 3> attenuation{};
    LightShape shape{}; // derived ONCE from authored_range, never from support
    bool moving = false;
};
struct AggregateSnapshot
{
    uint64_t generation = 0;
    std::vector<AggregateMember> members;
    std::vector<AggregateGroup> groups;
    bool valid() const
    {
        if (members.size() > kAggregateSources || groups.size() > kAggregateGroups) return false;
        for (const auto &g : groups)
            if (!g.count || g.first > members.size() || g.count > members.size() - g.first) return false;
        return true;
    }
};
struct AggregateRef
{
    std::shared_ptr<const AggregateSnapshot> snapshot;
    uint32_t group = 0;
    const AggregateGroup *get() const
    {
        return snapshot && group < snapshot->groups.size() ? &snapshot->groups[group] : nullptr;
    }
    bool operator==(const AggregateRef &) const = default;
};
inline float AggregateDistance(const std::array<float, 3> &a, const std::array<float, 3> &b)
{
    float d = 0;
    for (int k = 0; k < 3; ++k) d += (a[k] - b[k]) * (a[k] - b[k]);
    return std::sqrt(d);
}
inline float AggregateScore(const AggregateRef &ref, const std::array<float, 3> &receiver, float radius)
{
    const auto *g = ref.get();
    if (!g) return 0;
    float score = 0;
    for (uint32_t i = 0; i < g->count; ++i)
    {
        const auto &m = ref.snapshot->members[g->first + i];
        const float d = std::max(AggregateDistance(m.position, receiver) - radius, 0.0f);
        score += std::max({m.steady[0], m.steady[1], m.steady[2]}) *
                 std::min(LightFalloffGammaAt(g->shape, d), 4.0f);
    }
    return score;
}
// Only RGB may differ when a restored snapshot reuses current shadow faces and grid membership.
inline bool SameAggregateGeometry(const AggregateRef &a, const AggregateRef &b)
{
    const auto *x=a.get(), *y=b.get();
    if (!x || !y || x->id!=y->id || x->center!=y->center || x->shape!=y->shape || x->support!=y->support ||
        x->authored_range!=y->authored_range || x->attenuation!=y->attenuation || x->moving!=y->moving || x->count!=y->count)
        return false;
    for (uint32_t i=0;i<x->count;++i)
    {
        const auto &m=a.snapshot->members[x->first+i], &n=b.snapshot->members[y->first+i];
        if (m.id!=n.id || m.position!=n.position || m.steady!=n.steady) return false;
    }
    return true;
}
// CPU packing is the shader's exact two-header/two-texel layout. Failure is atomic: never partial payload.
struct AggregatePayload
{
    std::array<std::array<float, 4>, kAggregateTextureSide * kAggregateTextureSide> texels{};
    std::vector<AggregateRef> refs;
    bool pack(std::span<const AggregateRef> input)
    {
        std::vector<AggregateRef> unique;
        size_t members = 0;
        for (const auto &ref : input)
        {
            if (!ref.get() || !ref.snapshot->valid()) return false;
            if (std::find(unique.begin(), unique.end(), ref) != unique.end()) continue;
            members += ref.get()->count;
            unique.push_back(ref);
        }
        if (unique.size() > kAggregateGroups || members > kAggregateSources) return false;
        std::array<std::array<float, 4>, kAggregateTextureSide * kAggregateTextureSide> packed{};
        uint32_t cursor = static_cast<uint32_t>(2 * unique.size());
        for (uint32_t i = 0; i < unique.size(); ++i)
        {
            const auto &ref = unique[i];
            const auto &g = *ref.get();
            packed[2 * i] = {float(cursor), float(g.count), g.shape.range, g.shape.intensity};
            packed[2 * i + 1] = {g.shape.core * g.shape.core, 0, 0, 0};
            for (uint32_t j = 0; j < g.count; ++j)
            {
                const auto &m = ref.snapshot->members[g.first + j];
                packed[cursor++] = {m.position[0], m.position[1], m.position[2], 0};
                packed[cursor++] = {m.color[0], m.color[1], m.color[2], 0};
            }
        }
        texels = packed;
        refs = std::move(unique);
        return true;
    }
    float index(const AggregateRef &ref) const
    {
        const auto it = std::find(refs.begin(), refs.end(), ref);
        return it == refs.end() ? 0.0f : float(it - refs.begin() + 1);
    }
};
} // namespace storm::fx
