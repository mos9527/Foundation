
find_program(SLANGC_EXECUTABLE
    NAMES slangc
    DOC "Slang compiler executable"
)

if(NOT SLANGC_EXECUTABLE)
    message(FATAL_ERROR "slangc not found!")
endif()

function(slangc_compile TARGET)
    set(options)
    set(oneValueArgs SOURCE OUTPUT_DIR OUTPUT_NAME)
    set(multiValueArgs DEFINES INCLUDE_DIRS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    get_filename_component(SOURCE_FILENAME ${ARG_SOURCE} NAME_WE)
    if(NOT ARG_OUTPUT_NAME)
        set(ARG_OUTPUT_NAME ${SOURCE_FILENAME})
    endif()

    set(GENERATED_FILES "")
    math(EXPR LAST_INDEX "${NUM_TARGETS} - 1")
    # Loop through each requested shader target and create a build command for it.
    foreach(I RANGE ${LAST_INDEX})
        set(OUTPUT_FILENAME "${ARG_OUTPUT_DIR}/${ARG_OUTPUT_NAME}.spv")
        # https://www.khronos.org/assets/uploads/developers/presentations/Vulkan_BOF_Using_Slang_with_Vulkan_SIGG24.pdf
        # https://shader-slang.org/slang/user-guide/spirv-target-specific.html
        set(SLANGC_ARGS
            "${ARG_SOURCE}"
            -o "${OUTPUT_FILENAME}"
            -target spirv
            -profile spirv_1_4
            -emit-spirv-directly
            -matrix-layout-column-major
            -fvk-use-entrypoint-name
        )
        foreach(DEFINE ${ARG_DEFINES})
            list(APPEND SLANGC_ARGS -D${DEFINE})
        endforeach()
        foreach(INCLUDE_DIR ${ARG_INCLUDE_DIRS})
            list(APPEND SLANGC_ARGS -I"${INCLUDE_DIR}")
        endforeach()

        add_custom_command(
            OUTPUT  "${OUTPUT_FILENAME}"
            COMMAND ${SLANGC_EXECUTABLE} ${SLANGC_ARGS}
            DEPENDS "${ARG_SOURCE}"
            COMMENT "${SOURCE_FILENAME}.slang -> ${OUTPUT_FILENAME}"
            VERBATIM
        )
        message("[slangc] ${SOURCE_FILENAME}.slang -> ${OUTPUT_FILENAME}")
        list(APPEND GENERATED_FILES "${OUTPUT_FILENAME}")
    endforeach()    
    add_custom_target(${TARGET} ALL DEPENDS ${GENERATED_FILES})
endfunction()
