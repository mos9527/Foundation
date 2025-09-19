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
add_library(Foundation_Atomic ${SOURCES} ${INCLUDES})
target_link_libraries(Foundation_Atomic PUBLIC Foundation_Core)
set_target_properties(Foundation_Atomic PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(
    Foundation_Atomic
    PUBLIC
    ..
)