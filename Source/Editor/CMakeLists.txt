cmake_minimum_required(VERSION 3.26)


file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${INCLUDES})

add_compile_definitions(SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Shaders")

add_executable(Editor "Main.cpp" ${SOURCES} ${INCLUDES})
target_include_directories(Editor PRIVATE Runtime)
target_precompile_headers(Editor REUSE_FROM PCH)
target_link_libraries(Editor PUBLIC PCH)
target_link_libraries(Editor PUBLIC Runtime)

set_property(TARGET Editor PROPERTY CXX_STANDARD 20)
