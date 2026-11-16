// Per-pixel lighting, point / spot lights and alpha to coverage for the fixed-function emulator (fx_lighting.sh,
// fx_coverage.sh). The shim asks prepare() which program variant a fixed-function draw uses and which of its
// vertex-lighting terms move to the fragment shader, then calls submit() before bgfx::submit.
//
// What the shim sees and what it decides (documented in FX.md, "Lighting"):
//   * lit draw (D3DRS_LIGHTING on, vertex normals, not pre-transformed): vs_ff_lit + a lit family. The first enabled
//     directional light is the sun (per pixel with kLightingPerPixel), the global ambient becomes the hemisphere
//     (kLightingHemisphere), and the strongest point / spot lights up to the budget (4 / 8) are evaluated per
//     pixel (kLightingPoint); the rest stay in the vertex sum, so no light is lost.
//   * baked draw (lighting off, vertex normals): the location's static models with baked colours. They get the
//     dynamic lights the engine reports (SetEffectsDynamicLights: carried lanterns), a baked family; without any of
//     this list's effects the legacy program draws them.
//   * program families (FfFamily, renderer_bgfx/CMakeLists.txt): each lit or baked program compiles only the product
//     blocks of ff_fragment.sh its family names (lamps, AO volume, town wall, rain); ChooseFamily gives a draw the
//     narrowest family that holds all it receives, so a draw without lamps does not carry the lamps' code.
//   * lamps (Settings::lamp_shadows, effects/lamp_shadows.hpp): lit and baked draws into the game picture are relit
//     by the location's own lamps around the camera, each with its shadow maps; a lit draw's D3D point lights that
//     are such lamps leave its sums (LampShadows::relit). Lit draws of an interior take their ambient from the room
//     light (the baked colours around them) instead of the weather's ambient.
//   * material: from the technique the renderer executes (SetEffectsTechnique): Animation* = character,
//     ShipSail* / ShipFlag = sail (two-sided, translucent), DLightModel = stone; lit draws without a technique
//     are the ship hulls, masts and deck objects = hull.
//   * relief (kLightingNormalMap): a lit or baked draw whose stage 1 texture (a grey height map), or else stage 0
//     texture (stone, wood, plaster, ground), has a derived normal map (effects/normal_maps.hpp) uses the
//     FX_LIGHT_NORMALMAP variant with the map at stage 14, that stage's sampler flags and texture coordinates
//     (u_fxFf.y); lit draws perturb the normal, baked ones scale their colour by fxBakedRelief. Characters (their atlases:
//     fx::ReliefClass::Character) take it with Settings::character_relief at its strength, faded out on skin and hair by
//     the vertices' skin weight (ff_fragment.sh ffSkin); their parallax is shallow and has no self-shadow.
//   * parallax (Settings::parallax): a relief draw whose map follows stage 0 (not projected) and has a height takes the
//     FX_LIGHT_PARALLAX variant with the height at stage 7 (stage 0's sampler flags) and u_fxParallax.
//   * sun shadows (Settings::shadows, effects/shadows.hpp): lit draws into the game picture receive both layers
//     (every lit and baked program of such a preset is its FX_LIGHT_SHADOW variant; a draw that receives none binds
//     the map unshadowed), as the towns' relit walls do. Surfaces with their own static light
//     (lightmapped and baked ground, decks) under a moving caster take its shadow from the dynamic layer alone:
//     baked draws in the shadowed variant's ground mode, plain ones through fs_ff_dyn (FfDraw::ground). What casts
//     is not decided here: the lights publish their shadow volumes (shadow_volumes()) and the shim's caster entry
//     offers every draw to them (effects/shadow_casters.hpp).
//   * alpha to coverage (RendererState::alpha_to_coverage): alpha-tested (GREATER / GREATEREQUAL), non-blended draws
//     into a multisampled target: BGFX_STATE_BLEND_ALPHA_TO_COVERAGE and fxCoverageAlpha() instead of the discard.
//   * wet surfaces (Settings::wet_surfaces, fx/wetness.hpp): in the rain, lit and baked draws into the game picture
//     outdoors (not characters or sails) get wet where the visibility volume shows the sky above them (dry under roofs):
//     darker and glossier, their relief filled, the overcast sky reflected, puddles in the relief's hollows with 2
//     (shaders/include/ff_wet.sh, u_fxWet). The wet gloss drives the gloss highlights (sun, lamps with their shadows,
//     dynamic lights), so wet baked draws take the specular variant where there is one. Switched per draw by
//     u_fxWet[0].x.
//   * aerial perspective (kLightingAerial): every draw through the fixed-function vertex pipeline whose fog is the
//     game's exp fog takes fog mode 5 (ff_finish.sh, fx_aerial.sh fxAerialFog): the same fog colour and density with
//     height haze and desaturation, for lit, baked and unlit draws and ported pixel shaders alike. Draws with a ported
//     vertex shader keep the fixed-function fog (reported once); without weather (interiors) the game's fog stays.
// Cube-mapped draws (fs_ff_cube*) have no lighting or coverage variant: they keep the legacy path, reported once.
#pragma once

