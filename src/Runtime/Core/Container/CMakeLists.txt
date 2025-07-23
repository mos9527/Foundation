cmake_minimum_required(VERSION 3.26)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})
add_library(Foundation_Container INTERFACE ${SOURCES} ${INCLUDES})

set_target_properties(Foundation_Container PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(Foundation_Container INTERFACE Foundation_Allocator)
target_include_directories(Foundation_Container INTERFACE ..)

