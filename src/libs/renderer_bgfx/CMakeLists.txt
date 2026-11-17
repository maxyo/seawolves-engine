# bgfx is the engine's only GPU backend. This directory owns the pinned
# bgfx/bx/bimg dependency, the offline shader build and the D3D9-shaped shim
# (see README.md). Other libraries link `storm_bgfx` and never fetch bgfx.
cmake_minimum_required(VERSION 3.21)
include(FetchContent)
# Third-party code must not inherit the engine's global flags (UNICODE, the
# Windows type headers, -std=c++20 for C sources ...).
set_directory_properties(PROPERTIES INCLUDE_DIRECTORIES "" COMPILE_DEFINITIONS "" COMPILE_OPTIONS "")
# bgfx runs its GL backend on its own render thread, which owns the EGL context; the game thread records frames
# and hands them over in bgfx::frame() (shim/device.hpp, Context). A host that must stay on one thread calls
# bgfx::renderFrame() before bgfx::init (the launcher's screens under a GLSurfaceView, ui/android/jni_host.cpp;
# the engine with render_thread=0).
set(BGFX_CONFIG_MULTITHREADED ON CACHE BOOL "" FORCE)
set(BGFX_CONFIG_MAX_TEXTURES 8192 CACHE STRING "" FORCE)
set(BGFX_CONFIG_MAX_VERTEX_LAYOUTS 256 CACHE STRING "" FORCE)
set(BGFX_CONFIG_MAX_DYNAMIC_VERTEX_BUFFERS 8192 CACHE STRING "" FORCE)
set(BGFX_CONFIG_MAX_DYNAMIC_INDEX_BUFFERS 8192 CACHE STRING "" FORCE)
set(BGFX_CONFIG_MAX_VERTEX_BUFFERS 8192 CACHE STRING "" FORCE)
set(BGFX_CONFIG_MAX_INDEX_BUFFERS 8192 CACHE STRING "" FORCE)
set(BGFX_CONFIG_MAX_FRAME_BUFFERS 256 CACHE STRING "" FORCE)
# Upper bounds of the transient buffers; the sizes the renderer uses are set at bgfx::init (shim context.cpp).
set(BGFX_CONFIG_MAX_TRANSIENT_VERTEX_BUFFER_SIZE 33554432 CACHE STRING "" FORCE)
set(BGFX_CONFIG_MAX_TRANSIENT_INDEX_BUFFER_SIZE 8388608 CACHE STRING "" FORCE)
# Shaders compile with a host shaderc built once per machine and pin (cmake/storm_shaderc.cmake),
# never with bgfx's tools inside this build.
set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
# The pinned bgfx/bx/bimg.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/bgfx_upstream.cmake")
# Two backends, on Android and on the Linux development host alike, so the same shader blobs and code paths are
# exercised: OpenGL ES 3.0 and Vulkan (which one a device runs: src/shim/backend_select.hpp). Selecting renderers
# explicitly disables all the others (bgfx/src/config.h). bgfx loads libvulkan itself at run time: no link
# dependency, and a system without Vulkan runs OpenGL ES.
target_compile_definitions(bgfx PRIVATE BGFX_CONFIG_RENDERER_OPENGLES=30 BGFX_CONFIG_RENDERER_VULKAN=1)
# Tracy builds (cmake/tracy.cmake): bgfx reports its own scopes through the callback's profiler functions
# (shim/program_cache.cpp).
if(STORM_TRACY)
    target_compile_definitions(bgfx PRIVATE BGFX_CONFIG_PROFILER=1)
endif()
if(ANDROID)
    target_link_libraries(bgfx PUBLIC android log EGL GLESv3)
else()
    target_link_libraries(bgfx PUBLIC EGL GLESv2)
endif()

