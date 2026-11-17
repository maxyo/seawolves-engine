# Relief preparation: PBRify's models through ncnn, on the GPU or the CPU. The game makes its maps with them
# while it runs (renderer_bgfx effects/relief_generator); storm_relief_cli does a whole game at once.
include(FetchContent)

# PBRify's normal, height and roughness models (CC0-1.0): not in the repository. The
# float models are fetched by their pins; model/make_ncnn_models.py converts them to ncnn graphs at build time, once per
# machine (storm_derived_files, cmake/StormDerivedFiles.cmake), and the results must be byte for byte the pinned ones.
# The maps' method includes the weights' digests (fx/relief_model.hpp): a new model must change them.
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/StormDerivedFiles.cmake")
set(storm_pbrify_revision b35b7f4c924936803bf79e868302d7f8f643d302)
set(storm_relief_sources "")
set(storm_relief_inputs "")
foreach(pair "normal|1x-PBRify_NormalV3.onnx|19d78f555f994219e2692876ab0fb6c3e4382503952dbe72d74db89d24870ba4"
             "height|1x-PBRify_Height.onnx|2b14afd583148cb5b0a554bf86a009b6a898aadc00d213aa3ec2544e17c835f5"
             "roughness|1x-PBRify_RoughnessV2.onnx|8a8de1bbeab70df8b2550bdc11fddd5a315a12eeb47d561ce55b38551ea52131")
    string(REPLACE "|" ";" fields "${pair}")
    list(GET fields 0 model)
    list(GET fields 1 file)
    list(GET fields 2 sha256)
    # The ONNX exports of PBRify_Remix's models by fernandotonon/QtMeshEditor-models (third_party/licenses/PBRify-*).
    storm_fetch_declare(storm_pbrify_${model}
        URL https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/${storm_pbrify_revision}/${file}
        URL_HASH SHA256=${sha256}
        DOWNLOAD_NO_EXTRACT TRUE
        STORM_POPULATE_ONLY)
    FetchContent_MakeAvailable(storm_pbrify_${model})
    list(APPEND storm_relief_sources "${storm_pbrify_${model}_SOURCE_DIR}")
    list(APPEND storm_relief_inputs "${sha256}")
endforeach()
# The graphs and their pins; the weights' digests are fx::kRelief*ModelDigest's.
set(storm_relief_outputs
    "pbrify-normal.ncnn.param||6f6d55ceb44b7b0387e4c6cab96a10ee548ce0939e24d19de3189659627f5c9c"
    "pbrify-normal.ncnn.bin|kReliefNormalModelDigest|bc13e7082ffb70533a7e3cc91f1e99be0346fad3913b952d863e3f6d0bfc58cf"
    "pbrify-height.ncnn.param||6f6d55ceb44b7b0387e4c6cab96a10ee548ce0939e24d19de3189659627f5c9c"
    "pbrify-height.ncnn.bin|kReliefHeightModelDigest|d54154e6179a217891516c7c8a00033a10c8447074728f89f234b7d49b6c5226"
    "pbrify-roughness.ncnn.param||6f6d55ceb44b7b0387e4c6cab96a10ee548ce0939e24d19de3189659627f5c9c"
    "pbrify-roughness.ncnn.bin|kReliefRoughnessModelDigest|3fe407f25ac9f0d0585209483f8e4920f5d8698129e86b666fe1ffd9edf9c908")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../renderer_bgfx/fx/include/storm/fx/relief_model.hpp" relief_model_header)
