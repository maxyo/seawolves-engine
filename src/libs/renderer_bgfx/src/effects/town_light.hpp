// Our own lightmaps for the towns (fx/town_light.hpp), in place of the game's on Средняя / Высокая.
//
// When an outdoor place is loaded the location reports its lightmapped triangles and lightmaps
// (render::SetEffectsLightmaps) and its still geometry (render::SetEffectsLocationGeometry). On the bake thread
// (fx::PostBake), once per place (cached on disk): each lightmap's texels, the sky's visibility on them, the direction the game's lightmaps
// were baked with (fx::FitBakedSun) and their tone (fx::CalibrateTownLight). The hour's sun needs no bake: whenever it
// has moved by fx::kSunStepDegrees the place's still geometry is drawn once into the place's sun map and every
// lightmap is composed again on the GPU, one a frame, into its own texture (fx/sun_relight.hpp; render(), in two
// pre-game views). A draw that samples one of the game's lightmaps samples ours instead at the same stage
// (Lighting::prepare / submit): one texture fetch as before. Until a place's bake is done, and on Низкая, the game's
// lightmaps stay; nothing is baked while fx_town_light is off (Низкая), and switching it off cancels the bakes still
// running.
//
// With the bounce (set_bounce_scene, effects/bounce.hpp) a job on the bake thread gathers the sun's bounce field
// whenever the sun has moved by kBounceStepDegrees and reads it on the texels: the light the lit surfaces around
// reflect adds to the sun's on our lightmap, and the bounce of the bake's own sun (once per place) leaves the
// game's. The field goes on to the bounce texture (sun_bounce): lit models in the street take the same light.
#pragma once

#include "effects/bounce.hpp"
#include "storm/fx/sun_fit.hpp"
#include "storm/fx/sun_relight.hpp"
#include "storm/fx/town_light.hpp"
#include "storm/render/effects.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace storm::shim
{
struct Device;
struct TexStore;
} // namespace storm::shim

namespace storm::effects
{

class Casters;
class SunShadows;

class TownLight
{
  public:
    // Off: the bakes still running are cancelled and no new one starts; on: the place's bake starts if it is missing.
    void set_enabled(bool enabled);
    // Debug view (option fx_town_light_debug): our lightmap shows the sun's visibility it holds (grey) in place of
    // the relit paint, to prove where the hour's sun reaches.
    void set_debug(bool debug);
    // The place's lightmapped triangles and lightmaps (empty: none, e.g. at sea, indoors, when leaving).
    void set_lightmaps(std::string_view name, std::span<const std::array<float, 3>> positions,
                       std::span<const std::array<float, 2>> uvs, std::span<const uint32_t> indices,
                       std::span<const uint32_t> triangle_lightmap, std::span<const render::EffectsLightmap> lightmaps);
    // The place's still geometry (outdoors): what casts the shadows and closes the sky.
    // `uvs` per position and `cut` per triangle: palm leaves and nets let the sun and the sky through their holes.
    // `colors`: the baked colours per position (the walls' relight is calibrated on them).
    void set_occluders(std::string_view name, std::span<const std::array<float, 3>> positions,
                       std::span<const std::array<float, 2>> uvs, std::span<const uint32_t> indices,
                       std::vector<fx::Occluders::Cut> cut, std::span<const uint32_t> colors);
    // Main thread, every frame: the place's visibility volume (null: none yet); the walls are calibrated with it.
    void set_volume(const fx::VisibilityVolume *volume);
    // The walls' relight (Lighting, u_fxTownWall): on while the sun is up and the walls are calibrated.
    bool walls_active() const { return active() && wall_ready_ && sun_present_ && wall_.valid; }
    const fx::TownLightTone *wall_light() const { return wall_ready_ ? &wall_light_ : nullptr; }
    const fx::TownWallTone *wall_tone() const { return &wall_; }
    const std::array<float, 3> &live_sun() const { return live_sun_; }
    float live_sun_luminance() const { return live_sun_luminance_; }
    std::array<float, 3> bake_sun() const { return {bake_dir_[0], bake_dir_[1], bake_dir_[2]}; }
    // Once per frame, before the draws: `sun` the hour's direction towards the sun (world), `present` false at night.
    void update(const std::array<float, 3> &sun, const std::array<float, 3> &sun_color, bool present);
    // Once per frame after update(): the hour's sun on our lightmaps. Draws the place's sun map when the sun moved
    // (the still geometry's tiles of `shadows`) and composes the lightmaps that are due, one a frame.
    void render(shim::Device &device, SunShadows &shadows, Casters &casters);
    // Main thread, every frame: the place's bounce scene (null: none, or not ready).
    void set_bounce_scene(std::shared_ptr<const Bounce::Scene> scene);
    // The sun's bounce field of the last bounce bake (white sun; null: none yet).
    std::shared_ptr<const fx::BounceField> sun_bounce() const { return sun_bounce_; }
    // The texture a draw samples in place of the game's lightmap `store` (invalid: keep the game's).
    // Our lightmap for a draw that binds the game's `store` (any draw: the shim asks for every lightmap it binds),
    // invalid when it has none; counted for the effects profile.
    bgfx::TextureHandle replacement(const shim::TexStore *store);
    // Ours replaces the game's lightmap `store` (no count: Lighting asks before the shim binds).
    bool relit(const shim::TexStore *store) const;
    // What a shadow leaves of an open texel of our lightmap for `store`: its light without the hour's sun over its
    // light with it, from the lightmap's tone (a + c sky) / (a + b sun + c sky); `unmeasured` where ours is not in place.
    float sun_shade(const shim::TexStore *store, float unmeasured) const;
    void end_frame();
    // Draws of the last frame that bound a game town lightmap / were given ours instead.
    std::array<uint32_t, 2> last_counts() const { return last_counts_; }
    // Since the start: the place's sun map drawn, lightmaps composed, bounce bakes started (host checks, the profile line).
    struct SunCounters
    {
        uint32_t place_draws = 0, composes = 0, bounce_bakes = 0;
    };
    const SunCounters &sun_counters() const { return sun_counters_; }
    bool active() const { return enabled_ && ready_; }

