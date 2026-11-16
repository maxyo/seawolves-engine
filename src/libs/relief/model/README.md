# The PBRify models of the relief maps

Not in the repository (the owner's rule): the build makes them. The game runs ncnn graphs (fp16 weights) of
Kim2091/PBRify_Remix's SPAN models (CC0-1.0), the same files on the GPU and on the CPU (`../backend.hpp`):

| Graph | Made of | `.bin` sha256 (the `.param` pins are in `../CMakeLists.txt`) |
| --- | --- | --- |
| `pbrify-normal.ncnn.param` / `.bin` | 1x-PBRify_NormalV3 | `bc13e7082ffb70533a7e3cc91f1e99be0346fad3913b952d863e3f6d0bfc58cf` |
| `pbrify-height.ncnn.param` / `.bin` | 1x-PBRify_Height | `d54154e6179a217891516c7c8a00033a10c8447074728f89f234b7d49b6c5226` |
| `pbrify-roughness.ncnn.param` / `.bin` | 1x-PBRify_RoughnessV2 | `3fe407f25ac9f0d0585209483f8e4920f5d8698129e86b666fe1ffd9edf9c908` |
| `pbrify-upscale.ncnn.param` / `.bin` | 4x-PBRify_UpscalerSPANV4 | `a0a97f7dd9400708ff5fc784c6c96e62cc47a357d102939ea01a5ebed19e97be` |

They stay CC0-1.0 (`third_party/licenses/PBRify-CC0-1.0.txt`; provenance in `PBRify-PROVENANCE.txt`). Input 1x3xHxW
(blob `in0`), output 1x3xHxW (the upscaler 1x3x4Hx4W; blob `out0`), 0..1, any size. The first 8 bytes of each `.bin`
digest are `fx::kRelief*ModelDigest` / `fx::kUpscaleModelDigest`: part of the maps' and the ×2's methods, so a new graph
makes them again.

## How the build makes them

`../CMakeLists.txt` fetches the three relief networks as the ONNX exports by fernandotonon/QtMeshEditor-models
(revision b35b7f4c, CC0-1.0) by URL and sha256 (`storm_fetch_declare`: one shared copy per machine in
`STORM_DEPS_CACHE`; offline, `FETCHCONTENT_SOURCE_DIR_STORM_PBRIFY_{NORMAL,HEIGHT,ROUGHNESS}` name folders with the
files), and the PBRify_Remix release v1.7.2 for the upscaler's `.pth`. `storm_derived_files`
(`cmake/StormDerivedFiles.cmake`) makes the graphs: a venv with exactly the pinned requirements (pip, binary wheels, no
dependency resolution; offline, `STORM_PIP_WHEELHOUSE` names a folder of those wheels), then

- `make_ncnn_models.py` (`requirements-ncnn.txt`: pnnx, ncnn's converter, its standalone binary): each ONNX graph into
  ncnn's;
- `make_upscale_model.py` (`upscale-requirements.txt`: torch CPU and spandrel for the export, pnnx): the `.pth`
  exported to ONNX (opset 17) with spandrel, then converted by pnnx;

every file checked against its pinned sha256 (another toolchain that makes other bytes fails the build), then kept in
`STORM_DEPS_CACHE/derived/<name>-<digest>` for every build directory. The Android build copies them into
`<build>/game/assets`; host builds write the folder into `<build>/relief_models_path.txt`.

To change a model or the tools: edit the script or its requirements, make the files once by hand (a venv with the
requirements, then `<script> --sources <dir> --output <dir>`), and update the sha256 pins in `../CMakeLists.txt` and
`fx::kRelief*ModelDigest` / `fx::kUpscaleModelDigest`.

## Accuracy

The fp16 graphs against the float models (onnxruntime fp32) on 10 TEHO textures at 512 px, inner area: on the CPU
(fp16 weights, fp32 arithmetic) 84..87 dB, mean normal error 0.01 degrees; on the GPU (fp16 storage and arithmetic)
normal 57 dB / 0.19 degrees, height and roughness alike, x4 63 dB. The int8 models the CPU ran before: normal 40 dB /
1.77 degrees, height 45 dB, roughness 44 dB, x4 38 dB. The fp16 graphs against the float models: 58 dB on a brick
pattern. ncnn on the CPU against ncnn on the GPU: 57..62 dB
on 8 TEHO textures at 512 px (only their arithmetic differs). Until 2026-11 the CPU ran int8 quantisations of the same
models through onnxruntime; ncnn's graphs against them: 35..47 dB (the int8 models' own error; side by side the
difference is grain, no structure).

## The upscaler

`pbrify-upscale.ncnn.param` (sha256 `2cbb837676788bcb187766e8e47f35cb312f1e63ca341a09a84ab02b1c90ebdd`) and
`pbrify-upscale.ncnn.bin` (sha256 `a0a97f7dd9400708ff5fc784c6c96e62cc47a357d102939ea01a5ebed19e97be`) are made by
`make_upscale_model.py` from Kim2091/PBRify_Remix release v1.7.2's `4x-PBRify_UpscalerSPANV4.pth` (CC0-1.0; the release
and the file pinned by sha256): exported with spandrel, converted by pnnx (fp16 weights). Input 1x3xHxW, output
1x3x4Hx4W, 0..1.

Unlike the relief models, these bytes depend on the CPU: GitHub's AVX2 runners make another ncnn `.bin` than the
AVX-512 reference machine that made the pins. So the build does not make them: it fetches the reference machine's files
by sha256 from the engine's GitHub release `models-upscale-v1` (CC0-1.0, noted there), one `storm_fetch_declare` per
file (offline, `FETCHCONTENT_SOURCE_DIR_STORM_PBRIFY_UPSCALE_{PARAM,BIN}` name folders with the files; the release's
int8 file is no longer used). The host target `storm_upscale_model_check` makes them again with the script, as a
`storm_derived_files` (`STORM_DEPS_CACHE/derived/storm_upscale_model-<digest>`; delete that folder to make them again
for the same inputs), and fails unless every file matches its pin. To change the model: edit the script or its
requirements, build `storm_upscale_model_check` on the reference machine with the new pins, publish the files as a new
release (`models-upscale-v<N>`) and change the release and the pins in `../CMakeLists.txt`. `fx::kUpscaleModelDigest`
(fx/upscale.hpp) is the `.bin`'s prefix, part of the ×2 textures' method and of the relief maps' (the maps of small
textures are made from the ×2).
