# renderer_bgfx

The engine's only renderer: [bgfx](https://github.com/bkaradzic/bgfx), on OpenGL ES 3.0 or on Vulkan,
on Android and on the Linux development host alike.

- `cmake/bgfx_pin.cmake` — the single pin of bgfx, bx, bimg and bgfx.cmake; `cmake/bgfx_upstream.cmake`
  adds those targets with `patches/` applied (each patch starts with its reason; the configure fails when a
  patch does not apply or changes nothing). Everything that uses bgfx, bx or bimg links `storm_bgfx_runtime`
  (or `storm_bgfx`), never those targets, and never fetches bgfx itself. On Android `storm_bgfx_runtime` is
  `libbgfx.so`, shared by `libengine.so` and `libstorm_ui.so` so a process has exactly one bgfx
  (`tools/android/verify_engine.py` refuses a `libengine.so` with its own). Offline builds pass
  `-DFETCHCONTENT_SOURCE_DIR_STORM_{BGFX,BX,BIMG,BGFX_CMAKE}=<pinned checkout>` (bgfx patched).
  Licences: `third_party/licenses/{bgfx,bx,bimg}-BSD-2-Clause.txt`.
- `src/shim/` — the D3D9-shaped device behind `VRenderer` (src/libs/renderer/include/renderer.h); an
  unimplemented path logs a `[storm-bgfx] TRAP`.
- `shaders/` — fixed-function emulation, the blit and `include/storm_common.sh`; `shaders/ports/` +
  `src/shim/ports.cpp` — hand ports of the vs_1_1/ps_1_x shaders used by `src/techniques`, looked up by
  `<fx base name>_<shader name>` (unknown keys are logged and their draws skipped).
- `include/storm/render/frame_owner.hpp` — the frame owner contract (the header is the documentation).
- `src/texture/` — GPU copies of the game's DXT textures with rebuilt mip chains, made in the background
  with an on-device cache; the tier (bc / etc2 / rgba8) is chosen per GPU and logged.
- `fx/` — optional effects package ([fx/README.md](fx/README.md)), driven by `src/effects/`.
- `host-tools/` — builds only the host `shaderc` from the same pin.

## Backends

`render_backend = auto | gles | vulkan` in engine.ini (debuggable Android builds: the graphics setting,
0 / 1 / 2). `auto` (default) takes Vulkan when the device has Vulkan 1.1 with `VK_KHR_swapchain` and
`VK_KHR_maintenance1`, a hardware GPU, ETC2 or BC textures, is not on `VulkanDenied`
(`src/shim/backend_select.cpp`) and has no recorded Vulkan crash, and when `bgfx::init` and the backend
self-test pass. Otherwise the device runs OpenGL ES; every start logs one line
`render backend: ...: <reason>` that the diagnostics export and the graphics settings show.

A process that died during the Vulkan start leaves `<cache>/RenderBackend/vulkan-running.txt`; the next start
records it (`vulkan-crashed.txt`) and that device and driver run OpenGL ES until the driver changes or
«Очистить кэш». A bgfx FATAL on Vulkan later in the run (`ProgramCache::fatal`: an error line in error.log, then
the abort) is recorded the same way (`Context::vulkan_failed`).

`VulkanDenied` lists drivers that pass every check above and still break the game, each with its evidence: every
Imagination (PowerVR) driver for now (Rogue 1.13@5776728 on the Galaxy A12: corrupted geometry and
`VK_ERROR_OUT_OF_HOST_MEMORY`; D-Series 25.1@6794074 on the Pixel 10 Pro: `VK_ERROR_UNKNOWN` creating the title's
pipelines). `auto` runs OpenGL ES there and logs the reason as a warning; `render_backend =
vulkan` still takes Vulkan.

