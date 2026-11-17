#include "effects/shadows.hpp"

#include "effects/generation_priority.hpp"
#include "shim/device.hpp"
#include "storm/render/frame_owner.hpp"
#include "fnv_hash.hpp"

#include <bx/math.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace storm::effects
{

namespace
{
// The shadowed region is centred on the player (the controlled character on land, the main ship at sea: the sea
// camera stands 50-150 m off the ship, which a region in front of the camera would miss altogether) while the
// camera is within kPlayerReach of it; a camera with no player near (the menu's scene, a fly-by) centres it kAhead
// metres in front of itself. Its size, the static layer's band and the caster reach are fx/shadow_model.hpp's.
constexpr float kAhead = 18.0f;
constexpr float kPlayerReach = 250.0f;
constexpr bgfx::ViewId kStaticView = render::kPreGameViewBegin;
constexpr bgfx::ViewId kDynamicView = render::kPreGameViewBegin + 3; // 1 and 2 are the lamp atlases
// The place's still geometry is cut into tiles of this size (xz) so the static layer draws only what it reaches.
constexpr float kTileSize = 24.0f;

float Dot(const fx::Vec3 &a, const fx::Vec3 &b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

} // namespace

void SunShadows::set_casters(const render::EffectsLocationGeometry &geometry)
{
    casters_name_ = std::string(geometry.name);
    casters_.assign(geometry.positions.begin(), geometry.positions.end());
    caster_indices_.assign(geometry.indices.begin(), geometry.indices.end());
    caster_uvs_.clear();
    caster_texture_.assign(caster_indices_.size() / 3, nullptr);
    if (geometry.uvs.size() == geometry.positions.size())
    {
        caster_uvs_.assign(geometry.uvs.begin(), geometry.uvs.end());
        for (const auto &surface : geometry.surfaces)
        {
            const shim::TexStorePtr store = shim::shared_store_of(surface.texture);
            if (!store || !store->cutout || location_caster_material(store.get()).cut <= 0.0f ||
                surface.first_index + surface.index_count > caster_indices_.size())
                continue;
            for (uint32_t i = surface.first_index / 3; i < (surface.first_index + surface.index_count) / 3; ++i)
                caster_texture_[i] = store;
        }
    }
    else
        caster_uvs_.assign(casters_.size(), {0.0f, 0.0f});
    tiles_dirty_ = true;
    tracker_.Invalidate();
}

bool SunShadows::prepare(shim::Device &device, const std::array<float, 16> &view, const std::array<float, 3> &origin,
                         Casters &casters, ShadowCasters &volumes)
{
    if (prepared_)
        return active_;
    prepared_ = true;
    active_ = false;
    if (!enabled() || !sun_present_)
        return false;
    origin_ = origin;
    // Camera position -t * R^T and forward (+z of the row-vector view: the third column), in absolute world.
    fx::Vec3 eye, focus;
    for (int i = 0; i < 3; ++i)
    {
        eye[i] = origin[i] - (view[12] * view[i * 4 + 0] + view[13] * view[i * 4 + 1] + view[14] * view[i * 4 + 2]);
        focus[i] = eye[i] + view[i * 4 + 2] * kAhead;
    }
    // 0 the camera's view (no player known), 1 the controlled character, 2 the main ship, 3 the camera's view (the
    // player is far). Said again whenever it or the player entity changes (a load, a new location, boarding a boat).
    int centre = 0;
    float player_distance = 0.0f;
    const GenerationFocus &player_focus = generation_focus();
    const void *player_owner = nullptr;
    if (std::array<float, 3> player; player_focus.player(player))
    {
        const fx::Vec3 d{player[0] - eye[0], player[1] - eye[1], player[2] - eye[2]};
        player_distance = std::sqrt(Dot(d, d));
        player_owner = player_focus.player_owner();
        centre = player_distance > kPlayerReach ? 3 : player_focus.player_role() == PlayerRole::Ship ? 2 : 1;
        if (centre != 3)
            focus = player;
    }
    if (centre != logged_centre_ || player_owner != logged_owner_)
    {
        logged_centre_ = centre;
        logged_owner_ = player_owner;
        static const char *const what[] = {"in front of the camera (no player position is known)",
                                           "on the controlled character", "on the player's ship",
                                           "in front of the camera (the player is too far from it)"};
        shim::logf("effects: sun shadows: the shadowed region is centred %s, %.0f m from the camera", what[centre],
                   centre == 0 ? kAhead : player_distance);
    }
    const float strength = fx::SunShadowStrength(sun_dir_[1]);
    if (strength <= 0.0f)
        return false;
    if (tiles_dirty_)
        build_tiles();
    if (cut_signature() != drawn_cut_signature_)
        tracker_.Invalidate();
    const fx::SunLayerTracker::Frame step =
        tracker_.Advance({sun_dir_[0], sun_dir_[1], sun_dir_[2]}, focus, fx::SunShadowBudgetFor(settings_.shadows).size,
                         tiles_.empty() ? fx::kSunShadowRadiusSea : fx::kSunShadowRadius);
    layers_ = step.layers;
    fx::SunFrame frame;
    frame.static_view = kStaticView;
    frame.dynamic_view = kDynamicView;
    frame.layers = layers_;
    frame.origin = {origin[0], origin[1], origin[2]};
    frame.strength = strength;
    frame.redraw_static = step.redraw != fx::SunLayerTracker::Redraw::None;
    active_ = shadows_.Begin(settings_, frame);
    if (!active_)
    {
        tracker_.Invalidate(); // nothing was drawn for this step
        return false;
    }
    if (shadows_.StaticWanted())
    {
        const fx::SunLayers &l = layers_;
        const std::array<uint32_t, 2> drawn = draw_tiles(device, casters, kStaticView, frame.origin, [&l](const fx::Vec3 &c, float r) {
            // The tile's sphere against the layer's box in light space.
            const float x = Dot(c, l.right) - l.anchor[0], y = Dot(c, l.up) - l.anchor[1], z = Dot(c, l.forward) - l.anchor[2];
            return std::fabs(x) <= l.static_half + r && std::fabs(y) <= l.static_half + r && z >= l.near_plane - r &&
                   z <= l.far_plane + r;
        });
        drawn_cut_signature_ = cut_signature();
        counters_.static_tiles += drawn[0];
        counters_.static_triangles += drawn[1];
        totals_.static_tiles += drawn[0];
        totals_.static_triangles += drawn[1];
        ++counters_.static_redraws;
        ++totals_.static_redraws;
    }
    shadows_.SetStaticCasters(!tiles_.empty());
    // The dynamic layer as a shadow volume, in the device's world: what moves and meets it is drawn into it by the
    // shim's caster entry. The place's still geometry is in the static layer from its tiles: its own draws stay out.
    ShadowVolume volume;
    volume.layer = ShadowLayer::Dynamic;
    volume.still = tiles_.empty() ? 0 : render::kShadowStillLocation;
    volume.pass = Casters::Pass::Sun;
    volume.view = kDynamicView;
    volume.origin = origin;
    volume.state = fx::Shadows::kCasterState;
    volume.light = {layers_.sun[0], layers_.sun[1], layers_.sun[2], 0.0f};
    volume.shadow_length = layers_.far_plane - layers_.near_plane;
    std::array<float, 16> to_clip;
    bx::mtxMul(to_clip.data(), layers_.View(frame.origin).data(), layers_.DynamicProjection().data());
    volume.set_clip_bounds(to_clip, false); // the model's projections: depth 0..1
    volume_ = volumes.publish(volume);
    volumes_ = &volumes;
    published_ = true;
    return true;
}

std::array<uint32_t, 2> SunShadows::draw_tiles(shim::Device &device, Casters &casters, bgfx::ViewId view, const fx::Vec3 &origin,
                                               const std::function<bool(const fx::Vec3 &, float)> &reaches)
{
    std::array<uint32_t, 2> drawn{};
    if (tiles_dirty_)
        build_tiles();
    if (tiles_failed_ || tiles_.empty())
        return drawn; // a loading error is kept: never submit invalid required buffers
    const bgfx::ProgramHandle program = casters.program(device, Casters::Pass::Sun, false, CasterMaterial{});
    if (!bgfx::isValid(program))
        return drawn;
    // The tiles are absolute.
    float to_view_world[16];
    bx::mtxTranslate(to_view_world, -origin[0], -origin[1], -origin[2]);
    for (const Tile &tile : tiles_)
    {
        fx::Vec3 c, h;
        for (int k = 0; k < 3; ++k)
        {
            c[k] = 0.5f * (tile.lo[k] + tile.hi[k]);
            h[k] = 0.5f * (tile.hi[k] - tile.lo[k]);
        }
        if (!reaches(c, std::sqrt(Dot(h, h))))
            continue;
        if (bgfx::isValid(tile.solid))
        {
            bgfx::setTransform(to_view_world);
            bgfx::setVertexBuffer(0, tiles_vb_);
            bgfx::setIndexBuffer(tile.solid);
            bgfx::setState(fx::Shadows::kCasterState);
            bgfx::submit(view, program);
        }
        // Foliage, nets, gratings: with their texture, so the shadow has their holes (the colour pass's rule).
        for (const auto &[store, triangles] : tile.cut)
        {
            const bgfx::ProgramHandle program_cut = casters.program(device, Casters::Pass::Sun, false, location_caster_material(store.get()));
            if (!bgfx::isValid(program_cut))
                continue;
            store->sync();
            bgfx::setTexture(0, casters.sampler(), store->handle);
            bgfx::setTransform(to_view_world);
            bgfx::setVertexBuffer(0, tiles_vb_);
            bgfx::setIndexBuffer(triangles);
            bgfx::setState(fx::Shadows::kCasterState);
            bgfx::submit(view, program_cut);
        }
        ++drawn[0];
        drawn[1] += tile.triangles;
    }
    return drawn;
}

uint64_t SunShadows::cut_signature() const
{
    uint64_t h = storm::hash::kFnvOffset;
    for (const Tile &tile : tiles_)
        for (const auto &[store, triangles] : tile.cut)
        {
            h = storm::hash::fnvRound(h, store ? store->handle.idx : 0xffffu);
        }
    return h;
}

bool SunShadows::shaded(const fx::Vec3 &center, float radius) const
{
    if (!active_ || !footprint_)
        return false;
    // The casters' box swept along the light (forward: away from the sun), in light space.
    const fx::Vec3 at{center[0] + origin_[0], center[1] + origin_[1], center[2] + origin_[2]};
    const float x = Dot(at, layers_.right), y = Dot(at, layers_.up), z = Dot(at, layers_.forward);
    return x >= footprint_lo_[0] - radius && x <= footprint_hi_[0] + radius && y >= footprint_lo_[1] - radius &&
           y <= footprint_hi_[1] + radius && z >= footprint_near_ - radius;
}

void SunShadows::build_tiles()
{
    destroy_tiles();
    tiles_dirty_ = false;
    tiles_failed_ = false;
    if (caster_indices_.empty())
        return;
    float x0 = 1e30f, z0 = 1e30f;
    for (const fx::Vec3 &p : casters_)
    {
        x0 = std::min(x0, p[0]);
        z0 = std::min(z0, p[2]);
    }
    // One vertex buffer of the whole place (positions, texture coordinates); per tile the solid triangles and, per
    // cut-out texture, its own.
    struct Cell
    {
        fx::Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        std::vector<uint32_t> solid;
        std::vector<std::pair<shim::TexStorePtr, std::vector<uint32_t>>> cut;
    };
    std::unordered_map<uint64_t, Cell> cells;
    for (size_t t = 0; t + 2 < caster_indices_.size(); t += 3)
    {
        const fx::Vec3 &a = casters_[caster_indices_[t]], &b = casters_[caster_indices_[t + 1]], &c = casters_[caster_indices_[t + 2]];
        const auto ix = static_cast<uint64_t>(std::max(0.0f, ((a[0] + b[0] + c[0]) / 3.0f - x0) / kTileSize));
        const auto iz = static_cast<uint64_t>(std::max(0.0f, ((a[2] + b[2] + c[2]) / 3.0f - z0) / kTileSize));
        Cell &cell = cells[ix << 32 | iz];
        for (const fx::Vec3 *v : {&a, &b, &c})
            for (int d = 0; d < 3; ++d)
            {
                cell.lo[d] = std::min(cell.lo[d], (*v)[d]);
                cell.hi[d] = std::max(cell.hi[d], (*v)[d]);
            }
        const shim::TexStorePtr &texture = caster_texture_[t / 3];
        std::vector<uint32_t> *into = &cell.solid;
        if (texture)
        {
            auto it = std::find_if(cell.cut.begin(), cell.cut.end(), [&](const auto &e) { return e.first == texture; });
            if (it == cell.cut.end())
                it = cell.cut.insert(cell.cut.end(), {texture, {}});
            into = &it->second;
        }
        into->insert(into->end(), caster_indices_.begin() + static_cast<std::ptrdiff_t>(t),
                     caster_indices_.begin() + static_cast<std::ptrdiff_t>(t + 3));
    }
    struct Vertex
    {
        float x, y, z, u, v;
    };
    std::vector<Vertex> vertices(casters_.size());
    for (size_t i = 0; i < casters_.size(); ++i)
        vertices[i] = {casters_[i][0], casters_[i][1], casters_[i][2], caster_uvs_[i][0], caster_uvs_[i][1]};
    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float).end();
    tiles_vb_ = bgfx::createVertexBuffer(bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(Vertex))), layout);
    tiles_failed_ = !bgfx::isValid(tiles_vb_);
    auto index_buffer = [this](const std::vector<uint32_t> &indices) {
        const auto handle = bgfx::createIndexBuffer(bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * 4)), BGFX_BUFFER_INDEX32);
        tiles_failed_ |= !bgfx::isValid(handle);
        return handle;
    };
    size_t cut_triangles = 0;
    for (auto &[key, cell] : cells)
    {
        Tile tile;
        tile.lo = cell.lo;
        tile.hi = cell.hi;
        tile.triangles = static_cast<uint32_t>(cell.solid.size() / 3);
        if (!cell.solid.empty())
            tile.solid = index_buffer(cell.solid);
        for (const auto &[texture, indices] : cell.cut)
        {
            tile.cut.push_back({texture, index_buffer(indices)});
            tile.triangles += static_cast<uint32_t>(indices.size() / 3);
            cut_triangles += indices.size() / 3;
        }
        tiles_.push_back(std::move(tile));
    }
    shim::logf("effects: sun shadows: %s's still geometry (%zu triangles, %zu of them cut out by their texture) casts in "
               "%zu tiles of %.0f m",
               casters_name_.c_str(), caster_indices_.size() / 3, cut_triangles, tiles_.size(), kTileSize);
}

