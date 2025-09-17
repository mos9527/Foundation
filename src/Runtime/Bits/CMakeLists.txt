cmake_minimum_required(VERSION 3.26)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})
add_library(Foundation_Bits INTERFACE ${SOURCES} ${INCLUDES})

target_link_libraries(Foundation_Bits INTERFACE Foundation_Core)
set_target_properties(Foundation_Bits PROPERTIES LINKER_LANGUAGE CXX)
