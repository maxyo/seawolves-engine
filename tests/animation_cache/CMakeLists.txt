cmake_minimum_required(VERSION 3.20)
project(animation_cache_check LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/StormBuildCache.cmake")

enable_testing()
set(STORM_GLM_SOURCE "" CACHE PATH "Extracted pinned GLM 1.0.3 source (no download)")
if(NOT EXISTS "${STORM_GLM_SOURCE}/glm/glm.hpp")
    message(FATAL_ERROR "Set STORM_GLM_SOURCE to the pinned GLM 1.0.3 checkout")
endif()

get_filename_component(STORM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ANIMATION_SOURCE "${STORM_ROOT}/src/libs/animation/src")
add_executable(animation_cache_check
    check.cpp
    "${ANIMATION_SOURCE}/animation_service_imp.cpp"
    "${ANIMATION_SOURCE}/animation_info.cpp"
    "${ANIMATION_SOURCE}/animation_imp.cpp"
    "${ANIMATION_SOURCE}/action_info.cpp"
    "${ANIMATION_SOURCE}/action_player_imp.cpp"
    "${ANIMATION_SOURCE}/animation_timer_imp.cpp"
    "${ANIMATION_SOURCE}/bone.cpp")
target_compile_features(animation_cache_check PRIVATE cxx_std_20)
target_compile_definitions(animation_cache_check PRIVATE EX_OFF _DEBUG)
target_include_directories(animation_cache_check PRIVATE
    stubs "${ANIMATION_SOURCE}"
    "${STORM_ROOT}/src/libs/animation/include"
    "${STORM_ROOT}/src/libs/core/include"
    "${STORM_ROOT}/src/libs/util/include"
    "${STORM_ROOT}/src/libs/math/include")
target_include_directories(animation_cache_check SYSTEM PRIVATE "${STORM_GLM_SOURCE}")
# Engine normally supplies these through its precompiled header.
target_compile_options(animation_cache_check PRIVATE "SHELL:-include cstring" "SHELL:-include memory")
option(ANIMATION_CACHE_SANITIZERS "Enable ASan and UBSan" ON)
if(ANIMATION_CACHE_SANITIZERS)
    target_compile_options(animation_cache_check PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(animation_cache_check PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME animation_cache COMMAND animation_cache_check "${CMAKE_CURRENT_BINARY_DIR}/fixture.an")
