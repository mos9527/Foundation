cmake_minimum_required(VERSION 3.26)
project(Foundation)
set(CMAKE_CXX_STANDARD 20)

add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

set(DERIVED_DATA_DIR "${CMAKE_SOURCE_DIR}/.derived")

add_subdirectory(src/Runtime)
add_subdirectory(src/Renderer)
add_subdirectory(src/ModelViewer)

add_subdirectory(tests)
