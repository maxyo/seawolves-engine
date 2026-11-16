STORM_SETUP(
    TARGET_NAME core
    TYPE library
    DEPENDENCIES diagnostics math shared_headers fast_float ${SDL2_LIBRARIES} window
)

# The VM's checks live in this library only: the other libraries build the same with and without them.
if(STORM_VM_VERIFY)
    target_compile_definitions(core PRIVATE STORM_VM_VERIFY=1)
endif()
