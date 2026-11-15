# Host check of the lamp face projections: the exact production face/submit fragments (fuse.py) with the
# pinned bx math, no renderer or device link.
# Configured by run.py next to this file. bx is used header-only: only its pin is fetched (bx_pin.cmake).
cmake_minimum_required(VERSION 3.21)
project(storm_lamp_face_matrices_check LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/StormBuildCache.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/cmake/bx_pin.cmake")
FetchContent_MakeAvailable(storm_bx)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# fuse.py's depfile names the production sources it extracts from: Ninja re-fuses when one changes.
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/check.cpp"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/fuse.py"
        "${CMAKE_CURRENT_BINARY_DIR}/check.cpp" "${CMAKE_CURRENT_BINARY_DIR}/check.cpp.d"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/fuse.py"
    DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/check.cpp.d"
    VERBATIM)
add_executable(lamp_face_matrices_check "${CMAKE_CURRENT_BINARY_DIR}/check.cpp")
target_compile_features(lamp_face_matrices_check PRIVATE cxx_std_20)
target_compile_options(lamp_face_matrices_check PRIVATE -O1 -Wall -Wextra -Werror)
target_compile_definitions(lamp_face_matrices_check PRIVATE BX_CONFIG_DEBUG=0)
target_include_directories(lamp_face_matrices_check PRIVATE "${storm_bx_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libs/renderer_bgfx/fx/include")
