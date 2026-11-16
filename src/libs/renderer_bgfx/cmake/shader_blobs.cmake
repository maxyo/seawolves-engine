set(STORM_SHADER_BLOBS "${CMAKE_CURRENT_LIST_DIR}/shader_blobs.py" CACHE INTERNAL "")

# storm_shader_blob_library(<target> <namespace> <directory> <names> <blobs>): the static library of the blobs
# (shader_blobs.py writes blobs.cpp and includes.inc only when their content changes). Used by storm_shaders.cmake and
# fx/cmake/storm_fx_shaders.cmake.
function(storm_shader_blob_library target namespace directory names blobs)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(OUTPUT "${directory}/blobs.cpp" "${directory}/includes.inc"
        COMMAND Python3::Interpreter "${STORM_SHADER_BLOBS}" ${namespace} "${directory}" ${names}
        DEPENDS ${blobs} "${STORM_SHADER_BLOBS}" VERBATIM)
    add_library(${target} STATIC "${directory}/blobs.cpp")
    # The data is copied by the assembler; LTO bitcode would only carry it through every link.
    set_source_files_properties("${directory}/blobs.cpp" PROPERTIES COMPILE_OPTIONS "-fno-lto")
    get_filename_component(gen_root "${directory}" DIRECTORY)
    target_include_directories(${target} PUBLIC "${gen_root}")
endfunction()
