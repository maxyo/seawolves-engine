# Warm builds: ccache for every native build

Rule: **never build cold except in exceptional cases.** Every native build of the engine, the UI
runtime, the archive and torrent readers, host tools and CMake-based tests, in every worktree
and in the application's engine submodule, compiles through `tools/ccache/launcher` into one
shared cache.

## How it works

- `cmake/StormBuildCache.cmake`, included right after `project()` by every Storm CMake project, sets
  `CMAKE_C/CXX_COMPILER_LAUNCHER` to `tools/ccache/launcher` and **fails configuration** when ccache is missing
  or a different launcher is set. Build scripts pass the launcher to the third-party projects they build.
- `tools/ccache/launcher` sets defaults unless already set: `CCACHE_DIR=~/.cache/ccache-storm` (isolated from
  `~/.config/ccache/ccache.conf`, which belongs to another project: do not edit it, its cache, or run
  `ccache -C/-z` on it), `CCACHE_BASEDIR` = the workspace holding all worktrees, `CCACHE_NOHASHDIR=1`,
  `CCACHE_COMPILERCHECK=content`, sloppiness `include_file_ctime,include_file_mtime,time_macros,locale`,
  `CCACHE_MAXSIZE=30G`, compression, and `CCACHE_STATSLOG=<build dir>/ccache-stats.log`.
- Never put absolute paths into compile definitions (`-DX="/abs/path"`): every worktree misses. Generate a
  header into a build include directory instead (as `src/libs/diagnostics/include/watermark.hpp` does).
- Cross-worktree hits need **the same build directory depth**: `build/<name>` everywhere, or the script
  defaults (`build/android-ci/<name>`).
- Pinned FetchContent sources are fetched once per machine into `~/.cache/storm-deps/fetchcontent/`
  (`storm_fetch_declare`; `STORM_DEPS_CACHE` overrides the root) and shared read-only. Patched ones (bgfx,
  RmlUi) compile from `<build dir>/_deps/<name>-patched-<digest>` (`storm_patched_view`): links into the
  pristine copy plus the patched files; editing a patch reconfigures and rebuilds only its dependents. An
  explicit `FETCHCONTENT_SOURCE_DIR_<NAME>` elsewhere must carry every patch or configuration fails.
- The host `shaderc` is built once per machine into `~/.cache/storm-deps/shaderc-<key>`
  (`src/libs/renderer_bgfx/cmake/storm_shaderc.cmake`; `-DSTORM_SHADERC=<binary>` overrides); compiled shaders
  are cached by content in `~/.cache/storm-deps/shader-cache`.
- Android Release (ThinLTO) links with lld's ThinLTO cache `~/.cache/storm-deps/thinlto-cache`.

## Is this build warm?

```sh
tools/ccache/warmth begin <build dir>   # after configure: fails unless the launcher is set
cmake --build <build dir> ...
tools/ccache/warmth end <build dir>     # prints: N compilations, H hits (x%), M misses
```

`end` raises the **ХОЛОДНАЯ СБОРКА / COLD BUILD** alarm and exits 1 when a fresh build directory misses more
than 100 compilations at a hit rate below 60%. Genuinely new sources pass with
`STORM_ALLOW_COLD_BUILD="<reason>"`.

- `ninja -C <build dir> -n <target> | tail -1` shows how many steps would run.
- `tools/ccache/check-build-dirs` lists every build directory of the workspace without the launcher (exit 1 if
  any); reconfigure those with `cmake -S <source> -B <build dir>`.
- `tools/ccache/launcher -s` shows the shared cache's totals.

## The exception: building without the cache

Configure with `STORM_ALLOW_NO_CCACHE="<reason>"` (environment or `-D`; the value is the reason, `1`/`ON` are
rejected). Every compile then prints `STORM ALARM: UNCACHED COMPILE` (`tools/ccache/uncached`). End it with
`cmake -U STORM_ALLOW_NO_CCACHE <build dir>`.

## Scripts that run the compiler directly

Checks that call `$CXX` themselves get `CXX=<repo>/tools/ccache/cxx` from `tools/ci/run_host_tests.py` and
`tools/android/ci_dependencies.sh`; it runs `$STORM_CCACHE_CXX` (default `c++`) through the launcher and splits
compile-and-link commands into cached `-c` compiles plus the link (`STORM_CXX_NO_SPLIT="<reason>"` turns that
off). By hand: `CXX=tools/ccache/cxx STORM_CCACHE_CXX=clang++ python3 tests/<name>/run.py`. With
`STORM_CXX_DEPS=<file>` every compile appends the files it read; `tools/ci/affected.py` selects checks by them.
