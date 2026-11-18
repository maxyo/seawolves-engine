# Graphics effects package for the bgfx renderer (see README.md).
#
# This directory is self-contained and is NOT picked up by the engine build on
# its own: the renderer (or tools/fx-preview) adds it with add_subdirectory()
# after bgfx exists. Required from the parent scope:
#   target   storm_bgfx_runtime (bgfx, bx, bimg)
#   STORM_FX_SHADERC_COMMAND       host shaderc built from the pinned bgfx
#   STORM_FX_SHADERC_DEPENDS       target/file the shader build depends on
#   STORM_FX_BGFX_SHADER_INCLUDE   <bgfx>/src (bgfx_shader.sh)
# render/bgfx can pass its STORM_SHADERC_COMMAND / STORM_SHADERC_DEPENDS and
# "${STORM_BGFX_SOURCE_DIR}/src" unchanged.
cmake_minimum_required(VERSION 3.21)
if(NOT TARGET storm_bgfx_runtime)
    message(FATAL_ERROR "renderer_bgfx/fx needs the pinned bgfx (storm_bgfx_runtime); configure tools/fx-preview or add it from renderer_bgfx")
endif()
set(STORM_FX_SHADER_PROFILE "300_es" CACHE STRING "shaderc profile for the embedded fx shader blobs")
include(cmake/storm_fx_shaders.cmake)

# madmann91/bvh (MIT, header-only, C++20): the bounding volume hierarchy of the place's ray bakes (src/sun_fit.cpp
# Occluders). Offline: -DFETCHCONTENT_SOURCE_DIR_STORM_BVH=/path/to/this/revision
include(FetchContent)
storm_fetch_declare(storm_bvh
    URL https://codeload.github.com/madmann91/bvh/tar.gz/ad9f7ee3fa54014cef8e789a8d45a3c2e7a117d0
    URL_HASH SHA256=e4c66620e358265c4c81f9442de7a43fab129b079f5145ed417d9e87e8564db5
    STORM_POPULATE_ONLY)
# SMAA (MIT): the reference SMAA.hlsl (included by shaders/include/fx_smaa.sh) and its precomputed area and search
# tables (Textures/AreaTex.h, SearchTex.h, uploaded by src/post_chain.cpp). Offline: -DFETCHCONTENT_SOURCE_DIR_STORM_SMAA=...
storm_fetch_declare(storm_smaa
    URL https://codeload.github.com/iryoku/smaa/tar.gz/71c806a838bdd7d517df19192a20f0c61b3ca29d
    URL_HASH SHA256=046a435f5713f8bfe8148fe6305c2abf1409fc2122bee6070061c675e1e55f57
    STORM_POPULATE_ONLY)
# AMD FidelityFX Super Resolution 1 (MIT): ffx-fsr/ffx_a.h and ffx_fsr1.h, the EASU / RCAS pixel shaders
# (shaders/include/fx_fsr.sh) and their constant setup on the CPU (src/post_chain.cpp).
# Offline: -DFETCHCONTENT_SOURCE_DIR_STORM_FIDELITYFX_FSR=...
storm_fetch_declare(storm_fidelityfx_fsr
    URL https://codeload.github.com/GPUOpen-Effects/FidelityFX-FSR/tar.gz/a21ffb8f6c13233ba336352bdff293894c706575
    URL_HASH SHA256=b529a31d21971ee9f2a3231bbe17e31d183ccbb7fb6e3f7b0508b2624f61c2d9
    STORM_POPULATE_ONLY)
FetchContent_MakeAvailable(storm_bvh storm_smaa storm_fidelityfx_fsr)
include(shaders/shaders.cmake)