Developer keys in engine.ini: `render_backend_self_test = fail` (tests the fallback),
`missing_texture_look = neutral | loud` (stand-in for a failed texture; loud is the default on the host and in
debuggable builds), `vulkan_async_pipelines = 0` (create every Vulkan pipeline on the render thread),
`texture_compression`, `texture_cache_mb`.

`tests/renderer_bgfx/run.py` runs every check that draws on OpenGL ES and again on Vulkan
(`STORM_CHECK_BACKEND=vulkan`) where the host has a Vulkan device.

## Threads

bgfx runs multithreaded: the game thread records a frame, `bgfx::frame()` (only in `Context::run_frame`)
hands it to the render thread, which executes the backend and presents after `bgfx::frame()` returned.

- No GL or EGL call outside bgfx; driver facts come from `shim::Context::gl_info()`.
- `bgfx::makeRef` without a release function only for data that lives as long as the program; otherwise
  `bgfx::copy` or a release callback (runs on the render thread). A pointer given to `bgfx::read` stays valid
  until `bgfx::frame()` has returned the frame number `bgfx::read` gave.
- bgfx's callbacks and the present function run on the render thread.
- `render_thread = 0` keeps bgfx on the game thread, `render_overlap = 0 / 1` forces the pacing choice
  (engine.ini or the debug graphics settings; for measurements).

## Building

The configure step builds the pin's host `shaderc` once per machine into `~/.cache/storm-deps/shaderc-<key>`
(`-DSTORM_SHADERC=<binary>` overrides it). Shader compiles go through `cmake/shaderc_cache.py`, a
content-addressed cache shared by all build directories (`~/.cache/storm-deps/shader-cache`;
`STORM_SHADER_CACHE` moves it, `STORM_NO_SHADER_CACHE="<reason>"` turns it off; statistics in
`<build dir>/shader-cache-stats.log`). Host:

```sh
cmake -S . -B build/host -G Ninja -DSTORM_HOST_SOURCES=<prepared sources> -DSTORM_AUDIO_SOURCES=<audio sources>
ninja -C build/host engine
```

Android (`tools/android/ci_engine.sh` does this):

```sh
cmake -S . -B build/android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/android-arm64.cmake \
  -DSTORM_ANDROID_NDK=... -DSTORM_ANDROID_DEPS=... -DSTORM_ANDROID_SOURCES=... -DSTORM_AUDIO_SOURCES=...
ninja -C build/android engine
```

Texture tests and benchmark: `-DSTORM_BGFX_BUILD_TESTS=ON` (`storm_texture_tests`, `storm_texture_bench`).

## Writing shaders

`storm_bgfx_embed_shaders` (`cmake/storm_shaders.cmake`) compiles every `shaders/*.sc` and `shaders/ports/*.sc`
at build time twice, for OpenGL ES (`300_es`) and Vulkan (`spirv`), and embeds the blobs (`<name>`,
`<name>_spv`); nothing is compiled at run time and no shader files ship in the APK.

- The SPIR-V build compiles the sources as HLSL: stage inputs and outputs (`v_*`, `a_*`, `gl_Position`, ...)
  exist in `main()` only, and globals cannot be written. Pass what a helper needs as parameters or use a
  macro expanded in `main()`.
- Everything must be highp: `cmake/shader_precision_check.py` fails the build otherwise.
- Matrices are D3D row-vector matrices uploaded as they are: multiply `vector * matrix`. vs_1_1 ports output
  D3D clip space and apply the shim's clip-space fix-up matrix themselves.
- `D3DCOLOR` vertex colours arrive as B,G,R,A bytes: swizzle with `D3DCOLOR_TO_RGBA`.

## Known limits

- 5 fixed-function texture stages (`kFfStages`).
- 1 user clip plane, emulated with `discard` in the fragment shader.
- `D3DTEXF_NONE` as a mip filter is not emulated (sampled like `D3DTEXF_POINT`).
- The game may use views `0..223` per frame (`kGameViewEnd`); later passes reuse the last view
  and the overflow is logged once.
