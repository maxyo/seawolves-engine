# Real simplifier plus GEOM/GeomFarLod integration checks, configured by run.py.
cmake_minimum_required(VERSION 3.21)
project(storm_far_lod_check LANGUAGES C CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/StormBuildCache.cmake")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_subdirectory("${root}/src/libs/geometry/far_lod" far_lod)
add_executable(far_lod_check check.cpp)
target_compile_options(far_lod_check PRIVATE -Wall -Wextra -Werror)
target_link_libraries(far_lod_check PRIVATE storm_far_lod)

include("${root}/cmake/glm.cmake")
find_path(STORM_TEST_FMT_INCLUDE fmt/format.h HINTS "${STORM_TEST_DEPENDENCY_SOURCES}/fmt/include" REQUIRED)
find_path(STORM_TEST_SPDLOG_INCLUDE spdlog/spdlog.h HINTS "${STORM_TEST_DEPENDENCY_SOURCES}/spdlog/include" REQUIRED)
add_executable(far_lod_runtime_check runtime_check.cpp
    "${root}/src/libs/geometry/src/geom_far_lod.cpp"
    "${root}/src/libs/geometry/src/geom_static.cpp"
    "${root}/src/libs/geometry/src/collide.cpp")
target_include_directories(far_lod_runtime_check PRIVATE
    "${root}/src/libs/geometry/src" "${root}/src/libs/geometry/include"
    "${root}/src/libs/math/include" "${root}/src/libs/core/include"
    "${root}/src/libs/renderer_bgfx/include" "${root}/src/libs/renderer_bgfx/fx/include")
target_include_directories(far_lod_runtime_check SYSTEM PRIVATE "${STORM_TEST_FMT_INCLUDE}" "${STORM_TEST_SPDLOG_INCLUDE}")
target_compile_definitions(far_lod_runtime_check PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL)
# Legacy RDF_VERSION is a four-byte constant; production sources retain their normal warnings.
target_compile_options(far_lod_runtime_check PRIVATE -Wall -Wextra -Wno-multichar)
target_link_libraries(far_lod_runtime_check PRIVATE storm_far_lod glm::glm-header-only)
if(STORM_TEST_SANITIZE)
    foreach(target far_lod_check far_lod_runtime_check storm_far_lod storm_meshoptimizer)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()
