# Host check of the bgfx renderer: builds storm_bgfx (shim, shaders compiled by the pinned shaderc)
# runs the texture transcode tests and the draw/state counter check (perf_check.cpp).
# Configured by run.py next to this file.
cmake_minimum_required(VERSION 3.21)
project(storm_renderer_bgfx_check LANGUAGES C CXX)
enable_testing()
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/StormBuildCache.cmake")
set(CMAKE_CXX_STANDARD 20)
set(STORM_SDL_INCLUDE "" CACHE PATH "directory that contains SDL2/SDL.h")
add_library(storm_check_sdl INTERFACE)
target_include_directories(storm_check_sdl INTERFACE "${STORM_SDL_INCLUDE}")
set(SDL2_LIBRARIES storm_check_sdl)
set(STORM_BGFX_BUILD_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx" renderer_bgfx)
# The relief preparation (ncnn + PBRify): storm_relief_check, storm_relief_finalizer_check.
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../src/libs/relief" relief)
# The pinned SDL source headers carry no platform configuration; the checks run on the host's X11
# SDL runtime, so the shim takes its native window from SDL's X11 window info (skinning_check).
target_compile_definitions(storm_bgfx PRIVATE SDL_VIDEO_DRIVER_X11=1)

# perf_check drives the shim on bgfx's Noop backend; the shim calls a few SDL window functions,
# so it links the host's SDL2 runtime like the other SDL host checks.
# storm_check_sdl carries both: the pinned headers (the host has no SDL development package in CI)
# and the runtime, so every check that includes <SDL2/SDL.h> links it instead of the bare runtime.
find_library(STORM_SDL_RUNTIME NAMES SDL2-2.0 SDL2 libSDL2-2.0.so.0 REQUIRED)
target_link_libraries(storm_check_sdl INTERFACE "${STORM_SDL_RUNTIME}")
add_executable(perf_check perf_check.cpp)
target_include_directories(perf_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_compile_definitions(perf_check PRIVATE NOMINMAX)
target_compile_options(perf_check PRIVATE -fsigned-char)
target_link_libraries(perf_check PRIVATE storm_bgfx storm_check_sdl)

# program_family_check: every lit / baked program Lighting can pick is embedded with its opaque twin, and the family
# rule gives each draw the narrowest family that holds what it reads (no GPU).
add_executable(program_family_check program_family_check.cpp)
target_include_directories(program_family_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(program_family_check PRIVATE NOMINMAX)
target_compile_options(program_family_check PRIVATE -fsigned-char)
target_link_libraries(program_family_check PRIVATE storm_bgfx storm_check_sdl)

add_executable(grass_vertex_check grass_vertex_check.cpp)
target_include_directories(grass_vertex_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_link_libraries(grass_vertex_check PRIVATE storm_bgfx storm_check_sdl)
add_executable(coast_foam_vertex_check coast_foam_vertex_check.cpp)
target_include_directories(coast_foam_vertex_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_link_libraries(coast_foam_vertex_check PRIVATE storm_bgfx storm_check_sdl)
add_executable(sea_surface_vertex_check sea_surface_vertex_check.cpp)
target_include_directories(sea_surface_vertex_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_link_libraries(sea_surface_vertex_check PRIVATE storm_bgfx storm_check_sdl)

# view_schedule_check: the execution order of the game's views (src/shim/view_schedule.hpp). No GPU.
add_executable(view_schedule_check view_schedule_check.cpp)
target_include_directories(view_schedule_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_link_libraries(view_schedule_check PRIVATE storm_bgfx storm_check_sdl)

# backend_select_check: the choice between Vulkan and OpenGL ES, the denylist and the crash guard of a Vulkan start
# (src/shim/backend_select.hpp). No GPU.
add_executable(backend_select_check backend_select_check.cpp)
target_include_directories(backend_select_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_link_libraries(backend_select_check PRIVATE storm_bgfx storm_check_sdl)

# skinning_check renders through OpenGL ES in an SDL window (an X display: xvfb-run in CI) and
# compares GPU skinning with the CPU kernel of src/libs/model.
add_executable(skinning_check skinning_check.cpp)
target_include_directories(skinning_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/model/src" "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/model/include")
target_compile_definitions(skinning_check PRIVATE NOMINMAX)
target_compile_options(skinning_check PRIVATE -fsigned-char)
target_link_libraries(skinning_check PRIVATE storm_bgfx storm_check_sdl)

# uniform_commit_check: the GL backend uploads only the uniforms a program does not hold yet
# (patches/bgfx-gl-uniform-commit.patch), on OpenGL ES (an X display, like skinning_check).
add_executable(uniform_commit_check uniform_commit_check.cpp)
target_include_directories(uniform_commit_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_compile_definitions(uniform_commit_check PRIVATE NOMINMAX)
target_compile_options(uniform_commit_check PRIVATE -fsigned-char)
target_link_libraries(uniform_commit_check PRIVATE storm_bgfx storm_check_sdl)

# Structural Vulkan regression runs offline: the production compiler must export AND store BuiltIn PointSize.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(point_size_spirv_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/point_size_spirv_check.py"
        --shaderc "${STORM_SHADERC_COMMAND}" --bgfx-include "${STORM_BGFX_SOURCE_DIR}/src"
    VERBATIM)

# point_sprite_check: a point sprite's texture lies across the point (the night sky's stars), through the shim on
# OpenGL ES (an X display, like skinning_check).
add_executable(point_sprite_check point_sprite_check.cpp)
target_include_directories(point_sprite_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_compile_definitions(point_sprite_check PRIVATE NOMINMAX)
target_compile_options(point_sprite_check PRIVATE -fsigned-char)
target_link_libraries(point_sprite_check PRIVATE storm_bgfx storm_check_sdl)

# preset_switch_check: a live graphics preset switch through the shim on OpenGL ES (an X display, like
# skinning_check): the previous settings draw until the new ones are prepared, no black frame after it.
add_executable(preset_switch_check preset_switch_check.cpp)
target_include_directories(preset_switch_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(preset_switch_check PRIVATE NOMINMAX)
target_compile_options(preset_switch_check PRIVATE -fsigned-char)
target_link_libraries(preset_switch_check PRIVATE storm_bgfx storm_check_sdl)

# render_thread_check: bgfx's render thread through the shim on OpenGL ES (an X display, like skinning_check): the
# driver's strings arrive from it, the surface is released only with no frame in flight, both gate choices and
# render_thread=0 present, the renderer restarts in either mode; on Vulkan also the crash guard and self-test fallback.
add_executable(render_thread_check render_thread_check.cpp)
target_include_directories(render_thread_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(render_thread_check PRIVATE NOMINMAX)
target_compile_options(render_thread_check PRIVATE -fsigned-char)
target_link_libraries(render_thread_check PRIVATE storm_bgfx storm_check_sdl)

# surface_lifecycle_check: Android's surface events (surfaceDestroyed / Created / Changed, pause, resume, rotation) in
# every order against the game thread's frames, resets and read-backs, through the shim's Context (an X display).
add_executable(surface_lifecycle_check surface_lifecycle_check.cpp)
target_include_directories(surface_lifecycle_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(surface_lifecycle_check PRIVATE NOMINMAX)
target_compile_options(surface_lifecycle_check PRIVATE -fsigned-char)
target_link_libraries(surface_lifecycle_check PRIVATE storm_bgfx storm_check_sdl)

# program_cache_check: program binaries between renderer starts (src/shim/program_cache.hpp) on OpenGL ES (an X
# display, like skinning_check): stored, loaded, corrupt / rejected entries relinked, the size limit.
add_executable(program_cache_check program_cache_check.cpp)
target_include_directories(program_cache_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(program_cache_check PRIVATE NOMINMAX)
target_compile_options(program_cache_check PRIVATE -fsigned-char)
target_link_libraries(program_cache_check PRIVATE storm_bgfx storm_check_sdl)

# ao_volume_check: the interior AO volume end to end on OpenGL ES (an X display, like skinning_check): baked floor
# under a table top, «Низкая» untouched, the baked variant without the volume unchanged, levels 1..3 darker under it.
add_executable(ao_volume_check ao_volume_check.cpp)
target_include_directories(ao_volume_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(ao_volume_check PRIVATE NOMINMAX)
target_compile_options(ao_volume_check PRIVATE -fsigned-char)
target_link_libraries(ao_volume_check PRIVATE storm_bgfx storm_check_sdl)

# cutout_caster_check: a material whose texture alpha cuts holes casts with them in the lamp atlases (still geometry,
# characters) and the sun cascade; blended materials without depth write cast nothing (src/effects/casters.hpp).
add_executable(cutout_caster_check cutout_caster_check.cpp)
target_include_directories(cutout_caster_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_compile_definitions(cutout_caster_check PRIVATE NOMINMAX)
target_compile_options(cutout_caster_check PRIVATE -fsigned-char)
target_link_libraries(cutout_caster_check PRIVATE storm_bgfx storm_check_sdl)

# sun_layers_check: the sun's two shadow layers through the shim (an X display, like cutout_caster_check): the static
# layer is kept inside its band and step, the dynamic layer shades lit and baked receivers.
add_executable(sun_layers_check sun_layers_check.cpp)
target_include_directories(sun_layers_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(sun_layers_check PRIVATE NOMINMAX)
target_compile_options(sun_layers_check PRIVATE -fsigned-char)
target_link_libraries(sun_layers_check PRIVATE storm_bgfx storm_check_sdl)

# sun_relight_check: a town's lightmap relit from the place's sun map on the GPU, through the shim (an X display).
add_executable(sun_relight_check sun_relight_check.cpp)
target_include_directories(sun_relight_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(sun_relight_check PRIVATE NOMINMAX)
target_compile_options(sun_relight_check PRIVATE -fsigned-char)
target_link_libraries(sun_relight_check PRIVATE storm_bgfx storm_check_sdl)

# shadow_casters_check: the lights collect their casters (src/effects/shadow_casters.hpp): the rules of the shadow
# volumes without a GPU, then through the shim: a caster off screen shadows a visible receiver, a caster is submitted
# into a volume once and only when it reaches it, skinned casters are included, still geometry stays out of the
# dynamic layer.
add_executable(shadow_casters_check shadow_casters_check.cpp)
target_include_directories(shadow_casters_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/model/src" "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/model/include")
target_compile_definitions(shadow_casters_check PRIVATE NOMINMAX)
target_compile_options(shadow_casters_check PRIVATE -fsigned-char)
target_link_libraries(shadow_casters_check PRIVATE storm_bgfx storm_check_sdl)

# Cached lamp geometry owns texture storage independently of the game's texture wrappers. No window/GPU.
# lamp_shadow_check: the lamps' shadow faces as the frame uses them: a caster's faces reach the draws before it (frame
# uniform), the characters' atlas only in frames with casters, a lamp's reach, a moving light's whole shape.
add_executable(lamp_shadow_check lamp_shadow_check.cpp)
target_include_directories(lamp_shadow_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
                                                    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(lamp_shadow_check PRIVATE NOMINMAX)
target_compile_options(lamp_shadow_check PRIVATE -fsigned-char)
target_link_libraries(lamp_shadow_check PRIVATE storm_bgfx storm_check_sdl)

add_executable(lamp_lifetime_check lamp_lifetime_check.cpp)
target_include_directories(lamp_lifetime_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_link_libraries(lamp_lifetime_check PRIVATE storm_bgfx storm_check_sdl)

# sea_edge_check: the sea's soft edges (fs_port_sea2 + fx::SeaDepth, «Высокая») next to an object in front of the sea,
# through the shim on OpenGL ES (an X display, like skinning_check): no water is faded where nothing lies under it.
add_executable(sea_edge_check sea_edge_check.cpp)
target_include_directories(sea_edge_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_compile_definitions(sea_edge_check PRIVATE NOMINMAX)
target_compile_options(sea_edge_check PRIVATE -fsigned-char)
target_link_libraries(sea_edge_check PRIVATE storm_bgfx storm_check_sdl)

# relief_switch_check: textures loaded while relief is off are looked up when it is switched on (a preset raised in a
# location): requested from the generator, height maps derived. No GPU (shim stores filled like TextureLoad).
add_executable(relief_switch_check relief_switch_check.cpp)
target_include_directories(relief_switch_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(relief_switch_check PRIVATE NOMINMAX)
target_compile_options(relief_switch_check PRIVATE -fsigned-char)
target_link_libraries(relief_switch_check PRIVATE storm_bgfx storm_check_sdl)

add_executable(texture_format_check texture_format_check.cpp)
target_include_directories(texture_format_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(texture_format_check PRIVATE NOMINMAX)
target_compile_options(texture_format_check PRIVATE -fsigned-char)
target_link_libraries(texture_format_check PRIVATE storm_bgfx storm_check_sdl)

# The texture swap fade's state (src/shim/texture_fade.hpp): CPU only.
add_executable(texture_fade_check texture_fade_check.cpp)
target_include_directories(texture_fade_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_compile_options(texture_fade_check PRIVATE -UNDEBUG)

# The in-play texture realize budget (src/shim/realize_budget.hpp): the budget, and new textures in view through the
# shim on bgfx's Noop backend.
add_executable(realize_budget_check realize_budget_check.cpp)
target_include_directories(realize_budget_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_compile_definitions(realize_budget_check PRIVATE NOMINMAX)
target_compile_options(realize_budget_check PRIVATE -fsigned-char -UNDEBUG)
target_link_libraries(realize_budget_check PRIVATE storm_bgfx storm_check_sdl)

# Upscale swaps through the real draw binding: offscreen pixels on GLES/Vulkan, --noop for submission/state only.
# Location reveal state, actual producer state chains and worker/cache retry. Noop only, no display/GPU.
foreach(pair IN ITEMS "scene_readiness_check;check" "scene_resources_check;resources" "scene_retry_check;retry"
                  "scene_texture_failure_check;texture_failure")
    list(GET pair 0 target)
    list(GET pair 1 source)
    add_executable(${target} "${CMAKE_CURRENT_LIST_DIR}/../scene_readiness_check/${source}.cpp")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
    target_compile_options(${target} PRIVATE -UNDEBUG)
    target_link_libraries(${target} PRIVATE storm_bgfx storm_check_sdl)
endforeach()

# The scene_* ctest TIMEOUTs below are deadlock guards, not time budgets: each test compiles its sanitized fixtures
# first, which takes long on a loaded land queue.
# Actual post-target handoff followed by Present qualification. CPU-only extracted
# production latch/bridge control; no GPU/window and no historical git dependency.
add_test(NAME scene_post_handoff_check
    COMMAND "${CMAKE_COMMAND}" -E env
        "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}" "CXX=${CMAKE_CXX_COMPILER}"
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../scene_readiness_check/post_handoff.py")
set_tests_properties(scene_post_handoff_check PROPERTIES
    LABELS "scene_readiness;cpu" TIMEOUT 300)

# A lazy texture request during hidden rendering must not recursively Present or
# replace the reflection target; the outer loading RunEnd remains authoritative.
add_test(NAME scene_nested_progress_check
    COMMAND "${CMAKE_COMMAND}" -E env
        "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}" "CXX=${CMAKE_CXX_COMPILER}"
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../scene_readiness_check/nested_progress.py")
set_tests_properties(scene_nested_progress_check PROPERTIES
    LABELS "scene_readiness;cpu" TIMEOUT 300)

# Native stock ownership/cadence, using actual core and host-controller bodies.
# No renderer window, model inference or GPU completion assumption.
foreach(check IN ITEMS transaction scheduler progress progress_rate consumers)
    add_test(NAME scene_stock_${check}_check
        COMMAND "${CMAKE_COMMAND}" -E env
            "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}" "CXX=${CMAKE_CXX_COMPILER}"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../scene_readiness_check/stock_${check}.py")
    set_tests_properties(scene_stock_${check}_check PROPERTIES
        LABELS "scene_readiness;cpu" TIMEOUT 300)
endforeach()

# Hidden loading frames: no beauty, probe or opaque acknowledgement, while resource preparation goes on.
add_test(NAME scene_hidden_work_check
    COMMAND "${CMAKE_COMMAND}" -E env
        "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}" "CXX=${CMAKE_CXX_COMPILER}"
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../scene_readiness_check/hidden_scene_work.py")
set_tests_properties(scene_hidden_work_check PROPERTIES
    LABELS "scene_readiness;cpu" TIMEOUT 300)

add_executable(upscale_swap_check upscale_swap_check.cpp)
target_include_directories(upscale_swap_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(upscale_swap_check PRIVATE NOMINMAX)
target_compile_options(upscale_swap_check PRIVATE -fsigned-char)
target_link_libraries(upscale_swap_check PRIVATE storm_bgfx storm_check_sdl)

# buffer_pool_check: write-once buffers in bgfx's shared dynamic buffers (src/shim/draw.cpp) on OpenGL ES (an X display, like
# skinning_check): every range draws exactly its buffer's data, after releases, rewrites and another stride.
add_executable(buffer_pool_check buffer_pool_check.cpp)
target_include_directories(buffer_pool_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_compile_definitions(buffer_pool_check PRIVATE NOMINMAX)
target_compile_options(buffer_pool_check PRIVATE -fsigned-char)
target_link_libraries(buffer_pool_check PRIVATE storm_bgfx storm_check_sdl)

# thumbnail_check: the save thumbnail of a background autosave (no surface): the back buffer is read from the copy of
# the last presented frame (src/shim/device.cpp copy_frame), on OpenGL ES (an X display, like skinning_check) and Vulkan.
add_executable(thumbnail_check thumbnail_check.cpp)
target_include_directories(thumbnail_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_compile_definitions(thumbnail_check PRIVATE NOMINMAX)
target_compile_options(thumbnail_check PRIVATE -fsigned-char)
target_link_libraries(thumbnail_check PRIVATE storm_bgfx storm_check_sdl)

# pipeline_prewarm_check: the scenes' Vulkan pipelines (src/shim/pipeline_sets.hpp): the interface never waits for a
# pipeline, the scene's recorded pipelines are prewarmed when it begins (an X display; OpenGL ES and Vulkan).
add_executable(pipeline_prewarm_check pipeline_prewarm_check.cpp)
target_include_directories(pipeline_prewarm_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
                           "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(pipeline_prewarm_check PRIVATE NOMINMAX)
target_compile_options(pipeline_prewarm_check PRIVATE -fsigned-char)
target_link_libraries(pipeline_prewarm_check PRIVATE storm_bgfx storm_check_sdl)
# pipeline_stress_check: the asynchronous pipelines under load: new pipelines every frame with read-backs, scene
# changes, prewarmed programs destroyed while their pipelines are pending, a shutdown with pipelines pending.
add_executable(pipeline_stress_check pipeline_stress_check.cpp)
target_include_directories(pipeline_stress_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src")
target_compile_definitions(pipeline_stress_check PRIVATE NOMINMAX)
target_compile_options(pipeline_stress_check PRIVATE -fsigned-char)
target_link_libraries(pipeline_stress_check PRIVATE storm_bgfx storm_check_sdl)

# wet_surfaces_check: the rain's wetness on a baked floor (src/effects/lighting.cpp, ff_wet.sh): «Низкая» and interiors
# dry, «Высокая» in the rain darker, the same on an absolute and a camera-relative device, dry under a roof the
# visibility volume sees; a lamp's highlight on the wet street, gone where a wall shadows the lamp.
add_executable(wet_surfaces_check wet_surfaces_check.cpp)
target_include_directories(wet_surfaces_check PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/src/shim")
target_compile_definitions(wet_surfaces_check PRIVATE NOMINMAX)
target_compile_options(wet_surfaces_check PRIVATE -fsigned-char)
target_link_libraries(wet_surfaces_check PRIVATE storm_bgfx storm_check_sdl)

# STORM_CHECK_ASAN (run.py's second tree, build/asan): the texture-lifetime checks with the renderer's own code under
# AddressSanitizer, so a read of a texture the game has freed fails them. bgfx, ncnn and the other third-party libraries
# stay uninstrumented: their compiles are cache hits of the first tree.
option(STORM_CHECK_ASAN "the renderer's code and the lifetime checks with AddressSanitizer" OFF)
set(STORM_ASAN_CHECKS cutout_caster_check lamp_lifetime_check sun_relight_check)
if(STORM_CHECK_ASAN)
    foreach(target storm_bgfx storm_bgfx_fx storm_bgfx_fx_core storm_texture_core ${STORM_ASAN_CHECKS})
        target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    endforeach()
    foreach(target ${STORM_ASAN_CHECKS})
        target_link_options(${target} PRIVATE -fsanitize=address)
    endforeach()
endif()
