# CMake module for unit test

# Create symbolic links in the binary dir to input SQL files.
function(_Link_Test_Files src_DIR dest_DIR suffix)
    get_filename_component(src_DIR ${src_DIR} ABSOLUTE)
    file(MAKE_DIRECTORY ${dest_DIR})
    file(GLOB files "${src_DIR}/*.${suffix}")
    foreach(f ${files})
        get_filename_component(file_name ${f} NAME)
        file(CREATE_LINK ${f} ${dest_DIR}/${file_name} SYMBOLIC)
    endforeach()
endfunction()

function(copy_unit_test_input_dir src_dir)
    set(working_DIR "${CMAKE_CURRENT_BINARY_DIR}/unit_test")
    file(MAKE_DIRECTORY ${working_DIR})

    # Link input files to the build dir
    _Link_Test_Files(${src_dir}/input ${working_DIR}/input json)
endfunction()