add_library(storm_bgfx_fx_core STATIC
    src/color_lut.cpp src/noise_bake.cpp src/sky_model.cpp
    src/lighting_model.cpp src/light_falloff.cpp src/light_set.cpp src/shadow_model.cpp src/normal_map.cpp src/settings.cpp src/log.cpp
    src/ao_volume.cpp src/lamp_shadows.cpp src/bakes.cpp src/sun_fit.cpp src/town_light.cpp src/visibility.cpp src/place_probes.cpp src/relief_model.cpp src/relief_store.cpp src/gloss_map.cpp src/upscale.cpp
    src/height_map.cpp src/parallax_trust.cpp src/parallax_exclusions.cpp src/parallax_probe.cpp src/cutout.cpp src/wetness.cpp src/gpu_crash_guard.cpp
    src/skin_tone.cpp)
target_compile_features(storm_bgfx_fx_core PUBLIC cxx_std_20)
target_include_directories(storm_bgfx_fx_core PUBLIC include PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../util/include") # fnv_hash.hpp
target_include_directories(storm_bgfx_fx_core SYSTEM PRIVATE "${storm_bvh_SOURCE_DIR}/src")
find_package(Threads REQUIRED)
target_link_libraries(storm_bgfx_fx_core PUBLIC Threads::Threads) # the bake pool
set_target_properties(storm_bgfx_fx_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(storm_bgfx_fx STATIC
    src/gpu_util.cpp src/post_chain.cpp src/sea_depth.cpp src/sea_wakes.cpp src/sea_waves.cpp src/sky.cpp src/lighting.cpp src/shadows.cpp src/sun_relight.cpp src/capabilities.cpp
    src/self_test.cpp)
target_link_libraries(storm_bgfx_fx PUBLIC storm_bgfx_fx_core storm_bgfx_runtime PRIVATE storm_fx_shaders)
target_include_directories(storm_bgfx_fx SYSTEM PRIVATE "${storm_smaa_SOURCE_DIR}/Textures" "${storm_fidelityfx_fsr_SOURCE_DIR}/ffx-fsr")
# storm/render/shader_blob.hpp (header only): the embedded blob of the running backend; also for the tools that
# embed their own shaders with storm_fx_build_shaders.
target_include_directories(storm_bgfx_fx PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include")
set_target_properties(storm_bgfx_fx PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(storm_fx_core_check tests/core_check.cpp)
    target_link_libraries(storm_fx_core_check PRIVATE storm_bgfx_fx_core)
    add_test(NAME fx_core COMMAND storm_fx_core_check)
    add_executable(storm_fx_wave_source_check tests/wave_source_check.cpp)
    target_compile_features(storm_fx_wave_source_check PRIVATE cxx_std_20)
    target_include_directories(storm_fx_wave_source_check PRIVATE include)
    add_test(NAME fx_wave_source COMMAND storm_fx_wave_source_check)
    add_executable(storm_fx_wave_storage_check tests/wave_storage_check.cpp)
    target_compile_features(storm_fx_wave_storage_check PRIVATE cxx_std_20)
    target_include_directories(storm_fx_wave_storage_check PRIVATE include)
    add_test(NAME fx_wave_storage COMMAND storm_fx_wave_storage_check)
    add_executable(storm_fx_wave_idle_check tests/wave_idle_check.cpp)
    target_compile_features(storm_fx_wave_idle_check PRIVATE cxx_std_20)
    target_include_directories(storm_fx_wave_idle_check PRIVATE include)
    add_test(NAME fx_wave_idle COMMAND storm_fx_wave_idle_check)
    add_executable(storm_fx_wake_check tests/wake_check.cpp)
    target_compile_features(storm_fx_wake_check PRIVATE cxx_std_20)
    target_include_directories(storm_fx_wake_check PRIVATE include)
    add_test(NAME fx_wake COMMAND storm_fx_wake_check)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(storm_fx_aggregate_check "${CMAKE_CURRENT_LIST_DIR}/../../../../tests/renderer_bgfx/aggregate_light_check.cpp")
    target_link_libraries(storm_fx_aggregate_check PRIVATE storm_bgfx_fx_core)
    add_test(NAME fx_aggregate COMMAND storm_fx_aggregate_check)
endif()
