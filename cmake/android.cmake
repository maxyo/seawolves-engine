# Full engine dependencies; all sources/toolchains are supplied explicitly.
foreach(path STORM_ANDROID_DEPS STORM_ANDROID_SOURCES STORM_AUDIO_SOURCES)
    if(NOT IS_DIRECTORY "${${path}}")
        message(FATAL_ERROR "Set ${path} to a prepared dependency directory")
    endif()
endforeach()
if(STORM_ENABLE_CRASH_REPORTS)
    message(FATAL_ERROR "Android currently requires crash reports OFF")
endif()
include(engine_sources)
storm_check_engine_sources("${STORM_ANDROID_SOURCES}")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
add_compile_options(-fsigned-char)
add_library(sdl SHARED IMPORTED GLOBAL)
# Only SDL's headers from the prefix: a prefix prepared before fmt moved to the engine's own build
# still holds fmt 8 headers in <prefix>/include/fmt, which must never shadow the pinned fmt.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/sdl-include")
file(CREATE_LINK "${STORM_ANDROID_DEPS}/include/SDL2" "${CMAKE_BINARY_DIR}/sdl-include/SDL2" SYMBOLIC)
set_target_properties(sdl PROPERTIES IMPORTED_LOCATION "${STORM_ANDROID_DEPS}/lib/libSDL2.so"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/sdl-include;${STORM_ANDROID_DEPS}/include/SDL2")
set(SDL2_LIBRARIES sdl)
add_library(fast_float INTERFACE)
target_include_directories(fast_float INTERFACE "${STORM_ANDROID_SOURCES}/fast_float/include")
add_library(spdlog INTERFACE)
target_include_directories(spdlog INTERFACE "${STORM_ANDROID_SOURCES}/spdlog/include")
target_compile_definitions(spdlog INTERFACE SPDLOG_FMT_EXTERNAL)
target_link_libraries(spdlog INTERFACE fmt)
add_library(catch2 INTERFACE)
target_include_directories(catch2 INTERFACE "${STORM_ANDROID_SOURCES}/catch2/single_include")
# Catch2 writes its output to the Android log (__android_log_write) on Android.
target_link_libraries(catch2 INTERFACE log)
# No mimalloc on Android: API 24's emulated TLS makes its first allocation call libc malloc, so
# interposing malloc re-enters the uninitialized heap, and shared libc++ frees storage internally,
# so an engine-only new override corrupts allocator ownership. C and C++ allocation stay on Bionic.
# Release = the RelWithDebInfo flags (-O2 -g -DNDEBUG, set in src/CMakeLists.txt)
# plus hidden visibility, section GC and ThinLTO for the engine's own targets
# (declared after this point).
# Hidden visibility drops ~11.5k C++ symbols from .dynsym, which shortens
# load-time symbol resolution and lets the linker discard and inline across
# modules. What must stay exported is marked in the sources: SDL_main
# (SDLMAIN_DECLSPEC) and the JNI entry points (JNIEXPORT). Debug info is kept
# so the application's release packaging can split symbols from the stripped
# library. -DSTORM_ANDROID_RELEASE_LTO=OFF keeps everything but LTO.
option(STORM_ANDROID_RELEASE_LTO "ThinLTO for Android Release builds" ON)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
    if(STORM_ANDROID_RELEASE_LTO)
        add_compile_options(-flto=thin)
        add_link_options(-flto=thin)
        # lld's ThinLTO cache: a relink code-generates only the modules that changed (a one-file edit relinked all
        # of libengine.so's ~1000 modules, 35-150 s). Keyed by module content and options, shared by every worktree
        # like ccache (objects are path-independent there), pruned to 8 GB / 7 days by lld itself.
        add_link_options("-Wl,--thinlto-cache-dir=${STORM_DEPS_CACHE}/thinlto-cache"
                         "-Wl,--thinlto-cache-policy=prune_after=168h:cache_size_bytes=8g")
    endif()
endif()
# fmt is linked statically into the engine (built here with the flags above, ThinLTO included).
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${STORM_ANDROID_SOURCES}/fmt" "${CMAKE_BINARY_DIR}/fmt" EXCLUDE_FROM_ALL)
# NDK exposes the Android platform zlib ABI. Do not package a competing libz.so.
add_library(zlib INTERFACE)
target_link_libraries(zlib INTERFACE z)
add_library(storm_open_audio STATIC "${CMAKE_SOURCE_DIR}/android/audio/implementation.cpp" "${CMAKE_SOURCE_DIR}/android/audio/service_backend.cpp")
target_include_directories(storm_open_audio PUBLIC "${CMAKE_SOURCE_DIR}/android/audio" "${STORM_AUDIO_SOURCES}")
target_link_libraries(storm_open_audio PUBLIC dl m)
# Short ABI name of the toolchain file (cmake/toolchains/android-<arch>.cmake): arm64 for every
# release, x86_64 for emulator debug builds.
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(STORM_ANDROID_ARCH arm64)
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(STORM_ANDROID_ARCH x86_64)
else()
    message(FATAL_ERROR "Unsupported ANDROID_ABI ${ANDROID_ABI} (arm64-v8a or x86_64)")
endif()
include(ffmpeg)
storm_ffmpeg("${STORM_ANDROID_SOURCES}" android-${STORM_ANDROID_ARCH})
execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE STORM_ANDROID_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE)
string(TOUPPER "${STORM_ANDROID_ARCH}" storm_watermark_arch)
file(WRITE "${STORM_WATERMARK_FILE}" "#pragma once\n#define STORM_BUILD_WATERMARK Android_${storm_watermark_arch}_${STORM_ANDROID_REVISION}_open_audio\n")
