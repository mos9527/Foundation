cmake_minimum_required(VERSION 3.26)


file(
    GLOB_RECURSE SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})

add_library(
    Foundation_Renderer ${SOURCES} ${INCLUDES}
)
target_link_libraries(Foundation_Renderer PUBLIC Foundation_Core)
target_link_libraries(Foundation_Renderer PUBLIC Foundation_Platform)
target_include_directories(
	Foundation_Renderer 
	PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/.."
)

find_program(SLANGC_EXECUTABLE
    NAMES slangc
    DOC "Slang compiler executable"
)

if(NOT SLANGC_EXECUTABLE)
    message(FATAL_ERROR "slangc not found!")
endif()


function(add_slang_shader TARGET)
    set(options)
    set(oneValueArgs SOURCE OUTPUT_DIR OUTPUT_NAME)
    set(multiValueArgs TARGETS PROFILES ENTRY_POINTS DEFINES INCLUDE_DIRS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    get_filename_component(SOURCE_FILENAME ${ARG_SOURCE} NAME_WE)
    if(NOT ARG_OUTPUT_NAME)
        set(ARG_OUTPUT_NAME ${SOURCE_FILENAME})
    endif()

    list(LENGTH ARG_TARGETS NUM_TARGETS)
    list(LENGTH ARG_PROFILES NUM_PROFILES)
    list(LENGTH ARG_ENTRY_POINTS NUM_ENTRY_POINTS)

    if(NOT ${NUM_TARGETS} EQUAL ${NUM_PROFILES} OR NOT ${NUM_TARGETS} EQUAL ${NUM_ENTRY_POINTS})
        message(FATAL_ERROR "add_slang_shader: The number of TARGETS, PROFILES, and ENTRY_POINTS must be equal.")
    endif()

    set(GENERATED_FILES "")
    math(EXPR LAST_INDEX "${NUM_TARGETS} - 1")

    # Loop through each requested shader target and create a build command for it.
    foreach(I RANGE ${LAST_INDEX})
        list(GET ARG_TARGETS ${I} SHADER_TARGET)
        list(GET ARG_PROFILES ${I} SHADER_PROFILE)
        list(GET ARG_ENTRY_POINTS ${I} SHADER_ENTRY)

        set(OUTPUT_FILENAME "${ARG_OUTPUT_DIR}/${ARG_OUTPUT_NAME}_${SHADER_ENTRY}.${SHADER_TARGET}")
        # https://www.khronos.org/assets/uploads/developers/presentations/Vulkan_BOF_Using_Slang_with_Vulkan_SIGG24.pdf
        set(SLANGC_ARGS
            "${ARG_SOURCE}"
            -o "${OUTPUT_FILENAME}"
            -target ${SHADER_TARGET}
            -profile ${SHADER_PROFILE}
            -entry ${SHADER_ENTRY}
            -emit-spirv-directly
            -matrix-layout-column-major
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
            COMMENT "${SOURCE_FILENAME}.slang -> ${SHADER_ENTRY} (${SHADER_TARGET}) @ ${OUTPUT_FILENAME}"
            VERBATIM
        )
        message("[slangc] ${SOURCE_FILENAME}.slang -> ${SHADER_ENTRY} (${SHADER_TARGET}) @ ${OUTPUT_FILENAME}")
        list(APPEND GENERATED_FILES "${OUTPUT_FILENAME}")
    endforeach()    
    add_custom_target(${TARGET} ALL DEPENDS ${GENERATED_FILES})
endfunction()

set(SHADER_BUILD_DIR "${CMAKE_SOURCE_DIR}/.derived/shaders")
file(MAKE_DIRECTORY ${SHADER_BUILD_DIR})
add_slang_shader(
    Shader_Triangle
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/Triangle.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
    TARGETS         spirv               spirv
    PROFILES        spirv_1_4           spirv_1_4 
    ENTRY_POINTS    vertMain            fragMain
)
add_dependencies(Foundation_Renderer Shader_Triangle)
