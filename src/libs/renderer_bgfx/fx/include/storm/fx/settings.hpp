// Runtime switches of the graphics effects package.
//
// Every field may be changed between frames; the passes read the struct each
// frame and (re)create GPU resources lazily. `QualityPreset::Low` turns
// every effect off: the post chain degenerates to a plain copy and the legacy
// sea / sky / fixed-function lighting paths are used unchanged.
#pragma once

#include "storm/fx/light_falloff.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace storm::fx
{

// Sampler stages fx binds into the fixed-function emulator's programs (FX_LIGHT_SHADOW, FX_LIGHT_NORMALMAP).
// They are above the emulator's own units (stages 0..4, cube maps 5..7) except 10..12: the shim also maps volume
// textures (8..10, 12) and a stage-3 cube (11) there, which only ported pixel shaders read; the fx lit / baked
// variants never run a port and bind their own textures to those units after the shim, so every combination fits
// in the 16 units of GLES 3.0.
inline constexpr uint8_t kShadowMapStage = 15; // 13 is the renderer's bone palette (vertex shader)
// The location's visibility volume (fx/visibility.hpp): the unit of the stage 0 cube map, which no lit or baked
// variant has (a cube-mapped fixed-function draw keeps vertex lighting: Lighting::prepare).
inline constexpr uint8_t kVisibilityStage = 5;
inline constexpr uint8_t kAoVolumeStage = 12;    // the interior's AO volume (RGBA8 3D, four lobes)
inline constexpr uint8_t kLampDynamicStage = 10; // lamp shadow atlases (characters / location)
inline constexpr uint8_t kLampStaticStage = 11;
inline constexpr uint8_t kRoomLightStage = 8;    // the baked colours around every point of an interior (RGBA8 3D)
inline constexpr uint8_t kLampGridStage = 9;     // which lamps reach which cell of the place (RGBA8 3D)
inline constexpr uint8_t kNormalMapStage = 14;
// The material texture (EAC RG11: R the relief's height for the parallax and its cavity, height_map.hpp; G gloss,
// gloss_map.hpp) in the lit and baked programs: unit 7 is the stage-2 cube of the plain fixed-function cube variants,
// which those programs never are (cube-mapped draws keep the fixed-function path).
inline constexpr uint8_t kMaterialStage = 7;
// The point lights' and lamps' aggregate payload (fx_lighting.sh s_fxAggregate): the unit of the stage 1 cube map.
inline constexpr uint8_t kAggregateStage = 6;
// Every unit above, with the emulator's own stages 0..4 (and the renderer's bone palette at 13, shim/device.hpp),
// is claimed once: src/effects/lighting.cpp checks at compile time that no two claims meet (FfUnitsDistinct).
inline constexpr uint8_t kFxStages[] = {kVisibilityStage, kAggregateStage, kMaterialStage, kRoomLightStage, kLampGridStage,
                                        kLampDynamicStage, kLampStaticStage, kAoVolumeStage, kNormalMapStage, kShadowMapStage};

enum class QualityPreset : uint8_t
{
    Low,    // «Низкая»: the legacy look, no effect runs (parity with the original renderer); weak phones
    Medium, // «Средняя»: Mali-G52 / Adreno 610 class at 30 fps
    High,   // «Высокая»: Adreno 640+ / Mali-G76+ class
};

// Colour grade, independent of the quality preset.
enum class GradePreset : uint8_t
{
    Original,  // «Оригинал»: identity look-up table
    Caribbean, // «Карибы»: warm, saturated
    Overcast,  // «Пасмурно»: cool, low saturation, softer contrast
};

// Bits understood by shaders/include/fx_lighting.sh (see lighting.hpp).
enum LightingFlags : uint32_t
{
    kLightingNone = 0,
    kLightingPerPixel = 1u << 0,   // FX_LIGHT_PER_PIXEL: sun N·L evaluated per pixel
    kLightingHemisphere = 1u << 1, // FX_LIGHT_HEMISPHERE: sky/ground ambient instead of a flat one
    kLightingSpecular = 1u << 2,   // FX_LIGHT_SPECULAR: Blinn-Phong highlight
    kLightingTranslucent = 1u << 3, // FX_LIGHT_TRANSLUCENT: back-lit thin surfaces (sails, flags)
    kLightingPoint = 1u << 4,       // FX_LIGHT_POINT: point and spot lights evaluated per pixel
    kLightingNormalMap = 1u << 5,   // FX_LIGHT_NORMALMAP: relief from normal maps derived from the game's textures
    kLightingRim = 1u << 6,         // FX_LIGHT_RIM: rim light on characters and ships seen against the sun
    kLightingLinear = 1u << 7,      // FX_LIGHT_LINEAR: lights are added in linear light instead of gamma space
    kLightingAerial = 1u << 8,      // FX_AERIAL: aerial perspective and height fog instead of the plain fog
    kLightingWind = 1u << 9,        // FX_WIND: wind sway in the vertex shader (foliage, grass, sails)
    kLightingAll = 0x3ff,
};

// Parallax from the relief's height (Settings::parallax, height_map.hpp) on the classes that have one.
enum class ParallaxQuality : uint8_t
{
    Off,
    Offset,    // one tap: offset mapping with offset limiting
    Occlusion, // parallax occlusion mapping: 8 steps looking straight at a surface, up to kParallaxSteps at grazing
};
inline constexpr int kParallaxSteps = 24;

// The relief of the characters (Settings::character_relief): their clothes, armour and gear, faded out on skin and hair
// by the skin weight of the vertices (the head, neck and hand bones: animation skin_bones.hpp).
enum class CharacterRelief : uint8_t
{
    Off,
    Normals,  // the normal maps only
    Parallax, // and a shallow parallax (Settings::parallax's quality, no self-shadow)
};

enum class ShadowQuality : uint8_t
{
    Off,
    Low,  // 1024² map, one hardware-filtered tap
    Soft, // 2048² map, four taps
};

// The occlusion of the interior AO volume against its level's own (Settings::ao_gain).
inline constexpr float kAoGain = 1.25f;
// Strengths of the interior ambient-occlusion volume (Settings::ao_volume, option fx_ao_volume), for the owner's
// choice of the interior contrast. The shader darkens by mix(1, pow(ao, contrast), strength), where ao is the baked
// visibility relative to an open floor or wall (1 there, lower under tables, behind barrels, in corners).
inline constexpr uint8_t kAoVolumeLevels = 3;
struct AoVolumeLevel
{
    const char *name;
    float strength;
    float contrast;
};
inline constexpr AoVolumeLevel kAoVolumeLevel[kAoVolumeLevels + 1] = {
    {"off", 0.0f, 1.0f},
    {"as the game's data", 0.6f, 1.0f},
    {"moderate contrast", 0.85f, 1.4f},
    {"strong stylised contrast", 1.0f, 1.7f},
};

// The presets' internal scene heights (Settings::render_height): non-decreasing from «Низкая», the fastest.
inline constexpr uint16_t kLowRenderHeight = 900;
inline constexpr uint16_t kMediumRenderHeight = 1000;
inline constexpr uint16_t kHighRenderHeight = 1100;

struct Settings
{
    // ---- post chain ----
    bool tonemap = false;       // AgX tone curve on the luminance (fx_tonemap.sh)
    float exposure = 1.0f;      // linear multiplier before the tone curve
    float tonemap_strength = 0.6f; // 0 = untouched .. 1 = the full AgX curve
    GradePreset grade = GradePreset::Original;
    float grade_strength = 1.0f; // 0 = identity .. 1 = full preset
    bool bloom = false;
    uint8_t bloom_downscale = 4;  // 2 = bloom chain starts at half, 4 = at quarter resolution
    uint8_t bloom_levels = 4;     // dual-filter down/up steps (1..6)
    float bloom_threshold = 0.85f; // linear value of the brightest channel above which a pixel glows
    float bloom_knee = 0.12f;     // glow fades in from threshold * (1 - knee) and is full at white
    float bloom_intensity = 0.35f;
    bool smaa = false;            // SMAA 1x (post anti-aliasing), where MSAA is unavailable (ConstrainSettings)
    // The 3D scene's internal height in pixels on displays taller than this (FSR 1 EASU up to the display, the
    // game's interfaces drawn at the display size after it); a display this tall or smaller renders natively, so a
    // small phone is not scaled for nothing. 0 = native everywhere. RenderScale() gives the factor for a display.
    uint16_t render_height = 0;
    // Far LODs of islands, ships and outdoor locations (src/libs/geometry/far_lod): the largest error a simplified
    // level may show, in pixels of the target drawn; 0 = every model drawn whole. Not a look: on every preset.
    float far_lod_pixels = 0.f;
    bool sharpen = false;         // FSR 1 RCAS (robust contrast-adaptive sharpening) after the EASU upscale
    float sharpness = 0.5f;       // 0..1: RCAS strength, 1 = the reference's maximum (0 stops), 0 = 2 stops softer
    bool sun_shafts = false;      // high end only
    float sun_shafts_intensity = 0.35f;
    bool dither = false;          // +-0.5/255 ordered noise on the sky and the final image: no banding in gradients
    bool lens_flare = false;      // procedural sun glare and flare ghosts (switch the legacy flare sprites off)
    float lens_flare_intensity = 0.5f;
    bool scene_grading = false;   // PostChain::Frame::scene_grade (mood of the place and hour) is applied

    // ---- renderer state (the renderer applies these; see RendererState()) ----
    uint8_t msaa = 1;             // samples of the scene target: 1 (off), 2 or 4. The primary anti-aliasing on tile GPUs
    bool alpha_to_coverage = false; // alpha-tested geometry (grass, foliage, rigging) gets smooth edges; needs msaa > 1
    uint8_t anisotropy = 1;       // 1 (off), 4 or 8: anisotropic filtering of world textures

    // ---- world passes ----
    // The game's own sea (the ported Sea2 technique, shaders/ports/fs_port_sea2.sc) with improvements on top; the
    // reflected ray is clamped to the horizon always, these are optional:
    bool sea_band_limit = false;  // the bump detail fades with the pixel footprint: no shimmer towards the horizon
    bool sea_soft_edges = false;  // the sea fades out where shallow in front of the scene (needs the scene depth)
    bool sea_turbidity = false;  // depth-dependent transmission and water-colour scattering
    bool sea_specular = false;    // the sun's glint from the bumped normal along the game's own sun road (Средняя, Высокая)
    float sea_edge_depth = 1.5f;  // metres of water over which a soft edge fades in
    float sea_specular_power = 400.0f;
    float sea_specular_strength = 0.6f;
    // Waves from objects (sea_waves.hpp): ripples of a wave-equation height field around the camera, stirred by
    // swimmers, waders, boats and ship hulls, in the normals and crest foam of the sea; `sea_waves_lift` also raises
    // the sea's vertices by them (vertex texture fetch). A ship's wake is what its hull leaves in the field.
    bool sea_waves = false;
    bool sea_waves_lift = false;
    uint8_t sea_waves_debug = 0; // 1: the sea shows the ripple slope (red), crest foam (blue);
                                 // 2: the tilt the waves add to its normal (x red, z blue, none grey)
    uint32_t lighting_flags = kLightingNone;
    uint8_t point_lights = 4;      // per draw call with kLightingPoint: 4 or 8
    // Flash lights of shots and explosions (light_set.hpp FlashLights): how many light at once, 0..8 (needs
    // kLightingPoint). Each draw gives them at most half of its point lights.
    uint8_t flash_lights = 0;
    // With kLightingNormalMap: strength of all relief (lit models and baked surfaces alike; texture classes keep
    // their ratios). 1.0 keeps the generated normals, while Medium and High soften their lighting contrast.
    float normal_map_strength = 1.0f;
    // Saturation of added per-pixel light, preserving luminance; albedo and baked colours stay untouched.
    float light_saturation = 1.0f;
    // With kLightingTranslucent: strength of the sunlight through sails and flags (scales LightingMaterial::translucency).
    float sail_light = 1.0f;
    uint16_t normal_map_size = 512;   // with kLightingNormalMap: larger side of the resident normal maps (EAC RG11 + mips)
    // With kLightingSpecular: the highlights follow per-texel gloss maps derived from the colour textures
    // (gloss_map.hpp): the sun's and the lamps' on lit and baked surfaces alike. Off: the lit materials' fixed highlight
    // only (no lamp or baked highlights).
    bool gloss = false;
    // Neural ×2 textures (upscale.hpp, renderer src/effects/upscale.hpp): the relief classes' colour textures and the
    // characters' drawn near the camera take PBRify's ×2 made on the device, within a GPU budget from the device's RAM.
    // No preset sets it: the texture quality «Ультра» does (ApplyTextureQuality).
    bool upscale = false;
    uint16_t upscale_budget_mb = 0; // with upscale: their GPU memory, 0 = UpscaleBudgetBytes of the device's RAM
    float gloss_strength = 1.0f;   // with gloss: the share of the gloss maps' highlights (0..3), 1 = the chosen look
    uint16_t gloss_map_size = 256; // with gloss: larger side of the resident gloss maps (EAC RG11 + mips)
    // 0 off; 1 the gloss of every lit or baked pixel as grey (magenta: no gloss map, the material's own highlight);
    // 2 the relief: the shading normal of the pixels whose relief is on, grey elsewhere; 3 the share of the open sun the
    // bake shows (its highlight's mask on baked surfaces); 4 the metal mask; 5 the characters' skin weight (red skin,
    // green relief: ff_fragment.sh ffSkin).
    uint8_t gloss_debug = 0;
    // With kLightingNormalMap: parallax from the relief's height (R of the material texture, at the normal map's size) on
    // stone, wood, plaster, ground and the buildings' atlases, fading out between 10 and 15 m.
    ParallaxQuality parallax = ParallaxQuality::Off;
    // The parallax's depth, scaling every class's own (fx::ReliefDepthTexels in height_map.hpp), 0..3.
    float parallax_strength = 1.0f;
    // With ParallaxQuality::Occlusion: the relief's own shadow towards the sun (six more taps of the height).
    bool parallax_shadow = false;
    // With kLightingNormalMap: the relief's cavity (fx_lighting.sh fxReliefCavity) darkens joints, gaps and recesses from
    // the same height, under the lighting like an occlusion; with or without the parallax.
    bool relief_cavity = false;
    bool parallax_debug = false; // parallax draws show their uv shift (red) and height (green) instead of their colour
    // With kLightingNormalMap: relief on the characters (CharacterRelief), at character_relief_strength of the relief
    // strength (their textures are painted: PBRify's bumps read stronger on them than on stone or wood).
    CharacterRelief character_relief = CharacterRelief::Off;
    float character_relief_strength = 0.5f;
    // With CharacterRelief::Parallax: the characters' parallax depth against their class's (ReliefDepthTexels), 0..10.
    float character_parallax_depth = 1.0f;
    // With character_relief: on the torso and arm bones the texels in the character's own skin colour count as skin
    // (fx/skin_tone.hpp: bare chests and arms lose their relief as faces do).
    bool character_skin_colour = true;
    ShadowQuality shadows = ShadowQuality::Off; // sun shadows on per-pixel lit surfaces (needs kLightingPerPixel)
    // Towns light their lightmapped surfaces with our own bake for the live sun instead of the game's lightmaps
    // (needs kLightingPerPixel; src/effects/town_light.hpp).
    bool town_light = false;
    // With town_light: the town's walls and props without a lightmap relit per pixel the same way (our sun with its
    // shadow, our sky over the game's baked sun and sky; src/effects/town_light.hpp TownWallTone).
    bool town_walls = false;
    // With town_light: our lightmaps have this many texels per side for each of the game's (1, 2, 4): the hour's sun
    // shadows are read that much finer than the game's lightmaps hold them (src/effects/town_light.hpp).
    int town_light_scale = 1;
    bool town_light_debug = false;
    // The location's visibility volume (fx/visibility.hpp: sky visibility, the wet surfaces' shelter) for the lit
    // draws (per-pixel or point lighting); off for A/B comparisons (option fx_visibility).
    bool visibility = true;
    bool visibility_debug = false; // lit and baked surfaces show the location's visibility volume (red sky, green open) // our town lightmaps show their sun visibility (grey) instead of the relit paint
    // Ambient-occlusion volume of interiors (ao_volume.hpp): 0 off, else one of the strengths AoVolumeLevel names.
    uint8_t ao_volume = 0;
    // What the AO volume takes times this (option fx_ao_gain). Over 1: the probes' rays see a place a little more
    // open than the voxel bake before them did (thin things were a voxel thick there), and the screen-space
    // occlusion left the presets, so corners and the floor under furniture keep the depth it gave them. Measured on
    // FortFrance's bank and port office: 1.15 gives the old bake's darkening, 1.25 a little more.
    float ao_gain = kAoGain;
    // The lamps of a location relight it with shadows (lamp_shadows.hpp): how many lamps around the camera take
    // part, 0, 8 or 16. Interiors also light their lit models with the room's own baked light.
    uint8_t lamp_shadows = 0;
    // With lamp_soft: the light (gamma space, per unit of the receivers' strongest gain) under which the strongest lamp
    // takes the 3x3 filter after all: a dim lamp's wider penumbra does not show (0: the 5x5 wherever it lights).
    float lamp_soft_light = 24.0f / 255.0f;
    // The light (gamma space, per unit of the receivers' strongest gain) where a lamp ends: the grid lists it and its
    // faces take casters only where its light (with kLampReachHeadroom for the flicker) is above it.
    float lamp_reach_light = 0.5f / 255.0f; // fx::kLampNegligible
    // The light under which a lamp's light at a pixel is not checked against its shadow (no taps).
    float lamp_shadow_light = 0.5f / 255.0f; // fx::kLampNegligible
    bool lamp_soft = false;  // a 5x5 instead of a 3x3 filter on the lamp shadows of the still geometry, for the
                             // strongest lamp of a pixel (the others take the 3x3)
    // The most lamps that light one pixel with their shadows, strongest of its cell first (1..kLampGridCapacity):
    // the weaker lamps of a crowded cell keep only the light the bake holds of them.
    uint8_t lamp_per_pixel = 8;
    // Of those, how many take filtered shadows (1..kLampGridCapacity): the strongest the 5x5 (lamp_soft) or 3x3
    // filter, the next ones the 3x3; the pixel's other lamps one hardware-filtered tap (2x2 texels).
    uint8_t lamp_filtered = 8;
    // How many of those slots are kept for lights that move (carried lanterns and torches): the nearest to the
    // camera cast shadows, their still faces redrawn whenever they move. 0, 1 or 2.
    uint8_t lamp_moving = 0;
    float lamp_gain = 1.0f;  // the lamps' strength on baked surfaces, 1 = the chosen look (0..3)
    // The share of a lamp's own colour in the light it adds to baked surfaces (0..1); the rest takes the tone of the
    // baked colours, which hold the game's own, much less saturated lamp light.
    float lamp_tint = 0.25f;
    float lamp_baked = 0.25f; // the part of that light the baked colours are taken to hold already (0..1): 1 = lamps
                             // only take light away where they are hidden, less = they also brighten what they reach
    // Indoors the lit models stand this much above the room light around them (0 = at its level): the owner wants
    // the characters to stand out from the room a little.
    float lamp_model_lift = 0.2f;
    uint8_t lamp_debug = 0;  // 1: every lamp slot in its own colour, 2: the shadow mask, 3: the shadow taps per pixel,
                             // 4: the lamps that reach each pixel
    // Rain wetness of the outdoor surfaces where the rain reaches them (wetness.hpp, ff_wet.sh): 0 off, 1 wet (darker,
    // glossier, the sky's reflection; following the rain and drying after it), 2 also puddles in the relief's hollows
    // (the parallax's height). Lit and baked draws (per-pixel lighting); highlights where there is FX_LIGHT_SPECULAR.
    uint8_t wet_surfaces = 0;
    float wet_force = -1.0f; // development: 0..1 = this wetness outdoors whatever the weather, < 0 = the rain's
    // The one falloff of every light of the per-pixel lighting (light_falloff.hpp): how much further than the game's
    // lights they reach, their soft core, their level against the game's (options fx_light_reach, fx_light_core,
    // fx_light_level). The same in every preset: the owner's look.
    LightFalloff light_falloff{};

    static Settings FromPreset(QualityPreset preset);

    // True when the post chain would not change a single pixel (given equal
    // input and output size). Callers may then skip PostChain entirely.
    bool PostIsIdentity() const;

    bool operator==(const Settings &) const = default;
};

// The products the renderer makes in the background (worker threads, the GPU between frames) for these settings:
// the one place that says which bake or texture generation runs. Each runs only while the settings read what it makes;
// switching a product off cancels its queued and running work (the renderer's gates, effects/), switching it on
// starts it for what is loaded. «Низкая» needs the far LODs only.
struct BackgroundProducts
{
    bool far_lods = false;     // the simplified far models (far_lod_pixels)
    bool visibility = false;   // the location's visibility volume (lit draws: per-pixel or point lighting)
    bool ao_volume = false;    // an interior's AO volume
    bool town_light = false;   // our town lightmaps (place bake, sun relight)
    bool town_walls = false;   // the town walls' calibration (with town_light)
    bool lamp_shadows = false; // the lamps' shadow faces of the still geometry
    bool sun_shadows = false;  // the sun's shadow cascade and its caster tiles
    bool relief = false;       // relief maps (PBRify normal / height) of the loaded textures
    bool gloss = false;        // gloss maps (PBRify roughness) of the loaded textures
    bool upscale = false;      // neural ×2 textures near the camera

    // "far LODs, visibility volume, ...", or "none".
    std::string Describe() const;
    bool operator==(const BackgroundProducts &) const = default;
};
BackgroundProducts BackgroundProductsFor(const Settings &settings);

// «Качество текстур» (owner decision 2026-11-16), one choice for the textures' resolution and the neural ×2:
// 0 Низкое, 1 Среднее, 2 Высокое (the game's textures from their second mip, their first, their full size), 3 Ультра
// (Высокое and the neural ×2 near the camera). The resolution applies at the next start (the textures are loaded at it);
// the ×2 at once. Высокое is the default.
enum class TextureQuality : int
{
    Low = 0,
    Medium = 1,
    High = 2,
    Ultra = 3,
};
inline constexpr int kTextureQualityLevels = 4;
inline constexpr TextureQuality kTextureQualityDefault = TextureQuality::High;
// The top mips the textures skip (the renderer's texture degradation): 2, 1, 0, 0.
inline int TextureDegradation(TextureQuality quality)
{
    return quality >= TextureQuality::High ? 0 : 2 - static_cast<int>(quality);
}
// The level's part of the effects settings: the neural ×2 with «Ультра» only, whatever the preset.
inline void ApplyTextureQuality(Settings &settings, TextureQuality quality)
{
    settings.upscale = quality == TextureQuality::Ultra;
}

enum class LampDepth : uint8_t
{
    None,
    D24,  // 24-bit fixed point
    D32F, // 32-bit float
};
const char *LampDepthName(LampDepth depth);

// What the device and the renderer can do, decided once at start-up (fx::DetectSupport() in capabilities.hpp
// fills the GPU part; the renderer fills `scene_depth`). Show it on the settings screen: an option whose
// requirement is missing must be disabled there, not quietly ignored.
struct Support
{
    bool lut_3d = false;        // 3D RGBA8 texture: the grading table every post effect passes through
    bool shadow_map = false;    // D16 texture as a render target: Settings::shadows
    // The depth format of the lamps' shadow faces (lamp_shadows.hpp): a perspective depth over 1 cm..the lamp's
    // radius needs 24 bits or a float (16 bits resolve 15 cm at 10 m). None: Settings::lamp_shadows is switched off.
    LampDepth lamp_depth = LampDepth::None;
    bool scene_depth = false;   // the scene's depth attachment can be sampled: Settings::sea_soft_edges, render scale
    bool normal_map = false;    // EAC RG11 textures (the derived maps): kLightingNormalMap, Settings::gloss
    uint8_t msaa_samples = 1;   // the highest MSAA sample count (1 = none, 2 or 4) the scene target renders with,
                                // RGBA8 colour and D24S8 depth both (per-format bgfx caps): Settings::msaa
    bool smaa = false;          // R8 and RG8 textures (the SMAA search and area tables): Settings::smaa
    bool msaa_depth = false;    // the scene depth can still be sampled when the scene target is multisampled
                                // (renderer knowledge, like scene_depth): msaa together with sea soft edges / render scale
    bool anisotropy = false;    // anisotropic texture filtering
    bool ao_volume = false;     // RGBA8 3D textures with linear filtering: Settings::ao_volume
    bool sea_waves_vertex = false; // RGBA8 textures can be sampled in a vertex shader: Settings::sea_waves_lift
    // RG32F render targets: two successive heights need full precision for their small difference.
    // Otherwise use packed RGBA8 (16-bit fixed point per height), never RG16F.
    bool sea_waves_float = false;
    // Independent RGBA16F surface (height, slopes, foam); otherwise RGBA8 with 8-bit slopes.
    bool sea_waves_surface_float = false;

    std::string Describe() const;
};

// Inconsistent or unsupported requests in `settings`, one message each; empty = every effect will run exactly
// as asked. PostChain, Shadows and the sea's soft edges refuse (and log) what this reports.
std::vector<std::string> ValidateSettings(const Settings &settings, const Support &support);

// The explicit, one-time alternative to a silent fallback: returns `requested` with every option that
// ValidateSettings() objects to switched off or lowered, and appends one line per change to `changes`.
// Call it when settings are loaded or changed, log the lines, and store / display the result.
Settings ConstrainSettings(const Settings &requested, const Support &support, std::vector<std::string> &changes);

// What the renderer has to set up for these settings; fx cannot do it itself because the scene target and
// the samplers of the game's textures belong to the renderer.
struct RendererState
{
    uint8_t msaa_samples = 1;         // scene colour + depth target; resolve before PostChain reads it
    bool alpha_to_coverage = false;   // add BGFX_STATE_BLEND_ALPHA_TO_COVERAGE to alpha-tested draw calls and use
                                      // fxCoverageAlpha() (fx_coverage.sh) instead of the alpha test
    uint8_t max_anisotropy = 1;       // > 1: BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC on mip-mapped world
                                      // textures and BGFX_RESET_MAXANISOTROPY; cap the level at this value
    uint16_t render_height = 0;       // Settings::render_height: with RenderScale() < 1 the scene colour + depth target
                                      // is that fraction of the display size; PostChain upscales it, the interfaces
                                      // draw on the display-size picture
};
RendererState RendererStateFor(const Settings &settings);

// The factor of the display size the 3D scene renders at for Settings::render_height on a display `display_height`
// pixels tall: render_height / display_height, never below kMinRenderScale (FSR 1's range), 1 when the display is
// not taller than render_height or render_height is 0.
inline constexpr float kMinRenderScale = 0.5f;
float RenderScale(uint16_t render_height, uint32_t display_height);

// Names for the settings screen (UTF-8, Russian).
std::string_view NameRu(QualityPreset preset);
std::string_view NameRu(GradePreset preset);

} // namespace storm::fx