#include "effects/ao_volume.hpp"
#include "effects/visibility.hpp"
#include "effects/casters.hpp"
#include "effects/shadow_casters.hpp"
#include "effects/lamp_shadows.hpp"
#include "effects/normal_maps.hpp"
#include "effects/parallax_probe.hpp"
#include "effects/shadows.hpp"
#include "effects/town_light.hpp"
#include "storm/fx/lighting.hpp"
#include "storm/fx/settings.hpp"
#include "storm/fx/sky_model.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace storm::shim
{
struct Device;
struct LayoutInfo;
} // namespace storm::shim

namespace storm::effects
{

// What one fixed-function draw does differently under the effects.
// World-space bounding sphere of the vertices a draw covers (shim::Device::draw_bounds): the lights are chosen
// for it, so the choice depends on where the geometry is, never on the camera.
struct DrawSphere
{
    fx::Vec3 center{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    bool valid = false;
};

// A draw's sphere computed when prepare() first needs it (perf audit finding 9: walking the vertices of every dynamic
// lit draw cost 12-14 % of the engine's CPU at sea, where most draws choose no point lights).
// The vertices a draw reaches, in its vertex buffer's CPU copy (shim::Device::draw_vertices): `count` vertices of
// `stride` bytes from `data`; empty when they cannot be read.
struct DrawVertices
{
    const uint8_t *data = nullptr;
    uint32_t stride = 0, count = 0;
};

class DrawSphereSource
{
  public:
    using Compute = DrawSphere (*)(const void *context);
    using Vertices = DrawVertices (*)(const void *context);
    DrawSphereSource(Compute compute, const void *context, Vertices vertices = nullptr)
        : compute_(compute), context_(context), vertices_(vertices)
    {
    }
    DrawVertices vertices() const { return vertices_ ? vertices_(context_) : DrawVertices{}; }
    const DrawSphere &get() const
    {
        if (!computed_)
        {
            sphere_ = compute_(context_);
            computed_ = true;
        }
        return sphere_;
    }

