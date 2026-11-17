# fx-preview

Host harness for `src/libs/renderer_bgfx/fx`: renders procedural test scenes through the real passes with bgfx on a
headless OpenGL ES 3.0 context and writes PNG files for review. It uses no game data.

```sh
cmake -S tools/fx-preview -B build/fx-preview -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/fx-preview                          # fx_preview, storm_fx_core_check, ES 3.0 shader blobs
cmake --build build/fx-preview --target fx_shader_profiles   # every shader also for SPIR-V and Metal
ctest --test-dir build/fx-preview --output-on-failure   # CPU-side unit checks
build/fx-preview/fx_preview <output directory> [name filter]
```

- bgfx is the game's: the pin and patches of `src/libs/renderer_bgfx/cmake/bgfx_pin.cmake`, and the shared host
  shaderc of that pin (`storm_shaderc.cmake`); offline builds pass
  `-DFETCHCONTENT_SOURCE_DIR_STORM_{BGFX,BX,BIMG,BGFX_CMAKE}=<pinned checkouts>` (bgfx patched).
- Needs EGL and GLES development packages. Without a display use Mesa's software renderer:
  `EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 build/fx-preview/fx_preview out`.
- `--height-maps=<folder>` replaces the procedural height textures of the normal-map scene with
  `bump_houses.pgm`, `rockBump.pgm`, `woodBump.pgm` and `sailBumpU1.pgm` (binary 8-bit PGM) converted from
  your own copy of the game, for local look-dev; those shots get the suffix `_game`. Never commit such
  files or the images made from them.
- Write the images outside the repository (or into an ignored directory); they are review evidence, not sources.

The run fails (exit code 1) if the package reports an error or if `ConstrainSettings()` has to change a
shot's settings: every image shows exactly the settings in its name.
The sky, the original fixed-function lighting and the sea are stand-ins, so shots show what the package changes,
not how the game looks.
