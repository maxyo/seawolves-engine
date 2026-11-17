if(ANDROID)
set(SYSTEM_DEPS log android)
else()
set(SYSTEM_DEPS "ffi")
endif()

STORM_SETUP(
    TARGET_NAME engine
    TYPE executable
    DEPENDENCIES

    # internal
    core
    diagnostics
    util
    window

    # external
    $<$<NOT:$<BOOL:${ANDROID}>>:mimalloc>
    ${SDL2_LIBRARIES}
    zlib

    # system
    ${SYSTEM_DEPS}
)

if(ANDROID)
    set_target_properties(engine PROPERTIES VERSION "" SOVERSION "" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/game")
    include(build_stamp)
    storm_build_stamp(engine)
    # The APK takes the relief's ncnn graphs (assets) from the same <build>/game directory: building the engine puts
    # them there (src/libs/relief/README.md); ncnn itself is linked into libengine.so.
    add_dependencies(engine storm_relief_model_files)
endif()

