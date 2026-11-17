# Android native build

These scripts build the native engine library (`libengine.so`) and its runtime dependencies. They never
build or publish APKs: packaging and releases belong to the application repository.

## CI scripts

```sh
bash tools/android/ci_dependencies.sh   # pinned NDK (checksum verified), tools, dependencies, regressions
bash tools/android/ci_engine.sh         # builds and verifies only libengine.so
```

Requires JDK 17, CMake, Ninja, a C++ compiler, a Python venv, FFmpeg, curl and unzip. `STORM_CI_WORK`
overrides the build workspace.

`ci_engine.sh` defaults to `Release`: `-O2 -g -DNDEBUG` with ThinLTO, hidden symbol visibility and section
garbage collection (`cmake/android.cmake`; `-DSTORM_ANDROID_RELEASE_LTO=OFF` disables only LTO). Only
`SDL_main` and the JNI entry points are exported. Debug info stays in the library; the application's
release packaging strips it and keeps the symbol file. `STORM_NATIVE_BUILD_TYPE=Debug` gives an
unoptimized build; `STORM_NATIVE_BUILD_TYPE` and `STORM_NATIVE_BUILD_DIR` together select an isolated
build directory (e.g. `RelWithDebInfo`). Package `game/libengine.so` from the build.

## Manual build

Dependencies (SDL 2.0.18 `2e9821423a237a1206e3c09020778faacfe430be`, `https://github.com/libsdl-org/SDL`)
from local, legally obtained source/toolchain directories; the script downloads nothing and applies
`patches/sdl2-android-ndk30.patch` to the SDL checkout, so use a dedicated checkout:

```sh
python3 tools/android/build_dependencies.py \
  --ndk /path/to/android-ndk-r30 \
  --sdl-source /path/to/SDL \
  --build-dir /path/to/android-dependency-build \
  --prefix /path/to/android-dependency-install \
  --jobs 4
```

Other pinned sources (exact Git hashes in the script): `engine_sources.py --directory /path/to/sources`.
Where several worktrees build, use `engine_sources.py --shared "$STORM_CI_WORK"`: it prints
`$STORM_CI_WORK/pins-<digest>`, immutable checkouts for exactly this tree's pins (`ci_engine.sh` and
`tools/ci/gate.py` do this). CMake fails the configure step when `STORM_ANDROID_SOURCES` or
`STORM_HOST_SOURCES` is not at the pins (`engine_sources.py --check`). The configure step builds and caches
the LGPL FFmpeg (`tools/ffmpeg/README.md`; package `runtime/ffmpeg/*.so` and `configure.txt` with the
engine) and the host bgfx shader compiler, and compiles through ccache (`tools/ccache/README.md`).
Prepare the patched audio headers with `tests/audio/run.py`.

```sh
cmake -S . -B build/android-engine -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/android-arm64.cmake" \
  -DSTORM_ANDROID_NDK=/path/to/android-ndk-r30 \
  -DSTORM_ANDROID_DEPS=/path/to/dependency-prefix \
  -DSTORM_ANDROID_SOURCES=/path/to/sources \
  -DSTORM_AUDIO_SOURCES="$PWD/build/audio/patched"
cmake --build build/android-engine --target engine --parallel 2
python3 tools/android/verify_engine.py --library build/android-engine/game/libengine.so \
  --ndk /path/to/android-ndk-r30
```

`verify_engine.py` expects every `CREATE_*` registration in `src` whose `#ifdef` guards hold for the options
in the build's `CMakeCache.txt` (found above `--library`), and fails when an option-gated one (e.g.
`SceneSweepLibrary`, `STORM_SCENE_SWEEP`) is linked with its option off.

## ABIs: arm64-v8a releases, x86_64 emulator builds

Every release is arm64-v8a. x86_64 is only for debug builds that run natively on x86_64 emulators. The ABI
is a parameter of every Android build script, default `arm64-v8a`:

