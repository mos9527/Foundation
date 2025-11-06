cmake_minimum_required(VERSION 3.26)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
add_library(Foundation_RHICore INTERFACE ${INCLUDES})

set_target_properties(Foundation_RHICore PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(Foundation_RHICore INTERFACE Foundation_Core Foundation_Atomics Foundation_Bits Foundation_Math)

target_include_directories(
    Foundation_RHICore
    INTERFACE
    ..
)

target_precompile_headers(
    Foundation_RHICore
    INTERFACE
    Precompiled.inc
)