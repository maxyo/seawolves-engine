# Graphics effects package for the bgfx renderer

Post-processing (ambient occlusion, tone curve, colour grades, bloom, sun shafts, lens flare, dither, SMAA, the
FSR 1 upscale and sharpening), sea improvements and per-pixel lighting for the fixed-function emulator (sun, point
and spot lights, sun shadows, interior ambient-occlusion volume, relief from derived normal maps, aerial
perspective, wet surfaces). The package depends on bgfx only and takes plain structs; `src/effects`
(`effects::Host`) owns the settings, feeds per-frame inputs and calls it. Licences: [THIRD_PARTY.md](THIRD_PARTY.md).

## Presets and options

The player chooses «Низкая / Средняя / Высокая» (`Settings::FromPreset`, `include/storm/fx/settings.hpp`). Every
preset renders the 3D scene at an internal height (900 / 1000 / 1100 lines, `fx_render_height`, never below half
the display) on taller displays and upscales it with FSR 1; interfaces are drawn at the display size. «Низкая» is
the legacy image with the upscale only; «Средняя» and «Высокая» add the effects listed in `Settings::FromPreset`.
Where MSAA is unavailable `ConstrainSettings` replaces it with SMAA 1x (logged).

Single options are `fx_*` keys of engine.ini or `STORM_FX_OPTIONS` (parsed in `src/effects/effects.cpp`), for
development. Useful ones:

- `fx_gpu_profile=1`: per-view GPU timings (costs CPU on GL; off by default);
- `fx_sea_waves_debug=1|2`: ripple slope and foam / normal tilt;
- `fx_wet_force`: forces a rain wetness for review;
- `fx_gloss_debug`, `fx_character_relief` (0/1/2), `fx_character_relief_strength`,
  `fx_character_parallax_depth`, `fx_character_skin_colour`: relief and gloss review (debuggable builds);
- `fx_light_linear`: linear-light sums (in no preset: the game's lights were tuned in gamma space).

Parallax exclusions: the shipped list is `src/parallax_exclusions.inc`; the user file `pom_user_exclusions.txt`
in the app's files directory takes `exclude <name>` / `include <name>` lines (written by the debug view's
toggle). The log line `effects parallax: texture ...` says why a texture has no parallax.

## Build

Built with the renderer. Shaders are compiled offline with the pinned shaderc into embedded blobs for OpenGL ES
(`STORM_FX_SHADER_PROFILE`, default `300_es`) and Vulkan (SPIR-V); `shaders/shaders.cmake` lists every variant. Unit
checks of the CPU side (`storm_fx_core_check` and the `fx_*` ctests) run from `tests/renderer_bgfx/run.py`.
Stand-alone preview of every effect: [tools/fx-preview](../../../../tools/fx-preview/README.md).

## Rules

- No silent fallbacks: `ConstrainSettings()` is the only place a request is lowered, before rendering, and every
  change comes back as a line of text. A call that still cannot be honoured is refused and reported once through
  `fx/log.hpp` at `LogLevel::Error`; `tools/fx-preview` fails on any such report.
- Stage 15 belongs to the shadow map, stage 14 to the derived normal map (and `SeaDepth::Texture()` in the sea
  shader), stage 7 to the gloss / relief material.
- Normal, height and gloss maps are made on the device from the player's own textures (src/libs/relief); never ship
  or commit them.
