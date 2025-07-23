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
add_library(Foundation_Blobs ${SOURCES} ${INCLUDES})

target_link_libraries(Foundation_Blobs PUBLIC Foundation_RHICore)
set_target_properties(Foundation_Blobs PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(Foundation_Blobs PRIVATE ..)
