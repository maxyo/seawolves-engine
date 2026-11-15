# Far LODs

Islands, ships and outdoor locations are drawn with fewer triangles where they are far: every object of the model is
split into spatial clusters, every cluster has a chain of simplified levels, and each frame each cluster is drawn at the
coarsest level whose error stays under the preset's pixels (`fx::Settings::far_lod_pixels`: «Низкая» 2, «Средняя»
1.5, «Высокая» 1; graphics option `fx_far_lod` in tenths of a pixel, 0 = off). The levels are made from the player's
own `.gm` files on first load, by [meshoptimizer](https://github.com/zeux/meshoptimizer) 1.3 (MIT,
`third_party/licenses/meshoptimizer-MIT.txt`), and kept in the cache; nothing of the game is shipped.

## The levels (far_lod.hpp, no engine dependencies)

- **Clusters:** the object's triangles in meshoptimizer's spatial order, cut every ~1536 triangles. Triangles of one
  object that tie in the depth test (`DepthTiePairs`: coplanar, overlapping; decals a few millimetres off their
  surface) go to one cluster.
- **Levels:** level 0 is the cluster's own triangles; level 1..5 are simplified within 0.05, 0.2, 0.8, 3.2, 12.8 m
  (`meshopt_simplifyWithAttributes`, normal and uv0 as attributes, Permissive), then the parts smaller than the error
  that touch no seam are dropped. A level that lost a seam edge is made again with that edge's triangle locked. A
  level that would keep more than 90 % of the previous one is that one.
- **Only indices:** every level draws the object's own vertices. Vertex buffers, the baked colours (`.col`, by file
  vertex number), lightmap uvs, the collision and its BSP are untouched.
- **No cracks:** a vertex whose position another cluster or another object of the model also uses is locked, so
  neighbours at any two levels still share their edges. Boundaries between kept tie triangles and the
  simplified surface are checked too, even within one cluster. Seams between two models (a hull and its mast nodes,
  two models of a location) are not locked by the per-model builder.
- **Depth ties** (`ModelDepthTies`, over the whole model): tie triangles are never simplified. Those tying inside
  their object come last in every level, in the file's order (the game's less-or-equal depth test keeps the later
  one on top); a decal in an object of its own keeps the surface under it exact (objects keep their draw order).
