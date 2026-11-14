STORM_SETUP(
    TARGET_NAME ship
    TYPE storm_module
    DEPENDENCIES collide core geometry island location model sea sea_ai particles renderer
)

if(BUILD_TESTING)
    add_executable(storm_ship_track_sampling_check tests/track_sampling_check.cpp)
    target_compile_features(storm_ship_track_sampling_check PRIVATE cxx_std_17)
    target_include_directories(storm_ship_track_sampling_check PRIVATE src)
    add_test(NAME ship_track_sampling COMMAND storm_ship_track_sampling_check)
endif()
