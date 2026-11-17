// The sun's shadow maps for the receivers that define FX_LIGHT_SHADOW (fx_lighting.sh) or include
// ff_dyn_shadow.sh: one D16 array texture of two layers on one projection (fx/shadow_model.hpp), hardware-filtered
// depth compare. Layer 0, static: the place's still geometry, drawn only when SunLayerTracker asks. Layer 1,
// dynamic: the moving casters, cleared and drawn in the frames that have any.
//
// Per frame:
//   const auto step = tracker.Advance(sun, focus, SunShadowBudgetFor(settings.shadows).size);
//   if (shadows.Begin(settings, {static_view, dynamic_view, step.layers, origin, strength, step.redraw != None}))
//   {
//       if (shadows.StaticWanted())                          // the tracker's redraw, or a new texture
//           for each still caster: ... bgfx::submit(static_view, shadows.CasterProgram());
//       for each moving caster: ... bgfx::submit(dynamic_view, shadows.CasterProgram()); shadows.CountDynamic();
//   }
//   ...
//   shadows.Bind(camera_view, receiver);                     // before every receiver draw call
//   shadows.End();                                           // once, before bgfx::frame()
#pragma once

#include "storm/fx/shadow_model.hpp"

#include <bgfx/bgfx.h>

namespace storm::fx
{

struct SunFrame
{
    bgfx::ViewId static_view = 0;  // two views before the scene views
    bgfx::ViewId dynamic_view = 0;
    SunLayers layers;              // of this frame (SunLayerTracker::Advance)
    Vec3 origin{};                 // the world position the callers' coordinates are relative to
    float strength = 1.0f;         // SunShadowStrength of the frame's sun
    bool redraw_static = false;    // the tracker asked for the static layer
};

// How a draw receives.
struct SunReceiver
{
    bool dynamic_reaches = true; // a moving caster's shadow can fall on it (else the dynamic layer is not read)
    // Ground mode (fx_sun_map.sh fxSunGround): only the moving casters' shadows darken the surface, down to `shade`
    // (its light without the sun over its light with it), and not where the static layer has it in the shade already.
    bool ground = false;
    float shade = 0.5f;
};

class Shadows
{
  public:
    // Depth only, front and back faces (sails are single-sided sheets).
    static constexpr uint64_t kCasterState = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;

    Shadows() = default;
    ~Shadows();
    Shadows(const Shadows &) = delete;
    Shadows &operator=(const Shadows &) = delete;

    // Prepares the two views. Returns false when no layer exists this frame (shadows off, or the light is at or
    // below the horizon: shadows fade out as the sun sets, by design); receivers then get "fully lit" from Bind().
    // An unsupported device is an error (Support::shadow_map).
    bool Begin(const Settings &settings, const SunFrame &frame);
    // After a true Begin(): the static layer is cleared this frame and must be drawn (frame.redraw_static, or the
    // texture is new).
    bool StaticWanted() const { return static_wanted_; }
    // Casters were submitted into the dynamic view (once, or as they come).
    void CountDynamic() { ++dynamic_casters_; }
    void SetDynamicCasters(uint32_t count) { dynamic_casters_ = count; }
    // The static layer holds casters (the receivers skip its taps otherwise, e.g. at sea).
    void SetStaticCasters(bool any) { static_casters_ = any; }
    // Once per frame after the last caster: the dynamic layer is cleared and stored only when it has casters, and
    // once more after its last frame with any.
    void End();
    bgfx::ProgramHandle CasterProgram();

    // Sets the map (sampler stage kShadowMapStage), u_fxShadowMtx and u_fxShadow for the next draw call.
    // `camera_view`: the receiver's world -> view matrix (receivers shade in view space; draws of one frame may use
    // different cameras). Required for every draw of a receiver program; before the first Begin() it is an error.
    void Bind(const std::array<float, 16> &camera_view, const SunReceiver &receiver);
    // The same for a draw of a receiver program that receives nothing (no sun, an interior, a lightmapped surface): the
    // map of `settings` (made here if no frame made it yet) and u_fxShadow zero (strength 0: fx_lighting.sh returns
    // "fully lit" without a tap; not a ground receiver). The preset's lit / baked programs all have the sun shadow
    // (Lighting::prepare_programs), so the draws without it bind this. False: no map for these settings (an error).
    bool BindUnshadowed(const Settings &settings);

    void Shutdown();

  private:
    void EnsureResources(int map_size);

    bgfx::TextureHandle map_ = BGFX_INVALID_HANDLE; // two layers
    bgfx::FrameBufferHandle static_target_ = BGFX_INVALID_HANDLE, dynamic_target_ = BGFX_INVALID_HANDLE;
    int map_size_ = 0;
    bgfx::ProgramHandle caster_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_map_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_matrix_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_params_ = BGFX_INVALID_HANDLE;
    std::array<float, 16> world_to_shadow_{}; // callers' world -> static layer uv and 0..1 depth
    std::array<float, 16> camera_view_{};     // the camera view_to_shadow_ was made for
    std::array<float, 16> view_to_shadow_{};
    std::array<std::array<float, 4>, 4> params_{};
    float ground_clearance_ = 0.0f; // u_fxShadow[3].x
    bgfx::ViewId dynamic_view_ = 0;
    bool active_ = false;         // Begin() accepted this frame
    bool static_valid_ = false;   // the static layer was drawn into this texture
    bool static_wanted_ = false;
    bool static_casters_ = false;
    bool dynamic_dirty_ = true;   // the dynamic layer holds something other than "open"
    uint32_t dynamic_casters_ = 0;
    bool reported_bind_ = false; // one report per object is enough: both causes mean "no shadow map"
};

} // namespace storm::fx
