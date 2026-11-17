# Scene sweep

Loads every scene of the game in parallel headless engines and reports the scenes that
crash, log script errors, miss textures or models, time out or render a black/blank frame.

**On demand and engine-only.** A deliberate command for engine and renderer work. It is not
part of per-push/PR CI, pre-commit hooks or any other automatic check, and must not be added
to them: a full sweep takes tens of minutes of CPU and needs the player's game files.

## What it loads
`inventory.py` derives the scenes from the game's own scripts, so new content is picked up
without editing the tool:

- every location from the init functions that `PROGRAM/locations/locations_init.c` calls
  (towns, interiors, jungles, caves, forts, beaches, decks, cabins...), categorised by its `type`;
- the sea around every island of `PROGRAM/islands/islands_init.c`;
- special scenes: the main menu with its 3D back scene, the new-game intro, one location of
  every type at midnight, land fights (jungle, town, cave, deck), the sea surface seen from below
  (the first underwater location with a sea, camera looking up), the open sea at sunset (19:00) and at sunrise (7:00, the low sun in view), heading towards the sun, the open sea at 23:45 of the 14th heading into the rising moon (in view), the clear night sky at 0:30 on the 14th (`skynight:open`) and clear noon sky (`skyday:open`) with a raised camera, the hero's ship
  at midnight seen close from above (rigging against the water, bow wave, waterline), a sea battle, a boarding,
  the world map and a storm entered from the world map.
  The sea battle waits until the enemy ship has fired its cannons (`CANNON_FIRE` from that
  captain) and fails when it has not within 90 s of game time at sea.

After the new game the script reports the size of the game's own location and island tables;
the sweep fails (`inventory:tables`) when they differ from what `inventory.py` parsed.

`python3 tools/scene_sweep/run.py --game <game dir> --list` prints the counts by category.

## How it works
- The engine built with `-DSTORM_SCENE_SWEEP=ON` (never shipped builds) loads
  `scene_sweep.c` after `Main()` when `STORM_SCENE_SWEEP_DIR` is set
  (`src/libs/core/src/scene_sweep.cpp`). The script starts a new game through the main menu
  and enters each scene with the functions the game uses itself (quest reload to a location,
  quest sea login, world map, storm, boarding), at noon with the noon weather (or midnight).
  The hero enters a location where the game puts him: boarding decks and cabins at their
  `boarding.Loc.Hero` locator, other locations through their first door. A game day passes
  between locations so that phantom NPCs expire as in play.
- The videos before the main menu (80 s) are broken off the game's own way, as the phone does when it starts at the
  main menu: one `ICancel` per video (events `video skipped N`).
- For each scene it waits until the scene is loaded and the loading fader is gone, renders
  at least 5 frames and 0.3 s, then asks the runner to capture the frame. On Vulkan a scene draw whose
  pipeline is still being created off the render thread is left out of its frames (it appears a few frames
  later); a window in which that happened is not captured: the script renders another one
  (`SceneSweepWindowComplete`, `storm::render::LastIncompleteFrame`), counted in the report as
  `incomplete_windows` / `incomplete_ms`, and fails the scene after 180 s of them. The runner saves
  the screenshot, frame time, draw calls and memory, and attributes the engine's log lines
  to the scene.