    int initial_state() const {
        if (!enabled_ || positions_.empty() || lightmaps_.empty()) return 0;
        if ((place_job_ && place_job_->state.load() == 2) || (sun_job_ && sun_job_->state.load() == 2) ||
            (wall_job_ && wall_job_->state.load() == 2)) return 2;
        if (place_prepared_ && maps_.empty()) return 0; // calibration rejected all maps: method does not apply
        if (unsupported_reported_) return 0; // no relight on this device (render() said so)
        if (!ready_ || sun_job_ || (wall_job_ && !wall_ready_)) return 1;
        if (bounce_scene_ && !sun_bounce_) return 1; // corrective bake after the scene arrives is part of entry
        for (const auto &map : maps_) if (map.relight == fx::SunRelight::kNoMap) return 2; // its setup failed (logged)
        return 0; // later sun motion remains asynchronous; only the initial maps gate entry
    }
    void shutdown();
    void on_device_lost();

  private:
    struct Map
    {
        const shim::TexStore *store = nullptr; // the game's lightmap (its texture) this one replaces
        std::shared_ptr<const fx::LightmapTexels> texels; // shared with the sun jobs on the bake thread
        std::vector<uint8_t> game;   // its RGBA8 texels at texels' size
        std::vector<uint8_t> sky;    // per texel
        std::vector<uint8_t> baked;  // per texel: the sun's visibility along the direction the game's was baked with
        fx::TownLightTone tone;
        std::vector<uint8_t> bounce_baked, bounce; // RGB per texel (fx::TexelBounce) of the bake's sun and the hour's, empty: none
        fx::SunRelight::Map relight = fx::SunRelight::kNoMap; // its inputs and target on the GPU
        bool compose_due = true;
    };
    struct PlaceJob
    {
        std::atomic<int> state{0}; // 0 running, 1 done
        std::vector<Map> maps;
        fx::SunFit bake;
        std::string how;
        std::shared_ptr<const fx::Occluders> occluders; // the place's still geometry, built once for all its bakes
    };
    // The sun's bounce on the texels for one direction (the bake thread).
    struct SunJob
    {
        std::atomic<int> state{0};
        std::array<float, 3> sun{};
        std::shared_ptr<const fx::BounceField> field;   // the sun's field
        std::vector<std::vector<uint8_t>> bounce;       // per map, per texel
        double bounce_ms = 0.0;
        bool baked_bounce = false;                      // also the bake's sun's bounce (first bake with the scene)
        std::vector<std::vector<uint8_t>> bounce_baked;
    };
    void start_place();
    void start_bounce(const std::array<float, 3> &sun);
    void upload_map(Map &map);
    void upload_bounce(Map &map);
    void compose_all();

    bool enabled_ = false;
    bool ready_ = false;
    bool place_prepared_ = false;
    std::string name_, occluders_name_;
    std::vector<fx::Vec3> positions_;
    std::vector<std::array<float, 2>> uvs_;
    std::vector<uint32_t> indices_, triangle_lightmap_;
    std::vector<render::EffectsLightmap> lightmaps_;
    std::shared_ptr<const fx::Occluders> occluders_;
    std::vector<fx::Vec3> occluder_positions_;
    std::vector<uint32_t> occluder_indices_;
    std::vector<std::array<float, 2>> occluder_uvs_;
    std::vector<fx::Occluders::Cut> occluder_cut_;
    fx::Bake<PlaceJob> place_job_;
    fx::Bake<SunJob> sun_job_;
    std::vector<Map> maps_;
    std::array<float, 3> bounce_sun_{0.0f, 0.0f, 0.0f}; // the direction the bounce was baked for (0: none)
    fx::SunRelight relight_;
    std::array<float, 3> place_sun_{0.0f, 0.0f, 0.0f}; // the direction the place's sun map was drawn for (0: not drawn)
    uint64_t place_cut_signature_ = 0;                  // ... and the cut-out textures it was drawn with
    bool night_ = false;                                // the lightmaps were composed without a sun
    bool unsupported_reported_ = false;
    SunCounters sun_counters_{};
    std::shared_ptr<const Bounce::Scene> bounce_scene_;
    std::vector<uint32_t> occluder_colors_;
    struct WallJob
    {
        std::atomic<int> state{0};
        fx::TownWallTone wall;
        double ms = 0.0;
    };
    fx::Bake<WallJob> wall_job_;
    std::shared_ptr<const fx::VisibilityVolume> wall_volume_; // a copy of the volume the walls are calibrated with
    fx::TownWallTone wall_;
    fx::TownLightTone wall_light_; // the tone of the first relit lightmap
    bool wall_ready_ = false;
    bool sun_present_ = false;
    std::array<float, 3> live_sun_{0.0f, 1.0f, 0.0f};
    float live_sun_luminance_ = 1.0f;
    std::shared_ptr<const fx::BounceField> sun_bounce_;
    fx::Vec3 bake_dir_{0.0f, 1.0f, 0.0f}; // the direction the game's lightmaps were baked with
    unsigned generation_ = 0;                          // bounce bakes taken in at this place (log)
    bool debug_ = false;
    std::array<uint32_t, 2> counts_{}, last_counts_{};
};

} // namespace storm::effects
