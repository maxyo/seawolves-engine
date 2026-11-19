# Portable game UI runtime

`storm_ui` draws the game interface from a *UI pack* supplied by the application
(documents, styles, component specs, icons, fonts). It is built on RmlUi and renders only through
bgfx (`include/storm_ui/backend_bgfx.h`); the platform host owns the window, the frame loop and
touch delivery. The pack and the design rules live in the application repository
(`mobile-ui/design`). Game interfaces and the HUD: [src/screens/README.md](src/screens/README.md).

Build option `STORM_ENABLE_UI` (off in the main engine build; the directory is also a
stand-alone CMake project for host checks and captures). Dependencies are SHA256-pinned release
archives fetched at configure time: RmlUi 6.3 (MIT), FreeType 2.14.3 (FTL), lunasvg 3.5.0 with plutovg
(MIT), nlohmann/json 3.12.0 (MIT). Local fixes on top of the RmlUi pin live in `patches/rmlui-*.patch`.
The build pulls in `src/libs/renderer_bgfx` for bgfx (offline:
`-DFETCHCONTENT_SOURCE_DIR_STORM_{BGFX,BX,BIMG,BGFX_CMAKE}=<checkouts>`, bgfx patched).
No game data is involved in building or testing.

## Build and check on the host

```sh
cmake -S src/libs/ui -B build/ui-host -G Ninja
cmake --build build/ui-host
ctest --test-dir build/ui-host --output-on-failure
build/ui-host/storm_ui_shot --pack <pack dir> --doc gallery/button.rml --out shot.png
```

Pack contract check: draws a real pack (the application's `mobile-ui/design/build.py --pack DIR`) with the
real screen and HUD adapters on every recorded model and state of `tests/` (cases and exemptions:
`tests/pack-contract.json`), without a GL context:
`build/ui-host/storm_ui_pack_check --pack DIR --fixtures src/libs/ui/tests`. A case fails on any error line
and any element marked `ui-error`; a ported screen with no case and no listed reason fails too. It is the
ctest `ui_pack_contract` when configured with `-DSTORM_UI_PACK=<built pack>`, and the application runs it
in `tests/ui_pack_contract`.

### storm_ui_shot

Renders off-screen through the bgfx backend (GLES 3.0 on headless Mesa llvmpipe, simulated clock).
Options: `--views N` (UI view range), `--game-root` (decodes game pictures itself), `--safe L,T,R,B`
(safe-area insets in dp, `safe.left` … `safe.bottom` in templates), `--interface <model.json>`, `--state <channel>=<file.json>`, `--size`, `--density`.
Component actions are printed as `action <id> <verb> <value>`.

`--script` replays touches and checks frames, e.g.
`--script "down 1 660 88; wait 0.5; shot held.png; up 1 660 88"`: `shot` waits for the backend's texture
work; `seen <png>` saves what the last frame left on the screen; `mark` remembers that picture, `whole` fails
unless the last frame showed the marked picture or the new one complete, `arrived` fails while the marked
picture is still there; `bench 300 [<channel> <a.json> <b.json>]` measures frames as a platform host makes
them (with a channel the two states alternate every frame).

## Android

`tools/android/ci_engine.sh` builds `game/libstorm_ui.so` (target `storm_ui_runtime`) next to
`libengine.so` and `libbgfx.so`, and the licence texts of its dependencies in `<build>/licenses/`. The APK
ships the three from that one build. `libbgfx.so` is the only bgfx of a process; `libengine.so` and
`libstorm_ui.so` both link it. Application screens without the engine (launcher, downloads, gallery) load
`libstorm_ui.so` alone through `org.stormengine.port.StormUi` (`android/jni_host.cpp`) on their
GLSurfaceView's context. UI code is built with `-gline-tables-only` in builds with debug info.
Debuggable Android builds log frame costs every 5 s (logcat tag `StormUiFrames`).

## Rules for pack and screen authors

- Errors are loud: an unknown `<c-…>` tag, an attribute a component does not have, an enum value outside
  its options, a template or class expression error and an icon missing from the pack are logged with the
  element and, in debug builds, drawn as the striped `ui-error` marker or a crossed red icon (the pack styles
  `.ui-debug .ui-error`; `ui-debug` is the class of every pack document's root with `Config::show_errors`).
  Release builds only log them and draw nothing in their place. With
  `Config::strict_templates` (host checks, captures) a template path that names nothing is an error too;
  mark optional data in the template with `opt(r.reason)` or `or(r.id, r_i)` (`src/template.h`).
- A screen that throws closes its interface with an error line; a HUD scene that throws is off until a
  backed-off retry. Error lines are toasts only with `Config::show_errors` (debug builds); release builds
  only log them.
- `icon-shadow: <colour> <x> <y> <sigma>` bakes a drop shadow into an icon; use it instead of
  `filter: drop-shadow` on icons.
- The host draws only when `Host::Prepare` reports the frame out of date; a frame is shown only when its
  textures are ready. `tests/frame_cost_check.cpp` holds the frame-cost rules.
- Nothing delays a present: the host never keeps a frame on the screen for the game's answer, and no timer
  stands between a tap and what it shows. A tap's result that needs no decision of the game is drawn in the frame
  of the tap (`screens::Bindings::Predict`, src/screens/README.md); the game's answer confirms it, or corrects it
  with an error line "UI prediction corrected: …". The engine answers a widget action in the frame it arrived in
  (xinterface runs the action's script events before that frame's model goes out). Flicker is prevented at its
  cause: a screen does not draw a state it knows to be a step of the game's answer (a model rule, never a time).
- Flicker check (`src/action_trace.h`, `Config::trace_actions`, on in debug builds and device crawls): every
  action's presented frames are recorded until they settle; a frame that differs from both the start and the
  settled state is logged "UI flicker: …". Every line says when the screen first changed ("first change in frame N,
  +X ms") and, for a predicted action, "predicted" or "LATE" (its result was not in the tap's own frame, a warning). The pack contract
  fails on a late predicted tap anywhere, on a transient frame in a tap that needs no game and in `flows` (`tests/pack-contract.json`: taps with the game's real
  answers frame by frame); `tools/ui_crawl` reports each tap's and ranks a crawl's (`ui_crawl.py flicker DIR`).

## Licence boundary

Game pictures are never bundled: `<ui-game-image>` reads
`<game root>/RESOURCE/Textures/<texture>.tga.tx` at run time. Tests use synthetic
texture data only.
