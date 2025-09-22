cmake_minimum_required(VERSION 3.26)

file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${INCLUDES})
add_library(Foundation_Atomics INTERFACE ${INCLUDES})
target_link_libraries(Foundation_Atomics INTERFACE Foundation_Core)
set_target_properties(Foundation_Atomics PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(
    Foundation_Atomics
    INTERFACE
    ..
)
