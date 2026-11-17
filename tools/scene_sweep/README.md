# Scene sweep

Loads every scene of the game in parallel headless engines and reports the scenes that
crash, log script errors, miss textures or models, time out or render a black/blank frame.

**On demand and engine-only.** A deliberate command for engine and renderer work. It is not
part of per-push/PR CI, pre-commit hooks or any other automatic check, and must not be added
to them: a full sweep takes tens of minutes of CPU and needs the player's game files.

`inventory.py` derives the scenes from the game's own scripts (locations, the sea around every island, plus
special scenes: main menu, new-game intro, night, fights, underwater, sunrise/sunset, night sky, sea battle,
boarding, world map, storm). `python3 tools/scene_sweep/run.py --game <game dir> --list` prints the counts by
category. The engine built with `-DSTORM_SCENE_SWEEP=ON` (never shipped) runs `scene_sweep.c` when
`STORM_SCENE_SWEEP_DIR` is set; `run.py` shards the scenes over engines, each with its own Xvfb display and a
hard-linked copy of the game.

## Running

Needs a Linux host engine configured with `-DSTORM_SCENE_SWEEP=ON`, Xvfb, Pillow, a GPU driver and VirtualGL
(or `--render vulkan` / `--render llvmpipe`) and the game files. `--out` must be on the same filesystem as
`--game`. Pass the real game folder: a `--game` holding symbolic links is refused; a read-only `--game` works and
is checked to be unchanged after every engine run. The sweep copies the engine binary at start, so the build can
go on while it runs.

```
python3 tools/scene_sweep/run.py --game <game dir> --engine <build>/engine --out <report dir>
```

- `--jobs N` parallel engines (default: half of the idle cores, capped at 2 GB of free memory per engine);
  `--single-session` one engine without retries in `--only` order (with `--jobs 1`).
- `--only PATTERN...` scenes by shell pattern on `kind:name` or by category (`'loc:*_town' tavern map:world`).
- `--smoke` one scene of every category (a few minutes; the quick check before a full sweep).
- `--fail-fast` stops at the first failing scene.
- `--failed` reruns the previous report's failures; `--changed` only scenes whose scripts or models changed
  since it, plus its failures; `--baseline report.json` fails only on scenes that pass in that report.
- `--variants 'fx_preset=0;fx_preset=2,fx_bloom=0'` captures every scene once per option set (see below).
- `--render gles-gpu|vulkan|llvmpipe` (default `gles-gpu`); `--virtualgl DIR` (or `STORM_VIRTUALGL`) for
  `gles-gpu`; `--gl-threads` llvmpipe rasteriser threads.
- `--save FILE` boots every engine into this save (hero on land) instead of a new game; used by
  `tools/ci/boot_stage.py`.
- `--program-cache DIR`, `--script-cache DIR` keep the program/pipeline and compiled-script caches between runs
  (`--jobs 1`).
- `--relief-models DIR` relief networks (default: `relief_models_path.txt` beside `--engine`, made by
  `ninja storm_relief_model_files`); without them no relief map is made.
- `--timeout S` seconds without progress before an engine is killed (default 240); `--settle MS` render time
  before each capture (default 300; raise it with `--only` and `--jobs 1` for performance measurements).
- `--size WxH` (default 1280x720), `--lossless` PNG shots, `--techniques DIR`, `--keep-work`.

A scene fails on a crash, a timeout, script errors, missing textures or models, error-level log lines, a fader
that never goes, or a picture that cannot be the scene (black, unlit lower half, HUD not drawn, identical to the
previous scene). Output in `--out`: `report.json`, `summary.txt`, `shots/` and contact sheets in `contact/`.
Keep reports out of the repository.

**Known game data defects** that the engine reports correctly but cannot fix (we never ship or patch game data)
are listed in `known_defects.json` (game version, scene pattern, file, exact error text, reason); a scene whose
only problems match is reported `KNOWN`. Remove an entry once its cause is gone.

## Variants

Every `fx_*` option of `EffectsOptionNames()` (`src/libs/renderer_bgfx/src/effects/effects.cpp`) can be set in a
variant; an unknown name or value fails the scene. A variant is captured once its own settings draw; background
products are reported in `report.json` (`background`) unless the variant waits for them:

- `wait=upscale` the neural ×2 textures (`texture_quality=3`), `wait=relief` relief generation, `wait=bakes` the
  bake queue (town light, visibility, AO volume, far LODs); repeatable.
- `hour=<h>` (0..24) shows the variant at that time of day.

In locations game time stops from the first variant on, so every variant shows the same picture. A/B: switch one
product per variant over the same preset, e.g.
`--variants 'fx_preset=2,wait=bakes;fx_preset=2,fx_shadows=0,wait=bakes'`.

## Renderer

- `gles-gpu` (default): OpenGL ES on the host GPU under VirtualGL (NVIDIA's EGL cannot initialise on Xvfb).
  VirtualGL is supplied by the user, never fetched or committed: `dpkg -x VirtualGL_3.1.5_amd64.deb ~/vgl`
  (wxWindows Library Licence), then `--virtualgl ~/vgl`.
- `vulkan`: Vulkan on the host GPU, presenting into Xvfb directly.
- `llvmpipe`: OpenGL ES on Mesa llvmpipe (CPU), for hosts without a GPU.

The GPU device nodes must be accessible to the user (`/dev/nvidia*` or `/dev/dri/renderD*`). Each engine's
renderer is checked against the mode before the first capture; a mismatch stops the sweep.