set(storm_relief_pins "")
set(storm_relief_ncnn_files "")
foreach(pair ${storm_relief_outputs})
    string(REPLACE "|" ";" fields "${pair}")
    list(GET fields 0 file)
    list(GET fields 1 constant)
    list(GET fields 2 expected)
    if(constant)
        string(REGEX MATCH "${constant} = 0x([0-9a-f]+)ull" digest_line "${relief_model_header}")
        string(SUBSTRING "${expected}" 0 16 prefix)
        if(NOT CMAKE_MATCH_1 STREQUAL prefix)
            message(FATAL_ERROR "fx::${constant} (${CMAKE_MATCH_1}) is not the sha256 prefix of ${file} (${prefix})")
        endif()
    endif()
    list(APPEND storm_relief_pins "${file}=${expected}")
    list(APPEND storm_relief_ncnn_files "${file}")
endforeach()
storm_derived_files(storm_relief_ncnn_models storm_relief_ncnn_models
    SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/model/make_ncnn_models.py"
    REQUIREMENTS "${CMAKE_CURRENT_SOURCE_DIR}/model/requirements-ncnn.txt"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/model/pins.py"
    SOURCES ${storm_relief_sources}
    INPUTS ${storm_relief_inputs}
    OUTPUTS ${storm_relief_pins})