  private:
    Compute compute_;
    const void *context_;
    Vertices vertices_;
    mutable DrawSphere sphere_;
    mutable bool computed_ = false;
};

// The lit and baked program families (renderer_bgfx/CMakeLists.txt ff_families: the same names in the same order).
enum class FfFamily : uint8_t
{
    Lit,        // lit draws without lamps or the AO volume
    LitLamps,   // lit draws lit by the place's lamps or in the AO volume (the room light)
    Baked,      // baked colours without lamps or the AO volume (a town's walls)
    BakedLamps, // baked colours relit by the lamps or in the AO volume
};
inline constexpr size_t kFfFamilies = 4;
// The product blocks of ff_fragment.sh a draw reads (ChooseFamily) or a family's programs compile (FamilyBlocks).
struct FfBlocks
{
    bool lamps = false;     // FX_FF_LAMPS (with the room light)
    bool ao = false;        // FX_FF_AO
    bool town_wall = false; // FX_FF_TOWN_WALL
    bool wet = false;       // FX_FF_WET
};
FfBlocks FamilyBlocks(FfFamily family);
const char *FamilyName(FfFamily family); // "lit_lamps": the programs are fs_ff_<name>[_<s><n|np><h>]
// The narrowest family of a lit (D3DRS_LIGHTING) or baked draw whose programs hold every block in `needs`.
FfFamily ChooseFamily(bool lit, const FfBlocks &needs);

struct AggregateUpload
{
    fx::AggregatePayload payload;
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    uint64_t used_frame = 0; // Lighting's frame that last drew with it
    ~AggregateUpload() { if (bgfx::isValid(texture)) bgfx::destroy(texture); }
};

struct FfDraw
{
    bool active = false;      // a variant below replaces the legacy program
    uint32_t vs_id = 0, fs_id = 0;
    uint8_t vertex_diffuse_off = 0;  // enabled-light slots (order of apply_ff_uniforms) lit per pixel: no diffuse
    uint8_t vertex_specular_off = 0; // ... and no specular in the vertex sum
    bool aggregate_fallback = false;
    bool hemisphere_ambient = false; // ambient suppression that survives a room-light fallback
    bool vertex_ambient_off = false; // the global ambient is the per-pixel hemisphere
    uint64_t state = 0;              // extra bgfx state bits (alpha to coverage)
    // fragment uniforms, set by submit()
    bool lit = false; // FX_FF_LIT / FX_FF_BAKED program: u_fxLight, u_fxPoint, u_fxFf
    FfFamily family = FfFamily::Lit; // the program's family: submit() binds the uniforms of its blocks
    bool shadow_variant = false;     // the program reads the sun's maps (FX_LIGHT_SHADOW): `shadow` or unshadowed
    fx::LightingUniforms light{};
    fx::PointLightUniforms points{};
    std::shared_ptr<const AggregateUpload> aggregate;
    std::array<float, fx::kMaxShadowLamps> aggregate_lamps{};
    std::array<fx::AggregateRef, fx::kMaxShadowLamps> aggregate_lamp_overrides{};
    std::array<float, 4> ff{};
    std::array<float, 4> coverage{};
    bgfx::TextureHandle normal_map = BGFX_INVALID_HANDLE; // FX_LIGHT_NORMALMAP variant: bound at stage 14
    uint32_t normal_map_flags = 0;
    // The material texture at stage kMaterialStage (invalid: none): the relief's (R height, G gloss) when the draw takes
    // the parallax or its texture's relief has one, else stage 0's gloss map (R flat). u_fxGloss (ff_fragment.sh: x a
    // gloss is bound, y how much the texture colour tints the highlight, z debug view, w 1 = the gloss highlights of the
    // lamps and of baked surfaces are on); u_fxParallax[2] for the FX_LIGHT_PARALLAX variants.
    bgfx::TextureHandle material = BGFX_INVALID_HANDLE;
    uint32_t material_flags = 0;
    std::array<float, 4> gloss{};
    std::array<float, 8> parallax{};
    // u_fxSkinTone (ff_fragment.sh ffSkinLikeness): a character's skin colour for the torso test (fx/skin_tone.hpp
    // PackSkinTone, [1].w the lod the test reads); zero: no test (not a character, no reference).
    std::array<float, 8> skin_tone{};
    // Preserve sun suppression when only point/compound lighting falls back; the upgraded sun still runs.
    void use_unmerged_lights(uint8_t directional_mask)
    {
        aggregate_fallback = true;
        lamps = room_light = false;
        vertex_ambient_off = hemisphere_ambient;
        if (!hemisphere_ambient) // the flat ambient of the room light's sky share returns to the vertex sum
            for (int i = 0; i < 3; ++i)
                light.u_fxLight[3][i] = light.u_fxLight[4][i] = 0.0f;
        vertex_diffuse_off &= directional_mask;
        vertex_specular_off &= directional_mask;
        points = {};
        aggregate.reset();
        aggregate_lamps = {};
    }
    bool parallax_variant = false; // the FX_LIGHT_PARALLAX variant: u_fxParallax is set
    // The parallax variant also carries cavity-only draws. Count UV displacement only when it is enabled.
    bool has_parallax() const { return parallax_variant && parallax[2] > 0.0f && parallax[0] > 0.0f && parallax[1] > 0.0f; }
    bool parallax_untrusted = false; // ... but the texture's mask or a list takes the parallax from it (the cavity still draws)
    const NormalMapEntry *relief_entry = nullptr; // the relief texture's entry (its map ready or not): the parallax probe
    bool shadow = false;           // receives the sun's shadow maps: submit() / submit_ground() bind them for `view`
    // A plain fixed-function draw that receives the moving casters' sun shadows. Not `active`: the shim picks
    // vs_ff[_unlit]_dyn / fs_ff_dyn and calls submit_ground(). `active` (alpha to coverage): vs_id / fs_id are the
    // ground variant's coverage twin and submit() binds the maps.
    bool ground = false;
    fx::SunReceiver sun_receiver{}; // how it receives (fx/shadows.hpp)
    std::array<float, 16> view{};  // the draw's device world -> view (the sun's layers' receivers)
    // Absolute world -> view: the device's world is camera-relative (the game's Renderer moves the camera translation
    // into every world matrix and light: set_world_origin); the effects' own world data (AO volume, lamps and their
    // atlases, the location's geometry) is absolute.
    std::array<float, 16> world_view{};
    std::array<float, 3> origin{}; // absolute = device world + origin
    bool ao = false;               // samples the interior's AO volume (submit() binds it for `view`)
    bool lamps = false;            // lit by the place's lamps with their shadows (submit() binds them for `view`)
    bool room_light = false;       // its ambient is the room light of the interior
    bool town_wall = false;        // a town's wall relit by our sun and sky (u_fxTownWall)
    std::array<std::array<float, 4>, 2> wet{}; // u_fxWet (ff_wet.sh); x of the first = 0: the draw stays dry
};

// The rain's wetness of this frame for the draws outdoors (effects::Host from the weather, fx::Wetness).
struct WetFrame
{
    float level = 0.0f;         // 0 dry (or no weather: interiors) .. 1 soaked
    std::array<float, 3> sky{}; // the sky the wet surfaces reflect: the weather's fog colour (its horizon), darker at night
};

class Lighting
{
  public:
    // lamp_depth: Support::lamp_depth (the format of the lamps' shadow faces).
    void configure(const fx::Settings &settings, const fx::RendererState &state, fx::LampDepth lamp_depth);
    bool aggregate_available() const;
    // Creates the fixed-function programs the draws of `settings` can pick (lit / baked variants with and without
    // relief and shadow, their skinned twins, coverage, shadow casters), at most `budget` of them (reduced by the
    // number created; each is a shader link on the GPU driver). True once all exist. A settings switch calls it
    // while the previous settings still draw (Host::advance_switch).
    bool prepare_programs(shim::Device &device, const fx::Settings &settings, int &budget);
    // Nothing to decide per draw (preset «Низкая»): the shim skips prepare().
    bool idle() const { return !lighting_ && !coverage_ && !shadows_.enabled(); }