- `run.py` splits the scenes into shards, one engine per shard, each with its own Xvfb
  display (the engine draws with Vulkan on the GPU and presents into it; see *Renderer*),
  writable data directory and hard-linked copy of the game (the engine's
  techniques laid over the game's, as the APK build does). Every engine boots once per shard
  and then loads its scenes back to back. A crashed or hung engine fails
  its current scene and the shard restarts with the remaining ones.
- Every file of a copy that the sweep or the game writes (`engine.ini`, `options`, the
  `PROGRAM/scene_sweep.c` script, the `engine` link) is unlinked before it is written, so the
  write never goes through the hard link into the player's game or another sweep's copy. After
  every engine run the sweep checks that no file of `--game` changed (inode, size, modification
  time) and stops with a harness error naming the files if one did.
- The sweep never deletes or writes through a symbolic link: a copy that holds one (`--game` itself
  or any folder in it linked elsewhere, e.g. `RESOURCE` linked to a shared extraction) is refused
  with a harness error naming the links, and every path it deletes or writes is checked to be no
  link and to resolve inside its own copy (tests/scene_sweep_links). Pass the real game folder. A
  read-only `--game` (local-testing/extracted/app) works: only the copy's own directories are made
  writable, the hard-linked files keep their modes. The sweep never chmods a regular file and never
  writes a file with another name (nlink > 1): a written file is unlinked first and checked to be its
  own inode, and the check of `--game` after every engine run includes the file modes.

A scene fails on: a crash or fatal engine error, a timeout, a failed teleport to the entry locator, script errors (`error.log`),
missing textures or models, engine/renderer errors in the logs (an error by its level: `[error]`, `[critical]`, the
renderer's `ERROR: ...` lines; never a word such as "fail" in a path or a count), a fader that never goes,
or a picture that cannot be the scene. Every capture is checked, every variant too (reasons `variant N: ...`):
a black or single-colour frame, the lower part black at both sides under a lit upper part (a sea or ground not
drawn), the game's HUD not drawn (the hero's portrait at the top left, at sea also the compass at the top right;
in locations a dialogue's panel stands in for the portrait; not checked in `menu:main` and `newgame:intro`), or
(first variant) a frame identical to the previous scene's. The HUD boxes and thresholds are calibrated on
1280x720 sweeps.

**Known game data defects.** A problem in the game's own files that the engine reports correctly and cannot fix
(we never ship or patch game data) is listed in `known_defects.json`: the game version, the scene (a shell pattern), the file at fault,
the exact error text the sweep sees and the reason. A scene whose only problems match its entries is reported as
`KNOWN` (progress line, `status: known` in the report, its own section of the summary), not as failed; any other
problem still fails it. Remove an entry once its cause is gone. A texture the game's models name but the game does
not ship is not listed here: the engine knows it (`KnownMissingGameTexture`, src/libs/renderer_bgfx/src/effects/effects.cpp)
and logs it once per session as a `WARNING: known game data defect`, which fails nothing; any other missing texture
still fails the scene.

## Running
Needs a Linux host engine configured with `-DSTORM_SCENE_SWEEP=ON`, Xvfb, Pillow, a GPU driver and
VirtualGL (or `--render vulkan` / `--render llvmpipe`, see *Renderer*) and the game files (`--out` must be on the same filesystem as
`--game`). Keep one persistent build
directory with the option on and rebuild it incrementally (`ninja -C <build> engine`); the
sweep copies the binary once at start, so the build can go on while it runs.

```
python3 tools/scene_sweep/run.py --game <game dir> --engine <build>/engine --out <report dir>
```

- `--jobs N` parallel engines; by default half of the idle cores (at least a quarter of all
  cores), capped by free memory at 2 GB per engine.
- `--only PATTERN...` scenes by shell pattern on `kind:name` or by category
  (`'loc:*_town' tavern map:world`).
- `--smoke` one scene of every category (about 45): a few minutes, the quick check after
  an engine change before a full sweep.
- `--fail-fast` stops at the first failing scene.
- `--variants 'fx_preset=0;fx_preset=2,fx_bloom=0'` captures every scene once per set of graphics
  options (applied at run time through the renderer, `STORM_SCENE_SWEEP_VARIANTS`): shots
  `<scene>@v<i>.jpg`, frame time and draws per variant in the report (`variants`), measured over
  12 frames after a 4-frame warm-up that starts once the variant draws; the boot scenes are captured once. Used for
  the per-effect cost of the fx presets and the per-product A/B sheets (*Variants* below). In locations the game's time
  stands still from the first variant on (`SetTimeScale(0)`; the sweep's clock then counts 100 ms a frame), so every
  variant shows the same picture: camera, people, sea and sky.
- `--failed` reruns the failures of the previous report in `--out`; `--changed` runs only
  scenes whose scripts or model folders changed since it, plus its failures (everything
  when the engine binary or `scene_sweep.c` changed: use `--smoke` or `--failed` then).
  With nothing changed it reruns only the previous failures.
- `--baseline report.json` exits non-zero only for scenes that pass in that report.
- `--render gles-gpu|vulkan|llvmpipe` what the engines draw with (default `gles-gpu`), see *Renderer*;
  `--virtualgl DIR` (or `STORM_VIRTUALGL`) is the VirtualGL directory `gles-gpu` needs.
- `--program-cache DIR` keeps the engines' program and pipeline cache in DIR between runs (`--jobs 1`): the first
  sweep measures a cold cache, the next ones a warm one (Vulkan: the pipeline cache and the scenes' recorded pipelines,
  `src/libs/renderer_bgfx/src/shim/pipeline_sets.hpp`).
- `--script-cache DIR` keeps the engines' compiled-script cache in DIR between runs (`--jobs 1`; copied in and out of
  the engine's cache folder): a boot skips compiling the game's scripts while they and the engine are unchanged.
- `--save FILE` boots every engine into this save instead of a new game: the main menu loads it (scene `save:boot`,
  captured like the others; the load replaces the script state, so the engine loads `scene_sweep.c` again after it,
  `COMPILER::OnLoad`). The hero must be on land; the sea scenes return to where the save stands. Minutes faster than
  the new game. Used by the gate's boot stage (`tools/ci/boot_stage.py`).
- `--timeout S` seconds a scene may go without progress before its engine is killed (default 240): every event of
  the scene counts (loaded, a variant active or captured, the engine's report every 10 s while it prepares a variant).
  The script fails a scene that has not loaded 30 s before that with its own reason (`not loaded after ... ms`), and
  a variant that waits for a background product (`wait=`) as long after its settings became active.
- `--relief-models DIR` the relief networks the engines make their relief maps with (engine.ini `relief_models`); by
  default the folder the engine's build names (`relief_models_path.txt` beside `--engine`) when it has made it
  (`ninja storm_relief_model_files`; the `engine` target does not). Without it no relief map is
  made (the run says so) and `wait=relief` returns at once: pass it for relief A/B sheets of engines copied out of their
  build.
- `--lossless` saves the shots as PNG (comparison sheets of fine texture detail; JPEG by default).
- `--settle MS` how long each scene renders after its fader before the capture (default 300).
  Frame time and the renderer counters are means over this window: raise it (e.g. 5000) with
  `--only` on a few scenes and `--jobs 1` for performance measurements.

## Variants
A variant is ready when **its own settings and programs draw**. The renderer prepares new settings over many frames
while the previous ones keep drawing: it links their programs (a few per frame; the first switch to «Высокая» on a cold
program cache under VirtualGL links about 60 programs in about a minute) and runs its self-test. The engine times this
(`src/libs/core/src/scene_sweep.cpp`, `variantReadyNow`): the event `variant <i> active ms=.. programs=..` in
`events.log`, a log line, and every 10 s while it lasts `variant <i> preparing ms=.. programs=..`. Preparation fails the
scene only when it stops progressing: no new program for 30 s, or for 10 times the measured mean time per program
when that is longer (`graphics variant <i>: program preparation stalled: ...`).

Products that go on generating in the background after the settings are active are **reported, not waited for**:
each variant's entry in `report.json` has `prepare_ms`, `programs`, `settle_ms` and `background`, the products still
generating at its capture. A variant waits for one only when it names it; the product must then stay idle for 1 s:

| Item | Waits for |
|---|---|
| `wait=upscale` | the neural ×2 textures (the texture quality «Ультра», `texture_quality=3`): the last residency plan found every texture it chose resident |
| `wait=relief` | the relief generation: no relief map (or list of relief textures) being made |
| `wait=bakes` | the bake queue (`fx/bakes.hpp`: town light, visibility volume, AO volume, far LODs): none queued or running |

`wait=` items may be repeated (`fx_preset=2,wait=upscale,wait=bakes`); an unknown name stops the engine. A product
still generating `--timeout` - 30 s after the settings became active fails the scene with its name.

`hour=<h>` (0..24, e.g. `hour=7.5`) shows the variant at that time of day: the script sets the game's time and the
weather's sun follows it (`WEATHER::SunAt`); the weather and the location's lightmap set stay those it was entered with,
as when the hours pass while the player stays. Used for the town light's moving sun shadows at several sun heights
(`--variants 'fx_preset=1,hour=7.5,wait=bakes;fx_preset=1,hour=14,wait=bakes'`).

A/B comparisons switch one product per variant over the same preset, e.g.
`--variants 'fx_preset=2,wait=bakes;fx_preset=2,fx_shadows=0,wait=bakes'`. Every `fx_*` option of
`EffectsOptionNames()` (`src/libs/renderer_bgfx/src/effects/effects.cpp`) can be set in a variant; an unknown name or
value is logged as an error, which fails the scene. The products of the shadow and lighting work:

| Product | Option (off / on) | Note |
|---|---|---|
| Sun shadow map | `fx_shadows=0` / `1` (one tap) / `2` (soft) | needs per-pixel lighting (Средняя, Высокая) |
| Lamp shadows | `fx_lamp_shadows=0` / `8` / `16` | lamps that relight a location with their shadows |
| Moving lamps (carried lanterns, torches) | `fx_lamp_moving=0` / `1` / `2` | their dynamic shadow faces |
| Lamp shadow filter | `fx_lamp_soft=0` (3x3) / `1` (5x5) | |
| Lamp shadow view | `fx_lamp_debug=2` | the shadow mask over the picture |
| Visibility volume | `fx_visibility=0` / `1` | sky visibility and rain shelter |
| Interior AO volume | `fx_ao_volume=0` / `1`..`3` | |
| Town light (our lightmaps for the live sun) | `fx_town_light=0` / `1` | `fx_town_light_debug=1` shows its sun visibility |
| Relit town walls | `fx_town_walls=0` / `1` | with `fx_town_light=1` |
| Town light resolution | `fx_town_light_scale=1` / `2` / `4` | our lightmaps' texels per side for each of the game's (Средняя 2, Высокая 4) |
| Relief self-shadow | `fx_parallax_shadow=0` / `1` | with `fx_parallax=2` |

The baked products (visibility, AO, town light) are made in the background once switched on: compare them with
`wait=bakes`, or the capture shows them still missing (its `background` says `bakes`). Options added for new products
belong in `EffectsOptionNames()` and in this table.

Output in `--out`: `report.json` (per-scene status, reasons, load time, `begin_ms` (when the scene began, ms after its engine started; for `menu:main` the boot), `reveal_ms` (from loaded to the scene
shown: the loading fader, which Vulkan holds while the scene's pipelines are created), frame time with its
`frame_p95_ms` / `frame_p99_ms` / `frame_max_ms` over the window, `first_window` (frame-time percentiles of the first
window after the fader, also when it was not captured because draws waited for pipelines), draw
calls, RSS, and `renderer`: per-frame means of the bgfx shim counters, see
`src/libs/renderer_bgfx/include/storm/render/perf_stats.hpp`), `summary.txt`, `shots/` and contact sheets in `contact/` (failures framed red).
Keep reports out of the repository.

## Renderer
`--render` picks what every engine draws with; frames on the GPU take a few milliseconds instead of 0.1-3 s
on llvmpipe, so long scenes (many variants, a long `--settle`, scenes that wait for game time) finish in a
fraction of the time. The boot and the new game take as long in every mode (CPU-bound script and data
loading).

- `gles-gpu` (default): `render_backend = gles`, OpenGL ES on the host's GPU, the backend of phones without a usable Vulkan.
  NVIDIA's EGL cannot initialise on an Xvfb display, so the engine runs under VirtualGL
  (`VGL_DISPLAY=egl`, its `libdlfaker.so` and `libvglfaker.so` preloaded): VirtualGL renders on the GPU
  through EGL's device platform and copies each frame into the shard's Xvfb display, whose framebuffer
  file the sweep captures as before. VirtualGL is a tool the user supplies, never fetched or committed:
  unpack the official release once (`dpkg -x VirtualGL_3.1.5_amd64.deb ~/vgl`; it is under the
  wxWindows Library Licence) and pass `--virtualgl ~/vgl` or set `STORM_VIRTUALGL`. Without it the
  sweep refuses to start and says so.
- `vulkan`: `render_backend = vulkan` on the host's GPU, the phones' default where Vulkan runs; the Vulkan driver (measured with NVIDIA's)
  presents into the Xvfb display directly, no VirtualGL. A capture waits for a frame in which no draw
  waited for its pipeline (`SceneSweepWindowComplete`; the report's `incomplete_windows`/`incomplete_ms`).
- `llvmpipe`: OpenGL ES on Mesa llvmpipe on the CPU (`LIBGL_ALWAYS_SOFTWARE=1`, `--gl-threads`
  rasteriser threads), for hosts without a GPU or to compare with reports made before the GPU modes.

The GPU device nodes must be accessible to the user running the sweep (NVIDIA: `/dev/nvidia*`; Mesa
drivers: `/dev/dri/renderD*`, group `render`). Before its first capture each engine's logged renderer
(`bgfx renderer=`, the program cache's device) is checked against the mode, and anything else stops the
sweep with a harness error: no GPU, VirtualGL not interposing (OpenGL ES on llvmpipe), a missing Vulkan
driver (the engine falls back to OpenGL ES), or a software device such as lavapipe. The renderer is in
`report.json` (`meta.render`, `build.renderer`) and the summary's first line; `--failed` and `--changed`
treat a report of another mode as a different build.
