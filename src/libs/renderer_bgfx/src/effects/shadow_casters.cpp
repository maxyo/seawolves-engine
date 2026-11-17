#include "effects/shadow_casters.hpp"

#include "shim/device.hpp"

#include <bx/math.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace storm::effects
{

namespace
{

bool outside(const std::array<float, 4> &plane, const float p[3], float radius)
{
    return plane[0] * p[0] + plane[1] * p[1] + plane[2] * p[2] + plane[3] < -radius;
}

} // namespace

void ShadowVolume::set_clip_bounds(const std::array<float, 16> &m, bool homogeneous_depth, bool depth)
{
    // clip = p * m (row vectors): component c of the clip position is the dot product of (p, 1) with column c.
    const auto column = [&](int c) { return std::array<float, 4>{m[c], m[4 + c], m[8 + c], m[12 + c]}; };
    const std::array<float, 4> x = column(0), y = column(1), z = column(2), w = column(3);
    const auto combine = [](const std::array<float, 4> &a, float sign, const std::array<float, 4> &b) {
        return std::array<float, 4>{a[0] + sign * b[0], a[1] + sign * b[1], a[2] + sign * b[2], a[3] + sign * b[3]};
    };
    // -w <= x <= w, -w <= y <= w, near (0 or -w) <= z <= w.
    const std::array<std::array<float, 4>, 6> clip{combine(w, 1.0f, x), combine(w, -1.0f, x), combine(w, 1.0f, y),
                                                   combine(w, -1.0f, y), homogeneous_depth ? combine(w, 1.0f, z) : z,
                                                   combine(w, -1.0f, z)};
    plane_count = 0;
    for (size_t i = 0; i < (depth ? clip.size() : 4); ++i)
    {
        const std::array<float, 4> &p = clip[i];
        const float length = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (length <= 1e-12f)
            continue; // not a plane of this projection (a constant row)
        planes[plane_count++] = {p[0] / length, p[1] / length, p[2] / length, p[3] / length};
    }
}

// The caster's sphere centre in the volume's space, or false when the sphere lies outside the volume or its shadow
// outside the camera's view.
bool ShadowCasters::meets(const ShadowVolume &volume, const ShadowCaster &caster, std::array<float, 3> &in_volume) const
{
    for (int k = 0; k < 3; ++k)
        in_volume[k] = caster.center[k] + (caster.origin[k] - volume.origin[k]);
    const float radius = caster.radius;
    if (radius < 0.0f)
        return true; // no bounds: the volume takes it
    if (volume.sphere[3] > 0.0f)
    {
        const float dx = in_volume[0] - volume.sphere[0], dy = in_volume[1] - volume.sphere[1], dz = in_volume[2] - volume.sphere[2];
        const float reach = volume.sphere[3] + radius;
        if (dx * dx + dy * dy + dz * dz > reach * reach)
            return false;
    }
    for (uint8_t i = 0; i < volume.plane_count; ++i)
        if (outside(volume.planes[i], in_volume.data(), radius))
            return false;
    if (volume.light[3] < 0.0f || camera_.plane_count == 0)
        return true;
    // The shadow: the hull of the caster's sphere and of the sphere where its shadow ends, in the camera's space. A
    // camera plane that has both spheres outside has the whole shadow outside.
    float near_end[3], far_end[3], far_radius = radius;
    for (int k = 0; k < 3; ++k)
        near_end[k] = in_volume[k] + (volume.origin[k] - camera_.origin[k]);
    if (volume.light[3] > 0.5f)
    {
        float to[3], distance2 = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            to[k] = in_volume[k] - volume.light[k];
            distance2 += to[k] * to[k];
        }
        const float distance = std::sqrt(distance2);
        if (distance <= radius)
            return true; // the light is inside the caster: its shadow goes everywhere
        const float scale = std::max(1.0f, volume.sphere[3] / distance);
        far_radius = radius * scale;
        for (int k = 0; k < 3; ++k)
            far_end[k] = near_end[k] + to[k] * (scale - 1.0f);
    }
    else
        for (int k = 0; k < 3; ++k)
            far_end[k] = near_end[k] - volume.light[k] * volume.shadow_length;
    for (uint8_t i = 0; i < camera_.plane_count; ++i)
        if (outside(camera_.planes[i], near_end, radius) && outside(camera_.planes[i], far_end, far_radius))
            return false;
    return true;
}

void ShadowCasters::set_camera(const std::array<float, 16> &view_proj, const std::array<float, 3> &origin)
{
    camera_.set_clip_bounds(view_proj, false, false);
    camera_.origin = origin;
}

ShadowCasters::Id ShadowCasters::publish(const ShadowVolume &volume)
{
    if (volume.layer == ShadowLayer::Static && volume.still == 0)
        shim::trap("a static shadow volume without the owner of its still geometry takes nothing");
    const bool sphere = volume.sphere[3] > 0.0f;
    if (volumes_.empty())
        boxed_ = sphere;
    boxed_ = boxed_ && sphere;
    if (boxed_)
        for (int k = 0; k < 3; ++k)
        {
            const double c = static_cast<double>(volume.sphere[k]) + volume.origin[k];
            const double lo = c - volume.sphere[3], hi = c + volume.sphere[3];
            lo_[k] = volumes_.empty() ? lo : std::min(lo_[k], lo);
            hi_[k] = volumes_.empty() ? hi : std::max(hi_[k], hi);
        }
    volumes_.push_back(volume);
    stats_.emplace_back();
    return static_cast<Id>(volumes_.size() - 1);
}

