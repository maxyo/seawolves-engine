#include "storm/fx/light_set.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace storm::fx
{
namespace
{
float Peak(Vec3 c)
{
    return std::max({c[0], c[1], c[2]});
}
} // namespace

LightShape ShapeOf(const PointLight &light)
{
    return {light.range, light.intensity, light.core};
}

float LightAttenuation(const PointLight &light, float distance)
{
    return LightFalloffGammaAt(ShapeOf(light), distance);
}

LightSelection SelectLights(std::span<const PointLight> lights, Vec3 center, float radius, int max_count, bool include_baked,
                            bool fade_rank)
{
    struct Scored
    {
        float score;
        const PointLight *light;
    };
    std::vector<Scored> scored;
    scored.reserve(lights.size());
    for (const PointLight &light : lights)
    {
        if (light.baked && !include_baked)
            continue;
        const float dx = light.position[0] - center[0], dy = light.position[1] - center[1], dz = light.position[2] - center[2];
        const float nearest = std::max(std::sqrt(dx * dx + dy * dy + dz * dz) - radius, 0.0f);
        // Brightness at the closest point of the object; attenuation is clamped so a light inside the object
        // does not win by an infinite margin.
        Vec3 absolute = center;
        for (int k = 0; k < 3; ++k) absolute[k] += light.aggregate_origin[k];
        const float score = light.aggregate.get() ? AggregateScore(light.aggregate, absolute, radius) :
                            Peak(light.color) * std::min(LightAttenuation(light, nearest), 4.0f);
        if (score > 1.0f / 512.0f)
            scored.push_back({score, &light});
    }
    const size_t keep = std::min<size_t>(scored.size(), static_cast<size_t>(std::clamp(max_count, 0, kMaxPointLights)));
    std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(keep), scored.end(),
                      [](const Scored &a, const Scored &b) { return a.score > b.score; });
    LightSelection selection;
    selection.dropped = static_cast<int>(scored.size() - keep);
    for (size_t i = 0; i < keep; ++i)
    {
        selection.indices[selection.count] = static_cast<uint32_t>(scored[i].light - lights.data());
        selection.lights[selection.count++] = *scored[i].light;
    }
    if (fade_rank && keep > 0 && scored.size() > keep)
    {
        // partial_sort leaves the rest unordered: the strongest left-out light is the largest of them.
        float next = 0.0f;
        for (size_t i = keep; i < scored.size(); ++i)
            next = std::max(next, scored[i].score);
        const float last = scored[keep - 1].score;
        const float weight = std::clamp((last - next) / (kRankFade * last), 0.0f, 1.0f);
        selection.lights[keep - 1].aggregate_scale *= weight;
        for (float &c : selection.lights[keep - 1].color)
            c *= weight;
    }
    return selection;
}