    // Fixed-function draw (no shader ports). `cube_variant` = the fs_ff_cube* variant the shim picked (0 = none).
    void prepare(const shim::Device &device, const shim::LayoutInfo &layout, uint32_t cube_variant,
                 const DrawSphereSource &sphere_source, FfDraw &draw);
    void submit(const FfDraw &draw);
    // A plain fixed-function draw with FfDraw::ground: the sun map and its uniforms.
    void submit_ground(const FfDraw &draw);
    // The plain program pair of a draw with FfDraw::ground (`vertex_lit`: D3DRS_LIGHTING on).
    uint32_t ground_vs(bool vertex_lit)
    {
        find_ground_shaders();
        return vs_ground_[vertex_lit ? 0 : 1];
    }
    uint32_t ground_fs()
    {
        find_ground_shaders();
        return fs_ground_;
    }
    // Fog of a draw with fog enabled in D3DFOG_EXP mode (the game's): true when it takes the aerial perspective (fog
    // mode 5); u_fxAerialUp is then set for that draw and `haze` goes where fog mode 5 reads it (the fog start and end
    // slots of u_ffFs, unused by exp fog). `view`: the draw's world -> view.
    bool aerial_fog(const D3DMATRIX &view, bool vs_port, std::array<float, 2> &haze);