void SunShadows::destroy_tiles()
{
    for (Tile &tile : tiles_)
    {
        if (bgfx::isValid(tile.solid))
            bgfx::destroy(tile.solid);
        for (auto &[texture, triangles] : tile.cut)
            if (bgfx::isValid(triangles)) bgfx::destroy(triangles);
    }
    tiles_.clear();
    if (bgfx::isValid(tiles_vb_))
        bgfx::destroy(tiles_vb_);
    tiles_vb_ = BGFX_INVALID_HANDLE;
}

void SunShadows::before_frame()
{
    if (published_ && volumes_)
        shadows_.SetDynamicCasters(volumes_->stats(volume_).draws);
    shadows_.End();
}

void SunShadows::end_frame()
{
    if (published_ && volumes_)
    {
        const ShadowVolumeStats &stats = volumes_->stats(volume_);
        counters_.dynamic_casters += stats.draws;
        totals_.dynamic_casters += stats.draws;
        if (stats.draws > 0)
        {
            ++counters_.dynamic_frames;
            ++totals_.dynamic_frames;
        }
        // The casters' box (the volume's space: the device's world of this frame) in light space, for the next
        // frame's receivers.
        footprint_ = stats.draws > 0 && stats.lo[0] <= stats.hi[0];
        if (footprint_)
        {
            footprint_lo_ = {1e30f, 1e30f};
            footprint_hi_ = {-1e30f, -1e30f};
            footprint_near_ = 1e30f;
            for (int corner = 0; corner < 8; ++corner)
            {
                const fx::Vec3 p{(corner & 1 ? stats.hi[0] : stats.lo[0]) + origin_[0], (corner & 2 ? stats.hi[1] : stats.lo[1]) + origin_[1],
                                 (corner & 4 ? stats.hi[2] : stats.lo[2]) + origin_[2]};
                const float x = Dot(p, layers_.right), y = Dot(p, layers_.up), z = Dot(p, layers_.forward);
                footprint_lo_ = {std::min(footprint_lo_[0], x), std::min(footprint_lo_[1], y)};
                footprint_hi_ = {std::max(footprint_hi_[0], x), std::max(footprint_hi_[1], y)};
                footprint_near_ = std::min(footprint_near_, z);
            }
        }
    }
    // A frame without the layers (a menu over the scene, a frame without the weather) keeps the last footprint.
    volumes_ = nullptr;
    published_ = false;
    prepared_ = false;
    active_ = false;
}

void SunShadows::shutdown()
{
    destroy_tiles();
    caster_texture_.clear();
    shadows_.Shutdown();
    tracker_.Invalidate();
}

} // namespace storm::effects