namespace
{
// Warm powder light. The cannon's reach and brightness are those of the game's own cannon light (ShipLights.c
// "cannondefault": 2.0 white, 20 m, 1 / (0.3 d), which Низкая keeps): about as bright on the firing ship's deck and
// sails 3-6 m from the muzzle (measured on the ship:night broadside), warmer, and wider for the per-pixel falloff.
constexpr std::array<FlashProfile, kFlashKinds> kProfiles{{
    {{2.6f, 1.9f, 1.0f}, 9.0f, 0.03f, 0.15f},  // Gunshot
    {{4.4f, 3.0f, 1.3f}, 26.0f, 0.03f, 0.25f}, // Cannon
    {{3.4f, 2.2f, 0.9f}, 24.0f, 0.03f, 0.30f}, // BallBlast
    {{4.0f, 2.6f, 1.1f}, 36.0f, 0.03f, 0.35f}, // Explosion
    {{4.5f, 2.9f, 1.2f}, 60.0f, 0.04f, 0.40f}, // ShipExplosion
}};

// The widest flash range whose game light still ends inside kGameLightReach: its attenuation reaches kGameLightFloor at
// 0.59 of the range (16 x² + 3 x + 1 = 1 / 0.12).
constexpr float kFlashShapeRange = 20.0f;

// Strongest and nearest first: the peak (a flash still rising counts with it), softened with the distance on the scale
// of the flash's own range (no cut-off, so a big explosion far away still outranks a small one equally far).
float Score(const FlashProfile &p, const Vec3 &position, float age, const Vec3 &eye)
{
    const float dx = position[0] - eye[0], dy = position[1] - eye[1], dz = position[2] - eye[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    const float envelope = age < p.rise ? 1.0f : FlashEnvelope(p, age);
    return envelope * Peak(p.color) * p.range * p.range / (p.range * p.range + d2);
}
} // namespace

const FlashProfile &FlashProfileOf(FlashKind kind)
{
    return kProfiles[static_cast<size_t>(kind)];
}

LightShape FlashShapeOf(const FlashProfile &p, const LightFalloff &falloff)
{
    const float range = std::min(p.range, kFlashShapeRange);
    LightShape shape = ShapeOfGameLight(range, 1.0f, 3.0f / range, 16.0f / (range * range), falloff);
    const float scale = p.range / range;
    shape.range *= scale;
    shape.core *= scale;
    shape.intensity *= scale * scale;
    return shape;
}

float FlashEnvelope(const FlashProfile &p, float age)
{
    if (age < 0.0f)
        return 0.0f;
    if (age < p.rise)
        return age / p.rise;
    const float left = 1.0f - (age - p.rise) / p.decay;
    return left > 0.0f ? left * left : 0.0f;
}

void FlashLights::Add(FlashKind kind, Vec3 position)
{
    const FlashProfile &p = FlashProfileOf(kind);
    const float strength = Peak(p.color) * p.range;
    for (Flash &f : flashes_)
    {
        if (!f.fresh)
            continue;
        const float dx = f.position[0] - position[0], dy = f.position[1] - position[1], dz = f.position[2] - position[2];
        if (dx * dx + dy * dy + dz * dz > kMergeDistance * kMergeDistance)
            continue;
        const FlashProfile &q = FlashProfileOf(f.kind);
        if (strength > Peak(q.color) * q.range)
            f.kind = kind;
        return;
    }
    if (flashes_.size() >= kCapacity)
    {
        // Out of room: the flash with the least light left at the camera goes.
        auto weakest = std::min_element(flashes_.begin(), flashes_.end(),
                                        [](const Flash &a, const Flash &b) { return a.score < b.score; });
        flashes_.erase(weakest);
    }
    Flash flash{kind, position};
    flash.score = Score(p, position, 0.0f, eye_);
    flashes_.push_back(flash);
}

void FlashLights::Update(float delta_seconds, Vec3 eye, int cap)
{
    eye_ = eye;
    // A fresh flash was reported during the frame that just ended: it ages with it, so it lights the very next frame.
    for (Flash &f : flashes_)
        f.age += delta_seconds;
    std::erase_if(flashes_, [](const Flash &f) {
        const FlashProfile &p = FlashProfileOf(f.kind);
        return f.age >= p.rise + p.decay;
    });
    // Strength at the camera, the envelope's peak for a flash still rising (so a new flash ranks by what it becomes).
    std::vector<std::pair<float, size_t>> ranked;
    ranked.reserve(flashes_.size());
    for (size_t i = 0; i < flashes_.size(); ++i)
    {
        Flash &f = flashes_[i];
        const FlashProfile &p = FlashProfileOf(f.kind);
        f.score = Score(p, f.position, f.age, eye);
        ranked.push_back({f.score, i});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    const float step = kCapFade > 0.0f ? delta_seconds / kCapFade : 1.0f;
    for (size_t r = 0; r < ranked.size(); ++r)
    {
        Flash &f = flashes_[ranked[r].second];
        const float target = r < static_cast<size_t>(std::max(cap, 0)) ? 1.0f : 0.0f;
        if (f.fresh)
            f.weight = target;
        else
            f.weight = target > f.weight ? std::min(target, f.weight + step) : std::max(target, f.weight - step);
        f.fresh = false;
    }
}

size_t FlashLights::Lit() const
{
    return static_cast<size_t>(std::count_if(flashes_.begin(), flashes_.end(), [](const Flash &f) { return f.weight > 0.0f; }));
}

void FlashLights::AppendTo(std::vector<PointLight> &lights, const LightFalloff &falloff) const
{
    for (const Flash &f : flashes_)
    {
        if (f.weight <= 0.0f || f.fresh)
            continue;
        const FlashProfile &p = FlashProfileOf(f.kind);
        const float k = FlashEnvelope(p, f.age) * f.weight;
        if (k <= 0.0f)
            continue;
        const LightShape shape = FlashShapeOf(p, falloff);
        PointLight light;
        light.position = f.position;
        for (int i = 0; i < 3; ++i)
            light.color[i] = p.color[i] * k;
        light.range = shape.range;
        light.intensity = shape.intensity;
        light.core = shape.core;
        lights.push_back(light);
    }
}

PointLightUniforms PackPointLights(const LightSelection &selection, const std::array<float, 16> &view)
{
    PointLightUniforms u;
    for (int i = 0; i < selection.count; ++i)
    {
        const PointLight &l = selection.lights[i];
        auto &position = u.u_fxPoint[4 * i], &color = u.u_fxPoint[4 * i + 1], &params = u.u_fxPoint[4 * i + 2], &spot = u.u_fxPoint[4 * i + 3];
        for (int j = 0; j < 3; ++j)
        {
            position[j] = l.position[0] * view[j] + l.position[1] * view[4 + j] + l.position[2] * view[8 + j] + view[12 + j];
            spot[j] = l.direction[0] * view[j] + l.direction[1] * view[4 + j] + l.direction[2] * view[8 + j];
            color[j] = l.color[j];
        }
        position[3] = l.range;
        color[3] = l.intensity;
        params[0] = l.core * l.core;
        params[1] = 0.0f;
        // Spot cone as saturate(rho * scale + bias); a point light gets scale 0, bias 1.
        const bool is_spot = l.cos_outer > -1.0f;
        const float width = std::max(l.cos_inner - l.cos_outer, 1e-4f);
        params[2] = is_spot ? 1.0f / width : 0.0f;
        params[3] = is_spot ? -l.cos_outer / width : 1.0f;
        spot[3] = l.falloff;
    }
    u.u_fxPointCount = {static_cast<float>(selection.count), 0.0f, 0.0f, 0.0f};
    u.dropped = selection.dropped;
    return u;
}

PointLightFallback PackUnmergedPointLights(const LightSelection &selection, const std::array<float,16> &view,
                                          uint8_t old_diffuse, uint8_t old_specular, uint8_t directional_mask)
{
    PointLightFallback out;
    out.diffuse_off=old_diffuse & directional_mask;
    out.specular_off=old_specular & directional_mask;
    LightSelection independent;
    for (int i=0;i<selection.count;++i)
    {
        const auto &light=selection.lights[i];
        if (light.aggregate.get() && light.aggregate.get()->count>1) continue;
        independent.lights[independent.count++]=light;
        if (light.device_slot<kMaxPointLights)
        {
            const uint8_t mask=static_cast<uint8_t>(1u<<light.device_slot);
            out.diffuse_off |= old_diffuse & mask;
            out.specular_off |= old_specular & mask;
        }
    }
    out.points=PackPointLights(independent,view);
    return out;
}

} // namespace storm::fx