    void set_technique(const char *name);
    // The frame's dynamic point lights (not in any baked colours, absolute world); kept until a frame passes without
    // them.
    void set_dynamic_lights(std::span<const fx::PointLight> lights)
    {
        dynamic_lights_.assign(lights.begin(), lights.end());
        dynamic_lights_this_frame_ = true;
        device_lights_origin_valid_ = false;
    }
    // Where the device's camera-relative world has its origin in absolute world (the camera position the game's
    // Renderer subtracted from every world matrix and D3D light; render::SetEffectsWorldOrigin).
    void set_world_origin(const std::array<float, 3> &origin)
    {
        if (origin != origin_)
            device_lights_origin_valid_ = false;
        origin_ = origin;
    }
    const std::array<float, 3> &world_origin() const { return origin_; }
    void set_sky(const fx::SkyState *sky) { sky_ = sky; }
    // The weather's sun (towards it, world; its colour; above the horizon): the highlight of glossy baked surfaces
    // outdoors, whose draws have the fixed-function lights off and so no sun of their own.
    void set_weather_sun(const std::array<float, 3> &dir, const std::array<float, 3> &color, bool present)
    {
        weather_sun_ = {dir, color, present};
    }
    void set_wet(const WetFrame &wet) { wet_ = wet; }
    const WetFrame &wet() const { return wet_; }
    // The interior's AO volume for this frame's draws into the game picture (null: none). Once per frame. A volume
    // arriving where there was none fades in over kVolumeFade (one made after the reveal does not pop in).
    void set_ao_volume(const AoVolumes::Bound *volume);
    // This frame's share of the AO / visibility volume while it fades in (1 once it is whole, 0 without one).
    float ao_weight() const { return ao_weight_; }
    float visibility_weight() const { return visibility_weight_; }
    // The location's visibility volume for this frame's lit and baked draws (null: none yet, or no location). Once
    // per frame; fades in like the AO volume.
    void set_visibility(const Visibility::Bound *volume);
    NormalMaps &normal_maps() { return normal_maps_; }
    int prepare_scene_texture(shim::TexStore &store, bool *inference_pending = nullptr) {
        return normal_maps_.prepare_scene(store, fx_, inference_pending);
    }
    ParallaxProbe &probe() { return probe_; }
    SunShadows &shadows() { return shadows_; }
    TownLight &town_light() { return town_light_; }
    // After TownLight::update(): the hour's sun on our lightmaps (the place's sun map, the lightmaps' compose).
    void relight_town(shim::Device &device) { town_light_.render(device, shadows_, casters_); }
    // The shadow volumes of the frame for the picture the device is drawing now: on the first call of a frame the
    // lights prepare their maps for the device's camera and publish them (effects/shadow_casters.hpp). False: this
    // is not the game picture, or no light has a shadow map this frame.
    bool shadow_volumes(shim::Device &device);
    ShadowCasters &shadow_casters() { return shadow_casters_; }
    Casters &casters() { return casters_; }
    // A draw under these states is lit by the frame's sun and carries no location lightmap: what the sun's dynamic
    // layer takes (and what receives it).
    bool sun_lit(const shim::Device &device) const;
    // u_fxSkinTone of a character atlas (FfDraw::skin_tone); bounded partial draws accumulate its reference from the
    // vertices of that draw (the GPU copies of GEOS::AVERTEX0, model gpu_skinning.hpp).
    std::array<float, 8> skin_tone(shim::TexStore &atlas, const DrawSphereSource &draw_source);
    LampShadows &lamp_shadows() { return lamps_; }
    const LampShadows &lamp_shadows() const { return lamps_; }
    // Right before every bgfx::frame() (Context::set_before_frame): frame uniforms that depend on the frame's casters.
    void before_frame() { lamps_.before_frame(); }
    void end_frame();
    // A textured draw of `texture` for the parallax census: it drew with the parallax (kCoverageParallax) or missed it
    // for a ParallaxMiss. census() calls it; public for tests/renderer_bgfx/relief_switch_check.cpp.
    void cover(const shim::TexStore &texture, fx::ReliefClass relief, uint8_t status);
    static constexpr uint8_t kCoverageParallax = 0xff;
    static constexpr uint32_t kCoverageFrames = 300; // the census window
    // Why a textured draw drew without the parallax (census()).
    enum class ParallaxMiss : uint8_t
    {
        NoReliefClass, // the texture's class has no relief: what the parallax cannot work on (its rule says which)
        Ported,        // a ported vertex or pixel shader of the game's technique
        Decal,         // a slope-biased decal
        Pretransformed, // RHW vertices
        NoNormals,     // no vertex normals: no per-pixel lighting variant
        Cube,          // a cube-mapped stage: the legacy path
        Character,
        Off,           // relief or parallax switched off
        Stage1,        // the stage 1 grey height texture's relief took the draw, without a height of its own
        NotReady,      // the map not uploaded yet (being made, read, faded in)
        NoHeight,      // the map has no height (flat)
        Projected,     // the relief's stage with projected coordinates
        Clamped,       // the relief's stage clamp-addressed
        NoRelief,      // the draw took no relief: a lit material without it (props, hull parts), or fading in at 0
        NotTrusted,    // the texture's class takes no parallax (not masonry), its trust coverage is below kParallaxMinCoverage, or a list excludes it
        Other,
    };
    static const char *ParallaxMissName(ParallaxMiss miss);
    // After prepare() (or instead of it, for a draw prepare() never sees): counts the draw for the census.
    void census(const shim::Device &device, const shim::LayoutInfo &layout, bool ported, bool decal, uint32_t cube_variant,
                const FfDraw &draw);
    static constexpr size_t kParallaxMisses = 16;
    // Draws of the last presented frame that used a lit / baked / coverage variant (effects profile).
    struct Counts
    {
        uint32_t lit = 0, baked = 0, coverage = 0, relief = 0, parallax = 0, glossy = 0, shadowed = 0, ground = 0, aerial = 0,
                 ao = 0, lamp_lit = 0, town_walls = 0, wet = 0;
        // Textured draws that drew without the parallax, by the reason (ParallaxMiss), so a parallax that never
        // reaches the screen shows in the "effects parallax:" line.
        std::array<uint32_t, kParallaxMisses> parallax_miss{};
        uint32_t aggregate_uploads = 0, aggregate_bytes = 0, aggregate_members = 0, aggregate_fallbacks = 0;
        uint32_t gloss_yields = 0; // draws whose stage 0 gloss gave unit 7 to a stage 1 parallax
    };
    Counts last_frame() const { return last_frame_; }