bool ShadowCasters::accepts(const ShadowVolume &volume, const ShadowCaster &caster) const
{
    if (volume.layer == ShadowLayer::Static)
        return caster.still != 0 && caster.still == volume.still;
    if (!caster.lit || (volume.still != 0 && caster.still == volume.still))
        return false;
    return volume.pass != Casters::Pass::Sun || caster.sun;
}

bool ShadowCasters::reach(const std::array<float, 3> &center, float radius, const std::array<float, 3> &origin, uint64_t still) const
{
    ShadowCaster caster;
    caster.center = center;
    caster.radius = radius;
    caster.origin = origin;
    caster.still = still;
    caster.lit = caster.sun = true; // not known before the draw's states are set: the draw itself is tested again
    return takes(caster);
}

bool ShadowCasters::takes(const ShadowCaster &caster) const
{
    if (volumes_.empty())
        return false;
    if (boxed_ && caster.radius >= 0.0f)
        for (int k = 0; k < 3; ++k)
        {
            const double c = static_cast<double>(caster.center[k]) + caster.origin[k];
            if (c + caster.radius < lo_[k] || c - caster.radius > hi_[k])
                return false;
        }
    std::array<float, 3> in_volume;
    for (const ShadowVolume &volume : volumes_)
        if (accepts(volume, caster) && meets(volume, caster, in_volume))
            return true;
    return false;
}

uint32_t ShadowCasters::submit(shim::Device &device, Casters &programs, const ShadowCaster &caster)
{
    uint32_t submits = 0;
    std::array<float, 3> in_volume;
    for (size_t id = 0; id < volumes_.size(); ++id)
    {
        const ShadowVolume &volume = volumes_[id];
        if (!accepts(volume, caster) || !meets(volume, caster, in_volume))
            continue;
        // The program sets its cut uniforms; a failed one is logged by the device.
        Casters::Pass pass = volume.pass;
        if (volume.holder[3] > 0.0f)
        {
            const float dx = in_volume[0] - volume.holder[0], dy = in_volume[1] - volume.holder[1], dz = in_volume[2] - volume.holder[2];
            if (caster.radius < 0.0f || std::sqrt(dx * dx + dy * dy + dz * dz) < volume.holder[3] + caster.radius)
                pass = Casters::Pass::LampHolder;
        }
        const bgfx::ProgramHandle program = programs.program(device, pass, caster.skinned, caster.material);
        if (!bgfx::isValid(program))
            continue;
        float world[16], model[16];
        memcpy(world, caster.world, sizeof(world));
        for (int k = 0; k < 3; ++k)
            world[12 + k] += caster.origin[k] - volume.origin[k];
        bx::mtxMul(model, world, volume.transform.data());
        if (volume.scissor[2] != 0)
            bgfx::setScissor(volume.scissor[0], volume.scissor[1], volume.scissor[2], volume.scissor[3]);
        else
            bgfx::setScissor(UINT16_MAX);
        if (bgfx::isValid(volume.uniform))
            bgfx::setUniform(volume.uniform, volume.uniform_value.data());
        bgfx::setStencil(BGFX_STENCIL_NONE);
        bgfx::setTransform(model);
        bgfx::setState(volume.state | caster.primitive);
        // The caster's buffers, cut textures and bone palette stay for the next volume.
        bgfx::submit(volume.view, program, 0, BGFX_DISCARD_NONE);
        ShadowVolumeStats &stats = stats_[id];
        ++stats.draws;
        stats.triangles += caster.triangles;
        stats.skinned += caster.skinned ? 1 : 0;
        stats.offscreen += caster.offscreen ? 1 : 0;
        if (caster.radius >= 0.0f)
            for (int k = 0; k < 3; ++k)
            {
                stats.lo[k] = std::min(stats.lo[k], in_volume[k] - caster.radius);
                stats.hi[k] = std::max(stats.hi[k], in_volume[k] + caster.radius);
            }
        ++submits;
    }
    bgfx::discard(BGFX_DISCARD_ALL);
    if (submits != 0)
    {
        ++frame_.casters;
        frame_.offscreen += caster.offscreen ? 1 : 0;
        frame_.skinned += caster.skinned ? 1 : 0;
        if (caster.radius < 0.0f)
        {
            ++frame_.unbounded;
            if (!unbounded_reported_)
                shim::logf("effects: shadow casters: a %s draw without bounds (no model node around it, or no vertex "
                           "positions to read) is drawn into every shadow volume; counted as casters_unbounded",
                           caster.skinned ? "skinned" : "lit");
            unbounded_reported_ = true;
        }
    }
    return submits;
}

void ShadowCasters::end_frame()
{
    last_ = std::move(frame_);
    frame_ = {};
    for (size_t id = 0; id < volumes_.size(); ++id)
    {
        const bgfx::ViewId view = volumes_[id].view;
        auto it = std::find_if(last_.views.begin(), last_.views.end(), [&](const ViewCount &v) { return v.view == view; });
        if (it == last_.views.end())
            it = last_.views.insert(last_.views.end(), ViewCount{view, 0, 0});
        it->draws += stats_[id].draws;
        it->triangles += stats_[id].triangles;
    }
    volumes_.clear();
    stats_.clear();
    boxed_ = false;
    camera_.plane_count = 0;
}

} // namespace storm::effects
