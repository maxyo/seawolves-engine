# Relief maps from PBRify

Normal maps and material textures (R the height of the parallax and the cavity, G the gloss) for the colour textures
that get relief (fx `ClassifyTextureName`: every surface but characters, sails and flags, rigging, foliage, water,
glass, sky, UI, effects and light maps) are made from the player's
game files with PBRify's normal and height models (`fx/relief_model.hpp`) through ncnn, and kept in the relief
directory (`fx/relief_store.hpp`). Nothing derived is shipped. Grey "bump" textures are height maps; the game derives
their maps itself. The owner's choice: PBRify for both maps, its normals as the model gives them (no rescale).

## The neural ×2 first

A texture whose larger side is at most the map's size (`fx::ReliefMapsFromUpscale`, 512) is upscaled before its maps:
PBRify's 4x SPAN upscaler (`upscale.hpp`, CC0-1.0) on the device the relief's networks use (`backend.hpp`: its ncnn
graph on either, the GPU tiling it again by its calibrated size), in 256-texel tiles padded by 24
(wrapped for the tiling stone, wood and plaster, mirrored for atlases), its ×4 box-filtered to ×2 in linear light, the
alpha stretched bilinearly. The normal and height networks run on that ×2 at twice the map's size and their planes are
box-filtered to the map; the roughness runs on it at the gloss's size. The maps keep their sizes. Measured against
the maps from the texture itself (local-testing/pom/pbrify-set, x2.json and inter.json): half the height noise and
DXT steps, the stone's shape 8..26 % stronger, the normals' tilt the same (0.164 against 0.168); their inference takes
four times as long. Larger textures have the detail and make their maps from themselves.

The same ×2 is the game's display texture (renderer `src/effects/upscale.hpp`, fx/upscale.hpp): with
`UpscaleOutput` the preparation keeps it in the upscale directory encoded for the device's tier; `Session::Upscale`
makes it alone for the other textures the game shows at ×2 (characters, textures of 1024). `storm_relief_cli --x2-list`
lists a game's ×2 targets, `--x2` makes them (host renders).

## Where the maps are made

In the game process, lazily (renderer_bgfx `src/effects/relief_generator.hpp`): a texture loaded without its map is
requested; one worker thread (nice 10; while the game is played ncnn's CPU networks on a quarter of the cores, at most two,
one when the device is hot, paused at thermal status SEVERE and by explicit renderer holds; all cores while a place
loads) makes the most recently drawn first, then the other required loaded textures.
Normal/gloss and ×2 maps required by scene loading are prepared before the readiness gate reveals the scene;
textures loaded later still request their own missing maps. After a location is revealed, its direct neighbours (the
locations and the island's sea its exits lead to: `render::SetEffectsNeighbours`, from the location's script
attributes and the texture names in its neighbours' models) are made ahead with no loaded request waiting: the
neighbour whose exit is nearest the player first, at most `kNeighbourMaps` (24) new maps per place, never while the
device is hot (`device_hot`) or the networks wait for the GPU on a CPU detour; then the worker is idle
(`effects: relief generation: the neighbours of <place> are done: ...`). Nothing else of the game is listed, pruned
or prepared in the game process. Existing valid cached maps are still consumed; a newly made map is uploaded and its
relief fades in over 0.3 s. The host's `storm_relief_cli` makes a whole game at once (below).
A map's key holds its method (`fx::ReliefMethod`: the method version, the
models' digests `fx::kReliefNormalModelDigest`, `fx::kReliefHeightModelDigest`, `fx::kReliefRoughnessModelDigest`, the height's and the gloss's versions, the class's options with the derivation's version, the map size), so a
change never reuses a map made before it; the host's whole-game pass prunes the maps of other methods. The maps are a
cache: on Android the relief directory is in the app's cache, so «Очистить кэш» (or the system evicting it) removes
them and the game makes them again lazily. At start the stores of earlier builds are removed (logged): the files
directory's `relief`, kept across «Очистить кэш» (the app removes the old locations in its caches, GameCache); the
store in use is never removed as an old one. Each texture logs the key and method of its map when it is
looked up (`effects: relief <name> (<class>): map <key> of method <method>`). The models
are loaded at the first job and released after 10 s with no work left. The "effects relief:" log line counts the
textures waiting; each map logs its stages (`relief <name>: WxH, N ms (read, decode,
models of which inference, mips, material (depth), encode, write)`).

`storm_relief_cli <RESOURCE/Textures> <relief dir> <model dir> [map size] [threads]` makes a whole game at once:
host renders with every map in place (engine.ini `relief_dir = <relief dir>`). A host engine makes maps itself only
with engine.ini `relief_models = <the directory with the three model files>`; without it, it logs that no maps are
made. The build makes that directory (below); its path is in `<build>/relief_models_path.txt` of a host build. The
whole-game pass leaves the characters' atlases out (the game makes those only for the characters a location loads);
`storm_relief_cli <RESOURCE/Textures> <relief dir> <model dir> --files <texture>...` makes the named textures, of any
class, before a host render that shows them.

## Models and runtime

- Models: `pbrify-normal.ncnn.*`, `pbrify-height.ncnn.*` and `pbrify-roughness.ncnn.*` (CC0-1.0), ncnn graphs (fp16
  weights) of Kim2091/PBRify_Remix's 1x-PBRify_NormalV3, 1x-PBRify_Height and 1x-PBRify_RoughnessV2, not kept in the
  repository: the build fetches the float models by their pins and converts them with pnnx once per machine
  (`model/README.md`; provenance in `third_party/licenses/PBRify-PROVENANCE.txt`). Their digests are pinned in this
  folder's CMakeLists.txt and the weights' match `fx::kReliefNormalModelDigest`, `fx::kReliefHeightModelDigest` and
  `fx::kReliefRoughnessModelDigest`. Per texture: one decode, then one inference of each
  model on the whole image at the map's size, wrap-padded by `fx::kReliefModelPad` (24 px, beyond the networks'
  receptive field: no seam where the texture tiles); the roughness model at the gloss map's size.