    void shutdown();
    void on_device_lost();

  private:
    enum class Material : uint8_t
    {
        Hull,
        Stone,
        Sail,
        Character,
    };

    // Immutable snapshots: an upload stays valid while the frames keep drawing with the same refs; one not used in
    // a frame is released at its end (Lighting::end_frame).
    std::vector<std::shared_ptr<AggregateUpload>> aggregate_cache_;
    uint64_t aggregate_frame_ = 1;
    bgfx::UniformHandle s_aggregate_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle aggregate_open_ = BGFX_INVALID_HANDLE;
    bool aggregate_failed_ = false;
    uint8_t aggregate_reported_ = 0; // one log line per fallback reason (bits of report_aggregate_fallback)
    void report_aggregate_fallback(uint8_t reason_bit, const char *text);
    std::shared_ptr<const AggregateUpload> aggregate_upload(std::span<const fx::AggregateRef> refs);
    void prepare_aggregates(FfDraw &draw, const fx::LightSelection &selection, uint8_t directional_mask = 0);
    fx::Settings settings_{};
    uint32_t flags_ = 0;
    bool lighting_ = false; // per-pixel sun or point lights
    bool gloss_ = false;    // gloss maps on (Settings::gloss with per-pixel specular)
    bgfx::UniformHandle u_gloss_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_skin_tone_ = BGFX_INVALID_HANDLE;
    bool coverage_ = false;
    int budget_ = 0;
    Material material_ = Material::Hull;
    bool technique_set_ = false;
    std::vector<fx::PointLight> dynamic_lights_;        // absolute world
    std::vector<fx::PointLight> device_dynamic_lights_; // the same in the device's camera-relative world
    std::vector<fx::PointLight> candidate_lights_; // ... those a baked draw may take (scratch of prepare())
    bool device_lights_origin_valid_ = false;
    std::array<float, 3> origin_{};
    bool dynamic_lights_this_frame_ = false;
    const fx::SkyState *sky_ = nullptr;
    struct WeatherSun
    {
        std::array<float, 3> dir{0.0f, 1.0f, 0.0f}, color{};
        bool present = false;
    } weather_sun_;
    WetFrame wet_{};
    bgfx::UniformHandle u_wet_ = BGFX_INVALID_HANDLE;
    void find_shaders();

