# Far LODs

Islands, ships and outdoor locations are drawn with fewer triangles where they are far: every object of the model is
split into spatial clusters with a chain of simplified index-only levels, and each frame each cluster is drawn at the
coarsest level whose error stays under the preset's pixels (`fx::Settings::far_lod_pixels`; graphics option
`fx_far_lod` in tenths of a pixel, 0 = off). The levels are made from the player's own `.gm` files on first load by
[meshoptimizer](https://github.com/zeux/meshoptimizer) (MIT, `third_party/licenses/meshoptimizer-MIT.txt`) and kept in
the cache; nothing of the game is shipped. `far_lod.hpp` has no engine dependencies; the engine side is
`src/libs/geometry/src/geom_far_lod.*`.

Bump `kVersion` whenever `Build` or `Encode` changes their result: every cache entry is made again.

## Checks

- `python3 tools/ci/run_host_tests.py --only far_lod`: host checks of the simplifier and the GEOM runtime on
  synthetic geometry. Directly: `python3 tests/far_lod/run.py --build-dir build/far-lod
  --dependency-sources <parent-of-fmt-and-spdlog> --glm-source <glm-source> --meshoptimizer-source <meshoptimizer-source>`
  (`--sanitize` for ASan/UBSan).
- `python3 tools/game_checks/far_lod/run.py <game directory>`: every model of the game that gets far LODs (game-data
  check, before a release; ~10 min), with the invariants of `tests/far_lod/verify.hpp`.