- Parallax trust (`fx/parallax_trust.hpp`): after the height, a per-texel mask from the colour texture and the height (classical image processing, no model) marks recesses (windows, doors) and flat dark glass; the height goes to the top there (no parallax), the mask and its coverage go into the file. The log line of each map ends its depth with `parallax trust N%`.
- Height (`fx/height_map.hpp`): the height model's output, each atlas region normalised against its own neighbourhood
  (`fx::kHeightRegionTexels`), then its 2 % / 98 % become 0 and 1; the class's depth (`fx::ReliefDepthTexels`) is stored
  with it. Gloss (`fx/gloss_map.hpp`): the roughness model at 256² (`fx::kGlossMapSize`), corrected by
  `fx::GlossOfRoughness` (the height's grooves, dark texels, the metal mask, the class caps), stretched into G. The
  material texture is stored beside the normals in the same file (EAC RG11 with mips) for every relief texture; the
  textures of a gloss class without relief (glass...) get their own gloss map
  (`<key>.gloss`: R the metal mask, G the gloss) from the roughness and the height models at 256², in the same queue.
- Devices (`backend.hpp`): ncnn 20261020 (BSD-3-Clause; `cmake/ncnn.cmake`; notices in `third_party/licenses/ncnn-*`,
  `glslang-*`) is the one inference runtime, running the same graphs on the GPU (Vulkan compute, its in-house loader
  opening libvulkan at run time; fp16 storage, packing and arithmetic where the device has them, logged with
  `relief GPU model: ... ready`) or on the CPU (fp16 arithmetic on ARMv8.2 FP16 cores, fp32 otherwise, logged with
  `relief CPU runtime`; SSE / AVX picked at run time on x86-64; its own minimal OpenMP runtime, waiting threads sleeping;
  the thread count is the game's current CPU budget, applied to the next run without reloading anything; the image in
  tiles of 128 or 256 texels by the RAM, `cpu_tiles.hpp`, a smaller tile after an allocation failure, logged). No int8:
  ncnn's int8 graphs (ncnn2table + ncnn2int8 on the same procedural set) were 2.97..3.16 degrees of mean normal error
  against the float models (fp16: 0.19 on the GPU, 0.01 on the CPU) and no faster (ncnn's ARM tables: its winograd
  fp16 3x3 convolutions beat its int8 ones on Snapdragon 888 / X Elite / Cortex-A720). The
  device is measured once per game: a probe tile on both (`ChooseDevice`), the faster taken, logged with both numbers
  (`relief device: GPU (measured now on <GPU>, driver <v>: ... GPU x ms ..., CPU y ms ...)`) and kept in the relief
  directory's `relief-device.txt` for that GPU, driver, model and thread count; «Очистить кэш» measures again. That is
  the starting device; then each texture is placed (`relief.hpp` Session): on the GPU, two textures in a row 1.5 times
  slower than the CPU's estimate (the probe's, then each CPU texture's own cost per texel; a change of the CPU budget
  scales it by the thread counts until measured again) while the game draws its scene move the work to the CPU, which then makes only what is
  on screen (the rest of the game waits for the GPU); the GPU is tried again as soon as the game leaves it free (a
  loading or an interface screen, the game in the background: no scene drawn for 30 frames or no frame for a second)
  or after 60 s / 8 completed CPU textures (standalone ×2 included; without a CPU estimate to compare with, the GPU is
  kept only while the game leaves it free), when the next required texture arrives; no
  automatic whole-corpus work is admitted to trigger a retry. The ×2 compares its GPU time against measured CPU x4 time, rather
  than the unrelated normal-network probe. Scene presence comes from the renderer's perspective draws, including the
  low preset and sea; lighting-feature counters do not classify a scene as idle. Every switch is logged (`relief device: CPU for now ...`, `GPU again ...`, `CPU still
  ...`) and none is kept across sessions. Without a Vulkan compute device, or after a crash in its driver
  (`relief-gpu-crashed.txt`), the CPU alone, with the reason logged. A controlled GPU model-load/calibration error
  falls back to the CPU for that session and logs the error; an in-flight driver hang is not forcibly interrupted.
  Model-opening/calibration begin and end are logged before the first texture, separately from texture inference. On the GPU the image runs in tiles with the networks'
  24-texel reach around each (no seams), and each tile's graph in stages, one GPU submission each of at most 6 ms of
  GPU work, so the game's frames do not wait on a long one while the tiles stay large (the halo is recomputed work: a
  32-texel tile computes 6 texels for each it keeps). A stage ends at a convolution's output; the blobs later stages
  need stay on the GPU between submissions (the tile is uploaded before the first, downloaded after the last; those
  transfers are not counted as GPU work). The calibration (per graph and GPU, logged as `relief GPU tiles: N + 24
  texels in K submissions ...`) measures every convolution's part of the graph's work at two tile sizes and a
  submission's fixed wall time; the tile is the one of the least wall time per kept texel whose largest convolution fits
  6 ms (32 to 512 texels), its stages the fewest that each fit. A stage that takes longer than planned (another app,
  the GPU's clock down) halves the next tiles (`relief GPU tile guard`). An RTX 5070 Ti: 512 + 24 in 2 submissions.
  The GPU's output against the CPU's (the same graph): above 45 dB on a
  brick pattern (relief_check), 57..62 dB on game textures; against the float models 58 dB. NNAPI (deprecated) is not
  used.
- Maps: the models run on the texture downscaled to 512 px (`fx::kReliefMapSize`, the owner's choice); the GPU's
  bilinear filter upsamples it over the texture. Высокая uses it whole, Средняя from its second mip. On an x86-64 host
  with 4 threads both models take about 0.4 s for a 512 texture.

## Android

The engine build puts the eight graph files (`relief.hpp` kModelFiles) into `<build>/game/assets`; ncnn is linked into
libengine.so. The app packages them as the assets of the same names (the engine reads them through SDL_RWFromFile), and
the licence files above (`PBRify-CC0-1.0.txt`, `PBRify-PROVENANCE.txt`, `ncnn-BSD-3-Clause.txt`, `glslang-LICENSE.txt`)
on its licences screen. The relief directory: the engine's cache root (`fs::GetCachePath()`: the app's
`(getExternalCacheDir() ?: getCacheDir())/engine`) + `/relief` (engine `Renderer`).
