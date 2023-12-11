cmake_minimum_required(VERSION 3.26)

file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${INCLUDES})

add_library(Runtime STATIC ${SOURCES} ${INCLUDES})
target_precompile_headers(Runtime REUSE_FROM PCH)
target_link_libraries(Runtime PUBLIC PCH)

set_property(TARGET Runtime PROPERTY CXX_STANDARD 20)