# PBRify's 4x upscaler (CC0-1.0, fx/upscale.hpp): model/make_upscale_model.py exports the release's
# .pth with spandrel and converts it by pnnx to an ncnn graph, but those bytes depend on the CPU's ISA (an AVX2 machine
# makes other ones than the AVX-512 reference machine). So the build fetches the reference machine's files by sha256
# from the engine's release models-upscale-v1, one download per file; the host target storm_upscale_model_check makes
# them again with the script and fails unless every file matches its pin (run it on the reference machine after
# changing the script, its requirements or the release's .pth, then publish a new release and change the pins here).
# fx::kUpscaleModelDigest is the weights' prefix.
set(storm_upscale_release https://github.com/maxyo/seawolves-engine/releases/download/models-upscale-v1)
set(storm_upscale_pin a0a97f7dd9400708ff5fc784c6c96e62cc47a357d102939ea01a5ebed19e97be)
set(storm_upscale_files
    "param|pbrify-upscale.ncnn.param|2cbb837676788bcb187766e8e47f35cb312f1e63ca341a09a84ab02b1c90ebdd"
    "bin|pbrify-upscale.ncnn.bin|${storm_upscale_pin}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../renderer_bgfx/fx/include/storm/fx/upscale.hpp" upscale_header)
string(REGEX MATCH "kUpscaleModelDigest = 0x([0-9a-f]+)ull" digest_line "${upscale_header}")
string(SUBSTRING "${storm_upscale_pin}" 0 16 prefix)
if(NOT CMAKE_MATCH_1 STREQUAL prefix)
    message(FATAL_ERROR "fx::kUpscaleModelDigest (${CMAKE_MATCH_1}) is not the sha256 prefix of pbrify-upscale.ncnn.bin (${prefix})")
endif()
set(storm_upscale_copies "")
set(storm_upscale_pins "")
foreach(entry ${storm_upscale_files})
    string(REPLACE "|" ";" fields "${entry}")
    list(GET fields 0 part)
    list(GET fields 1 file)
    list(GET fields 2 sha256)
    storm_fetch_declare(storm_pbrify_upscale_${part}
        URL ${storm_upscale_release}/${file}
        URL_HASH SHA256=${sha256}
        DOWNLOAD_NO_EXTRACT TRUE
        STORM_POPULATE_ONLY)
    FetchContent_MakeAvailable(storm_pbrify_upscale_${part})
    list(APPEND storm_upscale_copies "${storm_pbrify_upscale_${part}_SOURCE_DIR}/${file}")
    list(APPEND storm_upscale_pins "${file}=${sha256}")
endforeach()
if(NOT ANDROID)
    storm_fetch_declare(storm_pbrify_upscaler
        URL https://github.com/Kim2091/PBRify_Remix/releases/download/v1.7.2/PBRify_Remix-1.7.2.zip
        URL_HASH SHA256=d787a88f644c542620f869e0c61e71f795488020fcece9a97e774ca2c2cecb0b
        STORM_POPULATE_ONLY)
    FetchContent_MakeAvailable(storm_pbrify_upscaler)
    storm_derived_files(storm_upscale_model_check storm_upscale_model
        SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/model/make_upscale_model.py"
        REQUIREMENTS "${CMAKE_CURRENT_SOURCE_DIR}/model/upscale-requirements.txt"
        SOURCES "${storm_pbrify_upscaler_SOURCE_DIR}"
        INPUTS d787a88f644c542620f869e0c61e71f795488020fcece9a97e774ca2c2cecb0b
        OUTPUTS ${storm_upscale_pins}
        PIP_ARGS --index-url https://download.pytorch.org/whl/cpu --extra-index-url https://pypi.org/simple)
endif()
# The graphs in one folder, as the game reads them (relief.hpp kModelFiles): <build>/game/assets on Android (packaged),
# <build>/relief_models on the host (engine.ini relief_models, the checks).
if(ANDROID)
    set(STORM_RELIEF_MODELS "${CMAKE_BINARY_DIR}/game/assets")
else()
    set(STORM_RELIEF_MODELS "${CMAKE_BINARY_DIR}/relief_models")
endif()
set(storm_relief_copies "")
foreach(file ${storm_relief_ncnn_files})
    list(APPEND storm_relief_copies COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${storm_relief_ncnn_models_DIR}/${file}" "${STORM_RELIEF_MODELS}/${file}")
endforeach()
foreach(path ${storm_upscale_copies})
    list(APPEND storm_relief_copies COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${path}" "${STORM_RELIEF_MODELS}/")
endforeach()
add_custom_target(storm_relief_model_files
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${STORM_RELIEF_MODELS}" ${storm_relief_copies}
    VERBATIM)
add_dependencies(storm_relief_model_files storm_relief_ncnn_models)

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/ncnn.cmake")

add_library(storm_relief_core STATIC prepare.cpp relief.hpp tx_file.cpp tx_file.hpp backend.cpp backend.hpp upscale.cpp upscale.hpp)
target_compile_features(storm_relief_core PUBLIC cxx_std_20)
target_include_directories(storm_relief_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../util/include") # fnv_hash.hpp
target_link_libraries(storm_relief_core PUBLIC storm_bgfx_fx_core storm_texture_core PRIVATE ncnn)
if(ANDROID)
    target_link_libraries(storm_relief_core PUBLIC log)
endif()
set_target_properties(storm_relief_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The same job from a shell: host checks, and timing on a device or emulator (adb push with the graphs).
add_executable(storm_relief_cli cli.cpp)
target_link_libraries(storm_relief_cli PRIVATE storm_relief_core)
if(NOT ANDROID)
    # tests/renderer_bgfx/run.py: the preparation with the real models on a small synthetic game.
    add_executable(storm_relief_device_retry_check tests/device_retry_check.cpp)
    target_link_libraries(storm_relief_device_retry_check PRIVATE storm_relief_core)
    # The finalization alone (finalizer.hpp) on synthetic planes: real encoders, writers and CPU admission, no models.
    add_executable(storm_relief_finalizer_check tests/finalizer_check.cpp)
    target_link_libraries(storm_relief_finalizer_check PRIVATE storm_relief_core)
    # The .tx reader (tx_file.hpp) on synthetic files: the levels it reads, and damaged files refused.
    add_executable(storm_relief_tx_check tests/tx_file_check.cpp)
    target_link_libraries(storm_relief_tx_check PRIVATE storm_relief_core)
    add_executable(storm_relief_check tests/relief_check.cpp)
    target_link_libraries(storm_relief_check PRIVATE storm_relief_core)
    add_dependencies(storm_relief_check storm_relief_model_files)
    # The models' directory for the runner (not a compile definition: absolute paths there defeat ccache).
    file(WRITE "${CMAKE_BINARY_DIR}/relief_models_path.txt" "${STORM_RELIEF_MODELS}")
endif()

