// Sun shadows for the per-pixel lighting (fx::Shadows, FX_LIGHT_SHADOW variants of fs_ff_lit).
//
// One orthographic cascade around the camera's focus, rendered into the pre-game view kPreGameViewBegin (it runs before
// every game view of the frame, so receivers of the same frame read it). The casters are the lit fixed-function draws
// (ships with their sails, characters, lit deck and location objects) that reach the cascade, on screen or not: the
// cascade is published as a shadow volume (effects/shadow_casters.hpp) and the shim's caster entry submits what meets
// it with the depth-only caster program vs_ff_caster[_skin] / fs_ff_caster. The map
// sits at stage 15 (fx::kShadowMapStage): 13 is the bone palette of the skinned vertex shaders. The receivers are the lit draws: they sample the map with their own camera.
// Baked location geometry (lighting off) and surfaces under a lightmap neither cast nor receive: their shading is in
// their colours and lightmaps (outdoors our own: effects/town_light.hpp, for the same sun). Outdoors the place's still
// geometry casts from tiles (set_casters) into the cascade along the hour's sun, so the houses, walls and palms shade
// the characters and ships where the lightmaps shade the ground; its own draws are left out of the volume
// (ShadowVolume::still).
// The map is prepared lazily by the first receiver or caster candidate of a frame (its camera is the scene camera);
// a sun at or below the horizon gives no shadows (fx::Shadows fades them out by design).
#pragma once

#include "storm/fx/settings.hpp"
#include "storm/fx/shadows.hpp"
#include "storm/fx/shadow_model.hpp"
#include "effects/casters.hpp"
#include "effects/shadow_casters.hpp"
#include "storm/render/effects.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <string>
#include <vector>
#include <cstdint>
#include <utility>

namespace storm::shim
{
struct Device;
}

namespace storm::effects
{

class SunShadows
{
  public:
    void configure(const fx::Settings &settings) { settings_ = settings; }
    bool enabled() const { return settings_.shadows != fx::ShadowQuality::Off; }
    // World direction towards the sun of this frame (WEATHER); none: no shadows this frame.
    void set_sun(const std::array<float, 3> &sun_dir, bool present)
    {
        sun_dir_ = sun_dir;
        sun_present_ = present;
    }
    // The place's still, collidable geometry (render::SetEffectsLocationGeometry, outdoors): it casts into the
    // cascade in tiles, with the holes its textures' alpha cuts (effects/casters.hpp). Empty: none (indoors, at sea).
    void set_casters(const render::EffectsLocationGeometry &geometry);
    // For a draw into the game picture with the camera `view` (D3D row-vector device world -> view; `origin`: the
    // device world's origin in absolute world, Lighting::set_world_origin): prepares this frame's map on the first
    // call, with the place's still geometry near the camera as casters, and publishes the cascade to `volumes` for
    // the casters that move. True when the frame has a map: lit draws receive it.
    bool prepare(shim::Device &device, const std::array<float, 16> &view, const std::array<float, 3> &origin,
                 Casters &casters, ShadowCasters &volumes);
    // Receiver: binds the map and its matrix for the draw's camera.
    void bind(const std::array<float, 16> &view) { shadows_.Bind(view); }
    int initial_state(bool applicable = true) const {
        if (!applicable || !enabled() || !sun_present_ || fx::SunShadowStrength(sun_dir_[1]) <= 0.0f) return 0;
        if (!prepared_) return 1;
        if (!active_) return 2;
        if (caster_indices_.empty()) return 0;
        if (tiles_dirty_) return 1;
        return !tiles_failed_ && bgfx::isValid(tiles_vb_) ? 0 : 2;
    }
    void end_frame();

    void shutdown();
    void on_device_lost();

  private:
    fx::Settings settings_{};
    fx::Shadows shadows_;
    std::string casters_name_;
    std::vector<fx::Vec3> casters_;
    std::vector<std::array<float, 2>> caster_uvs_;
    std::vector<uint32_t> caster_indices_;
    // Per triangle: the cut-out texture, null when solid. Held: the game may release its texture wrapper while the
    // place's geometry (and these tiles) live (effects.hpp: Surface::texture is alive only during the report).
    std::vector<shim::TexStorePtr> caster_texture_;
    std::array<float, 3> sun_dir_{0.0f, 1.0f, 0.0f};
    bool sun_present_ = false;
    bool prepared_ = false; // prepare() ran this frame
    bool active_ = false;   // ... and fx::Shadows::Begin() accepted it
    // The place's still geometry in tiles of kTileSize metres (xz): the tiles the cascade reaches are drawn into it.
    struct Tile
    {
        fx::Vec3 lo{}, hi{};
        bgfx::IndexBufferHandle solid = BGFX_INVALID_HANDLE;
        std::vector<std::pair<shim::TexStorePtr, bgfx::IndexBufferHandle>> cut; // with their texture's holes
        uint32_t triangles = 0;
    };
    bgfx::VertexBufferHandle tiles_vb_ = BGFX_INVALID_HANDLE; // positions and texture coordinates of every tile
    std::vector<Tile> tiles_;
    bool tiles_dirty_ = false;
    bool tiles_failed_ = false; // a required VB or (solid/cut) IB failed allocation
    void build_tiles();
    void destroy_tiles();
    uint32_t frame_tiles_ = 0, frame_tile_triangles_ = 0;

  public:
    // The still-geometry casters of the last frame (tiles, triangles): the effects profile line.
    std::array<uint32_t, 2> last_static_casters() const { return last_static_; }

  private:
    std::array<uint32_t, 2> last_static_{};
};

} // namespace storm::effects
