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
add_library(Foundation_Async ${SOURCES} ${INCLUDES})
target_link_libraries(Foundation_Async PUBLIC Foundation_Core)
set_target_properties(Foundation_Async PROPERTIES LINKER_LANGUAGE CXX)

