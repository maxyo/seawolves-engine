# Relief maps from PBRify

Normal maps, material textures (R height for parallax and cavity, G gloss), gloss maps and the neural ×2
textures are made from the player's own game files with PBRify's networks through ncnn (BSD-3-Clause,
`cmake/ncnn.cmake`), on the GPU (Vulkan compute) or the CPU, whichever a probe measures faster per device. Nothing
derived is shipped. Which textures get relief: fx `ClassifyTextureName`; map formats and keys:
`fx/relief_model.hpp`, `fx/relief_store.hpp`.

In the game the maps are made lazily in the background (renderer_bgfx `src/effects/relief_generator.hpp`) and
kept as a cache in `fs::GetCachePath()/relief` (on Android in the app's cache dir: «Очистить кэш» removes them and
they are made again). A map's key holds the method and model digests, so a changed method never reuses old maps.

## Memory on the CPU

The CPU networks of a session hold at most `kCpuNetworkBytes` (160 MB, `backend.hpp`) at once and return all of it
after each texture: one bounded allocator (`CpuMemory`, ncnn's allocator hook) instead of ncnn's own per-network pools,
which kept every block until the session ended (a 1024² ×2 took 440 MB, a relief texture 1.3..1.7 GB). Each network's
tiles are the largest that fit the bound by the bytes a texel it measured on this CPU (`cpu_tiles.hpp`; about 2.5 KB
with fp32 storage, half with ARMv8.2 fp16 storage); the upscaler's own tiles follow (`Network::TileCeiling`), so they are
not cut again. An allocation past the bound fails the run (ncnn's -100: a smaller tile, logged), and a texture that fits
no tile is not made (logged, unusable). The runtime's log line names the bound, each texture's line its peak. Check:
`storm_relief_check <models> --memory` (tests/renderer_bgfx/run.py).

## Host use

```sh
storm_relief_cli <RESOURCE/Textures> <relief dir> <model dir> [map size] [threads]   # a whole game
storm_relief_cli <RESOURCE/Textures> <relief dir> <model dir> --files <texture>...   # named textures, any class
storm_relief_cli --x2-list <RESOURCE/Textures>                                       # the ×2 targets
storm_relief_cli --x2 <RESOURCE/Textures> <upscale dir> <model dir> <bc 0|1> <threads> <list file>
```

A host engine reads finished maps with engine.ini `relief_dir = <relief dir>` and makes maps itself only with
`relief_models = <model dir>`; a host build writes that directory into `<build>/relief_models_path.txt`
(target `storm_relief_model_files`). The whole-game pass leaves character atlases out.

## Models and Android

The ncnn graphs are made by the build, not kept in the repository: [model/README.md](model/README.md). The
Android build puts the graph files into `<build>/game/assets`; the app packages them as assets of the same names
and shows `PBRify-CC0-1.0.txt`, `PBRify-PROVENANCE.txt`, `ncnn-BSD-3-Clause.txt` and `glslang-LICENSE.txt`
(`third_party/licenses`) on its licences screen.