- **Kept triangles:** triangles that tie with another object's are removed from the simplifier's input and appended
  unchanged to every level; every vertex at their corner positions is locked, so the simplified surface still meets
  them (checked even inside one cluster). Not protected yet (follow-up): decals raised 1 cm or more above their
  surface, and decals in a separate `.gm` (their placement is not part of a model's input). A first attempt that kept
  every overlapping parallel pair within 12.8 m exact made the game check several times slower (pair search in
  12.8 m distance bins) and was dropped.
- **Cache form:** the cluster table and the indices (`meshopt_encodeIndexBuffer`, ~2-3 bytes per triangle of level 0
  for all levels together). `Decode` checks range ownership, level-zero totals, level monotonicity, finite bounds
  and errors, and every index against the model. It rejects malformed structures; it does not prove that a changed
  but structurally valid payload still matches the source geometry.

## In the engine (geometry/src/geom_far_lod.*)

- **Which models:** `far_lod::Wanted`: `islands\`, `ships\`, and `locations\` models of at least 50 m radius
  (outdoor places), never models with animated vertices. A model whose layout fails `CheckMeshLayout` gets none
  (error).
- **Making them:** while loading an eligible model with far LODs enabled, GEOM copies its triangles and vertex
  position, normal and uv0 directly from the still-locked upload buffers. It never reads GPU buffers back. The
  background job is posted immediately on model load, so a model that is never drawn does not retain an idle source
  copy. The load stage reads the cache once (texture cache directory, kind `STMFLOD1`, key: the objects, triangles
  and every vertex's position, normal and uv0 — not its colour). A miss or damaged entry proceeds to a build on the
  bakes' threads (`fx::ParallelRanges`: nice 10, thermal pause, cancelled when the model is released), then writes
  the cache. Encoding or writing failure is logged and keeps the successfully built LODs in memory. Finishing a
  build, cache load or failure releases the source buffers and triangles.
- **Installing them:** the frame service's `OnFrameBegin` installs completed work before scene submission and
  allocates the LOD index buffer, all three passes' composition buffers, and the vectors used to choose levels and
  compose their indices. These allocations suppress the loading-progress callback, so installation cannot open
  a nested progress frame. `Ready` only checks readiness; `Draw` creates no index buffers or working vectors.
  A missing allocation falls back to drawing the whole model (or the affected object/pass) and is logged.
  `far LOD: <model>: ...` reports a successful build (info) or cache load (debug). Jobs dropped when the bakes shut
  down with the device are reposted by a later frame-begin callback, with a log message.
- **Switching off:** models loaded with far LODs off capture no source and queue no job. Switching off while a job
  is pending cancels it and releases its source once the worker relinquishes it. That model remains whole after
  switching on again until it is reloaded. Already installed LODs remain available and can resume when enabled.
- **Choosing:** `NODER::Draw` gives the view in the node's space (`storm/render/far_lod.hpp`): eye and view direction
  of the view being drawn (`inverse(GetView())`: the sea's reflection and cube faces set their own),
  scale, pixels per unit of the target being drawn (its viewport and projection; orthographic too) and the depth range.
  A level's error in pixels is its error over the distance to the cluster's sphere, widened to the screen's corner, so
  turning the camera never changes a level. A cluster goes coarser only when the next level is 1.25× under the limit.
- **Drawing:** an object of one cluster draws its level's range of the model's LOD index buffer. An object of several
  clusters draws a list composed for the chosen levels (nearest cluster first) in an index buffer of its own per pass
  (scene, sea reflection, sea cube faces), recomposed only when a level changes; a depth slice of the island
  (`ISLAND::Realize`) draws only the run of clusters that reaches into its depth range. A draw from a buffer written
  this frame takes its indices as they are at that draw (the shim copies them into the frame's transient indices), so
  a recomposition costs that copy once per draw in its frame. No index buffer for a list (logged) draws that object
  whole.
- **Counters:** the effects profile line and the scene sweep's renderer window: `far_lod_<main|refl|cube>_kt`
  (drawn) and `_full_kt` (what those objects have), composes, compose kB, `island_slices`, models pending.

## Checks

- `python3 tools/ci/run_host_tests.py --only far_lod`: host checks of the real simplifier and GEOM runtime.
  Synthetic geometry covers bumpy grids meeting at a seam, a decal object over a grid (kept exact, sealed seams),
  all three projection-axis rotations with an explicit nonempty oracle, empty/broken layouts,
  malformed cache structures and codec data; the result must not depend on the threads. Runtime checks compile
  the actual GEOM translation units and advance their posted bake callback with recording frame/cache services.
  To run directly with extracted dependency sources, use `python3 tests/far_lod/run.py --build-dir build/far-lod
  --dependency-sources <parent-of-fmt-and-spdlog> --glm-source <glm-source> --meshoptimizer-source <meshoptimizer-source>`.
  Add `--sanitize` to the direct invocation for ASan/UBSan.
- `python3 tools/game_checks/far_lod/run.py <game directory>`: every model of the game that gets far LODs (game-data
  check, before a release; ~10 min). Both check, per object and cluster (`tests/far_lod/verify.hpp`): level 0 is
  exactly the object's triangles, levels use only their cluster's vertices, every edge shared with another cluster or
  object at level 0 is still drawn at every level, fixed/simplified surface boundaries retain both sides,
  tie triangles stay last and in order, triangles tying with another object's stay at every level, and
  Encode/Decode round trips. The game check prints depth-pair counts, fixed/surface seams, triangles kept per level,
  encoded bytes and build time by folder.

Bump `kVersion` whenever `Build` or `Encode` changes their result: every cache entry is made again.
