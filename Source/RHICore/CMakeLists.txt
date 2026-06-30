cmake_minimum_required(VERSION 3.26)
file(
    GLOB_RECURSE INCLUDES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
add_library(Foundation_RHICore INTERFACE ${INCLUDES})

set_target_properties(Foundation_RHICore PROPERTIES LINKER_LANGUAGE CXX)

FetchContent_MakeAvailable(SDL3)
if (ANDROID)
    target_link_libraries(Foundation_RHICore INTERFACE Foundation_Core Foundation_Math SDL3::SDL3-shared)
else()
    target_link_libraries(Foundation_RHICore INTERFACE Foundation_Core Foundation_Math SDL3::SDL3-static)
endif()

target_include_directories(
    Foundation_RHICore
    INTERFACE
    ..
)