# storm_bgfx_runtime: what every bgfx user links (never bgfx, bx or bimg directly). On Android it
# is libbgfx.so, the whole of bgfx, bx and bimg in one shared library, so the process has exactly
# one bgfx (one context, one set of handles) for both libengine.so and libstorm_ui.so, and the
# code is in the APK once. The application screens without the engine load it with
# libstorm_ui.so. Elsewhere (desktop engine, host checks) it is the static libraries.
if(ANDROID)
    add_library(storm_bgfx_runtime SHARED runtime/bgfx_runtime.cpp)
    # Next to libengine.so (game/), where the APK packaging takes the engine's libraries from.
    set_target_properties(storm_bgfx_runtime PROPERTIES OUTPUT_NAME bgfx LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/game")
    # bimg_encode (the ETC2/EAC encoders of the texture transcode and the relief maps) is not linked whole: only what
    # the encoder entry points referenced by runtime/bgfx_runtime.cpp reach comes in, not its other codecs and tools.
    target_link_libraries(storm_bgfx_runtime PRIVATE -Wl,--whole-archive bgfx bimg bx -Wl,--no-whole-archive bimg_encode)
    target_link_options(storm_bgfx_runtime PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)
    # The consumers compile against the same headers and configuration (BX_CONFIG_DEBUG ...).
    foreach(part bgfx bimg bx bimg_encode)
        target_include_directories(storm_bgfx_runtime SYSTEM INTERFACE "$<TARGET_PROPERTY:${part},INTERFACE_INCLUDE_DIRECTORIES>")
        target_compile_definitions(storm_bgfx_runtime INTERFACE "$<TARGET_PROPERTY:${part},INTERFACE_COMPILE_DEFINITIONS>")
    endforeach()
    include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/build_stamp.cmake")
    storm_build_stamp(storm_bgfx_runtime)
else()
    add_library(storm_bgfx_runtime INTERFACE)
    target_link_libraries(storm_bgfx_runtime INTERFACE bgfx bimg_encode bimg bx)
endif()

include(cmake/storm_shaderc.cmake)
set(STORM_BGFX_SHADER_PROFILE "300_es" CACHE STRING "shaderc profile for embedded shader blobs")
set(STORM_BGFX_SHADER_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/include" CACHE INTERNAL "")
include(cmake/storm_shaders.cmake)

file(GLOB shader_sources CONFIGURE_DEPENDS shaders/*.sc shaders/ports/*.sc)
list(FILTER shader_sources EXCLUDE REGEX "varying\\.def\\.sc$")
# The lit and baked families of the fixed-function emulator (src/effects/lighting.hpp FfFamily: the same table, in the
# same order; src/effects/lighting.cpp ChooseFamily picks one per draw). A family compiles the product blocks of
# ff_fragment.sh it names and nothing else: the lamps' code only in the programs of the draws the lamps light.
#   lit          lit draws without lamps or the AO volume (rain: FX_FF_WET)
#   lit_lamps    lit draws lit by the place's lamps or in the AO volume (with the room light)
#   baked        baked colours without lamps or the AO volume (rain, a town's walls relit by our sun: FX_FF_TOWN_WALL)
#   baked_lamps  baked colours relit by the lamps or in the AO volume
# Every family has as many programs per preset as the one uber-shader had: each extra family adds 12 programs to the
# «Высокая» switch (6 relief/specular variants and their opaque twins), so the small blocks (AO, town wall) ride
# with a family instead of having their own.
# Each family in every combination of s = FX_LIGHT_SPECULAR, n = FX_LIGHT_NORMALMAP, np = and FX_LIGHT_PARALLAX,
# h = FX_LIGHT_SHADOW (the presets with sun shadows: all their lit / baked draws). Linear light and the soft sun filter
# are run-time switches of these programs (fx_lighting.sh), not variants. fs_ff_lit.sc / fs_ff_baked.sc are compiled
# only here.
list(FILTER shader_sources EXCLUDE REGEX "/fs_ff_(lit|baked)\\.sc$")
set(ff_families
    "lit|fs_ff_lit.sc|FX_FF_WET"
    "lit_lamps|fs_ff_lit.sc|FX_FF_LAMPS,FX_FF_AO,FX_FF_WET"
    "baked|fs_ff_baked.sc|FX_FF_TOWN_WALL,FX_FF_WET"
    "baked_lamps|fs_ff_baked.sc|FX_FF_LAMPS,FX_FF_AO,FX_FF_TOWN_WALL,FX_FF_WET")
set(ff_fx_variants)
foreach(family ${ff_families})
    string(REPLACE "|" ";" fields "${family}")
    list(GET fields 0 family_name)
    list(GET fields 1 family_source)
    list(GET fields 2 family_defines)
    string(REPLACE "," ";" family_defines "${family_defines}")
    foreach(specular "" s)
        foreach(relief "" n np)
            foreach(shadow "" h)
                set(defines ${family_defines})
                # (String comparisons: if("n") is false in CMake.)
                if(specular STREQUAL "s")
                    list(APPEND defines FX_LIGHT_SPECULAR)
                endif()
                if(NOT relief STREQUAL "")
                    list(APPEND defines FX_LIGHT_NORMALMAP)
                endif()
                if(relief STREQUAL "np")
                    list(APPEND defines FX_LIGHT_PARALLAX)
                endif()
                if(shadow STREQUAL "h")
                    list(APPEND defines FX_LIGHT_SHADOW)
                endif()
                set(suffix "${specular}${relief}${shadow}")
                if(NOT suffix STREQUAL "")
                    set(suffix "_${suffix}")
                endif()
                list(JOIN defines "," defines)
                list(APPEND ff_fx_variants "fs_ff_${family_name}${suffix}|${CMAKE_CURRENT_SOURCE_DIR}/shaders/${family_source}|${defines}")
            endforeach()
        endforeach()
    endforeach()
endforeach()
# Opaque twins (FF_OPAQUE, ff_finish.sh without its discards) of every fixed-function fragment shader the shim
# chooses (and of the ports below): draw.cpp uses "<name>_opaque" when the draw has no clip plane and no alpha test, so early depth stays on.
set(ff_opaque_variants)
foreach(base fs_ff fs_ff_dyn fs_ff_cube0 fs_ff_cube1 fs_ff_cube2)
    list(APPEND ff_opaque_variants "${base}_opaque|${CMAKE_CURRENT_SOURCE_DIR}/shaders/${base}.sc|FF_OPAQUE")
endforeach()
foreach(variant ${ff_fx_variants})
    string(REPLACE "|" ";" fields "${variant}")
    list(GET fields 0 name)
    list(GET fields 1 source)
    list(GET fields 2 defines)
    list(APPEND ff_opaque_variants "${name}_opaque|${source}|${defines},FF_OPAQUE")
endforeach()
# The ps_1_x ports' twins too (shaders/ports, draw.cpp picks them the same way), but not the slope-biased caustic port:
# its depth write turns early depth off anyway.
foreach(base fs_port_coastfoam fs_port_particles fs_port_postprocess fs_port_sea2 fs_port_sea3)
    list(APPEND ff_opaque_variants "${base}_opaque|${CMAKE_CURRENT_SOURCE_DIR}/shaders/ports/${base}.sc|FF_OPAQUE")
endforeach()
list(APPEND ff_fx_variants ${ff_opaque_variants})
# Grass alone packs POSITION as non-normalised SHORT4, which bgfx binds as integer input (GL glVertexAttribIPointer,
# Vulkan SINT): its vertex shader reads varying.def.sc with an ivec4 a_position, the varyings stay those of every
# other shader.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc" grass_varying)
string(REGEX REPLACE "\nvec4 a_position +: POSITION;" "\nivec4 a_position : POSITION;" grass_varying_int "${grass_varying}")
if(grass_varying_int STREQUAL grass_varying)
    message(FATAL_ERROR "shaders/varying.def.sc has no 'vec4 a_position : POSITION;' line for the grass varying")
endif()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/grass.varying.def.sc" CONTENT "${grass_varying_int}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/shaders/ports/vs_port_grass.sc" PROPERTIES
    STORM_SHADER_VARYING "${CMAKE_CURRENT_BINARY_DIR}/grass.varying.def.sc")
storm_bgfx_embed_shaders(TARGET storm_bgfx_shaders NAMESPACE storm_shaders
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc" SOURCES ${shader_sources}
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/fx/shaders/include" VARIANTS ${ff_fx_variants})

file(GLOB_RECURSE shim_sources CONFIGURE_DEPENDS src/*.cpp src/*.hpp)
list(FILTER shim_sources EXCLUDE REGEX "/src/texture/core/")
add_library(storm_bgfx STATIC ${shim_sources})
target_compile_features(storm_bgfx PUBLIC cxx_std_20)
target_include_directories(storm_bgfx
    PUBLIC include "${CMAKE_CURRENT_LIST_DIR}/../util/include"
    PRIVATE src)
target_link_libraries(storm_bgfx PUBLIC storm_bgfx_runtime PRIVATE storm_bgfx_shaders ${SDL2_LIBRARIES})
if(ANDROID)
    target_link_libraries(storm_bgfx PRIVATE GLESv3) # driver string and limit queries next to bgfx
    # Swappy (GL or Vk, by the backend) presents the game's frames (src/shim/frame_pacing.cpp). Only that file sees its headers and the
    # host hooks it reports to (header-only platform/host_hooks.hpp): the other shim sources keep their compile
    # commands (and their ccache entries).
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/swappy.cmake")
    set_source_files_properties(src/shim/frame_pacing.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${storm_swappy_include};${CMAKE_CURRENT_LIST_DIR}/../util/include"
        COMPILE_DEFINITIONS "STORM_SWAPPY_PACKAGE=\"games-frame-pacing ${storm_swappy_version}\"")
    target_link_libraries(storm_bgfx PRIVATE $<LINK_ONLY:storm_swappy>)
    # Swappy registers its JNI methods itself: none of its symbols are exported from the library linking it.
    target_link_options(storm_bgfx INTERFACE "LINKER:--exclude-libs,libswappy_static.a")
endif()
# The Vulkan probe (src/shim/backend_select.cpp) reads the loader through the Vulkan headers bgfx itself compiles
# against; no other source sees them.
set_source_files_properties(src/shim/backend_select.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${STORM_BGFX_SOURCE_DIR}/3rdparty/khronos")
target_link_libraries(storm_bgfx PRIVATE ${CMAKE_DL_LIBS})
set_target_properties(storm_bgfx PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(storm_bgfx PRIVATE -fsigned-char)
target_compile_definitions(storm_bgfx PRIVATE NOMINMAX)
# context.cpp records Tracy zones through src/libs/util/include/profiler.hpp (no other util header).
set_property(SOURCE src/shim/context.cpp APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../util/include")

# DXT -> ETC2 transcode and on-disk cache (bimg only, no bgfx; unit-tested on the host).
file(GLOB texture_core_sources CONFIGURE_DEPENDS src/texture/core/*.cpp src/texture/core/*.hpp)
add_library(storm_texture_core STATIC ${texture_core_sources})
target_compile_features(storm_texture_core PUBLIC cxx_std_20)
target_include_directories(storm_texture_core PUBLIC src/texture/core)
target_link_libraries(storm_texture_core PUBLIC storm_bgfx_runtime)
find_package(Threads REQUIRED)
target_link_libraries(storm_texture_core PUBLIC Threads::Threads)
set_target_properties(storm_texture_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(storm_bgfx PRIVATE storm_texture_core)
# The relief generation (src/effects/relief_generator.hpp): PBRify's models through ncnn (src/libs/relief).
target_link_libraries(storm_bgfx PRIVATE storm_relief_core)

# Graphics effects package (fx/README.md): presets «Низкая / Средняя / Высокая», driven by src/effects.
set(STORM_FX_SHADERC_COMMAND "${STORM_SHADERC_COMMAND}")
set(STORM_FX_SHADERC_DEPENDS "${STORM_SHADERC_DEPENDS}")
set(STORM_FX_BGFX_SHADER_INCLUDE "${STORM_BGFX_SOURCE_DIR}/src")
add_subdirectory(fx)
target_link_libraries(storm_bgfx PUBLIC storm_bgfx_fx)

# Texture transcode tests and benchmark: only on request, so projects that merely consume
# storm_bgfx (e.g. the standalone UI build) do not build them.
option(STORM_BGFX_BUILD_TESTS "Build the renderer_bgfx texture tests and benchmark" OFF)
if(STORM_BGFX_BUILD_TESTS)
    add_executable(storm_texture_bench tests/texture/bench.cpp)
    target_link_libraries(storm_texture_bench PRIVATE storm_texture_core)
    if(NOT CMAKE_CROSSCOMPILING)
        add_executable(storm_texture_tests tests/texture/tests.cpp)
        target_link_libraries(storm_texture_tests PRIVATE storm_texture_core bimg_decode storm_bgfx_runtime)
        enable_testing()
        add_test(NAME storm_texture_tests COMMAND storm_texture_tests)
        add_executable(storm_eac_encoder_tests tests/texture/eac_encoder_tests.cpp)
        target_link_libraries(storm_eac_encoder_tests PRIVATE storm_texture_core)
        add_test(NAME storm_eac_encoder_tests COMMAND storm_eac_encoder_tests)
    endif()
endif()