- `STORM_ANDROID_ABI=x86_64` for `ci_dependencies.sh` and `ci_engine.sh` (`abi.sh`); `--abi x86_64` for
  `build_dependencies.py`, `verify_engine.py`, `build_torrent.py` and `android/archive/build.py`
  (`android_abi.py`).
- CMake: `cmake/toolchains/android-x86_64.cmake` instead of `android-arm64.cmake`.
- Default directories of other ABIs carry a suffix (`$work/prefix-x86_64`, `$work/engine-x86_64`, ...);
  pass the archive build its own `--build-dir`.

## Fast emulator

Use an x86_64 build and let the emulator render on the host GPU through ANGLE in the guest (a plain
`-gpu host` under Xvfb gives software llvmpipe, 1-2 fps):

```sh
export DISPLAY=:71   # any X display (Xvfb is fine); without one the emulator falls back to SwiftShader
emulator -avd <avd> -port <port> -no-window -no-audio -no-boot-anim -no-snapshot \
  -gpu host -accel on -feature GuestAngle,VulkanNativeSwapchain
```

`adb shell dumpsys SurfaceFlinger | grep GLES:` must name ANGLE on the host GPU. Emulator present alone
takes ~50 ms, so performance numbers come from a real arm64 device.

## Runtime keys and diagnostics

- `render_metrics=1` in `engine.ini`: five-second logs of Present cadence, Present call time and draw
  count (not GPU timestamps). Off by default.
- `texture_anisotropy`: `0` keeps the game's techniques (default), `1` linear, `2`/`4`/`8`/`16`
  anisotropic, capped by the device. The application's graphics preferences override this key and
  `texture_quality`, `vsync`, `screen_x`, `screen_y`; changes need a restart.
- Frame pacing goes through Swappy (`src/libs/renderer_bgfx/src/shim/frame_pacing.hpp`) at the player's
  frame-rate setting; there is no `max_fps` key. Its state is logged under tag `storm-bgfx`
  (`frame pacing: ...`, every 30 s with frame-time percentiles). `adb shell setprop swappy.disable 1`
  (root) switches to unpaced `eglSwapBuffers` for an A/B with the same APK.

## Compiled script cache

`[script] cache_mode` in `engine.ini`: compiled script cache in `fs::GetCachePath()/Cache`. `2` (default) checks
the key once per session, `1` on every segment load, `0` turns it off. The key covers the program directory and
the APK interface overlay (path, size, mtime), the engine build and the game directory; contents are not hashed,
so an in-place edit with unchanged size and mtime is not detected.

## Tracy profiler (development builds)

`-DSTORM_TRACY=ON` (`cmake/tracy.cmake`, Tracy 0.14.1) links the Tracy client into a development build; use
its own build directory (e.g. `build/android-tracy`). The application refuses such a `libengine.so` for
release APKs. The client records on demand and listens on 127.0.0.1:8086 only. The viewer must be Tracy
0.14.1:

```sh
adb -s <serial> forward tcp:8086 tcp:8086
tracy-capture -o game.tracy -a 127.0.0.1 -s 30   # or connect tracy-profiler to 127.0.0.1
```

A host build (`-DSTORM_HOST_SOURCES=...`) with `-DSTORM_TRACY=ON` is profiled the same way, without the
forward.

## Host checks

`python3 tools/ci/run_host_tests.py` fetches the pinned sources into `build/host-ci` and runs every
`tests/*/run.py` (under `xvfb-run -a` without a display). `.github/workflows/host.yml` runs it with the host
build on pushes and pull requests to `develop`.

## The application contract

`app_contract.json` lists what the engine and the application exchange by name: main-loop commands, HUD
input, UI host actions and state channels, settings, script events, JNI natives and Java methods the engine
looks up. `tests/app_contract` checks the engine against it, the application's `tests/engine_contract`
checks its side against the same file in the pinned submodule. Rename one of these in the engine: change
the file in the same commit.

## Native crash reports

The engine links no crash-reporting SDK: debuggerd writes the tombstone and the
application's diagnostics export carries the backtrace with build ids; its `tools/android/symbolize.py`
resolves the frames against the release symbols.
