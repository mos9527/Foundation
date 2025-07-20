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
add_library(Foundation_RHICore ${SOURCES} ${INCLUDES})

set_target_properties(Foundation_RHICore PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(Foundation_RHICore PUBLIC Foundation_Core Foundation_Math)
target_include_directories(Foundation_RHICore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
