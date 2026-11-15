STORM_SETUP(
    TARGET_NAME xinterface
    TYPE storm_module
    DEPENDENCIES animation core geometry model renderer sound_service util video
    TEST_DEPENDENCIES catch2
)# The UI's interface model struct (header only): the Android collector fills it and hands it to
# the UI host in the same process (mobile_interface_model.hpp).
target_include_directories(xinterface PRIVATE "${CMAKE_SOURCE_DIR}/src/libs/ui/include")