    uint32_t vs_ff_ = 0, vs_lit_ = 0, vs_lit_skin_ = 0, fs_coverage_ = 0;
    // [family][specular][relief: none, normal map, normal map + parallax][sun shadow: the preset's, not the draw's]
    uint32_t fs_[kFfFamilies][2][3][2] = {};
    uint32_t vs_ground_[2] = {}, fs_ground_ = 0, fs_ground_coverage_ = 0; // FfDraw::ground: [lit per vertex, unlit]
    void find_ground_shaders();
    NormalMaps normal_maps_;
    ParallaxProbe probe_;
    SunShadows shadows_;
    TownLight town_light_;
    LampShadows lamps_;
    Casters casters_; // the caster programs and material rule of the sun's layers and the lamp atlases
    ShadowCasters shadow_casters_; // the frame's shadow volumes and the casters collected into them
    bool shadow_volumes_prepared_ = false; // shadow_volumes() asked the lights this frame
    bool cube_reported_ = false;
    bool bounds_reported_ = false;
    bool aerial_port_reported_ = false;
    bgfx::UniformHandle u_aerial_up_ = BGFX_INVALID_HANDLE;
    fx::Lighting fx_;
    bgfx::UniformHandle u_ff_ = BGFX_INVALID_HANDLE;
    // AO volume (ff_fragment.sh ffAoVolume): sampler stage kAoVolumeStage, view -> volume uvw, parameters
    void bind_ao_volume(const FfDraw &draw);
    const AoVolumes::Bound *ao_volume_ = nullptr;
    float ao_weight_ = 0.0f, visibility_weight_ = 0.0f; // 0..1 while a volume fades in (kVolumeFade)
    std::chrono::steady_clock::time_point ao_since_{}, visibility_since_{};
    bgfx::UniformHandle s_ao_volume_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_ao_matrix_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_ao_params_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle ao_open_ = BGFX_INVALID_HANDLE; // 1x1x1 RGBA8 = 255 for draws without the volume
    // Visibility volume (ff_fragment.sh ffVisibility): sampler stage kVisibilityStage, view -> volume uvw, x on
    void bind_visibility(const FfDraw &draw);
    // The town walls' relight (ff_fragment.sh, u_fxTownWall[0].w = 0: off for this draw).
    void bind_town_wall(const FfDraw &draw);
    bgfx::UniformHandle u_town_wall_ = BGFX_INVALID_HANDLE;
    const Visibility::Bound *visibility_ = nullptr;
    bgfx::UniformHandle s_visibility_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_visibility_matrix_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_visibility_params_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle visibility_open_ = BGFX_INVALID_HANDLE; // 1x1x1: sky 1, open 1
    std::array<float, 16> visibility_view_{}, visibility_view_to_uvw_{}, visibility_world_to_uvw_{};
    std::array<float, 16> ao_view_{};     // the camera ao_view_to_uvw_ was made for
    std::array<float, 16> ao_world_to_uvw_{}; // the room volume ao_view_to_uvw_ was made for
    std::array<float, 16> ao_view_to_uvw_{};
    bgfx::UniformHandle u_coverage_ = BGFX_INVALID_HANDLE;
    Counts frame_{}, last_frame_{};
    uint32_t census_frames_ = 0, logged_parallax_ = 0;
    std::array<uint32_t, kParallaxMisses> logged_miss_{};
    // The parallax's coverage by texture (census()): each texture drawn, its class, whether it drew with the parallax
    // (kCoverageParallax) or why not (a ParallaxMiss), the frame it was last drawn. The "effects parallax coverage:"
    // table counts the textures drawn in the last kCoverageFrames frames. Keyed by TexStore::serial; a texture's
    // status is logged once per window (log_coverage), with every reason it drew with in that window.
    struct Coverage
    {
        std::string name;
        uint32_t seen = 0;   // this window: bit per ParallaxMiss drawn with, bit 31 the parallax
        uint32_t logged = 0; // `seen` of the last line logged for it
        fx::ReliefClass relief = fx::ReliefClass::None;
        uint8_t status = 0;
        const char *rule = nullptr; // a texture without relief: the rule that says so
        uint32_t frame = 0;
        uint32_t parallax_frame = 0; // the last frame it drew with the parallax (0: never)
    };
    std::unordered_map<uint64_t, Coverage> parallax_coverage_;
    uint32_t census_frame_ = 1;
    std::string logged_coverage_;
    void log_texture(Coverage &c);
    void log_coverage();
};

} // namespace storm::effects
