// Location light components, before any receiver or shadow budget. No renderer/GPU state.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace storm::location
{
// World-space graph edges, independent of model, locator name, texture or receiver budget. Stock Residence02's
// 24-candle chandelier needs a 0.530106 m edge; Residence01's eight-candle rings need 0.861437 m. Connected
// fixtures remain transitive even when their endpoints are farther apart. Separate stock fixtures have gaps
// above this bound (Residence02's chandelier: 3.011533 m to the nearest other candle).
inline constexpr float kLightComponentRadius = 0.9f;
using LightVector = std::array<float, 3>;
struct ComponentSource
{
    uint64_t id = 0; // stable for this source's lifetime, NOT its compacted array index
    LightVector position{}, steady{}, color{};
    float range = 0.0f;
    LightVector attenuation{};
    bool moving = false; // baked and unbaked sources must never share an effective light
};
struct LightComponent : ComponentSource
{
    std::vector<size_t> members; // exact input indices, including chain ends far from the centroid
};
inline double LightLuminance(const LightVector &c)
{
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
}
inline double LightDistanceSquared(const LightVector &a, const LightVector &b)
{
    double d = 0.0;
    for (int k = 0; k < 3; ++k)
        d += (double(a[k]) - b[k]) * (double(a[k]) - b[k]);
    return d;
}
inline std::vector<LightComponent> BuildLightComponents(std::span<const ComponentSource> sources,
                                                       float radius = kLightComponentRadius)
{
    // Stable identity gives deterministic summation and group order under input shuffles, flicker and compaction.
    std::vector<size_t> order;
    for (size_t i = 0; i < sources.size(); ++i)
        if (LightLuminance(sources[i].steady) > 0.0 && sources[i].range > 0.0f)
            order.push_back(i); // switched-off/negative lighter sources cannot bridge two burning fixtures
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sources[a].id < sources[b].id; });
    std::vector<size_t> parent(order.size());
    std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&](size_t i) {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    };
    const double r2 = double(std::max(radius, 0.0f)) * std::max(radius, 0.0f);
    for (size_t i = 0; i < order.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (!sources[order[i]].moving && !sources[order[j]].moving &&
                sources[order[i]].attenuation == sources[order[j]].attenuation &&
                sources[order[i]].range == sources[order[j]].range &&
                LightDistanceSquared(sources[order[i]].position, sources[order[j]].position) <= r2)
            {
                const size_t a = root(i), b = root(j);
                parent[std::max(a, b)] = std::min(a, b);
            }
    std::vector<LightComponent> groups;
    std::vector<size_t> group(order.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
        const size_t r = root(i);
        if (r == i)
        {
            group[r] = groups.size();
            LightComponent g;
            g.id = sources[order[i]].id;
            g.moving = sources[order[i]].moving;
            groups.push_back(g);
        }
        groups[group[r]].members.push_back(order[i]);
    }
    for (LightComponent &g : groups)
    {
        if (g.members.size() == 1)
        {
            static_cast<ComponentSource &>(g) = sources[g.members.front()];
            continue; // exact original singleton, including its range and attenuation
        }
        double weight = 0.0;
        std::array<double, 3> position{}, steady{}, color{};
        // Distinct attenuation laws are separate graph domains: averaging coefficients would not
        // preserve sum(I / (a + b*d + c*d*d)). Common-type coincident lights remain exactly additive.
        g.attenuation = sources[g.members.front()].attenuation;
        for (const size_t i : g.members)
        {
            const ComponentSource &s = sources[i];
            const double w = LightLuminance(s.steady);
            weight += w;
            for (int k = 0; k < 3; ++k)
            {
                position[k] += w * s.position[k];
                steady[k] += s.steady[k];
                color[k] += s.color[k];
            }
        }
        for (int k = 0; k < 3; ++k)
        {
            g.position[k] = float(position[k] / weight);
            g.steady[k] = float(steady[k]);
            g.color[k] = float(color[k]);
        }
        // Enclose every member's original support. The one-point approximation preserves summed RGB,
        // first spatial moment and common-type attenuation; unlike flicker weights it never moves with the flame.
        for (const size_t i : g.members)
            g.range = std::max(g.range, sources[i].range + float(std::sqrt(LightDistanceSquared(g.position, sources[i].position))));
    }
    return groups;
}

// lights[] compacts when a portable source dies. IDs belong to callers; only backing-array indices change.
template<class Moving> void RebaseMovingLights(std::vector<Moving> &moving, int32_t erased)
{
    std::erase_if(moving, [&](const Moving &m) { return m.light == erased; });
    for (Moving &m : moving)
        if (m.light > erased)
            --m.light;
}
} // namespace storm::location
