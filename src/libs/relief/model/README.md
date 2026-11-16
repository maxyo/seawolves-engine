# The PBRify models of the relief maps

Not in the repository (the owner's rule): the build makes them. `pbrify-normal-int8.onnx` (501 397 bytes, sha256
`fc91ce30e48deaf5847ed19342bd1fc7a8ffc11a2ec94f26c4d6bcbe63df4b33`), `pbrify-height-int8.onnx` (501 394 bytes, sha256
`01f251dc40e79fa7ef5348b51f538de5a14bc6113c8ea3173ba78ce0b648c409`) and `pbrify-roughness-int8.onnx` (501 397 bytes,
sha256 `860845c92142b7d1bd89ac514ae58206d431fd2c899e955dd492348389c5f00b`) are made by `make_models.py` from
Kim2091/PBRify_Remix's SPAN models 1x-PBRify_NormalV3, 1x-PBRify_Height and 1x-PBRify_RoughnessV2 (CC0-1.0), as ONNX
exports by fernandotonon/QtMeshEditor-models (revision b35b7f4c, CC0-1.0): static int8 quantisation (QDQ, per-channel
weights) calibrated on procedural textures with a 48 px wrap padding (no game data), unused nodes dropped.
They stay CC0-1.0 (`third_party/licenses/PBRify-CC0-1.0.txt`; provenance in `PBRify-PROVENANCE.txt`). Input and output
1x3xHxW, 0..1, any size.

## How the build makes them

`../CMakeLists.txt` fetches the three float models by URL and sha256 (`storm_fetch_declare`: one shared copy per
machine in `STORM_DEPS_CACHE`; offline, `FETCHCONTENT_SOURCE_DIR_STORM_PBRIFY_{NORMAL,HEIGHT,ROUGHNESS}` name folders
with the files) and makes the int8 files with `storm_derived_files` (`cmake/StormDerivedFiles.cmake`): a venv with
exactly `requirements.txt` (pip, binary wheels, no dependency resolution; offline, `STORM_PIP_WHEELHOUSE` names a folder
of those wheels), `make_models.py --sources <the float models' folders> --output <scratch>`, every file checked against
its pinned sha256 (another toolchain that makes other bytes fails the build), then kept in
`STORM_DEPS_CACHE/derived/storm_relief_models-<digest>` for every build directory. The first make takes about three
minutes; later builds find it. The Android build copies them into `<build>/game/assets`; host builds write the folder
into `<build>/relief_models_path.txt`.

Why a venv and not our own tool: onnxruntime's static quantisation (calibration, QDQ insertion, per-channel weights)
exists only in its Python package; its C API has no quantiser, and a re-implementation would not make the same bytes.

To change a model or the tools: edit `make_models.py` or `requirements.txt`, make the files once by hand (a venv with
`requirements.txt`, then `make_models.py --sources <dir> --output <dir>`), and update the sha256 pins in
`../CMakeLists.txt` and `fx::kRelief*ModelDigest` (they are part of the maps' method, so the maps are made again).

Against the float models on 10 TEHO textures at 512 px, the correlation of the int8 output: at least 0.9985 for the
normals and 0.9917 for the height; the roughness on 8 of them at 256 px at least 0.991 (0.9913..0.9995; a calibration on
game textures was worse, 0.967..0.997, and would put game data into the file). One 512 texture on an x86-64 host with 4
threads: about 0.19 s per model (the float model 0.52 s).

## The upscaler

`pbrify-upscale-int8.onnx` (521 540 bytes, sha256 `5148c5c90562bdff8567dd612b427ebe001371d4abb906fb1358d94e1480f552`),
`pbrify-upscale.ncnn.param` (sha256 `2cbb837676788bcb187766e8e47f35cb312f1e63ca341a09a84ab02b1c90ebdd`) and
`pbrify-upscale.ncnn.bin` (sha256 `a0a97f7dd9400708ff5fc784c6c96e62cc47a357d102939ea01a5ebed19e97be`) are made by
`make_upscale_model.py` (its own `upscale-requirements.txt`: torch CPU and spandrel for the export) from
Kim2091/PBRify_Remix release v1.7.2's `4x-PBRify_UpscalerSPANV4.pth` (CC0-1.0; the release and the file pinned by
sha256): exported with spandrel, converted by pnnx into the ncnn files for the GPU, quantised to int8 as above
(calibration on the same procedural textures with 16 px wrap or mirror padding: no game data). Input 1x3xHxW, output
1x3x4Hx4W, 0..1.

Unlike the relief models, these bytes depend on the CPU: GitHub's AVX2 runners make another int8 file and another
ncnn `.bin` than the AVX-512 reference machine that made the pins. So the build does not make them: it fetches the
reference machine's files by sha256 from the engine's GitHub release `models-upscale-v1` (CC0-1.0, noted there),
one `storm_fetch_declare` per file (offline, `FETCHCONTENT_SOURCE_DIR_STORM_PBRIFY_UPSCALE_{INT8,PARAM,BIN}` name
folders with the files). The host target `storm_upscale_model_check` makes them again with the script, as a
`storm_derived_files` (`STORM_DEPS_CACHE/derived/storm_upscale_model-<digest>`; delete that folder to make them again
for the same inputs), and fails unless every file matches its pin. To change the model: edit the script or its
requirements, build `storm_upscale_model_check` on the reference machine with the new pins, publish the files as a new
release (`models-upscale-v<N>`) and change the release and the pins in `../CMakeLists.txt`. `fx::kUpscaleModelDigest`
(fx/upscale.hpp) is its prefix, part of the ×2 textures' method and of the relief maps' (the maps of small textures are made from the ×2).

Against the float model on 8 TEHO textures (256 px crops, ×4 then boxed to ×2 in linear light): 36.8..43.0 dB PSNR,
RMSE 0.007..0.015, a quarter of the network's own change against bicubic (0.22..0.32; 0.64 on one flat signboard);
side by side the difference is grain, no structure. A calibration on game crops was no better (37.6..42.1 dB) and would
put game data into the file; keeping the first and last convolutions in float bought 0.5 dB for 25 % more time. The
int8 model takes 2.7..3x less time than the float one (onnxruntime CPU, x86-64).
